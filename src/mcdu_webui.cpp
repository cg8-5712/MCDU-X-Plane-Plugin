// winsock2 必须在任何可能包含 windows.h 的头文件之前
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "mcdu_webui.h"
#include "bcdu_font.h"
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
#include <deque>
#include <unordered_map>
#include <string>
#include <sstream>
#include <cctype>
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

static std::mutex                                  gCommandMtx;
static std::deque<std::string>                     gPendingCommands;
static std::unordered_map<std::string, XPLMCommandRef> gCommandCache;

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
            // 特殊字符 → JSON Unicode 转义
            switch (ch) {
                case '`': js << "\\u00B0"; break;  // °
                case '|': js << "\\u25B5"; break;  // ▵
                case '~': js << "\\u25A1"; break;  // □
                case '!': js << "\\u2190"; break;  // ←
                case '@': js << "\\u2192"; break;  // →
                case '#': js << "\\u2191"; break;  // ↑
                case '$': js << "\\u2193"; break;  // ↓
                case '"': js << "\\\"";    break;
                case '\\': js << "\\\\";   break;
                default:
                    if (ch < 0x20) js << ' ';
                    else           js << ch;
                    break;
            }
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

static bool isAllowedCommandName(const std::string& commandName) {
    if (commandName.empty() || commandName.size() > 64) return false;

    const bool allowedPrefix =
        commandName.rfind("AirbusFBW/MCDU1", 0) == 0 ||
        commandName == "AirbusFBW/UndockMCDU1";
    if (!allowedPrefix) return false;

    for (unsigned char ch : commandName) {
        if (std::isalnum(ch) || ch == '/' || ch == '_') continue;
        return false;
    }

    return true;
}

static bool enqueueCommand(const std::string& commandName) {
    if (!isAllowedCommandName(commandName)) return false;

    std::lock_guard<std::mutex> lock(gCommandMtx);
    if (gPendingCommands.size() >= 128) return false;
    gPendingCommands.push_back(commandName);
    return true;
}

void MCDUWebUIPumpCommands() {
    std::deque<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(gCommandMtx);
        pending.swap(gPendingCommands);
    }

    for (const auto& commandName : pending) {
        XPLMCommandRef commandRef = nullptr;
        const auto it = gCommandCache.find(commandName);
        if (it != gCommandCache.end()) {
            commandRef = it->second;
        } else {
            commandRef = XPLMFindCommand(commandName.c_str());
            gCommandCache.emplace(commandName, commandRef);
        }

        if (commandRef) {
            XPLMCommandOnce(commandRef);
        } else {
            std::string msg = "MCDU WebUI: Command not found: " + commandName + "\n";
            XPLMDebugString(msg.c_str());
        }
    }
}

