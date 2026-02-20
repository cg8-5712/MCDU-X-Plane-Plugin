#include "mcdu_datarefs.h"
#include "XPLMDataAccess.h"
#include "XPLMUtilities.h"

#include <cstring>
#include <string>

// ── DataRef 表 ──────────────────────────────────────────────
// 每个 dataref 是一个 byte array (24 bytes = 24 chars per line).
// 颜色后缀 → MCDUColor 映射:
//   w → WHITE, g → GREEN, b → BLUE, a → AMBER, y → YELLOW, m → MAGENTA
// 特殊后缀:
//   s  → 标记小字体位置 (SMALL)
//   Lg → 大字体绿色 (LARGE + GREEN)
//   Lw → 大字体白色 (LARGE + WHITE)

struct ColorLayer {
    const char* suffix;
    MCDUColor   color;
    MCDUFont    font;   // NORMAL 除非是 Lg/Lw
};

// 普通颜色层（NORMAL 字体）
static const ColorLayer kColorLayers[] = {
    {"w", MCDUColor::WHITE,   MCDUFont::NORMAL},
    {"g", MCDUColor::GREEN,   MCDUFont::NORMAL},
    {"b", MCDUColor::BLUE,    MCDUFont::NORMAL},
    {"a", MCDUColor::AMBER,   MCDUFont::NORMAL},
    {"y", MCDUColor::YELLOW,  MCDUFont::NORMAL},
    {"m", MCDUColor::MAGENTA, MCDUFont::NORMAL},
};
static constexpr int kNumColorLayers = 6;

// 大字体层（仅 label 行有）
static const ColorLayer kLargeLayers[] = {
    {"Lg", MCDUColor::GREEN, MCDUFont::LARGE},
    {"Lw", MCDUColor::WHITE, MCDUFont::LARGE},
};
static constexpr int kNumLargeLayers = 2;

// ── DataRef 句柄存储 ───────────────────────────────────────

// title: 5 个颜色层 (b,g,s,w,y)
// stitle: 4 个颜色层 (b,g,w,y)
// label1-6: 每行 9 个层 (a,b,g,m,s,w,y,Lg,Lw)
// cont1-6: 每行 7 个层 (a,b,g,m,s,w,y)
// scont1-6: 每行 7 个层 (a,b,g,m,s,w,y)
// sp: 2 个层 (a,w)

// title 颜色后缀
static const char* kTitleSuffixes[] = {"b", "g", "s", "w", "y"};
static constexpr int kNumTitleSuffixes = 5;

// stitle 颜色后缀
static const char* kStitleSuffixes[] = {"b", "g", "w", "y"};
static constexpr int kNumStitleSuffixes = 4;

// label 颜色后缀 (含大字体)
static const char* kLabelSuffixes[] = {"a", "b", "g", "m", "s", "w", "y", "Lg", "Lw"};
static constexpr int kNumLabelSuffixes = 9;

// cont/scont 颜色后缀
static const char* kContSuffixes[] = {"a", "b", "g", "m", "s", "w", "y"};
static constexpr int kNumContSuffixes = 7;

// sp 颜色后缀
static const char* kSpSuffixes[] = {"a", "w"};
static constexpr int kNumSpSuffixes = 2;

// DataRef 句柄
static XPLMDataRef drTitle[kNumTitleSuffixes]       = {};
static XPLMDataRef drStitle[kNumStitleSuffixes]     = {};
static XPLMDataRef drLabel[6][kNumLabelSuffixes]    = {};
static XPLMDataRef drCont[6][kNumContSuffixes]      = {};
static XPLMDataRef drScont[6][kNumContSuffixes]     = {};
static XPLMDataRef drSp[kNumSpSuffixes]             = {};

static bool gAllFound = false;

// ── 辅助函数 ────────────────────────────────────────────────

static MCDUColor suffixToColor(const char* suffix) {
    if (strcmp(suffix, "w") == 0 || strcmp(suffix, "Lw") == 0) return MCDUColor::WHITE;
    if (strcmp(suffix, "g") == 0 || strcmp(suffix, "Lg") == 0) return MCDUColor::GREEN;
    if (strcmp(suffix, "b") == 0) return MCDUColor::BLUE;
    if (strcmp(suffix, "a") == 0) return MCDUColor::AMBER;
    if (strcmp(suffix, "y") == 0) return MCDUColor::YELLOW;
    if (strcmp(suffix, "m") == 0) return MCDUColor::MAGENTA;
    return MCDUColor::WHITE;
}

static MCDUFont suffixToFont(const char* suffix) {
    if (strcmp(suffix, "Lg") == 0 || strcmp(suffix, "Lw") == 0) return MCDUFont::LARGE;
    if (strcmp(suffix, "s") == 0) return MCDUFont::SMALL;
    return MCDUFont::NORMAL;
}

