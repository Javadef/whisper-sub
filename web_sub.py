#!/usr/bin/env python3
"""
whisper-sub web: faster-whisper → WebSocket → browser HLS player
No MPV required. Open http://localhost:8080 after launch.
"""

import asyncio
import functools
import json
import os
import re
import subprocess
import sys
import threading
import time
from http.server import HTTPServer, SimpleHTTPRequestHandler
from pathlib import Path

import numpy as np
import websockets
from faster_whisper import WhisperModel

# ── AJA / capture device detection ──────────────────────────────────────────

def detect_aja_ntv2() -> list[dict]:
    """Try AJA NTV2 Python SDK (ajantv2) to enumerate boards."""
    try:
        import ajantv2  # type: ignore
        scanner = ajantv2.CNTV2DeviceScanner()
        boards = []
        for i, info in enumerate(scanner.GetDeviceInfoList()):
            boards.append({"index": i, "name": info.deviceIdentifier,
                           "serial": getattr(info, "deviceSerialNumber", "?")})
        return boards
    except Exception:
        return []


def detect_dshow_devices() -> dict:
    """Enumerate DirectShow audio/video devices via ffmpeg (Windows only)."""
    if os.name != "nt":
        return {"video": [], "audio": []}
    try:
        r = subprocess.run(
            ["ffmpeg", "-f", "dshow", "-list_devices", "true", "-i", "dummy"],
            capture_output=True, text=True, timeout=8,
        )
        output = r.stderr
    except Exception:
        return {"video": [], "audio": []}

    video: list[str] = []
    audio: list[str] = []
    current: list[str] | None = None
    for line in output.splitlines():
        if "DirectShow video devices" in line:
            current = video
        elif "DirectShow audio devices" in line:
            current = audio
        elif current is not None and '"' in line:
            # Skip "@device" alternative-name lines
            if "@device" not in line:
                m = re.search(r'"([^"]+)"', line)
                if m:
                    current.append(m.group(1))
    return {"video": video, "audio": audio}


def scan_all_devices() -> dict:
    """Return combined device info: AJA NTV2 boards + DirectShow devices."""
    aja   = detect_aja_ntv2()
    dshow = detect_dshow_devices()
    # Flag which dshow devices look like AJA
    aja_names = {b["name"].lower() for b in aja}
    for lst in (dshow["video"], dshow["audio"]):
        pass  # already plain strings
    return {
        "aja_boards": aja,
        "dshow": dshow,
        "aja_dshow_audio": [d for d in dshow["audio"] if "aja" in d.lower()],
        "aja_dshow_video": [d for d in dshow["video"] if "aja" in d.lower()],
    }


# ── Config ────────────────────────────────────────────────────────────────────

STREAM_URL  = "http://hls.mirtv.cdnvideo.ru/mirtv-parampublish/mir24_2500/playlist.m3u8"

SAMPLE_RATE = 16000
CHUNK_SEC   = 5
OVERLAP_SEC = 0.5

MODEL_SIZE  = "large-v3"
LANGUAGE    = "ru"
DEVICE      = "cuda"
COMPUTE     = "float16"

HTTP_PORT   = 8080
WS_PORT     = 8765

# ─────────────────────────────────────────────────────────────────────────────

_model: WhisperModel | None = None
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
    global _model
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

def _build_ffmpeg_args(url: str) -> list[str]:
    """Build ffmpeg argv for either an HLS/HTTP URL or a dshow:// device."""
    if url.startswith("dshow://"):
        device_name = url[len("dshow://"):]
        return [
            "ffmpeg",
            "-f", "dshow",
            "-i", f"audio={device_name}",
            "-f", "s16le",
            "-ar", str(SAMPLE_RATE),
            "-ac", "1",
            "-loglevel", "quiet",
            "pipe:1",
        ]
    else:
        return [
            "ffmpeg", "-re",
            "-i", url,
            "-vn",
            "-f", "s16le",
            "-ar", str(SAMPLE_RATE),
            "-ac", "1",
            "-loglevel", "quiet",
            "pipe:1",
        ]


def audio_stream(stop: threading.Event):
    with _stream_lock:
        url = _current_stream_url
    proc = subprocess.Popen(
        _build_ffmpeg_args(url),
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    bytes_chunk   = SAMPLE_RATE * 2 * CHUNK_SEC
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
                # capture_t = wallclock time at the END of the audio chunk
                yield audio, time.time()
    finally:
        proc.terminate()


def caption_loop() -> None:
    global _active_language
    lag_history: list[float] = []
    while True:
        _restart_flag.clear()
        stop = threading.Event()
        for audio, capture_t in audio_stream(stop):
            if _restart_flag.is_set():
                stop.set()
                break
            t0 = time.perf_counter()
            try:
                segments, info = _model.transcribe(
                    audio,
                    language=_active_language or None,
                    vad_filter=True,
                    vad_parameters={"min_silence_duration_ms": 500},
                )
                text = " ".join(s.text.strip() for s in segments).strip()
                lag  = time.perf_counter() - t0

                if not _active_language:
                    _active_language = info.language  # lock to detected lang

                _stats["chunks"] += 1
                _stats["lang"]    = info.language
                _stats["lag"]     = round(lag, 2)
                lag_history.append(lag)
                if len(lag_history) > 10:
                    lag_history.pop(0)
                _stats["lag_avg"] = round(sum(lag_history) / len(lag_history), 2)

                if text:
                    print(f"[{info.language}|{lag:.1f}s] {text}", flush=True)
                    broadcast({
                        "type":      "caption",
                        "text":      text,
                        "lang":      info.language,
                        "lag":       _stats["lag"],
                        "lag_avg":   _stats["lag_avg"],
                        "chunks":    _stats["chunks"],
                        "device":    _stats["device"],
                        "model":     _stats["model"],
                        "compute":   _stats["compute"],
                        "audio_t":   capture_t,      # server wallclock at audio END
                        "server_now": time.time(),   # for clock-skew refresh
                    })
                else:
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
        await websocket.send(json.dumps({
            "type": "init",
            "server_now": time.time(),
            "chunk_sec": CHUNK_SEC,
            **_stats,
        }))
        async for raw in websocket:
            try:
                msg = json.loads(raw)
                if msg.get("type") == "set_stream":
                    url = str(msg.get("url", "")).strip()
                    if url:
                        set_stream_url(url)
                        broadcast({"type": "stream_changed", "url": url})
                elif msg.get("type") == "scan_devices":
                    devices = scan_all_devices()
                    print(f"[aja] scan: {devices['aja_boards']}", flush=True)
                    await websocket.send(json.dumps({"type": "devices", **devices}))
            except Exception:
                pass
    finally:
        with _clients_lock:
            _clients.discard(websocket)


async def ws_server_main() -> None:
    global _loop
    _loop = asyncio.get_running_loop()
    async with websockets.serve(ws_handler, "localhost", WS_PORT):
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

    threading.Thread(target=start_http_server, daemon=True).start()
    threading.Thread(target=caption_loop,       daemon=True).start()

    print(f"\n>>> http://localhost:{HTTP_PORT} <<<\n", flush=True)
    try:
        asyncio.run(ws_server_main())
    except KeyboardInterrupt:
        print("\n[exit]")


if __name__ == "__main__":
    main()
