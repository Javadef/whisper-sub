/* hls_to_sdi.exe [device_index]
 *
 * AJA KonaLHi: HLS stream -> SDI1 (1080i50, UYVY422, 8ch embedded audio 48kHz)
 * FFmpeg must be in PATH.
 *
 * Usage:  hls_to_sdi.exe [device_index]
 *         Then open http://localhost:8765 in a browser to control.
 *
 * Video: ffmpeg stdout -> anonymous pipe -> AJA
 * Audio: ffmpeg -> \\.\pipe\hls_sdi_audio (named pipe) -> AJA embedded audio
 */

// Winsock must come before windows.h (WIN32_LEAN_AND_MEAN/NOMINMAX set via CMake)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "ntv2card.h"
#include "ntv2devicescanner.h"
#include "ntv2utils.h"
#include "ntv2formatdescriptor.h"
#include "ntv2signalrouter.h"
#include "ntv2audiodefines.h"
#include "ajabase/system/process.h"

#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <algorithm>

// ---- App state ----
static volatile bool              gQuit       = false;
static volatile bool              gStreamStop = false;
static std::atomic<uint32_t>      gFrameCount { 0 };
static std::atomic<float>         gFps        { 0.0f };
static std::atomic<bool>          gAudioConn  { false };
static std::atomic<bool>          gRunning    { false };
static std::atomic<bool>          gMuted      { false };
static std::atomic<int>           gSeekOffset { 0 };    // seconds behind live (0 = live edge)

static std::mutex                 gUrlMtx;
static std::condition_variable    gUrlCv;
static std::string                gPendingUrl;
static bool                       gHasUrl     = false;

static std::mutex                 gStateMtx;   // protects gCurrentUrl
static std::string                gCurrentUrl;

static std::mutex                 gFfmpegMtx;
static HANDLE                     gFfmpegProc = INVALID_HANDLE_VALUE;

// ---- Caption burn-in (drawtext reload) ----
static std::mutex                 gCaptionMtx;
static std::string                gCaptionFile1;       // abs path, line 1
static std::string                gCaptionFile2;       // abs path, line 2
static std::string                gCaptionFilterPath1; // relative path in ffmpeg filter, line 1
static std::string                gCaptionFilterPath2; // relative path in ffmpeg filter, line 2

// ---- Audio ring ----
static std::mutex                 gAudioMtx;
static std::vector<uint8_t>       gAudioRing;
static size_t                     gAudioHead  = 0;
static const size_t               kRingMax    = 8 * 1024 * 1024;
static HANDLE                     gAudioPipe  = INVALID_HANDLE_VALUE;

static void OnSignal(int) { gQuit = true; gStreamStop = true; }

// ---- Audio thread (persistent, reconnects after each stream) ----
static void AudioThread()
{
    while (!gQuit) {
        gAudioConn = false;
        { std::lock_guard<std::mutex> lk(gAudioMtx); gAudioRing.clear(); gAudioHead = 0; }

        BOOL ok = ConnectNamedPipe(gAudioPipe, nullptr);
        if (!ok && GetLastError() != ERROR_PIPE_CONNECTED) {
            if (gQuit) break;
            Sleep(50);
            continue;
        }
        gAudioConn = true;

        std::vector<uint8_t> tmp(65536);
        DWORD nr = 0;
        while (!gQuit) {
            if (!ReadFile(gAudioPipe, tmp.data(), (DWORD)tmp.size(), &nr, nullptr) || nr == 0)
                break;
            std::lock_guard<std::mutex> lk(gAudioMtx);
            size_t avail = gAudioRing.size() - gAudioHead;
            if (avail + nr <= kRingMax) {
                if (gAudioHead > 4 * 1024 * 1024) {
                    gAudioRing.erase(gAudioRing.begin(), gAudioRing.begin() + gAudioHead);
                    gAudioHead = 0;
                }
                gAudioRing.insert(gAudioRing.end(), tmp.data(), tmp.data() + nr);
            }
        }
        gAudioConn = false;
        DisconnectNamedPipe(gAudioPipe);
    }
}

// ---- URL percent-decode ----
static std::string UrlDecode(const std::string& s)
{
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char h[3] = { s[i+1], s[i+2], 0 };
            r += (char)strtol(h, nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            r += ' ';
        } else {
            r += s[i];
        }
    }
    return r;
}

