// SDI passthrough: SDI In 1 -> [CPU: subtitle overlay] -> SDI Out 1
// Device: KONA LHi (index 0)
// Single-thread loop: capture -> draw overlay -> output (no queue, no stutter)

#include "ntv2card.h"
#include "ntv2devicefeatures.h"
#include "ntv2signalrouter.h"
#include "ntv2formatdescriptor.h"
#include "ntv2utils.h"
#include "ntv2enums.h"
#include "ajabase/system/systemtime.h"
#include "ajabase/system/process.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <csignal>
#include <cstring>

static constexpr uint32_t kAudioSizeMax = 401 * 1024;

static constexpr ULWord kAppType = NTV2_FOURCC('S','D','P','T');
static std::atomic<bool> gQuit{false};

static void OnSignal(int) { gQuit = true; }
static BOOL WINAPI ConsoleHandler(DWORD e)
{
    if (e == CTRL_C_EVENT || e == CTRL_BREAK_EVENT) { gQuit = true; return TRUE; }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Subtitle overlay – reads a UTF-8 text file and renders it using Windows GDI.
// Set kSubtitleFile to the path your caption generator writes to
// (e.g. the sdi2sdi_cap1.txt produced by whisper_sub / web_sub).
// ---------------------------------------------------------------------------
static constexpr const char* kSubtitleFile  = "C:\\Users\\Java\\Desktop\\whisper-sub\\sdi2sdi_cap1.txt";
static constexpr uint16_t    kAudioPort     = 9876;

// ---------------------------------------------------------------------------
// Audio TCP server — writes 16kHz mono s16le PCM on port 9876.
// Downsamples from 48kHz stereo 32-bit SDI audio.
// web_sub.py connects via: ffmpeg -f s16le -ar 16000 -ac 1 -i tcp://127.0.0.1:9876
// ---------------------------------------------------------------------------
struct AudioServer
{
    SOCKET listenSock = INVALID_SOCKET;
    SOCKET clientSock = INVALID_SOCKET;
    bool   connected  = false;

    bool Init()
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) return false;
        int opt = 1;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = htons(kAudioPort);
        if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (::listen(listenSock, 1) != 0) return false;
        return true;
    }

    // Accept next client; polls so gQuit is respected
    bool WaitForClient()
    {
        while (!gQuit) {
            fd_set fds; FD_ZERO(&fds); FD_SET(listenSock, &fds);
            timeval tv{0, 200000};
            if (select(0, &fds, nullptr, nullptr, &tv) > 0) {
                clientSock = accept(listenSock, nullptr, nullptr);
                if (clientSock != INVALID_SOCKET) { connected = true; return true; }
            }
        }
        return false;
    }

    bool Write(const int16_t* data, size_t samples)
    {
        if (!connected) return false;
        const char* ptr = reinterpret_cast<const char*>(data);
        int rem = static_cast<int>(samples * sizeof(int16_t));
        while (rem > 0) {
            int n = send(clientSock, ptr, rem, 0);
            if (n == SOCKET_ERROR) { Disconnect(); return false; }
            ptr += n; rem -= n;
        }
        return true;
    }

    void Disconnect()
    {
        if (clientSock != INVALID_SOCKET) { closesocket(clientSock); clientSock = INVALID_SOCKET; }
        connected = false;
    }
    void Close()
    {
        Disconnect();
        if (listenSock != INVALID_SOCKET) { closesocket(listenSock); listenSock = INVALID_SOCKET; }
        WSACleanup();
    }
};

