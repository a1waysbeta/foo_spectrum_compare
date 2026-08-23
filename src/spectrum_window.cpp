#include "stdafx.h"
#include "spectrum_window.h"
#include "i18n.h"
#include <helpers/BumpableElem.h>
#include <helpers/atl-misc.h>
#include <algorithm>
#include <cmath>

// Static config storage (registered once at program startup)
cfg_bool g_cfg_show_freq_axis(guid_cfg_show_freq_axis, true);
cfg_bool g_cfg_show_time_axis(guid_cfg_show_time_axis, true);
cfg_string g_cfg_title_format(guid_cfg_title_format, DEFAULT_TITLE_FORMAT);
cfg_int g_cfg_max_tracks(guid_cfg_max_tracks, 4);
cfg_int g_cfg_palette(guid_cfg_palette, (int)PALETTE_SOX);
cfg_int g_cfg_language(guid_cfg_language, (int)LANG_DEFAULT);
cfg_bool g_cfg_show_db_scale(guid_cfg_show_db_scale, true);

SpectrumCompareWindow::SpectrumCompareWindow(
    ui_element_config::ptr config,
    ui_element_instance_callback_ptr p_callback)
    : m_callback(p_callback)
{
    // Cache system DPI once at construction so rendering helpers don't need
    // to call ::GetDC(::GetDesktopWindow()) on every draw.
    HDC screenDC = ::GetDC(NULL);
    m_dpi = GetDeviceCaps(screenDC, LOGPIXELSX);
    if (m_dpi < 96) m_dpi = 96;
    ::ReleaseDC(NULL, screenDC);

    // Load persistent config into runtime members as fallbacks, then let
    // set_configuration() override them when a saved panel configuration
    // exists. This ensures:
    //   * Empty / default configuration falls back to cfg_* registry values.
    //   * Imported .fth / pasted settings from Scratchbox win over cfg_*.
    m_show_freq_axis = g_cfg_show_freq_axis;
    m_show_time_axis = g_cfg_show_time_axis;
    m_show_db_scale = g_cfg_show_db_scale;
    m_title_format = g_cfg_title_format.get();
    m_max_tracks = (int)g_cfg_max_tracks;
    m_palette = (palette_t)pfc::clip_t<int>(
        (int)g_cfg_palette, 0, (int)PALETTE_COUNT - 1);
    m_language = (language_t)pfc::clip_t<int>(
        (int)g_cfg_language, 0, (int)LANG_COUNT - 1);

    m_analyzer.set_title_format(m_title_format.c_str());

    // Apply the serialized instance configuration if present.
    set_configuration(std::move(config));
}

SpectrumCompareWindow::~SpectrumCompareWindow() {
    m_shutdown = true;

    // Destroy any lingering inline edit control so its subclass callback
    // doesn't fire on a half-destroyed window.
    if (m_hwnd_title_edit != NULL) {
        ::RemoveWindowSubclass(m_hwnd_title_edit, title_edit_subclass_proc, 1);
        ::DestroyWindow(m_hwnd_title_edit);
        m_hwnd_title_edit = NULL;
    }

    // Remove the parent (popup host) subclass.
    if (m_hwnd_parent != NULL && ::IsWindow(m_hwnd_parent)) {
        ::RemoveWindowSubclass(m_hwnd_parent, parent_subclass_proc, IDC_PARENT_SUBCLASS);
        m_hwnd_parent = NULL;
    }

    // Abort all in-flight analyses so worker threads exit promptly.
    {
        std::lock_guard<std::mutex> lock(m_tracks_mutex);
        for (auto& t : m_tracks) {
            if (t->analyzing) {
                try { t->abort.abort(); } catch (...) {}
            }
        }
    }

    // Stop timers
    if (m_hWnd) {
        KillTimer(TIMER_REPAINT);
        KillTimer(TIMER_END_EDIT);
    }

    // Unregister playlist callback
    try {
        static_api_ptr_t<playlist_manager>()->unregister_callback(this);
    } catch (...) {}

    // Wait for analysis threads to notice abort and exit.
    // analyze() catches abort_exception via catch(...) and marks analyzing=false.
    for (int i = 0; i < 200; i++) {
        bool any_running = false;
        {
            std::lock_guard<std::mutex> lock(m_tracks_mutex);
            for (auto& t : m_tracks) {
                if (t->analyzing) { any_running = true; break; }
            }
        }
        if (!any_running) break;
        Sleep(10);
    }
}

void SpectrumCompareWindow::initialize_window(HWND parent) {
    WIN32_OP(Create(parent) != NULL);

    // Register playlist callback for active playlist
    static_api_ptr_t<playlist_manager>()->register_callback(
        this,
        playlist_callback_single::flag_on_items_selection_change |
        playlist_callback_single::flag_on_playlist_switch |
        playlist_callback_single::flag_on_items_added |
        playlist_callback_single::flag_on_items_removed
    );

    // Start repaint timer for progressive rendering
    SetTimer(TIMER_REPAINT, 200, NULL);

    // Subclass the popup host (parent) window to strip "Export settings" etc.
    // from its system menu. This runs after Create(parent) so m_hWnd is valid.
    subclass_parent_window();

    // Initial selection update
    update_selection();
}

// ============================================================
// Instance configuration (set_configuration / get_configuration)
// ------------------------------------------------------------
// Binary layout of the ui_element_config payload we write / read.
// Versioned so we can append fields without breaking old .fth files.
//
//   uint32_t version         ; 3 (v1 = no language, v2 = no db_scale)
//   uint32_t max_tracks      ; 1..4
//   uint32_t palette         ; palette_t (0..PALETTE_COUNT-1)
//   uint8_t  show_freq_axis  ; 0 / 1
//   uint8_t  show_time_axis  ; 0 / 1
//   string8  title_format    ; raw bytes (UTF-8 titleformat expression)
//   uint8_t  language        ; language_t (0=EN, 1=ZH)  [v2+]
//   uint8_t  show_db_scale   ; 0 / 1                      [v3+]
//
// v1 readers stop after title_format and ignore the language byte,
// which is fine — language defaults to English on old .fth files.
// v2 readers stop after language and ignore show_db_scale, which
// defaults to true (dB axis visible).
// ============================================================
static constexpr uint32_t kConfigMagicVersion = 3u;
static constexpr int        kMinMaxTracks = 1;
static constexpr int        kMaxMaxTracks = 4;

// Forward decl — actual impl lives near render_db_scale, below the
// render_spectrum call site that references it.
static void soften_spectrum(uint32_t* pixels, int width, int height, int strength);

static ui_element_config::ptr build_instance_config(
    int max_tracks,
    palette_t palette,
    bool show_freq_axis,
    bool show_time_axis,
    const char* title_format,
    language_t language,
    bool show_db_scale)
{
    ui_element_config_builder b;
    b << kConfigMagicVersion;
    b << pfc::downcast_guarded<uint32_t>(
        pfc::clip_t<int>(max_tracks, kMinMaxTracks, kMaxMaxTracks));
    b << pfc::downcast_guarded<uint32_t>(pfc::clip_t<int>(
        (int)palette, 0, (int)PALETTE_COUNT - 1));
    b << (uint8_t)(show_freq_axis ? 1u : 0u);
    b << (uint8_t)(show_time_axis ? 1u : 0u);
    {
        pfc::string8 tf(title_format ? title_format : "");
        b << tf;
    }
    b << (uint8_t)pfc::clip_t<int>(
        (int)language, 0, (int)LANG_COUNT - 1);
    b << (uint8_t)(show_db_scale ? 1u : 0u);
    return b.finish(SpectrumCompareWindow::g_get_guid());
}

void SpectrumCompareWindow::set_configuration(ui_element_config::ptr config) {
    // Always remember the raw blob so later get_configuration() can fall
    // back to it (e.g. forward-compatible unknown bytes we don't parse).
    m_config = config;

    bool changed_something = false;
    bool title_format_changed = false;
    int new_max_tracks = m_max_tracks;
    palette_t new_palette = m_palette;
    bool new_show_freq = m_show_freq_axis;
    bool new_show_time = m_show_time_axis;
    bool new_show_db = m_show_db_scale;
    pfc::string8 new_title = m_title_format;
    language_t new_language = m_language;

    if (config.is_valid() && config->get_data_size() > 0) {
        try {
            ui_element_config_parser parser(config);
            uint32_t version = 0;
            parser >> version;
            if (version >= 1u) {
                uint32_t mx = 0, pal = 0;
                uint8_t sf = 0, st = 0;
                pfc::string8 tf;
                parser >> mx >> pal >> sf >> st >> tf;

                new_max_tracks = (int)pfc::clip_t<uint32_t>(
                    mx, (uint32_t)kMinMaxTracks, (uint32_t)kMaxMaxTracks);
                new_palette = (palette_t)pfc::clip_t<uint32_t>(
                    pal, 0u, (uint32_t)PALETTE_COUNT - 1u);
                new_show_freq = (sf != 0);
                new_show_time = (st != 0);
                if (tf.length() > 0) new_title = tf;

                // v2+ adds language field after title_format.
                if (version >= 2u) {
                    uint8_t lang = 0;
                    parser >> lang;
                    new_language = (language_t)pfc::clip_t<int>(
                        (int)lang, 0, (int)LANG_COUNT - 1);
                }
                // v3+ adds show_db_scale after language.
                if (version >= 3u) {
                    uint8_t sdb = 0;
                    parser >> sdb;
                    new_show_db = (sdb != 0);
                }
            }
            // (Earlier experimental versions of this file wrote extra
            // fields beyond version=1 with larger wire formats.  The PFC
            // parser doesn't require us to consume every byte so simply
            // ignoring anything past `title_format` is the correct,
            // forward-compatible thing to do.)
        } catch (...) {
            // Malformed payload → keep cfg defaults already loaded.
        }
    }

    // Commit parsed values to runtime members, mirror changes to cfg_*
    // storage, and invalidate any cached state that depends on them.
    if (new_max_tracks != m_max_tracks) {
        m_max_tracks = new_max_tracks;
        g_cfg_max_tracks = (int64_t)new_max_tracks;
        m_last_selection = last_selection_key{};
        changed_something = true;
    }
    if (new_palette != m_palette) {
        m_palette = new_palette;
        g_cfg_palette = (int64_t)new_palette;
        changed_something = true;
    }
    if (new_show_freq != m_show_freq_axis) {
        m_show_freq_axis = new_show_freq;
        g_cfg_show_freq_axis = new_show_freq;
        changed_something = true;
    }
    if (new_show_time != m_show_time_axis) {
        m_show_time_axis = new_show_time;
        g_cfg_show_time_axis = new_show_time;
        changed_something = true;
    }
    if (new_show_db != m_show_db_scale) {
        m_show_db_scale = new_show_db;
        g_cfg_show_db_scale = new_show_db;
        changed_something = true;
    }
    if (strcmp(new_title.get_ptr(), m_title_format.get_ptr()) != 0) {
        m_title_format = new_title;
        g_cfg_title_format.set(new_title);
        m_analyzer.set_title_format(new_title.get_ptr());
        // Don't clear m_last_selection — track selection itself hasn't
        // changed, only the label format.  Re-format existing titles
        // in-place via refresh_track_titles() instead of aborting and
        // re-analyzing everything from scratch.
        title_format_changed = true;
        changed_something = true;
    }
    if (new_language != m_language) {
        m_language = new_language;
        g_cfg_language = (int64_t)new_language;
        changed_something = true;
    }

    if (changed_something && IsWindow()) {
        if (title_format_changed) refresh_track_titles();
        Invalidate();
        update_selection();
    }
}

