// winsock2 必须在任何可能包含 windows.h 的头文件之前
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "mcdu_webui.h"
#include "mcdu_data.h"
#include "XPLMUtilities.h"
#include "httplib.h"

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define INVALID_SOCKET -1
#define SOCKET_ERROR   -1
typedef int SOCKET;
#endif

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <string>
#include <sstream>
#include <cstring>

// ── UDP 包常量（与 mcdu_udp.cpp 一致）────────────────────────
static constexpr int PACKET_SIZE = 1018;
static constexpr int CELL_DATA_OFFSET = 8;

// ── 全局状态 ────────────────────────────────────────────────
static httplib::Server*   gServer = nullptr;
static std::thread*       gHttpThread = nullptr;
static std::thread*       gUdpThread = nullptr;
static std::atomic<bool>  gRunning{false};
static SOCKET             gUdpRecvSock = INVALID_SOCKET;

// UDP 接收到的屏幕数据
static std::mutex              gMtx;
static std::condition_variable gCV;
static MCDUScreen              gScreen{};
static uint32_t                gFrameSeq = 0;

// ── UDP 包解析 ──────────────────────────────────────────────

static bool parsePacket(const uint8_t* pkt, int len, MCDUScreen& out) {
    if (len != PACKET_SIZE || pkt[0] != 'M' || pkt[1] != 'C') return false;
    out.frameCounter = pkt[4] | (pkt[5]<<8) | (pkt[6]<<16) | (pkt[7]<<24);
    int off = CELL_DATA_OFFSET;
    for (int r = 0; r < MCDU_ROWS; ++r) {
        for (int c = 0; c < MCDU_COLS; ++c) {
            out.cells[r][c].ch    = (char)pkt[off++];
            out.cells[r][c].color = (MCDUColor)pkt[off++];
            out.cells[r][c].font  = (MCDUFont)pkt[off++];
        }
    }
    return true;
}

// ── UDP 监听线程 ────────────────────────────────────────────

static void udpListenerThread(uint16_t port) {
    gUdpRecvSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (gUdpRecvSock == INVALID_SOCKET) {
        XPLMDebugString("MCDU WebUI: Failed to create UDP recv socket.\n");
        return;
    }

    int reuse = 1;
    setsockopt(gUdpRecvSock, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&reuse, sizeof(reuse));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(gUdpRecvSock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "MCDU WebUI: Failed to bind UDP port %u.\n", port);
        XPLMDebugString(msg);
#ifdef _WIN32
        closesocket(gUdpRecvSock);
#else
        close(gUdpRecvSock);
#endif
        gUdpRecvSock = INVALID_SOCKET;
        return;
    }

    // 接收超时 200ms，便于检查 gRunning
#ifdef _WIN32
    DWORD timeout = 200;
    setsockopt(gUdpRecvSock, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&timeout, sizeof(timeout));
#else
    struct timeval tv = {0, 200000};
    setsockopt(gUdpRecvSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    char logmsg[128];
    snprintf(logmsg, sizeof(logmsg),
             "MCDU WebUI: UDP listener started on port %u.\n", port);
    XPLMDebugString(logmsg);

    uint8_t packet[PACKET_SIZE];
    while (gRunning.load()) {
        int n = recvfrom(gUdpRecvSock, (char*)packet, sizeof(packet),
                         0, nullptr, nullptr);
        if (n == PACKET_SIZE) {
            MCDUScreen scr;
            if (parsePacket(packet, n, scr)) {
                {
                    std::lock_guard<std::mutex> lock(gMtx);
                    gScreen = scr;
                    gFrameSeq++;
                }
                gCV.notify_all();
            }
        }
    }

#ifdef _WIN32
    closesocket(gUdpRecvSock);
#else
    close(gUdpRecvSock);
#endif
    gUdpRecvSock = INVALID_SOCKET;
    XPLMDebugString("MCDU WebUI: UDP listener stopped.\n");
}

// ── JSON 序列化 ─────────────────────────────────────────────

static std::string screenToJson(const MCDUScreen& scr) {
    std::ostringstream js;
    js << "{\"frame\":" << scr.frameCounter << ",\"lines\":[";
    for (int r = 0; r < MCDU_ROWS; ++r) {
        if (r > 0) js << ",";
        js << "{\"text\":\"";
        for (int c = 0; c < MCDU_COLS; ++c) {
            char ch = scr.cells[r][c].ch;
            if (ch == '"')       js << "\\\"";
            else if (ch == '\\') js << "\\\\";
            else if (ch < 0x20)  js << ' ';
            else                 js << ch;
        }
        js << "\",\"colors\":\"";
        for (int c = 0; c < MCDU_COLS; ++c)
            js << static_cast<int>(scr.cells[r][c].color);
        js << "\",\"fonts\":\"";
        for (int c = 0; c < MCDU_COLS; ++c)
            js << static_cast<int>(scr.cells[r][c].font);
        js << "\"}";
    }
    js << "]}";
    return js.str();
}

// ── 内嵌 HTML 前端 ──────────────────────────────────────────

