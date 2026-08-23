#pragma once

#include <stdint.h>

// Built-in palettes — the numerical values (0, 1, 2, PALETTE_COUNT=3)
// have been stable since the first shipped release and are persisted
// inside .fth / layout config blobs.  DO NOT REORDER.
//
// Reference (source code verified on local checkouts of spek-master /
// spek-X-main at src/spek-palette.cc): all three entries are direct,
// verbatim, byte-for-byte C++ ports of the corresponding upstream
// functions.
enum palette_t {
    PALETTE_SPECTRUM = 0, // Dan Bruton's physics spectrum — the classic
                          // rainbow palette used by most audio visualisers.
                          // Clips hue ramp at level*0.6625 and applies a
                          // linear black-fade for level < 0.1.
    PALETTE_SOX,          // Rob Sykes' SoX palette — the default cold
                          // colour scheme used by SoX spectrogram.exe
                          // and the default scheme in spek.exe.
    PALETTE_MONO,         // Greyscale linear — 0→black, 1→white.  Good
                          // for printing / side-by-side comparisons.

    PALETTE_COUNT,
    PALETTE_DEFAULT = PALETTE_SOX,
};

// level: 0.0 (silence / noise floor) → 1.0 (normalised peak)
// return: 0x00RRGGBB
uint32_t spek_palette(palette_t palette, double level);