// ---- Embedded control page ----
static const char kHtml[] =
"<!DOCTYPE html>\n"
"<html><head>\n"
"<meta charset=\"utf-8\"><title>HLS-SDI</title>\n"
"<style>\n"
"*{box-sizing:border-box;margin:0;padding:0}\n"
"body{background:#111;color:#ddd;font-family:monospace;padding:28px;max-width:800px}\n"
"h2{color:#fff;margin-bottom:18px;font-size:1.2em;letter-spacing:.04em}\n"
".row{display:flex;gap:8px;margin-bottom:12px;align-items:center}\n"
"input{flex:1;padding:8px 10px;background:#1e1e1e;color:#eee;border:1px solid #3a3a3a;border-radius:4px;font-family:monospace;font-size:13px}\n"
"button{padding:9px 18px;border:none;border-radius:4px;font-size:13px;cursor:pointer;font-weight:bold;transition:background .15s}\n"
"#bStart{background:#1a7a3a;color:#fff}#bStart:hover{background:#1e9044}\n"
"#bStop{background:#7a1a1a;color:#fff}#bStop:hover{background:#9e2020}\n"
"#bMute{background:#2a2a2a;color:#ccc;min-width:42px}#bMute:hover{background:#3a3a3a}\n"
".player{margin-top:22px;background:#181818;border:1px solid #2a2a2a;border-radius:8px;padding:14px 18px}\n"
".pbar-wrap{position:relative;height:4px;background:#2a2a2a;border-radius:2px;margin:12px 0 10px;overflow:hidden}\n"
".pbar{height:4px;background:#2abb5e;border-radius:2px;width:0;transition:width .9s linear}\n"
".pbar.live{background:linear-gradient(90deg,#2abb5e 60%,#1e9044 100%);animation:pulse 2s infinite}\n"
"@keyframes pulse{0%,100%{opacity:1}50%{opacity:.6}}\n"
".ctrl{display:flex;align-items:center;gap:14px;font-size:13px}\n"
".seek-row{display:flex;gap:6px;margin-top:10px;align-items:center}\n"
".sb{padding:6px 12px;background:#222;color:#aaa;border:1px solid #333;border-radius:4px;font-size:12px;cursor:pointer;font-family:monospace}\n"
".sb:hover{background:#2e2e2e;color:#fff}\n"
".sb.live-btn{background:#1a3a1a;color:#2abb5e;border-color:#2abb5e}\n"
".sb.live-btn:hover{background:#1e5020}\n"
".behind{color:#aaa;font-size:12px;margin-left:8px}\n"
".badge{padding:2px 7px;border-radius:3px;font-size:10px;font-weight:bold;letter-spacing:.06em}\n"
".badge.live{background:#c00;color:#fff}\n"
".badge.stp{background:#333;color:#666}\n"
".time{color:#aaa;margin-left:auto}\n"
".run{color:#2abb5e;font-weight:bold}.stp2{color:#bb4444;font-weight:bold}\n"
".info{font-size:11px;color:#555;margin-top:10px;line-height:1.9}\n"
".info span{color:#888;margin-right:12px}\n"
"</style></head>\n"
"<body>\n"
"<h2>HLS &#x2192; SDI</h2>\n"
"<div class=\"row\">\n"
"  <input id=\"url\" type=\"text\" placeholder=\"HLS stream URL\"\n"
"    value=\"http://hls.mirtv.cdnvideo.ru/mirtv-parampublish/mir24_2500/playlist.m3u8\">\n"
"</div>\n"
"<div class=\"row\">\n"
"  <button id=\"bStart\" onclick=\"doStart()\">&#9654; START</button>\n"
"  <button id=\"bStop\"  onclick=\"doStop()\">&#9632; STOP</button>\n"
"  <button id=\"bMute\"  onclick=\"doMute()\">&#128266;</button>\n"
"</div>\n"
"<div class=\"player\">\n"
"  <div class=\"ctrl\">\n"
"    <span id=\"badge\" class=\"badge stp\">STOPPED</span>\n"
"    <span id=\"fps\" style=\"color:#555\">-- fps</span>\n"
"    <span id=\"audio\" style=\"color:#555\">&#128266; --</span>\n"
"    <span id=\"timer\" class=\"time\">0:00:00</span>\n"
"  </div>\n"
"  <div class=\"pbar-wrap\"><div id=\"pbar\" class=\"pbar\"></div></div>\n"
"  <div class=\"seek-row\">\n"
"    <button class=\"sb\" onclick=\"doSeek(-300)\">&lt;&lt; 5m</button>\n"
"    <button class=\"sb\" onclick=\"doSeek(-60)\">&lt;&lt; 1m</button>\n"
"    <button class=\"sb\" onclick=\"doSeek(-30)\">&lt; 30s</button>\n"
"    <button class=\"sb\" onclick=\"doSeek(-10)\">&lt; 10s</button>\n"
"    <button class=\"sb live-btn\" onclick=\"doLive()\">&#9679; LIVE</button>\n"
"    <button class=\"sb\" onclick=\"doSeek(10)\">10s &gt;</button>\n"
"    <button class=\"sb\" onclick=\"doSeek(30)\">30s &gt;</button>\n"
"    <span id=\"behind\" class=\"behind\"></span>\n"
"  </div>\n"
"  <div id=\"info\" class=\"info\"></div>\n"
"</div>\n"
"<script>\n"
"var isMuted=false;\n"
"function doStart(){\n"
"  var u=document.getElementById('url').value.trim();\n"
"  if(!u)return;\n"
"  fetch('/start',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},\n"
"    body:'url='+encodeURIComponent(u)});\n"
"}\n"
"function doStop(){fetch('/stop',{method:'POST'});}\n"
"function doMute(){\n"
"  isMuted=!isMuted;\n"
"  fetch(isMuted?'/mute':'/unmute',{method:'POST'});\n"
"  document.getElementById('bMute').textContent=isMuted?'\\ud83d\\udd07':'\\ud83d\\udd0a';\n"
"}\n"
"function doSeek(delta){fetch('/seek?delta='+delta,{method:'POST'});}\n"
"function doLive(){fetch('/seek?delta=0&live=1',{method:'POST'});}\n"
"function fmt(s){\n"
"  var h=Math.floor(s/3600),m=Math.floor((s%3600)/60),ss=s%60;\n"
"  return h+':'+(m<10?'0':'')+m+':'+(ss<10?'0':'')+ss;\n"
"}\n"
"function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}\n"
"var maxF=0;\n"
"function poll(){\n"
"  fetch('/status').then(r=>r.json()).then(d=>{\n"
"    var badge=document.getElementById('badge');\n"
"    var pbar =document.getElementById('pbar');\n"
"    var bh   =document.getElementById('behind');\n"
"    if(d.running){\n"
"      badge.textContent='LIVE';badge.className='badge live';\n"
"      if(d.frames>maxF)maxF=d.frames;\n"
"      var pct=(maxF>0?Math.min(100,(d.frames/Math.max(maxF,1))*100):0);\n"
"      pbar.style.width=pct+'%';pbar.className='pbar live';\n"
"    } else {\n"
"      badge.textContent='STOPPED';badge.className='badge stp';\n"
"      pbar.style.width='0';pbar.className='pbar';maxF=0;\n"
"    }\n"
"    if(d.seek_offset>0){\n"
"      var s=d.seek_offset,h=Math.floor(s/3600),m=Math.floor((s%3600)/60),ss=s%60;\n"
"      bh.textContent='\\u2190 '+h+':'+(m<10?'0':'')+m+':'+(ss<10?'0':'')+ss+' behind live';\n"
"      bh.style.color='#f0a040';\n"
"    } else { bh.textContent='\\u25cf live edge';bh.style.color='#2abb5e'; }\n"
"    var secs=d.fps>0?Math.round(d.frames/25):0;\n"
"    document.getElementById('timer').textContent=fmt(secs);\n"
"    document.getElementById('fps').textContent=d.running?d.fps.toFixed(1)+' fps':'-- fps';\n"
"    var am=document.getElementById('audio');\n"
"    if(d.muted){am.textContent='\\ud83d\\udd07 muted';am.style.color='#7a3a3a';}\n"
"    else if(d.audio){am.textContent='\\ud83d\\udd0a audio';am.style.color='#2abb5e';}\n"
"    else{am.textContent='\\ud83d\\udd0a --';am.style.color='#555';}\n"
"    var info='';\n"
"    if(d.url)info+='<span>'+esc(d.url)+'</span><br>';\n"
"    info+='<span>frames '+d.frames+'</span>';\n"
"    document.getElementById('info').innerHTML=info;\n"
"    isMuted=d.muted;\n"
"    document.getElementById('bMute').textContent=isMuted?'\\ud83d\\udd07':'\\ud83d\\udd0a';\n"
"  }).catch(function(){});\n"
"}\n"
"setInterval(poll,1000); poll();\n"
"</script>\n"
"</body></html>\n";

