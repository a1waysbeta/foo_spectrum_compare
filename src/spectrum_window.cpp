#include "stdafx.h"
#include "spectrum_window.h"
#include <algorithm>
#include <cmath>

// Static window class registration flag
bool SpectrumCompareWindow::s_classRegistered = false;
static const wchar_t* SPECTRUM_WND_CLASS = L"SpectrumCompareWndClass";

// ============================================================================
// Constructor / Destructor
// ============================================================================

SpectrumCompareWindow::SpectrumCompareWindow(ui_element_config::ptr config, ui_element_instance_callback_ptr p_callback)
    : m_config(config), m_callback(p_callback)
{
}

SpectrumCompareWindow::~SpectrumCompareWindow()
{
    m_shutdown = true;

    if (m_hWnd) {
        KillTimer(m_hWnd, TIMER_REPAINT);
        static_api_ptr_t<playlist_manager>()->unregister_callback(this);
        DestroyWindow(m_hWnd);
        m_hWnd = NULL;
    }

    // Wait for any running analysis threads
    std::lock_guard<std::mutex> lock(m_tracks_mutex);
    m_tracks.clear();
}

// ============================================================================
// Window creation & procedure
// ============================================================================

void SpectrumCompareWindow::initialize_window(HWND parent)
{
    // Register window class once
    if (!s_classRegistered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_VREDRAW | CS_HREDRAW;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL; // We handle WM_ERASEBKGND
        wc.lpszClassName = SPECTRUM_WND_CLASS;
        RegisterClassExW(&wc);
        s_classRegistered = true;
    }

    // Create the window, pass 'this' as create param
    m_hWnd = CreateWindowExW(
        0,
        SPECTRUM_WND_CLASS,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 100, 100,
        parent,
        NULL,
        GetModuleHandleW(NULL),
        this
    );

    // Register playlist callback
    static_api_ptr_t<playlist_manager>()->register_callback(
        this,
        playlist_callback_single::flag_on_items_selection_change |
        playlist_callback_single::flag_on_playlist_switch |
        playlist_callback_single::flag_on_items_added |
        playlist_callback_single::flag_on_items_removed
    );

    // Start repaint timer (for gradual updates during analysis)
    SetTimer(m_hWnd, TIMER_REPAINT, 250, NULL);

    // Initial selection
    update_selection();
}

