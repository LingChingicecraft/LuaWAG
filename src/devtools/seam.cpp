// =============================================================================
// devtools/seam.cpp —— 接缝的实现（两个开关都关时，本文件编译成空 TU）
//
// 两个开关的语义：
// WAG_DEVTOOLS_TRACE ⇒ 每次阶段边界写一条 32 字节记录；退出时把环 dump 成 CSV。
// WAG_DEVTOOLS_PROBE ⇒ 每 N 帧读一次默认帧缓冲，跑三条"画面还活着"的断言。
//
// 【为什么用环境变量而不是命令行参数】
// 命令行解析在 main.cpp，而 main.cpp 属于"装配"，不属于工具。
// 为工具加参数会污染那条唯一知道"引擎由哪些部件按什么次序装起来"的文件。
// 环境变量让接缝保持"零命令行面"：
// WAG_TRACE_CSV=<path> trace dump 到哪（默认 trace.csv）
// WAG_PROBE_EVERY=<n> 每 n 帧探一次（默认 0 = 不探）
// WAG_PROBE_DIR=<dir> 失败证据 PPM 落哪（默认当前目录）
// =============================================================================
#include "devtools/seam.hpp"

#if defined(WAG_DEVTOOLS_TRACE) || defined(WAG_DEVTOOLS_PROBE)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(WAG_DEVTOOLS_TRACE)
#    include <fstream>
#    include <iostream>
#endif

#if defined(WAG_DEVTOOLS_PROBE)
#    include <img_probe.hpp> // 通用层：纯像素 + 判据（header-only）

#    include <SDL3/SDL_opengl.h>
#endif

namespace wag::devtools {
namespace {

bool g_initialized = false;
std::uint64_t g_frame_index = 0; ///< 自增的"第几次 on_frame_begin"，与 frame_count 解耦

#if defined(WAG_DEVTOOLS_PROBE)
std::uint64_t g_probe_count = 0;
std::uint64_t g_probe_failures = 0;
int g_probe_every = 0;
std::string g_probe_dir = ".";

int read_int_env(const char* name, int fallback) {
    const char* s = std::getenv(name);
    if (s == nullptr || *s == '\0') {
        return fallback;
    }
    return std::atoi(s);
}

std::string read_str_env(const char* name, const char* fallback) {
    const char* s = std::getenv(name);
    return (s == nullptr || *s == '\0') ? std::string(fallback) : std::string(s);
}

/// 读默认帧缓冲并翻成"左上原点"（与 img_probe 的坐标同域）。
imgprobe::image grab_framebuffer(int w, int h) {
    imgprobe::image img;
    img.w = w;
    img.h = h;
    img.rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0);

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());

    const std::size_t stride = static_cast<std::size_t>(w) * 4u;
    std::vector<std::uint8_t> row(stride);
    for (int y = 0; y < h / 2; ++y) {
        std::uint8_t* top = img.rgba.data() + static_cast<std::size_t>(y) * stride;
        std::uint8_t* bottom = img.rgba.data() + static_cast<std::size_t>(h - 1 - y) * stride;
        std::memcpy(row.data(), top, stride);
        std::memcpy(top, bottom, stride);
        std::memcpy(bottom, row.data(), stride);
    }
    return img;
}

/// "这块不该是纯色 X"——白条检测用的反向断言。
/// 【为什么不复用 expect_color 再取反】取反会把 where/want 的语义弄乱；
/// 这里直接读 region()，结论与说明都写在同一个 check 里。
imgprobe::check expect_not_color(const imgprobe::image& img, imgprobe::rect r, std::uint32_t c,
                                 const char* what) {
    const imgprobe::stats s = imgprobe::region(img, r);
    imgprobe::check chk;
    chk.where = std::string(what) + " " + imgprobe::fmt_rect(r);
    chk.want = "!= " + imgprobe::fmt_color(c) + "（覆盖率 <= 0.90）";
    chk.got = imgprobe::fmt_stats(s);
    chk.ok = !(s.dominant == c && s.coverage > 0.90);
    return chk;
}

