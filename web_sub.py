#!/usr/bin/env python3
"""
whisper-sub web: faster-whisper → WebSocket → browser HLS player
No MPV required. Open http://localhost:8080 after launch.
"""

import asyncio
import functools
import json
import os
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from http.server import HTTPServer, SimpleHTTPRequestHandler
from pathlib import Path

import numpy as np
import torch
import websockets
from faster_whisper import WhisperModel
from transformers import WhisperProcessor, WhisperForConditionalGeneration

# ── Config ────────────────────────────────────────────────────────────────────

STREAM_URL  = "http://213.230.64.74:1510/mix/index.m3u8"

SAMPLE_RATE = 16000
CHUNK_SEC   = 5
OVERLAP_SEC = 0.5

MODEL_SIZE  = "medium"
LANGUAGE    = "uz"
DEVICE      = "cuda"
COMPUTE     = "float16"

USE_KOTIB  = True   # Use Kotib/uzbek_stt_v1 (Uzbek fine-tune). False = faster-whisper generic
KOTIB_MODEL = "Kotib/uzbek_stt_v1"

HTTP_PORT   = 8080
WS_PORT     = 8766

# hls_to_sdi.exe control endpoint (burns captions into SDI via ffmpeg drawtext reload)
SDI_CTRL_URL    = "http://localhost:8765"
SDI_AUTOSTART   = True   # POST /start on launch + on stream switch
SDI_EXE_PATH    = Path(__file__).parent / "libajantv2" / "build" / "demos" / "hls_to_sdi" / "Release" / "hls_to_sdi.exe"
SDI_AUTOLAUNCH  = True   # spawn hls_to_sdi.exe if not already running
SDI_BROWSER_OFFSET = 16  # seconds: delay SDI behind live edge to match browser HLS buffer

CAPTION_BLOCKLIST = [
    "Редактор субтитров",
    "Корректор",
    "Фондю любит тебя",
]

# ─────────────────────────────────────────────────────────────────────────────

_model: WhisperModel | WhisperForConditionalGeneration | None = None
_processor: WhisperProcessor | None = None
_device: str = "cpu"
_clients: set = set()
_clients_lock = threading.Lock()
_loop: asyncio.AbstractEventLoop | None = None

# ── Stream hot-reload ─────────────────────────────────────────────────────────
if sys.version_info >= (3, 9):
    _current_stream_url: str = STREAM_URL
else:
    _current_stream_url = STREAM_URL
_stream_lock        = threading.Lock()
_restart_flag       = threading.Event()
_active_language    = LANGUAGE   # reset to "" on stream switch → forces re-detect


def set_stream_url(url: str) -> None:
    global _current_stream_url, _active_language
    with _stream_lock:
        _current_stream_url = url
    _active_language = ""   # force re-detect on new stream
    _stats["stream"] = url
    _restart_flag.set()
    print(f"[stream] switching to {url}", flush=True)
    if SDI_AUTOSTART:
        sdi_start_stream(url)


# ── SDI bridge (hls_to_sdi.exe) ──────────────────────────────────────────────

def _sdi_post(path: str, data: dict | None = None) -> None:
    """Fire-and-forget POST to hls_to_sdi control server.
    Uses a raw socket so headers+body go in a single sendall(), avoiding the
    TCP split that causes the C++ server's single recv() to miss the body.
    """
    body_bytes = urllib.parse.urlencode(data).encode("utf-8") if data else b""
    request = (
        f"POST {path} HTTP/1.0\r\n"
        f"Host: 127.0.0.1:8765\r\n"
        f"Content-Type: application/x-www-form-urlencoded\r\n"
        f"Content-Length: {len(body_bytes)}\r\n"
        f"Connection: close\r\n"
        f"\r\n"
    ).encode("utf-8") + body_bytes

    silent = (path == "/caption")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect(("127.0.0.1", 8765))
        s.sendall(request)
        resp = b""
        try:
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                resp += chunk
        except OSError:
            pass  # WinError 10054 — server closed after writing response
        finally:
            s.close()
        # Check for HTTP error status
        status_line = resp.split(b"\r\n", 1)[0].decode(errors="replace")
        parts = status_line.split(" ", 2)
        if len(parts) >= 2 and not parts[1].startswith("2"):
            body_resp = resp.split(b"\r\n\r\n", 1)[-1].decode(errors="replace") if b"\r\n\r\n" in resp else ""
            if not silent:
                print(f"[sdi] POST {path} HTTP {parts[1]}: {body_resp}", flush=True)
    except Exception as err:
        if not silent:
            print(f"[sdi] POST {path} failed: {err}", flush=True)


