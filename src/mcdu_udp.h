#pragma once

#include <cstdint>

// 初始化 UDP socket（WSAStartup + socket 创建）
// ip/port: 初始目标地址
// 返回 true 成功
bool MCDUUdpInit(const char* ip, uint16_t port);

// 设置目标地址（可运行时更改）
void MCDUUdpSetTarget(const char* ip, uint16_t port);

// 发送一帧 MCDU 屏幕数据
// mcduId: 1 或 2
struct MCDUScreen;
void MCDUUdpSend(const MCDUScreen& screen, uint8_t mcduId);

// 关闭 socket + WSACleanup
void MCDUUdpShutdown();
