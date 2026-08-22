#pragma once

#include <stdint.h>

// Palette types borrowed from Spek
enum palette_t {
    PALETTE_SPECTRUM = 0, // Bruton physics spectrum (original spectrum()
                          // borrowed by upstream spek-audio; note it clips
                          // the colour wheel at level*0.6625 — so the top
                          // third of the intensity range is clamped to red).
    PALETTE_SOX,          // Rob Sykes' SoX default palette.
    PALETTE_MONO,         // Linear greyscale.

    // New — colour maps visually matched to the spek.png / spek-x.png
    // reference screenshots.  Each uses an 18-stop piecewise-linear
    // anchor table; we interpolate in RGB so transitions are smooth
    // (no colour banding at any bit-depth).
    PALETTE_SPEK,         // Classic Spek: black-indigo → deep blue →
                          // teal → emerald → gold → deep red.
    PALETTE_SPEKX,        // Spek-X: violet-black → indigo → cyan →
                          // emerald → gold → magenta → deep plum.

    PALETTE_COUNT,
    PALETTE_DEFAULT = PALETTE_SPECTRUM,
};

// level: 0.0 (silence / noise floor) → 1.0 (normalised peak)
// return: 0x00RRGGBB
uint32_t spek_palette(palette_t palette, double level);
