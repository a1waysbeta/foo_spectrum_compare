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

// Menu command IDs
#define IDM_SET_COUNT_1  1001
#define IDM_SET_COUNT_2  1002
#define IDM_SET_COUNT_3  1003
#define IDM_SET_COUNT_4  1004
#define IDM_REFRESH      1005
#define IDM_PALETTE_SPECTRUM 1010
#define IDM_PALETTE_SOX      1011
#define IDM_PALETTE_MONO     1012

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
        MESSAGE_HANDLER(WM_SPECTRUM_READY, OnSpectrumReady)
        COMMAND_ID_HANDLER_EX(IDM_SET_COUNT_1, OnSetCount)
        COMMAND_ID_HANDLER_EX(IDM_SET_COUNT_2, OnSetCount)
        COMMAND_ID_HANDLER_EX(IDM_SET_COUNT_3, OnSetCount)
        COMMAND_ID_HANDLER_EX(IDM_SET_COUNT_4, OnSetCount)
        COMMAND_ID_HANDLER_EX(IDM_REFRESH, OnRefresh)
        COMMAND_ID_HANDLER_EX(IDM_PALETTE_SPECTRUM, OnPalette)
        COMMAND_ID_HANDLER_EX(IDM_PALETTE_SOX, OnPalette)
        COMMAND_ID_HANDLER_EX(IDM_PALETTE_MONO, OnPalette)
    END_MSG_MAP()

    // ImplementBumpableElem (CRTP base via ui_element_impl_withpopup) accesses m_callback
    template<typename TClass> friend class ImplementBumpableElem;

private:
    LRESULT OnEraseBkgnd(CDCHandle dc);
    void OnPaint(CDCHandle);
    void OnSize(UINT nType, CSize size);
    void OnContextMenu(CWindow wnd, CPoint point);
    void OnTimer(UINT_PTR nIDEvent);

    void OnSetCount(UINT uNotifyCode, int nID, CWindow wndCtl);
    void OnRefresh(UINT uNotifyCode, int nID, CWindow wndCtl);
    void OnPalette(UINT uNotifyCode, int nID, CWindow wndCtl);

    // Spectrum rendering
    void render_spectrum(CDCHandle dc, const RECT& rc, const SpectrumData& data);
    void render_track_label(CDCHandle dc, const RECT& rc, const TrackSpectrum& track);

    // Selection management
    void update_selection();
    void start_analysis_for_track(size_t index);
    void analysis_worker(metadb_handle_ptr handle, std::shared_ptr<TrackSpectrum> target);

    // Config
    int m_max_tracks = 4;
    palette_t m_palette = PALETTE_SPECTRUM;

    ui_element_config::ptr m_config;
    const ui_element_instance_callback_ptr m_callback;

    std::vector<std::shared_ptr<TrackSpectrum>> m_tracks;
    std::mutex m_tracks_mutex;
    std::atomic<bool> m_shutdown{ false };

    SpectrumAnalyzer m_analyzer;

    static const UINT_PTR TIMER_REPAINT = 1;
    static const UINT WM_SPECTRUM_READY = WM_USER + 100;

    LRESULT OnSpectrumReady(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
};
