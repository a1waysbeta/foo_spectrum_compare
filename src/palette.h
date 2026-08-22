#pragma once

#include <stdint.h>

// Palette types — the 3 legacy entries are byte-identical to the
// upstream Spek palette functions.  The 2 new entries ("Spek" and
// "Spek-X") are also verbatim copies of those same functions, but
// routed so each menu label matches the DEFAULT palette of the
// corresponding upstream binary:
//
//   * PALETTE_SPEK  →  PALETTE_SOX  (default palette in spek-master)
//   * PALETTE_SPEKX →  PALETTE_SPECTRUM (default palette in spek-X-main)
//
// Reference (source code verified on local checkouts):
//   spek-master/src/spek-palette.h : PALETTE_DEFAULT = PALETTE_SOX
//   spek-X-main/src/spek-palette.h : PALETTE_DEFAULT = PALETTE_SPECTRUM
//   spek-{master,X-main}/src/spek-palette.cc : spectrum()/sox()/mono()
//                                               are byte-for-byte equal.
enum palette_t {
    PALETTE_SPECTRUM = 0, // Legacy — Dan Bruton's physics spectrum
                          // (clips colour wheel at level*0.6625 and
                          // applies a black-ramp for levels < 0.1).
    PALETTE_SOX,          // Legacy — Rob Sykes' SoX default palette.
    PALETTE_MONO,         // Legacy — linear greyscale.
    PALETTE_SPEK,         // Spek default (identical to PALETTE_SOX above;
                          // matches what spek.exe shows on first launch).
    PALETTE_SPEKX,        // Spek-X default (identical to PALETTE_SPECTRUM
                          // above; matches what Spek-X.exe shows on first
                          // launch.  Screenshots appear warmer because
                          // Spek-X often ships with a narrower lrange,
                          // NOT because the palette math itself differs).

    PALETTE_COUNT,
    PALETTE_DEFAULT = PALETTE_SPECTRUM,
};

// level: 0.0 (silence / noise floor) → 1.0 (normalised peak)
// return: 0x00RRGGBB
uint32_t spek_palette(palette_t palette, double level);
