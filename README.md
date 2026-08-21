# foo_spectrum_compare - foobar2000 频谱对比插件

[![Build](https://github.com/YOUR_USERNAME/foo_spectrum_compare/actions/workflows/build.yml/badge.svg)](https://github.com/YOUR_USERNAME/foo_spectrum_compare/actions/workflows/build.yml)

一个 foobar2000 UI 面板插件，用于垂直对比多首音轨的声学频谱图。配色方案借鉴自 Spek / Spek-X。

## 功能特性

- **选中即显示**：在播放列表中选中音轨时，面板自动显示该音轨的频谱图
- **多轨垂直对比**：选择多首音轨时，面板内垂直布局等分显示对应频谱
- **最多 4 首**：选中音轨过多时，仅显示前 4 首（可右键设置为 1/2/3/4）
- **Spek 风格配色**：默认使用 Spek 的 spectrum 调色板，也可切换 SoX 或单色
- **右键菜单**：支持设置显示数量、切换调色板、刷新分析
- **后台分析**：使用独立线程解码和分析，不阻塞 UI
- **自适应布局**：频谱高度根据面板高度自动等分

## 下载安装

### 从 GitHub Actions 下载（推荐）

本项目配置了 GitHub Actions 自动编译，每次推送代码都会自动构建：

1. 打开仓库的 **Actions** 页面
2. 点击最新的成功的 **Build** 工作流
3. 在页面底部的 **Artifacts** 区域下载：
   - `foo_spectrum_compare-x64` — 64 位版本
   - `foo_spectrum_compare-Win32` — 32 位版本
4. 解压下载的 zip，得到 `.fb2k-component` 文件
5. 双击该文件，foobar2000 会自动提示安装
6. 重启 foobar2000

### 从 Release 下载

当推送 `v*` 标签时，GitHub Actions 会自动创建 Release 并附加编译产物：

1. 打开仓库的 **Releases** 页面
2. 下载最新版本的 `.fb2k-component` 文件
3. 双击安装

### 手动编译

见下方 [构建说明](#构建说明)。

## 使用说明

### 添加面板

1. 在 foobar2000 中启用布局编辑模式（视图 → 布局 → 启用布局编辑模式）
2. 在任意面板位置右键 → 替换 UI 元素 → 工具 → Spectrum Compare
3. 或者在 Columns UI 等界面中添加 "Spectrum Compare" 面板

### 基本操作

1. 在播放列表中点击选中一首音轨 → 面板显示其频谱图
2. 按住 Ctrl 或 Shift 多选音轨 → 面板垂直排列显示多首频谱
3. 选中超过 4 首时，仅显示前 4 首

### 右键菜单

在频谱面板上右键：
- **Display count**：设置同时显示的音轨数量（1 / 2 / 3 / 4）
- **Palette**：切换配色方案
  - Spectrum (Spek)：默认的 Spek 风格彩虹色
  - SoX：SoX 工具的默认配色
  - Mono：单色灰度
- **Refresh analysis**：重新分析当前选中的音轨

### 频谱图说明

- **横轴**：时间（左为曲目开头，右为结尾）
- **纵轴**：频率（下为低频，上为高频，右上角标注奈奎斯特频率）
- **颜色**：能量强度（蓝/黑为低能量，红/黄为高能量）
- 每首音轨上方显示标题、采样率和声道数

## 项目结构

```
foo_spectrum_compare/
├── .github/
│   └── workflows/
│       └── build.yml              # GitHub Actions 自动编译配置
├── sdk/                            # foobar2000 SDK（已包含，无需额外下载）
│   ├── foobar2000/
│   │   ├── SDK/                   # SDK 头文件和源文件
│   │   ├── helpers/               # 辅助头文件
│   │   ├── shared/                # 共享库（含预编译 .lib）
│   │   └── foobar2000_component_client/
│   ├── pfc/                       # PFC 库
│   ├── libPPUI/                   # libPPUI 库
│   ├── sdk-license.txt
│   └── sdk-readme.html
├── src/                            # 插件源码
│   ├── stdafx.h                    # 预编译头
│   ├── PCH.cpp                     # 预编译头生成
│   ├── main.cpp                    # 组件入口
│   ├── fft.h / fft.cpp            # Cooley-Tukey radix-2 FFT 实现
│   ├── palette.h / palette.cpp    # 调色板（从 Spek 借鉴）
│   ├── spectrum_analyzer.h / .cpp # 频谱分析核心
│   └── spectrum_window.h / .cpp   # UI 面板窗口
├── foo_spectrum_compare.vcxproj    # Visual Studio 项目文件
├── foo_spectrum_compare.vcxproj.filters
├── foo_spectrum_compare.sln        # Visual Studio 解决方案
├── .gitignore
├── LICENSE
└── README.md
```

## 构建说明

### 前置要求

- Visual Studio 2022（v143 工具集）或更高版本
- Windows SDK 10.0
- foobar2000 SDK 已包含在仓库的 `sdk/` 目录中

### 使用 Visual Studio 编译

1. 打开 `foo_spectrum_compare.sln`
2. 选择配置：`Release` 和平台 `x64`（或 `Win32`）
3. 生成 → 生成解决方案
4. 编译产物：
   - x64: `x64\Release\foo_spectrum_compare.dll`
   - Win32: `Release\foo_spectrum_compare.dll`

### 使用命令行编译（MSBuild）

```powershell
# 64 位
msbuild /m /p:Configuration=Release /p:Platform=x64 foo_spectrum_compare.sln

# 32 位
msbuild /m /p:Configuration=Release /p:Platform=Win32 foo_spectrum_compare.sln
```

### 打包为 foobar2000 组件

将编译好的 DLL 重命名为 `.fb2k-component`：

```powershell
# x64
copy x64\Release\foo_spectrum_compare.dll foo_spectrum_compare-x64.fb2k-component

# Win32
copy Release\foo_spectrum_compare.dll foo_spectrum_compare-Win32.fb2k-component
```

## GitHub Actions 自动编译

本项目配置了 GitHub Actions，在以下情况自动编译：

- 推送到 `main` 或 `master` 分支
- 创建 Pull Request
- 推送 `v*` 标签（同时自动创建 Release）

工作流会同时编译 **x64** 和 **Win32** 两个平台，并将 `.fb2k-component` 文件作为 Artifact 上传。

工作流配置文件：`.github/workflows/build.yml`

## 技术实现

### 频谱分析流程

1. **音频解码**：使用 foobar2000 的 `input_helper` 解码选中的音轨
2. **声道混合**：将多声道音频混合为单声道
3. **分帧加窗**：使用 Hann 窗（默认）对音频分帧，2048 点 FFT
4. **FFT 变换**：自实现的 Cooley-Tukey radix-2 FFT（无外部依赖）
5. **幅度谱**：计算 FFT 输出的幅度
6. **时间轴压缩**：根据目标时间分辨率（默认 800 帧）调整 hop size
7. **动态范围**：使用 80dB 动态范围，对数刻度映射到调色板

### 线程模型

- 主线程：UI 绘制、播放列表事件处理
- 工作线程：每首音轨的解码和分析在独立线程中进行
- 线程安全：使用 `std::mutex` 保护共享数据，`std::atomic` 标志控制关闭
- 结果回传：工作线程完成后通过 `PostMessage` 通知主线程重绘

### 与 Spek 的关系

- **调色板**：`palette.cpp` 直接从 Spek-X 源码移植（spectrum / sox / mono 三种）
- **窗函数**：Hann / Hamming / Blackman-Harris 三种窗函数，与 Spek 一致
- **FFT**：自实现 radix-2 FFT，替代 Spek 使用的 FFmpeg av_fft
- **音频解码**：使用 foobar2000 原生解码，替代 Spek 使用的 FFmpeg/libav
- **UI 框架**：使用 foobar2000 UI element + Win32 GDI，替代 Spek 的 wxWidgets

## 自定义参数

如需调整频谱分析参数，修改 `src/spectrum_analyzer.h` 中的默认值：

```cpp
m_fft_bits = 11;        // FFT 点数 = 2^11 = 2048
m_window = WINDOW_HANN;  // 窗函数
m_target_frames = 800;   // 目标时间帧数
```

动态范围在 `src/spectrum_window.cpp` 的 `render_spectrum` 中：
```cpp
float dyn_range = 80.0f; // dB
```

## 已知限制

- 首次分析较长音轨可能需要几秒时间（取决于 CPU 和音频长度）
- 不支持实时播放中的频谱滚动显示（仅分析完整音轨）
- 频率轴使用近似对数刻度，非精确 log scale
- 目前仅支持 x64 和 Win32 平台

## 许可证

本项目代码采用 MIT 许可证。

第三方许可证：
- foobar2000 SDK：见 `sdk/sdk-license.txt`
- Spek / Spek-X 调色板代码：GPLv3

## 致谢

- Spek / Spek-X 项目：调色板算法和频谱分析思路
- foobar2000 SDK：提供插件开发框架
