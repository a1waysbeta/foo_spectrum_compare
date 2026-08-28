#include "stdafx.h"
#include "spectrum_analyzer.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

SpectrumAnalyzer::SpectrumAnalyzer() {}
SpectrumAnalyzer::~SpectrumAnalyzer() {}

// ================================================================
// T7：让表头能同时显示标称采样率和实际分析的 PCM 率
//
// 起因：DSD64 的表头显示 "2822400 Hz"，但频谱图的频率轴上限只到
// 176400 Hz —— 因为 foobar 的 DSD 解码器跑了 FIR 抽取，真正进 FFT 的是
// 352800 Hz 的 PCM（详见 F1 段）。表头和纵轴对不上，用户没法判断到底
// 哪个是真的，也看不出自己在 foobar 里设的 DSD 输出速率生效成了多少。
//
// 为什么不能只加一个 titleformat 字段就完事：%samplerate% 由 foobar 从
// **元数据**解析，我们无法改变它的取值。而真实 PCM 率只有解码器吐出第一
// 个 chunk 之后才知道（F1 探测），这在时间上晚于原来格式化标题的位置。
// 所以 T7 做了两件事：
//   1) 把标题格式化从 analyze() 开头挪到 F1 探测之后（见下方调用点）；
//   2) 用这个 hook 注入一个自定义字段，把探测到的值送进 titleformat。
//
// 字段命名 %analysis_samplerate%：故意带 analysis_ 前缀，既不会撞上
// foobar 现有字段，也向用户表明"这是分析链路的值，不是文件属性"。
//
// process_field 的三态返回是这里的关键（sdk titleformat.cpp:111）：
//   return true  + found=true   → 字段归我，有值，已写入
//   return true  + found=false  → 字段归我，但**没值**，外层 [] 折叠掉
//   return false                → 不认识，交给下一个 hook（file_info）
// 第二种是实现"两率相同时整段消失"的机制：DSD 之外的绝大多数格式
// 标称率 == PCM 率，此时再显示一遍纯属噪音，让 [] 自动收掉最干净。
// 也正因为返回 false 会落到 file_info hook，%title%/%codec% 等标准字段
// 完全不受影响。
// ================================================================
namespace {

class analysis_info_hook : public titleformat_hook {
public:
    // pcm_rate：解码器实测值；nominal_rate：元数据标称值（0 = 未知）
    analysis_info_hook(int pcm_rate, int nominal_rate)
        : m_pcm_rate(pcm_rate), m_nominal_rate(nominal_rate) {}

    bool process_field(titleformat_text_out* p_out, const char* p_name,
                       t_size p_name_length, bool& p_found_flag) override {
        if (pfc::stricmp_ascii_ex(p_name, p_name_length,
                                  "analysis_samplerate", SIZE_MAX) == 0) {
            // 相同则视为"无值"，让外层方括号把整段折叠掉。
            // 探测失败(m_pcm_rate<=0)时同样折叠 —— 显示一个 0 比不显示更糟。
            if (m_pcm_rate <= 0 || m_pcm_rate == m_nominal_rate) {
                p_found_flag = false;
                return true;
            }
            p_out->write_int(titleformat_inputtypes::unknown, m_pcm_rate);
            p_found_flag = true;
            return true;
        }
        p_found_flag = false;
        return false;
    }

    bool process_function(titleformat_text_out*, const char*, t_size,
                          titleformat_hook_function_params*, bool&) override {
        return false;
    }

private:
    const int m_pcm_rate;
    const int m_nominal_rate;
};

} // namespace

