// winsock2 必须在任何可能包含 windows.h 的头文件之前
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include "mcdu_udp.h"
#include "mcdu_data.h"
#include "XPLMUtilities.h"

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define INVALID_SOCKET -1
#define SOCKET_ERROR   -1
typedef int SOCKET;
#endif

#include <cstring>

// ── 包格式常量 ──────────────────────────────────────────────
// [0-1]   Magic: "MC" (0x4D43)
// [2]     Version: 0x01
// [3]     MCDU ID: 1 或 2
// [4-7]   Frame counter (uint32_t LE)
// [8-1015] 14×24×3 = 1008 bytes cell data
// [1016-1017] CRC16 (reserved, 0 for now)
static constexpr int PACKET_SIZE = 1018;
static constexpr int CELL_DATA_OFFSET = 8;

static SOCKET gSock = INVALID_SOCKET;
static struct sockaddr_in gTarget = {};
static bool gWsaInited = false;

// ── CRC16-CCITT (可选，暂填 0) ──────────────────────────────
// 预留接口，后续可启用
static uint16_t crc16(const uint8_t* /*data*/, int /*len*/) {
    return 0;
}

bool MCDUUdpInit(const char* ip, uint16_t port) {
#ifdef _WIN32
    if (!gWsaInited) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            XPLMDebugString("MCDU: WSAStartup failed.\n");
            return false;
        }
        gWsaInited = true;
    }
#endif

    gSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (gSock == INVALID_SOCKET) {
        XPLMDebugString("MCDU: Failed to create UDP socket.\n");
        return false;
    }

    // 允许广播
    int broadcastEnable = 1;
    setsockopt(gSock, SOL_SOCKET, SO_BROADCAST,
               (const char*)&broadcastEnable, sizeof(broadcastEnable));

    // 设置非阻塞
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(gSock, FIONBIO, &mode);
#else
    int flags = fcntl(gSock, F_GETFL, 0);
    fcntl(gSock, F_SETFL, flags | O_NONBLOCK);
#endif

    // 使用调用方指定的目标地址
    MCDUUdpSetTarget(ip, port);

    XPLMDebugString("MCDU: UDP socket initialized.\n");
    return true;
}

void MCDUUdpSetTarget(const char* ip, uint16_t port) {
    memset(&gTarget, 0, sizeof(gTarget));
    gTarget.sin_family = AF_INET;
    gTarget.sin_port = htons(port);
    inet_pton(AF_INET, ip, &gTarget.sin_addr);

    char msg[128];
    snprintf(msg, sizeof(msg), "MCDU: UDP target set to %s:%u\n", ip, port);
    XPLMDebugString(msg);
}

void MCDUUdpSend(const MCDUScreen& screen, uint8_t mcduId) {
    if (gSock == INVALID_SOCKET) return;

    uint8_t packet[PACKET_SIZE];
    memset(packet, 0, PACKET_SIZE);

    // Header
    packet[0] = 'M';
    packet[1] = 'C';
    packet[2] = 0x01;  // version
    packet[3] = mcduId;

    // Frame counter (little-endian)
    uint32_t fc = screen.frameCounter;
    packet[4] = (uint8_t)(fc & 0xFF);
    packet[5] = (uint8_t)((fc >> 8) & 0xFF);
    packet[6] = (uint8_t)((fc >> 16) & 0xFF);
    packet[7] = (uint8_t)((fc >> 24) & 0xFF);

    // Cell data: 14 rows × 24 cols × 3 bytes
    int offset = CELL_DATA_OFFSET;
    for (int r = 0; r < MCDU_ROWS; ++r) {
        for (int c = 0; c < MCDU_COLS; ++c) {
            const MCDUCell& cell = screen.cells[r][c];
            packet[offset++] = static_cast<uint8_t>(cell.ch);
            packet[offset++] = static_cast<uint8_t>(cell.color);
            packet[offset++] = static_cast<uint8_t>(cell.font);
        }
    }

    // CRC16 (reserved)
    uint16_t crc = crc16(packet, PACKET_SIZE - 2);
    packet[PACKET_SIZE - 2] = (uint8_t)(crc & 0xFF);
    packet[PACKET_SIZE - 1] = (uint8_t)((crc >> 8) & 0xFF);

    sendto(gSock, (const char*)packet, PACKET_SIZE, 0,
           (struct sockaddr*)&gTarget, sizeof(gTarget));
}

void MCDUUdpShutdown() {
    if (gSock != INVALID_SOCKET) {
#ifdef _WIN32
        closesocket(gSock);
#else
        close(gSock);
#endif
        gSock = INVALID_SOCKET;
    }

#ifdef _WIN32
    if (gWsaInited) {
        WSACleanup();
        gWsaInited = false;
    }
#endif

    XPLMDebugString("MCDU: UDP shutdown.\n");
}