ui_element_config::ptr SpectrumCompareWindow::get_configuration() {
    ui_element_config::ptr fresh = build_instance_config(
        m_max_tracks, m_palette,
        m_show_freq_axis, m_show_time_axis,
        m_title_format.get_ptr(), m_language, m_show_db_scale);
    m_config = fresh;
    return fresh;
}

ui_element_config::ptr SpectrumCompareWindow::g_get_default_configuration() {
    return build_instance_config(
        (int)g_cfg_max_tracks,
        (palette_t)pfc::clip_t<int>(
            (int)g_cfg_palette, 0, (int)PALETTE_COUNT - 1),
        (bool)g_cfg_show_freq_axis,
        (bool)g_cfg_show_time_axis,
        g_cfg_title_format.get_ptr(),
        (language_t)pfc::clip_t<int>(
            (int)g_cfg_language, 0, (int)LANG_COUNT - 1),
        (bool)g_cfg_show_db_scale);
}

void SpectrumCompareWindow::notify(const GUID& p_what, t_size p_param1, const void* p_param2, t_size p_param2size) {
    if (p_what == ui_element_notify_colors_changed || p_what == ui_element_notify_font_changed) {
        Invalidate();
    }
}

// ============================================================
// Playlist callbacks
// ============================================================

void SpectrumCompareWindow::on_items_added(t_size p_base, const pfc::list_base_const_t<metadb_handle_ptr>& p_data, const bit_array& p_selection) {
    update_selection();
}

void SpectrumCompareWindow::on_items_reordered(const t_size* p_order, t_size p_count) {
    update_selection();
}

void SpectrumCompareWindow::on_items_removing(const bit_array& p_mask, t_size p_old_count, t_size p_new_count) {}

void SpectrumCompareWindow::on_items_removed(const bit_array& p_mask, t_size p_old_count, t_size p_new_count) {
    update_selection();
}

void SpectrumCompareWindow::on_items_selection_change(const bit_array& p_affected, const bit_array& p_state) {
    update_selection();
}

void SpectrumCompareWindow::on_item_focus_change(t_size p_from, t_size p_to) {}

void SpectrumCompareWindow::on_items_modified(const bit_array& p_mask) {}

void SpectrumCompareWindow::on_items_modified_fromplayback(const bit_array& p_mask, play_control::t_display_level p_level) {}

void SpectrumCompareWindow::on_items_replaced(const bit_array& p_mask, const pfc::list_base_const_t<playlist_callback::t_on_items_replaced_entry>& p_data) {
    update_selection();
}

void SpectrumCompareWindow::on_item_ensure_visible(t_size p_idx) {}

void SpectrumCompareWindow::on_playlist_switch() {
    update_selection();
}

void SpectrumCompareWindow::on_playlist_renamed(const char* p_new_name, t_size p_new_name_len) {}

void SpectrumCompareWindow::on_playlist_locked(bool p_locked) {}

void SpectrumCompareWindow::on_default_format_changed() {}

void SpectrumCompareWindow::on_playback_order_changed(t_size p_new_index) {}

// ============================================================
// Selection management
// ============================================================

void SpectrumCompareWindow::update_selection() {
    metadb_handle_list selected;
    static_api_ptr_t<playlist_manager>()->activeplaylist_get_selected_items(selected);

    // Determine how many tracks to display
    const size_t max_tracks_sz = (size_t)m_max_tracks;
    const size_t display_count = (std::min)(max_tracks_sz, selected.get_count());

    // ------------------------------------------------------------------
    // Short-circuit: if the effective selection (first `display_count`
    // tracks, with current `m_max_tracks` cap) is byte-for-byte identical
    // to the last one we processed, there is nothing to abort or start.
    //
    // This guards against synthetic playlist callbacks:
    //   * inline title-format editor closes on Enter → focus returns →
    //     foobar2000 re-fires on_items_selection_change even though the
    //     selection itself did not change;
    //   * layout / visibility / reorder notifications that come in as
    //     on_items_added / on_items_reordered with identical contents.
    // Without this, every Enter press would abort all in-flight threads
    // and restart the analysis → the user sees the spectrum "reload".
    //
    // We still always Invalidate() so label rendering / axes / title text
    // pick up config changes (palette swap, titleformat edit, axis toggle)
    // independently of the playlist state.
    // ------------------------------------------------------------------
    const bool selection_unchanged = m_last_selection.equals(
        max_tracks_sz, selected);
    if (selection_unchanged) {
        // Callers expect the view to refresh; just redraw, don't touch
        // the analysis state.
        Invalidate();
        return;
    }

    // Build new track list, reusing existing data where possible
    std::vector<std::shared_ptr<TrackSpectrum>> new_tracks;
    for (size_t i = 0; i < display_count; i++) {
        auto handle = selected[i];

        bool found = false;
        {
            std::lock_guard<std::mutex> lock(m_tracks_mutex);
            for (auto& existing : m_tracks) {
                if (existing->handle == handle) {
                    new_tracks.push_back(existing);
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            auto track = std::make_shared<TrackSpectrum>();
            track->handle = handle;
            new_tracks.push_back(track);
        }
    }

    // Abort analysis for tracks that are no longer selected.
    // The shared_ptr keeps the TrackSpectrum alive until the worker thread exits.
    std::vector<std::shared_ptr<TrackSpectrum>> old_tracks;
    {
        std::lock_guard<std::mutex> lock(m_tracks_mutex);
        old_tracks = std::move(m_tracks);
        m_tracks = std::move(new_tracks);
    }

    // For each old track not in the new list, abort its analysis so the worker exits quickly.
    for (auto& old_t : old_tracks) {
        bool still_selected = false;
        for (auto& new_t : m_tracks) {
            if (new_t == old_t) { still_selected = true; break; }
        }
        if (!still_selected && old_t->analyzing) {
            old_t->abort.abort();
        }
    }

    // Start analysis for new tracks that need it (outside lock to avoid deadlock)
    for (size_t i = 0; i < m_tracks.size(); i++) {
        bool need_start = false;
        {
            std::lock_guard<std::mutex> lock(m_tracks_mutex);
            if (!m_tracks[i]->data.ready && !m_tracks[i]->analyzing) {
                need_start = true;
            }
        }
        if (need_start) {
            start_analysis_for_track(i);
        }
    }

    // Remember the selection snapshot AFTER a successful processing run so
    // the fast path can return on subsequent identical callbacks.
    m_last_selection.assign(max_tracks_sz, selected, display_count);

    Invalidate();
}

void SpectrumCompareWindow::start_analysis_for_track(size_t index) {
    metadb_handle_ptr handle;
    std::shared_ptr<TrackSpectrum> trackPtr;
    {
        std::lock_guard<std::mutex> lock(m_tracks_mutex);
        if (index >= m_tracks.size()) return;
        if (m_tracks[index]->analyzing) return;
        m_tracks[index]->analyzing = true;
        handle = m_tracks[index]->handle;
        trackPtr = m_tracks[index];
    }

    // Capture shared_ptr to keep the TrackSpectrum alive even if removed from m_tracks.
    std::thread([this, handle, trackPtr]() {
        analysis_worker(handle, trackPtr);
    }).detach();
}

void SpectrumCompareWindow::analysis_worker(metadb_handle_ptr handle, std::shared_ptr<TrackSpectrum> target) {
    if (m_shutdown) return;

    SpectrumData data;
    try {
        m_analyzer.analyze(handle, data, target->abort);
    } catch (...) {
        // aborted or failed; mark as done so destructor/selection update can proceed
        std::lock_guard<std::mutex> lock(m_tracks_mutex);
        for (auto& t : m_tracks) {
            if (t->handle == handle) {
                t->analyzing = false;
                break;
            }
        }
        return;
    }

    if (m_shutdown) return;

    {
        std::lock_guard<std::mutex> lock(m_tracks_mutex);
        if (m_shutdown) return;
        // Re-find target in case list changed
        for (auto& t : m_tracks) {
            if (t->handle == handle) {
                t->data = std::move(data);
                t->analyzing = false;
                t->needs_repaint = true;
                break;
            }
        }
    }

    if (!m_shutdown && m_hWnd && IsWindow()) {
        PostMessage(WM_SPECTRUM_READY, 0, 0);
    }
}

void SpectrumCompareWindow::refresh_track_titles() {
    // Re-evaluate the titleformat string for already-analyzed tracks WITHOUT
    // re-decoding audio.  This is called when the user edits the title format
    // string — the spectrum data is unchanged, only the label text updates.
    //
    // Previously this called OnRefresh() which aborted all analysis threads
    // and re-decoded every track from scratch, causing visible flicker + CPU
    // spike for what is just a text label change.
    service_ptr_t<titleformat_object> obj;
    const char* fmt = m_title_format.empty() ? "%title%" : m_title_format.c_str();
    try {
        static_api_ptr_t<titleformat_compiler>()->compile_safe(obj, fmt);
    } catch (...) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_tracks_mutex);
        for (auto& t : m_tracks) {
            if (!t->handle.is_valid() || !t->data.ready) continue;
            try {
                titleformat_hook* hook = NULL;
                pfc::string8 title_tmp;
                file_info_impl info;
                if (t->handle->get_info_async(info)) {
                    t->handle->format_title_from_external_info(info, hook, title_tmp, obj, NULL);
                } else {
                    t->handle->format_title(hook, title_tmp, obj, NULL);
                }
                if (title_tmp.length() > 0) {
                    t->data.title = title_tmp.c_str();
                }
            } catch (...) {
                // keep old title on failure
            }
        }
    }

    if (IsWindow()) Invalidate();
}

// ============================================================
// Painting
// ============================================================

LRESULT SpectrumCompareWindow::OnEraseBkgnd(CDCHandle dc) {
    return TRUE; // We handle all drawing in OnPaint
}

