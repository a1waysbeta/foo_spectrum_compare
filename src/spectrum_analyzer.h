#pragma once

#include "fft.h"
#include "palette.h"
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <functional>

// Window function types (from Spek)
enum window_function_t {
    WINDOW_HANN,
    WINDOW_HAMMING,
    WINDOW_BLACKMAN_HARRIS,
    WINDOW_COUNT,
    WINDOW_DEFAULT = WINDOW_HANN,
};

// Stores computed spectrogram data for one track
struct SpectrumData {
    std::string track_path;
    std::string title;
    int sample_rate = 0;
    int channels = 0;
    double duration = 0;
    int fft_size = 0;      // input FFT length (e.g. 2048)
    int fft_bins = 0;      // number of frequency bins (output_size)
    int hop_size = 0;      // samples between successive FFTs (overlap = 1 - hop/fft)
    int time_frames = 0;   // number of time columns **currently available**
    // 计划产出的总列数。渐进显示时 time_frames < total_frames，
    // 渲染器用 total_frames 作时间轴分母，用 time_frames 决定画到哪。
    // 这样已经画出来的部分不会随着新列到达而横向位移/缩放。
    int total_frames = 0;
    window_function_t window = WINDOW_DEFAULT;
    std::vector<float> data; // [time_frames * fft_bins], row-major
    float max_level = 0.0f;
    float min_level = 0.0f;
    bool ready = false;
    bool error = false;
    std::string error_msg;

    float get(int t, int f) const {
        if (t < 0 || t >= time_frames || f < 0 || f >= fft_bins) return 0;
        return data[t * fft_bins + f];
    }
};

// Analyzes audio and produces spectrogram data
class SpectrumAnalyzer {
public:
    SpectrumAnalyzer();
    ~SpectrumAnalyzer();

    // ================================================================
    // 渐进回调 —— 移植 Spek 的 spek_pipeline_cb 思路
    //
    // Spek 的 worker 线程每算完一个像素列就立刻回调，GUI 侧把该列写进
    // 常驻位图并 Refresh，所以用户从第一秒起就能看到频谱在生长，
    // 而不是干等整轨分析结束（spek-pipeline.cc:457）。
    //
    // 与 Spek 的关键差异：
    //   Spek 的一列 == 一个屏幕像素列（samples = 面板宽度），列数由窗口
    //   宽度决定，因此改变窗口宽度必须整轨重新分析（on_size 里 restart）。
    //   我们保留「固定 m_target_frames 列 + 渲染时重采样」的设计，改窗口
    //   大小无需重算，多轨各自宽度不同也能共用同一份数据。
    //   代价是回调不能直接对应像素列，所以这里传的是「已产出列数」，
    //   由渲染侧按 done/total 的比例决定画到哪一列为止。
    //
    // 语义约定：
    //   * 在**分析线程**上调用，绝不能触碰 GUI 对象；
    //   * frames_done 单调递增，每次至少 +1；
    //   * 回调期间 out.data 已经追加好了这些列，读取方需自行加锁；
    //   * 回调可能被调用几百次，实现必须廉价（我们的实现只做一次
    //     std::move 拷贝 + PostMessage 节流，见 analysis_worker）。
    // ================================================================
    typedef std::function<void(int frames_done, int frames_total)> progress_cb;

    // Analyze a track from a metadb_handle_ptr; runs synchronously
    // Returns true on success
    bool analyze(metadb_handle_ptr track, SpectrumData& out, abort_callback& abort,
                 progress_cb on_progress = nullptr);

    // Configuration
    void set_window(window_function_t w) { m_window = w; }
    window_function_t get_window() const { return m_window; }
    void set_time_resolution(int frames) { m_target_frames = frames; }
    int get_time_resolution() const { return m_target_frames; }

    // Set the titleformat string used to format the track label
    void set_title_format(const char* fmt) { m_title_format = fmt ? fmt : ""; }
    const char* get_title_format() const { return m_title_format.c_str(); }

private:
    static constexpr int FFT_BITS = 11;        // 2048-point FFT (fixed for speed)
    window_function_t m_window = WINDOW_DEFAULT;
    int m_target_frames = 800; // target number of time columns
    std::string m_title_format = "%title%";

    // 每列最多累加平均的 FFT 个数（移植 Spek 的区间平均思想）。
    //
    // Spek 把一个时间区间内**所有**非重叠 FFT 全部累加求平均，画质最好
    // 但 FFT 次数最多。这里设上限做速度/画质折中，成本与画质关系：
    //   - FFT 耗时随该值**线性增长**；
    //   - 噪声标准差只按 1/sqrt(N) 下降（收益递减）。
    //
    // 取 2 的理由：P0-1 干掉逐样本 cosf 省下的时间，刚好抵掉多做 1 次
    // FFT 的开销 —— 总分析耗时约等于改造前，同时拿到 sqrt(2)≈1.41 倍
    // 降噪。想要更干净的画面可调到 4（约 2 倍降噪，但分析会明显变慢）；
    // 调成 1 则退化为改造前的单帧抽样，纯粹追求最快。
    static constexpr int MAX_AVG_PER_COLUMN = 2;

    // 预计算窗函数表。
    //
    // 原实现在 apply_window 里对**每个采样点**实时调用 3 次 cosf，
    // 一首 3 分钟曲子约 500 万次三角函数调用，是分析阶段的主要开销。
    // 改为建表一次（n 次 cosf）后全部查表。
    //
    // 重要：调用方必须把表放在 analyze() 的**局部变量**里，不能做成
    // 成员变量 —— SpectrumAnalyzer 实例被多个分析线程共享（每个 track
    // 一个 detached 线程），成员级缓存会造成数据竞争。
    static void build_window_table(window_function_t f, int n, std::vector<float>& table);
};
