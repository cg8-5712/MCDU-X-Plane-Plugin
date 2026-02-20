#include "mcdu_menu.h"
#include "mcdu_config.h"
#include "mcdu_udp.h"
#include "mcdu_webui.h"

#include "XPLMMenus.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMUtilities.h"
#include "XPLMDefs.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

// ── 菜单项索引 ──────────────────────────────────────────────
enum MenuItemIdx {
    kMenuUdpTarget = 0,
    kMenuSep1,
    kMenuWebuiToggle,
    kMenuWebuiPort,
};

static XPLMMenuID gMenuId = nullptr;
static int        gMenuContainerIdx = -1;

// ── 输入窗口状态 ────────────────────────────────────────────
enum InputTarget { kInputNone, kInputUdpTarget, kInputWebuiPort };

static XPLMWindowID gInputWin   = nullptr;
static InputTarget  gInputTarget = kInputNone;
static char         gInputBuf[128];
static int          gInputLen = 0;
static const char*  gInputTitle = "";

// ── 前向声明 ────────────────────────────────────────────────
static void RefreshMenuLabels();
static void OpenInputWindow(InputTarget target, const char* title, const char* initVal);
static void CloseInputWindow();
static void ApplyInput();

// 外部：main.cpp 提供的 buffer 指针（用于 WebUI restart）
extern MCDUScreenBuffer gScreenBuffer;

// ── 菜单标签刷新 ────────────────────────────────────────────

static void RefreshMenuLabels() {
    MCDUConfig& cfg = MCDUConfigGet();
    char label[128];

    snprintf(label, sizeof(label), "UDP Target: %s:%u", cfg.udpTargetIP, cfg.udpPort);
    XPLMSetMenuItemName(gMenuId, kMenuUdpTarget, label, 0);

    snprintf(label, sizeof(label), "WebUI: %s", cfg.webuiEnabled ? "ON" : "OFF");
    XPLMSetMenuItemName(gMenuId, kMenuWebuiToggle, label, 0);

    snprintf(label, sizeof(label), "WebUI Port: %d", cfg.webuiPort);
    XPLMSetMenuItemName(gMenuId, kMenuWebuiPort, label, 0);
}

// ── 菜单回调 ────────────────────────────────────────────────

static void MenuHandler(void* /*menuRef*/, void* itemRef) {
    intptr_t idx = (intptr_t)itemRef;
    MCDUConfig& cfg = MCDUConfigGet();

    switch (idx) {
    case kMenuUdpTarget: {
        char initVal[80];
        snprintf(initVal, sizeof(initVal), "%s:%u", cfg.udpTargetIP, cfg.udpPort);
        OpenInputWindow(kInputUdpTarget, "UDP Target (IP:Port)", initVal);
        break;
    }
    case kMenuWebuiToggle:
        cfg.webuiEnabled = !cfg.webuiEnabled;
        if (cfg.webuiEnabled) {
            MCDUWebUIStart(&gScreenBuffer, cfg.webuiPort);
            XPLMDebugString("MCDU: WebUI enabled via menu.\n");
        } else {
            MCDUWebUIStop();
            XPLMDebugString("MCDU: WebUI disabled via menu.\n");
        }
        RefreshMenuLabels();
        break;
    case kMenuWebuiPort: {
        char initVal[16];
        snprintf(initVal, sizeof(initVal), "%d", cfg.webuiPort);
        OpenInputWindow(kInputWebuiPort, "WebUI Port", initVal);
        break;
    }
    }
}

// ── 输入窗口回调 ────────────────────────────────────────────

static void InputDrawCB(XPLMWindowID wnd, void* /*refcon*/) {
    int l, t, r, b;
    XPLMGetWindowGeometry(wnd, &l, &t, &r, &b);

    float white[] = {1.0f, 1.0f, 1.0f};
    float gray[]  = {0.6f, 0.6f, 0.6f};

    // 标题
    XPLMDrawString(white, l + 10, t - 25, (char*)gInputTitle, nullptr, xplmFont_Proportional);

    // 输入框内容
    char display[140];
    snprintf(display, sizeof(display), "> %s_", gInputBuf);
    XPLMDrawString(white, l + 10, t - 50, display, nullptr, xplmFont_Basic);

    // 提示
    XPLMDrawString(gray, l + 10, b + 10, (char*)"Enter=OK  Esc=Cancel", nullptr, xplmFont_Basic);
}

static void InputKeyCB(XPLMWindowID /*wnd*/, char inKey, XPLMKeyFlags inFlags,
                        char inVirtualKey, void* /*refcon*/, int losingFocus)
{
    if (losingFocus) {
        CloseInputWindow();
        return;
    }

    // 只处理 key-down
    if (!(inFlags & xplm_DownFlag)) return;

    unsigned char vk = (unsigned char)inVirtualKey;

    if (vk == XPLM_VK_ESCAPE) {
        CloseInputWindow();
        return;
    }

    if (vk == XPLM_VK_RETURN || vk == XPLM_VK_ENTER || vk == XPLM_VK_NUMPAD_ENT) {
        ApplyInput();
        CloseInputWindow();
        return;
    }

    if (vk == XPLM_VK_BACK) {
        if (gInputLen > 0) {
            gInputBuf[--gInputLen] = '\0';
        }
        return;
    }

    // 可打印字符
    if (inKey >= 0x20 && inKey < 0x7F && gInputLen < (int)sizeof(gInputBuf) - 1) {
        gInputBuf[gInputLen++] = inKey;
        gInputBuf[gInputLen] = '\0';
    }
}

