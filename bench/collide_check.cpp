// =============================================================================
//  bench/collide_check.cpp
//
//  碰撞流水线的验证程序（侧表 R18 + 宽相 R7 + 窄相 R20 + 报告 R15）。
//
//  【验证什么】
//    ① 组对过滤：只报 (gA,gB) 的对；同组、无关组不报
//    ② 宽相剪枝：远处的对象不产生候选对
//    ③ 跨格去重：两个 AABB 共享两个格子时，这一对只报一次
//    ④ swap-remove：删一条之后其余条目仍正确
//    ⑤ check_one：单容器 vs 一组的快路径
//    ⑥ 偏移 + 旋转：判定中心随 rot 旋转
//
//  【怎么跑】先构建 release-with-deps，再：
//    g++ -O2 -std=c++20 -I src bench/collide_check.cpp -o bench/collide_check.exe \
//        build/release-with-deps/lib/libwag_container.a \
//        build/release-with-deps/lib/libwag_error.a \
//        build/release-with-deps/lib/libwag_log.a
// =============================================================================

#include "container/collide.hpp"

#include <cstdio>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok) {
        ++failures;
    }
}

using wag::container::collision_world;
using wag::container::container_pool_set;
using wag::container::handle;
using wag::container::hit_entry;
using wag::container::hit_kind;

struct collector {
    std::vector<std::pair<handle, handle>> pairs;
};

void collect(void* user, handle a, handle b) {
    auto* c = static_cast<collector*>(user);
    if (a > b) {
        std::swap(a, b);
    }
    c->pairs.emplace_back(a, b);
}

bool has_pair(const collector& c, handle a, handle b) {
    if (a > b) {
        std::swap(a, b);
    }
    for (const auto& p : c.pairs) {
        if (p.first == a && p.second == b) {
            return true;
        }
    }
    return false;
}

hit_entry point() {
    return hit_entry{0, hit_kind::point, 0.0f, 0.0f, 0.0f, 0.0f};
}

hit_entry ellipse(float rx, float ry, float ox = 0.0f, float oy = 0.0f) {
    return hit_entry{0, hit_kind::ellipse, rx, ry, ox, oy};
}

/// 一个"池 + 世界"的测试场景；每个场景独立，避免计数互相污染。
struct scene {
    container_pool_set pools;
    collision_world world;

    scene() : world(pools, 64.0f) { (void)pools.create_pool("test"); }

    handle spawn(float x, float y, float rot, std::uint8_t group,
                 const hit_entry& e) {
        handle h;
        wag::container::container* c = pools.allocate(0, h);
        c->x = x;
        c->y = y;
        c->rot = rot;
        c->group = group;
        c->collides = 1;
        pools.pool(0).side_table().add(wag::container::handle_index(h), e);
        return h;
    }
};

} // namespace

int main() {
    using namespace wag::container;

    // =========================================================================
    //  ① 组对过滤 + ② 宽相剪枝 + ⑤ check_one
    // =========================================================================
    {
        scene s;
        const handle a = s.spawn(100, 100, 0, 1, ellipse(8, 3));
        const handle b = s.spawn(105, 100, 0, 2, point());
        const handle c = s.spawn(500, 500, 0, 2, point());
        const handle d = s.spawn(100, 100, 0, 1, point());
        const handle e = s.spawn(127, 100, 0, 1, ellipse(8, 3));
        const handle f = s.spawn(129, 100, 0, 2, point());

        s.world.rebuild();
        check(s.world.item_count() == 6, "rebuild 收进 6 个条目");

        collector col;
        s.world.check(1, 2, collect, &col);
        check(has_pair(col, a, b), "组 1 vs 2：报出 A-B");
        check(has_pair(col, e, f), "组 1 vs 2：报出跨格边界的 E-F");
        check(!has_pair(col, a, c) && !has_pair(col, a, d),
              "组 1 vs 2：不报远处的 C、不报同组的 D");
        check(!has_pair(col, a, e), "组 1 vs 2：不报两个互不重叠的椭圆");

        collector same;
        s.world.check(1, 1, collect, &same);
        check(has_pair(same, a, d), "组 1 vs 1：报出重叠的 A-D");
        check(!has_pair(same, a, b), "组 1 vs 1：不报跨组的 A-B");

        collector one;
        s.world.check_one(a, 2, collect, &one);
        check(has_pair(one, a, b), "check_one(A, 组2)：报出 A-B");
        check(one.pairs.size() == 1, "check_one(A, 组2)：只报一对（C 太远）");

        // 宽相剪枝：6 个对象的全部对数 = 15；候选对应远小于它
        check(s.world.candidate_count() < 15, "宽相剪枝：候选对少于全对数");
        check(s.world.candidate_count() >= 3, "宽相剪枝：候选对仍包含近处的对");
    }

    // =========================================================================
    //  ③ 跨格去重
    // =========================================================================
    {
        scene s;
        // 两个 rx=40 的椭圆相距 1 ⇒ AABB 都是 [260,340]×[260,340]，
        // 横跨 x=256..320 与 320..384 两个格子 ⇒ 不做全局去重就会报两次
        const handle g = s.spawn(300, 300, 0, 3, ellipse(40, 40));
        const handle h = s.spawn(301, 300, 0, 4, ellipse(40, 40));

        s.world.rebuild();
        collector col;
        s.world.check(3, 4, collect, &col);

        std::size_t n = 0;
        for (const auto& p : col.pairs) {
            if ((p.first == g && p.second == h) ||
                (p.first == h && p.second == g)) {
                ++n;
            }
        }
        check(n == 1, "共享两个格子的同一对只报一次");
        check(s.world.candidate_count() == 1, "候选对也只有一对");
    }

    // =========================================================================
    //  ④ swap-remove
    // =========================================================================
    {
        scene s;
        const handle a = s.spawn(0, 0, 0, 1, ellipse(10, 10));
        const handle b = s.spawn(5, 0, 0, 2, point());
        const handle c = s.spawn(5, 5, 0, 2, point());

        s.world.rebuild();
        collector before;
        s.world.check(1, 2, collect, &before);
        check(before.pairs.size() == 2, "删除前：A 同时碰到 B 与 C");

        // 删中间那条（B）⇒ 最后一条（C）会被 swap 到它的位置
        s.pools.pool(0).side_table().remove(handle_index(b));
        s.world.rebuild();
        collector after;
        s.world.check(1, 2, collect, &after);
        check(after.pairs.size() == 1, "删除 B 后只剩一对");
        check(has_pair(after, a, c), "swap-remove 之后 A-C 仍然正确");
        check(!has_pair(after, a, b), "B 不再被报告");
    }

    // =========================================================================
    //  ⑥ 偏移 + 旋转
    // =========================================================================
    {
        scene s;
        // 椭圆中心在局部 (10,0)；rot=90° ⇒ 世界中心 (0,10)
        const handle a = s.spawn(0, 0, 1.5707963f, 5, ellipse(8, 3, 10, 0));
        const handle hit = s.spawn(0, 10, 0, 6, point());
        const handle miss = s.spawn(10, 0, 0, 6, point());

        s.world.rebuild();
        collector col;
        s.world.check(5, 6, collect, &col);
        check(has_pair(col, a, hit), "偏移随 rot 旋转：点(0,10) 命中");
        check(!has_pair(col, a, miss), "偏移随 rot 旋转：点(10,0) 不命中");
    }

    std::printf("\n%s（失败 %d 项）\n", failures == 0 ? "全部通过" : "有失败",
                failures);
    return failures == 0 ? 0 : 1;
}
