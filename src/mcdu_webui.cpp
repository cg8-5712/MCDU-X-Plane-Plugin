#include "mcdu_webui.h"
#include "XPLMUtilities.h"
#include "httplib.h"

#include <thread>
#include <atomic>
#include <string>
#include <sstream>

// ── 全局状态 ────────────────────────────────────────────────

static httplib::Server*   gServer = nullptr;
static std::thread*       gThread = nullptr;
static std::atomic<bool>  gRunning{false};
static MCDUScreenBuffer*  gBuffer = nullptr;

// ── JSON 序列化 ─────────────────────────────────────────────

static const char* colorName(MCDUColor c) {
    switch (c) {
        case MCDUColor::WHITE:   return "white";
        case MCDUColor::GREEN:   return "green";
        case MCDUColor::BLUE:    return "cyan";
        case MCDUColor::AMBER:   return "amber";
        case MCDUColor::YELLOW:  return "yellow";
        case MCDUColor::MAGENTA: return "magenta";
    }
    return "white";
}

static std::string screenToJson(const MCDUScreen& scr) {
    std::ostringstream js;
    js << "{\"frame\":" << scr.frameCounter << ",\"lines\":[";

    for (int r = 0; r < MCDU_ROWS; ++r) {
        if (r > 0) js << ",";
        js << "{\"text\":\"";

        // text — JSON-escape 特殊字符
        for (int c = 0; c < MCDU_COLS; ++c) {
            char ch = scr.cells[r][c].ch;
            if (ch == '"')       js << "\\\"";
            else if (ch == '\\') js << "\\\\";
            else if (ch < 0x20)  js << ' ';
            else                 js << ch;
        }

        js << "\",\"colors\":\"";
        for (int c = 0; c < MCDU_COLS; ++c) {
            js << static_cast<int>(scr.cells[r][c].color);
        }

        js << "\",\"fonts\":\"";
        for (int c = 0; c < MCDU_COLS; ++c) {
            js << static_cast<int>(scr.cells[r][c].font);
        }

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
    padding: 12px 16px; line-height: 1.55;
  }
  .row { white-space: pre; font-size: 17px; height: 1.55em; }
  .row span { font-family: 'Consolas', 'Courier New', monospace; }
  /* 颜色类 */
  .c0 { color: #e0e0e0; } /* white */
  .c1 { color: #00ff00; } /* green */
  .c2 { color: #00ccff; } /* cyan/blue */
  .c3 { color: #ff8800; } /* amber */
  .c4 { color: #ffff00; } /* yellow */
  .c5 { color: #ff44ff; } /* magenta */
  /* 字体类 */
  .f0 { font-size: 17px; } /* normal */
  .f1 { font-size: 13px; } /* small */
  .f2 { font-size: 20px; font-weight: bold; } /* large */
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

// 初始快照
fetch('/api/screen').then(r => r.json()).then(renderScreen).catch(() => {});
connectSSE();
</script>
</body>
</html>
)HTML";

// ── HTTP 服务器线程 ─────────────────────────────────────────

static void serverThread(int port) {
    gServer = new httplib::Server();

    // GET / — 主页
    gServer->Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kHtmlPage, "text/html");
    });

    // GET /api/screen — JSON 快照
    gServer->Get("/api/screen", [](const httplib::Request&, httplib::Response& res) {
        if (!gBuffer) {
            res.status = 503;
            return;
        }
        MCDUScreen scr = gBuffer->read();
        res.set_content(screenToJson(scr), "application/json");
    });

    // GET /api/events — SSE 推送
    gServer->Get("/api/events", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Access-Control-Allow-Origin", "*");

        res.set_chunked_content_provider(
            "text/event-stream",
            [](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                uint32_t lastFrame = 0;
                while (gRunning.load()) {
                    if (!gBuffer) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    }

                    uint32_t curFrame = gBuffer->frameCounter();
                    if (curFrame != lastFrame) {
                        MCDUScreen scr = gBuffer->read();
                        std::string json = screenToJson(scr);
                        std::string msg = "data: " + json + "\n\n";
                        if (!sink.write(msg.c_str(), msg.size())) {
                            return false;  // 客户端断开
                        }
                        lastFrame = curFrame;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                return false;  // 服务器关闭
            },
            [](bool /*success*/) {}
        );
    });

    char msg[128];
    snprintf(msg, sizeof(msg), "MCDU: WebUI listening on http://localhost:%d\n", port);
    XPLMDebugString(msg);

    gServer->listen("0.0.0.0", port);

    // listen() 返回意味着服务器已停止
    XPLMDebugString("MCDU: WebUI server thread exited.\n");
}

// ── 公开接口 ────────────────────────────────────────────────

void MCDUWebUIStart(MCDUScreenBuffer* buffer, int port) {
    if (gRunning.load()) return;

    gBuffer = buffer;
    gRunning.store(true);
    gThread = new std::thread(serverThread, port);

    XPLMDebugString("MCDU: WebUI thread started.\n");
}

void MCDUWebUIStop() {
    if (!gRunning.load()) return;

    gRunning.store(false);

    if (gServer) {
        gServer->stop();
    }

    if (gThread && gThread->joinable()) {
        gThread->join();
    }

    delete gThread;
    gThread = nullptr;
    delete gServer;
    gServer = nullptr;
    gBuffer = nullptr;

    XPLMDebugString("MCDU: WebUI stopped.\n");
}
