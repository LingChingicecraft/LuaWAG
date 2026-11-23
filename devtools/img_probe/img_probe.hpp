// =============================================================================
//  img_probe.hpp —— 像素采样断言（通用层：纯数据 + 判据，零依赖）
//
//  【一句话】不需要参考图的图形断言：把"二维图"降成"0 维判定"。
//
//  【为什么不是 golden image（参考图比对）】
//      参考图对"抗锯齿 / 驱动舍入 / 字体栅格化"极其敏感 ⇒ 假失败天下第一。
//      而人眼在乎的其实只有四类语义：
//          ① 这块应该是纯色 X        → 主色 + 覆盖率
//          ② 这块应该还没被画        → 唯一色数 == 1
//          ③ 这两块应该不同          → 关系断言（y 翻转 / 裁剪 / 镜像）
//          ④ 这块应该是渐变的        → 唯一色数 > N
//      本文件只提供这四类，不做逐像素相等。
//
//  【采集与判据分离】（这是本工具唯一的架构纪律）
//      采集 = 贵、平台相关、有副作用（读后缓冲、必须发生在 present 之前）
//            ⇒ 在 driver 里（sdl_gl_grab.*），本文件不碰。
//      判据 = 便宜、纯数据、可脱机单测
//            ⇒ 在本文件，header-only，任何 RGBA 缓冲都能喂进来。
//
//  【坐标】纯像素坐标，左上原点（与窗口坐标同域）。
//      ⇒ 通用层不认识 DIP / 不认识 space::transform；换算在哪一层做由调用方决定。
//
//  【颜色】API 一律 ARGB（0xAARRGGBB），与引擎 vertex::color 同形。
//      内部字节序是 GL 的 RGBA ⇒ 转换只在本文件发生（一处，可测）。
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace imgprobe {

// -----------------------------------------------------------------------------
//  矩形与颜色
// -----------------------------------------------------------------------------
struct rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return w > 0 && h > 0; }
    [[nodiscard]] constexpr int area() const noexcept { return valid() ? w * h : 0; }
    [[nodiscard]] constexpr int right() const noexcept { return x + w; }
    [[nodiscard]] constexpr int bottom() const noexcept { return y + h; }
};

[[nodiscard]] constexpr rect make_rect(int x, int y, int w, int h) noexcept {
    return rect{x, y, w, h};
}

/// 交集；无效（不相交）时返回 w/h == 0 的矩形。
[[nodiscard]] constexpr rect intersect(rect a, rect b) noexcept {
    const int x0 = a.x > b.x ? a.x : b.x;
    const int y0 = a.y > b.y ? a.y : b.y;
    const int x1 = a.right() < b.right() ? a.right() : b.right();
    const int y1 = a.bottom() < b.bottom() ? a.bottom() : b.bottom();
    if (x1 <= x0 || y1 <= y0) {
        return rect{};
    }
    return rect{x0, y0, x1 - x0, y1 - y0};
}

/// ARGB 打包（与引擎 vertex::color 同形）。
[[nodiscard]] constexpr std::uint32_t argb(std::uint8_t a, std::uint8_t r, std::uint8_t g,
                                           std::uint8_t b) noexcept {
    return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(r) << 16) |
           (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
}

[[nodiscard]] constexpr std::uint8_t alpha_of(std::uint32_t c) noexcept {
    return static_cast<std::uint8_t>((c >> 24) & 0xFFu);
}
[[nodiscard]] constexpr std::uint8_t red_of(std::uint32_t c) noexcept {
    return static_cast<std::uint8_t>((c >> 16) & 0xFFu);
}
[[nodiscard]] constexpr std::uint8_t green_of(std::uint32_t c) noexcept {
    return static_cast<std::uint8_t>((c >> 8) & 0xFFu);
}
[[nodiscard]] constexpr std::uint8_t blue_of(std::uint32_t c) noexcept {
    return static_cast<std::uint8_t>(c & 0xFFu);
}

// -----------------------------------------------------------------------------
//  图像（纯数据，左上原点，RGBA 字节序）
// -----------------------------------------------------------------------------
struct image {
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> rgba;

    [[nodiscard]] bool valid() const noexcept {
        return w > 0 && h > 0 && rgba.size() == static_cast<std::size_t>(w) *
                                                  static_cast<std::size_t>(h) * 4u;
    }
    [[nodiscard]] rect bounds() const noexcept { return rect{0, 0, w, h}; }

