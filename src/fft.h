#pragma once

#include <vector>
#include <complex>

// Simple Cooley-Tukey radix-2 FFT implementation
// Borrowed concept from Spek, but self-contained (no FFmpeg dependency)
class FFTPlan {
public:
    explicit FFTPlan(int nbits);
    ~FFTPlan();

    int get_input_size() const { return m_input_size; }
    int get_output_size() const { return m_output_size; }

    void set_input(int i, float v) { m_input[i] = v; }
    float get_output(int i) const { return m_output[i]; }

    void execute();

private:
    int m_nbits;
    int m_input_size;
    int m_output_size;
    std::vector<float> m_input;
    std::vector<float> m_output;
    std::vector<std::complex<float>> m_complex_buf;
    std::vector<int> m_reverse_table;

    void build_reverse_table();
};
