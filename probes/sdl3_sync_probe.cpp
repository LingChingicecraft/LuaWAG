// =============================================================================
//  sdl3_sync_probe —— 三组对照：默认 / 每帧 SDL_SyncWindow / 开那个 hint
//
//  ============================================================================
//  【为什么做这个（联网查到的官方说法，见下面三条原文）】
//      ① SDL_SetWindowSize 是**请求**：
//           "On some windowing systems, this request is asynchronous and the new
//            window size may not have been applied immediately upon the return of
//            this function. If an immediate change is required, call SDL_SyncWindow()
//            to block until the changes have taken effect."
//           "Additionally, as this is just a request, it can be denied by the
//            windowing system."
//      ② SDL_SyncWindow：**阻塞直到待生效的窗口状态落实**
//           "Block until any pending window state is finalized."
//           "On windowing systems where changes are immediate, this does nothing."
//      ③ SDL_HINT_VIDEO_SYNC_WINDOW_OPERATIONS：**默认是异步**
//           "0": Window operations are non-blocking. (default)
//           "1": Window operations will block until completed.
//           "Be aware that amount of time spent blocking ... can be quite lengthy,
//            as animations may have to complete, which can take upwards of multiple
//            seconds in some cases."
//  ============================================================================
//
//  【它要回答的两个问题】
//      Q1 **手拖**窗口时，一个帧内"阶段 1 读到的尺寸"与"阶段 7 读到的尺寸"会不会不同？
//           ⇒ 三组各跑一次，比这个计数
//      Q2 SDL_SyncWindow 的**代价**是多少（每次调用阻塞多久）？
//           ⇒ 直接计时
//
//  【三组】
//      组 0：默认（什么都不做）
//      组 1：**每帧**在阶段 1 之前调一次 SDL_SyncWindow
//      组 2：SDL_SetHint(SDL_HINT_VIDEO_SYNC_WINDOW_OPERATIONS, "1")（启动时设一次）
//
//  【怎么产生"尺寸变化"】本探针**自己不带节奏**，全靠你**用手拖**：
//      运行时请**反复、缓慢地**拖动窗口右边缘与下边缘（每次几像素）
//      ⇒ 三种触发路径都试：拖右、拖下、拖右下角
//      时间不够就给第 3 个参数加长（秒）
//
//  【它不进引擎源码、不进 CMake】
//  编译与运行见本文件末尾。
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>
#include <cstdlib>

