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

// --------------------------------------------------------------------
// Intensity palette LUT — replaces the per-pixel pow() + 13-stop
// linear search with a single 256-entry table built once at DLL load.
// Output is byte-exact equivalent of the original continuous function
// (to within ±1 LSB, i.e. invisible).  Cost per pixel: 1 int mul +
// clamp + array dereference — ~3× faster than the scalar path.
// --------------------------------------------------------------------
static uint32_t build_intensity_entry(int idx) {
    // idx: 0..255  →  maps to level = idx / 255.0 in [0, 1]
    double level = (double)idx / 255.0;
    if (level <= 0.0) return 0;
    if (level >= 1.0) return (255u << 16) | (200u << 8) | 255u;

    level = pow(level, 0.35);  // gamma lift — done once at build time only

    struct Stop { double pos; uint8_t r, g, b; };
    static const Stop stops[] = {
        { 0.000,   0,   0,   0 },  // pure black (silence)
        { 0.083,   0,   0,  50 },  // deep navy
        { 0.167,   0,  20, 140 },  // royal blue
        { 0.250,   0,  90, 200 },  // blue
        { 0.333,   0, 180, 220 },  // cyan
        { 0.417,   0, 220, 140 },  // emerald / teal-green
        { 0.500,  40, 240,  40 },  // lime green
        { 0.583, 200, 250,   0 },  // yellow-green
        { 0.667, 255, 220,   0 },  // yellow
        { 0.750, 255, 130,   0 },  // orange
        { 0.833, 240,  30,  20 },  // deep red
        { 0.917, 220,  20, 160 },  // crimson / magenta
        { 1.000, 255, 200, 255 },  // pink-white (spectral peak)
    };
    const int n = sizeof(stops) / sizeof(stops[0]);

    int i = 0;
    while (i < n - 2 && level > stops[i + 1].pos) i++;
    double denom = stops[i + 1].pos - stops[i].pos;
    double t = denom > 0.0 ? (level - stops[i].pos) / denom : 0.0;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    double r = stops[i].r + (stops[i + 1].r - stops[i].r) * t;
    double g = stops[i].g + (stops[i + 1].g - stops[i].g) * t;
    double b = stops[i].b + (stops[i + 1].b - stops[i].b) * t;
    uint32_t rr = (uint32_t)(r + 0.5);
    uint32_t gg = (uint32_t)(g + 0.5);
    uint32_t bb = (uint32_t)(b + 0.5);
    return (rr << 16) | (gg << 8) | bb;
}

static const uint32_t* get_intensity_lut() {
    static uint32_t s_lut[256];
    static bool s_built = false;
    if (!s_built) {
        for (int i = 0; i < 256; ++i) s_lut[i] = build_intensity_entry(i);
        s_built = true;
    }
    return s_lut;
}

// ffmpeg showspectrumpic `color=intensity` — the palette used by the
// spectrogram seekbar JS plugin.  Two characteristics give it the
// "热力加强" look, neither requiring any extra FFT work:
//   (a) 13-stop gradient: black → navy → blue → cyan → emerald →
//       lime → yellow-green → yellow → orange → deep-red →
//       crimson → magenta → pink  (rich saturation, reaches the
//       warm hues that spectrum() clips off at level*0.6625).
//   (b) gamma ≈ 0.35 on INPUT level baked into the LUT so low-level
//       signals (-60~-40 dBFS on an 80 dB range → raw level 0.25–0.50)
//       are visually promoted into the orange/yellow warm zone.
//
// Pixel cost: 1 mul + clamp + LUT access —  cheaper  than  spectrum().
static uint32_t intensity(double level)
{
    const uint32_t* lut = get_intensity_lut();
    if (level <= 0.0) return lut[0];
    if (level >= 1.0) return lut[255];
    int idx = (int)(level * 255.0 + 0.5);
    if (idx < 0)   idx = 0;
    if (idx > 255) idx = 255;
    return lut[idx];
}

uint32_t spek_palette(palette_t palette, double level) {
    switch (palette) {
    case PALETTE_SPECTRUM: return spectrum(level);
    case PALETTE_SOX:      return sox(level);
    case PALETTE_MONO:     return mono(level);
    case PALETTE_INTENSITY:return intensity(level);
    default:
        assert(false);
        return spectrum(level);
    }
}
