#pragma once

#include "mcdu_data.h"

// 启动 HTTP 服务器线程（在 XPluginEnable 中调用）
// buffer: 共享屏幕缓冲区指针（生命周期由调用方管理）
// port: HTTP 监听端口
void MCDUWebUIStart(MCDUScreenBuffer* buffer, int port);

// 停止 HTTP 服务器线程（在 XPluginDisable 中调用）
void MCDUWebUIStop();
