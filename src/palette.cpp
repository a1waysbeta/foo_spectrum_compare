#include "stdafx.h"
#include "palette.h"
#include <cmath>
#include <assert.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================
// Helpers
// ============================================================

// Piecewise linear ramp between N equally-spaced colour anchors.
// `anchors` is a flat list of { r,g,b } tuples (each 0..255); an
// imaginary first anchor at t<0 and a last at t>=1 wrap the range.
struct rgb_stop { uint8_t r, g, b; };

static uint32_t ramp_piecewise(const rgb_stop* stops, size_t n, double level) {
    if (level <= 0.0) return (stops[0].r << 16) | (stops[0].g << 8) | stops[0].b;
    if (level >= 1.0) { const rgb_stop& s = stops[n - 1]; return (s.r << 16) | (s.g << 8) | s.b; }
    const double scaled = level * (double)(n - 1);
    const size_t lo = (size_t)floor(scaled);
    const size_t hi = lo + 1 >= n ? n - 1 : lo + 1;
    const double frac = scaled - (double)lo;
    const double r = (double)stops[lo].r + frac * ((double)stops[hi].r - (double)stops[lo].r);
    const double g = (double)stops[lo].g + frac * ((double)stops[hi].g - (double)stops[lo].g);
    const double b = (double)stops[lo].b + frac * ((double)stops[hi].b - (double)stops[lo].b);
    return ((uint32_t)(r + 0.5) << 16) | ((uint32_t)(g + 0.5) << 8) | (uint32_t)(b + 0.5);
}

// Clamp helper
static inline double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

// ============================================================
// Existing palettes (kept byte-for-byte identical)
// ============================================================

// Modified version of Dan Bruton's algorithm:
// http://www.physics.sfasu.edu/astro/color/spectra.html
// Borrowed directly from Spek (the spectrum() in upstream spek-audio).
static uint32_t spectrum(double level)
{
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

    // Intensity correction.
    double cf = 1.0;
    if (level >= 0.0 && level < 0.1) {
        cf = level / 0.1;
    }
    cf *= 255.0;

    // Pack RGB values into a 32-bit uint.
    uint32_t rr = (uint32_t)(r * cf + 0.5);
    uint32_t gg = (uint32_t)(g * cf + 0.5);
    uint32_t bb = (uint32_t)(b * cf + 0.5);
    return (rr << 16) + (gg << 8) + bb;
}

// The default palette used by SoX and written by Rob Sykes.
// Borrowed directly from Spek.
static uint32_t sox(double level)
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

static uint32_t mono(double level)
{
    uint32_t v = (uint32_t)(level * 255.0 + 0.5);
    return (v << 16) + (v << 8) + v;
}