static const char* kHtmlPage = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>MCDU Display</title>
<style>
  :root {
    --cols: 24;
    --cell-w: 19px;
    --cell-h: 29px;
  }
  * { margin: 0; padding: 0; box-sizing: border-box; }
  @font-face {
    font-family: 'BCDU';
    src: url('/assets/BCDU.otf') format('opentype');
    font-display: swap;
  }
  body {
    min-height: 100vh;
    padding: 20px;
    display: grid;
    place-items: center;
    background: linear-gradient(180deg, #242623 0%, #171917 100%);
    color: #d6d8d1;
    font-family: 'BCDU', 'Consolas', 'Courier New', monospace;
  }
  #wrapper {
    display: grid;
    gap: 14px;
    justify-items: center;
    width: fit-content;
    max-width: 100%;
    padding: 18px 18px 16px;
    border-radius: 24px;
    background: linear-gradient(180deg, #474b45 0%, #222520 100%);
    border: 1px solid rgba(255,255,255,0.07);
    box-shadow:
      0 18px 40px rgba(0,0,0,0.42),
      inset 0 1px 0 rgba(255,255,255,0.10);
  }
  #display-shell {
    display: grid;
    grid-template-columns: 34px auto 34px;
    gap: 10px;
    align-items: stretch;
  }
  #center-stack {
    display: grid;
    gap: 10px;
  }
  #header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    width: 100%;
    margin-bottom: 14px;
    color: #8a9087;
    font-size: 11px;
    letter-spacing: 0.24em;
    text-transform: uppercase;
  }
  .lsk-stack {
    display: grid;
    grid-template-rows: repeat(6, 1fr);
    gap: 16px;
    padding-top: 52px;
    padding-bottom: 34px;
    align-items: center;
  }
  #controls {
    width: 100%;
    display: grid;
    gap: 10px;
  }
  #top-controls {
    display: grid;
    grid-template-columns: minmax(0, 1fr) 42px;
    gap: 8px;
    align-items: start;
  }
  #function-grid {
    display: grid;
    grid-template-columns: repeat(6, minmax(54px, 1fr));
    gap: 8px;
  }
  #brightness-grid {
    display: grid;
    gap: 8px;
  }
  #keyboard-grid {
    display: grid;
    grid-template-columns: 148px minmax(0, 1fr);
    gap: 10px;
    align-items: start;
  }
  #left-pad {
    display: grid;
    gap: 8px;
  }
  #airport-grid,
  #arrow-grid,
  #num-grid,
  #alpha-grid {
    display: grid;
    gap: 8px;
  }
  #airport-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
  #arrow-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
  #num-grid {
    grid-template-columns: repeat(3, minmax(0, 1fr));
  }
  #alpha-grid {
    grid-template-columns: repeat(5, minmax(0, 1fr));
  }
  .mcdu-key,
  .key-spacer {
    min-height: 42px;
  }
  .mcdu-key {
    width: 100%;
    border: 1px solid #596167;
    border-radius: 8px;
    background: linear-gradient(180deg, #242931 0%, #0b0d12 100%);
    box-shadow:
      0 2px 0 rgba(0,0,0,0.85),
      inset 0 1px 0 rgba(255,255,255,0.08);
    color: #ddb29a;
    font-family: 'Segoe UI', 'Trebuchet MS', sans-serif;
    font-size: 12px;
    font-weight: 700;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    cursor: pointer;
    user-select: none;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 2px;
    padding: 6px 4px;
    transition: transform 0.05s ease, box-shadow 0.05s ease;
  }
  .mcdu-key span {
    display: block;
    line-height: 1.05;
    text-align: center;
    pointer-events: none;
  }
  .mcdu-key:active,
  .mcdu-key.pressed {
    transform: translateY(1px);
    box-shadow:
      0 1px 0 rgba(0,0,0,0.9),
      inset 0 1px 0 rgba(255,255,255,0.04);
  }
  .mcdu-key.lsk {
    min-height: 46px;
    font-size: 18px;
    letter-spacing: 0;
    padding: 0;
  }
  .mcdu-key.nav {
    font-size: 17px;
    letter-spacing: 0;
  }
  .mcdu-key.utility {
    min-height: 48px;
  }
  .mcdu-key.bright {
    min-height: 46px;
    min-width: 40px;
    font-size: 11px;
    padding: 4px 2px;
  }
  .mcdu-key.alpha {
    min-width: 54px;
    min-height: 46px;
    font-size: 16px;
    letter-spacing: 0.02em;
  }
  .mcdu-key.airport {
    min-height: 48px;
  }
  .mcdu-key.numeric {
    min-height: 40px;
    min-width: 40px;
    aspect-ratio: 1 / 1;
    border-radius: 999px;
    font-size: 18px;
    letter-spacing: 0;
    justify-self: center;
    width: 40px;
    padding: 0;
  }
  .mcdu-key.blank {
    cursor: default;
    color: transparent;
  }
  #screen {
    width: calc(var(--cols) * var(--cell-w) + 26px);
    position: relative;
    overflow: hidden;
    background: linear-gradient(180deg, #10130f 0%, #050605 100%);
    border: 1px solid #2b2f29;
    border-radius: 10px;
    padding: 12px 12px 10px;
    box-shadow:
      inset 0 0 0 1px rgba(255,255,255,0.03),
      inset 0 0 18px rgba(0,0,0,0.62),
      0 8px 24px rgba(0,0,0,0.28);
  }
  #screen::after {
    content: '';
    position: absolute;
    inset: 0;
    pointer-events: none;
    background:
      linear-gradient(180deg, rgba(255,255,255,0.035), transparent 14%, transparent 86%, rgba(255,255,255,0.025)),
      repeating-linear-gradient(180deg, rgba(255,255,255,0.015) 0, rgba(255,255,255,0.015) 1px, transparent 1px, transparent 4px);
    mix-blend-mode: screen;
    opacity: 0.45;
  }
  .row {
    white-space: nowrap;
    height: var(--cell-h);
    line-height: var(--cell-h);
    font-size: 0;
  }
  .cell,
  .field-run {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    vertical-align: top;
    height: var(--cell-h);
  }
  .cell {
    width: var(--cell-w);
    font-family: 'BCDU', 'Consolas', 'Courier New', monospace;
    font-variant-ligatures: none;
  }
  .c0 { color: #d0d0d0; }
  .c1 { color: #74ff6a; }
  .c2 { color: #7dd6ff; }
  .c3 { color: #ffb24f; }
  .c4 { color: #fff36e; }
  .c5 { color: #ff7ff5; }
  .f0 { font-size: 21px; }
  .f1 { font-size: 15px; }
  .f2 { font-size: 25px; font-weight: bold; }
  .field-run {
    width: calc(var(--cell-w) * var(--cells, 1));
    height: calc(var(--cell-h) - 10px);
    background:
      linear-gradient(currentColor 0 0) top left / 100% 1px no-repeat,
      linear-gradient(currentColor 0 0) bottom left / 100% 1px no-repeat,
      linear-gradient(currentColor 0 0) top left / 1px 100% no-repeat,
      linear-gradient(currentColor 0 0) top right / 1px 100% no-repeat,
      repeating-linear-gradient(
        to right,
        transparent 0 calc(var(--cell-w) - 1px),
        currentColor calc(var(--cell-w) - 1px) var(--cell-w)
      );
  }
  #status {
    text-align: center;
    font-size: 11px;
    color: #8a9087;
    margin-top: 12px;
    letter-spacing: 0.12em;
    text-transform: uppercase;
  }
  #status.connected { color: #b7bcb4; }
  #status.error { color: #ff9362; }
  @media (max-width: 640px) {
    :root {
      --cell-w: 15px;
      --cell-h: 25px;
    }
    #wrapper {
      padding: 14px 14px 12px;
      border-radius: 18px;
    }
    #screen {
      width: calc(var(--cols) * var(--cell-w) + 22px);
      padding: 10px 10px 8px;
    }
    #display-shell {
      grid-template-columns: 28px auto 28px;
      gap: 6px;
    }
    .lsk-stack {
      gap: 12px;
      padding-top: 44px;
      padding-bottom: 30px;
    }
    #function-grid {
      grid-template-columns: repeat(3, minmax(0, 1fr));
    }
    #top-controls {
      grid-template-columns: 1fr;
    }
    #brightness-grid {
      grid-template-columns: repeat(2, minmax(0, 1fr));
    }
    #keyboard-grid {
      grid-template-columns: 1fr;
    }
    #airport-grid,
    #arrow-grid,
    #num-grid {
      grid-template-columns: repeat(2, minmax(0, 1fr));
    }
    #num-grid {
      grid-template-columns: repeat(3, minmax(0, 1fr));
    }
    .mcdu-key,
    .key-spacer {
      min-height: 38px;
    }
    .mcdu-key {
      font-size: 11px;
    }
    .mcdu-key.alpha {
      min-width: 0;
      min-height: 40px;
      font-size: 14px;
    }
    .mcdu-key.numeric {
      width: 36px;
      min-width: 36px;
      min-height: 36px;
      font-size: 16px;
    }
    .mcdu-key.lsk {
      min-height: 38px;
      font-size: 14px;
    }
    .f0 { font-size: 18px; }
    .f1 { font-size: 13px; }
    .f2 { font-size: 21px; }
  }
