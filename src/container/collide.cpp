// =============================================================================
// container/collide.cpp
//
// 三段流水线的具体形状：
// ① rebuild：侧表 → items_ → 均匀网格
// ② check：网格候选对 → 组对过滤 → 窄相 → 回调
// ③ check_one：单容器 vs 一组RO(n)，不走网格）
//
// 【报告顺序】候选对按 (items 下标小, 大) 升序枚举；窄相通过的按枚举顺序回调。
// 这个顺序是稳定的R不依赖哈希），但**不承诺**按句柄大小排序。
// =============================================================================

#include "container/collide.hpp"

#include "error/error.hpp"

#include <string>

namespace wag::container {

collision_world::collision_world(container_pool_set& pools, float cell_size)
    : pools_(pools), grid_(cell_size) {}

void collision_world::rebuild() {
    items_.clear();
    grid_.clear();

    for (std::uint32_t p = 0; p < static_cast<std::uint32_t>(pools_.pool_count());
         ++p) {
        container_pool& pool = pools_.pool(p);
        const container_side_table& st = pool.side_table();

        for (std::size_t i = 0; i < st.size(); ++i) {
            const hit_entry& e = st.at(i);
            const container& c = pool.at(e.container);

 // 不变量：条目集合 ≡ { 存活 且 collides = true 的容器 }
 // 这里只查 collides；“存活”由池的回收路径保证，条目随销毁移除。
            if (c.collides == 0) {
                fail_internal("判定侧表：池「" + pool.name() + "」的容器 " +
                              std::to_string(e.container) +
                              " collides=false 却有条目（内部不变量被破坏）");
            }

            const handle h =
                make_handle(p, e.container, pool.generation_of(e.container));
            items_.push_back(item_ref{h, e});

            const hit_aabb box = hit_world_aabb(e, c.x, c.y, c.rot);
            grid_.insert(static_cast<std::uint32_t>(items_.size() - 1), box);
        }
    }
}

void collision_world::check(std::uint8_t gA, std::uint8_t gB, report_fn fn,
                            void* user) {
    grid_.candidate_pairs(pairs_);

    for (const auto& pr : pairs_) {
        const item_ref& A = items_[pr.first];
        const item_ref& B = items_[pr.second];

        container* ca = pools_.get(A.h);
        container* cb = pools_.get(B.h);

        const bool direct = (ca->group == gA && cb->group == gB);
        const bool swapped = (ca->group == gB && cb->group == gA);
        if (!direct && !swapped) {
            continue;
        }

        if (!hit_overlap(A.e, ca->x, ca->y, ca->rot, B.e, cb->x, cb->y,
                         cb->rot)) {
            continue;
        }

 // 报告顺序跟着"脚本声明的组对"走：direct 就 (A,B)，swapped 就 (B,A)
        if (direct) {
            fn(user, A.h, B.h);
        } else {
            fn(user, B.h, A.h);
        }
    }
}

void collision_world::check_one(handle h, std::uint8_t gB, report_fn fn,
                                void* user) {
    container* c = pools_.get(h);

    const std::uint32_t pool_id = handle_pool(h);
    const std::uint32_t index = handle_index(h);
    const hit_entry* e = pools_.pool(pool_id).side_table().find(index);
    if (e == nullptr) {
        fail_internal("check_one：句柄 " + std::to_string(h) +
                      " 没有判定条目Rcollides=false 或侧表未同步）");
    }

    for (const item_ref& item : items_) {
        if (item.h == h) {
            continue;
        }
        container* cb = pools_.get(item.h);
        if (cb->group != gB) {
            continue;
        }
        if (!hit_overlap(*e, c->x, c->y, c->rot, item.e, cb->x, cb->y,
                         cb->rot)) {
            continue;
        }
        fn(user, h, item.h);
    }
}

} // namespace wag::container
