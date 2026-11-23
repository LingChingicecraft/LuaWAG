// =============================================================================
//  container/broad.cpp
//
//  均匀网格的实现。三段：插入（AABB → 格子）、排序（按格子聚组）、
//  枚举（同格内两两）+ 全局去重。
// =============================================================================

#include "container/broad.hpp"

#include "error/error.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace wag::container {
namespace {

/// 单个 AABB 覆盖的格子上限。
/// 【为什么要有】格子尺寸给得过小时，一个 AABB 会覆盖成百上千格，
///   items_ 会爆炸；这是"格子尺寸配错了"，属于引擎内部配置错误，直接 L3。
constexpr std::int64_t max_cells_per_item = 4096;

} // namespace

uniform_grid::uniform_grid(float cell_size) : cell_(cell_size) {
    if (!(cell_ > 0.0f)) {
        fail_internal("均匀网格：格子尺寸必须是正数，got " +
                      std::to_string(cell_size));
    }
}

void uniform_grid::clear() noexcept {
    items_.clear();
}

void uniform_grid::insert(std::uint32_t id, const hit_aabb& box) {
    const std::int32_t c0x = static_cast<std::int32_t>(std::floor(box.min_x / cell_));
    const std::int32_t c0y = static_cast<std::int32_t>(std::floor(box.min_y / cell_));
    const std::int32_t c1x = static_cast<std::int32_t>(std::floor(box.max_x / cell_));
    const std::int32_t c1y = static_cast<std::int32_t>(std::floor(box.max_y / cell_));

    const std::int64_t nx = static_cast<std::int64_t>(c1x) - c0x + 1;
    const std::int64_t ny = static_cast<std::int64_t>(c1y) - c0y + 1;
    if (nx * ny > max_cells_per_item) {
        fail_internal("均匀网格：一个 AABB 覆盖了 " + std::to_string(nx * ny) +
                      " 个格子（上限 4096）—— 格子尺寸 " +
                      std::to_string(cell_) + " 配得过小");
    }

    for (std::int32_t cy = c0y; cy <= c1y; ++cy) {
        for (std::int32_t cx = c0x; cx <= c1x; ++cx) {
            items_.push_back(item{cx, cy, id});
        }
    }
}

void uniform_grid::candidate_pairs(
    std::vector<std::pair<std::uint32_t, std::uint32_t>>& out) const {
    out.clear();

    std::vector<item> sorted = items_;
    std::sort(sorted.begin(), sorted.end(), [](const item& a, const item& b) {
        if (a.cx != b.cx) {
            return a.cx < b.cx;
        }
        if (a.cy != b.cy) {
            return a.cy < b.cy;
        }
        return a.id < b.id;
    });

    for (std::size_t i = 0; i < sorted.size();) {
        std::size_t j = i + 1;
        while (j < sorted.size() && sorted[j].cx == sorted[i].cx &&
               sorted[j].cy == sorted[i].cy) {
            ++j;
        }
        // 同一个格子里两两成对
        for (std::size_t a = i; a < j; ++a) {
            for (std::size_t b = a + 1; b < j; ++b) {
                std::uint32_t x = sorted[a].id;
                std::uint32_t y = sorted[b].id;
                if (x > y) {
                    std::swap(x, y);
                }
                if (x != y) {
                    out.emplace_back(x, y);
                }
            }
        }
        i = j;
    }

    // 跨格去重：同一对可能共享多个格子
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

} // namespace wag::container
