#pragma once

// ============================================================
// i18n — Chinese / English string table for foo_spectrum_compare
// ------------------------------------------------------------
// All user-visible strings go through the t() helper which picks the
// active language at runtime.  Language is stored as a cfg_int and
// also serialized into .fth / ui_element_config (version=2, field 6).
// ============================================================

enum language_t {
    LANG_EN = 0,
    LANG_ZH = 1,
    LANG_COUNT,
    LANG_DEFAULT = LANG_EN,
};

// String IDs — indices into the lookup table.
enum strid_t {
    S_DISPLAY_COUNT = 0,    // submenu title
    S_1_TRACK,               // 4 leaves
    S_2_TRACKS,
    S_3_TRACKS,
    S_4_TRACKS,
    S_PALETTE,               // submenu title
    S_SPECTRUM,              // 3 palette names
    S_SOX,
    S_MONO,
    S_AXES,                  // submenu title
    S_FREQ_AXIS,             // 3 axis toggles
    S_TIME_AXIS,
    S_DB_SCALE,
    S_TITLE_FORMAT,          // submenu title
    S_EDIT_FORMAT,           // 2 title format actions
    S_RESET_DEFAULT,
    S_LANGUAGE,              // submenu title
    S_LANG_EN,               // 2 language options
    S_LANG_ZH,
    S_REFRESH,               // flat leaf
    S_SELECT_TRACKS,         // placeholder text when no tracks
    S_ANALYZING,             // "Analyzing..." status
    S_ERROR,                 // "Error: " prefix
    STRID_COUNT,
};

// Returns the string for the given ID in the current language.
// Uses narrow char (UTF-8 for Chinese, ASCII for English) — callers
// convert to wide via pfc::stringcvt when feeding to Win32 APIs.
const char* i18n(strid_t id, language_t lang);
