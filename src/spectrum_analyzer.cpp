#include "stdafx.h"
#include "spectrum_analyzer.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

SpectrumAnalyzer::SpectrumAnalyzer() {}
SpectrumAnalyzer::~SpectrumAnalyzer() {}

float SpectrumAnalyzer::get_window_val(window_function_t f, int i, int n) {
    // Pre-computed cos would be faster but this is fine
    float cf = 2.0f * (float)M_PI / (n - 1.0f);
    float coss_i = cosf(cf * i);
    float coss_2i = cosf(cf * 2 * i);
    float coss_3i = cosf(cf * 3 * i);

    switch (f) {
    case WINDOW_HANN:
        return 0.5f * (1.0f - coss_i);
    case WINDOW_HAMMING:
        return 0.53836f - 0.46164f * coss_i;
    case WINDOW_BLACKMAN_HARRIS:
        return 0.35875f - 0.48829f * coss_i + 0.14128f * coss_2i - 0.01168f * coss_3i;
    default:
        return 1.0f;
    }
}

void SpectrumAnalyzer::apply_window(float* data, int n, window_function_t w) {
    for (int i = 0; i < n; i++) {
        data[i] *= get_window_val(w, i, n);
    }
}

bool SpectrumAnalyzer::analyze(metadb_handle_ptr track, SpectrumData& out, abort_callback& abort) {
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
        const t_uint32 decode_flags = input_flag_no_seeking | input_flag_no_looping;
        input_helper input;
        input.open(NULL, track, decode_flags, abort);

        FFTPlan fft(FFT_BITS);
        int nfft = fft.get_input_size();
        int nbins = fft.get_output_size();
        out.fft_bins = nbins;

        // Decide hop size and number of time columns.
        //
        // Performance-tuned fast path (matches the pre-FFT-menu "秒加载"
        // behaviour the user wants back):
        //   - we aim for m_target_frames time columns spread evenly
        //     across the whole file;
        //   - hop = total_samples / target_cols, clamped so there's at
        //     least 50% window overlap (hop <= nfft/2) and never smaller
        //     than nfft/4 (avoids pathological oversampling on short
        //     clips);
        //   - exactly 1 FFT per displayed column, no bucket-averaging.
        //
        // This gives roughly 1200-1500 FFTs per 3-minute track at 2048
        // samples/FFT, which is the ~instant load the user remembers.
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
        // final count never exceeds `columns`.  Keeps 1 FFT/column.
        int stride = (int)pfc::max_t<int64_t>(
            1,
            total_hops_i64 / pfc::max_t<int64_t>(1, (int64_t)columns));
        columns = (int)pfc::max_t<int64_t>(
            1,
            total_hops_i64 / pfc::max_t<int64_t>(1, (int64_t)stride));

        // Ring buffer for audio samples (mono mixed down)
        std::vector<float> buffer(nfft * 2, 0.0f);
        int buf_pos = 0;
        int buf_filled = 0;

        int samples_since_fft = 0;
        int hops_since_last_emit = 0;

        std::vector<float> frame_data; // output frames: nbins floats per column
        frame_data.reserve((size_t)columns * (size_t)nbins);

        audio_chunk_impl_temporary chunk;
        while (input.run(chunk, abort)) {
            abort.check();

            const audio_sample* samples = chunk.get_data();
            // get_channels / get_sample_count return t_size = size_t.  We
            // cast to int with an explicit clamp because real audio chunks
            // are always well within INT_MAX samples / channels, but the
            // bare assignments used to trigger C4267.
            int nch = pfc::downcast_guarded<int, t_size>(chunk.get_channels());
            int nsamples = pfc::downcast_guarded<int, t_size>(chunk.get_sample_count());

            // Mix down to mono and fill ring buffer
            for (int i = 0; i < nsamples; i++) {
                float mono = 0;
                for (int c = 0; c < nch; c++) {
                    // audio_sample is typedef'd to float or double
                    // depending on SDK configuration.  Explicit cast to
                    // float avoids C4244 when the SDK chose double.
                    mono += static_cast<float>(samples[i * nch + c]);
                }
                mono /= nch;

                buffer[buf_pos] = mono;
                buf_pos = (buf_pos + 1) % pfc::downcast_guarded<int, size_t>(buffer.size());
                if (buf_filled < pfc::downcast_guarded<int, size_t>(buffer.size())) buf_filled++;

                samples_since_fft++;

                // Run an FFT every `hop` new audio samples.  With hop
                // between nfft/4 and nfft/2 this gives 50-75% overlap
                // — enough to smooth the time axis, then we decimate
                // with `stride` so we emit exactly 1 FFT per displayed
                // time column, NO extra averaging, NO accumulator copy.
                if (samples_since_fft >= hop && buf_filled >= nfft) {
                    samples_since_fft = 0;
                    hops_since_last_emit++;

                    if (hops_since_last_emit >= stride &&
                        (int)frame_data.size() / nbins < columns)
                    {
                        hops_since_last_emit = 0;

                        // Extract nfft samples from ring buffer (most recent)
                        std::vector<float> fft_in(nfft);
                        int start = (buf_pos - nfft + (int)buffer.size()) % (int)buffer.size();
                        for (int j = 0; j < nfft; j++) {
                            fft_in[j] = buffer[(start + j) % buffer.size()];
                        }

                        apply_window(fft_in.data(), nfft, m_window);
                        for (int j = 0; j < nfft; j++) {
                            fft.set_input(j, fft_in[j]);
                        }
                        fft.execute();

                        // 1 FFT → 1 time column, direct write
                        for (int j = 0; j < nbins; j++) {
                            frame_data.push_back(fft.get_output(j));
                        }
                    }
                }
            }
        }

        // Tail: if we haven't produced any column yet but have a full
        // window in the ring, emit one final FFT so short clips still
        // render (covers the <1-column edge case).
        if (frame_data.empty() && buf_filled >= nfft) {
            std::vector<float> fft_in(nfft);
            int start = (buf_pos - nfft + (int)buffer.size()) % (int)buffer.size();
            for (int j = 0; j < nfft; j++) {
                fft_in[j] = buffer[(start + j) % buffer.size()];
            }
            apply_window(fft_in.data(), nfft, m_window);
            for (int j = 0; j < nfft; j++) {
                fft.set_input(j, fft_in[j]);
            }
            fft.execute();
            for (int j = 0; j < nbins; j++) {
                frame_data.push_back(fft.get_output(j));
            }
        }

        out.time_frames = (int)(frame_data.size() / nbins);
        out.data = std::move(frame_data);

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