</style>
</head>
<body>
<div id="wrapper">
  <div id="display-shell">
    <div id="lsk-left" class="lsk-stack"></div>
    <div id="center-stack">
      <div id="header"><span>MCDU 1</span><span>WEB CONTROL</span></div>
      <div id="screen"></div>
      <div id="status">Connecting...</div>
    </div>
    <div id="lsk-right" class="lsk-stack"></div>
  </div>
  <div id="controls">
    <div id="top-controls">
      <div id="function-grid"></div>
      <div id="brightness-grid"></div>
    </div>
    <div id="keyboard-grid">
      <div id="left-pad">
        <div id="airport-grid"></div>
        <div id="arrow-grid"></div>
        <div id="num-grid"></div>
      </div>
      <div id="alpha-grid"></div>
    </div>
  </div>
</div>
<script>
const colorMap = {'0':'c0','1':'c1','2':'c2','3':'c3','4':'c4','5':'c5'};
const fontMap  = {'0':'f0','1':'f1','2':'f2'};
const screenEl = document.getElementById('screen');
const statusEl = document.getElementById('status');
const lskLeftEl = document.getElementById('lsk-left');
const lskRightEl = document.getElementById('lsk-right');
const functionGridEl = document.getElementById('function-grid');
const brightnessGridEl = document.getElementById('brightness-grid');
const airportGridEl = document.getElementById('airport-grid');
const arrowGridEl = document.getElementById('arrow-grid');
const numGridEl = document.getElementById('num-grid');
const alphaGridEl = document.getElementById('alpha-grid');