LRESULT CALLBACK SpectrumCompareWindow::WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    SpectrumCompareWindow* pThis = NULL;

    if (uMsg == WM_NCCREATE) {
        pThis = (SpectrumCompareWindow*)((LPCREATESTRUCTW)lParam)->lpCreateParams;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)pThis);
        pThis->m_hWnd = hWnd;
    } else {
        pThis = (SpectrumCompareWindow*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    }

    if (pThis) {
        return pThis->HandleMessage(uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT SpectrumCompareWindow::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_ERASEBKGND:
        return 1; // We draw everything in WM_PAINT

    case WM_PAINT:
        OnPaint();
        return 0;

    case WM_SIZE:
        OnSize();
        return 0;

    case WM_CONTEXTMENU:
        OnContextMenu(POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;

    case WM_TIMER:
        OnTimer((UINT_PTR)wParam);
        return 0;

    case WM_COMMAND:
        OnCommand(LOWORD(wParam));
        return 0;

    case WM_SPECTRUM_READY:
        InvalidateRect(m_hWnd, NULL, FALSE);
        return 0;

    case WM_NCDESTROY:
        m_hWnd = NULL;
        return DefWindowProcW(m_hWnd, uMsg, wParam, lParam);

    default:
        return DefWindowProcW(m_hWnd, uMsg, wParam, lParam);
    }
}

// ============================================================================
// Message handlers
// ============================================================================

void SpectrumCompareWindow::OnSize()
{
    InvalidateRect(m_hWnd, NULL, FALSE);
}

void SpectrumCompareWindow::OnTimer(UINT_PTR nIDEvent)
{
    if (nIDEvent == TIMER_REPAINT) {
        bool need_repaint = false;
        {
            std::lock_guard<std::mutex> lock(m_tracks_mutex);
            for (auto& t : m_tracks) {
                if (t->needs_repaint.exchange(false)) {
                    need_repaint = true;
                }
            }
        }
        if (need_repaint) {
            InvalidateRect(m_hWnd, NULL, FALSE);
        }
    }
}

void SpectrumCompareWindow::OnCommand(int id)
{
    switch (id) {
    case IDM_SET_COUNT_1: m_max_tracks = 1; update_selection(); break;
    case IDM_SET_COUNT_2: m_max_tracks = 2; update_selection(); break;
    case IDM_SET_COUNT_3: m_max_tracks = 3; update_selection(); break;
    case IDM_SET_COUNT_4: m_max_tracks = 4; update_selection(); break;
    case IDM_REFRESH: update_selection(); break;
    case IDM_PALETTE_SPECTRUM: m_palette = PALETTE_SPECTRUM; InvalidateRect(m_hWnd, NULL, FALSE); break;
    case IDM_PALETTE_SOX: m_palette = PALETTE_SOX; InvalidateRect(m_hWnd, NULL, FALSE); break;
    case IDM_PALETTE_MONO: m_palette = PALETTE_MONO; InvalidateRect(m_hWnd, NULL, FALSE); break;
    }
}

void SpectrumCompareWindow::OnContextMenu(POINT pt)
{
    HMENU hMenu = CreatePopupMenu();

    // Display count submenu
    HMENU hCountMenu = CreatePopupMenu();
    UINT countFlags[4] = { MF_STRING, MF_STRING, MF_STRING, MF_STRING };
    countFlags[m_max_tracks - 1] |= MF_CHECKED;
    AppendMenuW(hCountMenu, countFlags[0], IDM_SET_COUNT_1, L"1 track");
    AppendMenuW(hCountMenu, countFlags[1], IDM_SET_COUNT_2, L"2 tracks");
    AppendMenuW(hCountMenu, countFlags[2], IDM_SET_COUNT_3, L"3 tracks");
    AppendMenuW(hCountMenu, countFlags[3], IDM_SET_COUNT_4, L"4 tracks");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hCountMenu, L"Display count");

    // Palette submenu
    HMENU hPaletteMenu = CreatePopupMenu();
    UINT palFlags[3] = { MF_STRING, MF_STRING, MF_STRING };
    palFlags[(int)m_palette] |= MF_CHECKED;
    AppendMenuW(hPaletteMenu, palFlags[0], IDM_PALETTE_SPECTRUM, L"Spectrum (Spek)");
    AppendMenuW(hPaletteMenu, palFlags[1], IDM_PALETTE_SOX, L"SoX");
    AppendMenuW(hPaletteMenu, palFlags[2], IDM_PALETTE_MONO, L"Mono");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hPaletteMenu, L"Palette");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_REFRESH, L"Refresh");

    // Convert screen coordinates if needed
    if (pt.x == -1 && pt.y == -1) {
        RECT rc;
        GetClientRect(m_hWnd, &rc);
        pt.x = (rc.left + rc.right) / 2;
        pt.y = (rc.top + rc.bottom) / 2;
        ClientToScreen(m_hWnd, &pt);
    }

    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hWnd, NULL);

    DestroyMenu(hCountMenu);
    DestroyMenu(hPaletteMenu);
    DestroyMenu(hMenu);
}

// ============================================================================
// Painting
// ============================================================================

void SpectrumCompareWindow::OnPaint()
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(m_hWnd, &ps);

    RECT rcClient;
    GetClientRect(m_hWnd, &rcClient);

    // Background
    HBRUSH hBgBrush = CreateSolidBrush(RGB(20, 20, 24));
    FillRect(hdc, &rcClient, hBgBrush);
    DeleteObject(hBgBrush);

    std::lock_guard<std::mutex> lock(m_tracks_mutex);
    size_t count = m_tracks.size();

    if (count == 0) {
        // Empty state text
        SetTextColor(hdc, RGB(140, 140, 150));
        SetBkMode(hdc, TRANSPARENT);
        HFONT hFont = (HFONT)m_callback->query_font_ex(ui_font_default);
        HGDIOBJ hOldFont = SelectObject(hdc, hFont);
        DrawTextW(hdc, L"Select tracks in the playlist to view their spectrograms.", -1, &rcClient, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, hOldFont);
        EndPaint(m_hWnd, &ps);
        return;
    }

    // Calculate track heights (equal distribution)
    int labelHeight = 22;
    int totalHeight = rcClient.bottom - rcClient.top;
    int trackHeight = totalHeight / (int)count;

    for (size_t i = 0; i < count; i++) {
        RECT rcTrack;
        rcTrack.left = rcClient.left;
        rcTrack.right = rcClient.right;
        rcTrack.top = rcClient.top + (int)i * trackHeight;
        rcTrack.bottom = (i == count - 1) ? rcClient.bottom : rcTrack.top + trackHeight;

        // Label area
        RECT rcLabel = rcTrack;
        rcLabel.bottom = rcLabel.top + labelHeight;

        // Spectrum area
        RECT rcSpec = rcTrack;
        rcSpec.top = rcLabel.bottom;

        // Draw label
        render_track_label(hdc, rcLabel, *m_tracks[i]);

        // Draw spectrum
        if (m_tracks[i]->data.time_frames > 0 && m_tracks[i]->data.freq_bins > 0) {
            render_spectrum(hdc, rcSpec, m_tracks[i]->data);
        } else if (m_tracks[i]->analyzing.load()) {
            // "Analyzing..." text
            SetTextColor(hdc, RGB(120, 120, 130));
            SetBkMode(hdc, TRANSPARENT);
            HFONT hFont = (HFONT)m_callback->query_font_ex(ui_font_default);
            HGDIOBJ hOldFont = SelectObject(hdc, hFont);
            DrawTextW(hdc, L"Analyzing...", -1, &rcSpec, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, hOldFont);
        }

        // Separator line
        if (i < count - 1) {
            HPEN hPen = CreatePen(PS_SOLID, 1, RGB(50, 50, 58));
            HGDIOBJ hOldPen = SelectObject(hdc, hPen);
            MoveToEx(hdc, rcTrack.left, rcTrack.bottom, NULL);
            LineTo(hdc, rcTrack.right, rcTrack.bottom);
            SelectObject(hdc, hOldPen);
            DeleteObject(hPen);
        }
    }

    EndPaint(m_hWnd, &ps);
}