    [[nodiscard]] const std::uint8_t* at(int x, int y) const noexcept {
        return rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                              static_cast<std::size_t>(x)) * 4u;
    }
    [[nodiscard]] std::uint8_t* at(int x, int y) noexcept {
        return rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                              static_cast<std::size_t>(x)) * 4u;
    }

    static image make(int width, int height, std::uint32_t fill_argb = 0xFF000000u) {
        image img;
        img.w = width;
        img.h = height;
        if (width <= 0 || height <= 0) {
            return img;
        }
        img.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0);
        img.fill(fill_argb);
        return img;
    }

    void put(int x, int y, std::uint32_t c) noexcept {
        if (x < 0 || y < 0 || x >= w || y >= h) {
            return;
        }
        std::uint8_t* p = at(x, y);
        p[0] = red_of(c);
        p[1] = green_of(c);
        p[2] = blue_of(c);
        p[3] = alpha_of(c);
    }

    void fill(std::uint32_t c) noexcept {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                put(x, y, c);
            }
        }
    }

    void fill_rect(rect r, std::uint32_t c) noexcept {
        const rect b = intersect(r, bounds());
        for (int y = b.y; y < b.bottom(); ++y) {
            for (int x = b.x; x < b.right(); ++x) {
                put(x, y, c);
            }
        }
    }

    [[nodiscard]] std::uint32_t get(int x, int y) const noexcept {
        if (x < 0 || y < 0 || x >= w || y >= h) {
            return 0u;
        }
        const std::uint8_t* p = at(x, y);
        return argb(p[3], p[0], p[1], p[2]);
    }
};

// -----------------------------------------------------------------------------
//  区域统计（把二维降成三个数）
// -----------------------------------------------------------------------------
struct stats {
    std::uint64_t px = 0;          ///< 实际统计的像素数（裁剪后）
    std::uint32_t dominant = 0;    ///< 出现最多的精确颜色（ARGB）
    std::uint32_t dominant_count = 0; ///< dominant 出现的次数
    double coverage = 0.0;         ///< dominant 占 px 的比例 [0,1]
    std::uint32_t unique = 0;      ///< 不同颜色数（上限 unique_cap）
    bool unique_capped = false;    ///< 达到上限 ⇒ unique 是"至少这么多"
    double mean_r = 0.0;           ///< 各通道均值（用于"两块不同"的关系断言）
    double mean_g = 0.0;
    double mean_b = 0.0;
    double mean_a = 0.0;

    [[nodiscard]] bool empty() const noexcept { return px == 0; }
    /// "不是主色"的像素数。
    [[nodiscard]] std::uint64_t dirty() const noexcept { return px - dominant_count; }
};

/// 三档判据。默认 strict；跨驱动场景建议 loose。
/// 【为什么是档位而不是裸阈值】阈值是"只有跨驱动踩过坑才知道怎么设"的数，
///   交给初学者等于埋雷 ⇒ 默认给档位，需要时再用显式阈值的重载（见 expect_color_abs）。
///
/// 【loose 为什么按周长而不是按面积】
///   抗锯齿 / 混合的脏像素沿**边缘**分布 ⇒ 代价随周长增长，不随面积增长。
///   实测反例：16×16 的区域内脏一列 = 6.25% 面积，若用固定覆盖率(0.98)就会误判失败；
///   而它其实只脏了一条边。⇒ loose 给"周长/4"的脏像素预算。
enum class tier {
    strict,  ///< 脏 0 个像素且唯一色 <= 1   （清屏 / scissor 类断言）
    loose,   ///< 脏像素 <= max(2, 周长/4) 且唯一色 <= 8，覆盖率兜底 0.90
    any      ///< 覆盖率 >= 0.50              （只要求"至少一半是它"）
};

struct policy {
    double min_coverage = 1.0;
    std::uint32_t max_unique = 1;
    bool perimeter_budget = false;
};

[[nodiscard]] constexpr policy policy_of(tier t) noexcept {
    switch (t) {
    case tier::strict:
        return policy{1.0, 1, false};
    case tier::loose:
        return policy{0.90, 8, true};
    case tier::any:
        return policy{0.50, 0xFFFFFFFFu, false};
    }
    return policy{};
}

/// 该档允许的"非主色"像素数（给定区域；只对 perimeter_budget 的档有意义）。
[[nodiscard]] constexpr std::uint32_t dirty_allowance(const policy& p, rect r) noexcept {
    if (!p.perimeter_budget) {
        return 0;
    }
    const std::uint32_t perim =
        static_cast<std::uint32_t>(2 * (r.w > 0 ? r.w : 0) + 2 * (r.h > 0 ? r.h : 0));
    const std::uint32_t by_perim = perim / 4u;
    return by_perim > 2u ? by_perim : 2u;
}

