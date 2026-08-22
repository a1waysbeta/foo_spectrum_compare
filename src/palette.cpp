#include "stdafx.h"
#include "palette.h"
#include <cmath>
#include <assert.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// =====================================================================
// The three palette functions below are literal, byte-for-byte C++
// ports of the corresponding functions in:
//   spek-master/src/spek-palette.cc  AND  spek-X-main/src/spek-palette.cc
// Both repositories share an identical palette implementation (the
// only real difference is their PALETTE_DEFAULT setting).  We keep
// these three entries because their numeric values (0, 1, 2) are
// persisted in .fth / layout config blobs and must not be reordered.
// =====================================================================

static uint32_t spectrum(double level)
{
    // 8-stop gradient palette (user-specified colors).
    // Gamma correction applied so warm colors appear at lower signal levels
    // — without this, most audio content falls in the blue/dark range
    // because raw FFT magnitudes are heavily skewed toward small values.
    if (level <= 0.0) return 0;
    if (level >= 1.0) return (255u << 16) | (250u << 8) | 107u;
    // gamma=0.4 compresses the dark range (0.0-0.15) and expands the
    // warm range (0.15-1.0), so typical -40dB signals land in red/orange
    // instead of stuck in blue.
    level = pow(level, 0.4);

    struct Stop { double pos; uint8_t r, g, b; };
    static const Stop stops[] = {
        { 0.00,   0,   0,   0 },  // #000000
        { 0.1429, 0,   0,  79 },  // #00004F
        { 0.2857, 80,  0, 123 },  // #50007B
        { 0.4286, 153, 0, 118 },  // #990076
        { 0.5714, 210, 0,  64 },  // #D20040
        { 0.7143, 245, 31,  0 },  // #F51F00
        { 0.8571, 255,174,  0 },  // #FFAE00
        { 1.00,   255,250,107 },  // #FFFA6B
    };
    const int n = sizeof(stops) / sizeof(stops[0]);

    // Find segment
    int i = 0;
    while (i < n - 2 && level > stops[i + 1].pos) i++;
    double t = (level - stops[i].pos) / (stops[i + 1].pos - stops[i].pos);
    if (t < 0) t = 0;
    if (t > 1) t = 1;

    double r = stops[i].r + (stops[i + 1].r - stops[i].r) * t;
    double g = stops[i].g + (stops[i + 1].g - stops[i].g) * t;
    double b = stops[i].b + (stops[i + 1].b - stops[i].b) * t;

    uint32_t rr = (uint32_t)(r + 0.5);
    uint32_t gg = (uint32_t)(g + 0.5);
    uint32_t bb = (uint32_t)(b + 0.5);
    return (rr << 16) | (gg << 8) | bb;
}

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
    return (rr << 16) | (gg << 8) | bb;
}

static uint32_t mono(double level)
{
    uint32_t v = (uint32_t)(level * 255.0 + 0.5);
    return (v << 16) | (v << 8) | v;
}

uint32_t spek_palette(palette_t palette, double level) {
    switch (palette) {
    case PALETTE_SPECTRUM: return spectrum(level);
    case PALETTE_SOX:      return sox(level);
    case PALETTE_MONO:     return mono(level);
    default:
        assert(false);
        return spectrum(level);
    }
}