const functionButtons = [
  {label:'DIR', command:'AirbusFBW/MCDU1DirTo'},
  {label:'PROG', command:'AirbusFBW/MCDU1Prog'},
  {label:'PERF', command:'AirbusFBW/MCDU1Perf'},
  {label:'INIT', command:'AirbusFBW/MCDU1Init'},
  {label:'DATA', command:'AirbusFBW/MCDU1Data'},
  {label:'', className:'blank', disabled:true},
  {label:'F-PLN', command:'AirbusFBW/MCDU1Fpln'},
  {label:'RAD\\nNAV', command:'AirbusFBW/MCDU1RadNav'},
  {label:'FUEL\\nPRED', command:'AirbusFBW/MCDU1FuelPred'},
  {label:'SEC\\nF-PLN', command:'AirbusFBW/MCDU1SecFpln'},
  {label:'ATC\\nCOMM', command:'AirbusFBW/MCDU1ATC'},
  {label:'MCDU\\nMENU', command:'AirbusFBW/MCDU1Menu'},
];

const brightnessButtons = [
  {label:'BRT', command:'AirbusFBW/MCDU1KeyBright', className:'bright'},
  {label:'DIM', command:'AirbusFBW/MCDU1KeyDim', className:'bright'},
];

const airportButtons = [
  {label:'AIR\\nPORT', command:'AirbusFBW/MCDU1Airport', className:'airport'},
  {label:'', className:'blank', disabled:true},
];

const arrowButtons = [
  {label:'<-', command:'AirbusFBW/MCDU1SlewLeft', className:'nav'},
  {label:'^', command:'AirbusFBW/MCDU1SlewUp', className:'nav'},
  {label:'->', command:'AirbusFBW/MCDU1SlewRight', className:'nav'},
  {label:'v', command:'AirbusFBW/MCDU1SlewDown', className:'nav'},
];

const numButtons = [
  {label:'1', command:'AirbusFBW/MCDU1Key1', className:'numeric'},
  {label:'2', command:'AirbusFBW/MCDU1Key2', className:'numeric'},
  {label:'3', command:'AirbusFBW/MCDU1Key3', className:'numeric'},
  {label:'4', command:'AirbusFBW/MCDU1Key4', className:'numeric'},
  {label:'5', command:'AirbusFBW/MCDU1Key5', className:'numeric'},
  {label:'6', command:'AirbusFBW/MCDU1Key6', className:'numeric'},
  {label:'7', command:'AirbusFBW/MCDU1Key7', className:'numeric'},
  {label:'8', command:'AirbusFBW/MCDU1Key8', className:'numeric'},
  {label:'9', command:'AirbusFBW/MCDU1Key9', className:'numeric'},
  {label:'.', command:'AirbusFBW/MCDU1KeyDecimal', className:'numeric'},
  {label:'0', command:'AirbusFBW/MCDU1Key0', className:'numeric'},
  {label:'+/-', command:'AirbusFBW/MCDU1KeyPM', className:'numeric'},
];

const alphaButtons = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ'
  .split('')
  .map((letter) => ({label:letter, command:'AirbusFBW/MCDU1Key' + letter, className:'alpha'}))
  .concat([
    {label:'/', command:'AirbusFBW/MCDU1KeySlash', className:'utility'},
    {label:'SP', command:'AirbusFBW/MCDU1KeySpace', className:'utility'},
    {label:'OVFY', command:'AirbusFBW/MCDU1KeyOverfly', className:'utility'},
    {label:'CLR', command:'AirbusFBW/MCDU1KeyClear', className:'utility'},
  ]);

const leftLskButtons = Array.from(
  {length: 6},
  (_, index) => ({label:'>', command:'AirbusFBW/MCDU1LSK' + (index + 1) + 'L', className:'lsk'})
);
const rightLskButtons = Array.from(
  {length: 6},
  (_, index) => ({label:'<', command:'AirbusFBW/MCDU1LSK' + (index + 1) + 'R', className:'lsk'})
);

