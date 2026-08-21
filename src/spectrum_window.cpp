#include "stdafx.h"
#include "spectrum_window.h"
#include <helpers/BumpableElem.h>
#include <algorithm>
#include <cmath>

// Static config storage (registered once at program startup)
cfg_bool g_cfg_show_freq_axis(guid_cfg_show_freq_axis, true);
cfg_bool g_cfg_show_time_axis(guid_cfg_show_time_axis, true);
cfg_string g_cfg_title_format(guid_cfg_title_format, DEFAULT_TITLE_FORMAT);
cfg_int g_cfg_max_tracks(guid_cfg_max_tracks, 4);
cfg_int g_cfg_palette(guid_cfg_palette, (int)PALETTE_SPECTRUM);

SpectrumCompareWindow::SpectrumCompareWindow(
    ui_element_config::ptr config,
    ui_element_instance_callback_ptr p_callback)
    : m_callback(p_callback)
    , m_config(config)
{
    // Load persistent config into runtime members
    m_show_freq_axis = g_cfg_show_freq_axis;
    m_show_time_axis = g_cfg_show_time_axis;
    m_title_format = g_cfg_title_format.get();
    m_max_tracks = (int)g_cfg_max_tracks;
    m_palette = (palette_t)(int)g_cfg_palette;

    // Apply to analyzer
    m_analyzer.set_title_format(m_title_format.c_str());
}

