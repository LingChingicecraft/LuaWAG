// =============================================================================
//  soft_clip_probe.cpp —— 测引擎真正用的**软限幅曲线**（不是副本）
//
//  #include 的是 src/audio/soft_clip.hpp。
//  编译：g++ -O2 -I <repo>/src -o soft_clip_probe.exe soft_clip_probe.cpp
// =============================================================================

#include <cstdio>
#include <cmath>

#include "audio/soft_clip.hpp"

int main() {
    std::printf(" 输入      输出      与输入之差   备注\n");
    const float xs[] = {0.0f, 0.3f, 0.5f, 0.707f, 0.8f, 0.9f, 1.0f, 1.2f, 1.414f, 2.0f, 4.0f, 8.0f};
    for (float x : xs) {
        const float y = wag::audio::soft_clip(x);
        const char* note = "";
        if (x <= 0.8f) { note = "透明区（恒等）"; }
        else if (x >= 1.0f) { note = "被压"; }
        std::printf("%7.3f  %8.4f  %+11.5f   %s\n", (double)x, (double)y,
                    (double)(y - x), note);
    }

    // 上界检查：扫一遍确认永远 <= 1.0
    float worst = 0.0f;
    for (float x = 0.0f; x < 20.0f; x += 0.001f) {
        const float y = wag::audio::soft_clip(x);
        if (y > worst) { worst = y; }
    }
    std::printf("\n扫 0..20：最大输出 = %.6f（必须 ≤ 1.0）\n", (double)worst);

    // 透明性检查：0..0.8 必须恒等
    double max_err = 0.0;
    for (float x = 0.0f; x <= 0.8f; x += 0.001f) {
        const double err = std::fabs((double)wag::audio::soft_clip(x) - (double)x);
        if (err > max_err) { max_err = err; }
    }
    std::printf("0..0.8 区间最大误差 = %.9f（必须为 0）\n", max_err);

    // 单条 voice 峰值 0.707 会不会被碰
    std::printf("单条 voice 峰值 0.7070 → %.6f（应原样）\n",
                (double)wag::audio::soft_clip(0.707f));

    // 两条叠加 1.414 → 被压到多少
    std::printf("两条叠加 1.4140 → %.6f（应接近但 ≤ 1.0）\n",
                (double)wag::audio::soft_clip(1.414f));
    return 0;
}
