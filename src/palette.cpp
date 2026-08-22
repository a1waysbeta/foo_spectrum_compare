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
        g = (0.6625 - level) / (0.6625 - 0.5);
        b = 0.0;
    }

    // Intensity correction — linear black-ramp for level < 0.1 so the
    // very quiet noise floor drifts smoothly to pure black instead of
    // getting stuck on the darkest available blue (which otherwise
    // produces a "spurious blue band" at the bottom of every track).
    double cf = 1.0;
    if (level >= 0.0 && level < 0.1) {
        cf = level / 0.1;
    }
    cf *= 255.0;

    uint32_t rr = (uint32_t)(r * cf + 0.5);
    uint32_t gg = (uint32_t)(g * cf + 0.5);
    uint32_t bb = (uint32_t)(b * cf + 0.5);
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