static const char* kHtmlPage = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>MCDU Display</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body {
    background: #1a1a1a; color: #ccc;
    font-family: 'Consolas', 'Courier New', monospace;
    display: flex; justify-content: center; align-items: center;
    min-height: 100vh;
  }
  #wrapper {
    background: #0a0a0a; border: 3px solid #333; border-radius: 12px;
    padding: 24px 28px; box-shadow: 0 0 40px rgba(0,0,0,0.8);
  }
  h1 {
    text-align: center; font-size: 14px; color: #666;
    margin-bottom: 12px; letter-spacing: 2px;
  }
  #screen {
    background: #050505; border: 2px solid #222; border-radius: 4px;
    padding: 8px 12px;
  }
  .row {
    white-space: nowrap; height: 26px;
    line-height: 26px; font-size: 0;
  }
  .row span {
    display: inline-block; width: 16px; height: 26px;
    line-height: 26px; text-align: center;
    font-family: 'Consolas', 'Courier New', monospace;
    vertical-align: top;
    transform: scaleX(1.15);
  }
  .c0 { color: #d0d0d0; }
  .c1 { color: #00ff00; }
  .c2 { color: #00d4ff; }
  .c3 { color: #ff8800; }
  .c4 { color: #ffff00; }
  .c5 { color: #ff44ff; }
  .f0 { font-size: 18px; }
  .f1 { font-size: 10px; }
  .f2 { font-size: 22px; font-weight: bold; }
  #status {
    text-align: center; font-size: 11px; color: #444;
    margin-top: 10px;
  }
  #status.connected { color: #0a0; }
  #status.error { color: #a00; }
</style>
</head>
<body>
<div id="wrapper">
  <h1>MCDU 1</h1>
  <div id="screen"></div>
  <div id="status">Connecting...</div>
</div>
<script>
const colorMap = {'0':'c0','1':'c1','2':'c2','3':'c3','4':'c4','5':'c5'};
const fontMap  = {'0':'f0','1':'f1','2':'f2'};
const screenEl = document.getElementById('screen');
const statusEl = document.getElementById('status');

function renderScreen(data) {
  let html = '';
  for (const line of data.lines) {
    html += '<div class="row">';
    for (let i = 0; i < line.text.length; i++) {
      const cc = colorMap[line.colors[i]] || 'c0';
      const fc = fontMap[line.fonts[i]] || 'f0';
      const ch = line.text[i] === ' ' ? '&nbsp;' : escapeHtml(line.text[i]);
      html += '<span class="' + cc + ' ' + fc + '">' + ch + '</span>';
    }
    html += '</div>';
  }
  screenEl.innerHTML = html;
}

function escapeHtml(c) {
  if (c === '<') return '&lt;';
  if (c === '>') return '&gt;';
  if (c === '&') return '&amp;';
  return c;
}

function connectSSE() {
  const es = new EventSource('/api/events');
  es.onopen = () => {
    statusEl.textContent = 'Connected (SSE)';
    statusEl.className = 'connected';
  };
  es.onmessage = (e) => {
    try { renderScreen(JSON.parse(e.data)); }
    catch(err) { console.error('Parse error', err); }
  };
  es.onerror = () => {
    statusEl.textContent = 'Disconnected — retrying...';
    statusEl.className = 'error';
    es.close();
    setTimeout(connectSSE, 2000);
  };
}

fetch('/api/screen').then(r => r.json()).then(renderScreen).catch(() => {});
connectSSE();
</script>
</body>
</html>
)HTML";

// ── HTTP 服务器线程 ─────────────────────────────────────────

static void httpServerThread(int port) {
    gServer = new httplib::Server();

    gServer->Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kHtmlPage, "text/html");
    });

    gServer->Get("/api/screen", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(gMtx);
        res.set_content(screenToJson(gScreen), "application/json");
    });

    gServer->Get("/api/events", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Access-Control-Allow-Origin", "*");

        res.set_chunked_content_provider(
            "text/event-stream",
            [](size_t, httplib::DataSink& sink) -> bool {
                uint32_t lastSeq = 0;
                while (gRunning.load()) {
                    std::unique_lock<std::mutex> lock(gMtx);
                    gCV.wait_for(lock, std::chrono::milliseconds(500),
                        [&]{ return gFrameSeq != lastSeq || !gRunning.load(); });
                    if (!gRunning.load()) return false;
                    if (gFrameSeq == lastSeq) continue;

                    MCDUScreen scr = gScreen;
                    lastSeq = gFrameSeq;
                    lock.unlock();

                    std::string json = screenToJson(scr);
                    std::string msg = "data: " + json + "\n\n";
                    if (!sink.write(msg.c_str(), msg.size()))
                        return false;
                }
                return false;
            },
            [](bool) {}
        );
    });

    char msg[128];
    snprintf(msg, sizeof(msg),
             "MCDU: WebUI listening on http://localhost:%d\n", port);
    XPLMDebugString(msg);

    gServer->listen("0.0.0.0", port);
    XPLMDebugString("MCDU: WebUI server thread exited.\n");
}

// ── 公开接口 ────────────────────────────────────────────────

void MCDUWebUIStart(int httpPort, uint16_t udpPort) {
    if (gRunning.load()) return;

    gRunning.store(true);
    gFrameSeq = 0;

    gUdpThread  = new std::thread(udpListenerThread, udpPort);
    gHttpThread = new std::thread(httpServerThread, httpPort);

    XPLMDebugString("MCDU: WebUI started.\n");
}

void MCDUWebUIStop() {
    if (!gRunning.load()) return;

    gRunning.store(false);
    gCV.notify_all();  // 唤醒等待中的 SSE 线程

    if (gServer) gServer->stop();

    if (gHttpThread && gHttpThread->joinable()) gHttpThread->join();
    if (gUdpThread && gUdpThread->joinable())   gUdpThread->join();

    delete gHttpThread; gHttpThread = nullptr;
    delete gUdpThread;  gUdpThread  = nullptr;
    delete gServer;     gServer     = nullptr;

    XPLMDebugString("MCDU: WebUI stopped.\n");
}
