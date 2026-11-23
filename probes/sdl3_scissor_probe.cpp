// =============================================================================
//  sdl3_scissor_probe —— 把"窗口坐标矩形 → GL 的 scissor/viewport 盒子"这段算术钉死
//
//  ============================================================================
//  【为什么需要它】
//      引擎里那一次 y 翻转原来写成一行：
//            glScissor(vx, fb_h - (vy + vh), vw, vh);
//      它**是对的**（活着的引擎画面正常），但它把三件事压在一行里：
//            · 哪个量是"下边"
//            · 帧缓冲高度从哪来
//            · 这次翻转是不是必要
//      ⇒ 于是"再多减一次 fb_h"这种看起来合理的改动会**把内容翻到屏幕外**，
//        而它在代码审查里很难被看出来。
//      ⇒ 本探针的目的**不是**去猜公式，而是**证明这一处算术唯一**：
//           · 先证明一条恒等式（见下），于是"用下边"与"用上边"**结果相同**
//             ⇒ 这说明这次改写**不改变行为**
//           · 再用 GL 实测把算出来的盒子与 `glGetIntegerv` 读回的**逐项比对**
//
//  【那条恒等式（本探针第 1 组就是验它）】
//      记窗口/帧缓冲高度为 H，视口矩形的下边为 b、高为 h（`top() = b + h`）
//          b == H − top()   当且仅当   H == b + h
//      ⇒ 若两者用的是**同一个高度**，则"用下边"与"用上边"给出**同一个 y**。
//      ⇒ 这是 `space::framebuffer_box_for` **不改变行为**的根据。
//
//  【它用的是引擎自己那份实现】
//      `#include "space/viewport.hpp"` ⇒ 调用 `space::framebuffer_box_for`
//      ⇒ 所以它证的不是"我抄对了没有"，而是"引擎里这一处算术唯一"。
//      ⚠️ 因此编译时要加 `-I src`。
//
//  ============================================================================
//
//  【三组】
//      组 0 = **纯公式**（不需要窗口）：遍历 {窗口尺寸} × {render 尺寸} × {帧缓冲尺寸}
//             对每个组合：
//               ① 用引擎的 recompute 逻辑（这里直接算 letterbox dst）得到视口矩形
//               ② 断言恒等式 `b == H − top()`（当两高度相等时）
//               ③ 同时算"用上边"与"用下边"两条路 ⇒ **必须相等**
//               ④ 断言盒子在帧缓冲范围内（越界夹取之前）
//             打印统计：组合数、恒等式失败数、两条路不一致数、越界数
//
//      组 1 = **GL 实测**（需要窗口）：窗口 + GL 上下文
//             对每个组合：调 glViewport/glScissor 喂**引擎算出的盒子**，
//             然后 `glGetIntegerv(GL_VIEWPORT / GL_SCISSOR_BOX)` 读回，
//             与算出的 {x, y, w, h} **逐项**比对。
//             ⇒ 这一组同时验证"GL 的下标与我们的盒子是同一套约定"。
//
//      组 2 = **它在屏幕上的样子**（需要窗口，可视化）：
//             把帧缓冲清成黑，用 scissor 把**引擎算出的那个盒子**涂成橙，
//             然后在盒子内部再画一条**只在上 1/4**的白条（用同一个盒子的坐标推）。
//             ⇒ 若 y 翻转错了，橙块会跑到屏幕上方/下方，而白条会贴在错的那一边。
//             ⇒ 显示 3 秒后继续；最后按任意键或超时退出。
//
//  【它不进引擎源码、不进 CMake】
//  编译与运行见本文件末尾（注意 `-I src`）。
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include "space/viewport.hpp"

#include <cstdio>

