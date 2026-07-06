#!/usr/bin/env python3
"""
Whisper Subtitle Overlay — speech-to-text via Kotib/uzbek_stt_v1 or faster-whisper,
broadcasts captions over WebSocket, writes to sdi2sdi_cap1.txt for SDI overlay.
Open http://localhost:8080 after launch.
"""

import asyncio
import json
import os
import subprocess
import sys
import threading
import time
from http.server import HTTPServer, SimpleHTTPRequestHandler
from pathlib import Path

# ── CUDA DLL paths must be set BEFORE importing torch (Windows) ───────────────

def _setup_cuda_dll_paths() -> None:
    if os.name != "nt":
        return
    for p in {sys.prefix, sys.base_prefix}:
        for d in ("cudnn", "cublas", "cuda_nvrtc"):
            dll_dir = Path(p) / "Lib" / "site-packages" / "nvidia" / d / "bin"
            if dll_dir.is_dir():
                try:
                    os.add_dll_directory(str(dll_dir))
                except (AttributeError, FileNotFoundError):
                    pass
                os.environ["PATH"] = str(dll_dir) + os.pathsep + os.environ.get("PATH", "")

_setup_cuda_dll_paths()

import numpy as np
import torch
import websockets
from faster_whisper import WhisperModel
from transformers import WhisperProcessor, WhisperForConditionalGeneration

# ── Configuration ────────────────────────────────────────────────────────────

STREAM_URL  = "https://stream2-sarkor.clicktv.platform24.tv/mbrjwt/b34f22a36e730c70bd80b6ff598ffdf0a5b9df95-7061-0-clk3102913-5-3588636674-1783081179-e128-JzaLH-h2CTqEx3SjoWzD8dkQq1KN-EdQvQLo756Mxjw/index.m3u8?ml=true"
SAMPLE_RATE = 16000
CHUNK_SEC   = 5
OVERLAP_SEC = 0.5

MODEL_SIZE  = "medium"
LANGUAGE    = "uz"
DEVICE      = "cuda"
COMPUTE     = "float16"

USE_KOTIB   = True
KOTIB_MODEL = "Kotib/uzbek_stt_v1"

HTTP_PORT   = 8080
WS_PORT     = 8766

SDI_EXE_PATH   = Path(__file__).parent / "sdi_passthrough" / "build" / "Release" / "sdi_passthrough.exe"
SDI_AUTOLAUNCH = True

USE_HLS_AUDIO = True  # True = pull audio directly from HLS stream (no AJA card needed)

CAPTION_BLOCKLIST = (
    "Редактор субтитров",
    "Корректор",
    "Фондю любит тебя",
)

# ── Global state ─────────────────────────────────────────────────────────────

_model:      WhisperModel | WhisperForConditionalGeneration | None = None
_processor:  WhisperProcessor | None = None
_device:     str = "cpu"
_clients:    set = set()
_clients_lock = threading.Lock()
_loop:       asyncio.AbstractEventLoop | None = None

_current_stream_url: str = STREAM_URL
_stream_lock     = threading.Lock()
_restart_flag    = threading.Event()
_active_language = LANGUAGE

_stats = {
    "chunks":   0, "lang": "?", "lag": 0.0, "lag_avg": 0.0,
    "errors":   [], "device": DEVICE, "model": MODEL_SIZE,
    "compute":  COMPUTE, "stream": STREAM_URL,
}

# ── SDI passthrough ──────────────────────────────────────────────────────────

SDI2SDI_CAP_FILE = Path(__file__).parent / "sdi2sdi_cap1.txt"
LATEST_CAPTION_FILE = Path(__file__).parent / "latest_caption.txt"

def sdi_send_caption(text: str) -> None:
    wrapped = _wrap_caption(text)
    try:
        SDI2SDI_CAP_FILE.write_text(wrapped, encoding="utf-8")
        LATEST_CAPTION_FILE.write_text(wrapped, encoding="utf-8")
    except OSError:
        pass

def sdi_launch_exe() -> None:
    if not SDI_AUTOLAUNCH or not SDI_EXE_PATH.exists():
        return
    try:
        out = subprocess.check_output(["tasklist"], creationflags=subprocess.CREATE_NO_WINDOW)
        if b"sdi_passthrough" in out:
            print("[sdi] already running", flush=True)
            return
    except Exception:
        pass
    print(f"[sdi] launching {SDI_EXE_PATH.name}...", flush=True)
    try:
        subprocess.Popen(
            [str(SDI_EXE_PATH)],
            cwd=str(SDI_EXE_PATH.parent),
            creationflags=subprocess.CREATE_NEW_CONSOLE if os.name == "nt" else 0,
        )
    except Exception as err:
        print(f"[sdi] launch failed: {err}", flush=True)

