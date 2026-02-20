#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>

// MCDU 显示屏尺寸
constexpr int MCDU_ROWS = 14;
constexpr int MCDU_COLS = 24;

// 颜色枚举 — 与 UDP 包 byte 1 一致
enum class MCDUColor : uint8_t {
    WHITE   = 0,
    GREEN   = 1,
    BLUE    = 2,
    AMBER   = 3,
    YELLOW  = 4,
    MAGENTA = 5
};

// 字体枚举 — 与 UDP 包 byte 2 一致
enum class MCDUFont : uint8_t {
    NORMAL = 0,
    SMALL  = 1,
    LARGE  = 2
};

// 单个字符单元
struct MCDUCell {
    char     ch    = ' ';
    MCDUColor color = MCDUColor::WHITE;
    MCDUFont  font  = MCDUFont::NORMAL;
};

// 完整屏幕帧
struct MCDUScreen {
    MCDUCell cells[MCDU_ROWS][MCDU_COLS];
    uint32_t frameCounter = 0;

    void clear() {
        for (int r = 0; r < MCDU_ROWS; ++r)
            for (int c = 0; c < MCDU_COLS; ++c)
                cells[r][c] = {' ', MCDUColor::WHITE, MCDUFont::NORMAL};
    }

    bool operator==(const MCDUScreen& o) const {
        return std::memcmp(cells, o.cells, sizeof(cells)) == 0;
    }
    bool operator!=(const MCDUScreen& o) const { return !(*this == o); }
};

// 线程安全的屏幕缓冲区 — 主线程写，HTTP 线程读
class MCDUScreenBuffer {
public:
    void write(const MCDUScreen& screen) {
        std::lock_guard<std::mutex> lock(mtx_);
        screen_ = screen;
    }

    MCDUScreen read() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return screen_;
    }

    uint32_t frameCounter() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return screen_.frameCounter;
    }

private:
    mutable std::mutex mtx_;
    MCDUScreen screen_{};
};