SpectrumCompareWindow::~SpectrumCompareWindow() {
    m_shutdown = true;

    // Abort all in-flight analyses so worker threads exit promptly.
    {
        std::lock_guard<std::mutex> lock(m_tracks_mutex);
        for (auto& t : m_tracks) {
            if (t->analyzing) {
                try { t->abort.abort(); } catch (...) {}
            }
        }
    }

    // Stop timer
    if (m_hWnd) KillTimer(TIMER_REPAINT);

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

    // Initial selection update
    update_selection();
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
    size_t display_count = (std::min)((size_t)m_max_tracks, selected.get_size());

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

// ============================================================
// Painting
// ============================================================

LRESULT SpectrumCompareWindow::OnEraseBkgnd(CDCHandle dc) {
    return TRUE; // We handle all drawing in OnPaint
}

void SpectrumCompareWindow::OnPaint(CDCHandle) {
    CPaintDC dc(*this);
    CRect rc;
    GetClientRect(&rc);

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
        dc.DrawText(_T("Select tracks in the playlist to view spectrograms"), -1, &rc, format);
        return;
    }

    // DPI-aware scaling: get system DPI and scale dimensions accordingly.
    // All measurements below are in pixels at 96 DPI, scaled to actual DPI.
    HDC screenDC = ::GetDC(NULL);
    int dpi = GetDeviceCaps(screenDC, LOGPIXELSX);
    ::ReleaseDC(NULL, screenDC);
    auto scale = [dpi](int v) -> int { return MulDiv(v, dpi, 96); };

    // Layout measurements (designed at 96 DPI, scaled to actual DPI)
    const int padding_outer = scale(8);      // outer margin around all content
    const int label_height = scale(22);      // title label row height
    const int freq_axis_width = m_show_freq_axis ? scale(48) : 0;
    const int time_axis_height = m_show_time_axis ? scale(20) : 0;
    const int track_gap = scale(6);          // gap between tracks
    const int label_to_spec_gap = scale(4);  // gap below label before spectrum
    const int spec_to_axis_gap = scale(2);   // gap between spectrum and axes

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

        // Label area (top of track, full width)
        CRect label_rc(track_rc.left, track_rc.top, track_rc.right, track_rc.top + label_height);
        render_track_label(dc.m_hDC, label_rc, *m_tracks[i]);

        // Spectrum area: below label, with freq axis (left) and time axis (bottom)
        int spec_top = track_rc.top + label_height + label_to_spec_gap;
        int spec_bottom = track_rc.bottom - time_axis_height - spec_to_axis_gap;
        int spec_left = track_rc.left + freq_axis_width;
        int spec_right = track_rc.right;

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
            err << "Error: " << m_tracks[i]->data.error_msg.c_str();
            pfc::stringcvt::string_wide_from_utf8 err_w(err);
            dc.DrawText(err_w, -1, &spec_rc, DT_NOPREFIX | DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (m_tracks[i]->analyzing) {
            dc.SetTextColor(m_callback->query_std_color(ui_color_text));
            dc.SetBkMode(TRANSPARENT);
            SelectObjectScope fontScope(dc, (HGDIOBJ)m_callback->query_font_ex(ui_font_default));
            dc.DrawText(_T("Analyzing..."), -1, &spec_rc, DT_NOPREFIX | DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // Time axis (below spectrum, aligned with spectrum horizontal range)
        if (m_show_time_axis && m_tracks[i]->data.duration > 0) {
            CRect time_rc(spec_left, spec_bottom + spec_to_axis_gap, spec_right, track_rc.bottom);
            render_time_axis(dc.m_hDC, time_rc, m_tracks[i]->data.duration);
        }
    }
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
    HDC screenDC = ::GetDC(NULL);
    int dpi = GetDeviceCaps(screenDC, LOGPIXELSX);
    ::ReleaseDC(NULL, screenDC);
    int pad = MulDiv(8, dpi, 96);

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

    // Dynamic range for dB scaling
    float dyn_range = 80.0f; // dB
    float ref_level = data.max_level > 0 ? data.max_level : 1.0f;

    // Map spectrum data to pixels
    // X axis: time (left = start, right = end)
    // Y axis: frequency (bottom = low freq, top = high freq) - inverted because top-down DIB
    for (int py = 0; py < height; py++) {
        // Frequency bin: py=0 (top) = high freq, py=height-1 (bottom) = low freq
        float freq_norm = 1.0f - (float)py / (height - 1);
        // Use log frequency scale for better display
        float log_freq = powf(freq_norm, 1.5f); // slight log-like compression
        int fbin = (int)(log_freq * (data.fft_bins - 1));
        if (fbin < 0) fbin = 0;
        if (fbin >= data.fft_bins) fbin = data.fft_bins - 1;

        for (int px = 0; px < width; px++) {
            int tframe = (int)((float)px / width * (data.time_frames - 1));
            if (tframe < 0) tframe = 0;
            if (tframe >= data.time_frames) tframe = data.time_frames - 1;

            float val = data.get(tframe, fbin);

            // Convert to dB scale
            float level_db = 0;
            if (val > 0 && ref_level > 0) {
                level_db = 20.0f * log10f(val / ref_level);
            }
            // Normalize to 0..1
            float level_norm = (level_db + dyn_range) / dyn_range;
            if (level_norm < 0) level_norm = 0;
            if (level_norm > 1) level_norm = 1;

            uint32_t color = spek_palette(m_palette, level_norm);
            // Convert 0xRRGGBB to BGRA for DIB
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = color & 0xFF;
            pixel_data[py * width + px] = (b << 16) | (g << 8) | r;
        }
    }

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

    // Frequency labels at 0, 5, 10, 15, 20, 22 kHz (capped at Nyquist)
    static const int kHz_ticks[] = { 0, 5, 10, 15, 20, 22 };

    // DPI-aware tick and label dimensions
    HDC screenDC = ::GetDC(NULL);
    int dpi = GetDeviceCaps(screenDC, LOGPIXELSX);
    ::ReleaseDC(NULL, screenDC);
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

    for (int khz : kHz_ticks) {
        int freq_hz = khz * 1000;
        if (freq_hz > nyquist) continue;

        // Map frequency to y position (matching spectrum's pow(1.5) compression)
        float freq_ratio = (float)freq_hz / nyquist;
        float freq_norm = powf(freq_ratio, 2.0f / 3.0f);
        int py = rc.top + (int)((1.0f - freq_norm) * (height - 1));

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
    HDC screenDC = ::GetDC(NULL);
    int dpi = GetDeviceCaps(screenDC, LOGPIXELSX);
    ::ReleaseDC(NULL, screenDC);
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

// ============================================================
// Size / Timer
// ============================================================

void SpectrumCompareWindow::OnSize(UINT nType, CSize size) {
    Invalidate();
}

void SpectrumCompareWindow::OnTimer(UINT_PTR nIDEvent) {
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
// ============================================================

void SpectrumCompareWindow::OnContextMenu(CWindow wnd, CPoint point) {
    CMenu menu;
    menu.CreatePopupMenu();

    // Display count submenu
    CMenu count_menu;
    count_menu.CreatePopupMenu();
    count_menu.AppendMenu(MF_STRING | (m_max_tracks == 1 ? MF_CHECKED : 0), IDM_SET_COUNT_1, _T("1 track"));
    count_menu.AppendMenu(MF_STRING | (m_max_tracks == 2 ? MF_CHECKED : 0), IDM_SET_COUNT_2, _T("2 tracks"));
    count_menu.AppendMenu(MF_STRING | (m_max_tracks == 3 ? MF_CHECKED : 0), IDM_SET_COUNT_3, _T("3 tracks"));
    count_menu.AppendMenu(MF_STRING | (m_max_tracks == 4 ? MF_CHECKED : 0), IDM_SET_COUNT_4, _T("4 tracks"));
    menu.AppendMenu(MF_POPUP, (UINT_PTR)count_menu.m_hMenu, _T("Display count"));

    menu.AppendMenu(MF_SEPARATOR);

    // Palette submenu
    CMenu palette_menu;
    palette_menu.CreatePopupMenu();
    palette_menu.AppendMenu(MF_STRING | (m_palette == PALETTE_SPECTRUM ? MF_CHECKED : 0), IDM_PALETTE_SPECTRUM, _T("Spectrum (Spek)"));
    palette_menu.AppendMenu(MF_STRING | (m_palette == PALETTE_SOX ? MF_CHECKED : 0), IDM_PALETTE_SOX, _T("SoX"));
    palette_menu.AppendMenu(MF_STRING | (m_palette == PALETTE_MONO ? MF_CHECKED : 0), IDM_PALETTE_MONO, _T("Mono"));
    menu.AppendMenu(MF_POPUP, (UINT_PTR)palette_menu.m_hMenu, _T("Palette"));

    menu.AppendMenu(MF_SEPARATOR);

    // Axes submenu
    CMenu axes_menu;
    axes_menu.CreatePopupMenu();
    axes_menu.AppendMenu(MF_STRING | (m_show_freq_axis ? MF_CHECKED : 0), IDM_TOGGLE_FREQ_AXIS, _T("Frequency axis (kHz)"));
    axes_menu.AppendMenu(MF_STRING | (m_show_time_axis ? MF_CHECKED : 0), IDM_TOGGLE_TIME_AXIS, _T("Time axis (20s)"));
    menu.AppendMenu(MF_POPUP, (UINT_PTR)axes_menu.m_hMenu, _T("Axes"));

    // Title format submenu
    CMenu fmt_menu;
    fmt_menu.CreatePopupMenu();
    fmt_menu.AppendMenu(MF_STRING, IDM_EDIT_TITLE_FORMAT, _T("Edit format..."));
    fmt_menu.AppendMenu(MF_STRING, IDM_RESET_TITLE_FORMAT, _T("Reset to default"));
    menu.AppendMenu(MF_POPUP, (UINT_PTR)fmt_menu.m_hMenu, _T("Title format"));

    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, IDM_REFRESH, _T("Refresh analysis"));

    // All submenu handles must remain valid until TrackPopupMenu returns.
    int cmd = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        point.x, point.y,
        0, m_hWnd, NULL
    );

    if (cmd > 0) {
        SendMessage(WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
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
    switch (nID) {
    case IDM_PALETTE_SPECTRUM: m_palette = PALETTE_SPECTRUM; break;
    case IDM_PALETTE_SOX: m_palette = PALETTE_SOX; break;
    case IDM_PALETTE_MONO: m_palette = PALETTE_MONO; break;
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

// In-memory dialog for editing the title format string
namespace {
    // Dialog item IDs
    enum { IDC_EDIT = 1000 };

    struct DialogData {
        pfc::string8* result;
        bool ok;
    };

    INT_PTR CALLBACK TitleFormatDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_INITDIALOG: {
            DialogData* data = (DialogData*)lParam;
            SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)data);
            // Set initial text in edit control
            pfc::stringcvt::string_wide_from_utf8 w(data->result->c_str());
            SetDlgItemTextW(hDlg, IDC_EDIT, w);
            // Select all text
            SendDlgItemMessageW(hDlg, IDC_EDIT, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(hDlg, IDC_EDIT));
            return FALSE; // we set focus
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case IDOK: {
                DialogData* data = (DialogData*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
                HWND edit = GetDlgItem(hDlg, IDC_EDIT);
                int len = GetWindowTextLengthW(edit);
                pfc::array_t<wchar_t> buf;
                buf.set_size(len + 1);
                GetWindowTextW(edit, buf.get_ptr(), (int)buf.get_size());
                *(data->result) = pfc::stringcvt::string_utf8_from_wide(buf.get_ptr());
                data->ok = true;
                EndDialog(hDlg, IDOK);
                return TRUE;
            }
            case IDCANCEL:
                EndDialog(hDlg, IDCANCEL);
                return TRUE;
            }
            break;
        }
        return FALSE;
    }

    // Build a DLGTEMPLATE in memory. All offsets must be DWORD-aligned.
    // Returns the complete template buffer.
    std::vector<BYTE> buildTitleFormatDialogTemplate() {
        // Use DLGTEMPLATEEX format for better font support
        // DLGTEMPLATEEX: signature(WORD=0xFFFF) dlgVer(WORD=1) helpID(DWORD) exStyle(DWORD) style(DWORD) cDlgItems(WORD) x,y,cx,cy(WORD each)
        std::vector<BYTE> buf;
        auto align = [&buf]() {
            while (buf.size() % 4 != 0) buf.push_back(0);
        };
        // Helper lambdas. NOTE: we do NOT cross-call lambdas to avoid MSVC
        // issues with lambda-variable capture inside another lambda.
        auto pushWord = [&buf](WORD w) {
            buf.push_back((BYTE)(w & 0xFF));
            buf.push_back((BYTE)(w >> 8));
        };
        auto pushDWord = [&buf](DWORD dw) {
            buf.push_back((BYTE)(dw & 0xFF));
            buf.push_back((BYTE)((dw >> 8) & 0xFF));
            buf.push_back((BYTE)((dw >> 16) & 0xFF));
            buf.push_back((BYTE)((dw >> 24) & 0xFF));
        };
        auto pushWideStr = [&buf](const wchar_t* s) {
            while (*s) {
                WORD ch = (WORD)*s;
                buf.push_back((BYTE)(ch & 0xFF));
                buf.push_back((BYTE)(ch >> 8));
                ++s;
            }
            // explicit null terminator (WORD 0)
            buf.push_back(0);
            buf.push_back(0);
        };
        auto pushWordZero = [&buf]() {
            buf.push_back(0);
            buf.push_back(0);
        };

        align();
        // DLGTEMPLATEEX header
        pushWord(0xFFFF); // signature
        pushWord(1);       // dlgVer
        pushDWord(0);      // helpID
        pushDWord(WS_EX_DLGMODALFRAME); // exStyle
        pushDWord(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT); // style
        pushWord(4);       // cDlgItems
        pushWord(0); pushWord(0);   // x, y
        pushWord(300); pushWord(180); // cx, cy
        // menu, class (both 0)
        pushWordZero(); pushWordZero();
        // title
        pushWideStr(L"Edit Title Format");
        // font (DS_SETFONT): point size + face name
        pushWord(9); // 9pt
        pushWideStr(L"Segoe UI");

        // Item 1: Static text (label)
        align();
        pushDWord(WS_CHILD | WS_VISIBLE | SS_LEFT); // style
        pushDWord(0); // exStyle
        pushWord(5); pushWord(5); pushWord(290); pushWord(10); // x,y,cx,cy
        pushWord(0xFFFF); // class as atom
        pushWord(0x0082); // Static atom
        pushWideStr(L"Enter foobar2000 titleformat string:");
        pushWordZero(); // extra count

        // Item 2: Edit control (multiline)
        align();
        pushDWord(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL);
        pushDWord(WS_EX_CLIENTEDGE); // exStyle
        pushWord(5); pushWord(20); pushWord(290); pushWord(120);
        pushWord(IDC_EDIT);
        pushWord(0xFFFF); // class as atom
        pushWord(0x0081); // Edit atom
        pushWordZero(); // empty title (wide null)
        pushWordZero(); // extra count

        // Item 3: OK button
        align();
        pushDWord(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON);
        pushDWord(0);
        pushWord(190); pushWord(155); pushWord(50); pushWord(14);
        pushWord(IDOK);
        pushWord(0xFFFF);
        pushWord(0x0080); // Button atom
        pushWideStr(L"OK");
        pushWordZero();

        // Item 4: Cancel button
        align();
        pushDWord(WS_CHILD | WS_VISIBLE | WS_TABSTOP);
        pushDWord(0);
        pushWord(245); pushWord(155); pushWord(50); pushWord(14);
        pushWord(IDCANCEL);
        pushWord(0xFFFF);
        pushWord(0x0080); // Button atom
        pushWideStr(L"Cancel");
        pushWordZero();

        return buf;
    }

    bool showTitleFormatDialog(HWND parent, pfc::string8& format) {
        auto buf = buildTitleFormatDialogTemplate();
        DialogData data{ &format, false };
        INT_PTR ret = DialogBoxIndirectParamW(
            core_api::get_my_instance(),
            (LPCDLGTEMPLATEW)buf.data(),
            parent,
            TitleFormatDlgProc,
            (LPARAM)&data
        );
        return ret == IDOK && data.ok;
    }
}

void SpectrumCompareWindow::OnEditTitleFormat(UINT uNotifyCode, int nID, CWindow wndCtl) {
    pfc::string8 newFormat = m_title_format;
    if (showTitleFormatDialog(m_hWnd, newFormat)) {
        m_title_format = newFormat;
        g_cfg_title_format.set(m_title_format);
        m_analyzer.set_title_format(m_title_format.c_str());
        // Re-analyze all tracks to update labels
        OnRefresh(0, IDM_REFRESH, nullptr);
    }
}

void SpectrumCompareWindow::OnResetTitleFormat(UINT uNotifyCode, int nID, CWindow wndCtl) {
    m_title_format = DEFAULT_TITLE_FORMAT;
    g_cfg_title_format.set(m_title_format);
    m_analyzer.set_title_format(m_title_format.c_str());
    OnRefresh(0, IDM_REFRESH, nullptr);
}

// ============================================================
// Component registration
// ============================================================

namespace {
    // Custom ui_element_impl without KFlagHavePopupCommand to avoid foobar2000's
    // built-in context menu (export settings, etc.) on the panel title bar.
    class ui_element_spectrum_compare :
        public ui_element_impl<ImplementBumpableElem<SpectrumCompareWindow>, ui_element_v2>
    {
    public:
        t_uint32 get_flags() override { return ui_element_v2::KFlagSupportsBump; }
        bool bump() override { return ImplementBumpableElem<SpectrumCompareWindow>::Bump(); }
    };
    static service_factory_single_t<ui_element_spectrum_compare> g_spectrum_compare_factory;

    // Component version info
    DECLARE_COMPONENT_VERSION(
        "Spectrum Compare",
        "1.0",
        "Vertical spectrogram comparison panel for selected tracks. Spek-style coloring.\n\n"
        "Select one or more tracks in the playlist to view their spectrograms.\n"
        "Right-click to set display count (1-4), palette, or refresh.\n"
        "Useful for comparing audio quality and frequency content across tracks."
    );
}