[[nodiscard]] inline stats region(const image& img, rect r, std::uint32_t unique_cap = 256) {
    stats s{};
    const rect b = intersect(r, img.bounds());
    if (!img.valid() || !b.valid()) {
        return s;
    }

    std::unordered_map<std::uint32_t, std::uint32_t> counts;
    counts.reserve(std::min<std::size_t>(static_cast<std::size_t>(b.area()), 4096u));

    double sr = 0.0, sg = 0.0, sb = 0.0, sa = 0.0;
    std::uint32_t best_count = 0;
    std::uint32_t best_color = 0;

    for (int y = b.y; y < b.bottom(); ++y) {
        for (int x = b.x; x < b.right(); ++x) {
            const std::uint8_t* p = img.at(x, y);
            const std::uint32_t c = argb(p[3], p[0], p[1], p[2]);

            sr += p[0];
            sg += p[1];
            sb += p[2];
            sa += p[3];
            ++s.px;

            auto it = counts.find(c);
            if (it == counts.end()) {
                if (counts.size() < unique_cap) {
                    it = counts.emplace(c, 0).first;
                    ++s.unique;
                } else {
                    s.unique_capped = true;
                }
            }
            if (it != counts.end()) {
                const std::uint32_t n = ++it->second;
                if (n > best_count) {
                    best_count = n;
                    best_color = c;
                }
            }
        }
    }

    s.dominant = best_color;
    s.dominant_count = best_count;
    s.coverage = s.px > 0 ? static_cast<double>(best_count) / static_cast<double>(s.px) : 0.0;
    const double inv = s.px > 0 ? 1.0 / static_cast<double>(s.px) : 0.0;
    s.mean_r = sr * inv;
    s.mean_g = sg * inv;
    s.mean_b = sb * inv;
    s.mean_a = sa * inv;
    return s;
}

// -----------------------------------------------------------------------------
//  断言结果
// -----------------------------------------------------------------------------
struct check {
    bool ok = false;
    std::string where;  ///< 断言的自我描述（region(x,y,w,h)）
    std::string want;   ///< 期望
    std::string got;    ///< 实测（失败时这是唯一能定位的东西）

    [[nodiscard]] std::string text() const {
        char buf[640];
        std::snprintf(buf, sizeof(buf), "%s  %s | want %s | got %s", ok ? "PASS" : "FAIL",
                      where.c_str(), want.c_str(), got.c_str());
        return std::string(buf);
    }
};

[[nodiscard]] inline std::string fmt_color(std::uint32_t c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%08X", static_cast<unsigned>(c));
    return std::string(buf);
}

[[nodiscard]] inline std::string fmt_stats(const stats& s) {
    char buf[224];
    std::snprintf(buf, sizeof(buf), "dominant=%s coverage=%.4f unique=%u%s dirty=%llu",
                  fmt_color(s.dominant).c_str(), s.coverage, static_cast<unsigned>(s.unique),
                  s.unique_capped ? "+" : "",
                  static_cast<unsigned long long>(s.dirty()));
    return std::string(buf);
}

[[nodiscard]] inline std::string fmt_rect(rect r) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "region(%d,%d,%d,%d)", r.x, r.y, r.w, r.h);
    return std::string(buf);
}

/// ① 这块应该是纯色 X。
[[nodiscard]] inline check expect_color(const image& img, rect r, std::uint32_t expected,
                                        tier t = tier::strict) {
    const policy p = policy_of(t);
    const stats s = region(img, r);
    const std::uint32_t allow = dirty_allowance(p, r);
    check c;
    c.where = fmt_rect(r);
    c.want = fmt_color(expected) +
             (t == tier::strict ? " (strict: dirty=0)"
                                : t == tier::loose ? " (loose: dirty<=周长/4)" : " (any: coverage>=0.5)");
    c.got = fmt_stats(s);
    c.ok = s.px > 0 && s.dominant == expected && s.coverage >= p.min_coverage &&
           s.unique <= p.max_unique && s.dirty() <= allow;
    return c;
}

/// ①' 显式阈值版（逃生口：知道自己在做什么时用）。
[[nodiscard]] inline check expect_color_abs(const image& img, rect r, std::uint32_t expected,
                                            double min_coverage) {
    const stats s = region(img, r);
    check c;
    c.where = fmt_rect(r);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s (coverage>=%.4f)", fmt_color(expected).c_str(), min_coverage);
    c.want = buf;
    c.got = fmt_stats(s);
    c.ok = s.dominant == expected && s.coverage >= min_coverage;
    return c;
}

/// ② 这块应该还没被画（唯一色只有 1 种）。
[[nodiscard]] inline check expect_uniform(const image& img, rect r) {
    const stats s = region(img, r);
    check c;
    c.where = fmt_rect(r);
    c.want = "unique<=1";
    c.got = fmt_stats(s);
    c.ok = s.px > 0 && s.unique <= 1;
    return c;
}

