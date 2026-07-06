// SDI passthrough: SDI In 1 -> [subtitle overlay bar] -> SDI Out 3

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
#include <algorithm>

static constexpr uint32_t kAudioSizeMax = 401 * 1024;
static constexpr ULWord   kAppType      = NTV2_FOURCC('S','D','P','T');
static constexpr const char* kSubtitleFile = "C:\\Users\\Java\\Desktop\\whisper-sub\\sdi2sdi_cap1.txt";
static constexpr uint16_t    kAudioPort    = 9876;
static std::atomic<bool> gQuit{false};

static void OnSignal(int) { gQuit = true; }
static BOOL WINAPI ConsoleHandler(DWORD e) {
    if (e == CTRL_C_EVENT || e == CTRL_BREAK_EVENT) { gQuit = true; return TRUE; }
    return FALSE;
}

// ── Audio TCP server ────────────────────────────────────────────────────

struct AudioServer {
    SOCKET listenSock = INVALID_SOCKET;
    SOCKET clientSock = INVALID_SOCKET;
    bool   connected  = false;

    bool Init() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) return false;
        int opt = 1;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(kAudioPort);
        if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) != 0) return false;
        if (::listen(listenSock, 1) != 0) return false;
        return true;
    }
    bool WaitForClient() {
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
    bool Write(const int16_t* data, size_t samples) {
        if (!connected) return false;
        const char* ptr = (const char*)data;
        int rem = (int)(samples * sizeof(int16_t));
        while (rem > 0) {
            int n = send(clientSock, ptr, rem, 0);
            if (n == SOCKET_ERROR) { Disconnect(); return false; }
            ptr += n; rem -= n;
        }
        return true;
    }
    void Disconnect() {
        if (clientSock != INVALID_SOCKET) { closesocket(clientSock); clientSock = INVALID_SOCKET; }
        connected = false;
    }
    void Close() { Disconnect(); if (listenSock != INVALID_SOCKET) { closesocket(listenSock); listenSock = INVALID_SOCKET; } WSACleanup(); }
};

// ── Audio downsampler ───────────────────────────────────────────────────

static size_t DownsampleSdiAudio(const uint8_t* src, uint32_t srcBytes, uint16_t nCh, std::vector<int16_t>& out) {
    const uint32_t frameSamples = srcBytes / (nCh * 4);
    const uint32_t outSamples = frameSamples / 3;
    out.resize(outSamples);
    for (uint32_t i = 0; i < outSamples; ++i) {
        const uint8_t* p = src + (i * 3) * nCh * 4;
        int32_t s0, s1;
        std::memcpy(&s0, p, 4);
        std::memcpy(&s1, p + 4, 4);
        out[i] = (int16_t)((((int64_t)s0 + s1) / 2) >> 16);
    }
    return outSamples;
}

// ── File I/O helpers ────────────────────────────────────────────────────

static std::string ReadSubtitleFile(const char* path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    char buf[4096]{};
    DWORD nRead = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &nRead, nullptr);
    CloseHandle(h);
    while (nRead > 0 && (buf[nRead-1] == '\n' || buf[nRead-1] == '\r' || buf[nRead-1] == ' '))
        buf[--nRead] = '\0';
    return std::string(buf, nRead);
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

// ── Overlay bar (pure GDI) ──────────────────────────────────────────────

