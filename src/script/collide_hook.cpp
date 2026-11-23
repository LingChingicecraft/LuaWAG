// =============================================================================
// script/collide_hook.cpp
//
// // 的执行点实现。
//
// 【顺序】rebuild 一次 → 每个请求做宽相+窄相 → 每个请求一次 Lua 回调。
//："每帧每组对最多一次回调"由 primitives 的"重复声明 ⇒ L2"保证。
// 【报告形态】扁平数组 [a1,b1,a2,b2,…]；库层按需两两消费。
// 【计时】整个 run_collisions 的墙钟（含 Lua 回调）写进 engine 的
// collision_stats；`wag.collide.stats()` 读它。只读统计，不参与碰撞语义。
// =============================================================================

#include "script/collide_hook.hpp"

#include "container/collide.hpp"
#include "engine/engine.hpp"
#include "error/error.hpp"
#include "script/state.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace wag::script {

namespace {

/// collision_world 的 report_fn：把 (a,b) 追加进扁平数组。
void collect(void* user, container::handle a, container::handle b) {
    auto* v = static_cast<std::vector<container::handle>*>(user);
    v->push_back(a);
    v->push_back(b);
}

} // namespace

void run_collisions(engine& eng) {
    auto& st = eng.collision_stats_ref();
    st = collision_stats{};

    auto& reqs = eng.collision_requests();
    if (reqs.empty()) {
        return;
    }

    const auto t0 = std::chrono::steady_clock::now();

    auto& world = eng.collision_world_ref();
    world.rebuild(); // 每帧重建（第一版，已记录代价）
    st.items = world.item_count();
    st.requests = reqs.size();

    auto& sc = eng.script();
    for (auto& r : reqs) {
        std::vector<container::handle> hits;
        if (r.k == collision_request::kind::pair) {
            world.check(r.group_a, r.group_b, &collect, &hits);
            st.candidates += world.candidate_count();
        } else {
 // check_one 不走网格：它扫描全部条目 ⇒ 候选数 = items 数
            world.check_one(r.handle, r.group_b, &collect, &hits);
            st.candidates += world.item_count();
        }

        std::string err;
        const bool ok =
            sc.call_collision(r.callback_ref, hits.data(), hits.size(), &err);
        sc.unref(r.callback_ref);
        r.callback_ref = -1;
        if (!ok) {
            wag::fail_script("collide 回调报错：" + err);
        }

        st.hits += hits.size() / 2;
    }
    reqs.clear();

    const auto t1 = std::chrono::steady_clock::now();
    st.last_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
}

void drop_collisions(engine& eng) {
    auto& reqs = eng.collision_requests();
    if (reqs.empty()) {
        return;
    }
    auto& sc = eng.script();
    for (auto& r : reqs) {
        sc.unref(r.callback_ref);
        r.callback_ref = -1;
    }
    reqs.clear();
}

} // namespace wag::script