// ---- HTTP control server ----
static void HttpThread()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "ERROR: WSAStartup failed\n";
        return;
    }
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { WSACleanup(); return; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(8765);

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(srv, 8) != 0) {
        std::cerr << "ERROR: HTTP bind/listen failed on :8765\n";
        closesocket(srv); WSACleanup(); return;
    }
    std::cerr << "Control: http://localhost:8765\n";

    static char buf[16384];
    while (!gQuit) {
        fd_set fds; FD_ZERO(&fds); FD_SET(srv, &fds);
        timeval tv { 0, 500000 };
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;

        SOCKET cli = accept(srv, nullptr, nullptr);
        if (cli == INVALID_SOCKET) continue;

        DWORD rto = 2000;
        setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, (char*)&rto, sizeof(rto));

        int n = recv(cli, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            std::string req(buf, n);

            auto sendResp = [&](int code, const char* ct, const std::string& body) {
                std::ostringstream h;
                h << "HTTP/1.1 " << code << " OK\r\nContent-Type: " << ct
                  << "\r\nContent-Length: " << body.size()
                  << "\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
                std::string hdr = h.str();
                send(cli, hdr.c_str(), (int)hdr.size(), 0);
                if (!body.empty())
                    send(cli, body.c_str(), (int)body.size(), 0);
            };

            // Parse method + path
            size_t sp1 = req.find(' ');
            size_t sp2 = (sp1 != std::string::npos) ? req.find(' ', sp1 + 1) : std::string::npos;
            std::string method = (sp1 != std::string::npos) ? req.substr(0, sp1) : "";
            std::string path   = (sp1 != std::string::npos && sp2 != std::string::npos)
                                 ? req.substr(sp1 + 1, sp2 - sp1 - 1) : "/";
            size_t qm = path.find('?');
            if (qm != std::string::npos) path = path.substr(0, qm);

            if (method == "GET") {
                if (path == "/" || path == "/index.html") {
                    sendResp(200, "text/html; charset=utf-8", std::string(kHtml, sizeof(kHtml) - 1));
                } else if (path == "/status") {
                    std::string curUrl;
                    { std::lock_guard<std::mutex> lk(gStateMtx); curUrl = gCurrentUrl; }
                    // Escape double quotes for JSON
                    std::string safeUrl;
                    for (char c : curUrl) { if (c == '"') safeUrl += "\\\""; else safeUrl += c; }
                    std::ostringstream j;
                    j << "{\"running\":" << (gRunning.load() ? "true" : "false")
                      << ",\"frames\":"  << gFrameCount.load()
                      << ",\"fps\":"     << gFps.load()
                      << ",\"audio\":"   << (gAudioConn.load() ? "true" : "false")
                      << ",\"muted\":"       << (gMuted.load()  ? "true" : "false")
                      << ",\"seek_offset\":" << gSeekOffset.load()
                      << ",\"url\":\""       << safeUrl << "\"}";
                    sendResp(200, "application/json", j.str());
                } else {
                    sendResp(404, "text/plain", "Not Found");
                }
            } else if (method == "POST") {
                if (path == "/start") {
                    size_t bodyOff = req.find("\r\n\r\n");
                    std::string url;
                    if (bodyOff != std::string::npos) {
                        std::string body = req.substr(bodyOff + 4);
                        size_t p = body.find("url=");
                        if (p != std::string::npos) {
                            url = UrlDecode(body.substr(p + 4));
                            while (!url.empty() && (url.back() == '\r' || url.back() == '\n' || url.back() == ' '))
                                url.pop_back();
                        }
                    }
                    if (!url.empty()) {
                        gStreamStop = true;
                        {
                            std::lock_guard<std::mutex> lk(gFfmpegMtx);
                            if (gFfmpegProc != INVALID_HANDLE_VALUE)
                                TerminateProcess(gFfmpegProc, 0);
                        }
                        {
                            std::lock_guard<std::mutex> lk(gUrlMtx);
                            gPendingUrl = url;
                            gHasUrl     = true;
                        }
                        gUrlCv.notify_one();
                        sendResp(200, "application/json", "{\"ok\":true}");
                    } else {
                        sendResp(400, "application/json", "{\"ok\":false,\"error\":\"missing url\"}");
                    }
                } else if (path == "/stop") {
                    gStreamStop = true;
                    {
                        std::lock_guard<std::mutex> lk(gFfmpegMtx);
                        if (gFfmpegProc != INVALID_HANDLE_VALUE)
                            TerminateProcess(gFfmpegProc, 0);
                    }
                    sendResp(200, "application/json", "{\"ok\":true}");
                } else if (path == "/mute") {
                    gMuted = true;
                    gStreamStop = true;
                    {
                        std::lock_guard<std::mutex> lk(gFfmpegMtx);
                        if (gFfmpegProc != INVALID_HANDLE_VALUE)
                            TerminateProcess(gFfmpegProc, 0);
                    }
                    {
                        std::string reurl;
                        { std::lock_guard<std::mutex> lk(gStateMtx); reurl = gCurrentUrl; }
                        if (!reurl.empty()) {
                            std::lock_guard<std::mutex> lk(gUrlMtx);
                            gPendingUrl = reurl; gHasUrl = true;
                        }
                    }
                    gUrlCv.notify_one();
                    sendResp(200, "application/json", "{\"ok\":true}");
                } else if (path == "/unmute") {
                    gMuted = false;
                    gStreamStop = true;
                    {
                        std::lock_guard<std::mutex> lk(gFfmpegMtx);
                        if (gFfmpegProc != INVALID_HANDLE_VALUE)
                            TerminateProcess(gFfmpegProc, 0);
                    }
                    {
                        std::string reurl;
                        { std::lock_guard<std::mutex> lk(gStateMtx); reurl = gCurrentUrl; }
                        if (!reurl.empty()) {
                            std::lock_guard<std::mutex> lk(gUrlMtx);
                            gPendingUrl = reurl; gHasUrl = true;
                        }
                    }
                    gUrlCv.notify_one();
                    sendResp(200, "application/json", "{\"ok\":true}");
                } else if (path == "/seek") {
                    // Parse delta and optional live=1 from query string of the *original* path
                    // path was stripped of query; re-parse from raw request
                    std::string query;
                    size_t qp = req.find(' ');
                    if (qp != std::string::npos) {
                        size_t qp2 = req.find(' ', qp + 1);
                        std::string rawPath = (qp2 != std::string::npos)
                            ? req.substr(qp + 1, qp2 - qp - 1) : "";
                        size_t qq = rawPath.find('?');
                        if (qq != std::string::npos) query = rawPath.substr(qq + 1);
                    }
                    bool goLive = (query.find("live=1") != std::string::npos);
                    int delta = 0;
                    size_t dp = query.find("delta=");
                    if (dp != std::string::npos) delta = atoi(query.c_str() + dp + 6);
                    if (goLive) {
                        gSeekOffset = 0;
                    } else {
                        int newOff = gSeekOffset.load() - delta;  // delta negative = go back
                        if (newOff < 0) newOff = 0;
                        gSeekOffset = newOff;
                    }
                    // Restart stream at new offset
                    gStreamStop = true;
                    {
                        std::lock_guard<std::mutex> lk(gFfmpegMtx);
                        if (gFfmpegProc != INVALID_HANDLE_VALUE)
                            TerminateProcess(gFfmpegProc, 0);
                    }
                    {
                        std::string reurl;
                        { std::lock_guard<std::mutex> lk(gStateMtx); reurl = gCurrentUrl; }
                        if (!reurl.empty()) {
                            std::lock_guard<std::mutex> lk(gUrlMtx);
                            gPendingUrl = reurl; gHasUrl = true;
                        }
                    }
                    gUrlCv.notify_one();
                    sendResp(200, "application/json", "{\"ok\":true}");
                } else if (path == "/caption") {
                    size_t bodyOff = req.find("\r\n\r\n");
                    std::string text;
                    if (bodyOff != std::string::npos) {
                        std::string body = req.substr(bodyOff + 4);
                        size_t p = body.find("text=");
                        if (p != std::string::npos) {
                            text = UrlDecode(body.substr(p + 5));
                            while (!text.empty() && (text.back()=='\r'||text.back()=='\n')) text.pop_back();
                            // Strip all \r anywhere (Windows line endings render as glyph)
                            text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
                        }
                    }
                    // Split at first \n into line1 / line2 (each written to its own file)
                    // so ffmpeg drawtext never sees a newline character (which renders as a box glyph).
                    std::string line1, line2;
                    {
                        auto nl = text.find('\n');
                        if (nl == std::string::npos) {
                            line1 = text; line2 = "";
                        } else {
                            line1 = text.substr(0, nl);
                            line2 = text.substr(nl + 1);
                            line2.erase(std::remove(line2.begin(), line2.end(), '\n'), line2.end());
                        }
                    }
                    auto writeCapLine = [&](const std::string& content, const std::string& absPath) {
                        if (absPath.empty()) return;
                        std::string tmp = absPath + ".tmp";
                        FILE* cf = nullptr;
                        fopen_s(&cf, tmp.c_str(), "wb");
                        if (cf) {
                            fwrite(content.c_str(), 1, content.size(), cf);
                            fflush(cf); fclose(cf);
                            MoveFileExA(tmp.c_str(), absPath.c_str(),
                                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                        }
                    };
                    {
                        std::lock_guard<std::mutex> lk(gCaptionMtx);
                        writeCapLine(line1, gCaptionFile1);
                        writeCapLine(line2, gCaptionFile2);
                    }
                    sendResp(200, "application/json", "{\"ok\":true}");
                } else {
                    sendResp(404, "text/plain", "Not Found");
                }
            } else {
                sendResp(200, "text/plain", "OK");
            }
        }
        closesocket(cli);
    }
    closesocket(srv);
    WSACleanup();
}

