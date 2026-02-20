#pragma once

#include <cstdint>
#include <cstring>

struct MCDUConfig {
    char     udpTargetIP[64];
    uint16_t udpPort;
    int      webuiPort;
    bool     webuiEnabled;

    MCDUConfig() : udpPort(7001), webuiPort(8080), webuiEnabled(true) {
        strcpy(udpTargetIP, "192.168.1.255");
    }
};

// 全局单例访问
MCDUConfig& MCDUConfigGet();
