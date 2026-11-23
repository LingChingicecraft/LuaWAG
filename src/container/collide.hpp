// =============================================================================
// container/collide.hpp
//
// 碰撞胶水：侧表→ 宽相（均匀网格）→ 窄相→ 报告。
//
// 【它不做的事】
// · 不决定"删不删 / 掉不掉血 / 算不算命中"（：引擎只报告）
// · 不持久化组对规则（：每帧由脚本声明检哪两组）
// · 不解释 group（：不透明标签，只做相等比较）
//
// 【为什么 rebuild 是"每帧重建"而不是增量】
// 这是第一版：先保证语义正确、可验证。增量维护（移动时改格子）是优化，
// 等有真实场景的量再决定 —— 那时改的是 broad.cpp，不是这里的语义。
// =============================================================================

#pragma once

#include "container/broad.hpp"
#include "container/hit.hpp"
#include "container/pool.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace wag::container {

class collision_world {
public:
    using report_fn = void (*)(void* user, handle a, handle b);

    collision_world(container_pool_set& pools, float cell_size);

 /// 帧初：把各池侧表里的条目投影成网格条目。
    void rebuild();

 /// 检 group A 与 group B；每对命中调用一次 fn（按句柄升序的报告顺序不保证，
 /// 但**同一对每帧最多一次**）。
    void check(std::uint8_t gA, std::uint8_t gB, report_fn fn, void* user);

 /// 一侧只有一个容器时的快路径（的 check_one）。
    void check_one(handle h, std::uint8_t gB, report_fn fn, void* user);

    [[nodiscard]] std::size_t item_count() const noexcept { return items_.size(); }
    [[nodiscard]] std::size_t candidate_count() const noexcept { return pairs_.size(); }

private:
    struct item_ref {
        handle h;
        hit_entry e;
    };

    container_pool_set& pools_;
    uniform_grid grid_;
    std::vector<item_ref> items_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs_;
};

} // namespace wag::container