namespace {

struct counters {
    int frames = 0;
    int mismatch = 0;          // 阶段1 尺寸 != 阶段7 尺寸
    int pix_ne_L = 0;          // 阶段7 像素尺寸 != 阶段1 尺寸
    int size_events = 0;       // 泵到的 RESIZED / PIXEL_SIZE_CHANGED 次数
    unsigned long long sync_total_us = 0;
    unsigned long long sync_max_us = 0;
    int sync_calls = 0;
};

void run_group(int group, double seconds) {
    const char* gname = (group == 0) ? "默认（什么都不做）"
                      : (group == 1) ? "每帧 SDL_SyncWindow"
                                     : "SDL_HINT_VIDEO_SYNC_WINDOW_OPERATIONS=1";

    std::printf("\n=============================================================\n");
    std::printf("【组 %d】%s\n", group, gname);
    if (group == 2) {
        SDL_SetHint(SDL_HINT_VIDEO_SYNC_WINDOW_OPERATIONS, "1");
        std::printf("    已设 hint = \"1\"（注意：这个 hint 是全局的，本进程内不再复位）\n");
    }
    std::printf("    请用手**反复缓慢**拖动窗口的右边缘与下边缘（每次几像素）\n");
    std::printf("=============================================================\n");

    SDL_Window* win = SDL_CreateWindow("sync probe", 900, 600,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (win == nullptr) {
        std::printf("    SDL_CreateWindow 失败：%s\n", SDL_GetError());
        return;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (gl == nullptr) {
        std::printf("    SDL_GL_CreateContext 失败：%s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        return;
    }
    SDL_ShowWindow(win);
    SDL_PumpEvents();
    SDL_Delay(300);

    counters c{};
    const Uint64 t_end = SDL_GetTicks() + static_cast<Uint64>(seconds * 1000.0);
    int shown = 0;

    while (SDL_GetTicks() < t_end) {
        // ---- 组 1：每帧同步一次（计时）----
        if (group == 1) {
            const Uint64 a = SDL_GetPerformanceCounter();
            SDL_SyncWindow(win);
            const Uint64 b = SDL_GetPerformanceCounter();
            const unsigned long long us =
                (b - a) * 1000000ull / SDL_GetPerformanceFrequency();
            c.sync_total_us += us;
            if (us > c.sync_max_us) { c.sync_max_us = us; }
            ++c.sync_calls;
        }

        // ---- 阶段 1 ----
        int wL = 0, hL = 0;
        SDL_GetWindowSize(win, &wL, &hL);

        // ---- 阶段 3 ----
        int ev = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { /* 继续，靠时间结束 */ }
            if (e.type == SDL_EVENT_WINDOW_RESIZED ||
                e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) { ++ev; }
        }
        c.size_events += ev;

        // ---- 阶段 7 ----
        int w7 = 0, h7 = 0, p7 = 0, q7 = 0;
        SDL_GetWindowSize(win, &w7, &h7);
        SDL_GetWindowSizeInPixels(win, &p7, &q7);

        const bool mismatch = (wL != w7) || (hL != h7);
        const bool pix_ne = (p7 != wL) || (q7 != hL);
        if (mismatch) { ++c.mismatch; }
        if (pix_ne) { ++c.pix_ne_L; }

        if ((mismatch || pix_ne) && shown < 24) {
            std::printf("    [帧 %d] 阶段1=%dx%d  阶段7=%dx%d  像素=%dx%d  事件=%d  %s\n",
                        c.frames, wL, hL, w7, h7, p7, q7, ev,
                        mismatch ? "**不一致**" : "**像素!=阶段1**");
            ++shown;
        }

        // ---- 画一帧（维持窗口有内容）----
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, p7 > 0 ? p7 : wL, q7 > 0 ? q7 : hL);
        glClearColor(0.0f, 0.0f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(win);

        ++c.frames;
        SDL_Delay(8);
    }

    std::printf("\n    小结（组 %d：%s）\n", group, gname);
    std::printf("        帧数                    = %d\n", c.frames);
    std::printf("        泵到的尺寸事件          = %d\n", c.size_events);
    std::printf("        阶段1 != 阶段7          = %d 帧\n", c.mismatch);
    std::printf("        像素尺寸 != 阶段1 尺寸  = %d 帧\n", c.pix_ne_L);
    if (group == 1 && c.sync_calls > 0) {
        std::printf("        SyncWindow 调用         = %d 次\n", c.sync_calls);
        std::printf("        SyncWindow 总耗时       = %.2f ms\n",
                    static_cast<double>(c.sync_total_us) / 1000.0);
        std::printf("        SyncWindow 单次最大     = %.3f ms  ← 这就是它的代价\n",
                    static_cast<double>(c.sync_max_us) / 1000.0);
        std::printf("        SyncWindow 单次平均     = %.3f ms\n",
                    static_cast<double>(c.sync_total_us) / static_cast<double>(c.sync_calls) / 1000.0);
    }

    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(win);
    SDL_Delay(400);
}

} // namespace

int main(int argc, char** argv) {
    double seconds = 12.0;
    int only = -1;
    if (argc >= 2) { seconds = std::atof(argv[1]); }
    if (argc >= 3) { only = std::atoi(argv[2]); }

    std::printf("=============================================================\n");
    std::printf(" SDL_SyncWindow / SYNC_WINDOW_OPERATIONS 对照探针\n");
    std::printf(" 每组跑 %.1f 秒\n", seconds);
    if (only >= 0) { std::printf(" 只跑组 %d\n", only); }
    std::printf("=============================================================\n");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }

    for (int g = 0; g <= 2; ++g) {
        if (only >= 0 && g != only) { continue; }
        run_group(g, seconds);
    }

    // 复位 hint（虽然本进程要退出了，但保持干净）
    SDL_ResetHint(SDL_HINT_VIDEO_SYNC_WINDOW_OPERATIONS);

    std::printf("\n=============================================================\n");
    std::printf("【怎么读】\n");
    std::printf("  · 若组 0 的「阶段1 != 阶段7」> 0，而组 1 或组 2 = 0\n");
    std::printf("      ⇒ **同步确实消除了「一帧内两个尺寸」** ⇒ 它是可用的一条修法\n");
    std::printf("  · 若三组都 > 0 ⇒ 手拖的中间态**不是 SDL_SyncWindow 能锁住的**\n");
    std::printf("      ⇒ 那就只能靠「清屏覆盖得比测量值多」（修法A）\n");
    std::printf("  · 组 1 的「单次最大耗时」若很大（几十 ms 以上）⇒ 每帧同步不可接受\n");
    std::printf("      ⇒ 只能做成可选/临时\n");
    std::printf("=============================================================\n");

    SDL_Quit();
    return 0;
}

// =============================================================================
//  编译与运行（MSYS2 UCRT64）
//
//  cd /c/Users/vqwjq/Documents/LuaWAG/LuaWAG-Vanilla
//  g++ -std=c++20 -O1 -g -IC:/msys64/ucrt64/include sdl3_sync_probe.cpp \
//      -o bin/sdl3_sync_probe.exe -LC:/msys64/ucrt64/lib -lSDL3 -lopengl32
//
//  ./bin/sdl3_sync_probe.exe 12 > sync.txt 2>&1; cat sync.txt
//  ./bin/sdl3_sync_probe.exe 12 0 > sync0.txt 2>&1; cat sync0.txt
//  ./bin/sdl3_sync_probe.exe 12 1 > sync1.txt 2>&1; cat sync1.txt
//  ./bin/sdl3_sync_probe.exe 12 2 > sync2.txt 2>&1; cat sync2.txt
// =============================================================================