void probe_frame(double fb_w, double fb_h) {
    const int w = static_cast<int>(fb_w);
    const int h = static_cast<int>(fb_h);
    if (w <= 0 || h <= 0) {
        return;
    }

    const imgprobe::image img = grab_framebuffer(w, h);
    if (!img.valid()) {
        return;
    }

    imgprobe::report rep;

 // ① 画面还活着：整屏不该只有一两种颜色（防"清屏之后什么都没画"）。
    rep.add(imgprobe::expect_gradient(img, imgprobe::make_rect(0, 0, w, h), 3));

 // ② 白条检测：四条 1px 边带不该是纯白。
 // 这一条直接对应里的"白条"——
 // 那次是拖拽/暴露期间没人重画 ⇒ 窗口背景露出来。它当时只能靠眼睛看。
    const std::uint32_t white = imgprobe::argb(0xFF, 0xFF, 0xFF, 0xFF);
    rep.add(expect_not_color(img, imgprobe::make_rect(0, 0, w, 1), white, "上边带"));
    rep.add(expect_not_color(img, imgprobe::make_rect(0, h - 1, w, 1), white, "下边带"));
    rep.add(expect_not_color(img, imgprobe::make_rect(0, 0, 1, h), white, "左边带"));
    rep.add(expect_not_color(img, imgprobe::make_rect(w - 1, 0, 1, h), white, "右边带"));

    ++g_probe_count;
    if (rep.all_ok()) {
        return;
    }

    ++g_probe_failures;
    std::fprintf(stderr, "[devtools][probe] 第 %llu 次采样失败（帧 %llu）：%d 项\n",
                 static_cast<unsigned long long>(g_probe_count),
                 static_cast<unsigned long long>(g_frame_index), rep.failed);
    for (const imgprobe::check& c : rep.failures) {
        std::fprintf(stderr, "[devtools][probe]   %s\n", c.text().c_str());
    }

 // 失败证据：落一张 PPM，并把四个边带框出来（日记里最缺的就是这个）。
    char path[512];
    std::snprintf(path, sizeof(path), "%s/probe_fail_%06llu.ppm", g_probe_dir.c_str(),
                  static_cast<unsigned long long>(g_probe_count));
    if (imgprobe::write_ppm(img, path,
                            {imgprobe::make_rect(0, 0, w, 1), imgprobe::make_rect(0, h - 1, w, 1),
                             imgprobe::make_rect(0, 0, 1, h), imgprobe::make_rect(w - 1, 0, 1, h)})) {
        std::fprintf(stderr, "[devtools][probe]   证据图：%s\n", path);
    }
}
#endif // WAG_DEVTOOLS_PROBE

#if defined(WAG_DEVTOOLS_TRACE)
/// 退出时把环 dump 出来。
/// 【为什么在 atexit 而不是关闭序列】关闭序列是**内核的规格**，加一步就是改规格；
/// 而"工具把日志吐到文件"不属于规格。atexit 让接缝保持零侵入。
void dump_trace_on_exit() {
    const std::string path = std::getenv("WAG_TRACE_CSV") != nullptr
                                 ? std::string(std::getenv("WAG_TRACE_CSV"))
                                 : std::string("trace.csv");

    std::ofstream out(path);
    if (out) {
        ::wag::trace::dump_csv(out, true);
        std::fprintf(stderr, "[devtools][trace] 已写 %s（%zu 条，环满丢弃 %llu 条）\n", path.c_str(),
                     ::wag::trace::size(),
                     static_cast<unsigned long long>(::wag::trace::dropped()));
    } else {
        std::fprintf(stderr, "[devtools][trace] 写不了 %s\n", path.c_str());
    }

 // 尾 24 条同时打到 stderr：崩溃/退出后不用打开文件也能看最后发生了什么。
    ::wag::trace::dump_text(std::cerr, 24);
}
#endif

#if defined(WAG_DEVTOOLS_PROBE)
/// 退出时给一行汇总。**没有这一行，"探针跑过没有"就无从判断**——
/// 采样成功时它什么都不打印，于是"没跑"和"跑了且全过"看起来一模一样。
void dump_probe_summary() {
    if (g_probe_count == 0) {
        return;
    }
    std::fprintf(stderr, "[devtools][probe] 采样 %llu 次，失败 %llu 次（每 %d 帧一次）\n",
                 static_cast<unsigned long long>(g_probe_count),
                 static_cast<unsigned long long>(g_probe_failures), g_probe_every);
}
#endif

} // namespace

void ensure_init() noexcept {
    if (g_initialized) {
        return;
    }
    g_initialized = true;
#if defined(WAG_DEVTOOLS_TRACE)
 // 时间原点在这里定；帧号由 set_frame 每帧给。
    ::wag::trace::reset(0);
    std::atexit(&dump_trace_on_exit);
#endif
#if defined(WAG_DEVTOOLS_PROBE)
    g_probe_every = read_int_env("WAG_PROBE_EVERY", 0);
    g_probe_dir = read_str_env("WAG_PROBE_DIR", ".");
    std::atexit(&dump_probe_summary);
#endif
}

void set_frame(std::uint64_t frame_count) noexcept {
    ++g_frame_index;
#if defined(WAG_DEVTOOLS_TRACE)
    ::wag::trace::set_frame(static_cast<std::uint32_t>(frame_count));
#else
    (void)frame_count;
#endif
}

#if defined(WAG_DEVTOOLS_PROBE)
void on_before_present(double fb_w, double fb_h) noexcept {
 // 默认 0 ⇒ 不采样（保留零成本；要看画面时才开）。
    if (g_probe_every > 0 && (g_frame_index % static_cast<std::uint64_t>(g_probe_every)) == 0u) {
        probe_frame(fb_w, fb_h);
    }
}
#endif

} // namespace wag::devtools

#endif // 两个开关任一开启
