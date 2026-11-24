// =============================================================================
//  light/light.hpp —— 光（一次性提交）
//
//  【为什么不是池】光每帧由脚本重新提交；没有句柄、没有生命周期。
//    池化只省极小开销，却引入"忘了 kill ⇒ 光不灭"的隐蔽语义。
//  【字段】全必填（除 softness/falloff）；引擎不替作者猜默认值。
//  【本文件是 header-only】它没有 GL 依赖；画光在 renderer。
// =============================================================================
#pragma once

#include <cstdint>

namespace wag::light {

// -----------------------------------------------------------------------------
//  光图分辨率
//
//  【这里只固定“光图有多大”，不再固定“世界有多大”】
//    光照覆盖的世界矩形由 renderer 的 light space 决定：
//      · 默认取启动后的当前 render；
//      · 作者可用 wag.light.set_space{...} 显式覆盖。
//    光图/遮蔽图/带掩码仍使用半分辨率：
//      光图   960 × 540
//      遮蔽图 960 × 540
//      带掩码 960 × 540
//    ⇒ 光图分辨率与 light space 的世界尺寸解耦：
//      局部 light space 会更清晰，大范围 light space 会更低清。
//
//  【改光图分辨率时】这里和 shader 的 mask 尺寸要一起改；
//    light space 的矩形不在本文件里固定。
// -----------------------------------------------------------------------------
inline constexpr int map_width = 960;
inline constexpr int map_height = 540;

enum class kind : std::uint8_t { point = 0, cone = 1, beam = 2, quad = 3 };
enum class reach : std::uint8_t { below = 0, self_only = 1 };

struct light {
    kind k = kind::point;
    float x = 0.0f;
    float y = 0.0f;
    float radius = 0.0f;    ///< 圆/锥的半径
    float width = 0.0f;     ///< 束的宽度
    std::uint32_t color = 0xFFFFFFFFu; ///< 0xAARRGGBB
    float intensity = 1.0f; ///< 有符号（负 = 黑光）
    std::int64_t layer = 0; ///< 必须落在某个 band 内
    reach r = reach::below;
    std::uint8_t depth = 0; ///< 与遮挡者比较
    float angle = 0.0f;     ///< 度，逆时针，0° = +x
    float spread = 0.0f;    ///< 锥角（度）
    float softness = 2.0f;  ///< 可选
    float falloff = 1.0f;   ///< 可选
};

} // namespace wag::light