void SpectrumCompareWindow::OnPaint(CDCHandle) {
    CPaintDC paint_dc(*this);
    CRect rc;
    GetClientRect(&rc);

    // Double-buffer: render everything to a memory DC, then BitBlt once.
    // Without this, the multiple Invalidate() calls during track selection
    // (update_selection → WM_PAINT with "Analyzing...", then
    // WM_SPECTRUM_READY → WM_PAINT with spectrum) each produce a visible
    // flash because the background fill is shown momentarily before content
    // is drawn on top.  Drawing to an off-screen bitmap and blitting in a
    // single operation eliminates all intermediate states.
    HDC mem_dc = CreateCompatibleDC(paint_dc);
    HBITMAP mem_bmp = CreateCompatibleBitmap(paint_dc, rc.Width(), rc.Height());
    HBITMAP old_bmp = (HBITMAP)SelectObject(mem_dc, mem_bmp);
    CDCHandle dc = mem_dc;

    // Fill background
    COLORREF bg_color = m_callback->query_std_color(ui_color_background);
    CBrush bg_brush;
    bg_brush.CreateSolidBrush(bg_color);
    dc.FillRect(&rc, bg_brush);

    std::lock_guard<std::mutex> lock(m_tracks_mutex);

    if (m_tracks.empty()) {
        // Draw placeholder text
        dc.SetTextColor(m_callback->query_std_color(ui_color_text));
        dc.SetBkMode(TRANSPARENT);
        SelectObjectScope fontScope(dc, (HGDIOBJ)m_callback->query_font_ex(ui_font_default));
        const UINT format = DT_NOPREFIX | DT_CENTER | DT_VCENTER | DT_SINGLELINE;
        pfc::stringcvt::string_wide_from_utf8 w_placeholder(i18n(S_SELECT_TRACKS, m_language));
        dc.DrawText(w_placeholder, -1, &rc, format);
    } else {

    // DPI-aware scaling: use DPI cached at construction time so we don't need
    // to grab a screen DC on every paint.
    int dpi = m_dpi;
    auto scale = [dpi](int v) -> int { return MulDiv(v, dpi, 96); };

    // Layout measurements (designed at 96 DPI, scaled to actual DPI)
    const int padding_outer = scale(8);      // outer margin around all content
    const int label_height = scale(22);      // title label row height
    const int freq_axis_width = m_show_freq_axis ? scale(48) : 0;
    const int time_axis_height = m_show_time_axis ? scale(20) : 0;
    const int track_gap = scale(6);          // gap between tracks
    const int label_to_spec_gap = scale(4);  // gap below label before spectrum
    const int spec_to_axis_gap = scale(2);   // gap between spectrum and axes
    const int db_bar_width = scale(10);      // 右侧色条宽度
    const int db_label_gap = scale(3);       // 色条和 dB 数字之间的 gap
    const int db_label_width = scale(46);    // dB 数字宽度 ("-85" / "0 dB")
    const int db_scale_total = m_show_db_scale ? (db_bar_width + db_label_gap + db_label_width) : 0;

    int track_count = (int)m_tracks.size();
    int total_track_gap = track_gap * (track_count - 1);
    int available_height = rc.Height() - 2 * padding_outer - total_track_gap;
    int track_height = available_height / track_count;
    if (track_height < label_height + time_axis_height + scale(20)) {
        track_height = label_height + time_axis_height + scale(20);
    }

    int y = rc.top + padding_outer;
    for (int i = 0; i < track_count; i++) {
        CRect track_rc(rc.left + padding_outer, y, rc.right - padding_outer, y + track_height);
        y += track_height + track_gap;

        // Spectrum area: below label, with freq axis (left) and time axis (bottom).
        // Compute these edges first so the title label can be aligned with the
        // spectrum left edge (avoids overlapping the 22kHz tick in the freq axis).
        int spec_top = track_rc.top + label_height + label_to_spec_gap;
        int spec_bottom = track_rc.bottom - time_axis_height - spec_to_axis_gap;
        int spec_left = track_rc.left + freq_axis_width;
        int spec_right = track_rc.right - db_scale_total;

        // Label area. Left edge is intentionally aligned with the spectrum
        // itself (not the outer panel) to keep text out of the freq axis column.
        CRect label_rc(spec_left, track_rc.top, spec_right, track_rc.top + label_height);
        render_track_label(dc.m_hDC, label_rc, *m_tracks[i]);

        // Frequency axis (left of spectrum, aligned with spectrum vertical range)
        if (m_show_freq_axis) {
            CRect freq_rc(track_rc.left, spec_top, track_rc.left + freq_axis_width, spec_bottom);
            render_freq_axis(dc.m_hDC, freq_rc, m_tracks[i]->data.sample_rate);
        }

        CRect spec_rc(spec_left, spec_top, spec_right, spec_bottom);

        // Draw spectrum or status
        if (m_tracks[i]->data.ready && !m_tracks[i]->data.error) {
            render_spectrum(dc.m_hDC, spec_rc, m_tracks[i]->data);
        } else if (m_tracks[i]->data.error) {
            dc.SetTextColor(RGB(255, 80, 80));
            dc.SetBkMode(TRANSPARENT);
            SelectObjectScope fontScope(dc, (HGDIOBJ)m_callback->query_font_ex(ui_font_default));
            pfc::string8 err;
            err << i18n(S_ERROR, m_language) << m_tracks[i]->data.error_msg.c_str();
            pfc::stringcvt::string_wide_from_utf8 err_w(err);
            dc.DrawText(err_w, -1, &spec_rc, DT_NOPREFIX | DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (m_tracks[i]->analyzing) {
            dc.SetTextColor(m_callback->query_std_color(ui_color_text));
            dc.SetBkMode(TRANSPARENT);
            SelectObjectScope fontScope(dc, (HGDIOBJ)m_callback->query_font_ex(ui_font_default));
            pfc::stringcvt::string_wide_from_utf8 w_analyzing(i18n(S_ANALYZING, m_language));
            dc.DrawText(w_analyzing, -1, &spec_rc, DT_NOPREFIX | DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // Time axis (below spectrum, aligned with spectrum horizontal range)
        if (m_show_time_axis && m_tracks[i]->data.duration > 0) {
            CRect time_rc(spec_left, spec_bottom + spec_to_axis_gap, spec_right, track_rc.bottom);
            render_time_axis(dc.m_hDC, time_rc, m_tracks[i]->data.duration);
        }

        // dB 色条 + 标签（频谱右侧，可由右键菜单开关）
        if (m_show_db_scale) {
            CRect db_bar_rc(spec_right + db_label_gap, spec_top, spec_right + db_label_gap + db_bar_width, spec_bottom);
            CRect db_label_rc(db_bar_rc.right + db_label_gap, spec_top, db_bar_rc.right + db_label_gap + db_label_width, spec_bottom);
            render_db_scale(dc.m_hDC, db_bar_rc, db_label_rc, m_palette);
        }
    }
    } // end else (m_tracks not empty)

    // Single BitBlt to screen — no intermediate states visible → no flicker.
    BitBlt(paint_dc, 0, 0, rc.Width(), rc.Height(), mem_dc, 0, 0, SRCCOPY);
    SelectObject(mem_dc, old_bmp);
    DeleteDC(mem_dc);
    DeleteObject(mem_bmp);
}

void SpectrumCompareWindow::render_track_label(CDCHandle dc, const RECT& rc, const TrackSpectrum& track) {
    // Draw label background
    COLORREF bg = m_callback->query_std_color(ui_color_background);
    CBrush brush;
    brush.CreateSolidBrush(bg);
    dc.FillRect(&rc, brush);

    // Draw track title (already formatted by analyzer using configured titleformat string)
    dc.SetTextColor(m_callback->query_std_color(ui_color_text));
    dc.SetBkMode(TRANSPARENT);
    SelectObjectScope fontScope(dc, (HGDIOBJ)m_callback->query_font_ex(ui_font_default));

    // DPI-aware horizontal padding
    int pad = MulDiv(8, m_dpi, 96);

    CRect text_rc(rc);
    text_rc.left += pad;
    text_rc.right -= pad;
    pfc::stringcvt::string_wide_from_utf8 label_w(track.data.title.c_str());
    dc.DrawText(label_w, -1, &text_rc, DT_NOPREFIX | DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void SpectrumCompareWindow::render_spectrum(CDCHandle dc, const RECT& rc, const SpectrumData& data) {
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    if (width <= 0 || height <= 0 || data.time_frames <= 0 || data.fft_bins <= 0) return;

    // Create DIB section for fast pixel access
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP hbmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!hbmp || !pixels) return;

    uint32_t* pixel_data = (uint32_t*)pixels;

    // Dynamic range for dB scaling — matches Spek upstream default:
    //   spek-spectrogram.cc  LRANGE=-120, URANGE=0  → range=120 dB
    // Previous versions used 80 dB, which compressed anything quieter
    // than -80 dBFS into palette level 0.0 (→ deep blue of spectrum()),
    // producing the user-reported "蓝色带" at the bottom of every track.
    // At 120 dB we give ~40 dB more headroom: the noise floor at ~-96
    // 与 spek 完全一致的 floor=-120dB, dyn_range=120dB，线性无 gamma。
    // 之前 floor=-85 直接把 -90~-120dB 的深蓝→墨蓝段裁成纯黑，
    // 导致 25-40kHz 高频区 spek 看起来是深蓝渐变，SC 却直接变成紫+黑，严重失真。
    // 恢复 120dB 完整范围后，-90dB (level 0.25) 会显示正蓝色，
    // -100dB (level 0.167) 深蓝，-120dB 黑，和 spek 截图完全对应。
    // 同时**不使用 gamma**：线性 dB → 线性 palette level，
    // 配合 13 段精确锚点插值，颜色丝滑无断层。
    const float floor_db = -120.0f;
    const float dyn_range = 120.0f;
    // 绝对 dBFS 参考 (Spek 式)：1.0 = 0dBFS 满量程正弦峰值。
    // 不能再用 data.max_level（track 自身频谱峰）做参考 — 那会把
    // 每首歌都归一化显示，导致 16-bit 量化噪声（-96dBFS）被抬升
    // 显示成紫色，橙黄红色整体前移 10-20dB。
    // analysis_worker 已经做了 DFT 2/N 归一化 + Hann 窗 coherent
    // gain 0.5 补偿（× 4 / N），所以真实 0dBFS 正弦峰 bin ≈ 1.0。
    const float ref_level = 1.0f;

    // Map spectrum data to pixels.
    //
    // X axis: time  (left = start, right = end).
    // Y axis: freq  (bottom = 0 Hz, top = Nyquist).  We use LINEAR mapping,
    //         which matches Spek/Spek-X exactly:
    //
    //   fft_bins[]        ——  index 0 = DC, index (fft_bins-1) ≈ Nyquist/2
    //   pixel row py      ——  py=height-1 (bottom of rect) → DC,
    //                          py=0 (top of rect) → Nyquist.
    //
    // Thus fbin_idx = (1 - py/(H-1)) * (fft_bins - 1)  (linear, no curve).
    //
    // 注意：前一版使用 powf(freq_norm, 1.5f) "类对数压缩"，使得频率轴
    // 的 0-5kHz 被人为拉伸超过 5-10kHz 的距离，造成截图中看到的
    // "刻度不均匀"。现已去掉该曲线，render_freq_axis 也做了对应调整，
    // 两者保持一致 → 0/5/10/15/20/22kHz 每段间距完全相等。
    //
    // To avoid the "staircase blocky look" that appears when
    // bins > panel_height (we're now using linear mapping, so a 1025-bin
    // spectrum displayed in a 500px rect would otherwise show every 2nd
    // bin as a 2px row), we do bilinear interpolation both vertically
    // (between bin i and bin i+1) and horizontally (between time frame
    // t and frame t+1).  This is exactly what Spek does when it calls
    // image.Scale(W,H) on the wxImage before blitting (wx uses bilinear
    // by default).  It adds ~5% CPU but materially improves "细腻度".
    for (int py = 0; py < height; py++) {
        // Vertical (frequency) sample position — float so we can lerp
        float freq_f = (1.0f - (float)py / (height - 1)) * (data.fft_bins - 1);
        if (freq_f < 0) freq_f = 0;
        if (freq_f > (data.fft_bins - 1)) freq_f = (float)(data.fft_bins - 1);
        int fi = (int)floor(freq_f);
        float ff = freq_f - (float)fi;
        int fi2 = (fi + 1 < data.fft_bins) ? fi + 1 : fi;
        // (spectrum values are stored bin0=DC; our data layout matches)

        for (int px = 0; px < width; px++) {
            // Horizontal (time) sample position
            float time_f = (float)px / width * (data.time_frames - 1);
            if (time_f < 0) time_f = 0;
            if (time_f > (data.time_frames - 1)) time_f = (float)(data.time_frames - 1);
            int ti = (int)floor(time_f);
            float tf = time_f - (float)ti;
            int ti2 = (ti + 1 < data.time_frames) ? ti + 1 : ti;

            // Four neighbours for bilinear
            float v00 = data.get(ti,  fi);
            float v10 = data.get(ti,  fi2);
            float v01 = data.get(ti2, fi);
            float v11 = data.get(ti2, fi2);

            float v0 = v00 + (v01 - v00) * tf;  // top edge (freq fi → fi+1), at time ti+tf  in ti
            float v1 = v10 + (v11 - v10) * tf;  // bottom edge
            float val = v0 + (v1 - v0) * ff;    // then lerp vertically

            // Convert to dBFS relative to track peak, then clamp to
            // [floor_db, 0] exactly like Spek's:
            //   value = fmin(urange, fmax(lrange, values[y]))
            //   level = (value - lrange) / (urange - lrange)
            // where urange=0, lrange=-120.  Clamping keeps palette
            // input within [0,1] and lets the cf black-ramp in
            // spectrum() do its job cleanly.
            float level_db = floor_db;
            if (val > 0 && ref_level > 0) {
                level_db = 20.0f * log10f(val / ref_level);
            }
            if (level_db > 0) level_db = 0;
            if (level_db < floor_db) level_db = floor_db;

            // 线性归一化，无 gamma。
            // level_norm = 1.0 ↔ 0dBFS (白), 0.0 ↔ -120dBFS (纯黑)
            // 13 段精确 RGB 插值保证：
            //   -40dB (人声泛音中心 0.667) → 纯红，不是橙红
            //   -60dB (谐波 0.5)          → 紫红
            //   -80dB (低电平 0.333)      → 蓝紫
            //   -90dB (高频空气 0.25)     → 正蓝色 (← 之前没显示的关键段)
            //   -110dB (0.083)            → 墨蓝
            float level_norm = (level_db - floor_db) / dyn_range;
            if (level_norm < 0) level_norm = 0;
            if (level_norm > 1) level_norm = 1;

            uint32_t color = spek_palette(m_palette, level_norm);
            // spek_palette 返回 0x00RRGGBB 格式。
            // Windows top-down 32-bit DIB 的 DWORD 格式也是 0x00RRGGBB
            // （小端内存顺序 = B G R X），因此直接写入 color 即可。
            // 错误做法：(b<<16)|(g<<8)|r 会把 R 和 B 通道对调，导致橙黄显示成深蓝。
            pixel_data[py * width + px] = color;
        }
    }

    // --------------------------------------------------------------
    // Post-process: configurable soften/smooth (cosmetic only).
    // m_soften_strength = 0 OFF, 1 LIGHT, 2 NORMAL  (see header for details)
    // NOTE: the real spek-style "silky smear" comes from FFT overlap
    // (Hann window + 75% overlap -> 4x temporal oversample), not from
    // post blur. Tweaking this strength from outside is cheap:
    //   search for `m_soften_strength = 1;` in spectrum_window.h
    // --------------------------------------------------------------
    soften_spectrum(pixel_data, width, height, m_soften_strength);

    // Blit to screen
    HDC mem_dc = CreateCompatibleDC(dc);
    HBITMAP old_bmp = (HBITMAP)SelectObject(mem_dc, hbmp);
    BitBlt(dc, rc.left, rc.top, width, height, mem_dc, 0, 0, SRCCOPY);
    SelectObject(mem_dc, old_bmp);
    DeleteDC(mem_dc);
    DeleteObject(hbmp);
}

void SpectrumCompareWindow::render_freq_axis(CDCHandle dc, const RECT& rc, int sample_rate) {
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0 || sample_rate <= 0) return;

    int nyquist = sample_rate / 2;

    // 根据 Nyquist 动态选择频率刻度。
    // 从 0kHz 开始，每 5kHz 一步，直到超过 Nyquist；
    // 超出 25kHz 后若 Nyquist 高（如 96kHz → 48kHz），再补充 25/30/35/40/45/48kHz，
    // 这样 44.1kHz 样本只到 22k，96kHz 样本会完整显示到 48k。
    int max_khz = (nyquist + 500) / 1000;
    std::vector<int> ticks;
    for (int k = 0; k <= 22; k += 5) ticks.push_back(k);
    static const int hi_ticks[] = { 25, 30, 35, 40, 45, 48, 50, 60, 70, 80, 90, 96 };
    for (int k : hi_ticks) if (k > 22 && k <= max_khz) ticks.push_back(k);
    // 尾部对齐 Nyquist（整数 kHz）
    if (max_khz >= 24 && (ticks.empty() || ticks.back() < max_khz)) {
        // 如果 max_khz 比最后一个 tick 远 >= 2kHz，则直接在 max_khz 补一格
        int last = ticks.empty() ? 0 : ticks.back();
        if (max_khz - last >= 2) ticks.push_back(max_khz);
    }

    // DPI-aware tick and label dimensions
    int dpi = m_dpi;
    auto scale = [dpi](int v) { return MulDiv(v, dpi, 96); };
    int tick_len = scale(4);
    int label_half_h = scale(8);
    int tick_gap = scale(2);

    COLORREF text_color = m_callback->query_std_color(ui_color_text);
    COLORREF bg_color = m_callback->query_std_color(ui_color_background);
    COLORREF grid_color = RGB(
        (GetRValue(text_color) + GetRValue(bg_color)) / 2,
        (GetGValue(text_color) + GetGValue(bg_color)) / 2,
        (GetBValue(text_color) + GetBValue(bg_color)) / 2
    );

    dc.SetTextColor(text_color);
    dc.SetBkMode(TRANSPARENT);
    SelectObjectScope fontScope(dc, (HGDIOBJ)m_callback->query_font_ex(ui_font_default));

    // Draw axis line on the right edge
    CPen axisPen;
    axisPen.CreatePen(PS_SOLID, 1, grid_color);
    SelectObjectScope penScope(dc, axisPen);
    dc.MoveTo(rc.right - 1, rc.top);
    dc.LineTo(rc.right - 1, rc.bottom);

    for (int khz : ticks) {
        int freq_hz = khz * 1000;
        if (freq_hz > nyquist) continue;

        // Map frequency to y-coordinate: LINEAR over [0, nyquist],
        // matching render_spectrum()'s new linear bin mapping.  So the
        // visual distance between 0–5, 5–10, 10–15, … kHz ticks is
        // exactly equal — this is what Spek does too.
        float freq_ratio = (float)freq_hz / nyquist;   // 0.0 (DC) → 1.0 (Nyquist)
        // bottom of rect = DC, top of rect = Nyquist.
        int py = rc.top + (int)((1.0f - freq_ratio) * (height - 1));

        if (py < rc.top || py > rc.bottom) continue;

        // Tick mark
        dc.MoveTo(rc.right - tick_len - tick_gap, py);
        dc.LineTo(rc.right - tick_gap, py);

        // Label (right-aligned, left of tick)
        pfc::string8 label;
        label << khz << "k";
        pfc::stringcvt::string_wide_from_utf8 label_w(label);
        CRect label_rc(rc.left, py - label_half_h, rc.right - tick_len - tick_gap - 1, py + label_half_h);
        dc.DrawText(label_w, -1, &label_rc, DT_NOPREFIX | DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

void SpectrumCompareWindow::render_time_axis(CDCHandle dc, const RECT& rc, double duration) {
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0 || duration <= 0) return;

    // Time labels at 20-second intervals (like Spek)
    const int time_interval = 20; // seconds

    // DPI-aware dimensions
    int dpi = m_dpi;
    auto scale = [dpi](int v) { return MulDiv(v, dpi, 96); };
    int tick_len = scale(4);
    int tick_gap = scale(1);
    int label_half_w = scale(22);

    COLORREF text_color = m_callback->query_std_color(ui_color_text);
    COLORREF bg_color = m_callback->query_std_color(ui_color_background);
    COLORREF grid_color = RGB(
        (GetRValue(text_color) + GetRValue(bg_color)) / 2,
        (GetGValue(text_color) + GetGValue(bg_color)) / 2,
        (GetBValue(text_color) + GetBValue(bg_color)) / 2
    );

    dc.SetTextColor(text_color);
    dc.SetBkMode(TRANSPARENT);
    SelectObjectScope fontScope(dc, (HGDIOBJ)m_callback->query_font_ex(ui_font_default));

    // Draw axis line on top edge
    CPen axisPen;
    axisPen.CreatePen(PS_SOLID, 1, grid_color);
    SelectObjectScope penScope(dc, axisPen);
    dc.MoveTo(rc.left, rc.top + tick_gap);
    dc.LineTo(rc.right, rc.top + tick_gap);

    for (int t = 0; t <= (int)duration; t += time_interval) {
        int px = rc.left + (int)((double)t / duration * width);
        if (px < rc.left || px > rc.right) continue;

        // Tick mark
        dc.MoveTo(px, rc.top + tick_gap);
        dc.LineTo(px, rc.top + tick_len + tick_gap);

        // Label (centered below tick)
        int min = t / 60;
        int sec = t % 60;
        pfc::string8 label;
        label << min << ":";
        if (sec < 10) label << "0";
        label << sec;
        pfc::stringcvt::string_wide_from_utf8 label_w(label);
        CRect label_rc(px - label_half_w, rc.top + tick_len + tick_gap + 1, px + label_half_w, rc.bottom);
        dc.DrawText(label_w, -1, &label_rc, DT_NOPREFIX | DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// ---------------------------------------------------------------------------
// soften_spectrum — separable 1D-then-transpose blur on a top-down 32-bit DIB.
// Strength table:
//   0 = OFF                       bypass (no copy, no cycle)
//   1 = LIGHT  (default)          [1,6,1] / 8    center 75%, edge 12.5% each
//   2 = NORMAL                    [1,2,1] / 4    center 50%, edge 25% each
//   3+ reserved.  Anything unknown falls back to OFF.
// Why not a 5-tap kernel? Because the real "spek smear" comes from its
// FFT overlap (Hann window + 75% overlap = 4x oversample along time axis)
// which we can't mimic here without redoing the analysis pipeline. This
// post-process is cosmetic only, so keep it cheap and bounded.
// ---------------------------------------------------------------------------
static void soften_spectrum(uint32_t* pixels, int width, int height, int strength) {
    if (!pixels || width < 3 || height < 3) return;
    int a, b, c, shift;
    switch (strength) {
    case 1: a = 1; b = 6; c = 1; shift = 3; break;  // / 8
    case 2: a = 1; b = 2; c = 1; shift = 2; break;  // / 4
    case 0: default: return;
    }
    const int round = 1 << (shift - 1);  // .5 for integer division rounding

    std::vector<uint32_t> tmp(size_t(width) * height);

    // --- Horizontal pass ---
    for (int y = 0; y < height; y++) {
        const uint32_t* src = pixels + size_t(y) * width;
        uint32_t* dst = tmp.data() + size_t(y) * width;
        for (int x = 0; x < width; x++) {
            int xl = (x > 0) ? x - 1 : x;
            int xr = (x < width - 1) ? x + 1 : x;
            uint32_t cl = src[xl], cc = src[x], cr = src[xr];
            uint32_t rl = (cl >> 16) & 0xFF, gl = (cl >> 8) & 0xFF, bl = cl & 0xFF;
            uint32_t rm = (cc >> 16) & 0xFF, gm = (cc >> 8) & 0xFF, bm = cc & 0xFF;
            uint32_t rr = (cr >> 16) & 0xFF, gr = (cr >> 8) & 0xFF, br = cr & 0xFF;
            uint32_t ro = (a * rl + b * rm + c * rr + round) >> shift;
            uint32_t go = (a * gl + b * gm + c * gr + round) >> shift;
            uint32_t bo = (a * bl + b * bm + c * br + round) >> shift;
            dst[x] = (ro << 16) | (go << 8) | bo;
        }
    }

    // --- Vertical pass ---
    for (int x = 0; x < width; x++) {
        for (int y = 0; y < height; y++) {
            int yu = (y > 0) ? y - 1 : y;
            int yd = (y < height - 1) ? y + 1 : y;
            uint32_t cu = tmp[size_t(yu) * width + x];
            uint32_t cm = tmp[size_t(y) * width + x];
            uint32_t cd = tmp[size_t(yd) * width + x];
            uint32_t rl = (cu >> 16) & 0xFF, gl = (cu >> 8) & 0xFF, bl = cu & 0xFF;
            uint32_t rm = (cm >> 16) & 0xFF, gm = (cm >> 8) & 0xFF, bm = cm & 0xFF;
            uint32_t rr = (cd >> 16) & 0xFF, gr = (cd >> 8) & 0xFF, br = cd & 0xFF;
            uint32_t ro = (a * rl + b * rm + c * rr + round) >> shift;
            uint32_t go = (a * gl + b * gm + c * gr + round) >> shift;
            uint32_t bo = (a * bl + b * bm + c * br + round) >> shift;
            pixels[size_t(y) * width + x] = (ro << 16) | (go << 8) | bo;
        }
    }
}

void SpectrumCompareWindow::render_db_scale(CDCHandle dc, const RECT& bar_rc, const RECT& label_rc, palette_t palette) {
    int bw = bar_rc.right - bar_rc.left;
    int bh = bar_rc.bottom - bar_rc.top;
    if (bw <= 0 || bh <= 0) return;

    // 与 render_spectrum 严格同步：floor=-120dB, dyn=120dB, 线性无 gamma
    const float floor_db = -120.0f;
    const float dyn_range = 120.0f;

    COLORREF text_color = m_callback->query_std_color(ui_color_text);
    COLORREF bg_color = m_callback->query_std_color(ui_color_background);
    COLORREF grid_color = RGB(
        (GetRValue(text_color) + GetRValue(bg_color)) / 2,
        (GetGValue(text_color) + GetGValue(bg_color)) / 2,
        (GetBValue(text_color) + GetBValue(bg_color)) / 2
    );

    // 1) 垂直画 colorbar：线性无 gamma，和 render_spectrum 完全一致
    //    top(y=0) = 0dB → level=1 白, bottom = -120dB → level=0 黑
    for (int y = 0; y < bh; y++) {
        float level = 1.0f - (float)y / (float)(bh - 1);
        uint32_t c = spek_palette(palette, level);
        uint8_t r = (uint8_t)((c >> 16) & 0xFF);
        uint8_t g = (uint8_t)((c >> 8) & 0xFF);
        uint8_t b = (uint8_t)(c & 0xFF);
        COLORREF cr = RGB(r, g, b);
        HPEN hPen = CreatePen(PS_SOLID, 1, cr);
        HPEN oldPen = (HPEN)SelectObject(dc, hPen);
        dc.MoveTo(bar_rc.left, bar_rc.top + y);
        dc.LineTo(bar_rc.right, bar_rc.top + y);
        SelectObject(dc, oldPen);
        DeleteObject(hPen);
    }

    // 色条边框
    CPen axisPen;
    axisPen.CreatePen(PS_SOLID, 1, grid_color);
    HPEN oldPen = (HPEN)SelectObject(dc, axisPen);
    dc.MoveTo(bar_rc.left, bar_rc.top);
    dc.LineTo(bar_rc.right - 1, bar_rc.top);
    dc.LineTo(bar_rc.right - 1, bar_rc.bottom - 1);
    dc.LineTo(bar_rc.left, bar_rc.bottom - 1);
    dc.LineTo(bar_rc.left, bar_rc.top);
    SelectObject(dc, oldPen);

    // 2) dB 刻度：每 20dB 一格，再加上 0dB（顶部）和 floor（底部）
    int dpi = m_dpi;
    auto scale = [dpi](int v) { return MulDiv(v, dpi, 96); };
    int label_half_h = scale(8);
    int tick_len = scale(4);

    dc.SetTextColor(text_color);
    dc.SetBkMode(TRANSPARENT);
    SelectObjectScope fontScope(dc, (HGDIOBJ)m_callback->query_font_ex(ui_font_default));

    // 每 10dB 一条刻度线，每 20dB 一档文字标签（避免 13 行文字太挤）
    std::vector<int> db_ticks;
    for (int db = 0; db >= (int)floor_db - 5; db -= 10) {
        db_ticks.push_back(db);
    }

    CPen tickPen;
    tickPen.CreatePen(PS_SOLID, 1, grid_color);
    SelectObjectScope tickScope(dc, tickPen);

    for (int db : db_ticks) {
        // 线性 ratio：(dB - floor) / dyn_range  →  0dB=1.0, -120dB=0.0
        float ratio = ((float)db - floor_db) / dyn_range;
        if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
        // top (ratio=1.0) → y=bar_rc.top; bottom (ratio=0) → y=bar_rc.bottom-1
        int py = bar_rc.top + (int)((1.0f - ratio) * (bh - 1));
        if (py < bar_rc.top || py >= bar_rc.bottom) continue;

        // Tick 小横线（所有人都画）
        dc.MoveTo(bar_rc.left - tick_len, py);
        dc.LineTo(bar_rc.left, py);

        // 文字标签：只在 20dB 倍数或 0 点画，防止 13 档文字太挤重叠
        if (db == 0 || (db % 20 == 0)) {
            pfc::string8 label;
            if (db == 0) label = "0 dB";
            else label << db;
            pfc::stringcvt::string_wide_from_utf8 label_w(label);
            CRect lr(label_rc.left, py - label_half_h, label_rc.right, py + label_half_h);
            dc.DrawText(label_w, -1, &lr, DT_NOPREFIX | DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

// ============================================================
// Size / Timer
// ============================================================

void SpectrumCompareWindow::OnSize(UINT nType, CSize size) {
    Invalidate();
}

void SpectrumCompareWindow::OnTimer(UINT_PTR nIDEvent) {
    if (nIDEvent == TIMER_END_EDIT) {
        // Fire-and-forget timer: the subclass proc set m_edit_commit and
        // started this timer to defer end_inline_title_format_edit() out of
        // the EDIT control's own message processing. Using WM_TIMER (instead
        // of PostMessage with WM_USER) guarantees delivery because WM_TIMER
        // always goes through the standard DispatchMessage path.
        KillTimer(TIMER_END_EDIT);
        bool commit = m_edit_commit;
        m_edit_finish_pending = false;
        end_inline_title_format_edit(commit);
        return;
    }
    if (nIDEvent == TIMER_REPAINT) {
        // Check if any track needs repaint
        bool need = false;
        {
            std::lock_guard<std::mutex> lock(m_tracks_mutex);
            for (auto& t : m_tracks) {
                if (t->needs_repaint) {
                    t->needs_repaint = false;
                    need = true;
                }
            }
        }
        if (need) Invalidate();
    }
}

LRESULT SpectrumCompareWindow::OnSpectrumReady(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
    Invalidate();
    bHandled = TRUE;
    return 0;
}

// ============================================================
// Context menu
// ------------------------------------------------------------
// Two modes, selected via m_callback->is_edit_mode_enabled():
//
//   * Layout Editing Mode ON:
//       We SetMsgHandled(FALSE) without showing any menu ourselves, so
//       the message falls through DefWindowProc onto the default
//       routing path — exactly what ControlPanelDUI in foo_nowbar does
//       (its handle_message() tail returns DefWindowProc).  The Default
//       UI / Columns UI host already has a pretranslate / subclass /
//       hook layer that walks child element slots and calls
//       ui_element_edit_tools::standard_edit_context_menu() with the
//       REAL slot (p_id) that owns our window.  That helper shows
//       Replace / Cut / Copy / Paste and, crucially, *asks the element
//       instance itself* whether it wants to append custom items via
//       the three edit_mode_context_menu_{test,build,command} virtual
//       hooks declared on ui_element_instance.  We implement those
//       hooks below so the menu ends up as:
//           ┌─────────────────────────────┐
//           │  <<Spectrum Compare>> (grey) │   label line
//           │  ──────────────────────────  │
//           │  Replace UI Element ...      │   ← host drives, real slot
//           │  ──────────────────────────  │
//           │  Cut / Copy / Paste          │
//           │  ──────────────────────────  │
//           │  Display count  ▶            │   ← our panel settings
//           │  Palette          ▶            (appended via _build hook)
//           │  Axes             ▶
//           │  Title format     ▶
//           │  ──────────────────────────  │
//           │  Refresh analysis            │
//           └─────────────────────────────┘
//
//   * Layout Editing Mode OFF:
//       We build & show the panel settings menu directly, without the
//       Replace / Cut / Copy / Paste section.  Reuses the SAME
//       low-level submenu builders (build_display_count_popup etc.) so
//       normal-mode vs edit-mode labels / checkmarks / clickable leaves
//       can never drift apart.
// ============================================================

// Indexed ordering of the settings block.  Every submenu builder below
// pushes one UINT entry into a `leaf_idms` vector for EACH CLICKABLE
// LEAF (not separators, not MF_POPUP root entries).  edit mode then
// captures that full ordering into `m_edit_mode_cmd_to_idm`, and
// `edit_mode_context_menu_command` translates (p_id - p_id_base) →
// that vector → IDM_* → SendMessage(WM_COMMAND, IDM_*).  Normal mode
// does the same translation but with an explicit local leaf_idms.
//
// Flat layout (indexed 0..N-1):
//   [0..3]   Display count  ▶  1 / 2 / 3 / 4 tracks        (4 leaves)
//   [4..6]   Palette        ▶  Spectrum / SoX / Mono       (3 leaves)
//   [7..9]   Axes           ▶  Freq / Time / dB            (3 leaves)
//   [10..11] Title format   ▶  Edit / Reset                (2 leaves)
//   [12..13] Language       ▶  English / Chinese           (2 leaves)
//   [14]     Refresh analysis                              (1 leaf)
static constexpr size_t kCountIdx    = 0;
static constexpr size_t kPaletteIdx  = 4;
static constexpr size_t kAxesIdx     = 7;
static constexpr size_t kTitleIdx    = 10;
static constexpr size_t kLangIdx     = 12;
static constexpr size_t kRefreshIdx  = 14;
static constexpr size_t kTotalLeaves = 15;

// Helper: narrow UTF-8 -> wide for Win32 menu APIs.
static void append_menu_i18n(HMENU hmenu, UINT flags, UINT id, strid_t sid, language_t lang) {
    pfc::stringcvt::string_wide_from_utf8 w(i18n(sid, lang));
    ::AppendMenuW(hmenu, flags, id, w);
}

static void build_display_count_popup(HMENU hmenu, unsigned base, int max_tracks,
                                      language_t lang, std::vector<UINT>& leaf_idms) {
    static const UINT s_ids[4] = { IDM_SET_COUNT_1, IDM_SET_COUNT_2, IDM_SET_COUNT_3, IDM_SET_COUNT_4 };
    static const strid_t s_sids[4] = { S_1_TRACK, S_2_TRACKS, S_3_TRACKS, S_4_TRACKS };
    PFC_ASSERT(leaf_idms.size() == kCountIdx);
    for (int i = 0; i < 4; ++i) {
        append_menu_i18n(hmenu, MF_STRING | ((i + 1) == max_tracks ? MF_CHECKED : 0),
                         base + (unsigned)leaf_idms.size(), s_sids[i], lang);
        leaf_idms.push_back(s_ids[i]);
    }
}

static void build_palette_popup(HMENU hmenu, unsigned base, palette_t current,
                                language_t lang, std::vector<UINT>& leaf_idms) {
    struct E { UINT id; strid_t sid; palette_t value; };
    static const E s_rows[3] = {
        { IDM_PALETTE_SPECTRUM, S_SPECTRUM, PALETTE_SPECTRUM },
        { IDM_PALETTE_SOX,      S_SOX,      PALETTE_SOX },
        { IDM_PALETTE_MONO,     S_MONO,     PALETTE_MONO },
    };
    PFC_ASSERT(leaf_idms.size() == kPaletteIdx);
    for (auto& r : s_rows) {
        append_menu_i18n(hmenu, MF_STRING | (current == r.value ? MF_CHECKED : 0),
                         base + (unsigned)leaf_idms.size(), r.sid, lang);
        leaf_idms.push_back(r.id);
    }
}

static void build_axes_popup(HMENU hmenu, unsigned base, bool show_freq, bool show_time,
                             bool show_db, language_t lang, std::vector<UINT>& leaf_idms) {
    PFC_ASSERT(leaf_idms.size() == kAxesIdx);
    append_menu_i18n(hmenu, MF_STRING | (show_freq ? MF_CHECKED : 0),
                     base + (unsigned)leaf_idms.size(), S_FREQ_AXIS, lang);
    leaf_idms.push_back(IDM_TOGGLE_FREQ_AXIS);
    append_menu_i18n(hmenu, MF_STRING | (show_time ? MF_CHECKED : 0),
                     base + (unsigned)leaf_idms.size(), S_TIME_AXIS, lang);
    leaf_idms.push_back(IDM_TOGGLE_TIME_AXIS);
    append_menu_i18n(hmenu, MF_STRING | (show_db ? MF_CHECKED : 0),
                     base + (unsigned)leaf_idms.size(), S_DB_SCALE, lang);
    leaf_idms.push_back(IDM_TOGGLE_DB_SCALE);
}

static void build_title_format_popup(HMENU hmenu, unsigned base,
                                     language_t lang, std::vector<UINT>& leaf_idms) {
    PFC_ASSERT(leaf_idms.size() == kTitleIdx);
    append_menu_i18n(hmenu, MF_STRING, base + (unsigned)leaf_idms.size(), S_EDIT_FORMAT, lang);
    leaf_idms.push_back(IDM_EDIT_TITLE_FORMAT);
    append_menu_i18n(hmenu, MF_STRING, base + (unsigned)leaf_idms.size(), S_RESET_DEFAULT, lang);
    leaf_idms.push_back(IDM_RESET_TITLE_FORMAT);
}

static void build_language_popup(HMENU hmenu, unsigned base, language_t current,
                                 language_t lang, std::vector<UINT>& leaf_idms) {
    PFC_ASSERT(leaf_idms.size() == kLangIdx);
    struct E { UINT id; strid_t sid; language_t value; };
    static const E s_rows[2] = {
        { IDM_LANG_ENGLISH, S_LANG_EN, LANG_EN },
        { IDM_LANG_CHINESE, S_LANG_ZH, LANG_ZH },
    };
    for (auto& r : s_rows) {
        append_menu_i18n(hmenu, MF_STRING | (current == r.value ? MF_CHECKED : 0),
                         base + (unsigned)leaf_idms.size(), r.sid, lang);
        leaf_idms.push_back(r.id);
    }
}

// Builds the full settings block into a root-level HMENU.  Returns the
// leaf id mapping (size == kTotalLeaves).
static std::vector<UINT> build_settings_block(HMENU p_menu, unsigned p_id_base,
                                              int max_tracks, palette_t palette,
                                              bool show_freq, bool show_time,
                                              bool show_db, language_t lang) {
    std::vector<UINT> leaf_idms;
    leaf_idms.reserve(kTotalLeaves);

    // Display count submenu
    {
        CMenu count_menu; WIN32_OP_D(count_menu.CreatePopupMenu() != NULL);
        build_display_count_popup(count_menu, p_id_base, max_tracks, lang, leaf_idms);
        pfc::stringcvt::string_wide_from_utf8 w(i18n(S_DISPLAY_COUNT, lang));
        WIN32_OP_D(::AppendMenuW(p_menu, MF_POPUP, (UINT_PTR)count_menu.m_hMenu, w));
        count_menu.Detach();
    }

    WIN32_OP_D(::AppendMenu(p_menu, MF_SEPARATOR, 0, _T("")));

    // Palette submenu
    {
        CMenu palette_menu; WIN32_OP_D(palette_menu.CreatePopupMenu() != NULL);
        build_palette_popup(palette_menu, p_id_base, palette, lang, leaf_idms);
        pfc::stringcvt::string_wide_from_utf8 w(i18n(S_PALETTE, lang));
        WIN32_OP_D(::AppendMenuW(p_menu, MF_POPUP, (UINT_PTR)palette_menu.m_hMenu, w));
        palette_menu.Detach();
    }

    WIN32_OP_D(::AppendMenu(p_menu, MF_SEPARATOR, 0, _T("")));

    // Axes submenu
    {
        CMenu axes_menu; WIN32_OP_D(axes_menu.CreatePopupMenu() != NULL);
        build_axes_popup(axes_menu, p_id_base, show_freq, show_time, show_db, lang, leaf_idms);
        pfc::stringcvt::string_wide_from_utf8 w(i18n(S_AXES, lang));
        WIN32_OP_D(::AppendMenuW(p_menu, MF_POPUP, (UINT_PTR)axes_menu.m_hMenu, w));
        axes_menu.Detach();
    }

    // Title format submenu
    {
        CMenu fmt_menu; WIN32_OP_D(fmt_menu.CreatePopupMenu() != NULL);
        build_title_format_popup(fmt_menu, p_id_base, lang, leaf_idms);
        pfc::stringcvt::string_wide_from_utf8 w(i18n(S_TITLE_FORMAT, lang));
        WIN32_OP_D(::AppendMenuW(p_menu, MF_POPUP, (UINT_PTR)fmt_menu.m_hMenu, w));
        fmt_menu.Detach();
    }

    WIN32_OP_D(::AppendMenu(p_menu, MF_SEPARATOR, 0, _T("")));

    // Language submenu
    {
        CMenu lang_menu; WIN32_OP_D(lang_menu.CreatePopupMenu() != NULL);
        build_language_popup(lang_menu, p_id_base, lang, lang, leaf_idms);
        pfc::stringcvt::string_wide_from_utf8 w(i18n(S_LANGUAGE, lang));
        WIN32_OP_D(::AppendMenuW(p_menu, MF_POPUP, (UINT_PTR)lang_menu.m_hMenu, w));
        lang_menu.Detach();
    }

    WIN32_OP_D(::AppendMenu(p_menu, MF_SEPARATOR, 0, _T("")));

    // Refresh analysis (flat item) — the last leaf (index kRefreshIdx).
    PFC_ASSERT(leaf_idms.size() == kRefreshIdx);
    append_menu_i18n(p_menu, MF_STRING, p_id_base + (unsigned)leaf_idms.size(), S_REFRESH, lang);
    leaf_idms.push_back(IDM_REFRESH);

    PFC_ASSERT(leaf_idms.size() == kTotalLeaves);
    return leaf_idms;
}

// ---------------------------------------------------------------------------
// edit_mode_context_menu_* — hooks called BY the host's standard_edit_context_menu
// ---------------------------------------------------------------------------

bool SpectrumCompareWindow::edit_mode_context_menu_test(const POINT& /*p_point*/, bool /*p_fromkeyboard*/) {
    // We always have settings to offer — our panel is one large client
    // area with no "dead" subregions where settings would be inapplicable.
    return true;
}

void SpectrumCompareWindow::edit_mode_context_menu_build(const POINT& /*p_point*/, bool /*p_fromkeyboard*/, HMENU p_menu, unsigned p_id_base) {
    // -------------------------------------------------------------------
    // Defensive "trim trailing separators" step.
    //
    // The SDK's standard_edit_context_menu() can leave more than one
    // MF_SEPARATOR at the tail of the menu before invoking our hook.
    // Specifically, the code path in ui_element_helpers.cpp does:
    //
    //   line 143  → AppendMenu(MF_SEPARATOR)          (before Cut/Copy/Paste)
    //   line 146  → Cut UI Element
    //   line 147  → Copy UI Element
    //   line 148  → Paste UI Element
    //   line 154  → AppendMenu(MF_SEPARATOR)          // if host_test==true
    //   line 156  → host_edit_mode_context_menu_build
    //   line 161  → AppendMenu(MF_SEPARATOR)          // before our hook
    //   line 163  → edit_mode_context_menu_build(THIS)
    //
    // Cases that can produce double separators between "Paste UI Element"
    // and our first "Display count" submenu:
    //
    //  (A) host_edit_mode_context_menu_test returns true AND the host
    //      hook appends zero clickable items — line 154's separator is
    //      orphaned, adjacent to line 161's separator ⇒ 2 blank lines.
    //
    //  (B) Previous versions of *this* function prepended yet another
    //      MF_SEPARATOR, stacking 3 in total — the current function
    //      never does that, but the SDK's double-case (A) is enough
    //      by itself to reproduce the user-reported visual bug.
    //
    // Fix: collapse any run of trailing MF_SEPARATOR entries down to
    // exactly **one** before we append our settings block.  This is
    // safe because a trailing separator at the very end of a menu is
    // never rendered anyway, and between the SDK section and the
    // settings block we want exactly one visual gap.
    // -------------------------------------------------------------------
    {
        int count = ::GetMenuItemCount(p_menu);
        // Strip trailing MF_SEPARATOR entries until the last item is NOT
        // a separator (or the menu is empty, should never happen here).
        while (count > 0) {
            const int lastIdx = count - 1;
            MENUITEMINFO mii;
            memset(&mii, 0, sizeof(mii));
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_FTYPE;
            if (!::GetMenuItemInfo(p_menu, (UINT)lastIdx, TRUE, &mii)) break;
            if ((mii.fType & MFT_SEPARATOR) == 0) break; // last entry not a separator → done
            // Drop this orphan trailing separator.
            ::DeleteMenu(p_menu, (UINT)lastIdx, MF_BYPOSITION);
            --count;
        }
        // Now add back exactly ONE separator between "Paste UI Element /
        // last host custom item" and our settings block.
        if (count > 0) {
            // Only add if the menu already has real entries (it always
            // does because Replace/Cut/Copy/Paste are guaranteed).
            WIN32_OP_D(::AppendMenu(p_menu, MF_SEPARATOR, 0, _T("")));
        }
    }

    m_edit_mode_cmd_to_idm = build_settings_block(
        p_menu, p_id_base, m_max_tracks, m_palette,
        m_show_freq_axis, m_show_time_axis, m_show_db_scale, m_language);
}

void SpectrumCompareWindow::edit_mode_context_menu_command(
    const POINT& /*p_point*/, bool /*p_fromkeyboard*/, unsigned p_id, unsigned p_id_base)
{
    if ((int)p_id < (int)p_id_base) return;
    const size_t idx = (size_t)(p_id - p_id_base);
    if (idx >= m_edit_mode_cmd_to_idm.size()) return;
    const UINT idm = m_edit_mode_cmd_to_idm[idx];
    if (idm == 0) return; // should never happen for our block

    // Send through the normal WM_COMMAND path so every handler keeps
    // its existing invariants (window-created guards, cfg_* writes,
    // selection-snapshot invalidation, analysis aborts on refresh, …).
    if (IsWindow()) {
        SendMessage(WM_COMMAND, MAKEWPARAM(idm, 0), 0);
    }
}

// ---------------------------------------------------------------------------
// Normal-mode entry point
// ---------------------------------------------------------------------------

void SpectrumCompareWindow::OnContextMenu(CWindow wnd, CPoint point) {
    (void)wnd;

    if (m_callback->is_edit_mode_enabled()) {
        // Leave WM_CONTEXTMENU on the default routing path. The host
        // catches it, invokes standard_edit_context_menu() → which calls
        // our edit_mode_context_menu_{test,build,command} hooks → the
        // final shown menu combines Replace / Cut / Copy / Paste (host
        // driven, real slot id) with our full settings block.
        SetMsgHandled(FALSE);
        return;
    }

    CMenu menu;
    WIN32_OP(menu.CreatePopupMenu() != NULL);

    // Build settings block with id_base = 0, so each leaf's cmd id is a
    // 0-based index we can look up in `leaf_idms`.
    const std::vector<UINT> leaf_idms = build_settings_block(
        menu, 0, m_max_tracks, m_palette,
        m_show_freq_axis, m_show_time_axis, m_show_db_scale, m_language);

    CPoint ptShow = point;
    if (ptShow.x == -1 && ptShow.y == -1) {
        CRect rc;
        GetWindowRect(&rc);
        ptShow = rc.CenterPoint();
    }

    const int cmd = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        ptShow.x, ptShow.y,
        0, m_hWnd, NULL
    );

    if (cmd > 0) {
        const size_t idx = (size_t)cmd;
        if (idx < leaf_idms.size() && leaf_idms[idx] != 0) {
            SendMessage(WM_COMMAND, MAKEWPARAM(leaf_idms[idx], 0), 0);
        }
    }
}

void SpectrumCompareWindow::OnSetCount(UINT uNotifyCode, int nID, CWindow wndCtl) {
    int new_count = 0;
    switch (nID) {
    case IDM_SET_COUNT_1: new_count = 1; break;
    case IDM_SET_COUNT_2: new_count = 2; break;
    case IDM_SET_COUNT_3: new_count = 3; break;
    case IDM_SET_COUNT_4: new_count = 4; break;
    }
    if (new_count > 0 && new_count != m_max_tracks) {
        m_max_tracks = new_count;
        g_cfg_max_tracks = (int64_t)new_count;
        update_selection();
    }
}

void SpectrumCompareWindow::OnRefresh(UINT uNotifyCode, int nID, CWindow wndCtl) {
    (void)uNotifyCode; (void)nID; (void)wndCtl;
    // Mark all tracks as needing re-analysis
    {
        std::lock_guard<std::mutex> lock(m_tracks_mutex);
        for (auto& t : m_tracks) {
            // Abort any in-flight analysis so we can restart
            if (t->analyzing) {
                try { t->abort.abort(); } catch (...) {}
            }
            t->data.ready = false;
            t->data.error = false;
        }
    }

    // Invalidate the selection-snapshot cache so the next update_selection()
    // call (or the manual loop below) will actually restart analysis. Without
    // this, update_selection() would see identical handles and short-circuit
    // while data.ready stays false → "refresh analysis" looks like a no-op.
    m_last_selection = last_selection_key{};
    // Brief wait for in-flight threads to observe abort and mark analyzing=false.
    // Then reset the abort state by creating new TrackSpectrum entries is overkill;
    // instead we just wait for analyzing to clear.
    for (int i = 0; i < 100; i++) {
        bool any_running = false;
        {
            std::lock_guard<std::mutex> lock(m_tracks_mutex);
            for (auto& t : m_tracks) {
                if (t->analyzing) { any_running = true; break; }
            }
        }
        if (!any_running) break;
        Sleep(10);
    }
    // Now we cannot reuse the aborted abort_callback_impl (it's permanently aborted).
    // Rebuild the track list with fresh TrackSpectrum objects so abort state is clean.
    {
        std::lock_guard<std::mutex> lock(m_tracks_mutex);
        std::vector<std::shared_ptr<TrackSpectrum>> fresh;
        for (auto& t : m_tracks) {
            auto nt = std::make_shared<TrackSpectrum>();
            nt->handle = t->handle;
            // Preserve already-loaded data? No, refresh means re-analyze.
            fresh.push_back(nt);
        }
        m_tracks = std::move(fresh);
    }
    // Start analysis (outside lock; start_analysis_for_track takes the lock itself)
    size_t n = m_tracks.size();
    for (size_t i = 0; i < n; i++) {
        start_analysis_for_track(i);
    }
    Invalidate();
}

void SpectrumCompareWindow::OnPalette(UINT uNotifyCode, int nID, CWindow wndCtl) {
    (void)uNotifyCode; (void)wndCtl;
    switch (nID) {
    case IDM_PALETTE_SPECTRUM: m_palette = PALETTE_SPECTRUM; break;
    case IDM_PALETTE_SOX:      m_palette = PALETTE_SOX;      break;
    case IDM_PALETTE_MONO:     m_palette = PALETTE_MONO;     break;
    default: return;
    }
    g_cfg_palette = (int64_t)m_palette;
    Invalidate();
}

// ============================================================
// Axis and title format toggles
// ============================================================

void SpectrumCompareWindow::OnToggleFreqAxis(UINT uNotifyCode, int nID, CWindow wndCtl) {
    m_show_freq_axis = !m_show_freq_axis;
    g_cfg_show_freq_axis = m_show_freq_axis;
    Invalidate();
}

void SpectrumCompareWindow::OnToggleTimeAxis(UINT uNotifyCode, int nID, CWindow wndCtl) {
    m_show_time_axis = !m_show_time_axis;
    g_cfg_show_time_axis = m_show_time_axis;
    Invalidate();
}

void SpectrumCompareWindow::OnToggleDbScale(UINT uNotifyCode, int nID, CWindow wndCtl) {
    (void)uNotifyCode; (void)nID; (void)wndCtl;
    m_show_db_scale = !m_show_db_scale;
    g_cfg_show_db_scale = m_show_db_scale;
    Invalidate();
}

// ============================================================
// Inline title-format editing (right-click menu only)
// ------------------------------------------------------------
// Entry point: right-click -> Title format -> Edit format...
// Double-click was intentionally removed at user request so only the
// explicit menu path creates the editor.
//   * Enter -> commit; Esc -> discard; losing focus -> commit.
//   * Enter/Esc is captured at BOTH the WM_KEYDOWN and WM_CHAR levels so
//     message preprocessing / translation differences across foobar2000
//     window host implementations can't drop the keystroke.
//   * A per-instance `m_edit_finish_pending` flag turns the key capture and
//     the subsequent WM_KILLFOCUS into a single finish action.
// ============================================================

LRESULT CALLBACK SpectrumCompareWindow::title_edit_subclass_proc(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    (void)uIdSubclass;
    SpectrumCompareWindow* self = (SpectrumCompareWindow*)dwRefData;

    switch (uMsg) {
    // ------------------------------------------------------------------
    // Critical: tell any dialog-style message loop (foobar2000 host uses
    // IsDialogMessage / pretranslate_message) that THIS edit control wants
    // to receive *all* keyboard input — including VK_RETURN, VK_ESCAPE and
    // VK_TAB — instead of having it swallowed by the dialog manager before
    // it ever reaches our WM_KEYDOWN / WM_CHAR handlers.
    //
    // This mirrors foo_uie_playlist_tree's edit_subclass_t (EditSubclass.h):
    //   return DLGC_WANTALLKEYS | CallWindowProc(OldWndProc, ...)
    // Without it, the host will intercept VK_RETURN on our behalf and the
    // in-place edit never commits on Enter.
    // ------------------------------------------------------------------
    case WM_GETDLGCODE: {
        const LRESULT base = DefSubclassProc(hWnd, uMsg, wParam, lParam);
        return (base | DLGC_WANTALLKEYS);
    }
    case WM_KEYDOWN:
    case WM_CHAR: {
        bool commit = false;
        bool cancel = false;
        if (uMsg == WM_KEYDOWN) {
            if (wParam == VK_RETURN) commit = true;
            else if (wParam == VK_ESCAPE) cancel = true;
        } else { // WM_CHAR
            if (wParam == VK_RETURN) commit = true;
            else if (wParam == VK_ESCAPE) cancel = true;
        }
        if (commit || cancel) {
            if (self != NULL && !self->m_edit_finish_pending
                && self->m_hwnd_title_edit == hWnd)
            {
                self->m_edit_finish_pending = true;
                self->m_edit_commit = commit;
                // Use a timer instead of PostMessage to avoid the foobar2000
                // host's pretranslate_message layer intercepting WM_USER
                // messages (which is why Enter previously did nothing).
                // WM_TIMER goes through the standard dispatch path and is
                // already in the message map.
                self->SetTimer(self->TIMER_END_EDIT, 10, nullptr);
            }
            // Do NOT call DefSubclassProc for these keys. We don't want the
            // EDIT to beep on Enter (ES_WANTRETURN is off by default) and we
            // definitely don't want Esc to be re-processed after we've asked
            // for the control to be torn down.
            return 0;
        }
        break;
    }
    case WM_KILLFOCUS: {
        HWND newFocus = (HWND)wParam;
        if (self != NULL && self->m_hwnd_title_edit == hWnd
            && !self->m_edit_finish_pending && newFocus != hWnd)
        {
            self->m_edit_finish_pending = true;
            self->m_edit_commit = true; // commit on focus loss
            self->SetTimer(self->TIMER_END_EDIT, 10, nullptr);
        }
        break;
    }
    case WM_NCDESTROY:
        // Must remove the subclass before the HWND goes away.
        ::RemoveWindowSubclass(hWnd, title_edit_subclass_proc, 1);
        break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

void SpectrumCompareWindow::begin_inline_title_format_edit() {
    if (m_hWnd == NULL) return;
    if (m_hwnd_title_edit != NULL) {
        // Already editing: refocus and select all.
        ::SetFocus(m_hwnd_title_edit);
        SendMessage(m_hwnd_title_edit, EM_SETSEL, 0, -1);
        return;
    }

    // Reproduce the label-rect layout from OnPaint so the edit sits exactly
    // on top of the first track's title line, with left edge aligned to the
    // spectrum (same fix that prevents overlap with the 22kHz axis).
    CRect rc;
    GetClientRect(&rc);
    int dpi = m_dpi;
    auto scale = [dpi](int v) -> int { return MulDiv(v, dpi, 96); };

    const int padding_outer = scale(8);
    const int label_height = scale(22);
    const int freq_axis_width = m_show_freq_axis ? scale(48) : 0;
    const int time_axis_height = m_show_time_axis ? scale(20) : 0;

    int track_count = 0;
    {
        std::lock_guard<std::mutex> lock(m_tracks_mutex);
        track_count = (int)m_tracks.size();
    }
    if (track_count < 1) track_count = 1;
    const int track_gap = scale(6);
    int total_track_gap = track_gap * (track_count - 1);
    int available_height = rc.Height() - 2 * padding_outer - total_track_gap;
    int track_height = available_height / track_count;
    const int min_track_height = label_height + time_axis_height + scale(20);
    if (track_height < min_track_height) track_height = min_track_height;

    int track_left = rc.left + padding_outer;
    int track_right = rc.right - padding_outer;
    int y = rc.top + padding_outer;
    int spec_left = track_left + freq_axis_width;
    if (spec_left >= track_right) spec_left = track_left;

    CRect edit_rc(spec_left, y, track_right, y + label_height);

    DWORD style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP
                | ES_AUTOHSCROLL | ES_NOHIDESEL;
    DWORD exStyle = WS_EX_CLIENTEDGE;
    HWND hedit = CreateWindowExW(
        exStyle,
        L"EDIT",
        NULL,
        style,
        edit_rc.left, edit_rc.top, edit_rc.Width(), edit_rc.Height(),
        m_hWnd,
        (HMENU)IDC_INLINE_TITLE_EDIT,
        core_api::get_my_instance(),
        NULL
    );
    if (hedit == NULL) return;

    HFONT hFont = (HFONT)m_callback->query_font_ex(ui_font_default);
    if (hFont == NULL) hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessage(hedit, WM_SETFONT, (WPARAM)hFont, TRUE);

    pfc::stringcvt::string_wide_from_utf8 wtext(m_title_format.get_ptr());
    ::SetWindowTextW(hedit, wtext);
    SendMessage(hedit, EM_SETSEL, 0, -1);

    m_edit_finish_pending = false;
    ::SetWindowSubclass(hedit, title_edit_subclass_proc, 1, (DWORD_PTR)this);
    m_hwnd_title_edit = hedit;
    ::SetFocus(hedit);
}

void SpectrumCompareWindow::end_inline_title_format_edit(bool commit) {
    if (m_hwnd_title_edit == NULL) {
        m_edit_finish_pending = false;
        return;
    }
    HWND hedit = m_hwnd_title_edit;

    pfc::string8 newFormat;
    if (commit) {
        int len = ::GetWindowTextLengthW(hedit);
        pfc::array_t<wchar_t> buf;
        buf.set_size(len + 1);
        ::GetWindowTextW(hedit, buf.get_ptr(), (int)buf.get_size());
        newFormat = pfc::stringcvt::string_utf8_from_wide(buf.get_ptr());
    }

    // Tear down in a defined order. Setting m_hwnd_title_edit to NULL first
    // stops the KILLFOCUS path in the subclass from firing re-entrantly, and
    // the separate pending-flag means WM_KILLFOCUS + a key handler in flight
    // can't cause double-teardown either.
    m_hwnd_title_edit = NULL;
    m_edit_finish_pending = false;
    ::RemoveWindowSubclass(hedit, title_edit_subclass_proc, 1);
    ::DestroyWindow(hedit);

    if (commit) {
        if (newFormat != m_title_format) {
            m_title_format = newFormat;
            g_cfg_title_format.set(m_title_format);
            m_analyzer.set_title_format(m_title_format.c_str());
            refresh_track_titles();
        } else {
            Invalidate();
        }
    } else {
        Invalidate();
    }
}

void SpectrumCompareWindow::OnSetFocus(CWindow wndOld) {
    (void)wndOld;
    // If an inline editor is alive, forward focus to it. This also avoids a
    // WM_KILLFOCUS -> commit cascade when foobar2000's host re-focuses the
    // parent panel during a layout pass.
    if (m_hwnd_title_edit != NULL && ::IsWindow(m_hwnd_title_edit)) {
        ::SetFocus(m_hwnd_title_edit);
    }
}

void SpectrumCompareWindow::OnEditTitleFormat(UINT uNotifyCode, int nID, CWindow wndCtl) {
    (void)uNotifyCode; (void)nID; (void)wndCtl;
    begin_inline_title_format_edit();
}

void SpectrumCompareWindow::OnResetTitleFormat(UINT uNotifyCode, int nID, CWindow wndCtl) {
    (void)uNotifyCode; (void)nID; (void)wndCtl;
    // Close any open inline editor first (discard its text) so uncommitted
    // edits never appear to outlive a "reset to default" action.
    if (m_hwnd_title_edit != NULL) {
        end_inline_title_format_edit(false);
    }
    m_title_format = DEFAULT_TITLE_FORMAT;
    g_cfg_title_format.set(m_title_format);
    m_analyzer.set_title_format(m_title_format.c_str());
    refresh_track_titles();
}

void SpectrumCompareWindow::OnLanguage(UINT uNotifyCode, int nID, CWindow wndCtl) {
    (void)uNotifyCode; (void)wndCtl;
    language_t new_lang = (nID == IDM_LANG_CHINESE) ? LANG_ZH : LANG_EN;
    if (new_lang == m_language) return;
    m_language = new_lang;
    g_cfg_language = (int64_t)new_lang;
    // Language only affects menu/label text — no need to re-analyze.
    // Just repaint so "Analyzing..." / placeholder text updates.
    if (IsWindow()) Invalidate();
}

// ============================================================
// Parent (popup host) subclass
// ------------------------------------------------------------
// When KFlagHavePopupCommand is set, foobar2000 wraps our element in a
// popup host window. That host adds "Export settings", "Configure" etc.
// to the system menu (title bar right-click). We subclass the parent
// to intercept WM_INITMENUPOPUP and strip non-standard items so the
// title bar right-click only shows Move / Size / Close etc.
// ============================================================

void SpectrumCompareWindow::subclass_parent_window() {
    if (m_hWnd == NULL) return;
    HWND parent = ::GetParent(m_hWnd);
    if (parent == NULL || parent == m_hWnd) return;

    // Avoid double-subclassing if initialize_window is called again.
    if (m_hwnd_parent == parent) return;

    // If we previously subclassed a different parent, clean it up.
    if (m_hwnd_parent != NULL && ::IsWindow(m_hwnd_parent)) {
        ::RemoveWindowSubclass(m_hwnd_parent, parent_subclass_proc, IDC_PARENT_SUBCLASS);
    }

    m_hwnd_parent = parent;
    ::SetWindowSubclass(parent, parent_subclass_proc, IDC_PARENT_SUBCLASS, (DWORD_PTR)this);
}

LRESULT CALLBACK SpectrumCompareWindow::parent_subclass_proc(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    (void)uIdSubclass;
    SpectrumCompareWindow* self = (SpectrumCompareWindow*)dwRefData;

    switch (uMsg) {
    case WM_INITMENUPOPUP: {
        HMENU menu = (HMENU)wParam;
        BOOL is_sys_menu = (BOOL)HIWORD(lParam);
        if (is_sys_menu && menu != NULL) {
            // Walk the menu backwards and remove every item whose ID is NOT
            // one of the standard system menu commands.  foobar2000's popup
            // host inserts "Export settings", "Configure" etc. with custom IDs
            // outside the SC_* range (0xF000+).
            int count = ::GetMenuItemCount(menu);
            for (int i = count - 1; i >= 0; --i) {
                UINT id = ::GetMenuItemID(menu, i);
                // 0xFFFFFFFF = submenu placeholder or separator — keep.
                if (id == 0xFFFFFFFF) continue;
                // Keep only standard SC_* system menu items.
                if (id >= 0xF000 && id <= 0xFFF0) continue;
                ::DeleteMenu(menu, i, MF_BYPOSITION);
            }
        }
        break;
    }
    case WM_NCDESTROY:
        // Remove subclass before the HWND is destroyed.
        ::RemoveWindowSubclass(hWnd, parent_subclass_proc, IDC_PARENT_SUBCLASS);
        if (self != NULL) self->m_hwnd_parent = NULL;
        break;
    }
    return ::DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// ============================================================
// Component registration
// ============================================================

namespace {
    // Custom ui_element_impl: enable KFlagHavePopupCommand so foobar2000's UI
    // backend generates a menu entry under View → Utility that activates /
    // pops up the panel. KFlagPopupCommandHidden makes this entry hidden by
    // default; users must hold Shift while opening the menu to see it.
    // The popup host's system menu is cleaned up via subclass_parent_window()
    // to remove "Export settings" etc. that the host adds by default.
    class ui_element_spectrum_compare :
        public ui_element_impl<ImplementBumpableElem<SpectrumCompareWindow>, ui_element_v2>
    {
    public:
        t_uint32 get_flags() override {
            return ui_element_v2::KFlagHavePopupCommand
                 | ui_element_v2::KFlagPopupCommandHidden
                 | ui_element_v2::KFlagSupportsBump;
        }
        bool bump() override { return ImplementBumpableElem<SpectrumCompareWindow>::Bump(); }
    };
    static service_factory_single_t<ui_element_spectrum_compare> g_spectrum_compare_factory;

    // Component version info
    DECLARE_COMPONENT_VERSION(
        "Spectrum Compare",
        "1.2",
        "Vertical spectrogram comparison panel for selected tracks. Spek-style coloring.\n\n"
        "Authors: TRAE AI Coding Assistant, always beta, Asion\n\n"
        "Select one or more tracks in the playlist to view their spectrograms.\n"
        "Right-click to set display count (1-4), palette, axes, or refresh.\n"
        "Useful for comparing audio quality and frequency content across tracks."
    );
}
