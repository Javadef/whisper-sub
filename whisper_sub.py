#!/usr/bin/env python3
"""
whisper-sub: Live captions for HLS streams via faster-whisper + MPV

Lip sync method:
  MPV pauses VIDEO_DELAY seconds at startup while ffmpeg/Whisper pipeline
  begins processing live audio. By the time MPV unpauses, Whisper is already
  ~VIDEO_DELAY seconds into the stream — captions arrive in sync.

  Tune VIDEO_DELAY to match your hardware. Run once, observe console lag
  stamps like "[ru|6.2s lag]", set VIDEO_DELAY = that number.

Dependencies:  pip install faster-whisper numpy
External:      ffmpeg and mpv must be in PATH
"""

import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path
from uuid import uuid4

import numpy as np
import win32file
import pywintypes
from faster_whisper import WhisperModel

# ─── Config ──────────────────────────────────────────────────────────────────

STREAM_URL  = "http://hls.mirtv.cdnvideo.ru/mirtv-parampublish/mir24_2500/playlist.m3u8"

SAMPLE_RATE = 16000
CHUNK_SEC   = 5      # audio window (seconds). larger = more accurate, more lag
OVERLAP_SEC = 0.5    # chunk overlap — preserves words at boundaries

VIDEO_DELAY = 8.0    # seconds to pause MPV at start; tune to match Whisper lag

MODEL_SIZE  = "medium"   # tiny | base | small | medium | large-v3
LANGUAGE    = None        # None = auto-detect; or "ru", "en", etc.
DEVICE      = "cuda"      # "cuda" or "cpu"
COMPUTE     = "float16"   # "float16" for cuda, "int8" for cpu

MPV_IPC_BASE = "mpvsocket"
CAPTION_MS  = 6000   # OSD display duration per caption (ms)

# ─────────────────────────────────────────────────────────────────────────────

_model: WhisperModel | None = None
_ipc_lock = threading.Lock()
_mpv_proc: subprocess.Popen | None = None
_mpv_ipc_path = ""


def setup_cuda_dll_paths() -> None:
    """Expose DLL directories from NVIDIA pip wheels to Windows loader."""
    if os.name != "nt":
        return

    candidates = [
        Path(sys.prefix) / "Lib" / "site-packages" / "nvidia" / "cudnn" / "bin",
        Path(sys.prefix) / "Lib" / "site-packages" / "nvidia" / "cublas" / "bin",
        Path(sys.prefix) / "Lib" / "site-packages" / "nvidia" / "cuda_nvrtc" / "bin",
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
    except Exception as err:
        print(f"[whisper] CUDA init failed: {err}", flush=True)
        print("[whisper] Falling back to CPU int8.", flush=True)
        _model = WhisperModel(MODEL_SIZE, device="cpu", compute_type="int8")
    print("[whisper] Model ready.", flush=True)


def mpv_cmd(command: list, retries: int = 4, retry_delay: float = 0.12) -> bool:
    """Send JSON IPC command to MPV via Windows named pipe; retries while pipe initializes."""
    payload = (json.dumps({"command": command}) + "\n").encode("utf-8")
    with _ipc_lock:
        last_err = None
        for _ in range(retries):
            handle = None
            try:
                handle = win32file.CreateFile(
                    _mpv_ipc_path,
                    win32file.GENERIC_WRITE,
                    0,
                    None,
                    win32file.OPEN_EXISTING,
                    0,
                    None,
                )
                win32file.WriteFile(handle, payload)
                return True
            except pywintypes.error as err:
                last_err = err
                time.sleep(retry_delay)
            finally:
                if handle is not None:
                    win32file.CloseHandle(handle)
        if last_err is not None:
            mpv_alive = _mpv_proc is not None and _mpv_proc.poll() is None
            print(f"[mpv-ipc] command failed: {last_err} (mpv_alive={mpv_alive})", flush=True)
        return False


def audio_stream():
    """Yield float32 numpy arrays of audio chunks from the HLS stream."""
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

    bytes_chunk   = SAMPLE_RATE * 2 * CHUNK_SEC
    bytes_overlap = int(SAMPLE_RATE * 2 * OVERLAP_SEC)
    buf = b""

    try:
        while True:
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
    for audio in audio_stream():
        t0 = time.perf_counter()

        segments, info = _model.transcribe(
            audio,
            language=LANGUAGE,
            vad_filter=True,
            vad_parameters={"min_silence_duration_ms": 500},
        )

        text = " ".join(s.text.strip() for s in segments).strip()
        lag  = time.perf_counter() - t0

        if text:
            print(f"[{info.language}|{lag:.1f}s lag] {text}", flush=True)
            # {\an2} = ASS bottom-center alignment tag (MPV OSD respects it)
            mpv_cmd(["show-text", r"{\an2}" + text, CAPTION_MS])


def launch_mpv() -> subprocess.Popen:
    global _mpv_ipc_path
    print("[mpv] Starting...", flush=True)
    _mpv_ipc_path = rf"\\.\pipe\{MPV_IPC_BASE}-{uuid4().hex}"
    proc = subprocess.Popen(
        [
            "mpv",
            f"--input-ipc-server={_mpv_ipc_path}",
            "--cache=yes",
            "--keep-open=always",
            "--demuxer-readahead-secs=30",
            "--sid=no",
            "--sub-visibility=no",
            "--osd-level=3",
            "--osd-font-size=38",
            "--osd-bar=no",
            STREAM_URL,
        ]
    )

    # Wait for MPV + IPC socket to initialize
    time.sleep(2.5)

    # Pause video to build a buffer equal to expected Whisper lag (lip sync)
    mpv_cmd(["set_property", "pause", True])
    mpv_cmd(["show-text", "[whisper-sub] OSD test", 2500])
    print(f"[sync] Pre-buffering {VIDEO_DELAY:.0f}s for lip sync...", flush=True)
    time.sleep(VIDEO_DELAY)
    mpv_cmd(["set_property", "pause", False])
    print("[sync] Playing. Captions active.", flush=True)

    return proc


def check_deps() -> None:
    missing = []
    for tool in ("ffmpeg", "mpv"):
        result = subprocess.run(
            ["where", tool], capture_output=True, text=True
        )
        if result.returncode != 0:
            missing.append(tool)
    if missing:
        print(f"[error] Missing tools: {', '.join(missing)}")
        print("        Install and add to PATH: https://ffmpeg.org  https://mpv.io")
        sys.exit(1)


def main() -> None:
    global _mpv_proc
    check_deps()
    load_model()
    _mpv_proc = launch_mpv()

    thread = threading.Thread(target=caption_loop, daemon=True)
    thread.start()

    try:
        _mpv_proc.wait()
    except KeyboardInterrupt:
        pass
    finally:
        _mpv_proc.terminate()
        print("\n[exit]")


if __name__ == "__main__":
    main()
