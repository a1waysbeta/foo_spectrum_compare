#include "stdafx.h"
#include "palette.h"
#include <cmath>
#include <assert.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ===================================================================
// 关于 Spek / Spek-X 调色板的真实来源（根据本地源码逐字对照）
// -------------------------------------------------------------------
// spek-master/src/spek-palette.h : enum palette { SPECTRUM, SOX, MONO,
//                                   PALETTE_COUNT,
//                                   PALETTE_DEFAULT = PALETTE_SOX   };
// spek-X-main/src/spek-palette.h : enum palette { SPECTRUM, SOX, MONO,
//                                   PALETTE_COUNT,
//                                   PALETTE_DEFAULT = PALETTE_SPECTRUM };
//
// 两个仓库的 spek-palette.cc（spectrum/sox/mono 三个函数）是逐字相同的
// （byte-for-byte identical），视觉差异完全来自：
//   (a) 默认 palette 不同：Spek = SoX 冷色调、Spek-X = Spectrum 彩虹色
//   (b) 截图时 lrange/urange 不同（Spek-X 常见截图用更窄的 lrange=-100
//       左右，导致暗部被提亮、洋红/绿黄色域更显眼，并非 palette 本身改了）
//
// 所以下面的 PALETTE_SPEK / PALETTE_SPEKX 是「真实源码提取」：
//   PALETTE_SPEK  := 直接调用 spek 原版 sox(level)         ← spek 默认
//   PALETTE_SPEKX := 直接调用 spek 原版 spectrum(level)    ← spek-x 默认
// 同时为保持老版本 3 个配色（Spectrum/SoX/Mono）的保存值向后兼容，
// legacy 三个 entry 仍保持独立 dispatch（逻辑上与新版一模一样，但
// 用户已保存的 cfg_palette 枚举值不会混淆）。
// ===================================================================

// ——— spek-master / spek-X-main 源码的 spectrum() 函数，逐字拷贝 ———
static uint32_t spek_orig_spectrum(double level)
{
    // Modified version of Dan Bruton's algorithm:
    // http://www.physics.sfasu.edu/astro/color/spectra.html
    level *= 0.6625;
    double r = 0.0, g = 0.0, b = 0.0;
    if (level >= 0 && level < 0.15) {
        r = (0.15 - level) / (0.15 + 0.075);
        g = 0.0;
        b = 1.0;
    } else if (level >= 0.15 && level < 0.275) {
        r = 0.0;
        g = (level - 0.15) / (0.275 - 0.15);
        b = 1.0;
    } else if (level >= 0.275 && level < 0.325) {
        r = 0.0;
        g = 1.0;
        b = (0.325 - level) / (0.325 - 0.275);
    } else if (level >= 0.325 && level < 0.5) {
        r = (level - 0.325) / (0.5 - 0.325);
        g = 1.0;
        b = 0.0;
    } else if (level >= 0.5 && level < 0.6625) {
        r = 1.0;
        g = (0.6625 - level) / (0.6625 - 0.5f);
        b = 0.0;
    }

    // Intensity correction —— 把 level<0.1 的区域线性向黑拉，这才是
    // Spek-X 默认截图（Spectrum palette）在暗部不发蓝的根本原因。
    double cf = 1.0;
    if (level >= 0.0 && level < 0.1) {
        cf = level / 0.1;
    }
    cf *= 255.0;

    uint32_t rr = (uint32_t)(r * cf + 0.5);
    uint32_t gg = (uint32_t)(g * cf + 0.5);
    uint32_t bb = (uint32_t)(b * cf + 0.5);
    return (rr << 16) + (gg << 8) + bb;
}

// ——— spek-master / spek-X-main 源码的 sox() 函数，逐字拷贝 ———
static uint32_t spek_orig_sox(double level)
{
    double r = 0.0;
    if (level >= 0.13 && level < 0.73) {
        r = sin((level - 0.13) / 0.60 * M_PI / 2.0);
    } else if (level >= 0.73) {
        r = 1.0;
    }

    double g = 0.0;
    if (level >= 0.6 && level < 0.91) {
        g = sin((level - 0.6) / 0.31 * M_PI / 2.0);
    } else if (level >= 0.91) {
        g = 1.0;
    }

    double b = 0.0;
    if (level < 0.60) {
        b = 0.5 * sin(level / 0.6 * M_PI);
    } else if (level >= 0.78) {
        b = (level - 0.78) / 0.22;
    }

    uint32_t rr = (uint32_t)(r * 255.0 + 0.5);
    uint32_t gg = (uint32_t)(g * 255.0 + 0.5);
    uint32_t bb = (uint32_t)(b * 255.0 + 0.5);
    return (rr << 16) + (gg << 8) + bb;
}

// ——— spek-master / spek-X-main 源码的 mono() 函数，逐字拷贝 ———
static uint32_t spek_orig_mono(double level)
{
    uint32_t v = (uint32_t)(level * 255.0 + 0.5);
    return (v << 16) + (v << 8) + v;
}

// Legacy 别名——保持前 3 个枚举的输出与历史版本二进制一致。
// 之前的实现已经是逐字抄 spek_orig_*，这里只是显式保持别名。
static uint32_t spectrum(double level) { return spek_orig_spectrum(level); }
static uint32_t sox(double level)      { return spek_orig_sox(level); }
static uint32_t mono(double level)     { return spek_orig_mono(level); }

// ===================================================================
// Dispatcher
// -------------------------------------------------------------------
//  枚举索引        含义                     映射到哪个 spek 原函数
//  ------------     ---------------------    ------------------------
//   SPECTRUM(0)     历史 legacy(Spek 旧彩)   spectrum()  -> spek_orig_spectrum
//   SOX(1)          SoX 风格                 sox()       -> spek_orig_sox
//   MONO(2)         线性灰阶                 mono()      -> spek_orig_mono
//   SPEK(3)         ⭐ Spek 默认            spek_orig_sox      ← 与 spek-master 打开截图一致
//   SPEKX(4)        ⭐ Spek-X 默认          spek_orig_spectrum ← 与 spek-X-main 打开截图一致
// ===================================================================
uint32_t spek_palette(palette_t palette, double level) {
    switch (palette) {
    case PALETTE_SPECTRUM: return spectrum(level);
    case PALETTE_SOX:      return sox(level);
    case PALETTE_MONO:     return mono(level);
    case PALETTE_SPEK:     return spek_orig_sox(level);         // Spek 默认 = SoX
    case PALETTE_SPEKX:    return spek_orig_spectrum(level);    // Spek-X 默认 = Spectrum(带暗部 cf 向黑 ramp)
    default:
        assert(false);
        return spek_orig_spectrum(level);
    }
}