/// ③ 这两块必须不同（y 翻转 / 裁剪 / 镜像类断言唯一的表达方式）。
[[nodiscard]] inline check expect_diff(const image& img, rect a, rect b,
                                       double min_mean_delta = 8.0) {
    const stats sa = region(img, a);
    const stats sb = region(img, b);
    const double delta = (std::abs(sa.mean_r - sb.mean_r) + std::abs(sa.mean_g - sb.mean_g) +
                          std::abs(sa.mean_b - sb.mean_b)) /
                         3.0;
    check c;
    c.where = fmt_rect(a) + " vs " + fmt_rect(b);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "differ (|mean|>=%.1f or dominant!=)", min_mean_delta);
    c.want = buf;
    char got[256];
    std::snprintf(got, sizeof(got), "delta=%.2f  a{%s} b{%s}", delta, fmt_stats(sa).c_str(),
                  fmt_stats(sb).c_str());
    c.got = got;
    c.ok = sa.px > 0 && sb.px > 0 && (sa.dominant != sb.dominant || delta >= min_mean_delta);
    return c;
}

/// ③' 这两块必须相同（成对探针：非对称记号的两侧应一致）。
[[nodiscard]] inline check expect_same(const image& img, rect a, rect b,
                                       double max_mean_delta = 2.0) {
    const stats sa = region(img, a);
    const stats sb = region(img, b);
    const double delta = (std::abs(sa.mean_r - sb.mean_r) + std::abs(sa.mean_g - sb.mean_g) +
                          std::abs(sa.mean_b - sb.mean_b)) /
                         3.0;
    check c;
    c.where = fmt_rect(a) + " == " + fmt_rect(b);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "same (|mean|<=%.1f)", max_mean_delta);
    c.want = buf;
    char got[256];
    std::snprintf(got, sizeof(got), "delta=%.2f  a{%s} b{%s}", delta, fmt_stats(sa).c_str(),
                  fmt_stats(sb).c_str());
    c.got = got;
    c.ok = sa.px > 0 && sb.px > 0 && delta <= max_mean_delta;
    return c;
}

/// ④ 这块应该是渐变的（唯一色数 >= N）。
[[nodiscard]] inline check expect_gradient(const image& img, rect r, std::uint32_t min_unique = 8) {
    const stats s = region(img, r);
    check c;
    c.where = fmt_rect(r);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "unique>=%u", static_cast<unsigned>(min_unique));
    c.want = buf;
    c.got = fmt_stats(s);
    c.ok = s.px > 0 && (s.unique >= min_unique || s.unique_capped);
    return c;
}

// -----------------------------------------------------------------------------
//  证据落盘（失败时留下"能看见的东西"——这是日记里最缺的一项）
// -----------------------------------------------------------------------------
/// 写 PPM(P6)。零依赖、任何看图软件都能开。
/// marks：把这些矩形框成洋红色 1px 边（便于在图上直接看到断言位置）。
[[nodiscard]] inline bool write_ppm(const image& img, const char* path,
                                    const std::vector<rect>& marks = {}) {
    if (!img.valid()) {
        return false;
    }
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", img.w, img.h);

    image copy = img;
    for (const rect& m : marks) {
        const rect b = intersect(m, copy.bounds());
        if (!b.valid()) {
            continue;
        }
        const std::uint32_t magenta = argb(0xFF, 0xFF, 0x00, 0xFF);
        for (int x = b.x; x < b.right(); ++x) {
            copy.put(x, b.y, magenta);
            copy.put(x, b.bottom() - 1, magenta);
        }
        for (int y = b.y; y < b.bottom(); ++y) {
            copy.put(b.x, y, magenta);
            copy.put(b.right() - 1, y, magenta);
        }
    }

    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(img.w) * static_cast<std::size_t>(img.h) * 3u);
    for (std::size_t i = 0, n = static_cast<std::size_t>(img.w) * static_cast<std::size_t>(img.h); i < n; ++i) {
        rgb[i * 3 + 0] = copy.rgba[i * 4 + 0];
        rgb[i * 3 + 1] = copy.rgba[i * 4 + 1];
        rgb[i * 3 + 2] = copy.rgba[i * 4 + 2];
    }
    const bool ok = std::fwrite(rgb.data(), 1, rgb.size(), f) == rgb.size();
    std::fclose(f);
    return ok;
}

// -----------------------------------------------------------------------------
//  批量报告
// -----------------------------------------------------------------------------
struct report {
    int passed = 0;
    int failed = 0;
    std::vector<check> failures;

    void add(const check& c) {
        if (c.ok) {
            ++passed;
        } else {
            ++failed;
            failures.push_back(c);
        }
        std::printf("%s\n", c.text().c_str());
    }
    [[nodiscard]] bool all_ok() const noexcept { return failed == 0; }
};

} // namespace imgprobe
