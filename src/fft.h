#pragma once

#include <vector>

// 实数输入 FFT（radix-2），自带实现，不依赖 FFmpeg。
//
// 对外接口与旧版全复数实现完全一致（set_input / execute / get_output），
// 内部换成了两项优化：
//
//   P1-1 实数 FFT：音频输入是纯实数，旧实现把虚部填 0 做 N 点全复数
//        FFT，一半的蝶形运算作用在恒为 0 的虚部上。现改为把相邻两个
//        实数样本打包成一个复数，做 N/2 点复数 FFT 后再重组出实数频谱，
//        蝶形运算量减半。
//
//   P1-2 去 std::complex + 预计算旋转因子：改用平坦的 re/im 数组，
//        旋转因子全部查表。旧实现用 w *= wlen 递推，误差会沿蝶形逐级
//        累积；查表则每个因子都是精确值。
class FFTPlan {
public:
    explicit FFTPlan(int nbits);
    ~FFTPlan();

    int get_input_size() const { return m_input_size; }
    int get_output_size() const { return m_output_size; }

    // 直接暴露输入缓冲，供调用方一趟写入（省掉中转数组和逐点函数
    // 调用）。长度为 get_input_size()，execute() 会读走全部内容。
    float* input_data() { return m_input.data(); }

    // 返回该频点的**功率** re²+im²，而非幅度。
    //
    // 旧实现输出 std::abs() 即幅度，但上层的区间平均必须在功率域累加
    // （见 spectrum_analyzer.cpp 的 P0-2），于是每个频点要先开方再平方，
    // 一次 FFT 白做 1025 次 sqrt。直接给功率把这对运算全部省掉，
    // 开方只在每列输出时做一次。
    float get_output_power(int i) const { return m_output[i]; }

    void execute();

private:
    int m_nbits;
    int m_input_size;   // N，实数输入长度
    int m_output_size;  // N/2 + 1 个频点
    int m_half;         // M = N/2，内部复数 FFT 的长度

    std::vector<float> m_input;
    std::vector<float> m_output;   // 功率谱 re²+im²，长度 m_output_size

    // M 点复数 FFT 的工作缓冲（实部/虚部分离存放）
    std::vector<float> m_re;
    std::vector<float> m_im;

    // 蝶形旋转因子表：W_M^j = e^(-2πi·j/M)，j = 0 .. M/2-1
    std::vector<float> m_tw_re;
    std::vector<float> m_tw_im;

    // 实数频谱重组用的旋转因子表：W_N^k = e^(-2πi·k/N)，k = 0 .. M
    std::vector<float> m_out_tw_re;
    std::vector<float> m_out_tw_im;

    // 位反转表，作用于 M 点复数序列的下标
    std::vector<int> m_reverse_table;

    void build_tables();
};
