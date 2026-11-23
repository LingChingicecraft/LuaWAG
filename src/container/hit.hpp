// =============================================================================
//  container/hit.hpp
//
//  碰撞模型：判定形状的记录（24 B）与几何计算。
//
//  【这个模块负责什么】
//    hit_entry 是“这个容器怎么判”的最小数据；它不拥有容器，也不存句柄。
//    真正的容器按池存放，每个池有一张侧表，侧表里的槽指向池内下标。
//
//  【字段含义】
//    kind   : point / ellipse / rect（只有三种，没有 none）
//    a, b   : point ⇒ 0,0；ellipse ⇒ rx,ry；rect ⇒ w,h
//    ox, oy : 局部坐标偏移（随容器 rot 一起旋转）
//    container : 容器在所属池里的下标
//
//  【为什么记录里不存角度】
//    角度用容器的 `rot`。判定是容器的投影，投影只补判定自己的数，
//    不复制容器已有的数。
//
//  【为什么 container 是 u32 而不是 64 位句柄】
//    记录要尽量小：6 个 4 字节字段正好 24 B。池身份由“每池一张侧表”
//    提供，记录里只需要池内下标。改成 64 位句柄会变成 32 B，并让侧表
//    多存一份池身份，没有必要。
// =============================================================================

#pragma once

#include "container/container.hpp"

#include <cstddef>
#include <cstdint>

namespace wag::container {

// -----------------------------------------------------------------------------
//  判定形状枚举（只有三种）
// -----------------------------------------------------------------------------
enum class hit_kind : std::uint32_t {
    point = 0,   ///< 点：a = b = 0
    ellipse = 1, ///< 椭圆：a = rx，b = ry（圆即 a == b）
    rect = 2,    ///< 矩形：a = w，b = h（正方形即 a == b）
};

// -----------------------------------------------------------------------------
//  判定记录（24 B）
// -----------------------------------------------------------------------------
struct hit_entry {
    std::uint32_t container; ///< 池内下标
    hit_kind kind;           ///< 形状
    float a;                 ///< point: 0；ellipse: rx；rect: w
    float b;                 ///< point: 0；ellipse: ry；rect: h
    float ox;                ///< 局部偏移 x（随 rot 旋转）
    float oy;                ///< 局部偏移 y（随 rot 旋转）
};

static_assert(sizeof(hit_entry) == 24, "判定记录必须是 24 B");
static_assert(offsetof(hit_entry, container) == 0);
static_assert(offsetof(hit_entry, kind) == 4);
static_assert(offsetof(hit_entry, a) == 8);
static_assert(offsetof(hit_entry, b) == 12);
static_assert(offsetof(hit_entry, ox) == 16);
static_assert(offsetof(hit_entry, oy) == 20);

// -----------------------------------------------------------------------------
//  轴对齐包围盒（宽相用；旋转后 AABB）
// -----------------------------------------------------------------------------
struct hit_aabb {
    float min_x;
    float min_y;
    float max_x;
    float max_y;
};

/// 判定在世界里的中心。
/// 【为什么偏移要单独旋一次】ox/oy 是**局部**偏移：贴图尾部旋转后仍要落在尾部。
[[nodiscard]] float hit_center_x(const hit_entry& h, float x, float y,
                                 float rot) noexcept;
[[nodiscard]] float hit_center_y(const hit_entry& h, float x, float y,
                                 float rot) noexcept;

/// 旋转后的 AABB；圆/正方形跳过三角函数。
[[nodiscard]] hit_aabb hit_world_aabb(const hit_entry& h, float x, float y,
                                      float rot) noexcept;

/// 窄相：两个判定是否相交。
/// 精确度分级：
///   点 vs 椭圆/矩形 → 精确
///   圆 vs 矩形      → 精确
///   圆 vs 椭圆      → 保守（偏宽）
///   椭圆 vs 椭圆    → 其一按最大半轴退化为圆，再走"圆 vs 椭圆"（保守）
///   椭圆 vs 矩形    → 同上（保守）
///   矩形 vs 矩形    → SAT 四条分离轴（精确）
[[nodiscard]] bool hit_overlap(const hit_entry& a, float ax, float ay, float arot,
                               const hit_entry& b, float bx, float by,
                               float brot) noexcept;

} // namespace wag::container