void SpectrumCompareWindow::render_track_label(HDC hdc, const RECT& rc, const TrackSpectrum& track)
{
    // Background
    HBRUSH hBrush = CreateSolidBrush(RGB(32, 32, 38));
    FillRect(hdc, &rc, hBrush);
    DeleteObject(hBrush);

    // Get track title
    pfc::string8 title;
    if (track.handle.is_valid()) {
        track.handle->query_meta_field("title", 0, title);
        if (title.is_empty()) {
            // Fallback to filename
            pfc::string8 path = track.handle->get_path();
            const char* fn = pfc::string_filename(path);
            title = fn;
        }
    } else {
        title = "Unknown";
    }

    // Convert to wide string for DrawText
    int wlen = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, NULL, 0);
    std::wstring wtitle(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, &wtitle[0], wlen);

    // Text
    SetTextColor(hdc, RGB(220, 220, 230));
    SetBkMode(hdc, TRANSPARENT);
    HFONT hFont = (HFONT)m_callback->query_font_ex(ui_font_default);
    HGDIOBJ hOldFont = SelectObject(hdc, hFont);

    RECT rcText = rc;
    rcText.left += 6;
    rcText.right -= 6;
    DrawTextW(hdc, wtitle.c_str(), -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, hOldFont);
}

void SpectrumCompareWindow::render_spectrum(HDC hdc, const RECT& rc, const SpectrumData& data)
{
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    if (width <= 0 || height <= 0) return;

    // Create DIB section for the spectrogram
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;

    BYTE* pBits = NULL;
    HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void**)&pBits, NULL, 0);
    if (!hBmp || !pBits) return;

    HDC hMemDC = CreateCompatibleDC(hdc);
    HGDIOBJ hOldBmp = SelectObject(hMemDC, hBmp);

    // Fill with background
    memset(pBits, 16, (size_t)width * height * 3);

    // Render spectrogram pixels
    const auto& palette = get_palette(m_palette);
    int palette_size = (int)palette.size();

    for (int y = 0; y < height; y++) {
        // Map y to frequency bin (top = high freq, bottom = low freq)
        double freq_norm = 1.0 - (double)y / (double)height;
        // Log frequency mapping (like Spek)
        double log_freq = freq_norm; // linear for now, can adjust
        int freq_idx = (int)(log_freq * (data.freq_bins - 1));
        freq_idx = std::max(0, std::min((int)data.freq_bins - 1, freq_idx));

        BYTE* row = pBits + (size_t)y * width * 3;

        for (int x = 0; x < width; x++) {
            int time_idx = (int)((double)x / (double)width * (data.time_frames - 1));
            time_idx = std::max(0, std::min((int)data.time_frames - 1, time_idx));

            double val = data.data[freq_idx * data.time_frames + time_idx];
            // val is 0..1 (normalized dB)
            int pal_idx = (int)(val * (palette_size - 1));
            pal_idx = std::max(0, std::min(palette_size - 1, pal_idx));

            const auto& c = palette[pal_idx];
            row[x * 3 + 0] = (BYTE)(c.b * 255);
            row[x * 3 + 1] = (BYTE)(c.g * 255);
            row[x * 3 + 2] = (BYTE)(c.r * 255);
        }
    }

    // Blit to screen
    BitBlt(hdc, rc.left, rc.top, width, height, hMemDC, 0, 0, SRCCOPY);

    SelectObject(hMemDC, hOldBmp);
    DeleteDC(hMemDC);
    DeleteObject(hBmp);
}

// ============================================================================
// Selection management
// ============================================================================

