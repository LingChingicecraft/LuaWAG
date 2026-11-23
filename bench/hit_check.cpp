// =============================================================================
//  bench/hit_check.cpp
//
//  碰撞几何的验证程序（R20 的精确度分级逐条对）。
//
//  【它验证什么】
//    ① 记录是 24 B（编译期已在 hit.hpp 里钉死，这里再读一次运行期 sizeof）
//    ② 点 vs 椭圆 / 点 vs 矩形：精确（含旋转与局部偏移）
//    ③ 圆 vs 矩形：精确
//    ④ 圆 vs 椭圆：保守（相切算相交，稍外算不相交）
//    ⑤ 椭圆 vs 椭圆 / 椭圆 vs 矩形：保守 —— 真实的相交**一定**报，
//       真实的分离**可能**被报成相交（宁可多判）
//    ⑥ 矩形 vs 矩形：SAT（轴对齐 + 旋转）
//    ⑦ AABB：圆/正方形与角度无关；矩形旋转 45° 的解析值
//
//  【怎么跑】先构建 release-with-deps，再：
//    g++ -O2 -std=c++20 -I src bench/hit_check.cpp -o bench/hit_check.exe \
//        build/release-with-deps/lib/libwag_container.a \
//        build/release-with-deps/lib/libwag_error.a \
//        build/release-with-deps/lib/libwag_log.a
// =============================================================================

#include "container/hit.hpp"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok) {
        ++failures;
    }
}

bool near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

using wag::container::hit_aabb;
using wag::container::hit_entry;
using wag::container::hit_kind;
using wag::container::hit_overlap;
using wag::container::hit_world_aabb;

hit_entry make_point(float ox = 0.0f, float oy = 0.0f) {
    return hit_entry{0, hit_kind::point, 0.0f, 0.0f, ox, oy};
}

hit_entry make_ellipse(float rx, float ry, float ox = 0.0f, float oy = 0.0f) {
    return hit_entry{0, hit_kind::ellipse, rx, ry, ox, oy};
}

hit_entry make_rect(float w, float h, float ox = 0.0f, float oy = 0.0f) {
    return hit_entry{0, hit_kind::rect, w, h, ox, oy};
}

constexpr float pi = 3.14159265358979323846f;
constexpr float half_pi = pi * 0.5f;
constexpr float quarter_pi = pi * 0.25f;

} // namespace

