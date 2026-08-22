<p align="right">
  <a href="README.md">简体中文</a> | <b>English</b>
</p>

# foo_spectrum_compare

<p align="center">
  <a href="../../releases"><img src="https://img.shields.io/github/v/release/a1waysbeta/foo_spectrum_compare?style=flat-square&label=Release&color=blue" alt="Latest Version"></a>
  <a href="../../releases"><img src="https://img.shields.io/github/downloads/a1waysbeta/foo_spectrum_compare/total?style=flat-square&label=Downloads&color=success" alt="Total Downloads"></a>
  <a href="../../actions"><img src="https://img.shields.io/github/actions/workflow/status/a1waysbeta/foo_spectrum_compare/build.yml?style=flat-square&label=Build&color=blue" alt="Build Status"></a>
</p>

A vertical multi-track spectrogram comparison panel component for foobar2000, designed for the Default UI.

<p align="center">
  <img src="Preview image.jpg" alt="Preview" width="800" />
</p>

## Features

- **Floating Window**: Available as a standalone floating panel via the "View" main menu (hold Shift to reveal the hidden menu item).
- **Vertical Multi-Track Comparison**: Select multiple tracks to view them in a vertically stacked layout, supporting up to 4 tracks for side-by-side frequency comparison.
- **Background Thread Analysis**: Each track is decoded and analyzed in its own thread without blocking the UI. Analysis starts instantly on selection.
- **Spek-Style Coloring**: 3 built-in palettes — Spectrum (Spek classic rainbow), SoX, and Mono.
- **Frequency Axis**: Left-side linear frequency scale (0–22 kHz), toggleable via right-click menu.
- **Time Axis**: Bottom time scale with 20-second intervals, toggleable via right-click menu.
- **Custom Title Format**: Supports foobar2000 titleformat syntax for track labels, defaulting to `%title% | %codec% | %bitrate% kbps[ | $info(bitspersample) bit] | %samplerate% Hz | $info(channels) CH | %path%`. Press Enter to confirm edits without reloading the spectrum.
- **DPI Aware**: Properly scales on 4K and other high-DPI displays — spacing, fonts, and axes adapt automatically.
- **Double-Buffered Rendering**: Full double-buffered output ensures flicker-free painting during track switching.
- **Bilingual UI**: Right-click menu supports Chinese / English switching, with language persisted in panel configuration.
- **Configuration Persistence**: Panel settings serialized via .fth format.

> This project is developed with AI assistance. Contributors who enjoy this feature are welcome to help improve it.

## License

This component is provided as-is for personal use with foobar2000.

## Acknowledgements

- **[Spek](https://github.com/alexkay/spek)** — Spectrum color scheme and frequency/time axis design reference.
- **[foobar2000 SDK](https://www.foobar2000.org/SDK)** — foobar2000 plugin development framework.
- **[WTL](https://github.com/Win32-WTL/WTL)** — Windows Template Library for window and UI implementation.
- **[foo_nowbar](https://github.com/jame25/foo_nowbar)** — README layout and bilingual toggle design reference.
- **TRAE AI Coding Assistant** — Code implementation, debugging, build fixes, and refactoring.
- **always beta** — Product design, requirements, UI aesthetics, and integration testing.