function renderButtonLabel(label) {
  return label.split('\\n').map((part) => '<span>' + escapeHtml(part) + '</span>').join('');
}

function buildButton(item) {
  if (!item) return '<div class="key-spacer"></div>';
  const classes = ['mcdu-key'];
  if (item.className) classes.push(item.className);
  const attrs = item.disabled
    ? ' disabled aria-hidden="true"'
    : ' data-command="' + item.command + '"';
  return '<button type="button" class="' + classes.join(' ') + '"' + attrs + '>' +
    renderButtonLabel(item.label || '') + '</button>';
}

function renderControls() {
  lskLeftEl.innerHTML = leftLskButtons.map(buildButton).join('');
  lskRightEl.innerHTML = rightLskButtons.map(buildButton).join('');
  functionGridEl.innerHTML = functionButtons.map(buildButton).join('');
  brightnessGridEl.innerHTML = brightnessButtons.map(buildButton).join('');
  airportGridEl.innerHTML = airportButtons.map(buildButton).join('');
  arrowGridEl.innerHTML = arrowButtons.map(buildButton).join('');
  numGridEl.innerHTML = numButtons.map(buildButton).join('');
  alphaGridEl.innerHTML = alphaButtons.map(buildButton).join('');
}

function pressVisual(button) {
  button.classList.add('pressed');
  setTimeout(() => button.classList.remove('pressed'), 120);
}

function sendCommand(commandName) {
  fetch('/api/command?name=' + encodeURIComponent(commandName), {method: 'POST'})
    .catch((err) => console.error('Command error', err));
}

function isAmberInputBox(line, index) {
  return line.text[index] === '\u25A1' && (colorMap[line.colors[index]] || 'c0') === 'c3';
}

function renderLine(line) {
  let html = '<div class="row">';
  for (let i = 0; i < line.text.length; ) {
    const cc = colorMap[line.colors[i]] || 'c0';

    if (isAmberInputBox(line, i)) {
      let end = i + 1;
      while (end < line.text.length && isAmberInputBox(line, end)) end++;
      html += '<span class="field-run ' + cc + '" style="--cells:' + (end - i) + '"></span>';
      i = end;
      continue;
    }

    const fc = fontMap[line.fonts[i]] || 'f0';
    const ch = line.text[i] === ' ' ? '&nbsp;' : escapeHtml(line.text[i]);
    html += '<span class="cell ' + cc + ' ' + fc + '">' + ch + '</span>';
    i++;
  }
  html += '</div>';
  return html;
}

function renderScreen(data) {
  let html = '';
  for (const line of data.lines) {
    html += renderLine(line);
  }
  screenEl.innerHTML = html;
}

function escapeHtml(c) {
  if (c === '<') return '&lt;';
  if (c === '>') return '&gt;';
  if (c === '&') return '&amp;';
  return c;
}

document.getElementById('wrapper').addEventListener('click', (event) => {
  const button = event.target.closest('button[data-command]');
  if (!button) return;

  pressVisual(button);
  sendCommand(button.dataset.command);
});

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
    statusEl.textContent = 'Disconnected, retrying...';
    statusEl.className = 'error';
    es.close();
    setTimeout(connectSSE, 2000);
  };
}

renderControls();
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

    gServer->Get("/assets/BCDU.otf", [](const httplib::Request&, httplib::Response& res) {
        if (kBcduFontSize == 0) {
            res.status = 404;
            res.set_content("Font asset not found.\n", "text/plain");
            return;
        }

        res.set_header("Cache-Control", "public, max-age=86400");
        res.set_content(reinterpret_cast<const char*>(kBcduFontData),
                        kBcduFontSize,
                        "font/otf");
    });

    gServer->Post("/api/command", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("name")) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"missing name\"}", "application/json");
            return;
        }

        const std::string commandName = req.get_param_value("name");
        if (!enqueueCommand(commandName)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"error\":\"invalid command\"}", "application/json");
            return;
        }

        res.set_content("{\"ok\":true}", "application/json");
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
    {
        std::lock_guard<std::mutex> lock(gCommandMtx);
        gPendingCommands.clear();
    }
    gCommandCache.clear();

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
    {
        std::lock_guard<std::mutex> lock(gCommandMtx);
        gPendingCommands.clear();
    }
    gCommandCache.clear();

    XPLMDebugString("MCDU: WebUI stopped.\n");
}