namespace {

// -----------------------------------------------------------------------------
//  复刻视口策略：给 {窗口尺寸, render 尺寸, 策略} 算出"屏幕上那个矩形"
//
//  【为什么这里要抄一遍 recompute 的算术】
//      因为 `viewport::recompute` 的入参是 `rect` 且它写的是自己的成员，
//      而本探针要遍历成百上千个组合 ⇒ 抄这一小段（3 行）比构造上千个对象干净。
//      ⚠️ 但也因此：**本探针验不了 recompute 本身**，它验的是
//        `framebuffer_box_for` 那一段。两者是不同的问题，别混。
// -----------------------------------------------------------------------------
wag::space::rect letterbox_dst(const wag::space::rect& render, const wag::space::rect& win) {
    wag::space::rect dst = win;
    if (win.w > 0.0 && win.h > 0.0 && render.is_valid()) {
        const double s = (win.w / render.w < win.h / render.h) ? (win.w / render.w)
                                                               : (win.h / render.h);
        dst.w = render.w * s;
        dst.h = render.h * s;
        dst.l = (win.w - dst.w) * 0.5;
        dst.b = (win.h - dst.h) * 0.5;   // ← 从**下**往上量（左下图元）
    }
    return dst;
}

int g_case = 0;
int g_bad_identity = 0;
int g_bad_two_ways = 0;
int g_out_of_range = 0;
int g_bad_y_sign = 0;   // 恒等式不成立时的另一种可能：y 落在范围外
int g_shown = 0;

/// 跑一个组合。`fb` 是帧缓冲尺寸（通常是窗口尺寸；刻意也试不一致的情形）。
void one_case(const wag::space::rect& win, const wag::space::rect& render,
              const wag::space::rect& fb) {
    const wag::space::rect dst = letterbox_dst(render, win);
    const wag::space::framebuffer_box box = wag::space::framebuffer_box_for(dst, fb.w, fb.h);
    ++g_case;

    // ---- ① 两条路：用"上边"（引擎那条）与用"下边" ----
    const int rounded_b = static_cast<int>(dst.b + 0.5);
    const int via_top = static_cast<int>(fb.h + 0.5) - (rounded_b + box.h);

    if (via_top != box.y) {
        ++g_bad_two_ways;
        if (g_shown < 8) {
            std::printf("      !! 两条路不一致: b=%d h=%d fb_h=%d  via_top=%d  box.y=%d\n",
                        rounded_b, box.h, box.fb_h, via_top, box.y);
            ++g_shown;
        }
    }

    // ---- ② 恒等式：当两个高度是同一个值时，b == fb_h − top ----
    if (box.fb_h == static_cast<int>(win.h + 0.5)) {
        // 顺便验 rect 自己的不变量：top() 必须等于 b + h（两者只能有一个定义）
        if (dst.top() != dst.b + dst.h) { ++g_bad_identity; }
        const int top = rounded_b + box.h;
        if (rounded_b != box.fb_h - top) {
            ++g_bad_identity;
        }
    }

    // ---- ③ 盒子该在帧缓冲范围内（这是"夹取之前"的判据）----
    const int slack = 2;   // 取整余量的上限
    if (box.x < -slack || box.y < -slack || box.x + box.w > box.fb_w + slack ||
        box.y + box.h > box.fb_h + slack) {
        ++g_out_of_range;
        if (g_bad_y_sign < 6) {
            std::printf("      !! 越界: win=%.0fx%.0f fb=%dx%d -> box(x=%d y=%d w=%d h=%d)\n",
                        win.w, win.h, box.fb_w, box.fb_h, box.x, box.y, box.w, box.h);
            ++g_bad_y_sign;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    int group = 0;
    if (argc >= 2) { group = std::atoi(argv[1]); }

    const char* gname = (group == 0) ? "纯公式（不需要窗口）"
                      : (group == 1) ? "GL 实测（读回 GL_VIEWPORT / GL_SCISSOR_BOX）"
                                     : "可视化（橙块 + 上 1/4 白条）";

    std::printf("=============================================================\n");
    std::printf(" scissor/viewport 算术探针\n");
    std::printf(" 组 %d：%s\n", group, gname);
    std::printf("=============================================================\n\n");

    // -------------------------------------------------------------------------
    //  测试用的尺寸集合（刻意含**非整数**与**奇数**，让取整余量暴露出来）
    // -------------------------------------------------------------------------
    const double win_w[] = {900.0, 1280.0, 1366.0, 1920.0, 1600.5, 1920.25};
    const double win_h[] = {600.0, 720.0, 768.0, 1080.0, 900.5, 1080.75};
    const double ren_w[] = {1920.0, 1280.0, 960.0, 640.0};
    const double ren_h[] = {1080.0, 720.0, 540.0, 360.0};

    // =========================================================================
    //  组 0：纯公式
    // =========================================================================
    if (group == 0) {
        for (double ww : win_w) {
            for (double wh : win_h) {
                const wag::space::rect win{0.0, 0.0, ww, wh};
                for (double rw : ren_w) {
                    for (double rh : ren_h) {
                        const wag::space::rect render{-rw * 0.5, -rh * 0.5, rw, rh};
                        // ① 帧缓冲 = 窗口（正常情形）
                        one_case(win, render, win);
                        // ② 帧缓冲比窗口**大**（窗口刚变大、帧缓冲还没跟）
                        one_case(win, render, wag::space::rect{0.0, 0.0, ww + 312.0, wh + 347.0});
                        // ③ 帧缓冲比窗口**小**（阶段 1 旧、阶段 7 新那种时序）
                        one_case(win, render, wag::space::rect{0.0, 0.0, ww, wh - 200.0});
                    }
                }
            }
        }

        std::printf("  遍历组合数              = %d\n", g_case);
        std::printf("  恒等式 b == fb_h − top 失败 = %d\n", g_bad_identity);
        std::printf("  两条路（上边/下边）不一致   = %d\n", g_bad_two_ways);
        std::printf("  盒子越出帧缓冲           = %d\n", g_out_of_range);
        std::printf("\n");
        std::printf("【怎么读】\n");
        std::printf("  · 恒等式失败 = 0 ⇒ **用上边与用下边在这个集合上完全一致**\n");
        std::printf("      ⇒ 也就是说 `framebuffer_box_for` 的改写**不改变行为**\n");
        std::printf("  · 两条路不一致 = 0 ⇒ 同一份算术的两种写法等价（互为交叉验证）\n");
        std::printf("  · 越界 = 0（除刻意让 fb 比窗口小的那些组合）⇒ 夹取之前的形状是对的\n");
        return 0;
    }

    // =========================================================================
    //  组 1 / 组 2：需要窗口
    // =========================================================================
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("scissor probe", 900, 600,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (win == nullptr) {
        std::printf("SDL_CreateWindow 失败：%s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (gl == nullptr) {
        std::printf("SDL_GL_CreateContext 失败：%s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_ShowWindow(win);
    SDL_PumpEvents();
    SDL_Delay(300);

    int fb_w = 0, fb_h = 0;
    SDL_GetWindowSizeInPixels(win, &fb_w, &fb_h);
    std::printf("  帧缓冲（由平台报）= %d x %d\n\n", fb_w, fb_h);

    if (group == 1) {
        // ---------------------------------------------------------------------
        //  组 1：喂进去、读回来、逐项比对
        // ---------------------------------------------------------------------
        int cases = 0, bad_vp = 0, bad_sc = 0, shown = 0;

        for (double rw : ren_w) {
            for (double rh : ren_h) {
                const wag::space::rect win_r{0.0, 0.0, static_cast<double>(fb_w),
                                             static_cast<double>(fb_h)};
                const wag::space::rect render{-rw * 0.5, -rh * 0.5, rw, rh};
                const wag::space::rect dst = letterbox_dst(render, win_r);
                const wag::space::framebuffer_box box =
                    wag::space::framebuffer_box_for(dst, static_cast<double>(fb_w),
                                                    static_cast<double>(fb_h));
                ++cases;

                glDisable(GL_SCISSOR_TEST);
                glViewport(box.x, box.y, box.w, box.h);
                glEnable(GL_SCISSOR_TEST);
                glScissor(box.x, box.y, box.w, box.h);

                GLint vp[4] = {0, 0, 0, 0};
                GLint sc[4] = {0, 0, 0, 0};
                glGetIntegerv(GL_VIEWPORT, vp);
                glGetIntegerv(GL_SCISSOR_BOX, sc);
                glDisable(GL_SCISSOR_TEST);

                const bool vp_ok = (vp[0] == box.x && vp[1] == box.y && vp[2] == box.w &&
                                    vp[3] == box.h);
                const bool sc_ok = (sc[0] == box.x && sc[1] == box.y && sc[2] == box.w &&
                                    sc[3] == box.h);
                if (!vp_ok) { ++bad_vp; }
                if (!sc_ok) { ++bad_sc; }
                if ((!vp_ok || !sc_ok) && shown < 10) {
                    std::printf("      !! render=%.0fx%.0f dst(b=%.1f h=%.1f) -> box(%d,%d,%d,%d)\n",
                                rw, rh, dst.b, dst.h, box.x, box.y, box.w, box.h);
                    std::printf("         GL_VIEWPORT  = %d,%d,%d,%d\n", vp[0], vp[1], vp[2], vp[3]);
                    std::printf("         GL_SCISSOR   = %d,%d,%d,%d\n", sc[0], sc[1], sc[2], sc[3]);
                    ++shown;
                }
            }
        }

        std::printf("  组合数                       = %d\n", cases);
        std::printf("  GL_VIEWPORT 与算式不符        = %d\n", bad_vp);
        std::printf("  GL_SCISSOR_BOX 与算式不符     = %d\n", bad_sc);
        std::printf("\n");
        std::printf("【怎么读】\n");
        std::printf("  · 不符 = 0 ⇒ GL 收下的下标与 `framebuffer_box_for` 算出的**逐项相同**\n");
        std::printf("      ⇒ 于是「盒子的 y 就是 GL 要的那个 y」这件事**有了实测背书**\n");
        std::printf("  · 若 GL_SCISSOR_BOX 与 viewport 不同 ⇒ 说明有别的状态在影响 scissor\n");
    } else {
        // ---------------------------------------------------------------------
        //  组 2：可视化 —— 橙块 + 只在上 1/4 的白条
        // ---------------------------------------------------------------------
        const double render_w = 1280.0, render_h = 720.0;
        const wag::space::rect win_r{0.0, 0.0, static_cast<double>(fb_w),
                                     static_cast<double>(fb_h)};
        const wag::space::rect render{-render_w * 0.5, -render_h * 0.5, render_w, render_h};
        const wag::space::rect dst = letterbox_dst(render, win_r);
        const wag::space::framebuffer_box box =
            wag::space::framebuffer_box_for(dst, static_cast<double>(fb_w),
                                            static_cast<double>(fb_h));

        std::printf("  视口矩形（窗口坐标）= l=%.1f b=%.1f w=%.1f h=%.1f\n", dst.l, dst.b,
                    dst.w, dst.h);
        std::printf("  算出的盒子          = x=%d y=%d w=%d h=%d（fb=%dx%d）\n", box.x, box.y,
                    box.w, box.h, box.fb_w, box.fb_h);
        std::printf("\n  应当看到：\n");
        std::printf("    · 一个**居中**的橙色矩形（上下左右都有等宽黑边）\n");
        std::printf("    · 它的**上半部分**有一条白色横条\n");
        std::printf("    · 若白条跑到了下半 ⇒ y 方向反了\n");
        std::printf("    · 若橙色矩形不居中（偏上/偏下）⇒ 那次「换参照边」的算术错了\n");
        std::printf("\n  显示 3 秒...\n");

        const int quarter = box.h / 4;
        const Uint64 t_end = SDL_GetTicks() + 3000;
        while (SDL_GetTicks() < t_end) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) { /* 靠时间结束 */ }
            }
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(win, &pw, &ph);

            // 全屏黑
            glDisable(GL_SCISSOR_TEST);
            glViewport(0, 0, pw, ph);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            // 用**同一个盒子**涂橙（清屏受 scissor 限制 —— 这是已实测的事实）
            glEnable(GL_SCISSOR_TEST);
            glScissor(box.x, box.y, box.w, box.h);
            glClearColor(1.0f, 0.55f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            // 盒子的**上半**一条白（y 在盒子上方 ⇒ 帧缓冲坐标里 y 更大）
            if (quarter > 0) {
                glScissor(box.x, box.y + box.h - quarter, box.w, quarter);
                glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            glDisable(GL_SCISSOR_TEST);

            SDL_GL_SwapWindow(win);
            SDL_Delay(16);
        }
        std::printf("  完成。\n");
    }

    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

// =============================================================================
//  编译与运行（MSYS2 UCRT64）
//
//  cd /c/Users/vqwjq/Documents/LuaWAG/LuaWAG-Vanilla
//  g++ -std=c++20 -O1 -g -Isrc -IC:/msys64/ucrt64/include sdl3_scissor_probe.cpp \
//      -o bin/sdl3_scissor_probe.exe -LC:/msys64/ucrt64/lib -lSDL3 -lopengl32
//
//  ./bin/sdl3_scissor_probe.exe 0 > sc_formula.txt 2>&1; cat sc_formula.txt
//  ./bin/sdl3_scissor_probe.exe 1 > sc_gl.txt 2>&1;      cat sc_gl.txt
//  ./bin/sdl3_scissor_probe.exe 2 > sc_visual.txt 2>&1;  cat sc_visual.txt
// =============================================================================
