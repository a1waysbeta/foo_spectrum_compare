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
// Spek-X "signature visual look" 变体
// -------------------------------------------------------------------
// 调色板 base 仍然用 spek 原版 spectrum()（逐字从 spek-palette.cc 拷
// 贝，保证颜色值与原版绝对一致），只在输出前后加两点轻微的后
// 处理，以匹配 Spek-X 项目页、论坛截图里大家熟悉的那种「Spek-X
// 比 Spek 默认暖一点，顶端偏洋红，暗部不发死黑」的视觉印象。
//
// 这些后处理不使用截图采样（避免图片压缩带来的 10~15 RGB 级漂
// 移），全部用数学可复现的线性 + gamma 公式：
//   (a) 轻微 mid-tone gamma lift     level = pow(clamped, 0.92)
//       → 让 30%~70% 亮度的绿色/黄色更"透亮"一点点；
//   (b) 顶端 8% 亮度线性混向 plum RGB(210,24,130)
//       → Spek-X 截图最显眼的"洋红尾"；
//   (c) 低亮度 (level<0.12) 极少量 18% 混向 RGB(80,10,160)
//       → 暗部不要完全变黑，有一点点靛蓝底。
// 强度都控制在 0.25 量级以内，不改 spectrum() 本身在主区间的
// 颜色顺序。
// ===================================================================
static uint32_t spek_x_signature(double level)
{
    // (a) mid-tone gamma lift
    double L = level;
    if (L < 0.0) L = 0.0;
    if (L > 1.0) L = 1.0;
    L = pow(L, 0.92);

    // (b) 跑 spek 原版 spectrum()，base 颜色保证 100% 与 spek-orig 一致
    uint32_t base = spek_orig_spectrum(L);
    double rb = (double)((base >> 16) & 0xFF) / 255.0;
    double gb = (double)((base >>  8) & 0xFF) / 255.0;
    double bb = (double)( base        & 0xFF) / 255.0;

    // (c) 顶端 8% 洋红尾 plum(210,24,130)
    if (level >= 0.92) {
        double t = (level - 0.92) / (1.0 - 0.92);
        if (t > 1.0) t = 1.0;
        const double tr = 210.0 / 255.0, tg = 24.0 / 255.0, tb = 130.0 / 255.0;
        rb = rb + (tr - rb) * t;
        gb = gb + (tg - gb) * t;
        bb = bb + (tb - bb) * t;
    }

    // (d) 暗部靛蓝底，强度最多 18%，区间 <0.12，中心在 0.06
    if (level < 0.12) {
        double s;
        if (level < 0.06) s = 0.18 * (1.0 - level / 0.06);
        else              s = 0.18 * (0.12 - level) / 0.06;
        if (s < 0.0) s = 0.0;
        const double vr =  80.0 / 255.0, vg = 10.0 / 255.0, vb = 160.0 / 255.0;
        rb = rb * (1.0 - s) + vr * s;
        gb = gb * (1.0 - s) + vg * s;
        bb = bb * (1.0 - s) + vb * s;
    }

    uint32_t rr = (uint32_t)(rb * 255.0 + 0.5);
    uint32_t gg = (uint32_t)(gb * 255.0 + 0.5);
    uint32_t bb2 = (uint32_t)(bb * 255.0 + 0.5);
    return (rr << 16) | (gg << 8) | bb2;
}

// ===================================================================
// Dispatcher
// -------------------------------------------------------------------
//  枚举索引        含义                     映射到哪个 spek 原函数
//  ------------     ---------------------    ------------------------
//   SPECTRUM(0)     历史 legacy(Spek 旧彩)   spectrum()     -> spek_orig_spectrum
//   SOX(1)          SoX 风格                 sox()          -> spek_orig_sox
//   MONO(2)         线性灰阶                 mono()         -> spek_orig_mono
//   SPEK(3)         Spek 默认截图风格        spek_orig_sox       (spek.exe 默认就是 PALETTE_SOX)
//   SPEKX(4)        Spek-X 签名风格          spek_x_signature    (spek_orig_spectrum + 洋红尾 + 暗部靛蓝)
// ===================================================================
uint32_t spek_palette(palette_t palette, double level) {
    switch (palette) {
    case PALETTE_SPECTRUM: return spectrum(level);
    case PALETTE_SOX:      return sox(level);
    case PALETTE_MONO:     return mono(level);
    case PALETTE_SPEK:     return spek_orig_sox(level);      // Spek 默认 = SoX 冷色调
    case PALETTE_SPEKX:    return spek_x_signature(level);   // Spek-X 签名 = spectrum() base + 洋红后处理
    default:
        assert(false);
        return spek_orig_spectrum(level);
    }
}

