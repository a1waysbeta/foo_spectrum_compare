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
cfg_bool g_cfg_pseudo_transparency(guid_cfg_pseudo_transparency, false);

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
    m_pseudo_transparency = g_cfg_pseudo_transparency;
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
    // R3：200ms → 33ms（约 30fps）。
    // 原来必须用 200ms 是因为 render_spectrum 每帧重算整幅 W×H 像素，
    // 刷太快会把 GUI 线程打满。R2 加了位图缓存后单帧只算新增的几列，
    // 成本降了一两个数量级，才有条件把刷新率提到肉眼连续的水平。
    // 定时器只在真有轨道置了 needs_repaint 时才 Invalidate，
    // 空闲时这个回调就是一次加锁 + 遍历 ≤4 个元素，可以忽略。
    SetTimer(TIMER_REPAINT, 33, NULL);

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
//   uint32_t version         ; 4 (v1 = no language, v2 = no db_scale,
//                                    v3 = no pseudo_transparency)
//   uint32_t max_tracks      ; 1..4
//   uint32_t palette         ; palette_t (0..PALETTE_COUNT-1)
//   uint8_t  show_freq_axis  ; 0 / 1
//   uint8_t  show_time_axis  ; 0 / 1
//   string8  title_format    ; raw bytes (UTF-8 titleformat expression)
//   uint8_t  language        ; language_t (0=EN, 1=ZH)  [v2+]
//   uint8_t  show_db_scale   ; 0 / 1                      [v3+]
//   uint8_t  pseudo_transparency ; 0 / 1                  [v4+]
//
// v1 readers stop after title_format and ignore the language byte,
// which is fine — language defaults to English on old .fth files.
// v2 readers stop after language and ignore show_db_scale, which
// defaults to true (dB axis visible).
// v3 readers stop after show_db_scale and ignore pseudo_transparency,
// which defaults to false (opaque background).
// ============================================================
static constexpr uint32_t kConfigMagicVersion = 4u;
static constexpr int        kMinMaxTracks = 1;
static constexpr int        kMaxMaxTracks = 4;

