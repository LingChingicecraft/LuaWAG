// =============================================================================
// container/hit.cpp
//
// 几何实现。精确度分级严格照 的表：
// · 点 vs 椭圆 / 点 vs 矩形 精确
// · 圆 vs 矩形 精确
// · 圆 vs 椭圆 保守（偏宽）
// · 椭圆 vs 椭圆 其一按最大半轴退化为圆（保守）
// · 椭圆 vs 矩形 椭圆退化为圆（保守）
// · 矩形 vs 矩形 SAT 四条分离轴（精确）
//
// 【保守的含义】宁可多判、不会漏判：真实相交的两种情况一定回报"相交"，
// 代价是极少数"其实没碰到"会被报成相交。擦弹/吃弹这类次要场景接受它；
// 玩家点 vs 子弹是精确的（上面的第一行）。
// =============================================================================

#include "container/hit.hpp"

#include <algorithm>
#include <cmath>

namespace wag::container {
namespace {

struct point {
    float x;
    float y;
};

[[nodiscard]] point center_of(const hit_entry& h, float x, float y,
                              float rot) noexcept {
    const float c = std::cos(rot);
    const float s = std::sin(rot);
 // ox/oy 是局部偏移 ⇒ 旋转后再加到位置上（贴图尾部旋转后仍在尾部）
    return {x + c * h.ox - s * h.oy, y + s * h.ox + c * h.oy};
}

/// 世界点 → 以 c 为中心、旋转 rot 的局部坐标（逆旋）
[[nodiscard]] point to_local(point p, point c, float rot) noexcept {
    const float dx = p.x - c.x;
    const float dy = p.y - c.y;
    const float co = std::cos(rot);
    const float si = std::sin(rot);
    return {dx * co + dy * si, -dx * si + dy * co};
}

[[nodiscard]] bool point_vs_ellipse(point p, point c, float rot, float a,
                                    float b) noexcept {
    const point l = to_local(p, c, rot);
 // 退化守卫：a/b 为 0 时椭圆退化成线段/点 —— 仍按"点是否落在上面"判定
    if (a <= 0.0f && b <= 0.0f) {
        return l.x == 0.0f && l.y == 0.0f;
    }
    if (a <= 0.0f) {
        return l.x == 0.0f && std::fabs(l.y) <= b;
    }
    if (b <= 0.0f) {
        return l.y == 0.0f && std::fabs(l.x) <= a;
    }
    const float u = l.x / a;
    const float v = l.y / b;
    return u * u + v * v <= 1.0f;
}

[[nodiscard]] bool point_vs_rect(point p, point c, float rot, float w,
                                 float h) noexcept {
    const point l = to_local(p, c, rot);
    return std::fabs(l.x) <= w * 0.5f && std::fabs(l.y) <= h * 0.5f;
}

[[nodiscard]] bool point_vs(hit_kind kind, point p, point c, float rot, float a,
                            float b) noexcept {
    switch (kind) {
        case hit_kind::point:
 // 两个点：同一个坐标才算碰到（点没有半径）
            return p.x == c.x && p.y == c.y;
        case hit_kind::ellipse:
            return point_vs_ellipse(p, c, rot, a, b);
        case hit_kind::rect:
            return point_vs_rect(p, c, rot, a, b);
    }
    return false;
}

/// 圆 vs 矩形（精确）：把圆心夹到盒内，测距离² ≤ r²
[[nodiscard]] bool circle_vs_rect(point cc, float r, point c, float rot, float w,
                                  float h) noexcept {
    const point l = to_local(cc, c, rot);
    const float hw = w * 0.5f;
    const float hh = h * 0.5f;
    const float qx = std::clamp(l.x, -hw, hw);
    const float qy = std::clamp(l.y, -hh, hh);
    const float dx = l.x - qx;
    const float dy = l.y - qy;
    return dx * dx + dy * dy <= r * r;
}

/// 圆 vs 椭圆（保守）：两半轴各加 r，再按"点是否在放大后的椭圆内"判定
[[nodiscard]] bool circle_vs_ellipse(point cc, float r, point c, float rot,
                                     float a, float b) noexcept {
    const point l = to_local(cc, c, rot);
    const float aa = a + r;
    const float bb = b + r;
    if (aa <= 0.0f || bb <= 0.0f) {
        return false;
    }
    const float u = l.x / aa;
    const float v = l.y / bb;
    return u * u + v * v <= 1.0f;
}

/// 矩形 vs 矩形（精确）：SAT，四条分离轴
[[nodiscard]] bool rect_vs_rect(point ca, float arot, float aw, float ah,
                                point cb, float brot, float bw,
                                float bh) noexcept {
    const float ac = std::cos(arot);
    const float as = std::sin(arot);
    const float bc = std::cos(brot);
    const float bs = std::sin(brot);

 // 四条轴：A 的两条边方向 + B 的两条边方向
    const float axes[4][2] = {{ac, as}, {-as, ac}, {bc, bs}, {-bs, bc}};
    const float dx = cb.x - ca.x;
    const float dy = cb.y - ca.y;

    for (const auto& axis : axes) {
        const float nx = axis[0];
        const float ny = axis[1];
 // 半宽在这条轴上的投影 = |u·n|·w/2 + |v·n|·h/2
        const float ra = std::fabs(ac * nx + as * ny) * aw * 0.5f +
                         std::fabs(-as * nx + ac * ny) * ah * 0.5f;
        const float rb = std::fabs(bc * nx + bs * ny) * bw * 0.5f +
                         std::fabs(-bs * nx + bc * ny) * bh * 0.5f;
        if (std::fabs(dx * nx + dy * ny) > ra + rb) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ellipse_vs_ellipse(const hit_entry& a, point ca, float arot,
                                      const hit_entry& b, point cb,
                                      float brot) noexcept {
 // 圆是椭圆的特例 ⇒ 两边都是圆时走精确路径
    const bool a_circle = a.a == a.b;
    const bool b_circle = b.a == b.b;
    if (a_circle) {
        return circle_vs_ellipse(ca, a.a, cb, brot, b.a, b.b);
    }
    if (b_circle) {
        return circle_vs_ellipse(cb, b.b, ca, arot, a.a, a.b);
    }
 // 其一按最大半轴退化为圆。退化"最大半轴较小"的那个：
 // 圆仍然包住原椭圆（保守），但直径更小 ⇒ 误报更少。
    const float ra = std::max(a.a, a.b);
    const float rb = std::max(b.a, b.b);
    if (ra <= rb) {
        return circle_vs_ellipse(ca, ra, cb, brot, b.a, b.b);
    }
    return circle_vs_ellipse(cb, rb, ca, arot, a.a, a.b);
}

[[nodiscard]] bool ellipse_vs_rect(const hit_entry& e, point ce, float erot,
                                   const hit_entry& r, point cr,
                                   float rrot) noexcept {
 // 圆是特例 ⇒ 精确；否则椭圆退化为最大半轴圆（保守）
    if (e.a == e.b) {
        return circle_vs_rect(ce, e.a, cr, rrot, r.a, r.b);
    }
 // 退化成圆后**没有朝向** ⇒ erot 不参与计算（保留参数是为了调用处对称）
    (void)erot;
    return circle_vs_rect(ce, std::max(e.a, e.b), cr, rrot, r.a, r.b);
}

} // namespace

float hit_center_x(const hit_entry& h, float x, float y, float rot) noexcept {
    return center_of(h, x, y, rot).x;
}

float hit_center_y(const hit_entry& h, float x, float y, float rot) noexcept {
    return center_of(h, x, y, rot).y;
}

hit_aabb hit_world_aabb(const hit_entry& h, float x, float y,
                        float rot) noexcept {
    const point c = center_of(h, x, y, rot);
    float hx = 0.0f;
    float hy = 0.0f;

    switch (h.kind) {
        case hit_kind::point:
            break; // 点没有半径
        case hit_kind::ellipse:
            if (h.a == h.b) {
 // 圆：AABB 与角度无关 ⇒ 跳过三角函数（的优化）
                hx = h.a;
                hy = h.b;
            } else {
                const float co = std::cos(rot);
                const float si = std::sin(rot);
                hx = std::sqrt(h.a * h.a * co * co + h.b * h.b * si * si);
                hy = std::sqrt(h.a * h.a * si * si + h.b * h.b * co * co);
            }
            break;
        case hit_kind::rect:
            if (h.a == h.b) {
 // 正方形：同上
                hx = h.a * 0.5f;
                hy = h.b * 0.5f;
            } else {
                const float co = std::cos(rot);
                const float si = std::sin(rot);
                hx = (std::fabs(h.a * co) + std::fabs(h.b * si)) * 0.5f;
                hy = (std::fabs(h.a * si) + std::fabs(h.b * co)) * 0.5f;
            }
            break;
    }

    return {c.x - hx, c.y - hy, c.x + hx, c.y + hy};
}

bool hit_overlap(const hit_entry& a, float ax, float ay, float arot,
                 const hit_entry& b, float bx, float by, float brot) noexcept {
    const point ca = center_of(a, ax, ay, arot);
    const point cb = center_of(b, bx, by, brot);

 // 点先处理：它没有角度、也没有半径
    if (a.kind == hit_kind::point) {
        return point_vs(b.kind, ca, cb, brot, b.a, b.b);
    }
    if (b.kind == hit_kind::point) {
        return point_vs(a.kind, cb, ca, arot, a.a, a.b);
    }

    if (a.kind == hit_kind::rect && b.kind == hit_kind::rect) {
        return rect_vs_rect(ca, arot, a.a, a.b, cb, brot, b.a, b.b);
    }
    if (a.kind == hit_kind::rect) { // rect vs ellipse
        return ellipse_vs_rect(b, cb, brot, a, ca, arot);
    }
    if (b.kind == hit_kind::rect) { // ellipse vs rect
        return ellipse_vs_rect(a, ca, arot, b, cb, brot);
    }
    return ellipse_vs_ellipse(a, ca, arot, b, cb, brot);
}

} // namespace wag::container
