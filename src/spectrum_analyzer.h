#pragma once

#include "fft.h"
#include "palette.h"
#include <vector>
#include <string>
#include <atomic>
#include <mutex>

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
    int fft_bins = 0;      // number of frequency bins (output_size)
    int time_frames = 0;   // number of time columns
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

    // Analyze a track from a metadb_handle_ptr; runs synchronously
    // Returns true on success
    bool analyze(metadb_handle_ptr track, SpectrumData& out, abort_callback& abort);

    // Configuration
    void set_fft_bits(int bits) { m_fft_bits = bits; }
    int get_fft_bits() const { return m_fft_bits; }
    void set_window(window_function_t w) { m_window = w; }
    window_function_t get_window() const { return m_window; }
    void set_time_resolution(int frames) { m_target_frames = frames; }
    int get_time_resolution() const { return m_target_frames; }

private:
    int m_fft_bits = 11; // 2048-point FFT by default
    window_function_t m_window = WINDOW_DEFAULT;
    int m_target_frames = 800; // target number of time columns

    void apply_window(float* data, int n, window_function_t w);
    float get_window_val(window_function_t f, int i, int n);
};