def _wrap_caption(text: str, max_chars: int = 48) -> str:
    """Wrap caption to <=2 lines, ~max_chars per line. ffmpeg drawtext has no auto-wrap."""
    # Strip control chars (tab/VT/CR/etc) that render as boxes/glyphs in drawtext
    text = "".join(c for c in text if c.isprintable() or c == " ")
    text = text.strip()
    if len(text) <= max_chars:
        return text
    words = text.split()
    lines, cur = [], ""
    for w in words:
        if cur and len(cur) + 1 + len(w) > max_chars:
            lines.append(cur)
            cur = w
            if len(lines) >= 2:  # only 2 lines max
                break
        else:
            cur = (cur + " " + w) if cur else w
    if cur and len(lines) < 2:
        lines.append(cur)
    return "\n".join(lines)


SDI2SDI_CAP_FILE = Path(__file__).parent / "sdi2sdi_cap1.txt"

def sdi_send_caption(text: str) -> None:
    wrapped = _wrap_caption(text)
    threading.Thread(target=_sdi_post, args=("/caption", {"text": wrapped}), daemon=True).start()
    try:
        SDI2SDI_CAP_FILE.write_text(wrapped, encoding="utf-8")
    except OSError:
        pass


def sdi_start_stream(url: str) -> None:
    threading.Thread(target=_sdi_post, args=("/start", {"url": url}), daemon=True).start()


def sdi_launch_exe() -> None:
    if not SDI_AUTOLAUNCH:
        return
    if not SDI_EXE_PATH.exists():
        print(f"[sdi] exe not found: {SDI_EXE_PATH}", flush=True)
        return
    # quick probe — already running?
    already = False
    try:
        urllib.request.urlopen(SDI_CTRL_URL + "/status", timeout=0.5).read()
        already = True
        print("[sdi] already running", flush=True)
    except Exception:
        pass
    if not already:
        print(f"[sdi] launching {SDI_EXE_PATH.name}...", flush=True)
        try:
            subprocess.Popen(
                [str(SDI_EXE_PATH)],
                cwd=str(SDI_EXE_PATH.parent),
                creationflags=subprocess.CREATE_NEW_CONSOLE if os.name == "nt" else 0,
            )
        except Exception as err:
            print(f"[sdi] launch failed: {err}", flush=True)
            return
    if SDI_AUTOSTART:
        threading.Thread(target=_sdi_wait_and_start, args=(STREAM_URL,), daemon=True).start()