static void SaveFrameAsBMP(const char* path, const uint8_t* uyvy, int W, int H, int stride) {
    int rowSize = ((W * 3 + 3) / 4) * 4;
    int dataSize = rowSize * H;
    int fileSize = 54 + dataSize;
    std::vector<uint8_t> buf(54 + dataSize, 0);
    buf[0] = 'B'; buf[1] = 'M';
    std::memcpy(&buf[2],  &fileSize, 4);
    buf[10] = 54; buf[14] = 40;
    std::memcpy(&buf[18], &W, 4);
    std::memcpy(&buf[22], &H, 4);
    buf[26] = 1; buf[28] = 24;
    std::memcpy(&buf[34], &dataSize, 4);

    auto clamp = [](int v) -> uint8_t { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    for (int y = H - 1; y >= 0; --y) {
        const uint8_t* src = uyvy + y * stride;
        uint8_t* dst = &buf[54] + (H - 1 - y) * rowSize;
        for (int x = 0; x < W; x += 2) {
            const uint8_t* p = src + (x / 2) * 4;
            int u = p[0] - 128, v = p[2] - 128;
            dst[0] = clamp(p[1] + (u * 1772 / 1000));  // B = Y0 + 1.772*U
            dst[1] = clamp(p[1] - (u * 344 / 1000) - (v * 714 / 1000)); // G
            dst[2] = clamp(p[1] + (v * 1402 / 1000));  // R = Y0 + 1.402*V
            dst += 3;
            if (x + 1 < W) {
                dst[0] = clamp(p[3] + (u * 1772 / 1000));
                dst[1] = clamp(p[3] - (u * 344 / 1000) - (v * 714 / 1000));
                dst[2] = clamp(p[3] + (v * 1402 / 1000));
                dst += 3;
            }
        }
    }

    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(h, buf.data(), (DWORD)buf.size(), &written, nullptr);
        CloseHandle(h);
    }
}

