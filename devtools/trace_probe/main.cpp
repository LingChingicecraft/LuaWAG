// =============================================================================
//  main.cpp —— 时序打点演示 + "观测扰动"实测
//
//  同一个程序编两遍（-DWAG_TRACE=0 / =1）：
//      两次的**模拟工作量完全相同**，差别只有打点本身。
//      ⇒ 两者 mean_us 的差额就是"打点这件事的成本"。
//      ⇒ 关掉那一版必须是"零分支"，不是"判断后不走"。
//
//  用法：
//      trace_demo [--frames N] [--csv <path>] [--text N]
// =============================================================================
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <chrono>

#include "trace.hpp"

namespace {

volatile std::uint64_t g_sink = 0;

/// 固定工作量的"打工"循环（不优化掉）。
inline void spin(std::uint32_t iters) {
    std::uint64_t x = g_sink;
    for (std::uint32_t i = 0; i < iters; ++i) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
    }
    g_sink = x;
}

using clock_type = std::chrono::steady_clock;

inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch())
            .count());
}

/// 跑一帧：12 个阶段，每阶段固定工作量。
void run_frame(std::uint32_t frame) {
#if WAG_TRACE
    WAG_TRACE_FRAME(frame);
#else
    (void)frame;   // 关掉时它是纯模拟输入，不留未用参数告警
#endif
    for (int s = 0; s < 12; ++s) {
#if WAG_TRACE
        WAG_TRACE_ENTER(static_cast<wag::trace::stage>(s + 1));
#endif
        spin(200);

        // 每帧在 4 个阶段各放一个"事件"（模拟输入边沿 / 容器回收 / 碰撞命中 / 绘制条目）
        if (s == 3) {
#if WAG_TRACE
            WAG_TRACE_EVENT(wag::trace::domain::input, frame % 512u, (frame & 1u) | (((frame >> 1) & 1u) << 1));
#endif
        } else if (s == 5) {
#if WAG_TRACE
            WAG_TRACE_EVENT(wag::trace::domain::collide, frame % 97u, frame % 13u);
#endif
        } else if (s == 7) {
#if WAG_TRACE
            WAG_TRACE_EVENT(wag::trace::domain::draw, 64u, 16u);
#endif
        }

#if WAG_TRACE
        WAG_TRACE_EXIT(static_cast<wag::trace::stage>(s + 1));
#endif
    }
}

} // namespace

int main(int argc, char** argv) {
    std::uint32_t frames = 240;
    std::string csv_path;
    std::size_t text_rows = 32;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            csv_path = argv[++i];
        } else if (std::strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
            text_rows = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("trace_demo [--frames N] [--csv <path>] [--text N]\n");
            return 0;
        }
    }
    if (frames == 0) {
        frames = 1;
    }

    std::printf("=== trace_demo（WAG_TRACE=%d）===\n", WAG_TRACE);
    std::printf("阶段数 12，每阶段工作量固定；帧数 %u\n", frames);

#if WAG_TRACE
    wag::trace::reset(0);
    std::printf("环容量 %zu\n", wag::trace::capacity());
#endif

    // 预热（让第一次分配/缓存无关紧要）
    for (std::uint32_t f = 0; f < 16; ++f) {
        run_frame(f);
    }

    std::vector<double> us;
    us.reserve(frames);

    const std::uint64_t t_begin = now_ns();
    for (std::uint32_t f = 0; f < frames; ++f) {
        const std::uint64_t a = now_ns();
        run_frame(1000u + f);
        const std::uint64_t b = now_ns();
        us.push_back(static_cast<double>(b - a) / 1000.0);
    }
    const std::uint64_t t_end = now_ns();

    std::vector<double> sorted = us;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (double v : us) {
        sum += v;
    }
    const double mean = sum / static_cast<double>(us.size());
    const double p50 = sorted[sorted.size() / 2];
    const double p99 = sorted[static_cast<std::size_t>(static_cast<double>(sorted.size() - 1) * 0.99)];

    std::printf("\n每帧耗时（含全部 12 阶段）：mean %.2f us | p50 %.2f us | p99 %.2f us | total %.3f ms\n",
                mean, p50, p99, static_cast<double>(t_end - t_begin) / 1e6);

#if WAG_TRACE
    std::printf("记录数 %zu，丢弃 %llu\n", wag::trace::size(),
                static_cast<unsigned long long>(wag::trace::dropped()));
    wag::trace::dump_text(std::cout, text_rows);
    if (!csv_path.empty()) {
        std::ofstream out(csv_path);
        if (out) {
            wag::trace::dump_csv(out, true);
            std::printf("（CSV 已写入 %s）\n", csv_path.c_str());
        } else {
            std::printf("WARN  写不了 %s\n", csv_path.c_str());
        }
    }
#else
    std::printf("（本构建 WAG_TRACE=0：所有打点编译为空，无记录）\n");
    (void)text_rows;
    (void)csv_path;
#endif

    // 机器可读行：供两份构建对比用。
    std::printf("RESULT trace=%s frames=%u mean_us=%.3f p50_us=%.3f p99_us=%.3f\n",
#if WAG_TRACE
                "on",
#else
                "off",
#endif
                frames, mean, p50, p99);

    return 0;
}