// 两条求值路径共用的实现（声明处有完整的动机说明）。
//
// 这里自己再取一次 file_info，而不是让调用方传进来：analyze() 手上确实
// 已经有 info 了，但那次 get_info_async 相对于整轨解码（实测 214ms）是
// 可忽略的开销，换来的是两条路径走**完全同一份**代码 —— 包括
// "有 info 用 format_title_from_external_info、没有则退 format_title"
// 这个分支。不然某天只改了一处，注入字段又会在另一条路径上悄悄消失。
void SpectrumAnalyzer::format_track_title(metadb_handle_ptr track,
                                          const service_ptr_t<titleformat_object>& script,
                                          int pcm_rate, int nominal_rate,
                                          std::string& out_title) {
    if (!track.is_valid() || script.is_empty()) return;
    try {
        analysis_info_hook hook(pcm_rate, nominal_rate);
        pfc::string8 title_tmp;
        file_info_impl info;
        if (track->get_info_async(info)) {
            track->format_title_from_external_info(info, &hook, title_tmp, script, NULL);
        } else {
            track->format_title(&hook, title_tmp, script, NULL);
        }
        // 空结果不覆盖：调用方的兜底值（路径名 / 上一次的标题）比空白有用。
        if (title_tmp.length() > 0) out_title = title_tmp.c_str();
    } catch (...) {
        // 同上，保持 out_title 原值
    }
}

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
        // 标称率：只进表头，不参与任何计算。out.sample_rate 稍后会被 F1
        // 探测到的真实 PCM 率覆盖，而这个字段保留元数据原值。
        out.source_sample_rate = (int)info.info_get_int("samplerate");
        out.sample_rate = out.source_sample_rate;
        out.channels = (int)info.info_get_int("channels");
        out.duration = info.get_length();
    }

    // 兜底标题：真正的 titleformat 求值推迟到 F1 探测之后（T7），但下面
    // 任何一步抛异常都会带着 out.title 去走错误显示路径，所以这里先放上
    // 路径名。推迟的原因见上方 T7 段：%analysis_samplerate% 要等解码器
    // 吐出第一个 chunk 才有值。
    out.title = pathStr.c_str();

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

        // ================================================================
        // T2：chunk 粒度统计 —— 已得出结论，保留为回归监控
        //
        // 【当初的疑问】
        // 按设计每个 FFT 窗口只需解 preroll + nfft 个样本，改 T1 前是
        // 4096 + 2048 = 6144，乘 1614 窗口 = 992 万，占整轨 7508 万的
        // 132‰。但实测 decoded = 468‰，是需要量的 3.5 倍。
        //
        // 【实测结论：块粒度假说成立】
        //     chunks 1606, avg 4704, max 4704, used 435/1000
        // avg == max 是判定性的 —— 块大小**恒定**，不是偶发大块。
        // 4704 = 352800/75，正好一个 1/75 秒的 DSD 帧。也就是说
        // input.run() 每次吐出且仅吐出一整帧，我们只想要其中一段，
        // 但整帧的解码成本已经付掉了。
        //
        // 三个数互相印证，说明这套计数是可信的：
        //   chunks 1606 ≈ n_seek 1606 ≈ n_fft 1605  每窗口恰好一次
        //   used = 2048/4704 = 435‰                  与实测完全吻合
        //   used × 1606 × 4704 = 329 万 = 1605 × 2048 ✓
        //
        // 注意 used 的理论上限就是 nfft/块大小 = 435‰，不是我一度写的
        // 667‰（那是拿 preroll+nfft=3072 当分母，但真实分母是整块 4704）。
        // 换句话说 435‰ 已经**触到上限**，在"每窗口一次 seek"的框架下
        // 没有剩余压缩空间了；要再降只能改框架（例如一次 seek 复用同一
        // 块内的多个窗口）。
        //
        // 保留这三个计数器的价值：换格式/换采样率时块大小会变，而
        // T5 的 preroll 是按它算的，这几个数能立刻暴露假设失效。
        //   n_chunks     run() 返回次数，decoded/n_chunks = 平均块大小
        //   max_chunk    与平均值对比，判断块大小是否恒定
        //   used_samples 真正混音写进环形缓冲的样本数，即窗口内样本
        // ================================================================
        int     n_chunks = 0;          // input.run() 返回数据的次数
        int     max_chunk = 0;         // 见过的最大 chunk（每声道样本数）
        int64_t used_samples = 0;      // 真正进入环形缓冲的样本数

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
        // 一条 FIR 抽取滤波器，实际吐给我们的是 PCM。
        //
        // 关键在于这个 PCM 速率**不是固定倍率、也无法从元数据推出来** ——
        // 它取决于用户在 foobar 里设的 DSD 输出速率。实测一台设成 192000
        // 的机器上，2822400 出来就是 192000（甚至不是整数分频，中间还过了
        // 重采样）；换个人设成 352800 就会变成 352800。Spek 显示 352800 是
        // 因为它走自己的 libavcodec 链，与 foobar 的偏好设置无关，所以两边
        // 频率轴上限不同并不代表哪边错了。
        //
        // 结论：唯一可信的来源是解码器实际吐出的 audio_chunk。
        //
        // 拿错了会同时错三处（以 352800 为例，误差因子 8 倍）：
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
        // 探测到的解码块大小（每声道样本数），0 = 未知。T5 用它算 preroll。
        int probe_chunk = 0;

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
                // 顺手记下块大小：这一块反正已经解出来了，读它不花钱。
                probe_chunk = (int)probe.get_sample_count();
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

        // ================================================================
        // T7：真实 PCM 率已知，现在才能求值标题
        //
        // 必须在探测块之后：%analysis_samplerate% 的值来自
        // out.sample_rate，而它刚刚被上面的 probe 覆盖成解码器实测值。
        // 放在探测之前（T7 之前的位置）拿到的还是元数据标称值，那这整个
        // 需求就没法实现。
        //
        // 具体注入方式见 format_track_title 及 analysis_info_hook 的注释。
        // 失败降级：函数内部吞掉所有异常且不动 out.title，所以这里会保留
        // 前面设好的路径名兜底 —— 表头没标题比整轨分析失败好得多。
        // ================================================================
        try {
            service_ptr_t<titleformat_object> obj;
            const char* fmt = m_title_format.empty() ? "%title%" : m_title_format.c_str();
            static_api_ptr_t<titleformat_compiler>()->compile_safe(obj, fmt);
            format_track_title(track, obj, out.sample_rate, out.source_sample_rate, out.title);
        } catch (...) {
            // compile_safe 抛异常，保留兜底路径名
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
        //   2) Spek 平均区间内的**全部**非重叠 FFT（本例 85 次），我们
        //      受解码预算限制只能取其中一部分，取法见下方 T6。
        // ================================================================

        // ================================================================
        // T6：把「取样点数」和「每点连发数」拆开
        //
        // 【问题】用户在采样率对齐后仍反馈"SC 还是更糊更噪"。
        // 定量核对（Hotel California, DSD64 → 352800 Hz, 803 列）：
        //     每列区间 = 1.399 亿 / 803 = 174220 样本（494 ms）
        //     Spek 在区间内做 174220 / 2048 = 85 次非重叠 FFT
        //     我们只做 2 次
        // 换成画面上看得见的量（对数域噪声标准差 ≈ 4.34/sqrt(N) dB）：
        //     N=2  → 3.07 dB 起伏（占 120 dB 调色板的 2.6%）
        //     N=85 → 0.47 dB 起伏（0.4%）
        // 差 6.5 倍。这同时解释了"糊"和"噪"—— 3 dB 的随机斑点糊在谐波
        // 线上，主观感受就是线条发毛、不锐利。是同一个病。
        //
        // 【为什么不能直接把 N 提到 85】
        // 每个取样点要付一次 seek + 一整块解码。85 个点 = decode 从
        // 214ms 涨到 4 秒量级，等于把整轨解完，S3 的意义全丢了。
        //
        // 【解法：榨干块内空间，白拿一倍降噪】
        // T2/T5 已确认解码器按固定整块计费（本例块 = 4704 恒定）。
        // 那一整块反正都要解出来，所以块内能塞几个**非重叠** FFT 就
        // 白拿几个 —— 第二次 FFT 的解码成本是零：
        //     改前：preroll 2651 + nfft 2048        = 4699 → 1 次 FFT
        //     改后：preroll  608 + nfft 2048 × 2    = 4704 → 2 次 FFT
        // 取样点数仍是 2，decode 一个样本都不多解，N 却从 2 变成 4。
        //
        // 代价只有一个：preroll 从 2651 降到 608（T5 刚提上去的余量）。
        // 这是把"FIR 收敛冗余"换成"确定的 1.41 倍降噪"。608 PCM 样本
        // 对应 4864 个 DSD 样本的预热，与 T1 时期的 1024（8192 DSD）
        // 同量级，而 T1 那版画面本身并无问题（当时的"变糊"已查明是
        // 采样率量程变化所致，与 preroll 无关）。这笔交换是值得的。
        //
        // 注意 burst 的 FFT 必须**非重叠**（间隔 nfft 而不是 hop）：
        //   - 重叠的 FFT 看的是几乎相同的样本，噪声高度相关，
        //     平均下去几乎不降噪，1/sqrt(N) 根本不成立；
        //   - 非重叠才能拿到独立样本，也正是 Spek 的做法
        //     （其条件 frames % p->nfft == 0 就是每 nfft 步进一次）。
        // 为此下面用 fft_end 的**绝对样本位置**来排布连发窗口，
        // 而不是复用以 hop 为单位的 avg_hop_slots。
        // ================================================================
        int seek_points = stride < MAX_SEEK_POINTS_PER_COLUMN
            ? stride : MAX_SEEK_POINTS_PER_COLUMN;
        if (seek_points < 1) seek_points = 1;

        // 一个解码块内装得下几个非重叠 nfft 窗口。
        // probe_chunk 来自 F1 探测块（那一块本来就要解，读它零成本）。
        // 探测不到时保守取 1，退化为 T5 的行为。
        int burst = 1;
        if (probe_chunk > 0) {
            burst = probe_chunk / nfft;
            if (burst < 1) burst = 1;
            if (burst > MAX_BURST_PER_POINT) burst = MAX_BURST_PER_POINT;
        }

        // 连发窗口不能越过本列的时间区间，否则会偷到下一列的内容，
        // 时间轴上表现为列间涂抹。区间长度 = stride * hop 个样本。
        {
            const int64_t col_span = (int64_t)stride * (int64_t)hop;
            const int64_t span_per_point = col_span / seek_points;
            int fit = (int)(span_per_point / (int64_t)nfft);
            if (fit < 1) fit = 1;
            if (burst > fit) burst = fit;
        }

        const int avg_per_col = seek_points * burst;

        // 在 [0, stride) 内均匀挑 seek_points 个 hop 索引作为**连发起点**。
        // 例：stride=10, seek_points=2 → 命中 hop 序号 0, 5
        std::vector<int> avg_hop_slots((size_t)seek_points);
        for (int i = 0; i < seek_points; i++) {
            avg_hop_slots[(size_t)i] = (int)((int64_t)i * stride / seek_points);
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
        //     columns × avg_per_col = 800 × 4 = 3200 次 FFT，
        // 每次 FFT 只需要窗口内 nfft = 2048 个连续样本。其中每个取样点
        // 的 burst 次 FFT 是首尾相接的，合计需要
        //     columns × seek_points × nfft × burst = 800×2×2048×2 ≈ 655 万
        // 个样本 —— 占 8.5 亿的 0.8%。
        // 也就是说 99.2% 的逐样本工作是纯粹浪费。
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

        // 逐列、逐取样点、逐连发序号地推进「下一个 FFT 窗口的结束位置」。
        // 第 c 列第 s 个取样点的 hop 序号为 c*stride + avg_hop_slots[s]，
        // 该 hop 的窗口右边界（不含）= (hop_index + 1) * hop；
        // 点内第 b 个连发窗口再往后错开 b*nfft 个样本（非重叠，见 T6）。
        int cur_col = 0;
        int cur_point = 0;
        int cur_burst = 0;

        // ================================================================
        // 一个被否决的改法，以及 T6 如何把它的洞察换了个方向用
        //
        // 【被否决的版本：用连续块**取代**均匀铺开】
        // 曾考虑把同一列的两个平均窗口从「在 stride 区间内均匀铺开」改成
        // 「首尾相接的连续块」，这样每列只需 1 次 seek，seek 次数从 1600
        // 降到 800。
        //
        // 当初按「每样本计费」估算，以为能省 1.5 倍（983 万 → 655 万样本），
        // 结论是"省 0.4% 不值得动画质"。T2 量出解码器**按整块计费**
        // （块 4704 恒定）之后重算，连这 1.5 倍都不存在：
        //
        //   均匀铺开：preroll+2048 = 4704 → 1 块/取样点
        //             1600 次 × 4704 = 753 万样本
        //   连续块：  preroll+4096 = 6752 → 跨 2 块
        //             800 次 × 9408 = 753 万样本
        //
        // 完全相等 —— 少一半 seek 次数，但每次要多跨一个块，两者精确抵消。
        // 所以作为**替代**方案它是纯粹的画质损失、零收益：一列只反映
        // 4096 个样本（区间的 2.4%）那一瞬间，遇到列内有瞬态时容易采偏，
        // 而均匀铺开的两个窗口落在区间两端，代表性明显更好。
        //
        // 【T6 采纳的版本：连续块**叠加**在均匀铺开之上】
        // 上面那笔账里藏着一个正面结论：连续块方案的 6752 个样本仍然只
        // 跨 2 个块，也就是说**一个块里塞得下两个非重叠 FFT 而不多付钱**。
        // 既然如此，就不要用连续块去替换均匀铺开（那是拿画质换零收益），
        // 而是在**保留** 2 个均匀铺开取样点的前提下，让每个点在自己那一
        // 块内多做一次 FFT：
        //     2 个点 × 每点 2 次连发 = 每列 4 帧平均
        //     解码量 1600 × 4704 = 753 万样本（与改前**完全相同**）
        // 时间代表性一点没丢（取样点位置不变），噪声降 sqrt(2) 倍。
        //
        // 所以取样点位置**一个字节都不动**：S3/T5 只改「怎么把样本取到
        // 手」。T6 是唯一动了窗口集合的改动 —— 它只**新增**窗口，不移动
        // 任何已有窗口，且新增的都落在已付费的块内。
        //
        // 【"画面变了就是 seek 有 bug"这条判据要小心用】
        // 实测踩过一次：改 preroll 的同一轮里画面确实变糊了，当时差点去查
        // seek，结果是用户同时把 foobar 的 DSD 输出速率从 192000 改成了
        // 352800，Nyquist 从 96k 变 176.4k，轴量程变了。用这条判据之前，
        // 必须先确认 R4 日志里的采样率那一项没变。
        // ================================================================

        // 返回当前待处理 FFT 的窗口结束位置（全局样本下标）；
        // 所有列都做完则返回 -1。
        auto next_fft_end = [&]() -> int64_t {
            if (cur_col >= columns) return -1;
            const int64_t hop_index =
                (int64_t)cur_col * stride + avg_hop_slots[(size_t)cur_point];
            // 连发窗口紧邻排列、互不重叠：+ b*nfft。
            // 用绝对样本偏移而不是 hop 偏移 —— hop 只有 nfft/2，按 hop
            // 排会让相邻窗口重叠 50%，噪声高度相关，平均下去不降噪。
            return (hop_index + 1) * (int64_t)hop
                 + (int64_t)cur_burst * (int64_t)nfft;
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
        //      只用它把解码器状态喂热。见下方 T5：由于解码器按整块计费，
        //      这段预热在「块大小 - nfft」以内时是**完全免费**的。
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

        // ================================================================
        // T5：preroll 取"整块内免费"的最大值（T6 起要为连发窗口让位）
        //
        // 【T1 的两个结论都被实测推翻了，记录在这里避免重犯】
        //
        // T1 把 preroll 从 nfft*2(4096) 降到 nfft/2(1024)，当时写的理由是
        // "每窗口取样量 6144 → 3072，直接砍半"，并断言"只影响丢弃掉的样本，
        // 画面必须与改前完全一致"。两句话都错：
        //
        //   错误一：成本不是线性的。T2 的 chunk 统计量出
        //           chunks 1606, avg 4704, max 4704
        //   avg == max 说明解码器**按固定整块计费**（4704 = 352800/75，
        //   正好一个 1/75 秒的 DSD 帧）。所以真实成本只看"跨几个块"：
        //       preroll + nfft*burst ≤ 4704  → 1 块
        //       preroll + nfft*burst ≤ 9408  → 2 块
        //   改前 6144 落在 2 块，改后 3072 落在 1 块。收益是**台阶式的
        //   2 倍**，与 preroll 具体取 1024 还是 2000 无关。
        //
        //   错误二：用户实测"画面变糊了"。但归因也错了 —— 那一轮用户同时
        //   把 foobar 的 DSD 输出从 192000 改成了 352800，频率轴顶端
        //   (Nyquist) 从 96k 变成 176.4k。FFT 点数固定 2048，于是每个 bin
        //   从 93.75 Hz 变成 172.3 Hz，且 0-20kHz 的乐音区被压进原来 54%
        //   的高度。变糊是轴量程变化的必然结果，与 preroll 无关。
        //
        // 【块内空间怎么分配】
        // 一整块反正都要解出来，所以块内的每个样本都是已付费的。T5 起
        // 把整块吃满，但"吃满"有两种花法，T6 改了优先级：
        //
        //   T5：全给预热。preroll 2651 + nfft 2048 = 4699，1 次 FFT。
        //   T6：先喂 FFT，剩下的给预热。
        //       preroll 603 + nfft 2048 × 2 = 4699，2 次 FFT。
        //
        // 两者解码量完全相同（都是 1 块），但后者每列的平均帧数翻倍，
        // 换来 sqrt(2) ≈ 1.41 倍降噪。预热只需让解码器 FIR 收敛，几百
        // 个样本足够（603 PCM = 4864 个 DSD 样本），把上千个样本囤在
        // 预热区是浪费 —— 那些配额喂给 FFT 才有画质回报。
        //
        // probe_chunk 来自 F1 探测块（那一块本来就要解，读它零成本）。
        // 探测失败时退回 nfft/2，即 T1 的保守值。
        //
        // 【probe_chunk 偏差的方向是安全的】
        // 实测 preroll 算出 2651 而非预期的 2656，反推 probe_chunk = 4699，
        // 比稳态 4704 少 5 个样本 —— 解码器吐出的**第一块**偏短（DSD 抽取
        // FIR 通常会在起点扣掉自己的群延迟做对齐，5 个 PCM 样本 = 40 个
        // DSD 样本，量级吻合）。偏小恰好落在安全的一侧：算出的 preroll
        // 偏保守，仍在 1 块内。反方向（首块比稳态**大**）才危险，会让
        // preroll+nfft*burst 跨过块边界使 decode 翻倍。这个风险已经可
        // 观测：R4 日志同时打了 preroll、burst 和 avg，只要
        //     preroll + 2048*burst ≤ avg
        // 成立就没跨块，换格式或换采样率时看一眼这三个数即可。
        // ================================================================
        int64_t preroll = (int64_t)nfft / 2;
        {
            // FFT 窗口先占坑，剩下的才是预热配额。
            const int64_t fft_room = (int64_t)nfft * burst;
            if ((int64_t)probe_chunk > fft_room) {
                const int64_t free_room = (int64_t)probe_chunk - fft_room;
                // 上限夹紧：块特别大时(某些格式一次吐几十万样本)会让
                // preroll 大到毫无意义 —— FIR 收敛只需几百个样本，
                // nfft*2 已是数倍冗余，再多纯属浪费。
                const int64_t preroll_cap = (int64_t)nfft * 2;
                preroll = free_room < preroll_cap ? free_room : preroll_cap;
            }
        }
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
            n_chunks++;
            if (nsamples > max_chunk) max_chunk = nsamples;

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
                used_samples += take;

                // ---- 阶段 C：窗口填满 → 做 FFT，并推进到下一个目标 ----
                if (pos >= fft_end) {
                    if (contig >= nfft) {
                        run_fft_into_accum();
                    }

                    // 两层推进：先走完本取样点的所有连发窗口，再换点。
                    // 连发窗口在样本上紧邻，所以 phase 0 的 seek 门槛判断
                    // 会发现"已经在位置上"而不 seek，直接顺序读下去 ——
                    // 这正是连发不额外花解码成本的原因。
                    cur_burst++;
                    if (cur_burst >= burst) {
                        cur_burst = 0;
                        cur_point++;
                        if (cur_point >= seek_points) {
                            // 本列的取样点都取完了，输出这一列
                            cur_point = 0;
                            if (acc_count > 0) {
                                flush_column();
                            }
                            cur_col++;
                        }
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
        //
        // T2 追加的三个数用来解释 decoded 为何远高于理论值（见上方 T2 注释）：
        //   chunks   run() 返回数据的次数
        //   avg      decoded/chunks，即平均块大小 —— 判定性的那个数
        //   max      最大块，用来区分块大小恒定还是偶发大块
        //   used     真正进 FFT 的样本占解码量的千分比。因为解码器按整块
        //            计费，理论上限是 nfft*burst/块大小（本例 4096/4704 =
        //            870‰），**不是** nfft/(preroll+nfft)。
        //   preroll  T6 算出来的实际预热长度（块内喂完 FFT 后的余量）。
        //   burst    每个取样点连发几次非重叠 FFT，由块大小算出。
        //   avgN     每列实际平均的帧数 = seek_points × burst。画面噪声
        //            按 4.34/sqrt(avgN) dB 走，Spek 在本例是 85。
        //
        // 【换格式/换采样率时的自检公式】
        //     preroll + 2048 * burst ≤ avg
        // 成立说明每个取样点仍只解 1 个块。一旦不成立就是跨块了，
        // decode 会成倍上涨 —— 这三个数就是为了让这件事一眼可见。
        // ================================================================
        {
            const double total_ms = t_total.query() * 1000.0;
            const int64_t planned = (int64_t)(out.duration * out.sample_rate);
            const int decoded_permille = planned > 0
                ? (int)((int64_t)1000 * decoded_samples / planned)
                : 0;
            const int avg_chunk = n_chunks > 0
                ? (int)(decoded_samples / n_chunks)
                : 0;
            const int used_permille = decoded_samples > 0
                ? (int)((int64_t)1000 * used_samples / decoded_samples)
                : 0;
            const double other_ms = total_ms
                - (t_open + t_seek + t_decode + t_fft) * 1000.0;
            console::printf(
                "spectrum_compare: %s | %ims total"
                " (open %i, seek %i x%i, decode %i, fft %i x%i, other %i)"
                " | %i Hz %ich, %i cols, decoded %i/1000 of stream"
                " | chunks %i, avg %i, max %i, used %i/1000 of decoded"
                " | preroll %i, burst %i, avgN %i",
                out.title.c_str(),
                (int)total_ms,
                (int)(t_open * 1000.0),
                (int)(t_seek * 1000.0), n_seek,
                (int)(t_decode * 1000.0),
                (int)(t_fft * 1000.0), n_fft,
                (int)(other_ms < 0 ? 0 : other_ms),
                out.sample_rate, out.channels,
                out.time_frames, decoded_permille,
                n_chunks, avg_chunk, max_chunk, used_permille,
                (int)preroll, burst, avg_per_col);
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
