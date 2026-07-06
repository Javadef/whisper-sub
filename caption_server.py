#!/usr/bin/env python3
"""
Simple HTTP server that serves the latest caption from sdi2sdi_cap1.txt
for Harmonic templates to fetch. Run alongside whisper_sub.py.

Usage:  python caption_server.py [--port 8080]
"""
import argparse
import http.server
import os
import threading
import time
from pathlib import Path

ROOT_DIR = Path(__file__).parent
CAP_FILE = ROOT_DIR / "sdi2sdi_cap1.txt"
LATEST_FILE = ROOT_DIR / "latest_caption.txt"

_last_text = ""

def watch_caption_file():
    global _last_text
    last_mtime = 0
    while True:
        try:
            mtime = CAP_FILE.stat().st_mtime
            if mtime != last_mtime:
                last_mtime = mtime
                text = CAP_FILE.read_text(encoding="utf-8").strip()
                if text:
                    _last_text = text
                    LATEST_FILE.write_text(text, encoding="utf-8")
        except Exception:
            pass
        time.sleep(0.1)


class CaptionHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT_DIR), **kwargs)

    def do_GET(self):
        if self.path == "/caption" or self.path == "/caption.txt":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write((_last_text + "\n").encode("utf-8"))
        else:
            super().do_GET()

    def log_message(self, fmt, *args):
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    threading.Thread(target=watch_caption_file, daemon=True).start()

    server = http.server.HTTPServer(("0.0.0.0", args.port), CaptionHandler)
    print(f"[caption-server] http://localhost:{args.port}/caption")
    print(f"[caption-server] Watching {CAP_FILE}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[caption-server] stopped.")


if __name__ == "__main__":
    main()