// ---- Spawn ffmpeg as child process ----
static bool SpawnFfmpeg(const std::string& url, HANDLE* videoReadOut, HANDLE* procOut)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE pRead, pWrite;
    if (!CreatePipe(&pRead, &pWrite, &sa, 4 * 1024 * 1024)) return false;
    SetHandleInformation(pRead, HANDLE_FLAG_INHERIT, 0);

    char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp);
    char logPath[MAX_PATH]; sprintf_s(logPath, "%shls_sdi_ffmpeg.log", tmp);
    HANDLE hLog = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    HANDLE hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, OPEN_EXISTING, 0, nullptr);

    std::string audioOut = gMuted.load()
        ? " -map 0:a -af volume=0 -f s32le -ar 48000 -ac 8 \\\\.\\pipe\\hls_sdi_audio"
        : " -map 0:a -f s32le -ar 48000 -ac 8 \\\\.\\pipe\\hls_sdi_audio";
    int seekOff = gSeekOffset.load();
    // live_start_index -N = N segments from end; assume ~4s/segment
    std::string seekArgs = (seekOff > 0)
        ? " -live_start_index -" + std::to_string(std::max(1, seekOff / 4))
        : "";
    // Build vf filter — add drawtext burn-in if caption file exists
    std::string vfFilter = "scale=1920:1080";
    if (!gCaptionFilterPath1.empty()) {
        // Two single-line drawtext filters stacked vertically — avoids \n glyph issue on Windows.
        // Line 1 sits above line 2; each file contains only one plain text line.
        vfFilter +=
            // Line 1 (upper)
            ",drawtext=textfile=" + gCaptionFilterPath1 + ":reload=1"
            ":fontfile=arial.ttf:expansion=none"
            ":fontsize=48:fontcolor=white:borderw=2:bordercolor=black"
            ":box=1:boxcolor=black@0.6:boxborderw=14"
            ":x=(w-text_w)/2:y=h-text_h*2-44"
            // Line 2 (lower)
            ",drawtext=textfile=" + gCaptionFilterPath2 + ":reload=1"
            ":fontfile=arial.ttf:expansion=none"
            ":fontsize=48:fontcolor=white:borderw=2:bordercolor=black"
            ":box=1:boxcolor=black@0.6:boxborderw=14"
            ":x=(w-text_w)/2:y=h-text_h-30"
            // Watermark "AI" bottom-right
            ",drawtext=text='AI':fontfile=arial.ttf:expansion=none"
            ":fontsize=28:fontcolor=white@0.7:borderw=2:bordercolor=black@0.8"
            ":x=w-text_w-30:y=h-text_h-30";
    }
    std::string cmd =
        "ffmpeg -y -fflags +nobuffer+discardcorrupt -probesize 32768 -analyzeduration 0"
        " -reconnect 1 -reconnect_streamed 1 -reconnect_delay_max 5"
        + seekArgs +
        " -i \"" + url + "\""
        " -map 0:v -vf \"" + vfFilter + "\" -r 25 -f rawvideo -pix_fmt uyvy422 pipe:1"
        + audioOut;

    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = hNul;
    si.hStdOutput = pWrite;
    si.hStdError  = (hLog != INVALID_HANDLE_VALUE) ? hLog : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    CloseHandle(pWrite);
    if (hNul  != INVALID_HANDLE_VALUE) CloseHandle(hNul);
    if (hLog  != INVALID_HANDLE_VALUE) CloseHandle(hLog);

    if (!ok) { CloseHandle(pRead); return false; }
    CloseHandle(pi.hThread);

    *videoReadOut = pRead;
    *procOut      = pi.hProcess;
    return true;
}