// ============================================================
// New palette #1: Spek (原生渐变, 锚点取自 spek.png 视觉采样)
// ------------------------------------------------------------
// 原版 Spek 的颜色层次：
//   最暗（噪声底 / -120dB 附近）→ 接近纯黑的深蓝 (#030016)
//   -80 ~ -60 dB  → 暗蓝 → 宝蓝 (#001133 → #004488)
//   -60 ~ -40 dB  → 宝蓝 → 蓝绿 / 青 (#004488 → #008866 → #00AA55)
//   -40 ~ -20 dB  → 翠绿 → 亮黄 (#00BB22 → #88DD00 → #FFEE00)
//   -20 ~ 0 dB    → 金黄 → 橙红 → 饱和红 (#FFB800 → #FF5A00 → #C40000)
//   0 dB (clip)   → 暗红偏黑压顶, 不漂白
// 这些锚点是对 spek.png 截图中各段"典型色"的肉眼取样并做了连续
// 化平滑（每段均匀停点），保证从"几乎纯黑深蓝"一路渐进到"暗红"，
// 颜色数目 17 个，足够做 8-bit 连续渐变，不会出现明显条带。
// ============================================================
static uint32_t spek_original(double level)
{
    static const rgb_stop stops[] = {
        {   3,   0,  22 }, // 0.00 noise floor       deep black-indigo
        {   6,   0,  48 }, // 0.06                   dark navy
        {   8,   8, 100 }, // 0.12  -110 .. -90 dB   royal blue-black
        {   0,  34, 136 }, // 0.18                   mid blue
        {   0,  68, 160 }, // 0.25  ~-80dB           bright navy
        {   0, 110, 170 }, // 0.31                   steel blue
        {   0, 150, 150 }, // 0.37                   teal / cyan-blue
        {   0, 180, 116 }, // 0.43  ~-60dB           cyan-green
        {   0, 200,  80 }, // 0.50                   green
        {  40, 220,  30 }, // 0.56                   bright green
        { 110, 235,   0 }, // 0.62                   lime
        { 180, 235,   0 }, // 0.68  ~-40dB           yellow-green
        { 235, 220,  10 }, // 0.75                   vivid yellow
        { 255, 184,   0 }, // 0.81                   golden yellow
        { 255, 120,   0 }, // 0.87                   orange
        { 255,  60,   0 }, // 0.93  ~-10dB           red-orange
        { 220,   0,   0 }, // 0.97                   saturated red
        { 160,   0,   0 }, // 1.00  clip             deep red (no blow to white)
    };
    return ramp_piecewise(stops, sizeof(stops) / sizeof(stops[0]), clamp01(level));
}

// ============================================================
// New palette #2: Spek-X (Spek 加强版渐变, spek-x.png 视觉采样)
// ------------------------------------------------------------
// Spek-X 与 Spek 的肉眼差别：
//   * 低强度（噪声底区）不是深蓝 → 偏深紫蓝 / 紫灰，"冷感"更
//     强，整图看起来"更有层次"。
//   * 中段翠绿比 Spek 更亮、更饱和，黄段更金。
//   * 高强度段（0 ~ -20 dB）Spek-X 颜色从橙 → 洋红 → 深紫红收
//     尾，比 Spek 经典"亮红"少了刺眼感。
// ============================================================
static uint32_t spek_x(double level)
{
    static const rgb_stop stops[] = {
        {   5,   0,  32 }, // 0.00  black-violet
        {  22,   0,  78 }, // 0.06  deep violet     ← Spek 蓝，Spek-X 带紫
        {  52,   4, 128 }, // 0.12  indigo
        {  34,  34, 178 }, // 0.18  violet-blue
        {   0,  88, 188 }, // 0.25  royal blue
        {   0, 132, 184 }, // 0.31  steel blue
        {   0, 170, 164 }, // 0.37  teal-cyan
        {   0, 204, 128 }, // 0.43  mint-green
        {  28, 228,  88 }, // 0.50  emerald (比 Spek 更亮)
        { 100, 242,  40 }, // 0.56  neon-lime
        { 176, 245,   0 }, // 0.62  yellow-green
        { 232, 240,   8 }, // 0.68  lemon yellow
        { 255, 212,  24 }, // 0.75  gold
        { 255, 164,   0 }, // 0.81  warm amber
        { 255, 104,  32 }, // 0.87  orange
        { 244,  52, 104 }, // 0.93  reddish-magenta  ← Spek-X 标志性"红转洋红"
        { 200,  12, 140 }, // 0.97  magenta
        { 136,   0,  96 }, // 1.00  deep plum  ← Spek-X 不爆红，用深紫收尾
    };
    return ramp_piecewise(stops, sizeof(stops) / sizeof(stops[0]), clamp01(level));
}

// ============================================================
// Dispatcher
// ============================================================

uint32_t spek_palette(palette_t palette, double level) {
    switch (palette) {
    case PALETTE_SPECTRUM: return spectrum(level);
    case PALETTE_SOX:      return sox(level);
    case PALETTE_MONO:     return mono(level);
    case PALETTE_SPEK:     return spek_original(level);
    case PALETTE_SPEKX:    return spek_x(level);
    default:
        assert(false);
        return spek_original(level); // fail-safe
    }
}
