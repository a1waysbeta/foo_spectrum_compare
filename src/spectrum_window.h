#pragma once

#include "stdafx.h"
#include "spectrum_analyzer.h"
#include "i18n.h"
#include <thread>
#include <atomic>

// GUID for our UI element
static const GUID guid_spectrum_compare =
    { 0x7a3f1c2d, 0x4e5b, 0x6f8a, { 0x9c, 0x1d, 0x2e, 0x3f, 0x4a, 0x5b, 0x6c, 0x7d } };

// Window class GUID
static const wchar_t* SPECTRUM_WND_CLASS = L"{SpectrumCompare-8F3A2B1C-4D5E-6F7A-8B9C-0D1E2F3A4B5C}";

// Default title format string (foobar2000 titleformat syntax)
static const char* DEFAULT_TITLE_FORMAT =
    "%title% | %codec% | %bitrate% kbps"
    "[ | $info(bitspersample) bit] | %samplerate% Hz | $info(channels) CH | %path%";

// Config storage GUIDs — 5 persisted settings.  The numerical values and
// relative order of the palette/max_tracks/axis/title entries are part
// of the versioned .fth / ui_element_config binary format and must not
// be changed (see spectrum_window.cpp build_instance_config /
// set_configuration for the exact wire layout).
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
static const GUID guid_cfg_language =
    { 0x6f708192, 0xa3b4, 0xc5d6, { 0xe7, 0xf8, 0x09, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e } };
static const GUID guid_cfg_show_db_scale =
    { 0x71829304, 0xb5c6, 0xd7e8, { 0xf9, 0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f, 0x70 } };
static const GUID guid_cfg_pseudo_transparency =
    { 0x82930415, 0xc6d7, 0xe8f9, { 0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f, 0x70, 0x81 } };

// Persistent config storage (static globals, registered once at startup).
// Defined as static so they persist across window creation/destruction.
extern cfg_bool g_cfg_show_freq_axis;
extern cfg_bool g_cfg_show_time_axis;
extern cfg_string g_cfg_title_format;
extern cfg_int g_cfg_max_tracks;
extern cfg_int g_cfg_palette;
extern cfg_int g_cfg_language;
extern cfg_bool g_cfg_show_db_scale;
extern cfg_bool g_cfg_pseudo_transparency;

// Menu command IDs — values are arbitrary but unique within this window's
// command range.  Do NOT reuse numbers across different IDs because
// set_configuration round-trips cfg_* values by their enum name, NOT by
// the IDM_* constant — but collisions between IDM_* constants would make
// COMMAND_ID_HANDLER_EX dispatch ambiguous.
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
#define IDM_LANG_ENGLISH  1030
#define IDM_LANG_CHINESE  1031
#define IDM_TOGGLE_DB_SCALE 1024
#define IDM_TOGGLE_PSEUDO_TRANSPARENCY 1025

// Holds per-track spectrum state
struct TrackSpectrum {
    metadb_handle_ptr handle;
    SpectrumData data;
    std::atomic<bool> analyzing{ false };
    std::atomic<bool> needs_repaint{ false };
    abort_callback_impl abort; // per-track abort, triggered when track is removed or component shuts down

    // ================= 渐进渲染位图缓存 =================
    // 只由 GUI 线程在持有 m_tracks_mutex 时访问。
    //
    // 【为什么必须有】
    // render_spectrum 每个像素含一次 log10f + palette 求值，还要对源数据
    // 做区间面积平均。分析过程中每次重绘都把整幅 W×H 重算一遍，这就是
    // 渐进显示不丝滑的根因：单帧成本 O(W×H) 决定了刷新率上不去，只能靠
    // 100ms/200ms 节流硬压帧数，看起来就是"一块一块蹦出来"。
    //
    // 【为什么能缓存】
    // 时间轴分母固定为 total_frames（见 render_spectrum 开头的长注释），
    // 所以第 k 列永远落在同一个像素 x 上，已经画出的像素永远不会移动 ——
    // 新数据只是向右接续。于是可以把位图留下来，每帧只算右边新长出的列，
    // 成本从 O(W×H) 降到 O(新增列×H)。
    //
    // 位图始终按**完整** rect 宽度分配（而不是当前 draw_width），
    // 这样生长过程中行距恒定，老像素一次都不用搬移。
    HBITMAP   cache_bmp = nullptr;
    uint32_t* cache_pixels = nullptr; // 指向 cache_bmp 像素，top-down 32bpp，行距 = cache_w
    int cache_w = 0;                  // 建缓存时的 spec rect 宽/高
    int cache_h = 0;
    int cache_total = 0;              // 建缓存时的时间轴分母 total_frames
    int cache_bins = 0;               // 建缓存时的 fft_bins
    int cache_palette = -1;           // 建缓存时的调色板
    int cache_cols = 0;               // 已算进位图的像素列数，下一列从这里开始

