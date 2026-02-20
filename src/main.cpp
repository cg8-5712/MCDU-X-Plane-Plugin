#include "mcdu_data.h"
#include "mcdu_datarefs.h"
#include "mcdu_udp.h"
#include "mcdu_webui.h"
#include "mcdu_config.h"
#include "mcdu_menu.h"

#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

#include <cstring>

// ── 全局状态 ────────────────────────────────────────────────

static MCDUScreen       gLastScreen;      // 上一帧，用于变化检测
static uint32_t         gFrameCounter = 0;
static bool             gDataRefsReady = false;

// ── Flight Loop 回调 ────────────────────────────────────────

static float FlightLoopCallback(float /*inElapsed*/, float /*inElapsedSim*/,
                                 int /*inCounter*/, void* /*inRefcon*/)
{
    // 1. 延迟发现 datarefs
    if (!gDataRefsReady) {
        gDataRefsReady = MCDUDataRefsDiscover();
        if (!gDataRefsReady) {
            return 1.0f;  // 未就绪时 1 秒重试
        }
    }

    // 2. 读取并合成屏幕
    MCDUScreen screen;
    if (!MCDUDataRefsRead(screen)) {
        return 0.2f;
    }

    // 3. 变化检测
    if (screen != gLastScreen) {
        gFrameCounter++;
        screen.frameCounter = gFrameCounter;

        // UDP 发包（WebUI 通过 UDP 接收）
        MCDUUdpSend(screen, 1);

        gLastScreen = screen;
    }

    return 0.2f;  // 5 Hz
}

// ── 插件生命周期 ────────────────────────────────────────────

PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc)
{
    strcpy(outName, "ToLissMCDU");
    strcpy(outSig, "dzc.toliss.mcdu.controller");
    strcpy(outDesc, "MCDU external controller for ToLiss Airbus (X-Plane 11/12)");

    XPLMDebugString("MCDU: XPluginStart\n");

    // 初始化子系统
    MCDUDataRefsInit();

    MCDUConfig& cfg = MCDUConfigGet();
    if (!MCDUUdpInit(cfg.udpTargetIP, cfg.udpPort)) {
        XPLMDebugString("MCDU: WARNING - UDP init failed, continuing without UDP.\n");
    }

    MCDUMenuCreate();

    // 清空上一帧
    gLastScreen.clear();
    gFrameCounter = 0;
    gDataRefsReady = false;

    // 注册 flight loop
    XPLMRegisterFlightLoopCallback(FlightLoopCallback, 1.0f, nullptr);

    return 1;
}

PLUGIN_API void XPluginStop()
{
    XPLMDebugString("MCDU: XPluginStop\n");

    MCDUMenuDestroy();
    XPLMUnregisterFlightLoopCallback(FlightLoopCallback, nullptr);
    MCDUUdpShutdown();
}

PLUGIN_API int XPluginEnable()
{
    XPLMDebugString("MCDU: XPluginEnable\n");
    MCDUConfig& cfg = MCDUConfigGet();
    if (cfg.webuiEnabled) {
        XPLMDebugString("MCDU: Starting WebUI\n");
        MCDUWebUIStart(cfg.webuiPort, cfg.udpPort);
    }
    return 1;
}

PLUGIN_API void XPluginDisable()
{
    XPLMDebugString("MCDU: XPluginDisable — stopping WebUI\n");
    MCDUWebUIStop();
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID /*from*/, int /*msg*/, void* /*param*/)
{
}
