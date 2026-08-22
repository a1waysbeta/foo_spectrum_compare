<p align="right">
  <b>简体中文</b> | <a href="README_EN.md">English</a>
</p>

# foo_spectrum_compare

<p align="center">
  <a href="../../releases"><img src="https://img.shields.io/github/v/release/a1waysbeta/foo_spectrum_compare?style=flat-square&label=最新版本&color=blue" alt="最新版本"></a>
  <a href="../../releases"><img src="https://img.shields.io/github/downloads/a1waysbeta/foo_spectrum_compare/total?style=flat-square&label=下载次数&color=success" alt="下载次数"></a>
  <a href="../../actions"><img src="https://img.shields.io/github/actions/workflow/status/a1waysbeta/foo_spectrum_compare/build.yml?style=flat-square&label=编译状态&color=blue" alt="编译状态"></a>
</p>

一个为 foobar2000 设计的垂直多轨频谱对比面板组件，适用于默认界面（Default UI）。

<p align="center">
  <img src="Preview image.jpg" alt="Preview" width="800" />
</p>

## 功能特性

- **浮窗面板**：支持作为独立浮窗面板使用，通过主菜单「视图」打开（按住 Shift 显示隐藏菜单项）。
- **垂直多轨对比**：选中多首音轨后垂直等分布局，最多支持 4 首音轨同时对比频率特征。
- **后台线程分析**：每首音轨独立线程解码与 FFT 分析，不阻塞 UI 界面。选中即分析，秒级加载。
- **Spek 风格配色**：内置 3 种调色板 — Spectrum（Spek 经典彩虹）、SoX、Mono（单色）。
- **频率轴标注**：左侧显示 0–22 kHz 线性频率刻度，可在右键菜单中切换显示。
- **时间轴标注**：底部显示 20 秒间隔的时间刻度，可在右键菜单中切换显示。
- **自定义标题格式**：支持 foobar2000 titleformat 语法自定义音轨标签，默认显示 `%title% | %codec% | %bitrate% kbps[ | $info(bitspersample) bit] | %samplerate% Hz | $info(channels) CH | %path%`。右键编辑格式后回车确认，不重新加载频谱。
- **DPI 感知**：在 4K 等高分辨率屏幕上正确缩放，间距、字体、坐标轴均自适应。
- **双缓冲渲染**：整体画面双缓冲输出，选歌切换时无闪烁。
- **双语界面**：右键菜单支持中文 / English 一键切换，语言设置随面板配置持久化。
- **配置持久化**：面板设置通过 .fth 格式序列化。

> 本项目借助 AI 辅助开发，欢迎喜欢该功能的用户参与协作完善。

## 许可证

本组件以原样提供，供 foobar2000 个人使用。

## 致谢

- **[Spek](https://github.com/alexkay/spek)** — 频谱配色方案与频率/时间轴设计参考。
- **[foobar2000 SDK](https://www.foobar2000.org/SDK)** — foobar2000 插件开发框架。
- **[WTL](https://github.com/Win32-WTL/WTL)** — Windows Template Library，窗口与 UI 实现。
- **[foo_nowbar](https://github.com/jame25/foo_nowbar)** — README 排版与双语切换设计参考。
- **TRAE AI Coding Assistant** — 代码实现、调试、编译修复与重构。
- **always beta** — 产品设计、需求定义、UI 美学优化与集成测试。
