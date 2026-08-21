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
    out.track_path = track->get_path();

    // Get track info (technical info is stored as key-value pairs in this SDK version)
    file_info_impl info;
    if (track->get_info_async(info)) {
        const char* sr = info.info_get("samplerate");
        if (sr) out.sample_rate = atoi(sr);
        const char* ch = info.info_get("channels");
        if (ch) out.channels = atoi(ch);
        out.duration = info.get_length();
    }

    // Get title
    try {
        titleformat_hook* hook = NULL;
        service_ptr_t<titleformat_object> obj;
        static_api_ptr_t<titleformat_compiler>()->compile_safe(obj, "%title%");
        pfc::string8 title;
        track->format_title(hook, title, obj, NULL);
        out.title = title.get_ptr();
    } catch (...) {
        out.title = track->get_path();
    }
    if (out.title.empty()) out.title = track->get_path();

    try {
        const t_uint32 decode_flags = input_flag_no_seeking | input_flag_no_looping;
        input_helper input;
        input.open(NULL, track, decode_flags, abort);

        FFTPlan fft(m_fft_bits);
        int nfft = fft.get_input_size();
        int nbins = fft.get_output_size();
        out.fft_bins = nbins;

        // Determine hop size to get approximately target_frames time columns
        double total_samples = out.duration * out.sample_rate;
        int hop = (int)(total_samples / m_target_frames);
        if (hop < nfft / 4) hop = nfft / 4;
        if (hop < 1) hop = 1;

        // Ring buffer for audio samples (mono mixed down)
        std::vector<float> buffer(nfft * 4, 0.0f);
        int buf_pos = 0;
        int buf_filled = 0;

        // Accumulator for averaging multiple FFTs per time frame
        std::vector<float> accum(nbins, 0.0f);
        int accum_count = 0;
        int samples_since_frame = 0;

        std::vector<float> frame_data; // output frames

        audio_chunk_impl_temporary chunk;
        while (input.run(chunk, abort)) {
            abort.check();

            const audio_sample* samples = chunk.get_data();
            int nch = chunk.get_channels();
            int nsamples = chunk.get_sample_count();

            // Mix down to mono and fill ring buffer
            for (int i = 0; i < nsamples; i++) {
                float mono = 0;
                for (int c = 0; c < nch; c++) {
                    mono += samples[i * nch + c];
                }
                mono /= nch;

                buffer[buf_pos] = mono;
                buf_pos = (buf_pos + 1) % (int)buffer.size();
                if (buf_filled < (int)buffer.size()) buf_filled++;

                samples_since_frame++;

                // When we have enough samples for a frame, compute FFTs
                if (samples_since_frame >= hop && buf_filled >= nfft) {
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

                    for (int j = 0; j < nbins; j++) {
                        accum[j] += fft.get_output(j);
                    }
                    accum_count++;

                    // Output one time frame
                    for (int j = 0; j < nbins; j++) {
                        frame_data.push_back(accum[j] / accum_count);
                    }

                    std::fill(accum.begin(), accum.end(), 0.0f);
                    accum_count = 0;
                    samples_since_frame = 0;
                }
            }
        }

        // Process remaining data
        if (accum_count > 0 || buf_filled >= nfft) {
            if (accum_count == 0 && buf_filled >= nfft) {
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
            if (accum_count > 0) {
                for (int j = 0; j < nbins; j++) {
                    frame_data.push_back(accum[j] / accum_count);
                }
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
