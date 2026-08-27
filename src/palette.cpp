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
//   spek-0.8.5/src/spek-palette.cc
//
// They produce numerically identical output to Spek — any hand-tuned
// anchor table will deviate from Rob Sykes' sin() curves because the
// G and B ramps are non-linear S-shaped (sin π/2 t) and have dead
// bands (e.g. G stays strictly zero below level=0.6 ≈ -48 dB, which
// is why Spek keeps 2-10kHz mid-band strictly red/pink instead of
// prematurely going orange).
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

    // Intensity correction.
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
    // The default palette used by SoX, written by Rob Sykes.
    // Exact 1:1 port of spek-palette.cc sox() from Spek 0.8.5.
    //
    // Key characteristics vs our old anchor-table version:
    //   * R starts rising only at level=0.13 (≈ -104 dB) not at -120 dB,
    //     which makes the lowest noise band pure blue instead of purple.
    //   * G stays strictly 0 until level=0.6 (≈ -48 dB) — this single
    //     line is why Spek keeps its mid-band (2-10 kHz, -48..-72 dB)
    //     strictly red/pink instead of prematurely turning orange.
    //   * B goes through a sin-peak at level=0.3 then drops to 0 in
    //     [0.60, 0.78] before climbing linearly to white at level=1.
    //
    // None of these subtle features can be reproduced with a 13-point
    // linear anchor table; the sin curves produce a visibly smoother
    // gradient with no colour banding.
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
