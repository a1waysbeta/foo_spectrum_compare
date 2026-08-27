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
    , m_half(1 << (nbits - 1))
    , m_input(m_input_size, 0.0f)
    , m_output(m_output_size, 0.0f)
    , m_re(m_half, 0.0f)
    , m_im(m_half, 0.0f)
    , m_tw_re(m_half / 2, 0.0f)
    , m_tw_im(m_half / 2, 0.0f)
    , m_out_tw_re(m_half + 1, 0.0f)
    , m_out_tw_im(m_half + 1, 0.0f)
    , m_reverse_table(m_half, 0)
{
    build_tables();
}

FFTPlan::~FFTPlan() {}

void FFTPlan::build_tables() {
    const int M = m_half;
    const int mbits = m_nbits - 1;   // M 点 FFT 的位数

    // 位反转表（M 点、mbits 位）
    for (int i = 0; i < M; i++) {
        int rev = 0;
        int val = i;
        for (int j = 0; j < mbits; j++) {
            rev = (rev << 1) | (val & 1);
            val >>= 1;
        }
        m_reverse_table[i] = rev;
    }

    // 蝶形旋转因子 W_M^j = e^(-2πi·j/M)，j = 0 .. M/2-1
    //
    // 旧实现用 w *= wlen 递推，每级乘法都在累积舍入误差，到 2048 点的
    // 最后一级已经有可观漂移。这里全部按角度直算，每个因子都是精确值。
    for (int j = 0; j < M / 2; j++) {
        float ang = -2.0f * (float)M_PI * (float)j / (float)M;
        m_tw_re[j] = cosf(ang);
        m_tw_im[j] = sinf(ang);
    }

    // 实数频谱重组用的 W_N^k = e^(-2πi·k/N)，k = 0 .. M
    for (int k = 0; k <= M; k++) {
        float ang = -2.0f * (float)M_PI * (float)k / (float)m_input_size;
        m_out_tw_re[k] = cosf(ang);
        m_out_tw_im[k] = sinf(ang);
    }
}

void FFTPlan::execute() {
    const int M = m_half;
    float* re = m_re.data();
    float* im = m_im.data();

    // ================================================================
    // 步骤 1：打包 + 位反转
    //
    // 把相邻两个实数样本塞进一个复数：z[n] = x[2n] + i·x[2n+1]，
    // 于是 N 点实数 FFT 变成 M = N/2 点复数 FFT。位反转顺手在打包时
    // 完成，省一趟遍历。
    // ================================================================
    for (int i = 0; i < M; i++) {
        const int r = m_reverse_table[i];
        re[i] = m_input[2 * r];
        im[i] = m_input[2 * r + 1];
    }

    // ================================================================
    // 步骤 2：M 点原地复数 FFT
    //
    // 全部用平坦的 re[]/im[] 数组手写蝶形，不再走 std::complex。
    // std::complex<float> 的乘法带 NaN/Inf 特殊处理分支，编译器难以
    // 完全优化掉；手写四乘二加则可以直接向量化。
    // ================================================================
    for (int len = 2; len <= M; len <<= 1) {
        const int half = len >> 1;
        const int step = M / len;    // 查表步长：W_M^(j·step) = W_len^j
        for (int i = 0; i < M; i += len) {
            for (int j = 0; j < half; j++) {
                const int t = j * step;
                const float wr = m_tw_re[t];
                const float wi = m_tw_im[t];

                const int a = i + j;
                const int b = a + half;

                const float br = re[b];
                const float bi = im[b];
                // v = z[b] * W
                const float vr = br * wr - bi * wi;
                const float vi = br * wi + bi * wr;

                const float ar = re[a];
                const float ai = im[a];

                re[a] = ar + vr;
                im[a] = ai + vi;
                re[b] = ar - vr;
                im[b] = ai - vi;
            }
        }
    }

    // ================================================================
    // 步骤 3：从 M 点复数频谱重组出 N 点实数频谱
    //
    // 设 Z 为 z 的 M 点 DFT，Ze/Zo 分别为偶/奇下标子序列的频谱：
    //   Ze[k] = (Z[k] + conj(Z[M-k])) / 2
    //   Zo[k] = (Z[k] - conj(Z[M-k])) / (2i)
    //   X[k]  = Ze[k] + W_N^k · Zo[k]
    //
    // 展开成实数运算（A+iB = Z[k]，C+iD = Z[M-k]）：
    //   Ze_re = (A+C)/2      Ze_im = (B-D)/2
    //   Zo_re = (B+D)/2      Zo_im = (C-A)/2
    //
    // 两个端点由周期性单独处理（Z[M] = Z[0]，W_N^M = -1）：
    //   X[0] = A + B         （直流，实数）
    //   X[M] = A - B         （奈奎斯特，实数）
    // 其中 A = Z[0].re = 偶样本之和，B = Z[0].im = 奇样本之和。
    //
    // 输出存的是**功率** re²+im²，不是幅度 —— 见 get_output_power()
    // 的说明。
    // ================================================================
    {
        const float a0 = re[0];
        const float b0 = im[0];
        const float dc = a0 + b0;
        const float ny = a0 - b0;
        m_output[0] = dc * dc;
        m_output[M] = ny * ny;
    }

    for (int k = 1; k < M; k++) {
        const float A = re[k];
        const float B = im[k];
        const float C = re[M - k];
        const float D = im[M - k];

        const float ze_re = 0.5f * (A + C);
        const float ze_im = 0.5f * (B - D);
        const float zo_re = 0.5f * (B + D);
        const float zo_im = 0.5f * (C - A);

        const float wr = m_out_tw_re[k];
        const float wi = m_out_tw_im[k];

        const float xr = ze_re + wr * zo_re - wi * zo_im;
        const float xi = ze_im + wr * zo_im + wi * zo_re;

        m_output[k] = xr * xr + xi * xi;
    }
}
