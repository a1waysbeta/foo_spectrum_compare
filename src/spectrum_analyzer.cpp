#include "stdafx.h"
#include "spectrum_analyzer.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

SpectrumAnalyzer::SpectrumAnalyzer() {}
SpectrumAnalyzer::~SpectrumAnalyzer() {}

// 预计算窗函数表 —— 移植 Spek 的 coss[] 预计算思路。
//
// Spek 在 spek_pipeline_open() 里只算一次 cos 表：
//   p->coss[i] = cosf(cf * i);   cf = 2π / (nfft - 1)
// 然后 get_window() 全部查表。
//
// 我们更进一步：直接把最终窗值（而不是 cos 值）建成表，这样热循环里
// 连乘加都省了，只剩一次数组读取。窗函数在整个 analyze() 期间不变，
// 所以建一次即可。
//
// n 个 cosf 替代原来的 (总采样数 × 3) 次 cosf。
void SpectrumAnalyzer::build_window_table(window_function_t f, int n, std::vector<float>& table) {
    table.resize(n);
    if (n <= 0) return;

    // Blackman-Harris 需要 2i / 3i 项。Spek 用 coss[2*i % n] 取模复用同一张
    // cos 表；我们这里直接按角度算，数值上更准确，且只在建表时算一次。
    const float cf = 2.0f * (float)M_PI / (n - 1.0f);

    for (int i = 0; i < n; i++) {
        switch (f) {
        case WINDOW_HANN:
            table[i] = 0.5f * (1.0f - cosf(cf * i));
            break;
        case WINDOW_HAMMING:
            table[i] = 0.53836f - 0.46164f * cosf(cf * i);
            break;
        case WINDOW_BLACKMAN_HARRIS:
            table[i] = 0.35875f
                     - 0.48829f * cosf(cf * i)
                     + 0.14128f * cosf(cf * 2 * i)
                     - 0.01168f * cosf(cf * 3 * i);
            break;
        default:
            table[i] = 1.0f;
            break;
        }
    }
}