    void discard_render_cache() {
        if (cache_bmp) { DeleteObject(cache_bmp); cache_bmp = nullptr; }
        cache_pixels = nullptr;
        cache_w = cache_h = cache_total = cache_bins = cache_cols = 0;
        cache_palette = -1;
    }

    // 轨道被取消选中 / 组件关闭时释放 GDI 位图。
    // DIB section 是进程级 GDI 资源，从任何线程 DeleteObject 都合法；
    // 而且能走到析构说明引用计数已归零，不可能与 GUI 线程并发。
    ~TrackSpectrum() { discard_render_cache(); }
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

    // --- ui_element instance configuration ---
    //
    // The host (Default UI / Columns UI) drives these when our panel is
    // embedded in a layout, used as a floating popup, or exported /
    // imported via Scratchbox (.fth), or copy-pasted between slots.
    // We MUST round-trip *per-instance* runtime settings here — not just
    // hold the opaque m_config blob we were constructed with. Otherwise
    // "Export settings" writes 0 bytes of real payload and "Import /
    // Paste settings" has nothing to apply, which was the original bug.
    void set_configuration(ui_element_config::ptr config);
    ui_element_config::ptr get_configuration();

    static GUID g_get_guid() { return guid_spectrum_compare; }
    static GUID g_get_subclass() { return ui_element_subclass_utility; }
    static void g_get_name(pfc::string_base& out) { out = "Spectrum Compare"; }
    static ui_element_config::ptr g_get_default_configuration();
    static const char* g_get_description() {
        return "Vertical spectrogram comparison for selected tracks (Spek-style).";
    }

    void notify(const GUID& p_what, t_size p_param1, const void* p_param2, t_size p_param2size);

    // --- ui_element_instance: Layout Editing Mode custom context menu ----
    bool edit_mode_context_menu_test(const POINT& p_point, bool p_fromkeyboard) override;
    void edit_mode_context_menu_build(const POINT& p_point, bool p_fromkeyboard, HMENU p_menu, unsigned p_id_base) override;
    void edit_mode_context_menu_command(const POINT& p_point, bool p_fromkeyboard, unsigned p_id, unsigned p_id_base) override;

    // Temporary mapping recorded during edit_mode_context_menu_build and
    // consumed by edit_mode_context_menu_command.  Contents:
    //   m_edit_mode_cmd_to_idm[idx] == the IDM_* for clickable leaf #idx.
    // Block size == kTotalLeaves declared in spectrum_window.cpp.
    std::vector<UINT> m_edit_mode_cmd_to_idm;

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
        COMMAND_ID_HANDLER_EX(IDM_SET_COUNT_1, OnSetCount)
        COMMAND_ID_HANDLER_EX(IDM_SET_COUNT_2, OnSetCount)
        COMMAND_ID_HANDLER_EX(IDM_SET_COUNT_3, OnSetCount)
        COMMAND_ID_HANDLER_EX(IDM_SET_COUNT_4, OnSetCount)
        COMMAND_ID_HANDLER_EX(IDM_REFRESH, OnRefresh)
        COMMAND_ID_HANDLER_EX(IDM_PALETTE_SPECTRUM, OnPalette)
        COMMAND_ID_HANDLER_EX(IDM_PALETTE_SOX,      OnPalette)
        COMMAND_ID_HANDLER_EX(IDM_PALETTE_MONO,     OnPalette)
        COMMAND_ID_HANDLER_EX(IDM_TOGGLE_FREQ_AXIS, OnToggleFreqAxis)
        COMMAND_ID_HANDLER_EX(IDM_TOGGLE_TIME_AXIS, OnToggleTimeAxis)
        COMMAND_ID_HANDLER_EX(IDM_TOGGLE_DB_SCALE, OnToggleDbScale)
        COMMAND_ID_HANDLER_EX(IDM_TOGGLE_PSEUDO_TRANSPARENCY, OnTogglePseudoTransparency)
        COMMAND_ID_HANDLER_EX(IDM_EDIT_TITLE_FORMAT,  OnEditTitleFormat)
        COMMAND_ID_HANDLER_EX(IDM_RESET_TITLE_FORMAT, OnResetTitleFormat)
        COMMAND_ID_HANDLER_EX(IDM_LANG_ENGLISH,  OnLanguage)
        COMMAND_ID_HANDLER_EX(IDM_LANG_CHINESE,  OnLanguage)
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
    void OnToggleDbScale(UINT uNotifyCode, int nID, CWindow wndCtl);
    void OnTogglePseudoTransparency(UINT uNotifyCode, int nID, CWindow wndCtl);
    void OnEditTitleFormat(UINT uNotifyCode, int nID, CWindow wndCtl);
    void OnResetTitleFormat(UINT uNotifyCode, int nID, CWindow wndCtl);
    void OnLanguage(UINT uNotifyCode, int nID, CWindow wndCtl);

