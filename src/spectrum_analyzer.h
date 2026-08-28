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
    // 解码器实际吐给我们的 PCM 采样率。这是整个分析的基准：频率轴的
    // Nyquist、stride、seek 记账全部用它（见 .cpp 中 F1 段）。
    int sample_rate = 0;
    // 容器/元数据里**标称**的采样率（DSD64 = 2822400）。只用于表头展示，
    // 任何计算都不许碰它 —— 拿它当基准会把 Nyquist 算错 8 倍（F1 的原始
    // bug）。两者不同说明解码链跑了抽取/重采样，用户需要知道频谱图的真实
    // 频率上限不是标称值的一半。0 = 元数据不可用。
    int source_sample_rate = 0;
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

    // ================================================================
    // 用编译好的 titleformat 脚本求值一个轨道标签，并注入分析链路
    // 独有的字段（目前是 %analysis_samplerate%，见 .cpp 的 T7 段）。
    //
    // 做成 static 并对外公开，是因为标签求值有**两条**路径：
    //   1) analyze() 内部，探测出真实 PCM 率之后；
    //   2) SpectrumCompareWindow::refresh_track_titles()，用户改完格式串
    //      后不重新解码、只重算文字。
    // 早先两处各写一份 titleformat 调用，任何注入字段都必然在第 2 条路径
    // 上丢失（表现为：改一下格式串，采样率那段就消失了）。收敛到这一个
    // 函数后两条路径的行为按定义一致。
    //
    // 传编译后的 script 而不是格式串：调用方 2 要在多轨循环外只编译一次
    // （它本来就以"比 OnRefresh 便宜"为设计目标），别把那个优化抹掉。
    //
    // nominal_rate == pcm_rate 时 %analysis_samplerate% 会被判为"无值"，
    // 外层方括号整段折叠 —— 这是有意的，绝大多数 PCM 格式两者相同。
    // 失败时不改动 out_title（调用方应已填好兜底值）。
    // ================================================================
    static void format_track_title(metadb_handle_ptr track,
                                   const service_ptr_t<titleformat_object>& script,
                                   int pcm_rate, int nominal_rate,
                                   std::string& out_title);

private:
    static constexpr int FFT_BITS = 11;        // 2048-point FFT (fixed for speed)
    window_function_t m_window = WINDOW_DEFAULT;
    int m_target_frames = 800; // target number of time columns
    std::string m_title_format = "%title%";

    // ================================================================
    // 每列的平均帧数 = MAX_SEEK_POINTS_PER_COLUMN × 运行期算出的 burst
    //
    // T6 之前这里只有一个 MAX_AVG_PER_COLUMN = 2，含义是"每列做 2 次
    // FFT"。但那个单一常量把两种**成本完全不同**的东西混在了一起，
    // 必须拆开才能把画质提上去：
    //
    //   取样点数（seek points）—— 贵。
    //       每多一个点就多一次 seek + 多解一个完整的解码块。
    //       实测 DSD64：decode 214ms 占总耗时 246ms 的 87%，
    //       且它**严格正比于取样点数**。这就是速度的闸门。
    //
    //   每点连发数（burst）—— 在一定范围内免费。
    //       T2/T5 已确认解码器按固定整块计费（DSD64 实测块 = 4704
    //       样本恒定）。既然那一整块反正都要解出来，块内塞得下几个
    //       非重叠 FFT 就白拿几个。4704 装得下 2 × 2048 = 4096，
    //       所以第二次 FFT 的解码成本是**零**，只花 FFT 自己的算力
    //       （实测 19ms/1605 次 ≈ 12µs 一次，便宜到可以忽略）。
    //
    // 为什么必须提高平均帧数：周期图平均后每像素的对数域噪声标准差
    // 约为 4.34/sqrt(N) dB。
    //       N=2  → 3.07 dB   （T6 之前的 SC，用户反馈"更糊更噪"）
    //       N=4  → 2.17 dB
    //       N=85 → 0.47 dB   （Spek 在本例的实际值）
    // Spek 把整个区间内所有非重叠 FFT 全平均（本例 174220/2048 = 85
    // 次），画质自然最好 —— 但那等于把整轨解完，代价是 decode 从
    // 214ms 涨到 4 秒量级。burst 是唯一能白拿降噪的方向，所以策略是
    // **取样点数保持 2 不变，把块内空间榨干**。
    //
    // 想进一步接近 Spek 只能加取样点数（分析时间按倍数涨），
    // 调成 1 则退化为单帧抽样，最快但最噪。
    // ================================================================
    static constexpr int MAX_SEEK_POINTS_PER_COLUMN = 2;

    // 每个取样点最多连发几次 FFT 的安全上限。
    //
    // 实际 burst 由运行期块大小决定（见 .cpp 中 T6 段），这里只防
    // 极端情况：某些格式一次 run() 会吐几十万样本，若不设限会算出
    // 上百次 FFT/点 —— 而降噪按 1/sqrt(N) 收敛，N 超过十几以后每多
    // 一次 FFT 的画质回报已经看不出来了，纯属浪费算力。
    static constexpr int MAX_BURST_PER_POINT = 4;

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