static ui_element_config::ptr build_instance_config(
    int max_tracks,
    palette_t palette,
    bool show_freq_axis,
    bool show_time_axis,
    const char* title_format,
    language_t language,
    bool show_db_scale,
    bool pseudo_transparency)
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
    b << (uint8_t)(pseudo_transparency ? 1u : 0u);
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
    bool new_pseudo = m_pseudo_transparency;
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
                // v4+ adds pseudo_transparency after show_db_scale.
                if (version >= 4u) {
                    uint8_t pt = 0;
                    parser >> pt;
                    new_pseudo = (pt != 0);
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
    if (new_pseudo != m_pseudo_transparency) {
        m_pseudo_transparency = new_pseudo;
        g_cfg_pseudo_transparency = new_pseudo;
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
        m_title_format.get_ptr(), m_language, m_show_db_scale,
        m_pseudo_transparency);
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
        (bool)g_cfg_show_db_scale,
        (bool)g_cfg_pseudo_transparency);
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

    // ================================================================
    // 渐进发布 —— 对应 Spek 的 pipeline_cb → on_have_sample 那一环
    // ================================================================
    //
    // Spek 的做法：worker 每算完一个像素列就 wxPostEvent 一个
    // SpekHaveSampleEvent，GUI 线程在 on_have_sample 里把这一列写进
    // 常驻的 wxImage 然后 Refresh()（spek-spectrogram.cc:178）。
    // 因为它「一列 == 一个屏幕像素列」，单列事件的代价极小。
    //
    // 我们不能照抄「一列一次通知」：
    //   1) 我们一轨固定 800 列，但屏幕上一轨往往只有几百像素高、
    //      几百像素宽，源列和像素列不是一一对应；单列通知没有意义，
    //      而 800 次 Invalidate 的消息往返本身就够贵。
    //   2) 最多 4 轨并行分析，通知量再乘 4。
    //
    // 所以这里做两级节流：
    //   * 时间节流：至多每 publish_interval_ms 加锁发布一次，
    //     把 800 次回调压到几十次；
    //   * 帧合并：发布时只置 needs_repaint，由 TIMER_REPAINT
    //     统一合并成一次 Invalidate（见 OnTimer）。多轨同时在跑时，
    //     它们的进度会自动并到同一帧里，不会互相放大重绘次数。
    //     真正结束时才 PostMessage(WM_SPECTRUM_READY) 立即刷新。
    //   （R2 之前还有第三个理由：render_spectrum 每帧重算整幅 W×H
    //     像素，刷新率根本提不上去。现在位图常驻、每帧只算新增列，
    //     这个限制已经解除，两级间隔才敢从 100/200ms 降到 33/33ms。）
    //
    // 数据只做**增量追加**：dst 已有 k 列就只拷 [k, frames_done) 这一段，
    // 整轨累计拷贝量等于一次全量拷贝（约 800*1025*4B ≈ 3.3MB），
    // 而不是每次都全量重拷。
    uint64_t last_publish_tick = 0;
    // R3：100ms → 33ms。这是分析线程向 GUI 交付新列的最小间隔，
    // 和 TIMER_REPAINT 的 33ms 对齐，两级节流合起来才是真实帧率
    // （改造前 100ms 发布 × 200ms 定时器 ≈ 5fps，所以看起来一块一块蹦）。
    // 交付本身很便宜（一次 memcpy 式 insert），贵的是重绘，
    // 而重绘成本已经被 R2 的位图缓存压到只算新增列。
    const uint64_t publish_interval_ms = 33;
    bool header_published = false;

    auto publish_partial = [&](int frames_done, int frames_total) {
        (void)frames_total;
        if (m_shutdown) return;
        if (frames_done <= 0 || data.fft_bins <= 0) return;

        const uint64_t now = GetTickCount64();
        if (header_published && (now - last_publish_tick) < publish_interval_ms) return;
        last_publish_tick = now;

        std::lock_guard<std::mutex> lock(m_tracks_mutex);
        if (m_shutdown) return;

        // target 由 shared_ptr 保活，写它永远安全；但只有它还在
        // m_tracks 里时才值得触发重绘（OnRefresh 会整体换掉对象，
        // 那种情况下旧线程的进度不该再上屏）。
        bool still_listed = false;
        for (auto& t : m_tracks) {
            if (t.get() == target.get()) { still_listed = true; break; }
        }
        if (!still_listed) return;

        SpectrumData& dst = target->data;

        if (!header_published) {
            header_published = true;
            // 复用的 TrackSpectrum 可能残留上一轮的结果/错误，先清干净，
            // 否则 OnPaint 会走进 error 分支或拿旧列数当分母。
            dst = SpectrumData();
            dst.track_path = data.track_path;
            dst.title = data.title;
            dst.sample_rate = data.sample_rate;
            dst.channels = data.channels;
            dst.duration = data.duration;
            dst.fft_size = data.fft_size;
            dst.fft_bins = data.fft_bins;
            dst.hop_size = data.hop_size;
            dst.window = data.window;
            dst.data.reserve((size_t)data.total_frames * (size_t)data.fft_bins);
        }
        // total_frames 是渲染端的时间轴分母，分析末尾会收敛到真实列数，
        // 所以每次都同步。
        dst.total_frames = data.total_frames;

        const size_t nb = (size_t)data.fft_bins;
        const size_t want = (size_t)frames_done * nb;
        const size_t have = dst.data.size();
        if (want > have && want <= data.data.size()) {
            dst.data.insert(dst.data.end(),
                            data.data.begin() + (ptrdiff_t)have,
                            data.data.begin() + (ptrdiff_t)want);
        }
        dst.time_frames = (int)(dst.data.size() / nb);
        target->needs_repaint = true;
    };

    try {
        m_analyzer.analyze(handle, data, target->abort, publish_partial);
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

    // Fill background — pseudo-transparency asks the parent window to
    // paint its own background into our memory DC, so any spectrum-free
    // areas show the parent's background instead of a flat colour.
    // Technique borrowed from foo_spider_monkey_panel: map our origin
    // into parent coordinates, send WM_ERASEBKGND to the parent with
    // our DC, then restore the origin.  When off (or no parent), fall
    // back to the standard fb2k background colour fill.
    if (m_pseudo_transparency) {
        HWND parent = ::GetParent(m_hWnd);
        if (parent) {
            POINT tl = { 0, 0 };
            POINT prev_org;
            ::MapWindowPoints(m_hWnd, parent, &tl, 1);
            ::OffsetWindowOrgEx(dc, tl.x, tl.y, &prev_org);
            ::SendMessage(parent, WM_ERASEBKGND, (WPARAM)(HDC)dc, 0);
            ::SetWindowOrgEx(dc, prev_org.x, prev_org.y, nullptr);
        } else {
            COLORREF bg_color = m_callback->query_std_color(ui_color_background);
            CBrush bg_brush;
            bg_brush.CreateSolidBrush(bg_color);
            dc.FillRect(&rc, bg_brush);
        }
    } else {
        COLORREF bg_color = m_callback->query_std_color(ui_color_background);
        CBrush bg_brush;
        bg_brush.CreateSolidBrush(bg_color);
        dc.FillRect(&rc, bg_brush);
    }

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
    // 频率轴宽度：容纳最宽的标签 "000 kHz" + 刻度线 + 间隙。
    // 从 48 提到 60 是因为标签加了 " kHz" 单位（对齐 Spek）。
    // 注意：这个值在 OnLButtonDown 的命中测试里还有一份，必须同步。
    const int freq_axis_width = m_show_freq_axis ? scale(60) : 0;
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

        // 绘制频谱或状态。
        //
        // 【渐进显示的分支改造】
        // 旧逻辑只在 data.ready 时才画频谱，分析中一律显示 "Analyzing..."，
        // 于是 DSD64 这类高采样素材要空等十几秒。
        // 现在只要已经有列（time_frames > 0）就先把左侧画出来，
        // 右侧还没算到的区域保留背景，并在那片空白里显示进度提示，
        // 这样用户从第一秒就能看到频谱从左往右生长。
        const SpectrumData& tdata = m_tracks[i]->data;
        const bool has_partial = (tdata.time_frames > 0 && tdata.fft_bins > 0 && !tdata.error);

        if (tdata.error) {
            dc.SetTextColor(RGB(255, 80, 80));
            dc.SetBkMode(TRANSPARENT);
            SelectObjectScope fontScope(dc, (HGDIOBJ)m_callback->query_font_ex(ui_font_default));
            pfc::string8 err;
            err << i18n(S_ERROR, m_language) << tdata.error_msg.c_str();
            pfc::stringcvt::string_wide_from_utf8 err_w(err);
            dc.DrawText(err_w, -1, &spec_rc, DT_NOPREFIX | DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (has_partial) {
            int drawn = render_spectrum(dc.m_hDC, spec_rc, *m_tracks[i]);
            // 尚未画到的右侧区域：分析还在跑就在里面居中显示 "Analyzing..."。
            if (drawn < spec_rc.Width() && m_tracks[i]->analyzing) {
                CRect rest_rc(spec_rc.left + drawn, spec_rc.top, spec_rc.right, spec_rc.bottom);
                dc.SetTextColor(m_callback->query_std_color(ui_color_text));
                dc.SetBkMode(TRANSPARENT);
                SelectObjectScope fontScope(dc, (HGDIOBJ)m_callback->query_font_ex(ui_font_default));
                pfc::stringcvt::string_wide_from_utf8 w_analyzing(i18n(S_ANALYZING, m_language));
                dc.DrawText(w_analyzing, -1, &rest_rc,
                            DT_NOPREFIX | DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
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
    // Draw label background — skip when pseudo-transparency is on, since
    // the parent's background was already painted into our memory DC in
    // OnPaint.  Filling here would overwrite it with ui_color_background
    // (foobar2000 theme panel colour), which differs from the parent's
    // actual background and produces a visible colour mismatch (e.g.
    // "light green" vs "green" when the host background is green).
    if (!m_pseudo_transparency) {
        COLORREF bg = m_callback->query_std_color(ui_color_background);
        CBrush brush;
        brush.CreateSolidBrush(bg);
        dc.FillRect(&rc, brush);
    }

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

int SpectrumCompareWindow::render_spectrum(CDCHandle dc, const RECT& rc, TrackSpectrum& track) {
    const SpectrumData& data = track.data;
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    if (width <= 0 || height <= 0 || data.time_frames <= 0 || data.fft_bins <= 0) return 0;

    // ==============================================================
    // 渐进显示：只绘制已经算出来的左侧部分
    // ==============================================================
    //
    // 【为什么要区分 avail 和 total】
    // 分析线程边算边交付（见 SpectrumAnalyzer::progress_cb），所以进来时
    // data.time_frames 可能只有计划列数的一小部分。
    //
    // 时间轴映射必须用**计划总列数** total 当分母，而不是当前已有的
    // avail。否则每来一批新列，同一个源列就会被映射到不同的像素 x，
    // 画面会像手风琴一样被反复拉伸 —— 用户看到的是整幅图不停横向缩放，
    // 而不是从左往右生长。用 total 当分母后，第 k 列永远落在同一个像素
    // 位置上，新数据只是往右接续，已画出的部分完全静止。
    //
    // 这也是我们和 Spek 的设计分歧点：Spek 让「一列 == 一个屏幕像素列」
    // （samples = 面板宽度），天然不存在这个问题，但代价是改变窗口宽度
    // 必须整轨重新分析。我们选择保留重采样，换取改窗口大小零成本。
    const int total = data.total_frames > 0 ? data.total_frames : data.time_frames;
    int avail = data.time_frames;
    if (avail > total) avail = total;
    if (avail <= 0) return 0;

    // 已算出的列对应到多少像素宽。avail == total 时必须正好等于 width，
    // 否则分析结束后右侧会残留一条永久空白。
    int draw_width = (avail >= total)
        ? width
        : (int)((int64_t)width * (int64_t)avail / (int64_t)total);
    if (draw_width < 1) draw_width = 1;
    if (draw_width > width) draw_width = width;

    // ==============================================================
    // R2：常驻位图缓存 —— 每帧只计算「新长出来」的那几列
    // ==============================================================
    //
    // 【改造前的问题】
    // 这里原本每次调用都 CreateDIBSection 新建位图，然后把整幅
    // draw_width × height 重算一遍。每个像素含一次 log10f + 一次
    // palette 插值，外加对源数据的区间面积平均求和 —— 单帧成本
    // O(W×H)。分析过程中要重绘几十次，同一批像素被反复算了几十遍。
    // 这就是「渐进显示不丝滑」的真正根因：单帧太贵 → 只能用
    // 100ms 发布 + 200ms 定时器硬压到约 5fps → 看起来一块一块蹦。
    //
    // 【为什么可以缓存】
    // 时间轴分母恒定为 total（见上面的长注释），所以源列 k 永远映射到
    // 同一个像素 x，已画出的像素永远不会移动，新数据只向右接续。
    //
    // 【边界列会被算残，所以不能全部入缓存】
    // draw_width 由 floor 得出，最右那一两列的源区间上界（box 模式的
    // ceil、双线性模式的 lo+1）可能超过 avail，被夹紧后只能拿现有数据
    // 凑一个偏暗的临时值。临时值上屏没问题（用户要看到生长），但一旦
    // 进了缓存就会被永久冻结成一条竖向暗纹。
    // 因此 tmap 构建时直接检测夹紧、算出 final_cols（第一个残缺列），
    // 水位线只推进到 final_cols，残缺列下一帧重算。
    //
    // 【失效判据】
    // 与其去每个改状态的地方挂钩子（OnSize / 调色板 / 轴开关 /
    // 重分析），不如把建缓存时的全部前提存下来、每帧比对，
    // 漏一个都不可能：
    //   w,h      → OnSize、轴开关改变了 spec rect 几何
    //   total    → 分析结束时 total_frames 收敛成真实列数，分母变了
    //   bins     → fft 配置变化
    //   palette  → 颜色映射变化
    //   cache_cols <= draw_width → 防御 time_frames 变小（重分析复用对象）
    //
    // 位图按**完整** width 分配而非 draw_width，使行距在整个生长过程中
    // 恒定，老像素一次都不用搬移。右侧未计算区域不会被 BitBlt 取用。
    const bool cache_valid =
        track.cache_bmp != nullptr &&
        track.cache_pixels != nullptr &&
        track.cache_w == width &&
        track.cache_h == height &&
        track.cache_total == total &&
        track.cache_bins == data.fft_bins &&
        track.cache_palette == (int)m_palette &&
        track.cache_cols <= draw_width;

    if (!cache_valid) {
        track.discard_render_cache();

        // 32bpp 下行距 = width * 4，永远满足 DIB 的 DWORD 对齐要求。
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* pixels = nullptr;
        HBITMAP hbmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &pixels, NULL, 0);
        if (!hbmp || !pixels) {
            if (hbmp) DeleteObject(hbmp);
            return 0;
        }

        track.cache_bmp = hbmp;
        track.cache_pixels = (uint32_t*)pixels;
        track.cache_w = width;
        track.cache_h = height;
        track.cache_total = total;
        track.cache_bins = data.fft_bins;
        track.cache_palette = (int)m_palette;
        track.cache_cols = 0;
    }

    uint32_t* const pixel_data = track.cache_pixels;
    const int stride = track.cache_w;
    // 本帧只需要计算 [start_col, draw_width) 这一段新列。
    // 相等时下面的像素循环自然空转，只做一次 BitBlt。
    const int start_col = track.cache_cols;

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

    // ==============================================================
    // P2：真正的区间面积平均（box filter）替代纯双线性点采样
    // ==============================================================
    //
    // 【旧实现的缺陷】
    // 旧代码对每个目标像素只做一次双线性插值，即"点采样"：
    //   freq_f = (1 - py/(H-1)) * (bins-1)   然后只取 fi 与 fi+1 两个 bin。
    // 当 1025 个 bin 压进 400px 面板时，缩放比 ≈ 2.56，
    // 每个像素只读 2 个 bin，剩下 0.56 个 bin **永远不会被读到**。
    // 也就是说超过一半的频谱数据被直接丢弃 —— 这是典型的欠采样，
    // 后果是高频细节丢失 + 摩尔纹/锯齿闪烁（改变窗口高度时尤为明显）。
    // 时间轴同理：分析阶段固定产出最多 800 列（见 m_target_frames），
    // 面板宽度小于 800px 时同样在丢列。
    //
    // 【正确做法】
    // 下采样时对目标像素覆盖的**全部**源单元求平均（面积/盒式滤波），
    // 这才是数学上正确的降采样，也正是 Spek 的画质来源 ——
    // Spek 调用 image.Scale(w,h)，wxWidgets 在缩小时内部就是盒式滤波
    // （见 spek-spectrogram.cc:276）。
    //
    // 【与被删掉的 soften_spectrum 的本质区别】
    // 1) 域不同：soften 在 **RGB 空间**模糊。RGB 是 palette 非线性映射
    //    后的结果，对它取平均等于对颜色查找表插值，物理上毫无意义，
    //    甚至会跨越 palette 的锚点产生假色。
    //    本实现在 **线性幅度域**平均，之后才转 dB→palette，
    //    等价于对能量做正确积分。
    // 2) 信息量不同：soften 只是把已经丢掉数据的图再抹匀（丢失不可逆），
    //    面积平均是把本来读不到的数据全部纳入计算（信息量真实增加）。
    // 3) soften 默认 strength=0，运行时根本没执行，属于死代码。
    //
    // 【上采样保持双线性】
    // 当面板比数据还大（比如 22kHz 文件拉到 1000px 高）时，一个目标像素
    // 落在源单元内部，此时面积平均会退化成"最近邻"产生方块感，
    // 所以这种情况仍走双线性插值。两种模式按轴独立判断。
    //
    // 【实现要点：预计算区间表】
    // 把每个目标像素的源区间 [lo, hi) 预先算好存表，避免在
    // W*H 双重循环里反复做除法。频率表 H 项、时间表 W 项，
    // 内存开销可忽略（几 KB），却省下 W*H 次浮点除法。
    struct axis_map {
        int lo;        // 源起始索引（含）
        int hi;        // 源结束索引（不含），保证 hi > lo
        float frac;    // 上采样模式下的插值系数
        bool box;      // true = 面积平均, false = 双线性
    };

    // 频率轴映射表：py=height-1（矩形底部）→ DC，py=0（顶部）→ Nyquist
    std::vector<axis_map> fmap((size_t)height);
    {
        // 每个目标像素对应多少个源 bin
        const float bins_per_px = (float)data.fft_bins / (float)height;
        const bool  use_box = (bins_per_px > 1.0f);
        for (int py = 0; py < height; py++) {
            axis_map& m = fmap[(size_t)py];
            m.box = use_box;
            if (use_box) {
                // 像素行 py 在频率轴上的归一化区间（注意 y 轴翻转）
                // py 覆盖 [py, py+1) 像素带 → 频率从高到低
                float top = (float)(height - py)     * bins_per_px; // 上边界(高频)
                float bot = (float)(height - py - 1) * bins_per_px; // 下边界(低频)
                m.lo = (int)bot;
                m.hi = (int)ceilf(top);
                if (m.lo < 0) m.lo = 0;
                if (m.hi > data.fft_bins) m.hi = data.fft_bins;
                if (m.hi <= m.lo) m.hi = m.lo + 1;
                if (m.hi > data.fft_bins) { m.hi = data.fft_bins; m.lo = m.hi - 1; }
                m.frac = 0.0f;
            } else {
                float f = (height > 1)
                    ? (1.0f - (float)py / (float)(height - 1)) * (float)(data.fft_bins - 1)
                    : 0.0f;
                if (f < 0) f = 0;
                if (f > (float)(data.fft_bins - 1)) f = (float)(data.fft_bins - 1);
                m.lo = (int)f;
                m.frac = f - (float)m.lo;
                m.hi = (m.lo + 1 < data.fft_bins) ? m.lo + 1 : m.lo;
            }
        }
    }

    // 时间轴映射表：px=0 → 起点，px=width-1 → 整轨终点
    //
    // 分母固定用 total（计划总列数）而不是 avail，这样第 k 列永远落在
    // 同一个像素上；表本身只需要建到 draw_width，右边还没算出来的像素
    // 这一帧不会被访问。
    // 索引上界仍要按 avail 夹紧：draw_width 是整数截断的结果，
    // 边界像素的区间有可能算出比 avail 大一点的下标。
    //
    // R2：只有 [start_col, draw_width) 会被读到，所以只填这一段；
    // 前面的项保持默认值不会被访问。表仍按 draw_width 分配，
    // 让 px 可以直接当下标，省一次减法。
    //
    // 【final_cols：哪些列可以永久缓存】
    // 一列只有在它的源区间**没有被 avail 夹紧**时才是终值。被夹紧说明
    // 它想读的源列还没算出来，这一帧只能拿现有数据凑一个临时值 ——
    // 临时值可以上屏（用户要看到生长），但绝不能进缓存，否则会被永久
    // 冻结成一条颜色偏暗的竖线。
    //
    // 与其去推导"要保留几列"（下采样时约 1 列，上采样时约 width/total 列，
    // 还要考虑 total/width 的 float 舍入误差），不如**直接检测**：
    // 记下第一个发生夹紧的列，缓存水位线就停在那里，下一帧从它重算。
    // 这是精确判据，不依赖任何余量估算。
    //
    // 分析结束时 avail == total、draw_width == width，代入可知两个分支
    // 都不会再夹紧（box: hi = ceil(total) = total；bilinear: f < total-1），
    // 于是 final_cols == width，整幅图完整入缓存。
    int final_cols = draw_width;
    std::vector<axis_map> tmap((size_t)draw_width);
    {
        const float frames_per_px = (float)total / (float)width;
        const bool  use_box = (frames_per_px > 1.0f);
        for (int px = start_col; px < draw_width; px++) {
            axis_map& m = tmap[(size_t)px];
            m.box = use_box;
            if (use_box) {
                m.lo = (int)((float)px * frames_per_px);
                const int hi_raw = (int)ceilf((float)(px + 1) * frames_per_px);
                m.hi = hi_raw;
                if (m.lo < 0) m.lo = 0;
                if (m.hi > avail) m.hi = avail;
                if (m.hi <= m.lo) m.hi = m.lo + 1;
                if (m.hi > avail) { m.hi = avail; m.lo = m.hi - 1; }
                if (m.lo < 0) m.lo = 0;
                m.frac = 0.0f;
                if (hi_raw > avail && px < final_cols) final_cols = px;
            } else {
                float f = (width > 0)
                    ? (float)px / (float)width * (float)(total - 1)
                    : 0.0f;
                if (f < 0) f = 0;
                const bool clamped = (f > (float)(avail - 1));
                if (clamped) f = (float)(avail - 1);
                m.lo = (int)f;
                m.frac = f - (float)m.lo;
                m.hi = (m.lo + 1 < avail) ? m.lo + 1 : m.lo;
                // hi 退化成 lo 也算残缺：右邻列还没到，插值没法做。
                if ((clamped || m.lo + 1 >= avail) && px < final_cols) final_cols = px;
            }
        }
    }

    const float* src = data.data.data();
    const int    nbins = data.fft_bins;

    // px 从 start_col 起 —— 左边的列已经在缓存位图里，是终值，不重算。
    for (int py = 0; py < height; py++) {
        const axis_map& fm = fmap[(size_t)py];

        for (int px = start_col; px < draw_width; px++) {
            const axis_map& tm = tmap[(size_t)px];

            // ---- 在线性幅度域取值 ----
            // 四种组合分开写，让编译器能在各自的热循环里做向量化，
            // 而不是在最内层反复判断 box 标志。
            float val;
            if (fm.box && tm.box) {
                // 时间 + 频率双向下采样：完整二维面积平均
                float sum = 0.0f;
                for (int t = tm.lo; t < tm.hi; t++) {
                    const float* row = src + (size_t)t * nbins;
                    float rs = 0.0f;
                    for (int f = fm.lo; f < fm.hi; f++) rs += row[f];
                    sum += rs;
                }
                const int cnt = (tm.hi - tm.lo) * (fm.hi - fm.lo);
                val = sum / (float)cnt;
            } else if (fm.box) {
                // 仅频率下采样；时间方向在两列之间线性插值
                const float* row0 = src + (size_t)tm.lo * nbins;
                const float* row1 = src + (size_t)tm.hi * nbins;
                float s0 = 0.0f, s1 = 0.0f;
                for (int f = fm.lo; f < fm.hi; f++) { s0 += row0[f]; s1 += row1[f]; }
                const float inv = 1.0f / (float)(fm.hi - fm.lo);
                s0 *= inv; s1 *= inv;
                val = s0 + (s1 - s0) * tm.frac;
            } else if (tm.box) {
                // 仅时间下采样；频率方向在两 bin 之间线性插值
                float sum = 0.0f;
                for (int t = tm.lo; t < tm.hi; t++) {
                    const float* row = src + (size_t)t * nbins;
                    const float a = row[fm.lo];
                    const float b = row[fm.hi];
                    sum += a + (b - a) * fm.frac;
                }
                val = sum / (float)(tm.hi - tm.lo);
            } else {
                // 双向上采样：保持原双线性插值，避免方块感
                const float* row0 = src + (size_t)tm.lo * nbins;
                const float* row1 = src + (size_t)tm.hi * nbins;
                const float v00 = row0[fm.lo], v10 = row0[fm.hi];
                const float v01 = row1[fm.lo], v11 = row1[fm.hi];
                const float v0 = v00 + (v01 - v00) * tm.frac;
                const float v1 = v10 + (v11 - v10) * tm.frac;
                val = v0 + (v1 - v0) * fm.frac;
            }

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
            // 行距用 stride（= 完整 rect 宽度）而不是 draw_width：
            // 位图按完整宽度分配，生长时行距恒定，老像素无需搬移。
            pixel_data[(size_t)py * (size_t)stride + (size_t)px] = color;
        }
    }

    // 推进缓存水位线：只把**终值列**记进去。
    // final_cols 是本帧第一个源区间被 avail 夹紧的列（见 tmap 构建处），
    // 它和右边所有列都是拿不完整数据凑出来的临时值 —— 它们已经写进位图、
    // 会随本次 BitBlt 上屏（用户能看到生长），但不计入水位线，
    // 下一帧会从 final_cols 重新算，直到源数据补齐才定格。
    // 分析结束时 final_cols == width，整幅图入缓存，此后每帧纯 BitBlt。
    if (final_cols > track.cache_cols) track.cache_cols = final_cols;

    // 注意：这里原先有一次 soften_spectrum() 后置 RGB 模糊调用。
    // P2 已将其彻底移除 —— 上面的面积平均在线性幅度域做了正确的抗锯齿，
    // 再叠加一层 RGB 模糊只会白白牺牲清晰度（且它默认 strength=0 从未生效）。

    // Blit to screen —— 只贴已算出的 draw_width 那部分。
    // 位图是缓存，绝不能在这里 DeleteObject；它由 TrackSpectrum
    // 的 discard_render_cache()/析构函数负责释放。
    HDC mem_dc = CreateCompatibleDC(dc);
    HBITMAP old_bmp = (HBITMAP)SelectObject(mem_dc, track.cache_bmp);
    BitBlt(dc, rc.left, rc.top, draw_width, height, mem_dc, 0, 0, SRCCOPY);
    SelectObject(mem_dc, old_bmp);
    DeleteDC(mem_dc);
    return draw_width;
}

// ===========================================================================
// P3：通用刻度选取算法 —— 移植自 Spek 的 SpekRuler::draw
//     （spek/src/spek-ruler.cc，原实现 30-52 行）
//
// 【为什么要换掉原来的做法】
// 改造前三条轴各写一套硬编码步长，各有各的毛病：
//   - 频率轴：一个 `if (max_khz <= 22)` 特例 + nice_steps[] 表 +
//             两趟"舒适区间 [4,10]"启发式，逻辑长且难预测；
//   - 时间轴：`const int time_interval = 20` 固定 20 秒。
//             一首 3 小时的文件会画 540 个标签，糊成一片黑；
//   - dB 轴：固定每 10dB 一刻度、每 20dB 一标签。
//             面板拉矮时同样重叠。
// 更根本的问题是：三者都在**猜**标签有多大（label_half_h = scale(8)、
// label_half_w = scale(22) 这类魔法数），一旦用户换了大字号或高 DPI，
// 猜测立刻失效。
//
// 【Spek 的做法】
// 只有一条规则：真实测量一个样例标签的像素尺寸 len，然后从**升序**因子
// 数组里挑出第一个满足下式的因子：
//
//     |scale * factor| >= spacing * len
//
// 其中 scale = 像素/单位，spacing 是"标签间至少留几倍标签长度"。
// 这保证无论采样率、时长、面板尺寸、字号怎么变，标签永远不会挤在一起，
// 且总是取信息量最大（最密）的那一档。
//
// 【首尾端点】
// Spek 无条件画 min 和 max 两个端点（频率轴因此总能看到精确 Nyquist，
// 不再需要原来那段 `gap_px >= label_h` 的补丁）。中间刻度一旦逼近 max
// 就 break，避免和端点标签撞上 —— 判据是 len * 1.2。
//
// 【参数说明】
//   horizontal : true 用样例标签的宽度做判据（横轴），false 用高度（纵轴）。
//                这就是 Spek 里 `pos == TOP || pos == BOTTOM` 的分支。
//   factors    : 升序排列、以 0 结尾的因子数组。
//   返回值     : 测得的 len（像素）。调用方直接拿它当标签尺寸用，
//                彻底取代原先的魔法数。
//
// 前置条件：调用前必须已经把目标字体 SelectObject 进 hdc，否则量出来的
// 是系统默认字体的尺寸。
// ===========================================================================
static int pick_ruler_ticks(
    HDC hdc,
    const wchar_t* sample_label,
    bool horizontal,
    const int* factors,
    int min_units,
    int max_units,
    double spacing,
    double scale,
    std::vector<int>& out)
{
    out.clear();
    if (max_units <= min_units) return 0;

    SIZE ext = { 0, 0 };
    ::GetTextExtentPoint32(hdc, sample_label, (int)wcslen(sample_label), &ext);
    const int len = horizontal ? ext.cx : ext.cy;

    // 挑出第一个能拉开足够间距的因子；若一个都不满足（面板极小），
    // factor 保持 0，下面就只画首尾两个端点。
    int factor = 0;
    for (int i = 0; factors[i] != 0; ++i) {
        if (fabs(scale * (double)factors[i]) >= spacing * (double)len) {
            factor = factors[i];
            break;
        }
    }

    out.push_back(min_units);
    if (factor > 0) {
        for (int tick = min_units + factor; tick < max_units; tick += factor) {
            // 逼近 max 端点标签就停，防止最后两个标签重叠
            if (fabs(scale * (double)(max_units - tick)) < (double)len * 1.2) break;
            out.push_back(tick);
        }
    }
    out.push_back(max_units);
    return len;
}

void SpectrumCompareWindow::render_freq_axis(CDCHandle dc, const RECT& rc, int sample_rate) {
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0 || sample_rate <= 0) return;

    int nyquist = sample_rate / 2;

    // DPI-aware tick and label dimensions
    int dpi = m_dpi;
    auto scale = [dpi](int v) { return MulDiv(v, dpi, 96); };
    int tick_len = scale(4);
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
    // 字体必须先选进 DC，pick_ruler_ticks 才能量到正确的标签尺寸
    SelectObjectScope fontScope(dc, (HGDIOBJ)m_callback->query_font_ex(ui_font_default));

    // 因子数组，单位 Hz。前 5 项与 Spek 的 freq_factors 完全一致
    // （spek-spectrogram.cc:{1000,2000,5000,10000,20000,0}）；
    // 后几项是为高采样率素材扩展的 —— 修好采样率来源后，最高的
    // DSD64 抽取成 352800 Hz、Nyquist 176.4 kHz，20 kHz 因子会画出
    // 9 个标签，够用；但 384/768 kHz 的 PCM 仍需要更大的因子。
    static const int freq_factors[] = {
        1000, 2000, 5000, 10000, 20000,
        50000, 100000, 200000, 0
    };

    // 纵向标尺 → 用样例标签的**高度**判间距（对应 Spek 的 LEFT 标尺）。
    // 这也顺带说明为什么标签里的位数无关紧要：竖排挤的是行高，不是字宽。
    std::vector<int> ticks;
    int label_h = pick_ruler_ticks(
        dc, L"000 kHz", false, freq_factors,
        0, nyquist, 3.0, (double)height / (double)nyquist, ticks);
    if (label_h <= 0) label_h = scale(16);
    const int label_half_h = label_h / 2;

    // Draw axis line on the right edge
    CPen axisPen;
    axisPen.CreatePen(PS_SOLID, 1, grid_color);
    SelectObjectScope penScope(dc, axisPen);
    dc.MoveTo(rc.right - 1, rc.top);
    dc.LineTo(rc.right - 1, rc.bottom);

    for (int freq_hz : ticks) {
        if (freq_hz < 0 || freq_hz > nyquist) continue;

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

        // 标签文本对齐 Spek：数值 + 空格 + "kHz"（spek-ruler.cc 的
        // format 是 "%d kHz"）。原来写的是紧凑的 "176k"，少了单位也少
        // 了空格。四舍五入到最近的 kHz，这样 Nyquist=22050 显示 "22 kHz"。
        pfc::string8 label;
        label << ((freq_hz + 500) / 1000) << " kHz";
        pfc::stringcvt::string_wide_from_utf8 label_w(label);
        CRect label_rc(rc.left, py - label_half_h, rc.right - tick_len - tick_gap - 1, py + label_half_h);
        dc.DrawText(label_w, -1, &label_rc, DT_NOPREFIX | DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

void SpectrumCompareWindow::render_time_axis(CDCHandle dc, const RECT& rc, double duration) {
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0 || duration <= 0) return;

    // DPI-aware dimensions
    int dpi = m_dpi;
    auto scale = [dpi](int v) { return MulDiv(v, dpi, 96); };
    int tick_len = scale(4);
    int tick_gap = scale(1);

    COLORREF text_color = m_callback->query_std_color(ui_color_text);
    COLORREF bg_color = m_callback->query_std_color(ui_color_background);
    COLORREF grid_color = RGB(
        (GetRValue(text_color) + GetRValue(bg_color)) / 2,
        (GetGValue(text_color) + GetGValue(bg_color)) / 2,
        (GetBValue(text_color) + GetBValue(bg_color)) / 2
    );

    dc.SetTextColor(text_color);
    dc.SetBkMode(TRANSPARENT);
    // 字体必须先选进 DC，pick_ruler_ticks 才能量到正确的标签尺寸
    SelectObjectScope fontScope(dc, (HGDIOBJ)m_callback->query_font_ex(ui_font_default));

    // 因子数组，单位「秒」。前 12 项与 Spek 的 time_factors 完全一致；
    // 末尾补了 3600/7200（1 小时 / 2 小时），因为本组件要面对整盘 SACD、
    // 演唱会实录这类超长音轨，否则 30 分钟的因子会挤出上百个刻度。
    static const int time_factors[] = {
        1, 2, 5, 10, 20, 30, 60, 120, 300, 600, 1200, 1800,
        3600, 7200, 0
    };

    std::vector<int> ticks;
    int label_w = pick_ruler_ticks(
        dc, L"00:00", true, time_factors,
        0, (int)duration, 1.5, (double)width / duration, ticks);
    if (label_w <= 0) label_w = scale(44);
    const int label_half_w = label_w / 2 + tick_gap;

    // Draw axis line on top edge
    CPen axisPen;
    axisPen.CreatePen(PS_SOLID, 1, grid_color);
    SelectObjectScope penScope(dc, axisPen);
    dc.MoveTo(rc.left, rc.top + tick_gap);
    dc.LineTo(rc.right, rc.top + tick_gap);

    for (int t : ticks) {
        if (t < 0 || t > (int)duration) continue;
        int px = rc.left + (int)((double)t / duration * width);
        if (px < rc.left || px > rc.right) continue;

        // Tick mark
        dc.MoveTo(px, rc.top + tick_gap);
        dc.LineTo(px, rc.top + tick_len + tick_gap);

        // Label (centered below tick)，格式与 Spek 的 "%d:%02d" 一致
        int min = t / 60;
        int sec = t % 60;
        pfc::string8 label;
        label << min << ":";
        if (sec < 10) label << "0";
        label << sec;
        pfc::stringcvt::string_wide_from_utf8 label_w_str(label);
        CRect label_rc(px - label_half_w, rc.top + tick_len + tick_gap + 1, px + label_half_w, rc.bottom);
        dc.DrawText(label_w_str, -1, &label_rc, DT_NOPREFIX | DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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

    // 2) dB 刻度：交给 pick_ruler_ticks 按实测字高自适应挑选步长
    int dpi = m_dpi;
    auto scale = [dpi](int v) { return MulDiv(v, dpi, 96); };
    int tick_len = scale(4);

    dc.SetTextColor(text_color);
    dc.SetBkMode(TRANSPARENT);
    // 字体必须先选进 DC，pick_ruler_ticks 才能量到正确的标签尺寸
    SelectObjectScope fontScope(dc, (HGDIOBJ)m_callback->query_font_ex(ui_font_default));

    // 因子数组，单位 dB，与 Spek 的 density_factors 完全一致。
    // 原先是「每 10dB 画线 + 每 20dB 画字」的双重硬编码：色条矮时 13 档线
    // 糊成一片，色条高时又白白浪费空间。现在步长随色条高度和字号自动伸缩。
    static const int density_factors[] = { 1, 2, 5, 10, 20, 50, 0 };

    // pick_ruler_ticks 要求 min_units < max_units 的递增区间，所以这里用
    // 「衰减量」为单位（0 → 120），画的时候再翻回负 dB。
    std::vector<int> ticks;
    int label_h = pick_ruler_ticks(
        dc, L"-00 dB", false, density_factors,
        0, (int)dyn_range, 3.0, (double)bh / (double)dyn_range, ticks);
    if (label_h <= 0) label_h = scale(16);
    const int label_half_h = label_h / 2;

    CPen tickPen;
    tickPen.CreatePen(PS_SOLID, 1, grid_color);
    SelectObjectScope tickScope(dc, tickPen);

    for (int atten : ticks) {
        const int db = -atten;
        // 线性 ratio：(dB - floor) / dyn_range  →  0dB=1.0, -120dB=0.0
        float ratio = ((float)db - floor_db) / dyn_range;
        if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
        // top (ratio=1.0) → y=bar_rc.top; bottom (ratio=0) → y=bar_rc.bottom-1
        int py = bar_rc.top + (int)((1.0f - ratio) * (bh - 1));
        if (py < bar_rc.top || py >= bar_rc.bottom) continue;

        // Tick 小横线
        dc.MoveTo(bar_rc.left - tick_len, py);
        dc.LineTo(bar_rc.left, py);

        // 文字标签：刻度已经按字高选过间距，这里不再需要额外的抽稀门控
        pfc::string8 label;
        if (db == 0) label = "0 dB";
        else label << db;
        pfc::stringcvt::string_wide_from_utf8 label_w(label);
        CRect lr(label_rc.left, py - label_half_h, label_rc.right, py + label_half_h);
        dc.DrawText(label_w, -1, &lr, DT_NOPREFIX | DT_LEFT | DT_VCENTER | DT_SINGLELINE);
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
//   [10]     Pseudo-transparency                            (1 leaf)
//   [11..12] Title format   ▶  Edit / Reset                (2 leaves)
//   [13..14] Language       ▶  English / Chinese           (2 leaves)
//   [15]     Refresh analysis                              (1 leaf)
static constexpr size_t kCountIdx     = 0;
static constexpr size_t kPaletteIdx   = 4;
static constexpr size_t kAxesIdx      = 7;
static constexpr size_t kPseudoIdx    = 10;
static constexpr size_t kTitleIdx     = 11;
static constexpr size_t kLangIdx      = 13;
static constexpr size_t kRefreshIdx   = 15;
static constexpr size_t kTotalLeaves  = 16;

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
                                              bool show_db, bool pseudo_transparency,
                                              language_t lang) {
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

    // Pseudo-transparency — standalone checked toggle (no submenu)
    PFC_ASSERT(leaf_idms.size() == kPseudoIdx);
    append_menu_i18n(p_menu, MF_STRING | (pseudo_transparency ? MF_CHECKED : 0),
                     p_id_base + (unsigned)leaf_idms.size(), S_PSEUDO_TRANSPARENCY, lang);
    leaf_idms.push_back(IDM_TOGGLE_PSEUDO_TRANSPARENCY);

    WIN32_OP_D(::AppendMenu(p_menu, MF_SEPARATOR, 0, _T("")));

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
        m_show_freq_axis, m_show_time_axis, m_show_db_scale,
        m_pseudo_transparency, m_language);
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
        m_show_freq_axis, m_show_time_axis, m_show_db_scale,
        m_pseudo_transparency, m_language);

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

void SpectrumCompareWindow::OnTogglePseudoTransparency(UINT uNotifyCode, int nID, CWindow wndCtl) {
    (void)uNotifyCode; (void)nID; (void)wndCtl;
    m_pseudo_transparency = !m_pseudo_transparency;
    g_cfg_pseudo_transparency = m_pseudo_transparency;
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
    // 必须与 OnPaint 里的 freq_axis_width 完全一致，否则内联编辑框的
    // 左边缘会和标题行错开。
    const int freq_axis_width = m_show_freq_axis ? scale(60) : 0;
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
        "1.4",
        "Vertical spectrogram comparison panel for selected tracks. Spek-style coloring.\n\n"
        "Authors: TRAE AI Coding Assistant, always beta, Asion\n\n"
        "Select one or more tracks in the playlist to view their spectrograms.\n"
        "Right-click to set display count (1-4), palette, axes, or refresh.\n"
        "Useful for comparing audio quality and frequency content across tracks."
    );
}
