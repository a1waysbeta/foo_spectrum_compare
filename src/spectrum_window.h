#pragma once

#include "stdafx.h"
#include "spectrum_analyzer.h"
#include <thread>
#include <atomic>

// GUID for our UI element
static const GUID guid_spectrum_compare =
    { 0x7a3f1c2d, 0x4e5b, 0x6f8a, { 0x9c, 0x1d, 0x2e, 0x3f, 0x4a, 0x5b, 0x6c, 0x7d } };

// Window class GUID
static const wchar_t* SPECTRUM_WND_CLASS = L"{SpectrumCompare-8F3A2B1C-4D5E-6F7A-8B9C-0D1E2F3A4B5C}";

// Default title format string (foobar2000 titleformat syntax)
static const char* DEFAULT_TITLE_FORMAT =
    "%title% \xC2\xB7 %artist% | %codec% | %bitrate% kbps"
    "[ | $info(bitspersample) bit] | %samplerate% Hz | $info(channels) CH | %path%";

// Config storage GUIDs
static const GUID guid_cfg_show_freq_axis =
    { 0x1a2b3c4d, 0x5e6f, 0x7081, { 0x92, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8, 0x09 } };
static const GUID guid_cfg_show_time_axis =
    { 0x2b3c4d5e, 0x6f70, 0x8192, { 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8, 0x09, 0x1a } };
static const GUID guid_cfg_title_format =
    { 0x3c4d5e6f, 0x7081, 0x9293, { 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8, 0x09, 0x1a } };
static const GUID guid_cfg_max_tracks =
    { 0x4d5e6f70, 0x8192, 0xa3b4, { 0xc5, 0xd6, 0xe7, 0xf8, 0x09, 0x1a, 0x2b, 0x3c } };
static const GUID guid_cfg_palette =
    { 0x5e6f7081, 0x92a3, 0xb4c5, { 0xd6, 0xe7, 0xf8, 0x09, 0x1a, 0x2b, 0x3c, 0x4d } };

// Persistent config storage (static globals, registered once at startup)
// Defined as static so they persist across window creation/destruction.
extern cfg_bool g_cfg_show_freq_axis;
extern cfg_bool g_cfg_show_time_axis;
extern cfg_string g_cfg_title_format;
extern cfg_int g_cfg_max_tracks;
extern cfg_int g_cfg_palette;

// Menu command IDs
#define IDM_SET_COUNT_1  1001
#define IDM_SET_COUNT_2  1002
#define IDM_SET_COUNT_3  1003
#define IDM_SET_COUNT_4  1004
#define IDM_REFRESH      1005
#define IDM_PALETTE_SPECTRUM 1010
#define IDM_PALETTE_SOX      1011
#define IDM_PALETTE_MONO     1012
#define IDM_TOGGLE_FREQ_AXIS 1020
#define IDM_TOGGLE_TIME_AXIS 1021
#define IDM_EDIT_TITLE_FORMAT 1022
#define IDM_RESET_TITLE_FORMAT 1023

// Holds per-track spectrum state
struct TrackSpectrum {
    metadb_handle_ptr handle;
    SpectrumData data;
    std::atomic<bool> analyzing{ false };
    std::atomic<bool> needs_repaint{ false };
    abort_callback_impl abort; // per-track abort, triggered when track is removed or component shuts down
};

