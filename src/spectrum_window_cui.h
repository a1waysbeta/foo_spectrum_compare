#pragma once

#include "spectrum_window.h"
#include <columns_ui-sdk/ui_extension.h>

// GUID for the CUI panel extension (distinct from the DUI guid_spectrum_compare)
static const GUID guid_spectrum_compare_cui =
    { 0xb4c5d6e7, 0xf8a9, 0x0b1c, { 0x2d, 0x3e, 0x4f, 0x50, 0x61, 0x72, 0x83, 0x94 } };

// =============================================================================
// CuiSpectrumCallback
//
// Adapter that bridges the DUI ui_element_instance_callback interface to CUI's
// colour/font manager.  SpectrumCompareWindow calls m_callback->query_std_color()
// and m_callback->query_font_ex() during rendering; this class answers those calls
// using cui::colours::helper and cui::fonts::manager so the same rendering code
// works unchanged inside a Columns UI panel.
//
// Lifetime: owned by SpectrumCompareWindowCUI as a member.  The inner
// SpectrumCompareWindow holds a service_ptr to the callback service created by
// ui_element_instance_callback_get_ptr(); when the inner window is destroyed
// (destroy_window), it releases that ref, and CuiSpectrumCallback's destructor
// cleans up the service and the cached font.
// =============================================================================
class CuiSpectrumCallback : public ui_element_instance_callback_receiver
{
public:
    CuiSpectrumCallback() = default;
    ~CuiSpectrumCallback();

    // --- ui_element_instance_callback_receiver overrides ---

    // Map DUI colour GUIDs to CUI colour identifiers.
    //   ui_color_background  -> cui::colours::colour_background
    //   ui_color_text        -> cui::colours::colour_text
    //   (anything else)      -> false (host falls back to system colours)
    bool query_color(const GUID& p_what, t_ui_color& p_out) override;

    // Return the CUI default items font for ui_font_default.
    // The HFONT is cached and only recreated when invalidate_font() is called
    // (triggered by cui::fonts::common_callback::on_font_changed).
    t_ui_font query_font_ex(const GUID& p_what) override;

    // CUI has no DUI-style "edit mode"; panel context menu is always the
    // normal right-click menu.
    bool is_edit_mode_enabled() override { return false; }

    // Called by SpectrumCompareWindowCUI when a CUI font change notification
    // is received.  Deletes the cached font so the next query_font_ex() call
    // recreates it from the current CUI font settings.
    void invalidate_font() const;

private:
    // mutable so invalidate_font() (a const method, called from the const
    // common_callback::on_font_changed) can modify it.
    mutable HFONT m_cached_font = nullptr;
};

// =============================================================================
// SpectrumCompareWindowCUI
//
// A uie::window (Columns UI panel) that hosts a SpectrumCompareWindow instance.
// This is an adapter: it creates/destroys the existing DUI ui_element through
// its public API, bridging config (stream <-> ui_element_config), colours, and
// fonts.
//
// Registered via uie::window_factory<SpectrumCompareWindowCUI> as a multi-
// instance panel, so users can add it to any CUI layout slot.
// =============================================================================
class SpectrumCompareWindowCUI :
    public uie::window,
    public cui::colours::common_callback,
    public cui::fonts::common_callback
{
public:
    SpectrumCompareWindowCUI();
    ~SpectrumCompareWindowCUI();

    // --- uie::window / extension_base ---

    const GUID& get_extension_guid() const override { return guid_spectrum_compare_cui; }
    void get_name(pfc::string_base& out) const override { out = "Spectrum Compare"; }
    void get_category(pfc::string_base& out) const override { out = "Visualisations"; }
    unsigned get_type() const override { return uie::type_panel; }
    bool get_short_name(pfc::string_base& out) const override { out = "Spectrum"; return true; }
    bool get_description(pfc::string_base& out) const override {
        out = "Vertical spectrogram comparison for selected tracks (Spek-style).";
        return true;
    }
    bool get_prefer_multiple_instances() const override { return false; }
    bool is_available(const uie::window_host_ptr& p_host) const override { return true; }

    HWND create_or_transfer_window(
        HWND wnd_parent,
        const uie::window_host_ptr& p_host,
        const ui_helpers::window_position_t& p_position = ui_helpers::window_position_null) override;

    void destroy_window() override;
    HWND get_wnd() const override {
        return m_inner.is_valid() ? m_inner->get_wnd() : nullptr;
    }

    // --- config (extension_base) ---
    void set_config(stream_reader* p_reader, t_size p_size, abort_callback& p_abort) override;
    void get_config(stream_writer* p_writer, abort_callback& p_abort) const override;

    // --- cui::colours::common_callback ---
    void on_colour_changed(uint32_t changed_items_mask) const override;
    void on_bool_changed(uint32_t changed_items_mask) const override;

    // --- cui::fonts::common_callback ---
    void on_font_changed(uint32_t changed_items_mask) const override;

private:
    // CuiSpectrumCallback is declared before m_inner so that m_inner is
    // destroyed first (members are destroyed in reverse declaration order),
    // releasing the callback service before the callback receiver itself
    // is destroyed.
    CuiSpectrumCallback m_callback;
    // SpectrumCompareWindow inherits service_base (abstract); must use
    // service_impl_t to instantiate. service_ptr_t manages refcounting.
    service_ptr_t<SpectrumCompareWindow> m_inner;
    uie::window_host_ptr m_host;

    // Raw config bytes received via set_config() before the window is created.
    // On create_or_transfer_window(), these are wrapped into a ui_element_config
    // and passed to the SpectrumCompareWindow constructor.
    pfc::array_t<uint8_t> m_config_data;

    // CUI manager pointers for common callback registration/deregistration.
    cui::colours::manager::ptr m_colours_api;
    cui::fonts::manager::ptr m_fonts_api;
};