static bool isFontOnlySuffix(const char* suffix) {
    // "s" 层只标记小字体位置，不携带颜色字符
    return strcmp(suffix, "s") == 0;
}

// 读取一个 byte array dataref 到 buf[24]，不足部分填空格
static void readLine(XPLMDataRef dr, char buf[MCDU_COLS]) {
    memset(buf, ' ', MCDU_COLS);
    if (dr) {
        int len = XPLMGetDatab(dr, buf, 0, MCDU_COLS);
        // 不足部分保持空格
        for (int i = len; i < MCDU_COLS; ++i)
            buf[i] = ' ';
    }
}

// 将一个颜色层叠加到 MCDUCell 行
// 规则：非空格字符覆盖到对应位置
static void overlayColorLayer(MCDUCell row[MCDU_COLS], const char buf[MCDU_COLS],
                               MCDUColor color, MCDUFont font) {
    for (int c = 0; c < MCDU_COLS; ++c) {
        if (buf[c] != ' ' && buf[c] != '\0') {
            row[c].ch    = buf[c];
            row[c].color = color;
            if (font != MCDUFont::NORMAL) {
                row[c].font = font;
            }
        }
    }
}

// 将 "s" 层叠加 — 只标记小字体，不改变字符和颜色
static void overlaySmallFont(MCDUCell row[MCDU_COLS], const char buf[MCDU_COLS]) {
    for (int c = 0; c < MCDU_COLS; ++c) {
        if (buf[c] != ' ' && buf[c] != '\0') {
            row[c].font = MCDUFont::SMALL;
        }
    }
}

// ── 公开接口 ────────────────────────────────────────────────

void MCDUDataRefsInit() {
    memset(drTitle,  0, sizeof(drTitle));
    memset(drStitle, 0, sizeof(drStitle));
    memset(drLabel,  0, sizeof(drLabel));
    memset(drCont,   0, sizeof(drCont));
    memset(drScont,  0, sizeof(drScont));
    memset(drSp,     0, sizeof(drSp));
    gAllFound = false;
}

bool MCDUDataRefsDiscover() {
    if (gAllFound) return true;

    bool allOk = true;
    char name[128];

    // title{b,g,s,w,y}
    for (int i = 0; i < kNumTitleSuffixes; ++i) {
        if (!drTitle[i]) {
            snprintf(name, sizeof(name), "AirbusFBW/MCDU1title%s", kTitleSuffixes[i]);
            drTitle[i] = XPLMFindDataRef(name);
            if (!drTitle[i]) allOk = false;
        }
    }

    // stitle{b,g,w,y}
    for (int i = 0; i < kNumStitleSuffixes; ++i) {
        if (!drStitle[i]) {
            snprintf(name, sizeof(name), "AirbusFBW/MCDU1stitle%s", kStitleSuffixes[i]);
            drStitle[i] = XPLMFindDataRef(name);
            if (!drStitle[i]) allOk = false;
        }
    }

    // label1-6
    for (int row = 0; row < 6; ++row) {
        for (int i = 0; i < kNumLabelSuffixes; ++i) {
            if (!drLabel[row][i]) {
                snprintf(name, sizeof(name), "AirbusFBW/MCDU1label%d%s", row + 1, kLabelSuffixes[i]);
                drLabel[row][i] = XPLMFindDataRef(name);
                if (!drLabel[row][i]) allOk = false;
            }
        }
    }

    // cont1-6
    for (int row = 0; row < 6; ++row) {
        for (int i = 0; i < kNumContSuffixes; ++i) {
            if (!drCont[row][i]) {
                snprintf(name, sizeof(name), "AirbusFBW/MCDU1cont%d%s", row + 1, kContSuffixes[i]);
                drCont[row][i] = XPLMFindDataRef(name);
                if (!drCont[row][i]) allOk = false;
            }
        }
    }

    // scont1-6
    for (int row = 0; row < 6; ++row) {
        for (int i = 0; i < kNumContSuffixes; ++i) {
            if (!drScont[row][i]) {
                snprintf(name, sizeof(name), "AirbusFBW/MCDU1scont%d%s", row + 1, kContSuffixes[i]);
                drScont[row][i] = XPLMFindDataRef(name);
                if (!drScont[row][i]) allOk = false;
            }
        }
    }

    // sp{a,w}
    for (int i = 0; i < kNumSpSuffixes; ++i) {
        if (!drSp[i]) {
            snprintf(name, sizeof(name), "AirbusFBW/MCDU1sp%s", kSpSuffixes[i]);
            drSp[i] = XPLMFindDataRef(name);
            if (!drSp[i]) allOk = false;
        }
    }

    if (allOk && !gAllFound) {
        gAllFound = true;
        XPLMDebugString("MCDU: All 149 datarefs discovered.\n");
    }

    return gAllFound;
}