# ── Model loader ─────────────────────────────────────────────────────────────

def load_model() -> None:
    global _model, _processor, _device
    if USE_KOTIB:
        print(f"[kotib] Loading {KOTIB_MODEL}...", flush=True)
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
    try:
        _model = WhisperModel(MODEL_SIZE, device=DEVICE, compute_type=COMPUTE)
        _stats["device"] = DEVICE
    except Exception as err:
        print(f"[whisper] CUDA failed: {err}\n[whisper] CPU fallback.", flush=True)
        _model = WhisperModel(MODEL_SIZE, device="cpu", compute_type="int8")
        _stats["device"]  = "cpu (fallback)"
        _stats["compute"] = "int8"
    print("[whisper] Model ready.", flush=True)

# ── Caption wrapping ─────────────────────────────────────────────────────────

def _wrap_caption(text: str, max_chars: int = 80) -> str:
    text = "".join(c for c in text if c.isprintable() or c == " ")
    text = text.strip()
    if len(text) > max_chars:
        text = text[:max_chars - 3] + "..."
    return text

# ── WebSocket broadcast ──────────────────────────────────────────────────────

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

# ── Audio capture (via sdi_passthrough.exe TCP or HLS direct) ──────────────

def audio_stream(stop: threading.Event):
    if USE_HLS_AUDIO:
        while not stop.is_set():
            try:
                proc = subprocess.Popen(
                    [
                        "ffmpeg", "-re",
                        "-i", STREAM_URL,
                        "-vn",
                        "-f", "s16le",
                        "-ar", str(SAMPLE_RATE),
                        "-ac", "1",
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
                        pcm = buf[:bytes_chunk]
                        buf = buf[bytes_chunk - bytes_overlap:]
                        audio = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
                        yield audio
            finally:
                proc.terminate()
            print("[audio] stream ended, reconnecting...", flush=True)
            time.sleep(2)
    else:
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
                        pcm = buf[:bytes_chunk]
                        buf = buf[bytes_chunk - bytes_overlap:]
                        audio = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
                        yield audio
            finally:
                proc.terminate()
            print("[audio] SDI stream ended, reconnecting...", flush=True)
            time.sleep(2)

# ── Transcription loop ───────────────────────────────────────────────────────

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

                lag = time.perf_counter() - t0
                rms = float(np.sqrt(np.mean(audio ** 2)))

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
                    broadcast({"type": "caption", "text": text, "lang": lang, "lag": _stats["lag"], "lag_avg": _stats["lag_avg"], "chunks": _stats["chunks"], "device": _stats["device"], "model": _stats["model"], "compute": _stats["compute"]})
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

# ── Stream switching ─────────────────────────────────────────────────────────

def set_stream_url(url: str) -> None:
    global _current_stream_url, _active_language
    with _stream_lock:
        _current_stream_url = url
    _active_language = ""
    _stats["stream"] = url
    _restart_flag.set()
    print(f"[stream] switching to {url}", flush=True)

# ── WebSocket server ─────────────────────────────────────────────────────────

async def ws_handler(websocket, *args, **kwargs) -> None:
    with _clients_lock:
        _clients.add(websocket)
    try:
        await websocket.send(json.dumps({"type": "init", **_stats}))
        async for raw in websocket:
            try:
                msg = json.loads(raw)
                if msg.get("type") == "set_stream" and (url := str(msg.get("url", "")).strip()):
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
    async with websockets.serve(ws_handler, "localhost", WS_PORT, ping_interval=20, ping_timeout=60):
        print(f"[ws]   ws://localhost:{WS_PORT}", flush=True)
        await asyncio.Future()

# ── HTTP server ──────────────────────────────────────────────────────────────

class _QuietHandler(SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(Path(__file__).parent), **kw)
    def log_message(self, fmt, *args):
        pass

def start_http_server() -> None:
    server = HTTPServer(("localhost", HTTP_PORT), _QuietHandler)
    print(f"[http] http://localhost:{HTTP_PORT}", flush=True)
    server.serve_forever()

# ── Entry point ──────────────────────────────────────────────────────────────

def main() -> None:
    if subprocess.run(["where", "ffmpeg"], capture_output=True).returncode != 0:
        print("[error] ffmpeg not in PATH")
        sys.exit(1)

    load_model()
    sdi_launch_exe()

    threading.Thread(target=start_http_server, daemon=True).start()
    threading.Thread(target=caption_loop, daemon=True).start()

    print(f"\n>>> http://localhost:{HTTP_PORT} <<<\n", flush=True)
    try:
        asyncio.run(ws_server_main())
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()
