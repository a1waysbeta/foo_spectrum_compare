#include "stdafx.h"
#include "spectrum_window.h"
#include <helpers/BumpableElem.h>
#include <algorithm>
#include <cmath>

SpectrumCompareWindow::SpectrumCompareWindow(
    ui_element_config::ptr config,
    ui_element_instance_callback_ptr p_callback)
    : m_callback(p_callback)
    , m_config(config)
{
}

SpectrumCompareWindow::~SpectrumCompareWindow() {
    m_shutdown = true;

    // Stop timer
    if (m_hWnd) KillTimer(TIMER_REPAINT);

    // Unregister playlist callback
    try {
        static_api_ptr_t<playlist_manager>()->unregister_callback(this);
    } catch (...) {}

    // Wait briefly for analysis threads to notice shutdown
    // (they check m_shutdown and will exit early)
    Sleep(50);
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

    std::lock_guard<std::mutex> lock(m_tracks_mutex);

    // Determine how many tracks to display
    size_t display_count = (std::min)((size_t)m_max_tracks, selected.get_size());

    // Build new track list, reusing existing data where possible
    std::vector<std::unique_ptr<TrackSpectrum>> new_tracks;
    for (size_t i = 0; i < display_count; i++) {
        auto handle = selected[i];

        // Check if we already have this track analyzed
        bool found = false;
        for (auto& existing : m_tracks) {
            if (existing->handle == handle) {
                new_tracks.push_back(std::move(existing));
                found = true;
                break;
            }
        }

        if (!found) {
            auto track = std::make_unique<TrackSpectrum>();
            track->handle = handle;
            new_tracks.push_back(std::move(track));
        }
    }

    m_tracks = std::move(new_tracks);

    // Start analysis for tracks that need it
    for (size_t i = 0; i < m_tracks.size(); i++) {
        if (!m_tracks[i]->data.ready && !m_tracks[i]->analyzing) {
            start_analysis_for_track(i);
        }
    }

    Invalidate();
}

void SpectrumCompareWindow::start_analysis_for_track(size_t index) {
    if (index >= m_tracks.size()) return;
    auto& track = m_tracks[index];
    if (track->analyzing) return;

    track->analyzing = true;
    metadb_handle_ptr handle = track->handle;

    std::thread([this, index, handle]() {
        analysis_worker(index, handle);
    }).detach();
}

void SpectrumCompareWindow::analysis_worker(size_t index, metadb_handle_ptr handle) {
    if (m_shutdown) return;

    abort_callback_impl abort;

    // Find the track in the list (it might have been moved)
    TrackSpectrum* target = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_tracks_mutex);
        if (m_shutdown) return;
        if (index < m_tracks.size() && m_tracks[index]->handle == handle) {
            target = m_tracks[index].get();
        }
    }

    if (!target || m_shutdown) return;

    SpectrumData data;
    m_analyzer.analyze(handle, data, abort);

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

    // Calculate layout: vertical split, equal heights
    int track_count = (int)m_tracks.size();
    int label_height = 20;
    int gap = 2;
    int total_gap = gap * (track_count - 1);
    int available_height = rc.Height() - total_gap;
    int track_height = available_height / track_count;

    int y = rc.top;
    for (int i = 0; i < track_count; i++) {
        CRect track_rc(rc.left, y, rc.right, y + track_height);
        y += track_height + gap;

        // Draw label area
        CRect label_rc(track_rc.left, track_rc.top, track_rc.right, track_rc.top + label_height);
        render_track_label(dc.m_hDC, label_rc, *m_tracks[i]);

        // Draw spectrum area
        CRect spec_rc(track_rc.left, track_rc.top + label_height, track_rc.right, track_rc.bottom);
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
    }
}

void SpectrumCompareWindow::render_track_label(CDCHandle dc, const RECT& rc, const TrackSpectrum& track) {
    // Draw label background
    COLORREF bg = m_callback->query_std_color(ui_color_background);
    CBrush brush;
    brush.CreateSolidBrush(bg);
    dc.FillRect(&rc, brush);

    // Draw track title
    dc.SetTextColor(m_callback->query_std_color(ui_color_text));
    dc.SetBkMode(TRANSPARENT);
    SelectObjectScope fontScope(dc, (HGDIOBJ)m_callback->query_font_ex(ui_font_default));

    pfc::string8 label;
    label << track.data.title.c_str();
    if (track.data.sample_rate > 0) {
        label << "  [" << track.data.sample_rate << " Hz";
        if (track.data.channels > 0) label << ", " << track.data.channels << "ch";
        label << "]";
    }

    CRect text_rc(rc);
    text_rc.left += 4;
    text_rc.right -= 4;
    pfc::stringcvt::string_wide_from_utf8 label_w(label);
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

    // Draw frequency axis labels on right side
    dc.SetTextColor(m_callback->query_std_color(ui_color_text));
    dc.SetBkMode(TRANSPARENT);
    SelectObjectScope fontScope(dc, (HGDIOBJ)m_callback->query_font_ex(ui_font_default));

    if (data.sample_rate > 0 && width > 60) {
        int nyquist = data.sample_rate / 2;
        // Top label (high freq)
        pfc::string8 top_label;
        top_label << nyquist / 1000 << "k";
        pfc::stringcvt::string_wide_from_utf8 top_label_w(top_label);
        CRect top_rc(rc.right - 40, rc.top + 2, rc.right - 2, rc.top + 16);
        dc.DrawText(top_label_w, -1, &top_rc, DT_NOPREFIX | DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        // Bottom label (low freq)
        CRect bot_rc(rc.right - 40, rc.bottom - 18, rc.right - 2, rc.bottom - 4);
        dc.DrawText(_T("20"), -1, &bot_rc, DT_NOPREFIX | DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
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
    menu.AppendMenu(MF_STRING, IDM_REFRESH, _T("Refresh analysis"));

    // Track menu must outlive TrackPopupMenu
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
        update_selection();
    }
}

void SpectrumCompareWindow::OnRefresh(UINT uNotifyCode, int nID, CWindow wndCtl) {
    std::lock_guard<std::mutex> lock(m_tracks_mutex);
    for (auto& t : m_tracks) {
        t->data.ready = false;
        t->data.error = false;
    }
    for (size_t i = 0; i < m_tracks.size(); i++) {
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
    Invalidate();
}

// ============================================================
// Component registration
// ============================================================

namespace {
    class ui_element_spectrum_compare : public ui_element_impl_withpopup<SpectrumCompareWindow> {};
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
