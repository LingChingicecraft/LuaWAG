// =============================================================================
//  sdl3_resize_probe —— "绘制区到底多大" + "换尺寸之后它什么时候跟上"
//
//  ============================================================================
//  【第一版为什么错（这一段必须留着，否则下一个人还会这么写）】
//      第一版用 `glReadPixels` **读边界外是否报错** 来量帧缓冲尺寸。
//      ⇒ **那条判据不存在**：`glReadPixels` 的错误只有
//            GL_INVALID_ENUM（format/type 非法）
//            GL_INVALID_OPERATION（读的缓冲缺失 / 不完整 / 多重采样）
//            GL_INVALID_FRAMEBUFFER_OPERATION
//        ⇒ **"读窗口之外"不在其中** —— 那些像素的值**未定义**，但调用**成功**。
//      ⇒ 于是二分退化，"测"出来的值只是"我二分到哪里为止"：
//        同一个窗口两次测出 2160 与 1510 ⇒ 那是垃圾，不是尺寸。
//  ============================================================================
//
//  【现在用三条**互相独立**的来源】
//      ① `glGetIntegerv(GL_VIEWPORT)` 在**从未设过**的上下文上的**初值**
//            ⇒ 那是驱动眼里的 drawable 尺寸（我们没写过它 ⇒ 是"读回"而非"问自己"）
//      ② `SDL_GetWindowSizeInPixels`
//            ⇒ SDL 眼里的像素尺寸
//      ③ **Win32 `GetClientRect`**（走 `SDL_PROP_WINDOW_WIN32_HDC_POINTER` 拿 HWND）
//            ⇒ 完全不经过 SDL 的客户区尺寸
//      ⇒ 三者一致 ⇒ "绘制区 = 窗口客户区"这件事在三方都成立
//      ⇒ 三者不一致 ⇒ 逐项报告差在哪
//
//  【关于"跟上得及不及时"】这里只报一个**可观测**的量：
//      换尺寸之后，每一采样点上 `SDL 报的` 与 `GL 视口初值`（重新 MakeCurrent 后再读）
//      是否相等。⚠️ 而 GL 视口一旦被我们设过就不再是"初值"了 ⇒
//      所以本探针**每轮都重新 MakeCurrent 并 Swap**，希望驱动重置它；
//      若驱动不重置，本探针会在输出里**明说"这一项不可用"**，而不是编一个结论。
//
//  【它不进引擎源码、不进 CMake】
//  编译与运行见本文件末尾。
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace {

/// 读 GL 视口（当前上下文）。
void read_viewport(int out[4]) {
    out[0] = out[1] = out[2] = out[3] = -1;
    glGetIntegerv(GL_VIEWPORT, out);
}

/// 把整张"候选大画布"清成某个颜色（用 scissor 限范围）。
void clear_candidate(int w, int h, float r, float g, float b) {
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, w, h);
    glScissor(0, 0, w, h);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

/// 在 (x, 0) 读一点。
void read_point(int x, int y, unsigned char out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

} // namespace

int main(int argc, char** argv) {
    int w0 = 1920, h0 = 1080;
    int w1 = 1960, h1 = 1080;
    if (argc >= 5) {
        w0 = std::atoi(argv[1]); h0 = std::atoi(argv[2]);
        w1 = std::atoi(argv[3]); h1 = std::atoi(argv[4]);
    }
    const int slack = 400;                 // 候选大画布比窗口大多少

    std::printf("=============================================================\n");
    std::printf(" 绘制区尺寸与换尺寸延迟探针（第二版）\n");
    std::printf(" 初始 = %d x %d     之后改成 = %d x %d\n", w0, h0, w1, h1);
    std::printf(" 三条独立来源：GL 视口初值 / SDL 像素尺寸 / Win32 客户区\n");
    std::printf("=============================================================\n\n");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("resize probe v2", w0, h0,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (win == nullptr) {
        std::printf("SDL_CreateWindow 失败：%s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // ---------------------------------------------------------------- ① 建上下文后立刻读视口初值
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (gl == nullptr) {
        std::printf("SDL_GL_CreateContext 失败：%s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    int vp_before_swap[4] = {0, 0, 0, 0};
    read_viewport(vp_before_swap);

    SDL_ShowWindow(win);
    SDL_PumpEvents();
    SDL_Delay(400);

    int vp_after_show[4] = {0, 0, 0, 0};
    read_viewport(vp_after_show);

    // 一次 swap（模拟"平台把绘制面准备好"）
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, w0, h0);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(win);
    SDL_Delay(100);

    int vp_after_swap[4] = {0, 0, 0, 0};
    read_viewport(vp_after_swap);

    int sdl_coord_w = 0, sdl_coord_h = 0, sdl_pix_w = 0, sdl_pix_h = 0;
    SDL_GetWindowSize(win, &sdl_coord_w, &sdl_coord_h);
    SDL_GetWindowSizeInPixels(win, &sdl_pix_w, &sdl_pix_h);

    // ---------------------------------------------------------------- ② Win32 交叉
    long w32_client_w = -1, w32_client_h = -1;
#if defined(_WIN32)
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd != nullptr) {
        RECT cr{};
        if (GetClientRect(hwnd, &cr)) {
            w32_client_w = cr.right - cr.left;
            w32_client_h = cr.bottom - cr.top;
        }
    }
#endif

    std::printf("【一】三条独立来源（初始尺寸）\n");
    std::printf("    GL 视口 · 建上下文后立刻读 = %d x %d\n", vp_before_swap[2], vp_before_swap[3]);
    std::printf("    GL 视口 · ShowWindow 之后读 = %d x %d\n", vp_after_show[2], vp_after_show[3]);
    std::printf("    GL 视口 · 一次 Swap 之后读  = %d x %d\n", vp_after_swap[2], vp_after_swap[3]);
    std::printf("    SDL 窗口坐标               = %d x %d\n", sdl_coord_w, sdl_coord_h);
    std::printf("    SDL 像素尺寸               = %d x %d\n", sdl_pix_w, sdl_pix_h);
    std::printf("    Win32 GetClientRect        = %ld x %ld\n", w32_client_w, w32_client_h);
    std::printf("    请求的尺寸                 = %d x %d\n", w0, h0);
    std::printf("\n");

    const bool three_way_agree =
        (sdl_pix_w == static_cast<int>(w32_client_w)) &&
        (sdl_pix_h == static_cast<int>(w32_client_h));
    std::printf("    ⇒ SDL 像素 == Win32 客户区 ？ %s\n", three_way_agree ? "是 ✓" : "**否** ⚠️");
    std::printf("\n");

    // ---------------------------------------------------------------- ③ 候选大画布：两色差分
    // 目的：找"我们的清屏最多能覆盖到哪里"。
    // 方法：把候选区域清成红、读若干点；再清成绿、读同样这些点。
    //       值**随颜色改变** ⇒ 那些像素在我们的清屏范围内（且它们在帧缓冲内）。
    std::printf("【二】差分法：清红 vs 清绿，看哪些 x 处的像素**跟随清屏颜色变化**\n");
    std::printf("    （候选画布 = %d x %d）\n", sdl_pix_w + slack, sdl_pix_h + slack);

    const int xs[] = { 0, 100, sdl_pix_w - 1, sdl_pix_w, sdl_pix_w + 1,
                       sdl_pix_w + 50, sdl_pix_w + slack - 1 };
    const int n_xs = static_cast<int>(sizeof(xs) / sizeof(xs[0]));

    unsigned char red_rgba[8][4] = {};
    unsigned char grn_rgba[8][4] = {};

    clear_candidate(sdl_pix_w + slack, sdl_pix_h + slack, 1.0f, 0.0f, 0.0f);
    glFinish();
    for (int i = 0; i < n_xs; ++i) { read_point(xs[i], 0, red_rgba[i]); }

    clear_candidate(sdl_pix_w + slack, sdl_pix_h + slack, 0.0f, 1.0f, 0.0f);
    glFinish();
    for (int i = 0; i < n_xs; ++i) { read_point(xs[i], 0, grn_rgba[i]); }

    std::printf("\n    %-10s %-16s %-16s %s\n", "x", "清红后读到", "清绿后读到", "跟随颜色变化？");
    std::printf("    %-10s %-16s %-16s %s\n", "----", "---------", "---------", "-------------");
    int last_following = -1;
    for (int i = 0; i < n_xs; ++i) {
        const bool follows = (red_rgba[i][0] != grn_rgba[i][0]) ||
                             (red_rgba[i][1] != grn_rgba[i][1]) ||
                             (red_rgba[i][2] != grn_rgba[i][2]);
        char a[32], b[32];
        std::snprintf(a, sizeof(a), "%d,%d,%d", red_rgba[i][0], red_rgba[i][1], red_rgba[i][2]);
        std::snprintf(b, sizeof(b), "%d,%d,%d", grn_rgba[i][0], grn_rgba[i][1], grn_rgba[i][2]);
        std::printf("    %-10d %-16s %-16s %s\n", xs[i], a, b, follows ? "是 ✓" : "**否**");
        if (follows) { last_following = xs[i]; }
    }
    std::printf("\n    ⇒ 跟随颜色变化的**最大 x** = %d\n", last_following);
    std::printf("    ⇒ 若它 >= SDL 报的 %d ⇒ **绘制区至少和窗口一样大**（没问题）\n", sdl_pix_w - 1);
    std::printf("    ⇒ 若它 < SDL 报的宽度 ⇒ **绘制区比窗口小** ⇒ 右边会有一条没被清过的区\n");
    std::printf("\n");
    std::printf("    ⚠️ 注意本项的限度：**超出真实帧缓冲的读出是未定义的**，\n");
    std::printf("       两次读到的可能是同一堆垃圾 ⇒ 那些点会显示「否」。\n");
    std::printf("       而「否」因此**只能作为参考**，不能单独作为「绘制区到此为止」的证据。\n");
    std::printf("       真正的上限来自【一】的 **GL 视口初值**。\n");
    std::printf("\n");

    // ---------------------------------------------------------------- ④ 换尺寸后逐帧观察
    std::printf("【三】换尺寸：SDL_SetWindowSize(%d, %d)，逐轮重新 MakeCurrent + Swap + 读视口\n\n", w1, h1);
    std::printf("    %-8s %-12s %-12s %-14s %s\n",
                "t(ms)", "SDL窗口", "SDL像素", "GL视口初值", "事件");
    std::printf("    %-8s %-12s %-12s %-14s %s\n",
                "-----", "-------", "-------", "-----------", "----");

    const Uint64 t0 = SDL_GetTicks();
    SDL_SetWindowSize(win, w1, h1);

    bool pix_seen_new = false, vp_seen_new = false;
    unsigned long long pix_first = 0, vp_first = 0;
    int vp_after_resize[4] = {0, 0, 0, 0};

    for (int step = 0; step < 120; ++step) {
        int ev = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_WINDOW_RESIZED ||
                e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) { ++ev; }
        }

        int cw = 0, ch = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(win, &cw, &ch);
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        read_viewport(vp_after_resize);

        const unsigned long long ms = SDL_GetTicks() - t0;
        if (pw == w1 && !pix_seen_new) { pix_seen_new = true; pix_first = ms; }
        if (vp_after_resize[2] == w1 && !vp_seen_new) { vp_seen_new = true; vp_first = ms; }

        if (step < 3 || ev > 0) {
            std::printf("    %-8llu %-12d %-12d %-14d %d\n",
                        ms, cw, pw, vp_after_resize[2], ev);
        }
        SDL_Delay(10);
    }

    int fin_cw = 0, fin_ch = 0, fin_pw = 0, fin_ph = 0;
    SDL_GetWindowSize(win, &fin_cw, &fin_ch);
    SDL_GetWindowSizeInPixels(win, &fin_pw, &fin_ph);
    int fin_vp[4] = {0, 0, 0, 0};
    read_viewport(fin_vp);

#if defined(_WIN32)
    long fin_w32_w = -1, fin_w32_h = -1;
    if (hwnd != nullptr) {
        RECT cr{};
        if (GetClientRect(hwnd, &cr)) { fin_w32_w = cr.right - cr.left; fin_w32_h = cr.bottom - cr.top; }
    }
#else
    long fin_w32_w = -1, fin_w32_h = -1;
#endif

    std::printf("\n【四】收尾\n");
    std::printf("    SDL 窗口坐标 = %d x %d\n", fin_cw, fin_ch);
    std::printf("    SDL 像素     = %d x %d\n", fin_pw, fin_ph);
    std::printf("    GL 视口      = %d x %d\n", fin_vp[2], fin_vp[3]);
    std::printf("    Win32 客户区 = %ld x %ld\n", fin_w32_w, fin_w32_h);
    std::printf("    请求的尺寸   = %d x %d\n", w1, h1);
    std::printf("\n");

    std::printf("【五】判据\n");
    std::printf("    SDL 像素先变成新值：%s\n",
                pix_seen_new ? "是（见下表时刻）" : "**从未**");
    if (pix_seen_new) { std::printf("      → %llu ms\n", pix_first); }
    std::printf("    GL 视口先变成新值： %s\n",
                vp_seen_new ? "是" : "**从未**（或驱动不重置视口 ⇒ 这一项不可用）");
    if (vp_seen_new) { std::printf("      → %llu ms\n", vp_first); }
    std::printf("\n");
    if (vp_after_resize[2] != w1) {
        std::printf("    ⚠️ GL 视口最终没变成 %d ⇒ **很可能是驱动不重置视口**（我们设过它），\n", w1);
        std::printf("       而不是「帧缓冲没跟上」 ⇒ 这一项**不可用**，不要据此下结论。\n");
    }
    std::printf("\n");

    std::printf("按回车退出（或等 10 秒）。\n");
    {
        const Uint64 deadline = SDL_GetTicks() + 10000;
        bool quit = false;
        while (!quit && SDL_GetTicks() < deadline) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_KEY_DOWN ||
                    e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) { quit = true; }
            }
            SDL_Delay(50);
        }
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
//  g++ -std=c++20 -O1 -g -IC:/msys64/ucrt64/include sdl3_resize_probe.cpp \
//      -o bin/sdl3_resize_probe.exe -LC:/msys64/ucrt64/lib -lSDL3 -lopengl32 -luser32 -lgdi32
//
//  ./bin/sdl3_resize_probe.exe > resize.txt 2>&1; cat resize.txt
//  ./bin/sdl3_resize_probe.exe 1280 720 1310 720
// =============================================================================
