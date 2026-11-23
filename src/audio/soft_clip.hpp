// =============================================================================
//  audio/soft_clip.hpp
//
//  软限幅：低于拐点完全透明，之上平滑压到 1.0。
//
//  【为什么不用硬 clamp】硬 clamp 把峰值切成方波，产生奇次谐波，
//    听感上更毛、动态更平。
//
//  【为什么拐点选 0.8】
//    · 单条 voice 等功率中置峰值只到 0.707 ⇒ 完全不碰这条曲线；
//    · 两条叠加到 1.414 才开始被压 ⇒ 只在真的超出时介入；
//    · 拐点以下恒等映射，不是 tanh 那种全程染色。
//
//  【数学】|x| ≤ k 时 y = x；否则
//        y = sign(x) · (k + (1−k)·tanh((|x|−k)/(1−k)))
//    ⇒ 上界严格等于 1.0，且一阶导在 k 处连续，不会引入折点。
// =============================================================================

#pragma once

#include <cmath>

namespace wag::audio {

inline float soft_clip(float x) {
    constexpr float kKnee = 0.8f;
    const float a = std::fabs(x);
    if (a <= kKnee) {
        return x;                                   // 透明区
    }
    const float over = (a - kKnee) / (1.0f - kKnee);
    const float shaped = kKnee + (1.0f - kKnee) * std::tanh(over);
    return (x < 0.0f) ? -shaped : shaped;
}

} // namespace wag::audio
