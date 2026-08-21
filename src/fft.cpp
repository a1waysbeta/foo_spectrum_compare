#include "stdafx.h"
#include "fft.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

FFTPlan::FFTPlan(int nbits)
    : m_nbits(nbits)
    , m_input_size(1 << nbits)
    , m_output_size((1 << (nbits - 1)) + 1)
    , m_input(m_input_size, 0.0f)
    , m_output(m_output_size, 0.0f)
    , m_complex_buf(m_input_size)
    , m_reverse_table(m_input_size)
{
    build_reverse_table();
}

FFTPlan::~FFTPlan() {}

void FFTPlan::build_reverse_table() {
    for (int i = 0; i < m_input_size; i++) {
        int rev = 0;
        int val = i;
        for (int j = 0; j < m_nbits; j++) {
            rev = (rev << 1) | (val & 1);
            val >>= 1;
        }
        m_reverse_table[i] = rev;
    }
}

void FFTPlan::execute() {
    // Bit-reversal permutation
    for (int i = 0; i < m_input_size; i++) {
        m_complex_buf[i] = std::complex<float>(m_input[m_reverse_table[i]], 0.0f);
    }

    // Cooley-Tukey FFT
    for (int len = 2; len <= m_input_size; len <<= 1) {
        float ang = -2.0f * (float)M_PI / len;
        std::complex<float> wlen(cosf(ang), sinf(ang));
        for (int i = 0; i < m_input_size; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int j = 0; j < len / 2; j++) {
                std::complex<float> u = m_complex_buf[i + j];
                std::complex<float> v = m_complex_buf[i + j + len / 2] * w;
                m_complex_buf[i + j] = u + v;
                m_complex_buf[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }

    // Compute magnitude spectrum (only first N/2+1 bins)
    for (int i = 0; i < m_output_size; i++) {
        m_output[i] = std::abs(m_complex_buf[i]);
    }
}
