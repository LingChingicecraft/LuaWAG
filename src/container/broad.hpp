// =============================================================================
//  container/broad.hpp
//
//  **碰撞优化算法**：均匀网格（均匀网格足够，不需要 BVH）。
//
//  【它只做一件事】把一批 AABB 变成"可能相交的 id 对"。
//    它不知道 group、不知道形状、不知道命中处置 —— 那些在 collide.cpp。
//
//  【为什么单开一个文件】
//    这是唯一会随"对象数量/分布"改变复杂度的部件；
//    把它与"记录存储 / 窄相几何 / 组对规则"分开，才能单独量它的成本。
//
//  【为什么要去重】
//    一个 AABB 会覆盖多个格子；两个 AABB 可能共享不止一个格子，
//    于是同一对会在多个格子里被枚举到 ⇒ 全局排序 + unique。
//
//  【格子尺寸】
//    取"典型命中框尺寸的 2 倍"左右。太小 ⇒ 对象跨很多格、候选对被重复枚举；
//    太大 ⇒ 单格内 pair 数平方增长。本类不管怎么选，只按给定值工作。
// =============================================================================

#pragma once

#include "container/hit.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace wag::container {

class uniform_grid {
public:
    explicit uniform_grid(float cell_size);

    void clear() noexcept;

    /// 把一个 AABB 塞进它覆盖的所有格子。
    /// @param id 调用方给的编号（通常是 items_ 下标）
    void insert(std::uint32_t id, const hit_aabb& box);

    /// 枚举候选对：每对 (min,max) 只出现一次，按 (小,大) 升序。
    void candidate_pairs(
        std::vector<std::pair<std::uint32_t, std::uint32_t>>& out) const;

    [[nodiscard]] std::size_t item_count() const noexcept { return items_.size(); }
    [[nodiscard]] float cell_size() const noexcept { return cell_; }

private:
    struct item {
        std::int32_t cx;
        std::int32_t cy;
        std::uint32_t id;
    };

    float cell_;
    std::vector<item> items_;
};

} // namespace wag::container