bool MCDUDataRefsRead(MCDUScreen& out) {
    if (!gAllFound) return false;

    out.clear();
    char buf[MCDU_COLS];

    // ── Row 0: Title ────────────────────────────────────────
    // 合成 title 层: b,g,s,w,y
    for (int i = 0; i < kNumTitleSuffixes; ++i) {
        const char* suf = kTitleSuffixes[i];
        readLine(drTitle[i], buf);
        if (isFontOnlySuffix(suf)) {
            overlaySmallFont(out.cells[0], buf);
        } else {
            overlayColorLayer(out.cells[0], buf, suffixToColor(suf), suffixToFont(suf));
        }
    }

    // ── Row 1: Label 1 (含 stitle 合并) ─────────────────────
    // 先叠加 stitle 层
    for (int i = 0; i < kNumStitleSuffixes; ++i) {
        const char* suf = kStitleSuffixes[i];
        readLine(drStitle[i], buf);
        overlayColorLayer(out.cells[1], buf, suffixToColor(suf), MCDUFont::NORMAL);
    }
    // 再叠加 label1 层（label1 覆盖 stitle）
    for (int i = 0; i < kNumLabelSuffixes; ++i) {
        const char* suf = kLabelSuffixes[i];
        readLine(drLabel[0][i], buf);
        if (isFontOnlySuffix(suf)) {
            overlaySmallFont(out.cells[1], buf);
        } else {
            overlayColorLayer(out.cells[1], buf, suffixToColor(suf), suffixToFont(suf));
        }
    }

    // ── Row 2: Content 1 ────────────────────────────────────
    // cont1 层
    for (int i = 0; i < kNumContSuffixes; ++i) {
        const char* suf = kContSuffixes[i];
        readLine(drCont[0][i], buf);
        if (isFontOnlySuffix(suf)) {
            overlaySmallFont(out.cells[2], buf);
        } else {
            overlayColorLayer(out.cells[2], buf, suffixToColor(suf), suffixToFont(suf));
        }
    }
    // scont1 层叠加（小字体内容覆盖）
    for (int i = 0; i < kNumContSuffixes; ++i) {
        const char* suf = kContSuffixes[i];
        readLine(drScont[0][i], buf);
        if (isFontOnlySuffix(suf)) {
            overlaySmallFont(out.cells[2], buf);
        } else {
            // scont 内容默认小字体
            MCDUFont font = MCDUFont::SMALL;
            overlayColorLayer(out.cells[2], buf, suffixToColor(suf), font);
        }
    }

    // ── Rows 3-12: Label 2-6 / Content 2-6 交替 ────────────
    for (int grp = 1; grp < 6; ++grp) {
        int labelRow   = 1 + grp * 2;      // rows 3,5,7,9,11
        int contentRow = 2 + grp * 2;      // rows 4,6,8,10,12

        // Label N+1
        for (int i = 0; i < kNumLabelSuffixes; ++i) {
            const char* suf = kLabelSuffixes[i];
            readLine(drLabel[grp][i], buf);
            if (isFontOnlySuffix(suf)) {
                overlaySmallFont(out.cells[labelRow], buf);
            } else {
                overlayColorLayer(out.cells[labelRow], buf, suffixToColor(suf), suffixToFont(suf));
            }
        }

        // Content N+1
        for (int i = 0; i < kNumContSuffixes; ++i) {
            const char* suf = kContSuffixes[i];
            readLine(drCont[grp][i], buf);
            if (isFontOnlySuffix(suf)) {
                overlaySmallFont(out.cells[contentRow], buf);
            } else {
                overlayColorLayer(out.cells[contentRow], buf, suffixToColor(suf), suffixToFont(suf));
            }
        }
        // scont N+1
        for (int i = 0; i < kNumContSuffixes; ++i) {
            const char* suf = kContSuffixes[i];
            readLine(drScont[grp][i], buf);
            if (isFontOnlySuffix(suf)) {
                overlaySmallFont(out.cells[contentRow], buf);
            } else {
                overlayColorLayer(out.cells[contentRow], buf, suffixToColor(suf), MCDUFont::SMALL);
            }
        }
    }

    // ── Row 13: Scratchpad ──────────────────────────────────
    for (int i = 0; i < kNumSpSuffixes; ++i) {
        const char* suf = kSpSuffixes[i];
        readLine(drSp[i], buf);
        overlayColorLayer(out.cells[13], buf, suffixToColor(suf), MCDUFont::NORMAL);
    }

    return true;
}