// Downsample 48kHz stereo s32 SDI audio → 16kHz mono s16le.
// SDI audio: interleaved ch0 ch1 ch2 ... (32-bit little-endian per sample).
// Simple 3:1 decimation (every 3rd sample) — adequate for speech.
static size_t DownsampleSdiAudio(const uint8_t* src, uint32_t srcBytes,
                                  uint16_t nCh,
                                  std::vector<int16_t>& out)
{
    // each SDI sample = 4 bytes (32-bit)
    const uint32_t frameSamples = srcBytes / (nCh * 4);  // samples per channel
    // 48000 -> 16000 = factor 3
    const uint32_t outSamples   = frameSamples / 3;
    out.resize(outSamples);
    for (uint32_t i = 0; i < outSamples; ++i)
    {
        // Read ch0 + ch1, average to mono, take every 3rd frame
        const uint8_t* p = src + (i * 3) * nCh * 4;
        int32_t s0, s1;
        std::memcpy(&s0, p,         4);
        std::memcpy(&s1, p + 4,     4);
        // SDI 32-bit audio: actual sample in bits [31:8] (24-bit in high bytes)
        int32_t mono = ((int64_t)s0 + s1) / 2;
        out[i] = static_cast<int16_t>(mono >> 16);
    }
    return outSamples;
}

// Read subtitle text from file (UTF-8).
static std::string ReadSubtitleFile(const char* path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    char buf[4096]{};
    DWORD nRead = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &nRead, nullptr);
    CloseHandle(h);
    // Trim trailing whitespace / newlines
    while (nRead > 0 && (buf[nRead-1] == '\n' || buf[nRead-1] == '\r' || buf[nRead-1] == ' '))
        buf[--nRead] = '\0';
    return std::string(buf, nRead);
}

// UTF-8 -> UTF-16 for GDI.
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

// Render subtitle text onto a UYVY (8-bit 4:2:2) frame using Windows GDI.
// NTV2_FBF_8BIT_YCBCR byte order: Cb Y0 Cr Y1 ("uyvy"/"2vuy").
// Text is centred horizontally, placed kMarginBottom pixels from the bottom.
static void DrawSubtitle(uint8_t* buf, int stride, int frameW, int frameH,
                          const std::wstring& text)
{
    if (text.empty()) return;

    static const int kFontPx       = 48;
    static const int kPadX         = 24;
    static const int kPadY         = 12;
    static const int kMarginBottom = 80;

    HDC   hdcRef  = GetDC(nullptr);
    HDC   hdcMem  = CreateCompatibleDC(hdcRef);
    HFONT hFont   = CreateFontW(-kFontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");
    auto* hOldFont = static_cast<HFONT>(SelectObject(hdcMem, hFont));

    // Measure text bounding box (supports multi-line via DT_CALCRECT)
    RECT rcMeasure{ 0, 0, frameW - kPadX * 2, 0 };
    DrawTextW(hdcMem, text.c_str(), -1, &rcMeasure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    const int textW = rcMeasure.right  - rcMeasure.left;
    const int textH = rcMeasure.bottom - rcMeasure.top;

    const int bmpW = (textW + kPadX * 2 + 1) & ~1;
    const int bmpH = textH + kPadY * 2;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = bmpW;
    bmi.bmiHeader.biHeight      = -bmpH;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void*   pBits  = nullptr;
    HBITMAP hBmp   = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    auto*   hOldBmp = static_cast<HBITMAP>(SelectObject(hdcMem, hBmp));

    HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 0));
    RECT   rcFill{ 0, 0, bmpW, bmpH };
    FillRect(hdcMem, &rcFill, hBrush);
    DeleteObject(hBrush);

    SetTextColor(hdcMem, RGB(255, 255, 255));
    SetBkMode(hdcMem, TRANSPARENT);
    RECT rcText{ kPadX, kPadY, kPadX + textW, kPadY + textH };
    DrawTextW(hdcMem, text.c_str(), -1, &rcText, DT_WORDBREAK | DT_NOPREFIX);
    GdiFlush();

    const int dstX = ((frameW - bmpW) / 2) & ~1;
    const int dstY = frameH - bmpH - kMarginBottom;

    if (pBits && dstX >= 0 && dstY >= 0)
    {
        const int        copyW     = (bmpW < frameW - dstX ? bmpW : frameW - dstX) & ~1;
        const int        copyH     = (bmpH < frameH - dstY ? bmpH : frameH - dstY);
        const uint8_t*   pixels    = static_cast<const uint8_t*>(pBits);
        const int        dibStride = bmpW * 4;

        for (int row = 0; row < copyH; ++row)
        {
            const uint8_t* srcRow = pixels + row * dibStride;
            for (int col = 0; col < copyW; col += 2)
            {
                // GDI DIB pixel order: B G R A
                const uint8_t* p0 = srcRow + col * 4;
                const uint8_t* p1 = p0 + 4;

                // BT.601 luma (limited range 16-235)
                auto toY = [](uint8_t R, uint8_t G, uint8_t B) -> uint8_t {
                    return static_cast<uint8_t>(16 + ((R * 66 + G * 129 + B * 25 + 128) >> 8));
                };

                const uint8_t Y0 = toY(p0[2], p0[1], p0[0]);
                const uint8_t Y1 = toY(p1[2], p1[1], p1[0]);

                // Write UYVY pair: Cb Y0 Cr Y1 (NTV2_FBF_8BIT_YCBCR = "uyvy"/"2vuy")
                uint8_t* dst = buf + (dstY + row) * stride + ((dstX + col) / 2) * 4;
                dst[0] = 128;  // Cb — neutral chroma
                dst[1] = Y0;
                dst[2] = 128;  // Cr — neutral chroma
                dst[3] = Y1;
            }
        }
    }

    SelectObject(hdcMem, hOldBmp);
    SelectObject(hdcMem, hOldFont);
    DeleteObject(hBmp);
    DeleteObject(hFont);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcRef);
}

