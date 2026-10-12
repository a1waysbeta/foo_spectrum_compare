#include "stdafx.h"
#include "spectrum_window_cui.h"

// =============================================================================
// CuiSpectrumCallback
// =============================================================================

CuiSpectrumCallback::~CuiSpectrumCallback()
{
    if (m_cached_font) {
        DeleteObject(m_cached_font);
        m_cached_font = nullptr;
    }
    // ui_element_instance_callback_receiver destructor calls
    // ui_element_instance_callback_release() which orphans and releases
    // the callback service.
}

bool CuiSpectrumCallback::query_color(const GUID& p_what, t_ui_color& p_out)
{
    // Map DUI colour GUIDs to CUI colour identifiers.
    cui::colours::colour_identifier_t id;
    if (p_what == ui_color_background) {
        id = cui::colours::colour_background;
    } else if (p_what == ui_color_text) {
        id = cui::colours::colour_text;
    } else {
        return false;  // unknown colour — host falls back to system colours
    }

    cui::colours::helper helper;  // global colours (null GUID)
    p_out = helper.get_colour(id);
    return true;
}

t_ui_font CuiSpectrumCallback::query_font_ex(const GUID& p_what)
{
    (void)p_what;  // we only handle ui_font_default

    if (m_cached_font) return m_cached_font;

    try {
        auto api = fb2k::std_api_get<cui::fonts::manager>();
        m_cached_font = api->get_font(cui::fonts::font_type_items);
    } catch (...) {
        m_cached_font = nullptr;
    }
    return m_cached_font;
}

void CuiSpectrumCallback::invalidate_font() const
{
    if (m_cached_font) {
        DeleteObject(m_cached_font);
        m_cached_font = nullptr;
    }
}

// =============================================================================
// SpectrumCompareWindowCUI
// =============================================================================

SpectrumCompareWindowCUI::SpectrumCompareWindowCUI()
{
    // Register for global CUI colour and font change notifications.
    try {
        m_colours_api = fb2k::std_api_get<cui::colours::manager>();
        m_colours_api->register_common_callback(this);
    } catch (...) {}

    try {
        m_fonts_api = fb2k::std_api_get<cui::fonts::manager>();
        m_fonts_api->register_common_callback(this);
    } catch (...) {}
}

SpectrumCompareWindowCUI::~SpectrumCompareWindowCUI()
{
    // Deregister common callbacks before the object is destroyed.
    if (m_colours_api.is_valid()) {
        m_colours_api->deregister_common_callback(this);
    }
    if (m_fonts_api.is_valid()) {
        m_fonts_api->deregister_common_callback(this);
    }

    // Ensure the inner window is destroyed before m_callback.
    // destroy_window() should have been called by the host already, but
    // guard against the host not doing so.
    m_inner.reset();
}

HWND SpectrumCompareWindowCUI::create_or_transfer_window(
    HWND wnd_parent,
    const uie::window_host_ptr& p_host,
    const ui_helpers::window_position_t& p_position)
{
    m_host = p_host;

    // Build the ui_element_config from the stored raw bytes, or use defaults.
    ui_element_config::ptr config;
    if (m_config_data.get_size() > 0) {
        config = ui_element_config::g_create(
            guid_spectrum_compare, m_config_data.get_ptr(), m_config_data.get_size());
    } else {
        config = SpectrumCompareWindow::g_get_default_configuration();
    }

    // Construct the inner SpectrumCompareWindow with the CUI callback adapter.
    // ui_element_instance_callback_get_ptr() creates a service object wrapping
    // this CuiSpectrumCallback; the inner window holds a ref to it.
    m_inner = std::make_unique<SpectrumCompareWindow>(
        config, m_callback.ui_element_instance_callback_get_ptr());

    // Create the actual HWND (ATL CWindowImpl::Create).
    m_inner->initialize_window(wnd_parent);

    // Apply initial position if provided.
    HWND hwnd = m_inner->get_wnd();
    if (hwnd && p_position.cx > 0 && p_position.cy > 0) {
        ::SetWindowPos(hwnd, nullptr,
            p_position.x, p_position.y, p_position.cx, p_position.cy,
            SWP_NOZORDER);
    }

    return hwnd;
}

void SpectrumCompareWindowCUI::destroy_window()
{
    // Save the current configuration before destroying the inner window,
    // so get_config() can return valid data even after the panel is removed
    // from the layout (e.g. undo/redo).
    if (m_inner) {
        try {
            auto config = m_inner->get_configuration();
            t_size size = config->get_data_size();
            m_config_data.set_size(size);
            if (size > 0) {
                memcpy(m_config_data.get_ptr(), config->get_data(), size);
            }
        } catch (...) {}

        // Destroy the inner window first.  This releases the inner window's
        // ref to the callback service while CuiSpectrumCallback is still alive
        // (it's a member declared before m_inner, so it outlives m_inner).
        m_inner.reset();
    }

    m_host.release();
}

void SpectrumCompareWindowCUI::set_config(stream_reader* p_reader, t_size p_size, abort_callback& p_abort)
{
    // Store raw config bytes; they'll be interpreted as a ui_element_config
    // blob (same format as DUI) when the window is created.
    m_config_data.set_size(p_size);
    if (p_size > 0 && p_reader) {
        p_reader->read(m_config_data.get_ptr(), p_size, p_abort);
    }
}

void SpectrumCompareWindowCUI::get_config(stream_writer* p_writer, abort_callback& p_abort) const
{
    if (m_inner) {
        // Live window: serialize current runtime config.
        auto config = m_inner->get_configuration();
        p_writer->write(config->get_data(), config->get_data_size(), p_abort);
    } else if (m_config_data.get_size() > 0) {
        // No live window: return the last stored config.
        p_writer->write(m_config_data.get_ptr(), m_config_data.get_size(), p_abort);
    }
}

// --- cui::colours::common_callback ---

void SpectrumCompareWindowCUI::on_colour_changed(uint32_t changed_items_mask) const
{
    (void)changed_items_mask;
    if (m_inner && m_inner->get_wnd()) {
        InvalidateRect(m_inner->get_wnd(), nullptr, FALSE);
    }
}

void SpectrumCompareWindowCUI::on_bool_changed(uint32_t changed_items_mask) const
{
    // Dark mode toggle is a bool change — repaint to pick up new colours.
    (void)changed_items_mask;
    if (m_inner && m_inner->get_wnd()) {
        InvalidateRect(m_inner->get_wnd(), nullptr, FALSE);
    }
}

// --- cui::fonts::common_callback ---

void SpectrumCompareWindowCUI::on_font_changed(uint32_t changed_items_mask) const
{
    (void)changed_items_mask;
    // Invalidate the cached font so the next query_font_ex() recreates it.
    m_callback.invalidate_font();
    if (m_inner && m_inner->get_wnd()) {
        InvalidateRect(m_inner->get_wnd(), nullptr, FALSE);
    }
}

// =============================================================================
// CUI panel registration
// =============================================================================
static uie::window_factory<SpectrumCompareWindowCUI> g_spectrum_compare_cui_factory;
