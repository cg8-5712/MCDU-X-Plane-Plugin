#pragma once

#include <cstdint>

// 启动 WebUI（HTTP 服务器 + UDP 监听线程）
// httpPort: HTTP 监听端口
// udpPort: UDP 接收端口（与 MCDUUdp 发送端口一致）
void MCDUWebUIStart(int httpPort, uint16_t udpPort);

// 停止 WebUI
void MCDUWebUIStop();
