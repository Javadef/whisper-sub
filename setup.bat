@echo off
echo [setup] Installing Python dependencies...
pip install -r requirements.txt
if errorlevel 1 (
    echo [error] pip install failed. Ensure Python is installed.
    pause & exit /b 1
)

echo.
echo [check] ffmpeg...
where ffmpeg >nul 2>&1 || echo   WARNING: ffmpeg not in PATH. Download: https://ffmpeg.org/download.html

echo [check] mpv...
where mpv >nul 2>&1 || echo   WARNING: mpv not in PATH. Download: https://mpv.io/installation/

echo.
echo [done] Setup complete. Run: python whisper_sub.py
pause