// ---- Video + audio output loop for one stream session ----
static void RunStream(CNTV2Card& device,
                      NTV2Channel channel, NTV2AudioSystem audSys,
                      uint32_t frameBytes, uint32_t audioBytesPerFrame, uint16_t audCh,
                      std::vector<NTV2Buffer>& vidBufs,
                      std::vector<NTV2Buffer>& audBufs,
                      HANDLE videoReadPipe)
{
    for (int i = 0; i < 8; i++) device.AutoCirculateStop(NTV2Channel(i));
    device.WaitForOutputVerticalInterrupt(channel, 4);
    if (!device.AutoCirculateInitForOutput(channel, 7, audSys, 0)) {
        std::cerr << "ERROR: AutoCirculateInitForOutput failed\n";
        return;
    }

    AUTOCIRCULATE_TRANSFER xfer;
    AUTOCIRCULATE_STATUS   acStatus;
    const int RING = (int)vidBufs.size();
    uint32_t nFrames = 0, idx = 0;
    gFrameCount = 0; gFps = 0.0f; gRunning = true;

    auto fpsT    = std::chrono::steady_clock::now();
    uint32_t fpsBase = 0;

    while (!gQuit && !gStreamStop) {
        NTV2Buffer& vbuf = vidBufs[idx % RING];
        NTV2Buffer& abuf = audBufs[idx % RING];

        // Read one full video frame
        uint8_t* vptr = reinterpret_cast<uint8_t*>(vbuf.GetHostPointer());
        DWORD rem = frameBytes;
        while (rem > 0 && !gQuit && !gStreamStop) {
            DWORD nr = 0;
            if (!ReadFile(videoReadPipe, vptr, rem, &nr, nullptr) || nr == 0) {
                gStreamStop = true; break;
            }
            vptr += nr; rem -= nr;
        }
        if (gQuit || gStreamStop || rem > 0) break;

        // Grab audio: exactly one frame, silence-pad on underrun
        uint8_t* ap = reinterpret_cast<uint8_t*>(abuf.GetHostPointer());
        {
            std::lock_guard<std::mutex> lk(gAudioMtx);
            size_t avail = gAudioRing.size() - gAudioHead;
            size_t grab  = (std::min)(avail, (size_t)audioBytesPerFrame);
            grab = (grab / (audCh * sizeof(uint32_t))) * (audCh * sizeof(uint32_t));
            if (grab > 0) { memcpy(ap, gAudioRing.data() + gAudioHead, grab); gAudioHead += grab; }
            if (grab < audioBytesPerFrame) memset(ap + grab, 0, audioBytesPerFrame - grab);
        }

        // Wait for AJA output slot
        while (!gQuit && !gStreamStop) {
            device.AutoCirculateGetStatus(channel, acStatus);
            if (acStatus.CanAcceptMoreOutputFrames()) break;
            device.WaitForOutputVerticalInterrupt(channel);
        }
        if (gQuit || gStreamStop) break;

        xfer.SetVideoBuffer(vbuf, frameBytes);
        xfer.SetAudioBuffer(reinterpret_cast<ULWord*>(abuf.GetHostPointer()), audioBytesPerFrame);
        if (device.AutoCirculateTransfer(channel, xfer)) ++nFrames;
        if (nFrames == 3) device.AutoCirculateStart(channel);

        gFrameCount = nFrames;
        idx++;

        if (nFrames % 25 == 0) {
            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - fpsT).count();
            if (dt > 0) gFps = (float)(nFrames - fpsBase) / dt;
            fpsT = now; fpsBase = nFrames;
        }
    }

    device.AutoCirculateStop(channel);
    gRunning = false; gFps = 0.0f;
    std::cerr << "Stream stopped: " << nFrames << " frames\n";
}

