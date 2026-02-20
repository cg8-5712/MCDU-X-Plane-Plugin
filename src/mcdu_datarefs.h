#pragma once

#include "mcdu_data.h"

// 初始化 dataref 子系统（在 XPluginStart 中调用）
void MCDUDataRefsInit();

// 延迟发现 datarefs（在 flight loop 中每帧调用）
// 返回 true 表示所有 datarefs 已就绪
bool MCDUDataRefsDiscover();

// 读取所有 datarefs 并合成到 MCDUScreen
// 返回 true 表示成功读取（datarefs 已就绪）
bool MCDUDataRefsRead(MCDUScreen& out);
