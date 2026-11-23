// =============================================================================
//  main.cpp —— pixel_probe 的命令行入口
//
//  子命令：
//      selftest       判据层脱机自测（不需要 GPU / 窗口）
//      gl-demo        建 GL 上下文 → 造已知图案 → 抓帧 → 断言（证明 driver 正确）
//      engine-check   只读检查"探针依赖的引擎约定"是否还在
//      all            三者依次跑（默认）
//
//  设计取向（左左左）：
//      · 通用层纯像素坐标（不认识 DIP / transform）
//      · 阈值默认给档位（strict / loose / any），显式阈值只作逃生口
//      · 打点/探针各自独立域，不侵入引擎阶段
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine_check.hpp"
#include "img_probe.hpp"

#if defined(WAG_DEVTOOLS_HAVE_SDL3)
#include "sdl_gl_grab.hpp"
#endif

namespace {

using namespace imgprobe;

struct options {
    std::string command = "all";
    std::string root;          ///< 引擎根（LuaWAG-Vanilla）
    std::string save_dir = "."; ///< 证据落盘目录
    int width = 320;
    int height = 240;
    bool hidden = false;
};

constexpr std::uint32_t kRed = argb(0xFF, 0xFF, 0x00, 0x00);
constexpr std::uint32_t kGreen = argb(0xFF, 0x00, 0xFF, 0x00);
constexpr std::uint32_t kBlue = argb(0xFF, 0x00, 0x00, 0xFF);
constexpr std::uint32_t kWhite = argb(0xFF, 0xFF, 0xFF, 0xFF);
constexpr std::uint32_t kBlack = argb(0xFF, 0x00, 0x00, 0x00);

void usage() {
    std::printf(
        "pixel_probe —— 不需要参考图的图形断言\n"
        "\n"
        "用法: pixel_probe [命令] [选项]\n"
        "\n"
        "命令:\n"
        "  selftest       判据层脱机自测（不需要 GPU）\n"
        "  gl-demo        建 GL 上下文、造已知图案、抓帧、断言\n"
        "  engine-check   只读检查探针依赖的引擎约定\n"
        "  all            依次跑全部（默认）\n"
        "\n"
        "选项:\n"
        "  --root <dir>     引擎根目录（含 src/）\n"
        "  --save-dir <dir> 证据落盘目录（默认当前目录）\n"
        "  --size WxH       窗口尺寸（默认 320x240）\n"
        "  --hidden         隐藏窗口（无头）\n"
        "  --help\n");
}

bool parse_size(const char* s, int& w, int& h) {
    int a = 0;
    int b = 0;
    if (std::sscanf(s, "%dx%d", &a, &b) != 2 || a <= 0 || b <= 0) {
        return false;
    }
    w = a;
    h = b;
    return true;
}

// -----------------------------------------------------------------------------
//  子命令：engine-check
// -----------------------------------------------------------------------------
int run_engine_check(const options& opt) {
    std::printf("\n=== engine-check（只读契约检查）===\n");
    const std::string root = engine::find_root(opt.root);
    if (root.empty()) {
        std::printf("FAIL  找不到引擎根目录（用 --root 指定 LuaWAG-Vanilla）\n");
        return 1;
    }
    std::printf("引擎根：%s\n", root.c_str());

    const std::vector<engine::result> rs = engine::check(root);
    int failed = 0;
    for (const engine::result& r : rs) {
        std::printf("%s  %-32s %s\n", r.ok ? "PASS" : "FAIL", r.name.c_str(), r.evidence.c_str());
        if (!r.ok) {
            ++failed;
        }
    }
    std::printf("--- engine-check：%d 条，失败 %d ---\n", static_cast<int>(rs.size()), failed);
    return failed == 0 ? 0 : 1;
}

// -----------------------------------------------------------------------------
//  子命令：gl-demo
// -----------------------------------------------------------------------------
#if defined(WAG_DEVTOOLS_HAVE_SDL3)
int run_gl_demo(const options& opt) {
    std::printf("\n=== gl-demo（driver + 判据，端到端）===\n");

    sdl_gl::config cfg;
    cfg.w = opt.width;
    cfg.h = opt.height;
    cfg.hidden = opt.hidden;

    std::string err;
    if (!sdl_gl::create(cfg, err)) {
        std::printf("SKIP  %s\n", err.c_str());
        std::printf("--- gl-demo：跳过（无法建 GL 上下文，不算失败）---\n");
        return 0;
    }

    const int fw = sdl_gl::fb_w();
    const int fh = sdl_gl::fb_h();
    std::printf("帧缓冲：%dx%d\n", fw, fh);

    // 造一个已知图案：四象限 + 左上角一个非对称小方块。
    //   【为什么要有那个小方块】四象限在 180° 旋转下也能"看起来对"，
    //   而日记里方向判据翻车正是因为"对称判据判不出方向"。
    //   非对称记号是 y 翻转唯一的硬证据。
    const int hw = fw / 2;
    const int hh = fh / 2;
    sdl_gl::clear_all(kBlack);
    sdl_gl::clear_rect(make_rect(0, 0, hw, hh), kRed);            // 左上
    sdl_gl::clear_rect(make_rect(hw, 0, fw - hw, hh), kGreen);    // 右上
    sdl_gl::clear_rect(make_rect(0, hh, hw, fh - hh), kBlue);     // 左下
    sdl_gl::clear_rect(make_rect(hw, hh, fw - hw, fh - hh), kWhite); // 右下
    const rect mark = make_rect(8, 8, 16, 16);
    sdl_gl::clear_rect(mark, kWhite);
    sdl_gl::reset_state();

    // 【纪律】抓帧在 present 之前。
    image img = sdl_gl::grab(err);
    if (!img.valid()) {
        std::printf("FAIL  grab 失败：%s\n", err.c_str());
        sdl_gl::destroy();
        return 1;
    }
    sdl_gl::present();

    report rep;
    const int inset = 6;

    // 四象限（避开边界）
    rep.add(expect_color(img, make_rect(inset, inset + 20, hw - 2 * inset, hh - 2 * inset - 20), kRed,
                         tier::strict));
    rep.add(expect_color(img, make_rect(hw + inset, inset + 20, fw - hw - 2 * inset, hh - 2 * inset - 20),
                         kGreen, tier::strict));
    rep.add(expect_color(img, make_rect(inset, hh + inset, hw - 2 * inset, fh - hh - 2 * inset), kBlue,
                         tier::strict));
    rep.add(expect_color(img, make_rect(hw + inset, hh + inset, fw - hw - 2 * inset, fh - hh - 2 * inset),
                         kWhite, tier::strict));

    // 非对称记号：在左上，不在左下镜像位置
    rep.add(expect_color(img, mark, kWhite, tier::strict));
    rep.add(expect_diff(img, mark, make_rect(8, fh - 8 - 16, 16, 16), 8.0));

    // 方向关系：左上红 vs 左下蓝（若 y 翻转错，这两块会互换）
    rep.add(expect_color(img, make_rect(inset, inset, 2, 2), kRed, tier::strict));

    // 整屏不是单色（证明"图案真的画上了"）
    rep.add(expect_gradient(img, make_rect(0, 0, fw, fh), 3));

    const std::string evidence = opt.save_dir + "/gl_demo.ppm";
    if (write_ppm(img, evidence.c_str(), {mark})) {
        std::printf("（证据图：%s）\n", evidence.c_str());
    }

    if (sdl_gl::last_gl_error() != 0u) {
        std::printf("WARN  GL 错误：0x%04X\n", sdl_gl::last_gl_error());
    }

    sdl_gl::destroy();
    std::printf("--- gl-demo：通过 %d，失败 %d ---\n", rep.passed, rep.failed);
    return rep.all_ok() ? 0 : 1;
}
#else
int run_gl_demo(const options&) {
    std::printf("\n=== gl-demo ===\n");
    std::printf("SKIP  本次构建未链接 SDL3（WAG_DEVTOOLS_HAVE_SDL3 未定义）\n");
    return 0;
}
#endif

// -----------------------------------------------------------------------------
//  子命令：selftest
// -----------------------------------------------------------------------------
int run_selftest(const options& opt) {
    std::printf("\n=== selftest（判据层，脱机）===\n");
    // 复用同一套断言，不建窗口。
    report rep;
    const image quad = [] {
        image img = image::make(64, 64, kBlack);
        img.fill_rect(make_rect(0, 0, 32, 32), kRed);
        img.fill_rect(make_rect(32, 0, 32, 32), kGreen);
        img.fill_rect(make_rect(0, 32, 32, 32), kBlue);
        img.fill_rect(make_rect(32, 32, 32, 32), kWhite);
        return img;
    }();
    rep.add(expect_color(quad, make_rect(4, 4, 8, 8), kRed, tier::strict));
    rep.add(expect_color(quad, make_rect(52, 52, 8, 8), kWhite, tier::strict));
    rep.add(expect_diff(quad, make_rect(4, 4, 8, 8), make_rect(4, 52, 8, 8)));
    rep.add(expect_uniform(quad, make_rect(4, 4, 8, 8)));

    const std::string evidence = opt.save_dir + "/selftest_quad.ppm";
    if (write_ppm(quad, evidence.c_str(), {make_rect(4, 4, 8, 8), make_rect(4, 52, 8, 8)})) {
        std::printf("（证据图：%s）\n", evidence.c_str());
    }
    std::printf("--- selftest：通过 %d，失败 %d ---\n", rep.passed, rep.failed);
    return rep.all_ok() ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    options opt;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            usage();
            return 0;
        } else if (std::strcmp(a, "--root") == 0 && i + 1 < argc) {
            opt.root = argv[++i];
        } else if (std::strcmp(a, "--save-dir") == 0 && i + 1 < argc) {
            opt.save_dir = argv[++i];
        } else if (std::strcmp(a, "--size") == 0 && i + 1 < argc) {
            if (!parse_size(argv[++i], opt.width, opt.height)) {
                std::printf("FAIL  --size 格式应为 WxH\n");
                return 2;
            }
        } else if (std::strcmp(a, "--hidden") == 0) {
            opt.hidden = true;
        } else if (a[0] != '-') {
            opt.command = a;
        } else {
            std::printf("FAIL  未知参数 %s\n", a);
            usage();
            return 2;
        }
    }

    std::printf("pixel_probe · LuaWAG devtools\n");

    int rc = 0;
    if (opt.command == "selftest") {
        rc |= run_selftest(opt);
    } else if (opt.command == "gl-demo") {
        rc |= run_gl_demo(opt);
    } else if (opt.command == "engine-check") {
        rc |= run_engine_check(opt);
    } else if (opt.command == "all") {
        rc |= run_selftest(opt);
        rc |= run_engine_check(opt);
        rc |= run_gl_demo(opt);
    } else {
        std::printf("FAIL  未知命令 %s\n", opt.command.c_str());
        usage();
        return 2;
    }

    std::printf("\n总结果：%s\n", rc == 0 ? "通过" : "有失败");
    return rc;
}
