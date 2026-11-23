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
//  设计分辨率（固定 16:9）
//
//  【这是设计常量，不是运行时自适应】
//    项目选定 1920×1080 作为世界/视口设计空间；灯光链路固定用它的半分辨率：
//      世界  1920 × 1080
//      光图   960 ×  540
//      遮蔽图 960 ×  540
//      带掩码 960 ×  540
//    ⇒ 光图与场景同比例，合成到 16:9 视口时不会被纵向拉伸。
//
//  【改设计分辨率时】这里、light.vs / light.fs 里的世界常量必须一起改；
//    它们是同一个固定设计的两半。
// -----------------------------------------------------------------------------
inline constexpr int design_width = 1920;
inline constexpr int design_height = 1080;
inline constexpr int map_width = design_width / 2;   // 960
inline constexpr int map_height = design_height / 2; // 540

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