// ---- main ----
int main(int argc, char* argv[])
{
    const std::string deviceSpec = (argc > 1) ? argv[1] : "0";
    signal(SIGINT,  OnSignal);
    signal(SIGTERM, OnSignal);

    // Create named audio pipe (app lifetime)
    gAudioPipe = CreateNamedPipeA(
        "\\\\.\\pipe\\hls_sdi_audio",
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 131072, 131072, 0, nullptr);
    if (gAudioPipe == INVALID_HANDLE_VALUE) {
        std::cerr << "ERROR: CreateNamedPipe failed (" << GetLastError()
                  << ") - another instance running?\n";
        return 1;
    }

    // Open AJA device
    CNTV2Card device;
    if (!CNTV2DeviceScanner::GetFirstDeviceFromArgument(deviceSpec, device) ||
        !device.IsDeviceReady(false)) {
        std::cerr << "ERROR: device not found/ready\n";
        return 1;
    }
    std::cerr << "Opened: " << device.GetDisplayName() << "\n";

    if (!device.AcquireStreamForApplication(AJA_FOURCC('H','L','S','O'), int32_t(AJAProcess::GetPid()))) {
        std::cerr << "ERROR: device busy\n";
        return 1;
    }
    device.SetEveryFrameServices(NTV2_OEM_TASKS);

    const NTV2VideoFormat       vidFmt  = NTV2_FORMAT_1080i_5000;
    const NTV2FrameBufferFormat pixFmt  = NTV2_FBF_8BIT_YCBCR;
    const NTV2Channel           channel = NTV2_CHANNEL1;
    const NTV2AudioSystem       audSys  = NTV2_AUDIOSYSTEM_1;
    const uint16_t              audCh   = 8;
    const uint32_t              samplesPerFrame    = 1920;           // 48000 / 25fps
    const uint32_t              audioBytesPerFrame = samplesPerFrame * audCh * sizeof(uint32_t);

    // Video hardware setup (once)
    NTV2ChannelSet active;
    device.GetEnabledChannels(active);
    device.DisableChannels(active);
    device.EnableChannel(channel);
    device.SetVideoFormat(vidFmt, false, false, channel);
    device.SetFrameBufferFormat(channel, pixFmt);
    device.SetVANCMode(NTV2_VANCMODE_OFF, channel);
    device.SubscribeOutputVerticalEvent(channel);
    device.SetReference(NTV2_REFERENCE_FREERUN);
    device.Connect(::GetOutputDestInputXpt(NTV2_OUTPUTDESTINATION_SDI1),
                   ::GetFrameBufferOutputXptFromChannel(channel, false));
    device.SetSDITransmitEnable(channel, true);

    // Audio hardware setup (once)
    device.SetNumberAudioChannels(audCh, audSys);
    device.SetAudioRate(NTV2_AUDIO_48K, audSys);
    device.SetAudioBufferSize(NTV2_AUDIO_BUFFER_BIG, audSys);
    device.SetSDIOutputAudioSystem(channel, audSys);
    device.SetSDIOutputDS2AudioSystem(channel, audSys);
    device.SetAudioLoopBack(NTV2_AUDIO_LOOPBACK_OFF, audSys);

    NTV2FormatDescriptor fmtDesc(vidFmt, pixFmt);
    const uint32_t frameBytes = fmtDesc.GetTotalBytes();
    std::cerr << "Format: 1080i50  video=" << frameBytes
              << "B/frame  audio=" << audioBytesPerFrame << "B/frame\n";

    // Allocate DMA buffers (4-frame ring)
    const int RING = 4;
    std::vector<NTV2Buffer> vidBufs(RING), audBufs(RING);
    for (int i = 0; i < RING; i++) {
        if (!vidBufs[i].Allocate(frameBytes, true) || !audBufs[i].Allocate(256 * 1024, true)) {
            std::cerr << "ERROR: buffer allocation failed\n";
            return 1;
        }
        memset(audBufs[i].GetHostPointer(), 0, 256 * 1024);
    }

    // Create caption temp file for drawtext burn-in
    // Use relative filename (no drive colon) so ffmpeg filter textfile= needs no escaping.
    // Both hls_to_sdi.exe and ffmpeg inherit the same CWD, so relative path resolves correctly.
    // Also copy arial.ttf to CWD so drawtext fontfile= can use a relative path (no drive colon).
    {
        char cwd[MAX_PATH]; GetCurrentDirectoryA(MAX_PATH, cwd);
        gCaptionFilterPath1 = "hls_sdi_cap1.txt";                           // relative — used in ffmpeg filter
        gCaptionFilterPath2 = "hls_sdi_cap2.txt";
        gCaptionFile1 = std::string(cwd) + "\\hls_sdi_cap1.txt";             // absolute — used for fopen writes
        gCaptionFile2 = std::string(cwd) + "\\hls_sdi_cap2.txt";
        FILE* cf = nullptr;
        fopen_s(&cf, gCaptionFile1.c_str(), "w"); if (cf) fclose(cf);
        fopen_s(&cf, gCaptionFile2.c_str(), "w"); if (cf) fclose(cf);
        std::cerr << "Caption files: " << gCaptionFile1 << ", " << gCaptionFile2 << "\n";

        std::string fontDst = std::string(cwd) + "\\arial.ttf";
        if (GetFileAttributesA(fontDst.c_str()) == INVALID_FILE_ATTRIBUTES) {
            CopyFileA("C:\\Windows\\Fonts\\arial.ttf", fontDst.c_str(), FALSE);
        }
        std::cerr << "Font file: " << fontDst << "\n";
    }

    // Start worker threads
    std::thread audioThr(AudioThread);
    std::thread httpThr(HttpThread);

    std::cerr << "Ready. Open http://localhost:8765 to control.\n";

    // Main loop: wait for URL -> stream -> repeat
    while (!gQuit) {
        std::string url;
        {
            std::unique_lock<std::mutex> lk(gUrlMtx);
            gUrlCv.wait(lk, [] { return gHasUrl || gQuit; });
            if (gQuit) break;
            url     = gPendingUrl;
            gHasUrl = false;
        }

        { std::lock_guard<std::mutex> lk(gStateMtx); gCurrentUrl = url; }
        gStreamStop = false;
        std::cerr << "Starting: " << url << "\n";

        HANDLE videoPipe = INVALID_HANDLE_VALUE, proc = INVALID_HANDLE_VALUE;
        if (!SpawnFfmpeg(url, &videoPipe, &proc)) {
            std::cerr << "ERROR: failed to spawn ffmpeg\n";
            { std::lock_guard<std::mutex> lk(gStateMtx); gCurrentUrl.clear(); }
            continue;
        }
        { std::lock_guard<std::mutex> lk(gFfmpegMtx); gFfmpegProc = proc; }

        RunStream(device, channel, audSys, frameBytes, audioBytesPerFrame, audCh,
                  vidBufs, audBufs, videoPipe);

        // If seek landed outside DVR window (0 frames), fall back to live
        if (gFrameCount.load() == 0 && gSeekOffset.load() > 0) {
            std::cerr << "Seek outside DVR window, returning to live\n";
            gSeekOffset = 0;
            std::lock_guard<std::mutex> lk(gUrlMtx);
            gPendingUrl = url; gHasUrl = true;
            gUrlCv.notify_one();
        }

        {
            std::lock_guard<std::mutex> lk(gFfmpegMtx);
            TerminateProcess(gFfmpegProc, 0);
            WaitForSingleObject(gFfmpegProc, 3000);
            CloseHandle(gFfmpegProc);
            gFfmpegProc = INVALID_HANDLE_VALUE;
        }
        CloseHandle(videoPipe);
        { std::lock_guard<std::mutex> lk(gStateMtx); gCurrentUrl.clear(); }
    }

    // Shutdown
    device.AutoCirculateStop(channel);
    device.ReleaseStreamForApplication(AJA_FOURCC('H','L','S','O'), int32_t(AJAProcess::GetPid()));
    {
        std::lock_guard<std::mutex> lk(gFfmpegMtx);
        if (gFfmpegProc != INVALID_HANDLE_VALUE) {
            TerminateProcess(gFfmpegProc, 0);
            CloseHandle(gFfmpegProc);
        }
    }
    CloseHandle(gAudioPipe);
    if (audioThr.joinable()) audioThr.join();
    if (httpThr.joinable())  httpThr.join();
    return 0;
}
