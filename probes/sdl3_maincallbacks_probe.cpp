// =============================================================================
//  sdl3_maincallbacks_probe.cpp —— **独立的最小对照实验**，不属于引擎源码。
//
//  【它回答一个具体问题】
//      SDL3 官方文档（SDL3/AppFreezeDuringDrag）说：
//        "Using [the callbacks from SDL_main.h], SDL_AppEvent and SDL_AppIterate
//         **should be called for you, even when being resized or moved**."
//
//      ⇒ 若这句在本机（SDL 3.4.18 / Windows）成立，那么"把主循环改成
//        SDL main callbacks"就能在拖拽期间继续跑帧，**不需要**第二个渲染入口。
//      ⇒ 若不成立，那条路（甲）直接作废，只能回到"事件回调里补跑"。
//
//  【它怎么回答】
//      每 250 ms 打一行：距上次的**迭代次数**、**事件数**、**窗口位置/尺寸**。
//      拖拽期间若迭代次数掉到 0 ⇒ 不成立；若照常在 ~60/s ⇒ 成立。
//
//  【为什么要带 SDL_Delay(16)】
//      callback 模式的循环没有内置节流 API（SDL_main.h 里没有 Delay/Interval），
//      真实引擎必须自己睡。若睡的那 16 ms 把 SDL 的循环占住、导致拖拽期间拿不到
//      回调，那"能跑"也只是纸面结论。⇒ 探针默认**带睡**，这才是真实形态。
//      （PROBE_SLEEP=0 可关掉，用来对照"不睡时"的密度。）
//
//  【用法】
//      g++ -std=c++20 -O1 -I/c/msys64/ucrt64/include sdl3_maincallbacks_probe.cpp
//          -o bin/sdl3_maincallbacks_probe.exe -lSDL3 -lopengl32 -luser32 -lgdi32
//      然后用鼠标**拖住标题栏**，看 stderr 的 per-sec 行。
// =============================================================================

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstdlib>

namespace {

SDL_Window* g_win = nullptr;
SDL_Renderer* g_rend = nullptr;

unsigned long long g_iter = 0;
unsigned long long g_events = 0;
unsigned long long g_last_iter = 0;
unsigned long long g_last_events = 0;

bool g_sleep = true;
unsigned int g_report_ms = 250;

/// 打一行"这段时间里发生了多少次迭代 / 多少条事件 / 窗口在哪"。
void report(const char* tag) {
    int wx = 0;
    int wy = 0;
    int ww = 0;
    int wh = 0;
    if (g_win != nullptr) {
        SDL_GetWindowPosition(g_win, &wx, &wy);
        SDL_GetWindowSize(g_win, &ww, &wh);
    }
    std::fprintf(stderr,
                 "[iter %s] iter=%llu (+%llu) ev=%llu (+%llu) win=(%d,%d) %dx%d\n",
                 tag, g_iter, g_iter - g_last_iter, g_events, g_events - g_last_events,
                 wx, wy, ww, wh);
    std::fflush(stderr);
    g_last_iter = g_iter;
    g_last_events = g_events;
}

} // namespace

SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv) {
    (void)appstate;
    (void)argc;
    (void)argv;

    const char* sleep_env = std::getenv("PROBE_SLEEP");
    g_sleep = !(sleep_env != nullptr && sleep_env[0] == '0');
    const char* rep = std::getenv("PROBE_REPORT_MS");
    if (rep != nullptr) {
        const int v = std::atoi(rep);
        if (v >= 50) {
            g_report_ms = static_cast<unsigned int>(v);
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[probe] SDL_Init 失败：%s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    g_win = SDL_CreateWindow("maincallbacks probe（拖我）", 900, 620, SDL_WINDOW_RESIZABLE);
    if (g_win == nullptr) {
        std::fprintf(stderr, "[probe] 建窗口失败：%s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    g_rend = SDL_CreateRenderer(g_win, nullptr);
    if (g_rend == nullptr) {
        std::fprintf(stderr, "[probe] 建渲染器失败：%s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    std::fprintf(stderr, "[probe] 就绪：sleep=%d report=%u ms（拖住标题栏看计数）\n",
                 g_sleep ? 1 : 0, g_report_ms);
    std::fflush(stderr);
    report("start");
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    (void)appstate;

    ++g_iter;

    SDL_SetRenderDrawColor(g_rend, 26, 58, 92, 255);
    SDL_RenderClear(g_rend);
    SDL_SetRenderDrawColor(g_rend, 220, 222, 240, 255);
    const SDL_FRect box{40.0f, 40.0f, 240.0f, 140.0f};
    SDL_RenderFillRect(g_rend, &box);
    SDL_RenderPresent(g_rend);

    static Uint64 last_report = 0;
    const Uint64 now = SDL_GetTicks();
    if (now - last_report >= g_report_ms) {
        last_report = now;
        report("tick");
    }

    if (g_sleep) {
        SDL_Delay(16);
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    (void)appstate;
    ++g_events;
    if (event != nullptr && event->type == SDL_EVENT_QUIT) {
        return SDL_APP_SUCCESS;
    }
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result) {
    (void)appstate;
    (void)result;
    report("quit");
    if (g_rend != nullptr) {
        SDL_DestroyRenderer(g_rend);
        g_rend = nullptr;
    }
    if (g_win != nullptr) {
        SDL_DestroyWindow(g_win);
        g_win = nullptr;
    }
    SDL_Quit();
}