void SpectrumCompareWindow::update_selection()
{
    static_api_ptr_t<playlist_manager> pm;
    metadb_handle_list items;
    bit_array_bittable mask(pm->activeplaylist_get_item_count());
    pm->activeplaylist_get_selected_items(items);

    std::lock_guard<std::mutex> lock(m_tracks_mutex);

    // Limit to max_tracks
    size_t count = std::min((size_t)items.get_count(), (size_t)m_max_tracks);

    // Check if selection changed
    bool changed = false;
    if (m_tracks.size() != count) {
        changed = true;
    } else {
        for (size_t i = 0; i < count; i++) {
            if (m_tracks[i]->handle != items[i]) {
                changed = true;
                break;
            }
        }
    }

    if (!changed) return;

    // Rebuild tracks
    m_shutdown = false;
    m_tracks.clear();

    for (size_t i = 0; i < count; i++) {
        auto track = std::make_unique<TrackSpectrum>();
        track->handle = items[i];
        m_tracks.push_back(std::move(track));
    }

    InvalidateRect(m_hWnd, NULL, FALSE);

    // Start analysis for each track
    for (size_t i = 0; i < m_tracks.size(); i++) {
        start_analysis_for_track(i);
    }
}

void SpectrumCompareWindow::start_analysis_for_track(size_t index)
{
    if (index >= m_tracks.size()) return;
    if (m_tracks[index]->analyzing.exchange(true)) return; // already running

    metadb_handle_ptr handle = m_tracks[index]->handle;

    // Launch analysis in a detached thread
    std::thread([this, index, handle]() {
        analysis_worker(index, handle);
    }).detach();
}

void SpectrumCompareWindow::analysis_worker(size_t index, metadb_handle_ptr handle)
{
    try {
        SpectrumData result = m_analyzer.analyze(handle);

        if (m_shutdown.load()) return;

        {
            std::lock_guard<std::mutex> lock(m_tracks_mutex);
            if (index < m_tracks.size() && m_tracks[index]->handle == handle) {
                m_tracks[index]->data = std::move(result);
                m_tracks[index]->analyzing = false;
                m_tracks[index]->needs_repaint = true;
            }
        }

        // Notify main thread
        if (m_hWnd) {
            PostMessageW(m_hWnd, WM_SPECTRUM_READY, 0, 0);
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_tracks_mutex);
        if (index < m_tracks.size()) {
            m_tracks[index]->analyzing = false;
        }
    }
}

// ============================================================================
// ui_element_instance::notify
// ============================================================================

void SpectrumCompareWindow::notify(const GUID& p_what, t_size p_param1, const void* p_param2, t_size p_param2size)
{
    if (p_what == ui_element_notify_colors_changed || p_what == ui_element_notify_font_changed) {
        if (m_hWnd) InvalidateRect(m_hWnd, NULL, FALSE);
    }
}

// ============================================================================
// playlist_callback_single stubs
// ============================================================================

void SpectrumCompareWindow::on_items_added(t_size, metadb_handle_list_cref, const bit_array&) {}
void SpectrumCompareWindow::on_items_reordered(const t_size*, t_size) {}
void SpectrumCompareWindow::on_items_removing(const bit_array&, t_size, t_size) {}
void SpectrumCompareWindow::on_items_removed(const bit_array&, t_size, t_size) {}

void SpectrumCompareWindow::on_items_selection_change(const bit_array&, const bit_array&)
{
    update_selection();
}

void SpectrumCompareWindow::on_item_focus_change(t_size, t_size) {}
void SpectrumCompareWindow::on_items_modified(const bit_array&) {}
void SpectrumCompareWindow::on_items_modified_fromplayback(const bit_array&, play_control::t_display_level) {}
void SpectrumCompareWindow::on_items_replaced(const bit_array&, const pfc::list_base_const_t<playlist_callback::t_on_items_replaced_entry>&) {}
void SpectrumCompareWindow::on_item_ensure_visible(t_size) {}

void SpectrumCompareWindow::on_playlist_switch()
{
    update_selection();
}

void SpectrumCompareWindow::on_playlist_renamed(const char*, t_size) {}
void SpectrumCompareWindow::on_playlist_locked(bool) {}
void SpectrumCompareWindow::on_default_format_changed() {}
void SpectrumCompareWindow::on_playback_order_changed(t_size) {}

// ============================================================================
// Component registration
// ============================================================================

class SpectrumCompareUIElement : public ui_element_impl_withpopup<SpectrumCompareWindow> {};

static service_factory_single_t<SpectrumCompareUIElement> g_spectrum_compare_factory;

DECLARE_COMPONENT_VERSION(
    "Spectrum Compare",
    "1.0.0",
    "Vertical spectrogram comparison panel for selected tracks (Spek-style palette).\n"
    "Select one or more tracks in the playlist to view their spectrograms side by side.\n"
    "Right-click to change display count (1-4) and palette."
);

VALIDATE_COMPONENT_FILENAME("foo_spectrum_compare.dll");
