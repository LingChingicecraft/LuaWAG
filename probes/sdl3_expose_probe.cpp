// =============================================================================
//  sdl3_expose_probe —— 白条的两条官方方向对照
//
//  ============================================================================
//  【官方文档两条依据（本机 SDL3 头文件原文）】
//      ① SDL_events.h:139-140
//           SDL_EVENT_WINDOW_EXPOSED, /**< Window has been exposed and **should be
//             redrawn**, and **can be redrawn directly from event watchers for this
//             event**. data1 is **1 for live-resize expose events**, 0 otherwise. */
//      ② SDL_events.h:1484-1491（SDL_AddEventWatch）
//           "`filter` will be called when an event happens, and its **return value
//            is ignored**."
//           "**WARNING**: Be very careful of what you do in the event filter function,
//            as it **may run in a different thread**!"
//      ③ SDL_hints.h:4702-4717
//           SDL_HINT_WINDOWS_ERASE_BACKGROUND_MODE
//             "0"/"never"   : Never clear the window.
//             "1"/"initial" : Clear the window when the first WM_ERASEBKGND fires. (default)
//             "2"/"always"  : Clear the window on every WM_ERASEBKGND event.
//           "This hint should be set **before creating a window**."
//      ④ 官方 wiki（SDL3/AppFreezeDuringDrag）：
//           "your call into SDL_PollEvent ... **may block during resizing or dragging**...
//            so **new frames of video won't render**"
//           ⇒ 官方给的出路就是 **SDL_AddEventWatch + 监听 SDL_EVENT_WINDOW_EXPOSED**，
//             并在**回调里直接重画**。
//  ============================================================================
//
//  【本探针要回答的三件事】
//      Q1 **回调在哪个线程**？（若不在主线程，则"回调里调 GL"是未定义行为
//           —— 那会直接决定方案①能不能用）
//      Q2 暴露事件有多少、其中 **live-resize**（data1 == 1）有多少？
//      Q3 在拖拽期间，**主线程还能画多少帧**（帧率）？以及"回调里重画"能否追上暴露？
//
//  【多组（每次运行只跑一组，靠第 3 个参数选）】
//      0 = 默认（什么都不做）                       —— 基线
//      1 = 只设 SDL_HINT_WINDOWS_ERASE_BACKGROUND_MODE = "always"（建窗口前设）
//      2 = 只装事件观察者，**只在主线程**时立刻清屏
//      3 = 两者都上
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

/// 主线程 id 与"观察者是否跑在主线程"这个问题的答案。
std::thread::id g_main_thread_id;

struct watch_stats {
    std::atomic<int> cb_total{0};        // 回调总次数（所有事件）
    std::atomic<int> exposed{0};         // WINDOW_EXPOSED 次数
    std::atomic<int> exposed_live{0};    // 其中 data1 != 0（live-resize）
    std::atomic<int> redraws{0};         // 我们在回调里成功清屏的次数
    std::atomic<int> off_thread{0};      // 回调**不在主线程**的次数
    std::atomic<unsigned long long> cb_ns{0};   // 回调里花掉的总时间（纳秒）
};

watch_stats g_stats;
SDL_Window* g_win = nullptr;
bool g_watch_enabled = false;

/// 事件观察者。
///
/// ⚠️ 两处必须小心的（头文件明写）：
///   ① 回调**可能在不同线程** ⇒ 因此本函数**先判断线程**，
///      只有确认在主线程时才碰 GL（否则只记数，不碰上下文）。
///   ② 返回值**被忽略** ⇒ 所以返回 true 只是形式，不影响事件是否入队。
bool SDLCALL event_watch(void* userdata, SDL_Event* event) {
    watch_stats* st = static_cast<watch_stats*>(userdata);
    if (st == nullptr || event == nullptr) { return true; }

    const unsigned long long t0 = SDL_GetTicksNS();
    st->cb_total.fetch_add(1);

    if (event->type == SDL_EVENT_WINDOW_EXPOSED) {
        st->exposed.fetch_add(1);
        if (event->window.data1 != 0) { st->exposed_live.fetch_add(1); }

        if (g_watch_enabled && g_win != nullptr) {
            if (std::this_thread::get_id() == g_main_thread_id) {
                // —— 官方原话：可以在观察者里**直接重画** ——
                // 这里做**最轻**的一种重画：清屏（整块帧缓冲）。
                glDisable(GL_SCISSOR_TEST);
                glClearColor(0.0f, 0.35f, 0.0f, 1.0f);     // 绿：一眼看出是"回调画的"
                glClear(GL_COLOR_BUFFER_BIT);
                SDL_GL_SwapWindow(g_win);
                st->redraws.fetch_add(1);
            } else {
                st->off_thread.fetch_add(1);
            }
        }
    }

    st->cb_ns.fetch_add(SDL_GetTicksNS() - t0);
    return true;   // 返回值被忽略；这里保持"允许入队"
}

} // namespace

