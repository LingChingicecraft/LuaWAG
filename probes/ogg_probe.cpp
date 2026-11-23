// =============================================================================
//  ogg_probe.c —— 一次性探针：量一个 ogg 的首尾静音与循环标签
//
//  编译（ucrt64）：
//    g++ -O2 -o %TEMP%\ogg_probe.exe ogg_probe.cpp
//        /path/to/stb_vorbis.c
//
//  输出：
//    · 总帧数、时长
//    · 头部静音长度（|x| ≤ 阈值视为静音）
//    · 尾部静音长度
//    · Vorbis 注释里的 loop 相关标签（LOOPSTART / LOOPLENGTH / LOOPEND …）
//    · 首尾各 0.2 s 的 RMS
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "stb_vorbis.c"

static const int kThreshold = 100;   // /32768 ≈ -50 dBFS

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法：ogg_probe <file.ogg>\n");
        return 2;
    }

    int err = 0;
    stb_vorbis* v = stb_vorbis_open_filename(argv[1], &err, nullptr);
    if (!v) {
        std::printf("打不开（错误码 %d）\n", err);
        return 1;
    }

    stb_vorbis_info info = stb_vorbis_get_info(v);
    const unsigned int total = stb_vorbis_stream_length_in_samples(v);
    std::printf("channels=%d rate=%d total_frames=%u (%.3f s)\n",
                info.channels, info.sample_rate, total,
                (double)total / info.sample_rate);

    // 注释标签（找 loop 相关）
    int n = 0;
    stb_vorbis_comment c = stb_vorbis_get_comment(v);
    for (int i = 0; i < c.comment_list_length; ++i) {
        std::string s = c.comment_list[i] ? c.comment_list[i] : "";
        std::string lower = s;
        for (auto& ch : lower) { ch = (char)tolower((unsigned char)ch); }
        if (lower.find("loop") != std::string::npos ||
            lower.find("title") != std::string::npos) {
            std::printf("comment: %s\n", s.c_str());
            ++n;
        }
    }
    if (n == 0) { std::printf("comment: （没有 loop/title 标签）\n"); }

    // 整段解码
    std::vector<short> pcm((size_t)total * info.channels);
    int got = stb_vorbis_get_samples_short_interleaved(v, info.channels, pcm.data(),
                                                       (int)pcm.size());
    stb_vorbis_close(v);
    if (got <= 0) { std::printf("解码 0 帧\n"); return 1; }

    const int ch = info.channels;
    auto silent_at = [&](int frame) {
        for (int k = 0; k < ch; ++k) {
            if (std::abs((int)pcm[(size_t)frame * ch + k]) > kThreshold) { return false; }
        }
        return true;
    };

    // 头部静音
    int head = 0;
    while (head < got && silent_at(head)) { ++head; }
    // 尾部静音
    int tail = 0;
    while (tail < got && silent_at(got - 1 - tail)) { ++tail; }

    std::printf("leading_silence = %d frames (%.3f s)\n", head, (double)head / info.sample_rate);
    std::printf("trailing_silence = %d frames (%.3f s)\n", tail, (double)tail / info.sample_rate);
    std::printf("total_gap_if_looped = %.3f s\n",
                (double)(head + tail) / info.sample_rate);

    // 首尾各 0.2 s 的 RMS
    auto rms = [&](int from, int count) {
        double acc = 0.0;
        int n2 = 0;
        for (int i = from; i < from + count && i < got; ++i) {
            for (int k = 0; k < ch; ++k) {
                const double x = pcm[(size_t)i * ch + k] / 32768.0;
                acc += x * x;
                ++n2;
            }
        }
        return (n2 > 0) ? std::sqrt(acc / n2) : 0.0;
    };
    const int win = info.sample_rate / 5;   // 0.2 s
    std::printf("rms_first_0.2s = %.6f\n", rms(0, win));
    std::printf("rms_last_0.2s  = %.6f\n", rms(got - win, win));
    std::printf("rms_middle_0.2s = %.6f\n", rms(got / 2, win));

    // 边界采样值（看是不是在非零处硬切）
    // 峰值与"若按 1.0 增益播放会不会削顶"
    int peak = 0;
    long long over = 0;
    double sum = 0.0;
    for (int i = 0; i < got; ++i) {
        for (int k = 0; k < ch; ++k) {
            const int v = (int)pcm[(size_t)i * ch + k];
            const int a = v < 0 ? -v : v;
            if (a > peak) { peak = a; }
            if (a > 32767) { ++over; }
            const double x = v / 32768.0;
            sum += x * x;
        }
    }
    const double rms_all = std::sqrt(sum / (double)(got * ch));
    std::printf("peak = %d (%.2f dBFS)  rms_all = %.4f (%.2f dBFS)  samples_over_fullscale = %lld\n",
                peak, 20.0 * std::log10((double)peak / 32768.0),
                rms_all, 20.0 * std::log10(rms_all + 1e-12), over);
    std::printf("若按 1.00 增益：峰值 %d（%s）\n", peak, peak >= 32767 ? "会削顶" : "不削顶");
    std::printf("若按 0.707 增益（我们的等功率中置）：峰值 %d（-3.01 dB，余量 %d）\n",
                (int)(peak * 0.7071), 32767 - (int)(peak * 0.7071));

    // "若整体再抬 3 dB（×1.414）会有多少样本被削"：|x| > 1/1.414 = 0.7071
    {
        const int th = (int)(32767.0 / 1.41421356);
        long long n_over = 0;
        long long n_all = 0;
        for (int i = head; i < got - tail; ++i) {
            for (int k = 0; k < ch; ++k) {
                const int v = (int)pcm[(size_t)i * ch + k];
                if ((v < 0 ? -v : v) > th) { ++n_over; }
                ++n_all;
            }
        }
        std::printf("若抬 3 dB：阈值 %d，超阈样本 %lld / %lld = %.3f%%\n",
                    th, n_over, n_all, 100.0 * (double)n_over / (double)n_all);
    }

    std::printf("first_sample = %d,%d\n", (int)pcm[0], (int)pcm[ch == 2 ? 1 : 0]);
    std::printf("last_sample  = %d,%d\n",
                (int)pcm[(size_t)(got - 1) * ch], (int)pcm[(size_t)(got - 1) * ch + (ch == 2 ? 1 : 0)]);
    return 0;
}
