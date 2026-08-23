#include "stdafx.h"
#include "i18n.h"

// ============================================================
// String table — two rows per ID: English, then Chinese.
// Chinese strings are UTF-8 encoded.
// ============================================================
static const char* const g_strings[STRID_COUNT][LANG_COUNT] = {
    // S_DISPLAY_COUNT
    { "Display count",       "\xe6\x98\xbe\xe7\xa4\xba\xe6\x95\xb0\xe9\x87\x8f" },
    // S_1_TRACK .. S_4_TRACKS
    { "1 track",             "1 \xe9\xa6\x96" },
    { "2 tracks",            "2 \xe9\xa6\x96" },
    { "3 tracks",            "3 \xe9\xa6\x96" },
    { "4 tracks",            "4 \xe9\xa6\x96" },
    // S_PALETTE
    { "Palette",             "\xe9\x85\x8d\xe8\x89\xb2" },
    // S_SPECTRUM, S_SOX, S_MONO
    { "Spectrum",            "Spectrum" },
    { "SoX",                 "SoX" },
    { "Mono",                "\xe5\x8d\x95\xe8\x89\xb2" },
    // S_AXES
    { "Axes",                "\xe5\x9d\x90\xe6\xa0\x87\xe8\xbd\xb4" },
    // S_FREQ_AXIS, S_TIME_AXIS, S_DB_SCALE
    { "Frequency axis (kHz)", "\xe9\xa2\x91\xe7\x8e\x87\xe8\xbd\xb4 (kHz)" },
    { "Time axis (20s)",      "\xe6\x97\xb6\xe9\x97\xb4\xe8\xbd\xb4 (20\xe7\xa7\x92)" },
    { "dB scale",              "dB \xe5\x88\xbb\xe5\xba\xa6" },
    // S_TITLE_FORMAT
    { "Title format",        "\xe6\xa0\x87\xe9\xa2\x98\xe6\xa0\xbc\xe5\xbc\x8f" },
    // S_EDIT_FORMAT, S_RESET_DEFAULT
    { "Edit format...",      "\xe7\xbc\x96\xe8\xbe\x91\xe6\xa0\xbc\xe5\xbc\x8f..." },
    { "Reset to default",    "\xe6\x81\xa2\xe5\xa4\x8d\xe9\xbb\x98\xe8\xae\xa4" },
    // S_LANGUAGE
    { "Language",            "\xe8\xaf\xad\xe8\xa8\x80" },
    // S_LANG_EN, S_LANG_ZH
    { "English",             "English" },
    { "\xe4\xb8\xad\xe6\x96\x87", "\xe4\xb8\xad\xe6\x96\x87" },
    // S_REFRESH
    { "Refresh analysis",    "\xe5\x88\xb7\xe6\x96\xb0\xe5\x88\x86\xe6\x9e\x90" },
    // S_SELECT_TRACKS (placeholder)
    { "Select tracks in the playlist to view spectrograms",
      "\xe5\x9c\xa8\xe6\x92\xad\xe6\x94\xbe\xe5\x88\x97\xe8\xa1\xa8\xe4\xb8\xad\xe9\x80\x89\xe6\x8b\xa9\xe9\x9f\xb3\xe8\xbd\xa8\xe4\xbb\xa5\xe6\x9f\xa5\xe7\x9c\x8b\xe9\xa2\x91\xe8\xb0\xb1\xe5\x9b\xbe" },
    // S_ANALYZING
    { "Analyzing...",        "\xe5\x88\x86\xe6\x9e\x90\xe4\xb8\xad..." },
    // S_ERROR
    { "Error: ",              "\xe9\x94\x99\xe8\xaf\xaf\xef\xbc\x9a " },
};

const char* i18n(strid_t id, language_t lang) {
    if ((int)id < 0 || (int)id >= (int)STRID_COUNT) return "";
    if ((int)lang < 0 || (int)lang >= (int)LANG_COUNT) lang = LANG_DEFAULT;
    return g_strings[id][lang];
}
