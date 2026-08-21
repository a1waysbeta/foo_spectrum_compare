#pragma once

#include <stdint.h>

// Palette types borrowed from Spek
enum palette_t {
    PALETTE_SPECTRUM,
    PALETTE_SOX,
    PALETTE_MONO,
    PALETTE_COUNT,
    PALETTE_DEFAULT = PALETTE_SPECTRUM,
};

// level: 0.0 to 1.0
// returns 0x00RRGGBB
uint32_t spek_palette(palette_t palette, double level);