int main(int argc, char** argv) {
    int group = 0;
    double seconds = 20.0;
    if (argc >= 2) { group = std::atoi(argv[1]); }
    if (argc >= 3) { seconds = std::atof(argv[2]); }

    g_main_thread_id = std::this_thread::get_id();

    const char* gname =
        (group == 0) ? "默认（什么都不做）"
      : (group == 1) ? "只设 ERASE_BACKGROUND_MODE=always"
      : (group == 2) ? "只装事件观察者（回调里清屏）"
                     : "两者都上";

    std::printf("=============================================================\n");
    std::printf(" 白条 · 官方两条方向对照探针\n");
    std::printf(" 组 %d：%s\n", group, gname);
    std::printf(" 跑 %.1f 秒 —— 请**反复拖动窗口的右边缘与下边缘**\n", seconds);
    std::printf("=============================================================\n\n");

    // ⚠️ 那个 hint 必须在**创建窗口之前**设（头文件明写）
    if (group == 1 || group == 3) {
        SDL_SetHint(SDL_HINT_WINDOWS_ERASE_BACKGROUND_MODE, "always");
        std::printf("    已设 SDL_HINT_WINDOWS_ERASE_BACKGROUND_MODE = \"always\"（建窗口之前）\n");
    }
    if (group == 2 || group == 3) {
        g_watch_enabled = true;
        std::printf("    将在建窗口后装事件观察者（回调里清屏）\n");
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow("expose probe", 900, 600,
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

    if (group == 2 || group == 3) {
        if (!SDL_AddEventWatch(event_watch, &g_stats)) {
            std::printf("    SDL_AddEventWatch 失败：%s\n", SDL_GetError());
        } else {
            std::printf("    已装事件观察者\n");
        }
    }
    std::printf("\n");

    // 基线：先画一帧，确认上下文可用
    {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, pw, ph);
        glClearColor(0.0f, 0.0f, 0.25f, 1.0f);   // 深蓝：主循环画的
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(win);
    }

    std::printf("    颜色约定：主循环画的 = **深蓝**，回调画的 = **绿**\n");
    std::printf("    统计开始（拖窗口时看有没有卡顿、有没有白条）...\n\n");

    const Uint64 t0 = SDL_GetTicks();
    const Uint64 t_end = t0 + static_cast<Uint64>(seconds * 1000.0);
    int frames = 0;
    int pump_calls = 0;
    unsigned long long pump_blocked_ms = 0;   // 单次 PollEvent 循环超过 50ms 就算"阻塞"

    while (SDL_GetTicks() < t_end) {
        const Uint64 a = SDL_GetTicks();

        SDL_Event e;
        int n = 0;
        while (SDL_PollEvent(&e)) {
            ++n;
            if (e.type == SDL_EVENT_QUIT) { /* 继续，靠时间结束 */ }
        }
        ++pump_calls;
        {
            const unsigned long long took = SDL_GetTicks() - a;
            if (took > 50) { pump_blocked_ms += took; }
        }

        // 主循环画一帧（深蓝）
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, pw, ph);
        glClearColor(0.0f, 0.0f, 0.25f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(win);

        ++frames;
        SDL_Delay(4);
    }

    // 只统计"后半段"的帧率（避开启动）
    const double secs = static_cast<double>(SDL_GetTicks() - t0) / 1000.0;

    std::printf("=============================================================\n");
    std::printf("【小结】组 %d：%s\n", group, gname);
    std::printf("    运行时长               = %.1f 秒\n", secs);
    std::printf("    主循环帧数             = %d   （≈ %.1f fps）\n", frames,
                static_cast<double>(frames) / (secs > 0 ? secs : 1.0));
    std::printf("    PollEvent 调用次数     = %d\n", pump_calls);
    std::printf("    「单次泵 > 50ms」累计  = %llu ms   ← 这就是「拖拽期间被阻塞」的量\n",
                pump_blocked_ms);
    std::printf("\n");
    std::printf("    回调总次数             = %d\n", g_stats.cb_total.load());
    std::printf("    WINDOW_EXPOSED 次数    = %d\n", g_stats.exposed.load());
    std::printf("      其中 live-resize     = %d   （data1 != 0）\n", g_stats.exposed_live.load());
    std::printf("    回调里成功清屏         = %d\n", g_stats.redraws.load());
    std::printf("    ⚠️ 回调**不在主线程**  = %d   ← 若 > 0，则「回调里调 GL」是**未定义行为**\n",
                g_stats.off_thread.load());
    std::printf("    回调累计耗时           = %.2f ms\n",
                static_cast<double>(g_stats.cb_ns.load()) / 1000000.0);
    std::printf("\n");

    std::printf("【怎么读】\n");
    std::printf("  · 若「回调不在主线程」> 0 ⇒ **方案①（观察者里重画）不可用**（除非改造）\n");
    std::printf("  · 若「回调里成功清屏」 > 0 且你能看到**绿色**一闪 ⇒ 方案①**真的在补画**\n");
    std::printf("  · 对比各组「主循环帧数」与「泵阻塞累计」 ⇒ 拖拽期间哪一组更「跟得上」\n");
    std::printf("  · 而**白条是否消失**要看你的肉眼（探针只能给这些量）\n");
    std::printf("=============================================================\n");

    if (group == 2 || group == 3) {
        SDL_RemoveEventWatch(event_watch, &g_stats);
    }
    g_win = nullptr;
    g_watch_enabled = false;

    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

// =============================================================================
//  编译与运行（MSYS2 UCRT64）
//
//  cd /c/Users/vqwjq/Documents/LuaWAG/LuaWAG-Vanilla
//  g++ -std=c++20 -O1 -g -IC:/msys64/ucrt64/include sdl3_expose_probe.cpp \
//      -o bin/sdl3_expose_probe.exe -LC:/msys64/ucrt64/lib -lSDL3 -lopengl32
//
//  ./bin/sdl3_expose_probe.exe 0 20 > expose0.txt 2>&1; cat expose0.txt
//  ./bin/sdl3_expose_probe.exe 1 20 > expose1.txt 2>&1; cat expose1.txt
//  ./bin/sdl3_expose_probe.exe 2 20 > expose2.txt 2>&1; cat expose2.txt
//  ./bin/sdl3_expose_probe.exe 3 20 > expose3.txt 2>&1; cat expose3.txt
// =============================================================================
