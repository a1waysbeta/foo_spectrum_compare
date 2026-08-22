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

        FFTPlan fft(m_fft_bits);
        int nfft = fft.get_input_size();
        int nbins = fft.get_output_size();
        out.fft_bins = nbins;

        // Decide hop size and number of time columns.
        //
        // Previous (v1): hop = total_samples / m_target_frames, min = nfft/4,
        //                1 FFT per column, NO overlap.
        // This (v2) is closer to Spek and gives much smoother time-domain
        // rendering plus avoids the "blocks of 2px wide columns" blocky
        // look that users described:
        //   - we fix hop = nfft / 4  ↔ 75% window overlap (industry
        //                               standard for spectrogram display).
        //   - the number of time columns is then the natural total FFTs
        //     possible given hop = nfft/4:
        //         total_hops = ceil(total_samples / hop) - 3
        //     (we lose the first 3 half-windows because they're not
        //      fully overlapped; if total is small we clamp to 1 minimum).
        //   - we cap columns to m_target_frames (default 800, or higher if
        //     the user passed one via set_time_resolution); that keeps
        //     memory / paint cost flat on 3-hour mixes.  If the cap is
        //     smaller than total_hops we aggregate several consecutive
        //     FFTs per output column (Spek's same averaging).
        double total_samples = out.duration * out.sample_rate;
        int hop = nfft / 4;                  // 75% overlap, matches Spek's feel
        if (hop < 1) hop = 1;
        int64_t total_hops_i64 = total_samples > 0
            ? (int64_t)(total_samples / hop) - 3
            : 0;
        if (total_hops_i64 < 1) total_hops_i64 = 1;
        // Target frames: use user's setting if > 0, else 800 minimum,
        // don't exceed the realistic total_hops we can actually produce
        // (so short clips don't get upscaled blanks).
        //
        // NOTE: We intentionally use pfc::min_t<T> / pfc::max_t<T> here
        // instead of std::min / std::max.  The SDK PFC headers we pull
        // in (via stdafx.h → foobar2000 SDK → libPPUI → Windows.h) can
        // expose the <windows.h> object-like macros:
        //   #define min(a,b) (((a)<(b))?(a):(b))
        //   #define max(a,b) (((a)>(b))?(a):(b))
        // When those macros are live, writing `std::max(1, x)` gets
        // textually expanded by the preprocessor before the compiler
        // ever sees it, producing C2589 / C2664 on the `::` token.
        // The classic "wrap the qualified name in parens" workaround
        // (std::max)<T>(a,b) compiles with some MSVC frontends but not
        // reliably with /permissive- + /std:c++17 (it can produce
        // C2440: cannot convert from 'overloaded-function' to int).
        // pfc::min_t/max_t live in a PFC-namespaced template, their
        // names do NOT collide with the object-like max(a,b) macros
        // (different arity + qualified namespace → macro pattern
        // doesn't match), so they're 100 % safe regardless of
        // NOMINMAX / permissive mode / language version.
        int target_cols = m_target_frames > 0 ? m_target_frames : 800;
        int columns = (int)pfc::min_t<int64_t>(
            (int64_t)target_cols,
            pfc::max_t<int64_t>(1, total_hops_i64));
        // Bucket = how many consecutive FFTs we average per displayed column.
        // Minimum 1 so we never divide by zero.
        int ffts_per_col = (int)pfc::max_t<int64_t>(
            1,
            total_hops_i64 / pfc::max_t<int64_t>(1, (int64_t)columns));
        // Re-derive columns so the last bucket has full count too (avoids a
        // tiny final column whose average would be noisier).
        columns = (int)pfc::max_t<int64_t>(
            1,
            total_hops_i64 / pfc::max_t<int64_t>(1, (int64_t)ffts_per_col));

        // Ring buffer for audio samples (mono mixed down)
        std::vector<float> buffer(nfft * 4, 0.0f);
        int buf_pos = 0;
        int buf_filled = 0;

        // Accumulator for averaging multiple FFTs per displayed time column
        std::vector<float> accum(nbins, 0.0f);
        int accum_count = 0;
        int samples_since_fft = 0;
        int ffts_in_current_bucket = 0;

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

                // Run an FFT every `hop` new audio samples.  With hop =
                // nfft/4 this gives 75% consecutive overlap between windows,
                // which eliminates time-domain smearing when combined with
                // the multi-FFT-per-column average below.
                if (samples_since_fft >= hop && buf_filled >= nfft) {
                    samples_since_fft = 0;

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

                    // Accumulate into current bucket's running average
                    for (int j = 0; j < nbins; j++) {
                        accum[j] += fft.get_output(j);
                    }
                    accum_count++;
                    ffts_in_current_bucket++;

                    // Emit one displayed time column when we've averaged
                    // enough FFTs for this bucket.
                    if (ffts_in_current_bucket >= ffts_per_col &&
                        (int)frame_data.size() / nbins < columns)
                    {
                        for (int j = 0; j < nbins; j++) {
                            frame_data.push_back(accum[j] / (float)accum_count);
                        }
                        std::fill(accum.begin(), accum.end(), 0.0f);
                        accum_count = 0;
                        ffts_in_current_bucket = 0;
                    }
                }
            }
        }

        // Process remaining data.  If we have an unterminated bucket we
        // flush it (as long as there's >=1 FFT averaged).  If not but
        // there's at least one full window in the ring, we do one last
        // FFT so songs shorter than the bucket boundary still render.
        if (accum_count == 0 && buf_filled >= nfft &&
            (int)frame_data.size() / nbins < columns)
        {
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
                accum[j] += fft.get_output(j);
            }
            accum_count = 1;
        }
        if (accum_count > 0 && (int)frame_data.size() / nbins < columns) {
            for (int j = 0; j < nbins; j++) {
                frame_data.push_back(accum[j] / (float)accum_count);
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