// Draw small "AI" watermark at bottom-right corner, semi-transparent.
static void DrawWatermark(uint8_t* buf, int stride, int frameW, int frameH)
{
    static const wchar_t* kWText = L"AI";
    static const int kFontPx = 28;
    static const int kPad = 8;
    static const int kMarginRight = 12;
    static const int kMarginBottom = 12;
    static const int kAlpha = 160;  // ~63% opacity

    HDC   hdcRef = GetDC(nullptr);
    HDC   hdcMem = CreateCompatibleDC(hdcRef);
    HFONT hFont  = CreateFontW(-kFontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");
    auto* hOldFont = static_cast<HFONT>(SelectObject(hdcMem, hFont));

    RECT rc{ 0, 0, 200, 100 };
    DrawTextW(hdcMem, kWText, -1, &rc, DT_CALCRECT | DT_NOPREFIX);
    int textW = rc.right - rc.left;
    int textH = rc.bottom - rc.top;

    int bmpW = (textW + kPad * 2 + 1) & ~1;
    int bmpH = textH + kPad * 2;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = bmpW;
    bmi.bmiHeader.biHeight      = -bmpH;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void*   pBits = nullptr;
    HBITMAP hBmp  = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    auto*   hOldBmp = static_cast<HBITMAP>(SelectObject(hdcMem, hBmp));

    HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 0));
    RECT   rcFill{ 0, 0, bmpW, bmpH };
    FillRect(hdcMem, &rcFill, hBrush);
    DeleteObject(hBrush);

    SetTextColor(hdcMem, RGB(255, 255, 255));
    SetBkMode(hdcMem, TRANSPARENT);
    RECT rcText{ kPad, kPad, kPad + textW, kPad + textH };
    DrawTextW(hdcMem, kWText, -1, &rcText, DT_NOPREFIX);
    GdiFlush();

    int dstX = (frameW - bmpW - kMarginRight) & ~1;
    int dstY = frameH - bmpH - kMarginBottom;

    if (pBits && dstX >= 0 && dstY >= 0)
    {
        int copyW = (bmpW < frameW - dstX ? bmpW : frameW - dstX) & ~1;
        int copyH = (bmpH < frameH - dstY ? bmpH : frameH - dstY);
        auto* pixels = static_cast<const uint8_t*>(pBits);
        int dibStride = bmpW * 4;

        auto toY = [](uint8_t R, uint8_t G, uint8_t B) -> uint8_t {
            return static_cast<uint8_t>(16 + ((R * 66 + G * 129 + B * 25 + 128) >> 8));
        };

        for (int row = 0; row < copyH; ++row)
        {
            const uint8_t* srcRow = pixels + row * dibStride;
            for (int col = 0; col < copyW; col += 2)
            {
                const uint8_t* p0 = srcRow + col * 4;
                const uint8_t* p1 = p0 + 4;
                uint8_t wY0 = toY(p0[2], p0[1], p0[0]);
                uint8_t wY1 = toY(p1[2], p1[1], p1[0]);

                uint8_t* dst = buf + (dstY + row) * stride + ((dstX + col) / 2) * 4;
                dst[1] = (uint8_t)(((int)wY0 * kAlpha + (int)dst[1] * (255 - kAlpha)) / 255);
                dst[3] = (uint8_t)(((int)wY1 * kAlpha + (int)dst[3] * (255 - kAlpha)) / 255);
                // Neutral chroma
                dst[0] = (uint8_t)(((int)128 * kAlpha + (int)dst[0] * (255 - kAlpha)) / 255);
                dst[2] = (uint8_t)(((int)128 * kAlpha + (int)dst[2] * (255 - kAlpha)) / 255);
            }
        }
    }

    SelectObject(hdcMem, hOldBmp);
    SelectObject(hdcMem, hOldFont);
    DeleteObject(hBmp);
    DeleteObject(hFont);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcRef);
}