int main() {
    using namespace wag::container;

    // ---- ① 记录大小 ----
    check(sizeof(hit_entry) == 24, "判定记录是 24 B（R20）");

    // ---- ② 点 vs 椭圆 ----
    const hit_entry e = make_ellipse(8, 3);
    check(hit_overlap(make_point(), 105, 100, 0, e, 100, 100, 0),
          "点(105,100) 在 8×3 椭圆内");
    check(!hit_overlap(make_point(), 109, 100, 0, e, 100, 100, 0),
          "点(109,100) 在 8×3 椭圆外");
    check(hit_overlap(make_point(), 100, 102, 0, e, 100, 100, 0),
          "点(100,102) 在椭圆内");
    check(!hit_overlap(make_point(), 100, 104, 0, e, 100, 100, 0),
          "点(100,104) 在椭圆外");
    check(!hit_overlap(make_point(), 106, 102, 0, e, 100, 100, 0),
          "点(106,102) 刚出椭圆（0.75² + 0.667² > 1）");

    // 旋转 90°：rx 沿世界 y 轴
    check(hit_overlap(make_point(), 100, 107, 0, e, 100, 100, half_pi),
          "旋转 90° 后，点(100,107) 在椭圆内");
    check(!hit_overlap(make_point(), 107, 100, 0, e, 100, 100, half_pi),
          "旋转 90° 后，点(107,100) 在椭圆外（ry 只有 3）");

    // 局部偏移随 rot 旋转
    const hit_entry e_off = make_ellipse(8, 3, 10, 0);
    check(hit_overlap(make_point(), 10, 0, 0, e_off, 0, 0, 0),
          "偏移 (10,0) 在 rot=0 时把中心移到 (10,0)");
    check(hit_overlap(make_point(), 0, 10, 0, e_off, 0, 0, half_pi),
          "偏移 (10,0) 在 rot=90° 时把中心移到 (0,10)");

    // ---- 点 vs 矩形 ----
    const hit_entry r = make_rect(10, 4);
    check(hit_overlap(make_point(), 4, 1, 0, r, 0, 0, 0), "点(4,1) 在 10×4 矩形内");
    check(!hit_overlap(make_point(), 6, 1, 0, r, 0, 0, 0), "点(6,1) 在矩形外");
    check(hit_overlap(make_point(), 0, 2, 0, r, 0, 0, 0), "点(0,2) 在矩形边上");
    check(!hit_overlap(make_point(), 0, 2.1f, 0, r, 0, 0, 0), "点(0,2.1) 在矩形外");
    check(hit_overlap(make_point(), 1, 4, 0, r, 0, 0, half_pi),
          "旋转 90° 后，点(1,4) 在矩形内");
    check(!hit_overlap(make_point(), 5.1f, 0, 0, r, 0, 0, half_pi),
          "旋转 90° 后，点(5.1,0) 在矩形外");

    // ---- ③ 圆 vs 矩形（精确）----
    const hit_entry c2 = make_ellipse(2, 2);
    check(hit_overlap(c2, 6, 0, 0, r, 0, 0, 0), "半径 2 的圆在 (6,0) 碰到矩形");
    check(!hit_overlap(c2, 8, 0, 0, r, 0, 0, 0), "半径 2 的圆在 (8,0) 碰不到矩形");
    check(hit_overlap(c2, 6, 2, 0, r, 0, 0, 0), "圆在角上 (6,2) 碰到矩形");
    check(!hit_overlap(c2, 6.5f, 3.5f, 0, r, 0, 0, 0),
          "圆在 (6.5,3.5) 离角 1.5√2 > 2，碰不到");

    // ---- ④ 圆 vs 椭圆（保守）----
    const hit_entry big = make_ellipse(8, 3);
    check(hit_overlap(c2, 10, 0, 0, big, 0, 0, 0),
          "圆(10,0) 与椭圆相切 ⇒ 算相交");
    check(!hit_overlap(c2, 10.1f, 0, 0, big, 0, 0, 0),
          "圆(10.1,0) 稍外 ⇒ 不算相交");
    check(hit_overlap(c2, 0, 5, 0, big, 0, 0, 0),
          "圆(0,5) 与椭圆的短轴端相切 ⇒ 算相交");

    // ---- ⑤ 椭圆 vs 椭圆（保守）----
    const hit_entry ea = make_ellipse(8, 3);
    check(hit_overlap(ea, 0, 0, 0, ea, 12, 0, 0),
          "两个 8×3 椭圆相距 12（真实相交）⇒ 报相交");
    check(!hit_overlap(ea, 0, 0, 0, ea, 17, 0, 0),
          "相距 17（真实分离）⇒ 报分离");
    // 这一条是**保守的代价**：真实分离（3+3=6 < 9），退化成圆后会报相交
    check(hit_overlap(ea, 0, 0, 0, ea, 0, 9, 0),
          "相距 9（真实分离，但退化圆后报相交）⇒ 保守，宁可多判");
    check(hit_overlap(ea, 0, 0, 0, ea, 0, 5, 0),
          "相距 5（真实相交）⇒ 报相交");

    // ---- 椭圆 vs 矩形（保守）----
    const hit_entry r4 = make_rect(4, 4);
    check(hit_overlap(ea, 0, 0, 0, r4, 10, 0, 0),
          "椭圆(8×3) 与矩形(10,0) 相切 ⇒ 报相交（退化圆）");
    check(!hit_overlap(ea, 0, 0, 0, r4, 20, 0, 0),
          "矩形在 (20,0) ⇒ 报分离");

    // ---- ⑥ 矩形 vs 矩形（SAT）----
    const hit_entry r10x4 = make_rect(10, 4);
    check(hit_overlap(r10x4, 0, 0, 0, r10x4, 9, 0, 0),
          "两个 10×4 相距 9 ⇒ 相交");
    check(!hit_overlap(r10x4, 0, 0, 0, r10x4, 11, 0, 0),
          "相距 11 ⇒ 分离");
    // A 旋转 90° 后变成 4 宽 × 10 高（x ∈ [-2,2]，y ∈ [-5,5]）
    check(hit_overlap(r10x4, 0, 0, half_pi, r10x4, 3, 0, 0),
          "A 旋转 90° 后与 (3,0) 的矩形相交");
    // B 在 (5,0) 时 x ∈ [0,10]，与 A 的 [-2,2] 仍然重叠 ⇒ 相交（不是分离）
    check(hit_overlap(r10x4, 0, 0, half_pi, r10x4, 5, 0, 0),
          "A 旋转 90° 后与 (5,0) 的矩形仍然相交（x 重叠 0..2）");
    // 真正分离：B 在 (8,0) ⇒ x ∈ [3,13]，与 A 的 [-2,2] 不相交
    check(!hit_overlap(r10x4, 0, 0, half_pi, r10x4, 8, 0, 0),
          "A 旋转 90° 后与 (8,0) 的矩形分离");
    check(hit_overlap(r10x4, 0, 0, quarter_pi, r10x4, 0, 0, quarter_pi),
          "两个 45° 的矩形同心 ⇒ 相交");

    // ---- ⑦ AABB ----
    const hit_entry circle5 = make_ellipse(5, 5);
    const hit_aabb b0 = hit_world_aabb(circle5, 100, 100, 0);
    const hit_aabb b1 = hit_world_aabb(circle5, 100, 100, 1.234f);
    check(near(b0.min_x, 95) && near(b0.max_y, 105), "圆 AABB = 中心 ± 5");
    check(near(b1.min_x, 95) && near(b1.max_y, 105),
          "圆的 AABB 与角度无关（跳过三角函数）");

    const hit_aabb b2 = hit_world_aabb(make_rect(10, 4), 0, 0, 0);
    check(near(b2.min_x, -5) && near(b2.max_y, 2), "10×4 矩形 rot=0 的 AABB");
    const hit_aabb b3 = hit_world_aabb(make_rect(10, 4), 0, 0, half_pi);
    check(near(b3.min_x, -2) && near(b3.max_y, 5), "10×4 矩形 rot=90° 的 AABB");
    const hit_aabb b4 = hit_world_aabb(make_rect(10, 4), 0, 0, quarter_pi);
    check(near(b4.max_x, 4.9497f) && near(b4.max_y, 4.9497f),
          "10×4 矩形 rot=45° 的 AABB = ±4.9497");

    const hit_aabb b5 = hit_world_aabb(make_ellipse(8, 3, 10, 0), 0, 0, half_pi);
    check(near(b5.min_x, -3) && near(b5.max_y, 18),
          "偏移(10,0) + rot=90° ⇒ 中心(0,10)，AABB 宽 6 高 16");

    // ---- 点 vs 点 ----
    check(hit_overlap(make_point(), 3, 4, 0, make_point(), 3, 4, 0),
          "同一个坐标的两个点 ⇒ 相交");
    check(!hit_overlap(make_point(), 3, 4, 0, make_point(), 3.001f, 4, 0),
          "相差 0.001 的两个点 ⇒ 不相交（点没有半径）");

    std::printf("\n%s（失败 %d 项）\n", failures == 0 ? "全部通过" : "有失败",
                failures);
    return failures == 0 ? 0 : 1;
}