static int InputMouseCB(XPLMWindowID /*wnd*/, int /*x*/, int /*y*/,
                         XPLMMouseStatus /*mouse*/, void* /*refcon*/)
{
    return 1;  // consume
}

static XPLMCursorStatus InputCursorCB(XPLMWindowID /*wnd*/, int /*x*/, int /*y*/,
                                       void* /*refcon*/)
{
    return xplm_CursorArrow;
}

static int InputWheelCB(XPLMWindowID /*wnd*/, int /*x*/, int /*y*/,
                         int /*wheel*/, int /*clicks*/, void* /*refcon*/)
{
    return 1;
}

// ── 输入窗口管理 ────────────────────────────────────────────

static void OpenInputWindow(InputTarget target, const char* title, const char* initVal) {
    if (gInputWin) CloseInputWindow();

    gInputTarget = target;
    gInputTitle = title;
    strncpy(gInputBuf, initVal, sizeof(gInputBuf) - 1);
    gInputBuf[sizeof(gInputBuf) - 1] = '\0';
    gInputLen = (int)strlen(gInputBuf);

    int screenW, screenH;
    XPLMGetScreenSize(&screenW, &screenH);

    int winW = 360, winH = 100;
    int l = (screenW - winW) / 2;
    int t = (screenH + winH) / 2;

    XPLMCreateWindow_t params;
    memset(&params, 0, sizeof(params));
    params.structSize = sizeof(params);
    params.left   = l;
    params.top    = t;
    params.right  = l + winW;
    params.bottom = t - winH;
    params.visible = 1;
    params.drawWindowFunc       = InputDrawCB;
    params.handleKeyFunc        = InputKeyCB;
    params.handleMouseClickFunc = InputMouseCB;
    params.handleCursorFunc     = InputCursorCB;
    params.handleMouseWheelFunc = InputWheelCB;
    params.refcon = nullptr;
    params.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
    params.layer = xplm_WindowLayerFloatingWindows;
    params.handleRightClickFunc = InputMouseCB;

    gInputWin = XPLMCreateWindowEx(&params);
    XPLMSetWindowTitle(gInputWin, title);
    XPLMSetWindowResizingLimits(gInputWin, winW, winH, winW, winH);
    XPLMBringWindowToFront(gInputWin);
    XPLMTakeKeyboardFocus(gInputWin);
}

static void CloseInputWindow() {
    if (gInputWin) {
        XPLMTakeKeyboardFocus(nullptr);
        XPLMDestroyWindow(gInputWin);
        gInputWin = nullptr;
    }
    gInputTarget = kInputNone;
}

static void ApplyInput() {
    MCDUConfig& cfg = MCDUConfigGet();

    if (gInputTarget == kInputUdpTarget) {
        // 解析 "IP:Port" 格式
        char buf[128];
        strncpy(buf, gInputBuf, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char* colon = strrchr(buf, ':');
        if (colon) {
            *colon = '\0';
            const char* ip = buf;
            int port = atoi(colon + 1);
            if (port > 0 && port <= 65535) {
                strncpy(cfg.udpTargetIP, ip, sizeof(cfg.udpTargetIP) - 1);
                cfg.udpTargetIP[sizeof(cfg.udpTargetIP) - 1] = '\0';
                cfg.udpPort = (uint16_t)port;
                MCDUUdpSetTarget(cfg.udpTargetIP, cfg.udpPort);

                char msg[128];
                snprintf(msg, sizeof(msg), "MCDU: UDP target changed to %s:%u\n",
                         cfg.udpTargetIP, cfg.udpPort);
                XPLMDebugString(msg);
            }
        }
    }
    else if (gInputTarget == kInputWebuiPort) {
        int port = atoi(gInputBuf);
        if (port > 0 && port <= 65535 && port != cfg.webuiPort) {
            int oldPort = cfg.webuiPort;
            cfg.webuiPort = port;

            if (cfg.webuiEnabled) {
                MCDUWebUIStop();
                MCDUWebUIStart(&gScreenBuffer, cfg.webuiPort);
            }

            char msg[128];
            snprintf(msg, sizeof(msg), "MCDU: WebUI port changed %d -> %d\n",
                     oldPort, cfg.webuiPort);
            XPLMDebugString(msg);
        }
    }

    RefreshMenuLabels();
}

// ── 公开接口 ────────────────────────────────────────────────

void MCDUMenuCreate() {
    gMenuContainerIdx = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "ToLiss MCDU", nullptr, 0);

    gMenuId = XPLMCreateMenu("ToLiss MCDU", XPLMFindPluginsMenu(),
                              gMenuContainerIdx, MenuHandler, nullptr);

    XPLMAppendMenuItem(gMenuId, "UDP Target: ...",   (void*)(intptr_t)kMenuUdpTarget,   0);
    XPLMAppendMenuSeparator(gMenuId);
    XPLMAppendMenuItem(gMenuId, "WebUI: ...",        (void*)(intptr_t)kMenuWebuiToggle,  0);
    XPLMAppendMenuItem(gMenuId, "WebUI Port: ...",   (void*)(intptr_t)kMenuWebuiPort,    0);

    RefreshMenuLabels();
    XPLMDebugString("MCDU: Menu created.\n");
}

void MCDUMenuDestroy() {
    CloseInputWindow();
    if (gMenuId) {
        XPLMDestroyMenu(gMenuId);
        gMenuId = nullptr;
    }
    XPLMDebugString("MCDU: Menu destroyed.\n");
}
