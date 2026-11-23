// =============================================================================
//  sdl3_expose3_probe —— 回调里"清整屏" vs "只清新露出的条"（落地前的最后对照）
//
//  ============================================================================
//  【为什么还要比这一组】
//      前面已经确定：回调里"**清整屏 + Swap**"能消掉白条（使用者肉眼确认）。
//      ⚠️ 但它有一个**观感副作用**：在正常游戏里，回调会**抹掉当前画面一瞬间**
//         （下一帧就画回来）⇒ 表现为**闪一下**。
//      更保守的做法是：**只刷新露出来的那一条**（用 `glScissor`）。
//      ⇒ 而"新露出的区域"= **当前尺寸 减去 上一次清过的尺寸**，可以算出来。
//      ⇒ 本探针就是比这两条的**观感与代价**。
//  ============================================================================
//
//  【两组（每次运行跑一组）】
//      0 = 回调里 **清整屏 + Swap**            （已验证的那条）
//      1 = 回调里 **只清新露出的区域 + Swap**  （更保守的那条）
//
//  【"新露出的区域"怎么算（这是组 1 的核心）】
//      记 `last_w/last_h` = 上一次**成功交付**时的窗口像素尺寸（初始 = 创建时的尺寸）。
//      回调里读当前 `pw/ph`：
//          若 pw > last_w 且 ph > last_h ⇒ 新区域 = **三块**：
//                ① 右侧竖条  (last_w, 0)   .. (pw, ph)
//                ② 下方横条  (0, last_h)   .. (last_w, ph)
//                ③ 右下角    (last_w, last_h) .. (pw, ph)   —— 与 ①② 拼接后不重不漏
//          若只变宽  ⇒ 只有 ①（高度取 ph）
//          若只变高  ⇒ 只有 ②（宽度取 pw）
//          若变小或不变 ⇒ **什么都不清**（没有"新露出"）
//      清完把 `last_w/last_h = pw/ph`。
//      ⇒ ⚠️ 注意坐标：GL 的 `glScissor` 以**左下角**为原点、+y 向上
//         ⇒ "下方横条"在 GL 里的 y 起点就是 `last_h`（从下往上数）✓
//
//  【本探针的主循环是"正常游戏"的样子】
//      每帧画一个**会动的橙色竖条 + 深蓝底**，然后 Swap。
//      ⇒ 于是你能看清：**拖拽时主循环有没有机会画**、以及那一条露出什么。
//      ⇒ 主循环**每帧都清整屏**（所以正常情况下不会有残留）
//
//  【它不进引擎源码、不进 CMake】
//  编译与运行见本文件末尾。
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <atomic>
#include <cstdio>
#include <thread>

namespace {

std::thread::id g_main_thread_id;

struct stats {
    std::atomic<int> cb_total{0};
    std::atomic<int> exposed{0};
    std::atomic<int> exposed_live{0};
    std::atomic<int> did_work{0};
    std::atomic<int> off_thread{0};
    std::atomic<int> no_change{0};        // 回调时窗口没变大 ⇒ 什么都不必清
    std::atomic<int> grew_w{0};
    std::atomic<int> grew_h{0};
    std::atomic<unsigned long long> work_ns{0};
    std::atomic<unsigned long long> work_max_ns{0};
};

stats g_st;
SDL_Window* g_win = nullptr;
int g_group = 0;
int g_last_w = 0, g_last_h = 0;      // 上一次"清过"的尺寸
bool g_primed = false;

/// 清一个矩形（参数用"左下角原点、+y 向上"的 GL 坐标）。
void clear_rect(int x, int y, int w, int h, float r, float g, float b) {
    if (w <= 0 || h <= 0) { return; }
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

bool SDLCALL event_watch(void* userdata, SDL_Event* event) {
    stats* st = static_cast<stats*>(userdata);
    if (st == nullptr || event == nullptr) { return true; }
    st->cb_total.fetch_add(1);

    if (event->type != SDL_EVENT_WINDOW_EXPOSED) { return true; }
    st->exposed.fetch_add(1);
    if (event->window.data1 != 0) { st->exposed_live.fetch_add(1); }

    if (g_win == nullptr) { return true; }
    if (std::this_thread::get_id() != g_main_thread_id) {
        st->off_thread.fetch_add(1);
        return true;                 // 不在主线程 ⇒ 绝不碰 GL
    }
    if (!g_primed) { return true; }  // 还没画过第一帧 ⇒ 不乱清

    const unsigned long long t0 = SDL_GetTicksNS();

    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_win, &pw, &ph);
    if (pw <= 0 || ph <= 0) { return true; }

    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, pw, ph);