    // Inline title-format editing helpers (right-click menu entry only).
    void begin_inline_title_format_edit();
    void end_inline_title_format_edit(bool commit);
    static LRESULT CALLBACK title_edit_subclass_proc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

    // Parent (popup host) subclass to suppress the "Export settings" etc.
    // items that foobar2000's popup host adds to the system menu.
    void subclass_parent_window();
    static LRESULT CALLBACK parent_subclass_proc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

    // Spectrum rendering
    // 返回实际绘制出的像素宽度（渐进显示时可能小于 rc 宽度），
    // 让 OnPaint 知道右侧哪一段还是空白、可以放进度提示。
    // 取 TrackSpectrum& 而非 const SpectrumData&：需要读写该轨道的位图缓存。
    int render_spectrum(CDCHandle dc, const RECT& rc, TrackSpectrum& track);
    void render_track_label(CDCHandle dc, const RECT& rc, const TrackSpectrum& track);
    void render_freq_axis(CDCHandle dc, const RECT& rc, int sample_rate);
    void render_time_axis(CDCHandle dc, const RECT& rc, double duration);
    void render_db_scale(CDCHandle dc, const RECT& bar_rc, const RECT& label_rc, palette_t palette);

    // Selection management
    void update_selection();
    // Re-format titles of already-analyzed tracks using current m_title_format.
    // Much cheaper than OnRefresh — no audio decode, just titleformat evaluation.
    void refresh_track_titles();
    void start_analysis_for_track(size_t index);
    void analysis_worker(metadb_handle_ptr handle, std::shared_ptr<TrackSpectrum> target);

    // Runtime config (mirrors of cfg vars for fast access)
    int m_max_tracks = 4;
    palette_t m_palette = PALETTE_SOX;
    bool m_show_freq_axis = true;
    bool m_show_time_axis = true;
    bool m_show_db_scale = true;
    bool m_pseudo_transparency = false;
    pfc::string8 m_title_format = DEFAULT_TITLE_FORMAT;
    language_t m_language = LANG_DEFAULT;

    // 说明：这里原有一个 m_soften_strength（后置 RGB 模糊强度）成员。
    // P2 优化已移除该机制 —— render_spectrum 现在在**线性幅度域**做
    // 区间面积平均（box filter），是数学上正确的抗锯齿；
    // 而在 palette 映射之后再模糊 RGB 属于错误的域，只会损失清晰度。

    ui_element_config::ptr m_config;
    const ui_element_instance_callback_ptr m_callback;

    std::vector<std::shared_ptr<TrackSpectrum>> m_tracks;
    std::mutex m_tracks_mutex;
    std::atomic<bool> m_shutdown{ false };

    // Snapshot of the last selection that update_selection() actually processed
    // as "needing re-analysis".  Avoids spurious re-analysis when Enter in the
    // inline title editor triggers a synthetic focus-restore callback.
    struct last_selection_key {
        size_t max_tracks{ 0 };
        std::vector<metadb_handle_ptr> handles;
        bool equals(size_t mx, const metadb_handle_list& list) const {
            if (mx != max_tracks) return false;
            const size_t n = (std::min)(handles.size(), (size_t)list.get_count());
            for (size_t i = 0; i < n; ++i) {
                if (handles[i] != list[i]) return false;
            }
            return handles.size() == (size_t)list.get_count();
        }
        void assign(size_t mx, const metadb_handle_list& list, size_t use_count) {
            max_tracks = mx;
            handles.clear();
            handles.reserve(use_count);
            for (size_t i = 0; i < use_count; ++i) handles.emplace_back(list[i]);
        }
    } m_last_selection;

    SpectrumAnalyzer m_analyzer;

    // Cached system DPI so rendering helpers don't need to call GetDC repeatedly.
    int m_dpi = 96;

    // Inline edit control for title format (right-click menu -> Edit format...).
    HWND m_hwnd_title_edit = NULL;

    // Guard against double-finish when Enter/Esc and WM_KILLFOCUS race.
    bool m_edit_finish_pending = false;
    bool m_edit_commit = false;

    // Parent (popup host) window handle — subclassed to clean its system menu.
    HWND m_hwnd_parent = NULL;

    static const UINT_PTR TIMER_REPAINT = 1;
    static const UINT_PTR TIMER_END_EDIT = 2;
    static const UINT WM_SPECTRUM_READY = WM_USER + 100;
    static const UINT_PTR IDC_INLINE_TITLE_EDIT = 2001;
    static const UINT_PTR IDC_PARENT_SUBCLASS = 3;

    LRESULT OnSpectrumReady(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
};
