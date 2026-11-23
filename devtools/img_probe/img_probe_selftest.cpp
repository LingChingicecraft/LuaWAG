// =============================================================================
//  img_probe_selftest.cpp —— 判据层的脱机单测（不建窗口、不需要 GPU）
//
//  验证的是"采集与判据分离"里**判据那一半**：
//      给定一块 RGB 缓冲，四类断言是否给出正确结论。
//  这一半必须能脱机测——否则每次改判据都要开一次窗口。
// =============================================================================
#include <cstdio>

#include "img_probe.hpp"

using namespace imgprobe;

namespace {

int g_fail = 0;

void expect_true(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        ++g_fail;
    }
}

void expect_check(const check& c, bool want_ok, const char* what) {
    const bool ok = (c.ok == want_ok);
    std::printf("%s  %s | %s\n", ok ? "PASS" : "FAIL", what, c.text().c_str());
    if (!ok) {
        ++g_fail;
    }
}

constexpr std::uint32_t kRed = argb(0xFF, 0xFF, 0x00, 0x00);
constexpr std::uint32_t kGreen = argb(0xFF, 0x00, 0xFF, 0x00);
constexpr std::uint32_t kBlue = argb(0xFF, 0x00, 0x00, 0xFF);
constexpr std::uint32_t kWhite = argb(0xFF, 0xFF, 0xFF, 0xFF);
constexpr std::uint32_t kBlack = argb(0xFF, 0x00, 0x00, 0x00);

} // namespace

int main() {
    std::printf("=== img_probe 判据层自测 ===\n");

    // ---- 1. 纯色：颜色 / 均匀 / 非渐变 -----------------------------------
    {
        image img = image::make(64, 64, kRed);
        expect_check(expect_color(img, make_rect(8, 8, 16, 16), kRed), true, "纯色命中（strict）");
        expect_check(expect_uniform(img, make_rect(0, 0, 64, 64)), true, "纯色判为均匀");
        expect_check(expect_gradient(img, make_rect(0, 0, 64, 64)), false, "纯色不是渐变");
    }

    // ---- 2. 颜色打包与字节序（ARGB <-> RGBA）-----------------------------
    {
        image img = image::make(4, 4, kBlack);
        img.put(1, 1, argb(0xFF, 0x12, 0x34, 0x56));
        const std::uint32_t got = img.get(1, 1);
        expect_true(got == argb(0xFF, 0x12, 0x34, 0x56), "put/get 往返一致（ARGB）");
        const std::uint8_t* p = img.at(1, 1);
        expect_true(p[0] == 0x12 && p[1] == 0x34 && p[2] == 0x56 && p[3] == 0xFF,
                    "内存字节序是 R,G,B,A");
    }

    // ---- 3. 四象限：关系断言（这是 y 翻转 / 裁剪类断言唯一的表达方式）----
    {
        image img = image::make(64, 64, kBlack);
        img.fill_rect(make_rect(0, 0, 32, 32), kRed);      // 左上
        img.fill_rect(make_rect(32, 0, 32, 32), kGreen);   // 右上
        img.fill_rect(make_rect(0, 32, 32, 32), kBlue);    // 左下
        img.fill_rect(make_rect(32, 32, 32, 32), kWhite);  // 右下

        expect_check(expect_color(img, make_rect(4, 4, 8, 8), kRed), true, "左上是红");
        expect_check(expect_color(img, make_rect(52, 52, 8, 8), kWhite), true, "右下是白");
        expect_check(expect_diff(img, make_rect(4, 4, 8, 8), make_rect(52, 52, 8, 8)), true,
                     "左上与右下不同");
        expect_check(expect_diff(img, make_rect(4, 4, 8, 8), make_rect(4, 52, 8, 8)), true,
                     "左上与左下不同（镜像可检出）");
        expect_check(expect_same(img, make_rect(4, 4, 8, 8), make_rect(12, 12, 8, 8)), true,
                     "左上两块相同");
    }

    // ---- 4. 渐变 ---------------------------------------------------------
    {
        image img = image::make(64, 16, kBlack);
        for (int x = 0; x < 64; ++x) {
            const std::uint8_t v = static_cast<std::uint8_t>(x * 4);
            for (int y = 0; y < 16; ++y) {
                img.put(x, y, argb(0xFF, v, v, v));
            }
        }
        expect_check(expect_gradient(img, make_rect(0, 0, 64, 16), 32), true, "渐变被识别");
        expect_check(expect_color(img, make_rect(0, 0, 64, 16), kBlack), false, "渐变不是纯黑");
    }

    // ---- 5. 档位：有噪点的区域 strict 失败、any 通过 ----------------------
    {
        image img = image::make(16, 16, kRed);
        for (int y = 0; y < 16; ++y) {
            img.put(0, y, kBlack);   // 只弄脏最左一列
        }
        expect_check(expect_color(img, make_rect(0, 0, 16, 16), kRed, tier::strict), false,
                     "有 6% 噪点 ⇒ strict 失败");
        expect_check(expect_color(img, make_rect(0, 0, 16, 16), kRed, tier::loose), true,
                     "有 6% 噪点 ⇒ loose 通过");
        expect_check(expect_color(img, make_rect(1, 0, 15, 16), kRed, tier::strict), true,
                     "避开那列 ⇒ strict 通过");
    }

    // ---- 6. 越界矩形：不崩，返回 px==0 且断言失败 ------------------------
    {
        image img = image::make(8, 8, kRed);
        const stats s = region(img, make_rect(100, 100, 16, 16));
        expect_true(s.px == 0, "越界矩形：px==0");
        expect_check(expect_color(img, make_rect(100, 100, 16, 16), kRed), false, "越界矩形断言失败");
        const stats half = region(img, make_rect(-4, -4, 8, 8));
        expect_true(half.px == 16, "半越界矩形被裁剪到 4x4");
    }

    // ---- 7. 证据落盘（失败时能留下看得见的东西）--------------------------
    {
        image img = image::make(32, 32, kBlue);
        img.fill_rect(make_rect(8, 8, 16, 16), kGreen);
        const bool ok = write_ppm(img, "selftest_evidence.ppm", {make_rect(8, 8, 16, 16)});
        expect_true(ok, "write_ppm 落盘成功（selftest_evidence.ppm）");
    }

    std::printf("=== 结果：%s ===\n", g_fail == 0 ? "全部通过" : "有失败");
    return g_fail == 0 ? 0 : 1;
}