bool SpectrumAnalyzer::analyze(metadb_handle_ptr track, SpectrumData& out, abort_callback& abort,
                               progress_cb on_progress) {
    out.ready = false;
    out.error = false;

    // Capture path string immediately (the pointer from get_path() may be invalidated)
    pfc::string8 pathStr = track->get_path();
    out.track_path = pathStr.c_str();

    // Get track info (get_info_async is safe across threads, locks metadb temporarily)
    file_info_impl info;
    bool got_info = false;
    try {
        got_info = track->get_info_async(info);
    } catch (...) {
        got_info = false;
    }

    if (got_info) {
        out.sample_rate = (int)info.info_get_int("samplerate");
        out.channels = (int)info.info_get_int("channels");
        out.duration = info.get_length();
    }

    // Get title using the configured titleformat string
    try {
        titleformat_hook* hook = NULL;
        service_ptr_t<titleformat_object> obj;
        const char* fmt = m_title_format.empty() ? "%title%" : m_title_format.c_str();
        static_api_ptr_t<titleformat_compiler>()->compile_safe(obj, fmt);
        pfc::string8 title_tmp;
        if (got_info) {
            track->format_title_from_external_info(info, hook, title_tmp, obj, NULL);
        } else {
            track->format_title(hook, title_tmp, obj, NULL);
        }
        out.title = title_tmp.c_str();
    } catch (...) {
        out.title = pathStr.c_str();
    }
    if (out.title.empty()) out.title = pathStr.c_str();

    try {
        // ================================================================
        // S3：解除 no_seeking，把「跳过样本」升级成「不解码样本」
        //
        // 原来这里带 input_flag_no_seeking，本意是「我只顺序读，别为我建
        // 昂贵的 seektable」。但 S2 落地后瓶颈已经完全变了：整轨 8.5 亿
        // 样本里只有 0.4% 会进 FFT，S2 只是让我们不再对另外 99.6% 做混音
        // 运算，**却没能阻止解码器把它们算出来** —— 而 DSD→PCM 那条长
        // FIR 抽取滤波器正是最贵的一环。
        //
        // 打开 seek 后可以直接跳到下一个 FFT 窗口，解码量从 8.5 亿降到约
        // 328 万，两百多倍的差距。
        //
        // 有意**不加** input_flag_allow_inaccurate_seeking：我们依赖
        // 「seek(t) 之后下一个样本恰好是第 t*rate 个」来维持 pos 记账，
        // 近似 seek 会让每列的时间位置漂移。而真正需要加速的高采样格式
        // （DSF/DFF/FLAC/WAV）本身是定长块或自带索引，精确 seek 并不需要
        // 额外扫描整个文件。
        // ================================================================
        const t_uint32 decode_flags = input_flag_no_looping;
        input_helper input;

        // ================================================================
        // R4：分项计时 —— 定位残余速度差到底在哪一环
        //
        // 现在只知道"整体比 Spek 慢一点"，这个粒度没法优化。三条候选路径
        // 的优化手段完全不同，必须先分开量：
        //   t_open   打开 + 建索引。DSF/DFF 无索引，foobar 可能要扫全文件。
        //   t_seek   seek 调用本身。次数 × 单次成本，能看出 seek 是否在
        //            解码器内部退化成"从头解到目标点"。
        //   t_decode input.run() 的纯解码时间。DSD 要跑数字滤波抽取到
        //            PCM，这是理论上不可压缩的下限。
        //   t_fft    加窗 + FFT + 功率累加。列数固定，与格式无关。
        // 三者之外的残差是逐样本混音和记账开销。
        //
        // 计时器本身的成本：QueryPerformanceCounter 约 20~30ns，
        // 每个 chunk 前后各一次（chunk 通常上千样本），占比可忽略。
        // 不给「逐样本循环」计时正是因为那里调用频率太高会反被计时污染。
        // ================================================================
        pfc::hires_timer t_total;  t_total.start();
        double t_open = 0, t_seek = 0, t_decode = 0, t_fft = 0;
        int    n_seek = 0, n_fft = 0;
        int64_t decoded_samples = 0;   // 实际解码出来的样本数

        {
            pfc::hires_timer tm; tm.start();
            input.open(NULL, track, decode_flags, abort);
            t_open = tm.query();
        }

        // ================================================================
        // F1：采样率必须取自解码器，不能取自元数据
        //
        // 元数据里的 samplerate 是文件的**原生**速率。对 DSD 来说那是
        // 1-bit 位流速率（DSD64 = 2822400），但 foobar 的 DSD 解码器会跑
        // 一条 FIR 抽取滤波器，实际吐给我们的是 PCM —— DSD64 抽取成
        // 352800 Hz，正好 1/8。Spek 头部那行 "352800 Hz" 就是这个值。
        //
        // 拿错了会同时错三处（都源于同一个 8 倍因子）：
        //   1) 频率轴 Nyquist 算成 1411 kHz，真值是 176.4 kHz；
        //   2) total_samples 放大 8 倍 → stride 也放大 8 倍；
        //   3) seek 的 inv_rate 缩小 8 倍 → 落点只到目标位置的 1/8。
        //
        // 2) 和 3) 的误差方向相反、倍数相同，**恰好互相抵消**，所以 seek
        // 成功时列的落点仍然铺满整轨，画面看着是对的 —— 这也是为什么这个
        // bug 一直藏在频率轴标签后面没被发现。但一旦 seek 失败退回顺序
        // 跳读，2) 就失去了补偿：按放大 8 倍的 stride 铺列，走到流尾时
        // 800 列里只产出约 100 列，时间分辨率直接掉 8 倍。
        //
        // SDK 没有「不解码就问出 PCM 速率」的接口（get_info 返回的仍是
        // 元数据），所以先解一个 chunk 探测，再回到起点重新开始。代价是
        // 一个 chunk 的解码 + 一次 seek，相对整轨可忽略。
        //
        // 必须回到起点而不能顺手把探测到的 chunk 用掉：第一列的 FFT 窗口
        // 就落在 [0, nfft)，吞掉开头几千个样本会让第一列失去数据。
        // ================================================================
        {
            audio_chunk_impl_temporary probe;
            pfc::hires_timer tm; tm.start();
            const bool got_probe = input.run(probe, abort);
            t_decode += tm.query();
            if (got_probe) {
                const int pcm_rate = (int)probe.get_sample_rate();
                const int pcm_nch = (int)probe.get_channels();
                // 只在探测到合法值时覆盖，元数据缺失时这里反而是唯一来源。
                if (pcm_rate > 0) out.sample_rate = pcm_rate;
                if (pcm_nch > 0) out.channels = pcm_nch;
            }

            // 回到起点。优先用 seek(0)，它比 close+open 便宜得多；
            // 不可 seek 或 seek 抛异常时才重开。
            bool rewound = false;
            try {
                if (input.can_seek()) {
                    pfc::hires_timer ts; ts.start();
                    input.seek(0.0, abort);
                    t_seek += ts.query();
                    n_seek++;
                    rewound = true;
                }
            } catch (foobar2000_io::exception_aborted&) {
                throw;
            } catch (...) {
                rewound = false;
            }
            if (!rewound) {
                pfc::hires_timer to; to.start();
                input.close();
                input.open(NULL, track, decode_flags, abort);
                t_open += to.query();
            }
        }

        FFTPlan fft(FFT_BITS);
        int nfft = fft.get_input_size();
        int nbins = fft.get_output_size();
        out.fft_bins = nbins;

        // ================================================================
        // Magnitude → amplitude (dBFS) normalization.
        //
        // Our FFT outputs raw |X[k]| (no scaling). To map onto Spek's
        // absolute dBFS scale we need two compensations:
        //
        //   1) DFT magnitude to linear amplitude for real signals:
        //      A_amp[k] = 2 * |X[k]| / N     (positive freq portion)
        //   2) Window coherent-gain compensation for Hann window:
        //      sum_n w[n] / N = 0.5 for Hann, so actual windowed peak
        //      amplitude seen by FFT = A_true * 0.5.  We divide by 0.5
        //      to recover the true sinusoidal amplitude.
        //
        // Combined: out[k] = |X[k]| * 2 / (N * 0.5) = |X[k]| * 4 / N.
        // With N=2048 this is exactly 1/512 = 0.001953125.  After this
        // scaling, a true digital full-scale sine (peak ±1.0) will show
        // up in its peak FFT bin as ~1.0, which maps to 0 dBFS via
        // 20*log10(val / 1.0) in the renderer.  16-bit noise floor at
        // -96 dBFS correctly lands around level 0.20 → deep blue on
        // the palette, matching Spek.
        // ================================================================
        const float mag_to_amp = 4.0f / (float)nfft;

        // Decide hop size and number of time columns.
        //
        // 时间轴布局（配合下面的区间平均）：
        //   - 目标 m_target_frames 列，均匀铺满整轨；
        //   - hop = total_samples / target_cols，夹在 [nfft/4, nfft/2]
        //     之间保证至少 50% 重叠；
        //   - stride = 每列跨过多少个 hop。原实现把这 stride-1 个中间
        //     FFT **直接丢弃**（1 FFT/列的瞬时快照）；现在改为把它们
        //     累加求平均（见下方 P0-2 区间平均），画面显著降噪。
        double total_samples = out.duration * out.sample_rate;
        int target_cols = m_target_frames > 0 ? m_target_frames : 800;
        int64_t hop_i64 = target_cols > 0 && total_samples > 0
            ? (int64_t)(total_samples / target_cols)
            : (int64_t)(nfft / 2);
        // Clamp hop: nfft/4 (75% overlap) <= hop <= nfft/2 (50% overlap)
        const int64_t hop_min = (int64_t)(nfft / 4);
        const int64_t hop_max = (int64_t)(nfft / 2);
        if (hop_i64 < hop_min) hop_i64 = hop_min;
        if (hop_i64 > hop_max) hop_i64 = hop_max;
        int hop = (int)hop_i64;

        int64_t total_hops_i64 = total_samples > 0
            ? (int64_t)(total_samples / hop) - 1
            : 0;
        if (total_hops_i64 < 1) total_hops_i64 = 1;
        int columns = (int)pfc::min_t<int64_t>(
            (int64_t)target_cols,
            pfc::max_t<int64_t>(1, total_hops_i64));

        // Stride = how many hops we skip per displayed column so the
        // final count never exceeds `columns`.
        int stride = (int)pfc::max_t<int64_t>(
            1,
            total_hops_i64 / pfc::max_t<int64_t>(1, (int64_t)columns));
        columns = (int)pfc::max_t<int64_t>(
            1,
            total_hops_i64 / pfc::max_t<int64_t>(1, (int64_t)stride));

        // ================================================================
        // P0-2：区间平均（移植 Spek spek-pipeline.cc worker_func）
        //
        // Spek 对每个输出列在其时间区间内做多个**非重叠** FFT，累加后
        // 除以个数，只输出 1 列：
        //     p->output[i] += fft->get_output(i);   // 累加
        //     p->output[i] /= num_fft;              // 求平均
        //
        // 这样做能真正降低噪声（随机噪声按 1/sqrt(num_fft) 收敛），
        // 而不是像事后高斯模糊那样把噪声抹糊。
        //
        // 与 Spek 的两点差异（有意为之）：
        //   1) Spek 平均的是 dB 值（其 FFT 直接输出 10*log10）。我们存
        //      线性幅度，改为在**功率域**（幅度平方）累加平均，再开方
        //      回幅度 —— 这是能量上正确的平均方式，避免 dB 域平均把
        //      深谷过度加权（log 域平均会被极小值拉低，产生假暗条）。
        //   2) 我们给每列的平均帧数设了上限 MAX_AVG_PER_COLUMN，并把这
        //      些帧在整个 stride 区间内**均匀铺开**取样，而不是挤在区
        //      间开头连续取。均匀铺开的帧彼此重叠更少、相关性更低，
        //      降噪效果比连续取更接近 Spek 的非重叠平均。
        // ================================================================
        int avg_per_col = stride < MAX_AVG_PER_COLUMN ? stride : MAX_AVG_PER_COLUMN;
        if (avg_per_col < 1) avg_per_col = 1;

        // 在 [0, stride) 内均匀挑 avg_per_col 个 hop 索引参与平均。
        // 例：stride=10, avg=4 → 命中 hop 序号 0, 2, 5, 7
        std::vector<int> avg_hop_slots((size_t)avg_per_col);
        for (int i = 0; i < avg_per_col; i++) {
            avg_hop_slots[(size_t)i] = (int)((int64_t)i * stride / avg_per_col);
        }

        // ================================================================
        // S1：环形缓冲的取模换成掩码
        //
        // 改造前每个样本都要算一次 buf_pos = (buf_pos + 1) % bufsz。
        // bufsz 来自 (int)buffer.size()，是运行期变量，编译器无法证明它
        // 是 2 的幂，只能老老实实生成 idiv —— x86 上整数除法有 20~40
        // 周期延迟且几乎不能流水。对上亿样本的高采样素材，仅这一条指令
        // 就足够吃掉好几秒。
        //
        // nfft 恒为 2 的幂，nfft*2 也是，于是取模可直接换成
        // & (bufsz - 1)，成本降到 1 周期。
        // ================================================================
        const int bufsz = nfft * 2;
        const int buf_mask = bufsz - 1;
        std::vector<float> buffer((size_t)bufsz, 0.0f);
        int buf_pos = 0;

        // 自上一次「跳读」以来连续写入缓冲的样本数。只有它 >= nfft 时，
        // 缓冲里最近 nfft 个样本才是一段真正连续的波形（跳读会在缓冲里
        // 留下时间上不相邻的残留，绝不能拿去做 FFT）。
        int contig = 0;

        int acc_count = 0;              // 当前列已累加的 FFT 数

        // P0-1/P3：窗表与 FFT 输入缓冲都在循环外分配一次。
        // 窗表必须是局部变量 —— m_analyzer 被多个分析线程共享。
        std::vector<float> window_table;
        build_window_table(m_window, nfft, window_table);

        // 功率累加器（幅度平方），每列清零一次
        std::vector<float> accum((size_t)nbins, 0.0f);

        // ================================================================
        // 渐进输出：直接写进 out.data，而不是先攒在局部 frame_data 里
        //
        // 改造前这里是 `std::vector<float> frame_data;`，整轨算完才
        // `out.data = std::move(frame_data)` 一次性交付，所以 UI 在
        // 分析全程只能显示 "Analyzing..."。DSD64 这种 2.8MHz 素材要等
        // 十几秒甚至更久，体验很差。
        //
        // 现在每 flush 一列就追加进 out.data 并回调，消费方（窗口）能
        // 立刻把新列取走显示 —— 这是 Spek「频谱边算边长」的核心。
        //
        // reserve 一次到位很关键：消费方按「已有列数」做增量拷贝，
        // 生产方中途 realloc 只影响自己的缓冲区，不会让消费方读到悬空
        // 指针（消费方每次都在锁内重新取 data.data() ）。
        // ================================================================
        out.data.clear();
        out.data.reserve((size_t)columns * (size_t)nbins);
        out.time_frames = 0;
        // 计划总列数。渲染器用它当时间轴分母，保证已画出的部分不会
        // 随着新列到达而横向缩放位移。
        out.total_frames = columns;

        // 把环形缓冲里最近 nfft 个样本取出、加窗、做 FFT，
        // 并把功率累加进 accum。
        auto run_fft_into_accum = [&]() {
            pfc::hires_timer tm; tm.start();
            int start = (buf_pos - nfft + bufsz) & buf_mask;

            // P1-3：直写 FFT 输入缓冲，省掉中转数组和逐点 set_input 调用。
            float* dst = fft.input_data();

            // 环形缓冲最多跨两段连续内存，分段拷贝即可整段顺序读写，
            // 免掉热循环里每个样本一次的取模运算。
            const int first = nfft < (bufsz - start) ? nfft : (bufsz - start);
            for (int j = 0; j < first; j++) {
                dst[j] = buffer[(size_t)(start + j)] * window_table[(size_t)j];
            }
            for (int j = first; j < nfft; j++) {
                dst[j] = buffer[(size_t)(j - first)] * window_table[(size_t)j];
            }

            fft.execute();

            // FFT 已直接输出功率，无需再平方（见 fft.h get_output_power）
            for (int j = 0; j < nbins; j++) {
                accum[(size_t)j] += fft.get_output_power(j);
            }
            acc_count++;
            t_fft += tm.query();
            n_fft++;
        };

        // 把 accum 求平均、开方回幅度、归一化后写出一列
        auto flush_column = [&]() {
            if (acc_count <= 0) return;
            const float inv = 1.0f / (float)acc_count;
            for (int j = 0; j < nbins; j++) {
                float p = accum[(size_t)j] * inv;      // 平均功率
                float m = p > 0.0f ? sqrtf(p) : 0.0f;  // 回到幅度域
                out.data.push_back(m * mag_to_amp);
                accum[(size_t)j] = 0.0f;
            }
            acc_count = 0;

            // 列数自增后立刻通知消费方。time_frames 必须在回调**之前**
            // 更新，因为回调里消费方会按它决定拷贝多少列。
            out.time_frames++;
            if (on_progress) on_progress(out.time_frames, columns);
        };

        // ================================================================
        // S2：把「逐样本喂环形缓冲」改成「按需跳读」
        //
        // 这是本轮性能改造的核心。原实现对**每一个**解码出来的样本都执行
        // 一遍：多声道求和 → 除以声道数 → 写缓冲 → 取模递增 → 递增计数
        // → 比较是否到达 hop。DSD64（2.8224 MHz）双声道 5 分钟就是约
        // 8.5 亿个样本，即便每样本只花 3~4 ns，光这个循环也要 3 秒以上，
        // 而其中真正被 FFT 用到的样本占比极低。
        //
        // 实际需要的样本量有多少？我们总共只做
        //     columns × avg_per_col = 800 × 2 = 1600 次 FFT，
        // 每次 FFT 只需要窗口内 nfft = 2048 个连续样本，
        // 合计 1600 × 2048 ≈ 328 万个样本 —— 占 8.5 亿的 0.4%。
        // 也就是说 99.6% 的逐样本工作是纯粹浪费。
        //
        // Spek 之所以不受这个问题困扰，是因为它把解码放在 reader 线程、
        // FFT 放在 worker 线程（spek-pipeline.cc reader_func/worker_func），
        // 并且 AudioFileImpl::start 用 Bresenham 误差累积（frames_per_
        // interval / error_per_interval）来决定区间边界。
        //
        // 我们采用更直接的办法：预先算出下一次 FFT 窗口的起始样本位置，
        // 对位置之前的样本整块 memchr 式跳过（只推进计数器，不做任何
        // 逐样本运算），只在窗口范围内才真正混音写入缓冲。
        // 跳过一段后 contig 归零，保证不会用不连续的数据做 FFT。
        // ================================================================

        // 逐列、逐平均槽位地推进「下一个 FFT 窗口的结束样本位置」。
        // 第 c 列第 s 个平均槽的 hop 序号为 c*stride + avg_hop_slots[s]，
        // 该 hop 的窗口右边界（不含）= (hop_index + 1) * hop。
        int cur_col = 0;
        int cur_slot = 0;

        // ================================================================
        // 一个被评估后**否决**的改法，记录在此避免以后重复踩
        //
        // 曾考虑把同一列的 avg_per_col 个平均窗口从「在 stride 区间内均匀
        // 铺开」改成「首尾相接的连续块」，这样每列只需 1 次 seek，seek 次
        // 数从 1600 降到 800。算完成本才发现不值得：
        //
        //   均匀铺开：1600 次 seek × (2048 窗口 + 4096 预热) ≈ 983 万样本
        //   连续块：   800 次 seek × (4096 窗口 + 4096 预热) ≈ 655 万样本
        //
        // 只差 1.5 倍，而两者相对原来的 8.47 亿都是百倍量级的改进 ——
        // 省下的那 328 万样本占总量的 0.4%，完全不值得动画质。
        //
        // 代价那边却是实打实的：均匀铺开让两个窗口落在该列 106 万样本时间
        // 区间的两端，对区间内容的代表性明显更好；改成连续块后一列只反映
        // 4096 个样本（区间的 0.4%）那一瞬间的频谱，遇到列内有瞬态/切换
        // 时更容易采偏。
        //
        // 所以窗口位置**一个字节都不动**：S3 只改「怎么把样本取到手」，
        // 不改「取哪些样本」。这样验证也简单 —— 画面若有任何变化，那就是
        // seek 有 bug，而不是设计取舍。
        // ================================================================

        // 返回当前待处理 FFT 的窗口结束位置（全局样本下标）；
        // 所有列都做完则返回 -1。
        auto next_fft_end = [&]() -> int64_t {
            if (cur_col >= columns) return -1;
            const int64_t hop_index =
                (int64_t)cur_col * stride + avg_hop_slots[(size_t)cur_slot];
            return (hop_index + 1) * (int64_t)hop;
        };

        // ================================================================
        // S3-b：用 seek 把「跳过的样本」变成「根本没解码的样本」
        //
        // S2 已经让我们不再对无用样本做混音运算，但解码器仍然把它们逐个
        // 算了出来 —— 对 DSD 来说那是一条很长的 FIR 抽取滤波器，正是整个
        // 分析里最贵的一环。既然 input_helper 提供了 seek，就直接跳过去。
        //
        // 三个必须处理的细节：
        //
        //   1) 预热（preroll）。解码器 seek 之后内部滤波器/预测器状态是重
        //      置过的，紧随其后的头几百个样本可能带瞬态或衰减。我们落到
        //      窗口**前面** preroll 个样本处，让这段照常解码但直接丢弃，
        //      只用它把解码器状态喂热。代价是解码量翻倍（0.4% → 0.8%），
        //      相对 200 多倍的收益完全可以忽略。
        //
        //   2) 门槛（seek_threshold）。一次 seek 要付文件定位 + 解码器复位
        //      + 可能的帧重解，大致相当于解码几千个样本。间隔太小时顺序读
        //      更划算，所以只在间隔明显超过门槛时才 seek。短曲目里 stride
        //      很小、窗口本来就挨着，这条判断会让我们自然退回顺序读。
        //
        //   3) 兜底。can_seek() 为假（网络流、某些无索引格式），或者虽然
        //      声称支持、真调用时抛异常，都必须能无损退回 S2 的顺序跳读。
        //      失败后把 seekable 置假，之后不再重试。
        // ================================================================
        bool seekable = false;
        try {
            seekable = input.can_seek() && out.sample_rate > 0;
        } catch (...) {
            seekable = false;
        }
        const double inv_rate = out.sample_rate > 0 ? 1.0 / (double)out.sample_rate : 0.0;
        const int64_t preroll = (int64_t)nfft * 2;
        const int64_t seek_threshold = (int64_t)nfft * 8;

        int64_t fft_end = next_fft_end();
        int64_t pos = 0;                 // 已消费的全局样本数

        audio_chunk_impl_temporary chunk;
        while (fft_end >= 0) {
            // ---- 阶段 0：距离下一个窗口还很远 → 直接 seek 过去 ----
            // 落点故意提前 preroll 个样本，给解码器留一段预热区。
            if (seekable) {
                const int64_t win_start = fft_end - nfft;
                int64_t target = win_start - preroll;
                if (target < 0) target = 0;

                if (target - pos >= seek_threshold) {
                    bool ok = true;
                    pfc::hires_timer tm; tm.start();
                    try {
                        input.seek((double)target * inv_rate, abort);
                    } catch (foobar2000_io::exception_aborted&) {
                        throw;                       // 用户中止，照常上抛
                    } catch (...) {
                        ok = false;
                    }
                    t_seek += tm.query();
                    n_seek++;

                    if (ok) {
                        pos = target;
                        contig = 0;                  // seek 造成时间断裂
                    } else {
                        // seek 声称可用却失败了。此刻解码器位置未知，不能
                        // 继续按 pos 记账，否则会画出错位的频谱。重开一次
                        // 把位置拉回 0，之后一律走 S2 顺序跳读。
                        // 已经输出的列都是在正确位置算出来的，无需丢弃。
                        seekable = false;
                        try {
                            input.close();
                            input.open(NULL, track, decode_flags, abort);
                            pos = 0;
                            contig = 0;
                        } catch (foobar2000_io::exception_aborted&) {
                            throw;
                        } catch (...) {
                            // 连重开都失败，收工。已算出的列照常交付，
                            // 由循环外的尾部处理补上最后一列。
                            fft_end = -1;
                            break;
                        }
                    }
                }
            }

            {
                pfc::hires_timer tm; tm.start();
                const bool got = input.run(chunk, abort);
                t_decode += tm.query();
                if (!got) break;
            }
            abort.check();

            const audio_sample* samples = chunk.get_data();
            // get_channels / get_sample_count return t_size = size_t.  We
            // cast to int with an explicit clamp because real audio chunks
            // are always well within INT_MAX samples / channels, but the
            // bare assignments used to trigger C4267.
            int nch = pfc::downcast_guarded<int, t_size>(chunk.get_channels());
            int nsamples = pfc::downcast_guarded<int, t_size>(chunk.get_sample_count());
            if (nch <= 0) continue;
            decoded_samples += nsamples;

            // 声道平均用倒数乘替代除法：浮点除法约 14 周期且不流水，
            // 乘法 4 周期且可向量化。
            const float inv_nch = 1.0f / (float)nch;

            int i = 0;
            while (i < nsamples && fft_end >= 0) {
                // 本次 FFT 窗口覆盖 [fft_end - nfft, fft_end)。
                const int64_t win_start = fft_end - nfft;

                // ---- 阶段 A：整块跳过窗口之前的样本 ----
                // 这里被跳过的样本有两种来源：
                //   - seekable 为假时，是 S2 顺序跳读要丢掉的无用区间；
                //   - seek 成功后，是我们故意多解的 preroll 预热区 ——
                //     解码器状态已被它喂热，样本本身丢掉。
                // 两种情况都必须把 contig 归零：缓冲里剩下的是上一个窗口
                // 的残留，与即将写入的样本在时间上不相邻。
                if (pos < win_start) {
                    const int64_t skip = win_start - pos;
                    const int avail_now = nsamples - i;
                    if (skip >= (int64_t)avail_now) {
                        // 整个 chunk 都不需要，直接丢弃。
                        pos += avail_now;
                        i = nsamples;
                        contig = 0;
                        break;
                    }
                    i += (int)skip;
                    pos += skip;
                    contig = 0;   // 跳过造成时间断裂
                }

                // ---- 阶段 B：窗口内样本，正常混音写入 ----
                const int64_t need = fft_end - pos;          // 还差多少样本填满窗口
                const int take = (int)pfc::min_t<int64_t>(need, nsamples - i);
                const int end = i + take;
                for (; i < end; i++) {
                    const audio_sample* frame = samples + (size_t)i * nch;
                    float mono = 0.0f;
                    for (int c = 0; c < nch; c++) {
                        // audio_sample is typedef'd to float or double
                        // depending on SDK configuration.  Explicit cast to
                        // float avoids C4244 when the SDK chose double.
                        mono += static_cast<float>(frame[c]);
                    }
                    buffer[(size_t)buf_pos] = mono * inv_nch;
                    buf_pos = (buf_pos + 1) & buf_mask;
                    if (contig < bufsz) contig++;
                }
                pos += take;

                // ---- 阶段 C：窗口填满 → 做 FFT，并推进到下一个目标 ----
                if (pos >= fft_end) {
                    if (contig >= nfft) {
                        run_fft_into_accum();
                    }

                    cur_slot++;
                    if (cur_slot >= avg_per_col) {
                        // 本列的平均槽位都取完了，输出这一列
                        cur_slot = 0;
                        if (acc_count > 0) {
                            flush_column();
                        }
                        cur_col++;
                    }
                    fft_end = next_fft_end();
                }
            }
        }

        // 尾部：区间没走满但已有累加数据，补出最后一列，避免丢尾。
        if (acc_count > 0 && out.time_frames < columns) {
            flush_column();
        }

        // Tail: if we haven't produced any column yet but have a full
        // window in the ring, emit one final FFT so short clips still
        // render (covers the <1-column edge case).
        if (out.data.empty() && contig >= nfft) {
            run_fft_into_accum();
            flush_column();
        }

        // 实际产出可能少于计划列数（时长元数据不准、解码提前结束等）。
        // 把 total_frames 收敛到真实值，否则渲染器会按偏大的分母算时间
        // 轴，导致频谱右侧留一条永久空白。
        out.total_frames = out.time_frames;

        // Compute dynamic range (convert to dB-like scale)
        if (out.data.size() > 0) {
            float mx = 0, mn = 1e30f;
            for (float v : out.data) {
                if (v > mx) mx = v;
                if (v < mn && v > 0) mn = v;
            }
            out.max_level = mx;
            out.min_level = mn > 0 ? mn : 0.0001f;
        }

        out.ready = true;

        // ================================================================
        // R4：输出一行分项计时
        //
        // console::printf 走的是 pfc 自己的 printf 实现（pfc/printf.cpp），
        // **只认** %s %i/%d %u %x %c —— 没有 %f。所以全部换算成整数毫秒，
        // 不要写 %.1f，那会被当成字面量输出且吃掉一个参数。
        //
        // decoded/total 这个比值是判断 seek 有没有真正生效的关键指标：
        //   接近 1.0  → seek 没起作用（或退回了顺序跳读），整轨都在解码；
        //   远小于 1.0 → seek 生效，只解码了窗口附近的样本。
        // DSD64 五分钟约 8.5 亿样本，理想值在 1% 上下。
        // ================================================================
        {
            const double total_ms = t_total.query() * 1000.0;
            const int64_t planned = (int64_t)(out.duration * out.sample_rate);
            const int decoded_permille = planned > 0
                ? (int)((int64_t)1000 * decoded_samples / planned)
                : 0;
            const double other_ms = total_ms
                - (t_open + t_seek + t_decode + t_fft) * 1000.0;
            console::printf(
                "spectrum_compare: %s | %ims total"
                " (open %i, seek %i x%i, decode %i, fft %i x%i, other %i)"
                " | %i Hz %ich, %i cols, decoded %i/1000 of stream",
                out.title.c_str(),
                (int)total_ms,
                (int)(t_open * 1000.0),
                (int)(t_seek * 1000.0), n_seek,
                (int)(t_decode * 1000.0),
                (int)(t_fft * 1000.0), n_fft,
                (int)(other_ms < 0 ? 0 : other_ms),
                out.sample_rate, out.channels,
                out.time_frames, decoded_permille);
        }

        return true;
    }
    catch (std::exception const& e) {
        out.error = true;
        out.error_msg = e.what();
        return false;
    }
    catch (...) {
        out.error = true;
        out.error_msg = "Unknown error";
        return false;
    }
}