static void DrawOverlay(uint8_t* buf, int stride, int frameW, int frameH,
                         const std::wstring& text)
{
    static const int kBarHeight = 72;
    static const int kFontPx    = 40;
    static const int kLabelPx   = 32;
    static const int kPad       = 24;
    static const int kGap       = 32;
    static const int kBarAlpha  = 220;

    static HBITMAP sBg = nullptr;
    static int sBgW = 0, sBgH = 0;
    static bool sTried = false;
    if (!sTried) {
        sTried = true;
        sBg = (HBITMAP)LoadImageA(nullptr, "C:\\Users\\Java\\Desktop\\whisper-sub\\bg.bmp",
                                  IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
        if (sBg) { BITMAP bm; GetObject(sBg, sizeof(bm), &bm); sBgW = bm.bmWidth; sBgH = bm.bmHeight; }
    }

    const int barW = frameW;
    const int barH = kBarHeight;
    const int barY = frameH - barH;
    if (barY < 0) return;

    HDC hdcRef = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcRef);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = barW;
    bmi.bmiHeader.biHeight      = -barH;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    HGDIOBJ hOldBmp = SelectObject(hdcMem, hBmp);

    // 1. Background image — crop a proportional horizontal strip, then scale
    if (sBg) {
        HDC hdcBg = CreateCompatibleDC(hdcRef);
        HGDIOBJ hOldBg = SelectObject(hdcBg, sBg);
        SetStretchBltMode(hdcMem, HALFTONE);
        int cropH = (int)((int64_t)barH * sBgW / barW);   // aspect-matched source height
        if (cropH < 1) cropH = 1;
        if (cropH > sBgH) cropH = sBgH;
        int srcY = (sBgH - cropH) / 2;                     // centered vertically
        StretchBlt(hdcMem, 0, 0, barW, barH,
                   hdcBg, 0, srcY, sBgW, cropH, SRCCOPY);
        SelectObject(hdcBg, hOldBg);
        DeleteDC(hdcBg);
    } else {
        HBRUSH hBr = CreateSolidBrush(RGB(20, 20, 35));
        RECT r{0, 0, barW, barH}; FillRect(hdcMem, &r, hBr); DeleteObject(hBr);
    }

    // 2. Semi-transparent dark overlay using AlphaBlend
    {
        BITMAPINFO bmiDim{}; bmiDim.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmiDim.bmiHeader.biWidth = 1; bmiDim.bmiHeader.biHeight = -1;
        bmiDim.bmiHeader.biPlanes = 1; bmiDim.bmiHeader.biBitCount = 32;
        bmiDim.bmiHeader.biCompression = BI_RGB;
        void* dimBits = nullptr;
        HDC hdcDim = CreateCompatibleDC(hdcRef);
        HBITMAP hDim = CreateDIBSection(hdcDim, &bmiDim, DIB_RGB_COLORS, &dimBits, nullptr, 0);
        HGDIOBJ hOldDim = SelectObject(hdcDim, hDim);
        HBRUSH hBr = CreateSolidBrush(RGB(0, 0, 0));
        RECT r{0, 0, 1, 1}; FillRect(hdcDim, &r, hBr); DeleteObject(hBr);
        BLENDFUNCTION bf{}; bf.BlendOp = AC_SRC_OVER; bf.SourceConstantAlpha = 80;
        AlphaBlend(hdcMem, 0, 0, barW, barH, hdcDim, 0, 0, 1, 1, bf);
        SelectObject(hdcDim, hOldDim); DeleteObject(hDim); DeleteDC(hdcDim);
    }

    // 3. Label — 2 lines, centered
    {
        HFONT hFont = CreateFontW(-kLabelPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");
        HGDIOBJ hOld = SelectObject(hdcMem, hFont);
        SetTextColor(hdcMem, RGB(255, 255, 255));
        SetBkMode(hdcMem, TRANSPARENT);
        RECT r{0, 0, barW / 5, barH};
        DrawTextW(hdcMem, L"AI tomonidan\nyaratilgan subtitrlar", -1, &r,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(hdcMem, hOld); DeleteObject(hFont);
    }

    // 4. Divider — full height, 4px wide
    {
        RECT div{barW / 5, 0, barW / 5 + 4, barH};
        HBRUSH hBr = CreateSolidBrush(RGB(140, 140, 140));
        FillRect(hdcMem, &div, hBr); DeleteObject(hBr);
    }

    // 5. Subtitle text — white, single line
    if (!text.empty()) {
        HFONT hFont = CreateFontW(-kFontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");
        HGDIOBJ hOld = SelectObject(hdcMem, hFont);
        SetTextColor(hdcMem, RGB(255, 255, 255));
        SetBkMode(hdcMem, TRANSPARENT);
        int tx = barW / 5 + kGap;
        RECT r{tx, 0, barW - kPad, barH};
        DrawTextW(hdcMem, text.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(hdcMem, hOld); DeleteObject(hFont);
    }
    GdiFlush();

    // 6. Blend onto UYVY (with color — BT.601 limited range)
    const uint8_t* pixels = (const uint8_t*)pBits;
    const int dibStride = barW * 4;
    for (int row = 0; row < barH; ++row) {
        const uint8_t* srcRow = pixels + row * dibStride;
        for (int col = 0; col < barW; col += 2) {
            const uint8_t* p0 = srcRow + col * 4;   // BGRA
            const uint8_t* p1 = p0 + 4;
            // BT.601 limited-range YCbCr from RGB
            auto toYCbCr = [](uint8_t R, uint8_t G, uint8_t B,
                               uint8_t& Y, uint8_t& Cb, uint8_t& Cr) {
                Y  = (uint8_t)(16  + ( 66*R + 129*G +  25*B + 128) / 256);
                Cb = (uint8_t)(128 + (-38*R -  74*G + 112*B + 128) / 256);
                Cr = (uint8_t)(128 + (112*R -  94*G -  18*B + 128) / 256);
            };
            uint8_t Y0, Cb0, Cr0, Y1, Cb1, Cr1;
            toYCbCr(p0[2], p0[1], p0[0], Y0, Cb0, Cr0);
            toYCbCr(p1[2], p1[1], p1[0], Y1, Cb1, Cr1);
            uint8_t CbM = (uint8_t)(((int)Cb0 + Cb1) / 2);
            uint8_t CrM = (uint8_t)(((int)Cr0 + Cr1) / 2);

            uint8_t* dst = buf + (barY + row) * stride + (col / 2) * 4;
            dst[0] = (uint8_t)((CbM * kBarAlpha + dst[0] * (255 - kBarAlpha)) / 255);
            dst[1] = (uint8_t)((Y0  * kBarAlpha + dst[1] * (255 - kBarAlpha)) / 255);
            dst[2] = (uint8_t)((CrM * kBarAlpha + dst[2] * (255 - kBarAlpha)) / 255);
            dst[3] = (uint8_t)((Y1  * kBarAlpha + dst[3] * (255 - kBarAlpha)) / 255);
        }
    }

    SelectObject(hdcMem, hOldBmp); DeleteObject(hBmp);
    DeleteDC(hdcMem); ReleaseDC(nullptr, hdcRef);
}

// ── Main ────────────────────────────────────────────────────────────────

int main() {
    std::signal(SIGINT, OnSignal);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    CNTV2Card device;
    if (!device.Open(0)) { std::cerr << "ERROR: Cannot open device 0\n"; return 1; }
    std::cout << "Device: " << device.GetDisplayName() << "\n";

    if (!device.AcquireStreamForApplication(kAppType, (int32_t)AJAProcess::GetPid())) {
        std::cerr << "ERROR: device busy\n"; device.Close(); return 1;
    }

    NTV2TaskMode savedTaskMode;
    device.GetEveryFrameServices(savedTaskMode);
    device.SetEveryFrameServices(NTV2_OEM_TASKS);
    if (device.features().CanDoMultiFormat()) device.SetMultiFormatMode(true);
    for (int i = 0; i < 8; i++) device.AutoCirculateStop(NTV2Channel(i));

    // Wait for signal
    const NTV2Channel     inCh  = NTV2_CHANNEL1;
    const NTV2Channel     outCh = NTV2_CHANNEL3;
    const NTV2InputSource inSrc = NTV2_INPUTSOURCE_SDI1;

    device.EnableChannel(inCh);
    if (device.features().HasBiDirectionalSDI()) device.SetSDITransmitEnable(inCh, false);
    device.EnableInputInterrupt(inCh);
    device.SubscribeInputVerticalEvent(inCh);

    NTV2VideoFormat fmt = NTV2_FORMAT_UNKNOWN;
    for (int i = 0; i < 50 && fmt == NTV2_FORMAT_UNKNOWN && !gQuit; i++) {
        fmt = device.GetInputVideoFormat(inSrc);
        if (fmt == NTV2_FORMAT_UNKNOWN) { std::cout << "\rWaiting for SDI In 1 signal..." << std::flush; AJATime::Sleep(200); }
    }
    std::cout << "\n";
    if (fmt == NTV2_FORMAT_UNKNOWN) {
        std::cerr << "ERROR: No signal on SDI In 1\n";
        device.ReleaseStreamForApplication(kAppType, (int32_t)AJAProcess::GetPid());
        device.SetEveryFrameServices(savedTaskMode); device.Close(); return 1;
    }
    std::cout << "Signal: " << NTV2VideoFormatToString(fmt) << "\n";

    const NTV2FrameBufferFormat kFBF = NTV2_FBF_8BIT_YCBCR;
    NTV2FormatDescriptor fd(fmt, kFBF);
    ULWord W = fd.GetRasterWidth(), H = fd.GetRasterHeight();
    ULWord fSize = fd.GetTotalBytes();
    int stride = (int)(W * 2);
    std::cout << "Frame: " << W << "x" << H << "  bytes=" << fSize << "\n";

    device.SetVideoFormat(fmt, false, false, inCh);
    device.SetFrameBufferFormat(inCh, kFBF);

    device.EnableChannel(outCh);
    device.SetVideoFormat(fmt, false, false, outCh);
    device.SetFrameBufferFormat(outCh, kFBF);
    device.SetVANCMode(NTV2_VANCMODE_OFF, outCh);
    device.EnableOutputInterrupt(outCh);
    device.SubscribeOutputVerticalEvent(outCh);
    if (device.features().HasBiDirectionalSDI()) {
        device.SetSDITransmitEnable(NTV2_CHANNEL1, false);
        device.SetSDITransmitEnable(outCh, true);
    }
    device.SetReference(NTV2_REFERENCE_INPUT1);
    device.Connect(::GetOutputDestInputXpt(NTV2_OUTPUTDESTINATION_SDI3),
                   ::GetFrameBufferOutputXptFromChannel(outCh, false));

    // Audio
    int _maxAudCh = (int)device.features().GetMaxAudioChannels();
    uint16_t audCh = (uint16_t)(_maxAudCh < 8 ? _maxAudCh : 8);
    NTV2AudioSystem inAudSys = NTV2_AUDIOSYSTEM_1;
    NTV2AudioSystem outAudSys = (device.features().GetNumAudioSystems() >= 2) ? NTV2_AUDIOSYSTEM_2 : NTV2_AUDIOSYSTEM_1;
    device.SetNumberAudioChannels(audCh, inAudSys);
    device.SetAudioRate(NTV2_AUDIO_48K, inAudSys);
    device.SetAudioBufferSize(NTV2_AUDIO_BUFFER_BIG, inAudSys);
    device.SetAudioSystemInputSource(inAudSys, NTV2_AUDIO_EMBEDDED, ::NTV2ChannelToEmbeddedAudioInput(inCh));
    device.SetNumberAudioChannels(audCh, outAudSys);
    device.SetAudioRate(NTV2_AUDIO_48K, outAudSys);
    device.SetAudioBufferSize(NTV2_AUDIO_BUFFER_BIG, outAudSys);
    device.SetSDIOutputAudioSystem(outCh, outAudSys);
    device.SetSDIOutputDS2AudioSystem(outCh, outAudSys);
    device.SetAudioLoopBack(NTV2_AUDIO_LOOPBACK_OFF, outAudSys);

    NTV2FrameRate fr = NTV2_FRAMERATE_UNKNOWN;
    device.GetFrameRate(fr, outCh);
    ULWord frN = 25, frD = 1; ::GetFramesPerSecond(fr, frN, frD);
    uint32_t audioBytesPerFrame = (uint32_t)((uint64_t)48000 * frD / frN) * audCh * sizeof(uint32_t);

    // AutoCirculate
    constexpr int RING = 150;
    std::vector<std::vector<ULWord>> vidBufs(RING), audBufs(RING);
    std::vector<ULWord> audBytesArr(RING, 0);
    for (int i = 0; i < RING; i++) {
        vidBufs[i].resize((fSize + 3) / 4, 0);
        audBufs[i].resize((kAudioSizeMax + 3) / 4, 0);
    }

    if (!device.AutoCirculateInitForInput(inCh, 5, inAudSys, AUTOCIRCULATE_WITH_RP188)) {
        std::cerr << "ERROR: AutoCirculateInitForInput failed\n";
        device.ReleaseStreamForApplication(kAppType, (int32_t)AJAProcess::GetPid());
        device.SetEveryFrameServices(savedTaskMode); device.Close(); return 1;
    }
    if (!device.AutoCirculateInitForOutput(outCh, 4, outAudSys, 0)) {
        std::cerr << "ERROR: AutoCirculateInitForOutput failed\n";
        device.ReleaseStreamForApplication(kAppType, (int32_t)AJAProcess::GetPid());
        device.SetEveryFrameServices(savedTaskMode); device.Close(); return 1;
    }
    device.AutoCirculateStart(inCh);

    // Audio TCP
    AudioServer audioServer;
    std::thread audioServerThread;
    if (audioServer.Init()) {
        std::cout << "[audio tcp] listening on 127.0.0.1:" << kAudioPort << " (16kHz mono s16le)\n";
        audioServerThread = std::thread([&]() {
            while (!gQuit) {
                if (!audioServer.WaitForClient()) break;
                std::cout << "[audio tcp] client connected\n" << std::flush;
                while (!gQuit && audioServer.connected) AJATime::Sleep(100);
                if (!gQuit) { audioServer.Disconnect(); std::cout << "[audio tcp] client disconnected, waiting...\n" << std::flush; }
            }
        });
    } else {
        std::cerr << "[audio tcp] WARNING: could not start TCP server on port " << kAudioPort << "\n";
    }

    std::cout << "ACTIVE: SDI In 1 -> [" << kSubtitleFile << "] -> SDI Out 3\nPress Ctrl+C to stop.\n\n";

    // Main loop
    std::atomic<int> ringHead{0}, ringTail{0};
    std::atomic<uint32_t> nIn{0};

    std::thread captureThread([&]() {
        AUTOCIRCULATE_TRANSFER xfer; AUTOCIRCULATE_STATUS st;
        while (!gQuit) {
            device.AutoCirculateGetStatus(inCh, st);
            if (!st.IsRunning() || !st.HasAvailableInputFrame()) { device.WaitForInputVerticalInterrupt(inCh); continue; }
            while (!gQuit) {
                int h = ringHead.load(std::memory_order_relaxed);
                int t = ringTail.load(std::memory_order_acquire);
                if (h - t < RING - 1) break;
                AJATime::Sleep(2);
            }
            if (gQuit) break;
            int ri = ringHead.load(std::memory_order_relaxed) % RING;
            xfer.SetVideoBuffer(vidBufs[ri].data(), fSize);
            xfer.SetAudioBuffer(audBufs[ri].data(), kAudioSizeMax);
            device.AutoCirculateTransfer(inCh, xfer);
            ULWord bytes = xfer.GetCapturedAudioByteCount();
            if (bytes == 0) bytes = audioBytesPerFrame;
            bytes = (bytes / (audCh * 4)) * (audCh * 4);
            if (bytes == 0) bytes = audioBytesPerFrame;
            audBytesArr[ri] = bytes;
            if (audioServer.connected && bytes > 0) {
                static std::vector<int16_t> pcmOut;
                size_t n = DownsampleSdiAudio((const uint8_t*)audBufs[ri].data(), bytes, audCh, pcmOut);
                if (n > 0) audioServer.Write(pcmOut.data(), n);
            }
            ringHead.fetch_add(1, std::memory_order_release);
            nIn.fetch_add(1, std::memory_order_relaxed);
        }
    });

    AUTOCIRCULATE_TRANSFER outXfer; AUTOCIRCULATE_STATUS outSt;
    uint32_t nOut = 0; bool outputStarted = false;

    while (!gQuit) {
        int h = ringHead.load(std::memory_order_acquire), t = ringTail.load(std::memory_order_relaxed);
        int depth = h - t;
        if (!outputStarted && depth < RING - 2) { AJATime::Sleep(2); continue; }
        if (depth == 0) { AJATime::Sleep(2); continue; }

        device.AutoCirculateGetStatus(outCh, outSt);
        if (!outSt.CanAcceptMoreOutputFrames()) { device.WaitForOutputVerticalInterrupt(outCh); continue; }

        int ri = t % RING;
        DrawOverlay((uint8_t*)vidBufs[ri].data(), stride, (int)W, (int)H,
                    Utf8ToWide(ReadSubtitleFile(kSubtitleFile)));
        { static bool sSaved = false;
          if (!sSaved) { sSaved = true;
            SaveFrameAsBMP("C:\\Users\\Java\\Desktop\\whisper-sub\\overlay_preview.bmp",
                           (const uint8_t*)vidBufs[ri].data(), (int)W, (int)H, stride);
            std::cout << "\n[preview] saved overlay_preview.bmp\n" << std::flush; } }
        outXfer.SetVideoBuffer(vidBufs[ri].data(), fSize);
        outXfer.SetAudioBuffer(audBufs[ri].data(), audBytesArr[ri]);
        if (device.AutoCirculateTransfer(outCh, outXfer)) nOut++;
        ringTail.store(t + 1, std::memory_order_release);

        if (!outputStarted && nOut >= 1) { device.AutoCirculateStart(outCh); outputStarted = true; }

        if (nOut % 25 == 0) {
            int avail = ringHead.load(std::memory_order_relaxed) - ringTail.load(std::memory_order_relaxed);
            std::cout << "\rIn=" << nIn.load() << "  Out=" << nOut << "  ring=" << avail << "  buf=" << outSt.acBufferLevel << "    " << std::flush;
        }
    }

    captureThread.join();
    audioServer.Disconnect(); audioServer.Close();
    if (audioServerThread.joinable()) audioServerThread.join();

    std::cout << "\nShutting down.\n";
    device.AutoCirculateStop(inCh); device.AutoCirculateStop(outCh);
    device.UnsubscribeInputVerticalEvent(inCh); device.UnsubscribeOutputVerticalEvent(outCh);
    device.ReleaseStreamForApplication(kAppType, (int32_t)AJAProcess::GetPid());
    device.SetEveryFrameServices(savedTaskMode); device.Close();
    return 0;
}