def _sdi_wait_and_start(url: str, timeout: float = 30.0) -> None:
    """Poll /status until hls_to_sdi responds, then POST /start."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            urllib.request.urlopen(SDI_CTRL_URL + "/status", timeout=1.0).read()
            break
        except Exception:
            time.sleep(0.5)
    else:
        print("[sdi] timed out waiting for exe to start", flush=True)
        return
    print(f"[sdi] auto-starting stream: {url}", flush=True)
    _sdi_post("/start", {"url": url})
    # verify after 5s — enough time for ffmpeg to open the stream
    time.sleep(5.0)
    try:
        raw  = urllib.request.urlopen(SDI_CTRL_URL + "/status", timeout=2.0).read()
        data = json.loads(raw)
        if data.get("running"):
            print("[sdi] SDI stream running", flush=True)
            if SDI_BROWSER_OFFSET > 0:
                # delay SDI behind live edge to match browser HLS buffer
                # /seek delta=-N sets gSeekOffset=N (N seconds behind live)
                _sdi_post(f"/seek?delta=-{SDI_BROWSER_OFFSET}")
                print(f"[sdi] offset -{SDI_BROWSER_OFFSET}s to match browser", flush=True)
        else:
            print("[sdi] WARNING: SDI stream not running — check http://localhost:8765 for details", flush=True)
    except Exception as err:
        print(f"[sdi] status check failed: {err}", flush=True)


SDI_LIVE_SNAP_INTERVAL = 5 * 60  # snap to live edge every 5 minutes

def sdi_watchdog() -> None:
    """Check SDI stream every 5s; restart if stopped. Snap to live edge periodically."""
    time.sleep(15)  # initial grace period
    last_snap = time.monotonic()
    while True:
        time.sleep(5)
        try:
            raw  = urllib.request.urlopen(SDI_CTRL_URL + "/status", timeout=2.0).read()
            data = json.loads(raw)
            if not data.get("running"):
                with _stream_lock:
                    url = _current_stream_url
                print("[sdi] watchdog: stream stopped, restarting...", flush=True)
                _sdi_post("/start", {"url": url})
                last_snap = time.monotonic()
            elif time.monotonic() - last_snap >= SDI_LIVE_SNAP_INTERVAL:
                # snap back to configured offset from live edge
                seek_path = f"/seek?delta=-{SDI_BROWSER_OFFSET}" if SDI_BROWSER_OFFSET > 0 else "/seek?delta=0&live=1"
                print("[sdi] watchdog: snapping to live edge", flush=True)
                _sdi_post(seek_path)
                last_snap = time.monotonic()
        except Exception:
            pass  # exe not reachable — don't spam


_stats = {
    "chunks":    0,
    "lang":      "?",
    "lag":       0.0,
    "lag_avg":   0.0,
    "errors":    [],
    "device":    DEVICE,
    "model":     MODEL_SIZE,
    "compute":   COMPUTE,
    "stream":    STREAM_URL,
}


# ── CUDA ──────────────────────────────────────────────────────────────────────

def setup_cuda_dll_paths() -> None:
    if os.name != "nt":
        return
    candidates = [
        Path(sys.prefix) / "Lib" / "site-packages" / "nvidia" / "cudnn"       / "bin",
        Path(sys.prefix) / "Lib" / "site-packages" / "nvidia" / "cublas"      / "bin",
        Path(sys.prefix) / "Lib" / "site-packages" / "nvidia" / "cuda_nvrtc"  / "bin",
    ]
    for dll_dir in candidates:
        if dll_dir.exists():
            try:
                os.add_dll_directory(str(dll_dir))
            except (AttributeError, FileNotFoundError):
                pass
            os.environ["PATH"] = str(dll_dir) + os.pathsep + os.environ.get("PATH", "")


def load_model() -> None:
    global _model, _processor, _device
    if USE_KOTIB:
        print(f"[kotib] Loading {KOTIB_MODEL}...", flush=True)
        setup_cuda_dll_paths()
        _device = "cuda" if torch.cuda.is_available() else "cpu"
        try:
            _processor = WhisperProcessor.from_pretrained(KOTIB_MODEL)
            _model = WhisperForConditionalGeneration.from_pretrained(KOTIB_MODEL).to(_device)
            _stats["device"]  = _device
            _stats["model"]   = "Kotib/uzbek_stt_v1"
            _stats["compute"] = "fp32"
        except Exception as err:
            print(f"[kotib] Load failed: {err}", flush=True)
            sys.exit(1)
        print(f"[kotib] Ready on {_device}.", flush=True)
        return

    print(f"[whisper] Loading {MODEL_SIZE}...", flush=True)
    setup_cuda_dll_paths()
    try:
        _model = WhisperModel(MODEL_SIZE, device=DEVICE, compute_type=COMPUTE)
        _stats["device"] = DEVICE
    except Exception as err:
        print(f"[whisper] CUDA failed: {err}\n[whisper] CPU fallback.", flush=True)
        _model = WhisperModel(MODEL_SIZE, device="cpu", compute_type="int8")
        _stats["device"]  = "cpu (fallback)"
        _stats["compute"] = "int8"
    print("[whisper] Model ready.", flush=True)


# ── WebSocket broadcast ───────────────────────────────────────────────────────

def broadcast(msg: dict) -> None:
    if _loop is None:
        return
    payload = json.dumps(msg)
    with _clients_lock:
        clients = frozenset(_clients)
    if clients:
        asyncio.run_coroutine_threadsafe(_broadcast_async(payload, clients), _loop)


async def _broadcast_async(payload: str, clients: frozenset) -> None:
    for ws in clients:
        try:
            await ws.send(payload)
        except Exception:
            pass


# ── Whisper pipeline ──────────────────────────────────────────────────────────

def audio_stream(stop: threading.Event):
    # Read 16kHz mono s16le PCM from TCP server run by sdi_passthrough.exe
    SDI_TCP = "tcp://127.0.0.1:9876"
    while not stop.is_set():
        try:
            proc = subprocess.Popen(
                [
                    "ffmpeg",
                    "-f", "s16le", "-ar", "16000", "-ac", "1",
                    "-i", SDI_TCP,
                    "-f", "s16le", "-ar", str(SAMPLE_RATE), "-ac", "1",
                    "-loglevel", "quiet",
                    "pipe:1",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
        except Exception as e:
            print(f"[audio] ffmpeg launch failed: {e}", flush=True)
            time.sleep(2)
            continue
        bytes_chunk   = int(SAMPLE_RATE * 2 * CHUNK_SEC)
        bytes_overlap = int(SAMPLE_RATE * 2 * OVERLAP_SEC)
        buf = b""
        try:
            while not stop.is_set():
                raw = proc.stdout.read(8192)
                if not raw:
                    break
                buf += raw
                while len(buf) >= bytes_chunk:
                    pcm  = buf[:bytes_chunk]
                    buf  = buf[bytes_chunk - bytes_overlap:]
                    audio = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
                    yield audio
        finally:
            proc.terminate()


def caption_loop() -> None:
    global _active_language
    lag_history: list[float] = []
    while True:
        _restart_flag.clear()
        stop = threading.Event()
        for audio in audio_stream(stop):
            if _restart_flag.is_set():
                stop.set()
                break
            t0 = time.perf_counter()
            try:
                if USE_KOTIB:
                    input_features = _processor(audio, sampling_rate=16000, return_tensors="pt").input_features.to(_device)
                    with torch.no_grad():
                        predicted_ids = _model.generate(
                            input_features,
                            language="uz",
                            task="transcribe",
                            attention_mask=torch.ones(input_features.shape[:2], device=_device),
                        )
                    text = _processor.batch_decode(predicted_ids, skip_special_tokens=True, clean_up_tokenization_spaces=False)[0].strip()
                    lang = "uz"
                    if not _active_language:
                        _active_language = "uz"
                else:
                    segments, info = _model.transcribe(
                        audio,
                        language=_active_language or None,
                        vad_filter=False,
                    )
                    text = " ".join(s.text.strip() for s in segments).strip()
                    lang = info.language
                    if not _active_language:
                        _active_language = info.language

                lag  = time.perf_counter() - t0
                rms  = float(np.sqrt(np.mean(audio ** 2)))

                _stats["chunks"] += 1
                _stats["lang"]    = lang
                _stats["lag"]     = round(lag, 2)
                lag_history.append(lag)
                if len(lag_history) > 10:
                    lag_history.pop(0)
                _stats["lag_avg"] = round(sum(lag_history) / len(lag_history), 2)

                if text:
                    for blocked in CAPTION_BLOCKLIST:
                        if blocked.lower() in text.lower():
                            text = "••••"
                            break
                    print(f"[{lang}|{lag:.1f}s|rms={rms:.4f}] {text}", flush=True)
                    sdi_send_caption(text)
                    broadcast({
                        "type":    "caption",
                        "text":    text,
                        "lang":    lang,
                        "lag":     _stats["lag"],
                        "lag_avg": _stats["lag_avg"],
                        "chunks":  _stats["chunks"],
                        "device":  _stats["device"],
                        "model":   _stats["model"],
                        "compute": _stats["compute"],
                    })
                else:
                    print(f"[{lang}|{lag:.1f}s|rms={rms:.4f}] <silence>", flush=True)
                    broadcast({"type": "stats", **_stats})

            except Exception as err:
                msg = str(err)[:200]
                _stats["errors"].append({"t": time.strftime("%H:%M:%S"), "msg": msg})
                if len(_stats["errors"]) > 20:
                    _stats["errors"].pop(0)
                broadcast({"type": "error", "msg": msg, **_stats})
                print(f"[whisper error] {err}", flush=True)

        if not _restart_flag.is_set():
            print("[ffmpeg] stream ended, reconnecting in 2s...", flush=True)
            broadcast({"type": "reconnecting"})
            time.sleep(2)


# ── WebSocket server ──────────────────────────────────────────────────────────

async def ws_handler(websocket, *args, **kwargs) -> None:
    with _clients_lock:
        _clients.add(websocket)
    try:
        await websocket.send(json.dumps({"type": "init", **_stats}))
        async for raw in websocket:
            try:
                msg = json.loads(raw)
                if msg.get("type") == "set_stream":
                    url = str(msg.get("url", "")).strip()
                    if url:
                        set_stream_url(url)
                        broadcast({"type": "stream_changed", "url": url})
            except Exception:
                pass
    finally:
        with _clients_lock:
            _clients.discard(websocket)


async def ws_server_main() -> None:
    global _loop
    _loop = asyncio.get_running_loop()
    async with websockets.serve(ws_handler, "localhost", WS_PORT,
                                ping_interval=20, ping_timeout=60):
        print(f"[ws]   ws://localhost:{WS_PORT}", flush=True)
        await asyncio.Future()


# ── HTTP server ───────────────────────────────────────────────────────────────

def make_http_handler(directory: str):
    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=directory, **kw)

        def log_message(self, fmt, *args):
            pass  # suppress HTTP noise

    return Handler


def start_http_server() -> None:
    handler = make_http_handler(str(Path(__file__).parent))
    server  = HTTPServer(("localhost", HTTP_PORT), handler)
    print(f"[http] http://localhost:{HTTP_PORT}", flush=True)
    server.serve_forever()


# ── Entry point ───────────────────────────────────────────────────────────────

def check_deps() -> None:
    result = subprocess.run(["where", "ffmpeg"], capture_output=True, text=True)
    if result.returncode != 0:
        print("[error] ffmpeg not in PATH")
        sys.exit(1)


def main() -> None:
    check_deps()
    load_model()

    sdi_launch_exe()  # launches exe + auto-starts stream (no browser click needed)

    threading.Thread(target=start_http_server, daemon=True).start()
    threading.Thread(target=caption_loop,       daemon=True).start()
    threading.Thread(target=sdi_watchdog,        daemon=True).start()

    print(f"\n>>> http://localhost:{HTTP_PORT} <<<\n", flush=True)
    try:
        asyncio.run(ws_server_main())
    except KeyboardInterrupt:
        pass
    finally:
        print("\n[exit] stopping SDI stream...", flush=True)
        _sdi_post("/stop")
        print("[exit] done")


if __name__ == "__main__":
    main()