// ---------------------------------------------------------------------------

int main()
{
    std::signal(SIGINT, OnSignal);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    // -----------------------------------------------------------------------
    // 1. Open device
    // -----------------------------------------------------------------------
    CNTV2Card device;
    if (!device.Open(0))
    {
        std::cerr << "ERROR: Cannot open device 0\n";
        return 1;
    }
    std::cout << "Device: " << device.GetDisplayName() << "\n";

    if (!device.AcquireStreamForApplication(kAppType, int32_t(AJAProcess::GetPid())))
    {
        std::cerr << "ERROR: device busy\n";
        device.Close();
        return 1;
    }

    // OEM_TASKS: AJA service won't override our crosspoint / mode settings
    NTV2TaskMode savedTaskMode;
    device.GetEveryFrameServices(savedTaskMode);
    device.SetEveryFrameServices(NTV2_OEM_TASKS);

    if (device.features().CanDoMultiFormat())
        device.SetMultiFormatMode(true);

    for (int i = 0; i < 8; i++)
        device.AutoCirculateStop(NTV2Channel(i));

    // -----------------------------------------------------------------------
    // 2. Wait for input signal
    // -----------------------------------------------------------------------
    const NTV2Channel     inCh  = NTV2_CHANNEL1;
    const NTV2Channel     outCh = NTV2_CHANNEL2;
    const NTV2InputSource inSrc = NTV2_INPUTSOURCE_SDI1;

    device.EnableChannel(inCh);
    if (device.features().HasBiDirectionalSDI())
        device.SetSDITransmitEnable(inCh, false);
    device.EnableInputInterrupt(inCh);
    device.SubscribeInputVerticalEvent(inCh);

    NTV2VideoFormat fmt = NTV2_FORMAT_UNKNOWN;
    for (int i = 0; i < 50 && fmt == NTV2_FORMAT_UNKNOWN && !gQuit; i++)
    {
        fmt = device.GetInputVideoFormat(inSrc);
        if (fmt == NTV2_FORMAT_UNKNOWN)
        {
            std::cout << "\rWaiting for SDI In 1 signal..." << std::flush;
            AJATime::Sleep(200);
        }
    }
    std::cout << "\n";
    if (fmt == NTV2_FORMAT_UNKNOWN)
    {
        std::cerr << "ERROR: No signal on SDI In 1\n";
        device.ReleaseStreamForApplication(kAppType, int32_t(AJAProcess::GetPid()));
        device.SetEveryFrameServices(savedTaskMode);
        device.Close();
        return 1;
    }
    std::cout << "Signal: " << NTV2VideoFormatToString(fmt) << "\n";

    // -----------------------------------------------------------------------
    // 3. Frame geometry
    // -----------------------------------------------------------------------
    const NTV2FrameBufferFormat kFBF = NTV2_FBF_8BIT_YCBCR;
    const NTV2FormatDescriptor  fd(fmt, kFBF);
    const ULWord  W      = fd.GetRasterWidth();
    const ULWord  H      = fd.GetRasterHeight();
    const ULWord  fSize  = fd.GetTotalBytes();
    const int     stride = static_cast<int>(W * 2);
    std::cout << "Frame: " << W << "x" << H << "  bytes=" << fSize << "\n";

    // -----------------------------------------------------------------------
    // 4. Configure input channel
    // -----------------------------------------------------------------------
    device.SetVideoFormat(fmt, false, false, inCh);
    device.SetFrameBufferFormat(inCh, kFBF);

    // -----------------------------------------------------------------------
    // 5. Configure output channel
    // -----------------------------------------------------------------------
    device.EnableChannel(outCh);
    device.SetVideoFormat(fmt, false, false, outCh);
    device.SetFrameBufferFormat(outCh, kFBF);
    device.SetVANCMode(NTV2_VANCMODE_OFF, outCh);
    // Subscribe to Ch1 output VBI -- KONA LHi's only output interrupt source.
    device.EnableOutputInterrupt(NTV2_CHANNEL1);
    device.SubscribeOutputVerticalEvent(NTV2_CHANNEL1);

    // Enable SDI transmit on Ch1 output pin.
    // KONA LHi SDI Out 1 is Ch1's serializer regardless of which frame store
    // drives it via crosspoint.  Ch2 has no physical SDI port on this board.
    if (device.features().HasBiDirectionalSDI())
        device.SetSDITransmitEnable(NTV2_CHANNEL1, true);

    // Lock output clock to SDI In 1 -- eliminates drift/stutter on passthrough.
    // Fall back to freerun only if input has no signal.
    device.SetReference(NTV2_REFERENCE_INPUT1);

    // Route Ch2 frame buffer -> SDI Out 1.
    // On single-output boards (KONA LHi) NTV2ChannelToOutputDestination(Ch2)
    // returns SDI2 which does not exist.  Force SDI1 explicitly.
    device.Connect(::GetOutputDestInputXpt(NTV2_OUTPUTDESTINATION_SDI1),
                   ::GetFrameBufferOutputXptFromChannel(outCh, /*isDS2=*/false));

    // -----------------------------------------------------------------------
    // 6. Audio setup
    // -----------------------------------------------------------------------
    const int _maxAudCh = (int)device.features().GetMaxAudioChannels();
    const uint16_t audCh = static_cast<uint16_t>(_maxAudCh < 8 ? _maxAudCh : 8);
    const NTV2AudioSystem inAudSys  = NTV2_AUDIOSYSTEM_1;
    const NTV2AudioSystem outAudSys =
        (device.features().GetNumAudioSystems() >= 2)
        ? NTV2_AUDIOSYSTEM_2 : NTV2_AUDIOSYSTEM_1;

    device.SetNumberAudioChannels(audCh, inAudSys);
    device.SetAudioRate(NTV2_AUDIO_48K, inAudSys);
    device.SetAudioBufferSize(NTV2_AUDIO_BUFFER_BIG, inAudSys);
    device.SetAudioSystemInputSource(inAudSys, NTV2_AUDIO_EMBEDDED,
        ::NTV2ChannelToEmbeddedAudioInput(inCh));

    device.SetNumberAudioChannels(audCh, outAudSys);
    device.SetAudioRate(NTV2_AUDIO_48K, outAudSys);
    device.SetAudioBufferSize(NTV2_AUDIO_BUFFER_BIG, outAudSys);
    device.SetSDIOutputAudioSystem(outCh, outAudSys);
    device.SetSDIOutputDS2AudioSystem(outCh, outAudSys);
    device.SetAudioLoopBack(NTV2_AUDIO_LOOPBACK_OFF, outAudSys);

    NTV2FrameRate fr = NTV2_FRAMERATE_UNKNOWN;
    device.GetFrameRate(fr, outCh);
    ULWord frN = 25, frD = 1;
    ::GetFramesPerSecond(fr, frN, frD);
    const uint32_t audioBytesPerFrame =
        (uint32_t)((uint64_t)48000 * frD / frN) * audCh * sizeof(uint32_t);

    // -----------------------------------------------------------------------
    // 7. AutoCirculate init — ring of 150 frames (~6 s delay at 25 fps)
    //    Video output lags input by ~6 s so Whisper subtitles arrive in time.
    // -----------------------------------------------------------------------
    constexpr int RING = 150;

    std::vector<std::vector<ULWord>> vidBufs(RING), audBufs(RING);
    std::vector<ULWord> audBytesArr(RING, 0);
    for (int i = 0; i < RING; i++) {
        vidBufs[i].resize((fSize + 3) / 4, 0);
        audBufs[i].resize((kAudioSizeMax + 3) / 4, 0);
    }

    if (!device.AutoCirculateInitForInput(inCh, 5, inAudSys,
                                          AUTOCIRCULATE_WITH_RP188))
    {
        std::cerr << "ERROR: AutoCirculateInitForInput failed\n";
        device.ReleaseStreamForApplication(kAppType, int32_t(AJAProcess::GetPid()));
        device.SetEveryFrameServices(savedTaskMode);
        device.Close();
        return 1;
    }
    if (!device.AutoCirculateInitForOutput(outCh, 4, outAudSys, 0))
    {
        std::cerr << "ERROR: AutoCirculateInitForOutput failed\n";
        device.ReleaseStreamForApplication(kAppType, int32_t(AJAProcess::GetPid()));
        device.SetEveryFrameServices(savedTaskMode);
        device.Close();
        return 1;
    }

    device.AutoCirculateStart(inCh);

    // Start audio TCP server thread
    AudioServer audioServer;
    std::thread audioServerThread;
    if (audioServer.Init())
    {
        std::cout << "[audio tcp] listening on 127.0.0.1:" << kAudioPort << " (16kHz mono s16le)\n";
        audioServerThread = std::thread([&]() {
            while (!gQuit) {
                if (!audioServer.WaitForClient()) break;
                std::cout << "[audio tcp] client connected\n" << std::flush;
                while (!gQuit && audioServer.connected) AJATime::Sleep(100);
                if (!gQuit) { audioServer.Disconnect(); std::cout << "[audio tcp] client disconnected, waiting...\n" << std::flush; }
            }
        });
    }
    else
    {
        std::cerr << "[audio tcp] WARNING: could not start TCP server on port " << kAudioPort << "\n";
    }

    std::cout << "ACTIVE: SDI In 1 -> [subtitle: " << kSubtitleFile << "] -> SDI Out 1\n"
              << "Press Ctrl+C to stop.\n\n";

    // -----------------------------------------------------------------------
    // 8. Two-thread pipeline:
    //    captureThread: DMA input -> DrawTimestamp -> push to ring
    //    main thread:   pop from ring -> DMA output
    //    Ring depth 6 gives ~3 frames of headroom so buf stays > 0.
    // -----------------------------------------------------------------------
    std::atomic<int>      ringHead{0};  // producer advances
    std::atomic<int>      ringTail{0};  // consumer advances
    std::atomic<uint32_t> nIn{0};

    std::thread captureThread([&]() {
        AUTOCIRCULATE_TRANSFER xfer;
        AUTOCIRCULATE_STATUS   st;
        while (!gQuit) {
            device.AutoCirculateGetStatus(inCh, st);
            if (!st.IsRunning() || !st.HasAvailableInputFrame()) {
                device.WaitForInputVerticalInterrupt(inCh);
                continue;
            }
            // Stall if ring is full
            while (!gQuit) {
                int h = ringHead.load(std::memory_order_relaxed);
                int t = ringTail.load(std::memory_order_acquire);
                if (h - t < RING - 1) break;
                AJATime::Sleep(2);
            }
            if (gQuit) break;
            const int ri = ringHead.load(std::memory_order_relaxed) % RING;
            xfer.SetVideoBuffer(vidBufs[ri].data(), fSize);
            xfer.SetAudioBuffer(audBufs[ri].data(), kAudioSizeMax);
            device.AutoCirculateTransfer(inCh, xfer);
            ULWord bytes = xfer.GetCapturedAudioByteCount();
            if (bytes == 0) bytes = audioBytesPerFrame;
            bytes = (bytes / (audCh * 4)) * (audCh * 4);
            if (bytes == 0) bytes = audioBytesPerFrame;
            audBytesArr[ri] = bytes;
            // Write downsampled audio to TCP client (skip if no client connected)
            if (audioServer.connected && bytes > 0)
            {
                static std::vector<int16_t> pcmOut;
                size_t n = DownsampleSdiAudio(
                    reinterpret_cast<const uint8_t*>(audBufs[ri].data()),
                    bytes, audCh, pcmOut);
                if (n > 0) audioServer.Write(pcmOut.data(), n);
            }
            // NOTE: DrawSubtitle is called in output thread (just before SDI Out),
            // so the subtitle text is current when the delayed frame is transmitted.
            ringHead.fetch_add(1, std::memory_order_release);
            nIn.fetch_add(1, std::memory_order_relaxed);
        }
    });

    AUTOCIRCULATE_TRANSFER outXfer;
    AUTOCIRCULATE_STATUS   outSt;
    uint32_t nOut = 0;
    bool outputStarted = false;

    while (!gQuit) {
        // Wait until ring has enough frames buffered.
        // Before output starts, wait for RING-2 frames (full ~6 s delay).
        // After start, just need at least 1 frame ready.
        const int h = ringHead.load(std::memory_order_acquire);
        const int t = ringTail.load(std::memory_order_relaxed);
        const int depth = h - t;
        if (!outputStarted && depth < RING - 2) {
            AJATime::Sleep(2);
            continue;
        }
        if (depth == 0) {
            AJATime::Sleep(2);
            continue;
        }

        // Wait for output AC to accept
        device.AutoCirculateGetStatus(outCh, outSt);
        if (!outSt.CanAcceptMoreOutputFrames()) {
            device.WaitForOutputVerticalInterrupt(NTV2_CHANNEL1);
            continue;
        }

        const int ri = t % RING;
        // Draw subtitle NOW (at output time, 6s after capture).
        // Whisper had 6s to transcribe; subtitle file is current.
        DrawSubtitle(reinterpret_cast<uint8_t*>(vidBufs[ri].data()),
                     stride, static_cast<int>(W), static_cast<int>(H),
                     Utf8ToWide(ReadSubtitleFile(kSubtitleFile)));
        DrawWatermark(reinterpret_cast<uint8_t*>(vidBufs[ri].data()),
                      stride, static_cast<int>(W), static_cast<int>(H));
        outXfer.SetVideoBuffer(vidBufs[ri].data(), fSize);
        outXfer.SetAudioBuffer(audBufs[ri].data(), audBytesArr[ri]);
        if (device.AutoCirculateTransfer(outCh, outXfer)) nOut++;
        ringTail.store(t + 1, std::memory_order_release);

        // Start output once ring delay buffer is full
        if (!outputStarted && nOut >= 1) {
            device.AutoCirculateStart(outCh);
            outputStarted = true;
        }

        if (nOut % 25 == 0) {
            const int avail = ringHead.load(std::memory_order_relaxed)
                            - ringTail.load(std::memory_order_relaxed);
            std::cout << "\rIn=" << nIn.load() << "  Out=" << nOut
                      << "  ring=" << avail
                      << "  buf=" << outSt.acBufferLevel
                      << "    " << std::flush;
        }
    }

    captureThread.join();
    audioServer.Disconnect();
    audioServer.Close();
    if (audioServerThread.joinable()) audioServerThread.join();

    // -----------------------------------------------------------------------
    // 9. Shutdown
    // -----------------------------------------------------------------------
    std::cout << "\nShutting down.\n";
    device.AutoCirculateStop(inCh);
    device.AutoCirculateStop(outCh);

    device.UnsubscribeInputVerticalEvent(inCh);
    device.UnsubscribeOutputVerticalEvent(NTV2_CHANNEL1);
    device.ReleaseStreamForApplication(kAppType, int32_t(AJAProcess::GetPid()));
    device.SetEveryFrameServices(savedTaskMode);
    device.Close();
    return 0;
}