    if (g_group == 0) {
        // ---- 组 0：清整屏 ----
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);       // 信箱黑
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(g_win);
        g_last_w = pw; g_last_h = ph;
    } else {
        // ---- 组 1：只清新露出的那一条（或两块/三块）----
        const int lw = g_last_w, lh = g_last_h;
        const bool wider = (pw > lw);
        const bool taller = (ph > lh);
        if (!wider && !taller) {
            st->no_change.fetch_add(1);
            // 没变大 ⇒ 没有"新露出" ⇒ **不 Swap**（把这一帧留给主循环）
            st->work_ns.fetch_add(SDL_GetTicksNS() - t0);
            return true;
        }
        // 信箱黑：与引擎的信箱同色（观感最不突兀）
        const float R = 0.0f, G = 0.0f, B = 0.0f;
        if (wider && taller) {
            // 三块：右侧竖条 + 下方横条 + 右下角（拼接后不重不漏）
            clear_rect(lw, 0,    pw - lw, ph,      R, G, B);
            clear_rect(0,  lh,   lw,      ph - lh, R, G, B);
            clear_rect(lw, lh,   pw - lw, ph - lh, R, G, B);
            st->grew_w.fetch_add(1);
            st->grew_h.fetch_add(1);
        } else if (wider) {
            clear_rect(lw, 0, pw - lw, ph, R, G, B);
            st->grew_w.fetch_add(1);
        } else {
            clear_rect(0, lh, pw, ph - lh, R, G, B);
            st->grew_h.fetch_add(1);
        }
        SDL_GL_SwapWindow(g_win);           // ⚠️ 必须 Swap（否则等于没做）
        g_last_w = pw; g_last_h = ph;
    }

    const unsigned long long dt = SDL_GetTicksNS() - t0;
    st->work_ns.fetch_add(dt);
    if (dt > st->work_max_ns.load()) { st->work_max_ns.store(dt); }
    st->did_work.fetch_add(1);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    int group = 1;
    double seconds = 25.0;
    if (argc >= 2) { group = std::atoi(argv[1]); }
    if (argc >= 3) { seconds = std::atof(argv[2]); }

    g_group = group;
    g_main_thread_id = std::this_thread::get_id();

    const char* gname = (group == 0) ? "回调：清整屏 + Swap"
                                     : "回调：只清新露出的条 + Swap";

    std::printf("=============================================================\n");
    std::printf(" 白条解法 · 第三阶段（清整屏 vs 只清新条）\n");
    std::printf(" 组 %d：%s\n", group, gname);
    std::printf(" 跑 %.1f 秒 —— 请反复拖动窗口的右边缘与下边缘\n", seconds);
    std::printf("=============================================================\n\n");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow("expose3 probe", 900, 600,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (win == nullptr) {
        std::printf("SDL_CreateWindow 失败：%s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    g_win = win;
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

    // 第一帧：把整屏画成深蓝，并记下"清过的尺寸"
    {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, pw, ph);
        glClearColor(0.0f, 0.0f, 0.25f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(win);
        g_last_w = pw;
        g_last_h = ph;
        g_primed = true;
    }

    if (!SDL_AddEventWatch(event_watch, &g_st)) {
        std::printf("    SDL_AddEventWatch 失败：%s\n", SDL_GetError());
    } else {
        std::printf("    已装事件观察者\n");
    }

    std::printf("\n    颜色约定：主循环画 = **深蓝底 + 橙色竖条（会动）**\n");
    std::printf("              回调清的是 **信箱黑**\n");
    if (group == 0) {
        std::printf("    ⇒ 组 0 拖拽时**整窗会闪一下黑**（那是回调清了整屏）\n");
    } else {
        std::printf("    ⇒ 组 1 拖拽时**只有新露出的那一条变黑**，整窗不闪\n");
    }
    std::printf("\n    请观察两件事：① 白条还在不在 ② 有没有「整窗闪一下」\n\n");

    // ---------------------------------------------------------------- 主循环
    const Uint64 t0 = SDL_GetTicks();
    const Uint64 t_end = t0 + static_cast<Uint64>(seconds * 1000.0);
    int frames = 0;
    unsigned long long pump_blocked_ms = 0;
    int gl_error_count = 0;
    GLenum first_err = GL_NO_ERROR;

    while (SDL_GetTicks() < t_end) {
        // ---- 阶段 3：事件泵（回调由它触发）----
        const Uint64 a = SDL_GetTicks();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { /* 靠时间结束 */ }
        }
        {
            const unsigned long long took = SDL_GetTicks() - a;
            if (took > 50) { pump_blocked_ms += took; }
        }

        // ---- 阶段 7：正常画一帧（深蓝底 + 橙色竖条）----
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        if (pw > 0 && ph > 0) {
            glDisable(GL_SCISSOR_TEST);
            glViewport(0, 0, pw, ph);
            glClearColor(0.0f, 0.0f, 0.25f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            const float t = static_cast<float>(SDL_GetTicks() % 3000) / 3000.0f;
            const int bar_w = (pw / 12 > 8) ? pw / 12 : 8;
            const int bx = static_cast<int>(t * static_cast<float>(pw - bar_w));
            glMatrixMode(GL_PROJECTION); glLoadIdentity();
            glOrtho(0.0, static_cast<double>(pw), 0.0, static_cast<double>(ph), -1.0, 1.0);
            glMatrixMode(GL_MODELVIEW); glLoadIdentity();
            glColor3f(1.0f, 0.6f, 0.1f);
            glBegin(GL_QUADS);
            glVertex2i(bx, 0);
            glVertex2i(bx + bar_w, 0);
            glVertex2i(bx + bar_w, ph);
            glVertex2i(bx, ph);
            glEnd();

            SDL_GL_SwapWindow(win);
            g_last_w = pw;      // 主循环也"清过"了整屏 ⇒ 更新记录
            g_last_h = ph;
            ++frames;
        }

        for (;;) {
            const GLenum err = glGetError();
            if (err == GL_NO_ERROR) { break; }
            if (first_err == GL_NO_ERROR) { first_err = err; }
            ++gl_error_count;
        }
        SDL_Delay(4);
    }

    const double secs = static_cast<double>(SDL_GetTicks() - t0) / 1000.0;
    const int worked = g_st.did_work.load();
    const double total_ms = static_cast<double>(g_st.work_ns.load()) / 1000000.0;

    std::printf("=============================================================\n");
    std::printf("【小结】组 %d：%s\n", group, gname);
    std::printf("    运行时长                    = %.1f 秒\n", secs);
    std::printf("    主循环帧数                  = %d   （≈ %.1f fps）\n", frames,
                static_cast<double>(frames) / (secs > 0 ? secs : 1.0));
    std::printf("    「单次泵 > 50ms」累计       = %llu ms\n", pump_blocked_ms);
    std::printf("\n");
    std::printf("    回调总次数                  = %d\n", g_st.cb_total.load());
    std::printf("    WINDOW_EXPOSED 次数         = %d\n", g_st.exposed.load());
    std::printf("      其中 live-resize          = %d\n", g_st.exposed_live.load());
    std::printf("    回调里成功做事              = %d\n", worked);
    std::printf("    ⚠️ 回调不在主线程           = %d\n", g_st.off_thread.load());
    std::printf("    窗口没变大 ⇒ 什么都不清     = %d   （组 1 专用）\n", g_st.no_change.load());
    std::printf("    只变宽 / 只变高             = %d / %d\n",
                g_st.grew_w.load(), g_st.grew_h.load());
    std::printf("    回调累计耗时                = %.2f ms\n", total_ms);
    if (worked > 0) {
        std::printf("    回调单次平均                = %.3f ms\n", total_ms / static_cast<double>(worked));
    }
    std::printf("    回调单次最大                = %.3f ms\n",
                static_cast<double>(g_st.work_max_ns.load()) / 1000000.0);
    std::printf("\n");
    std::printf("    GL 错误数                   = %d", gl_error_count);
    if (gl_error_count > 0) { std::printf("   （第一个 = 0x%04X）", static_cast<unsigned>(first_err)); }
    std::printf("\n\n");

    std::printf("【怎么读】\n");
    std::printf("  · 比「回调单次平均」⇒ 清整屏 vs 只清新条，谁便宜\n");
    std::printf("  · 组 1 的「窗口没变大 ⇒ 什么都不清」若很大 ⇒ 说明多数暴露并非「变大」\n");
    std::printf("    （那些暴露我们不处理 ⇒ 主循环下一帧会补上）\n");
    std::printf("  · **观感只能你肉眼判断**：白条在不在 / 有没有整窗闪一下\n");
    std::printf("=============================================================\n");

    SDL_RemoveEventWatch(event_watch, &g_st);
    g_win = nullptr;
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

// =============================================================================
//  编译与运行（MSYS2 UCRT64）
//
//  cd /c/Users/vqwjq/Documents/LuaWAG/LuaWAG-Vanilla
//  g++ -std=c++20 -O1 -g -IC:/msys64/ucrt64/include sdl3_expose3_probe.cpp \
//      -o bin/sdl3_expose3_probe.exe -LC:/msys64/ucrt64/lib -lSDL3 -lopengl32
//
//  ./bin/sdl3_expose3_probe.exe 0 25 > e3_full.txt  2>&1; cat e3_full.txt
//  ./bin/sdl3_expose3_probe.exe 1 25 > e3_strip.txt 2>&1; cat e3_strip.txt
// =============================================================================