// Main UI element window
class SpectrumCompareWindow :
    public ui_element_instance,
    public CWindowImpl<SpectrumCompareWindow>,
    public playlist_callback_single
{
public:
    DECLARE_WND_CLASS_EX(SPECTRUM_WND_CLASS, CS_VREDRAW | CS_HREDRAW, -1);

    SpectrumCompareWindow(ui_element_config::ptr config, ui_element_instance_callback_ptr p_callback);
    ~SpectrumCompareWindow();

    void initialize_window(HWND parent);
    HWND get_wnd() { return *this; }

    void set_configuration(ui_element_config::ptr config) { m_config = config; }
    ui_element_config::ptr get_configuration() { return m_config; }

    static GUID g_get_guid() { return guid_spectrum_compare; }
    static GUID g_get_subclass() { return ui_element_subclass_utility; }
    static void g_get_name(pfc::string_base& out) { out = "Spectrum Compare"; }
    static ui_element_config::ptr g_get_default_configuration() {
        return ui_element_config::g_create_empty(g_get_guid());
    }
    static const char* g_get_description() {
        return "Vertical spectrogram comparison for selected tracks (Spek-style).";
    }

    void notify(const GUID& p_what, t_size p_param1, const void* p_param2, t_size p_param2size);

    // playlist_callback_single
    void on_items_added(t_size p_base, const pfc::list_base_const_t<metadb_handle_ptr>& p_data, const bit_array& p_selection) override;
    void on_items_reordered(const t_size* p_order, t_size p_count) override;
    void on_items_removing(const bit_array& p_mask, t_size p_old_count, t_size p_new_count) override;
    void on_items_removed(const bit_array& p_mask, t_size p_old_count, t_size p_new_count) override;
    void on_items_selection_change(const bit_array& p_affected, const bit_array& p_state) override;
    void on_item_focus_change(t_size p_from, t_size p_to) override;
    void on_items_modified(const bit_array& p_mask) override;
    void on_items_modified_fromplayback(const bit_array& p_mask, play_control::t_display_level p_level) override;
    void on_items_replaced(const bit_array& p_mask, const pfc::list_base_const_t<playlist_callback::t_on_items_replaced_entry>& p_data) override;
    void on_item_ensure_visible(t_size p_idx) override;
    void on_playlist_switch() override;
    void on_playlist_renamed(const char* p_new_name, t_size p_new_name_len) override;
    void on_playlist_locked(bool p_locked) override;
    void on_default_format_changed() override;
    void on_playback_order_changed(t_size p_new_index) override;

    BEGIN_MSG_MAP_EX(SpectrumCompareWindow)
        MSG_WM_ERASEBKGND(OnEraseBkgnd)
        MSG_WM_PAINT(OnPaint)
        MSG_WM_SIZE(OnSize)
        MSG_WM_CONTEXTMENU(OnContextMenu)
        MSG_WM_TIMER(OnTimer)
        MSG_WM_SETFOCUS(OnSetFocus)
        MESSAGE_HANDLER(WM_SPECTRUM_READY, OnSpectrumReady)
        MESSAGE_HANDLER(WM_FINISH_EDIT_TITLE_FORMAT, OnFinishEditTitleFormat)
        COMMAND_ID_HANDLER_EX(IDM_SET_COUNT_1, OnSetCount)
        COMMAND_ID_HANDLER_EX(IDM_SET_COUNT_2, OnSetCount)
        COMMAND_ID_HANDLER_EX(IDM_SET_COUNT_3, OnSetCount)
        COMMAND_ID_HANDLER_EX(IDM_SET_COUNT_4, OnSetCount)
        COMMAND_ID_HANDLER_EX(IDM_REFRESH, OnRefresh)
        COMMAND_ID_HANDLER_EX(IDM_PALETTE_SPECTRUM, OnPalette)
        COMMAND_ID_HANDLER_EX(IDM_PALETTE_SOX, OnPalette)
        COMMAND_ID_HANDLER_EX(IDM_PALETTE_MONO, OnPalette)
        COMMAND_ID_HANDLER_EX(IDM_TOGGLE_FREQ_AXIS, OnToggleFreqAxis)
        COMMAND_ID_HANDLER_EX(IDM_TOGGLE_TIME_AXIS, OnToggleTimeAxis)
        COMMAND_ID_HANDLER_EX(IDM_EDIT_TITLE_FORMAT, OnEditTitleFormat)
        COMMAND_ID_HANDLER_EX(IDM_RESET_TITLE_FORMAT, OnResetTitleFormat)
    END_MSG_MAP()

    // ImplementBumpableElem (CRTP base via ui_element_impl) accesses m_callback
    template<typename TClass> friend class ImplementBumpableElem;

private:
    LRESULT OnEraseBkgnd(CDCHandle dc);
    void OnPaint(CDCHandle);
    void OnSize(UINT nType, CSize size);
    void OnContextMenu(CWindow wnd, CPoint point);
    void OnTimer(UINT_PTR nIDEvent);
    void OnSetFocus(CWindow wndOld);

    void OnSetCount(UINT uNotifyCode, int nID, CWindow wndCtl);
    void OnRefresh(UINT uNotifyCode, int nID, CWindow wndCtl);
    void OnPalette(UINT uNotifyCode, int nID, CWindow wndCtl);
    void OnToggleFreqAxis(UINT uNotifyCode, int nID, CWindow wndCtl);
    void OnToggleTimeAxis(UINT uNotifyCode, int nID, CWindow wndCtl);
    void OnEditTitleFormat(UINT uNotifyCode, int nID, CWindow wndCtl);
    void OnResetTitleFormat(UINT uNotifyCode, int nID, CWindow wndCtl);

    // Inline title-format editing helpers (right-click menu entry only).
    void begin_inline_title_format_edit();
    void end_inline_title_format_edit(bool commit);
    static LRESULT CALLBACK title_edit_subclass_proc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

    // Spectrum rendering
    void render_spectrum(CDCHandle dc, const RECT& rc, const SpectrumData& data);
    void render_track_label(CDCHandle dc, const RECT& rc, const TrackSpectrum& track);
    void render_freq_axis(CDCHandle dc, const RECT& rc, int sample_rate);
    void render_time_axis(CDCHandle dc, const RECT& rc, double duration);

    // Selection management
    void update_selection();
    void start_analysis_for_track(size_t index);
    void analysis_worker(metadb_handle_ptr handle, std::shared_ptr<TrackSpectrum> target);

    // Runtime config (mirrors of cfg vars for fast access)
    int m_max_tracks = 4;
    palette_t m_palette = PALETTE_SPECTRUM;
    bool m_show_freq_axis = true;
    bool m_show_time_axis = true;
    pfc::string8 m_title_format = DEFAULT_TITLE_FORMAT;

    ui_element_config::ptr m_config;
    const ui_element_instance_callback_ptr m_callback;

    std::vector<std::shared_ptr<TrackSpectrum>> m_tracks;
    std::mutex m_tracks_mutex;
    std::atomic<bool> m_shutdown{ false };

    SpectrumAnalyzer m_analyzer;

    // Cached system DPI so rendering helpers don't need to call GetDC repeatedly.
    int m_dpi = 96;

    // Inline edit control for title format (right-click menu -> Edit format...).
    HWND m_hwnd_title_edit = NULL;

    // Guard against double-finish when Enter/Esc and WM_KILLFOCUS race.
    bool m_edit_finish_pending = false;

    static const UINT_PTR TIMER_REPAINT = 1;
    static const UINT WM_SPECTRUM_READY = WM_USER + 100;
    static const UINT WM_FINISH_EDIT_TITLE_FORMAT = WM_USER + 101;
    static const UINT_PTR IDC_INLINE_TITLE_EDIT = 2001;

    LRESULT OnSpectrumReady(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    LRESULT OnFinishEditTitleFormat(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
};
