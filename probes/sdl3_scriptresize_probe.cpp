// =============================================================================
//  sdl3_scriptresize_probe —— 只做"脚本式改尺寸"：这条路会不会出问题？
//
//  ============================================================================
//  【为什么要单独测这一条】
//      我们已经确认：**拖拽**窗口边缘时会长时间阻塞事件泵（20 秒里阻塞 3.5 秒）
//      ⇒ 帧发不出来 ⇒ 白条。
//      而现在的方向是：**禁用拖拽**，窗口尺寸改为**由脚本请求**（分辨率预设 / 菜单）。
//      ⇒ 那就必须先回答：**"脚本式改尺寸"这一条路上，有没有同样的问题？**
//         ① 事件泵会不会被阻塞？
//         ② 同一帧内会不会出现"阶段 1 与阶段 7 尺寸不一致"？
//         ③ 会不会收到 `SDL_EVENT_WINDOW_EXPOSED`？其中的 `data1` 是 0 还是 1？
//         ④ 代价是多少（与"普通帧的 SwapWindow"比）？
//      ⇒ 这四条共同决定：**那段"事件观察者补丁"还要不要留**。
//
//  【它怎么模拟"脚本改尺寸"】
//      阶段顺序照引擎的样子：
//          阶段 1 读尺寸（对应 frame.cpp:246）
//          阶段 3 泵事件（对应 stage_event_consume）
//          阶段 7 清屏 + Swap（对应 stage_render_begin + present）
//          **帧末**：若该换档了 ⇒ `SDL_SetWindowSize(下一个预设)`（对应阶段 11 的待生效值）
//      ⇒ 预设表在两个尺寸之间来回切，模拟"分辨率预设"的用法。
//
//  【三组（每次运行跑一组）】
//      0 = 改尺寸后**什么都不做**
//      1 = 改尺寸后立刻 **`SDL_SyncWindow`**
//      2 = 改尺寸后立刻 **`SDL_SyncWindow` + `SDL_PumpEvents`**
//      ⇒ 组 0 与组 1/2 的差别就是"要不要同步"的答案。
//
//  【它同时给一个基准】非 resize 帧的 `SwapWindow` 耗时 ⇒
//      于是"改尺寸比普通帧贵多少"才有参照（否则无法判断贵不贵）。
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
    std::atomic<int> exposed_live{0};      // data1 != 0（live-resize）
    std::atomic<int> exposed_plain{0};     // data1 == 0（其它暴露）
    std::atomic<int> off_thread{0};
    std::atomic<int> resized_events{0};    // WINDOW_RESIZED / PIXEL_SIZE_CHANGED
    std::atomic<int> work_ns{0};
};

stats g_st;
SDL_Window* g_win = nullptr;

bool SDLCALL event_watch(void* userdata, SDL_Event* event) {
    stats* st = static_cast<stats*>(userdata);
    if (st == nullptr || event == nullptr) { return true; }
    st->cb_total.fetch_add(1);
    if (std::this_thread::get_id() != g_main_thread_id) { st->off_thread.fetch_add(1); }

    switch (event->type) {
    case SDL_EVENT_WINDOW_EXPOSED:
        st->exposed.fetch_add(1);
        if (event->window.data1 != 0) { st->exposed_live.fetch_add(1); }
        else { st->exposed_plain.fetch_add(1); }
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        st->resized_events.fetch_add(1);
        break;
    default:
        break;
    }
    return true;
}

unsigned long long now_ns() { return SDL_GetTicksNS(); }
double ms(unsigned long long ns) { return static_cast<double>(ns) / 1000000.0; }

} // namespace

int main(int argc, char** argv) {
    int group = 0;
    double seconds = 20.0;
    if (argc >= 2) { group = std::atoi(argv[1]); }
    if (argc >= 3) { seconds = std::atof(argv[2]); }

    g_main_thread_id = std::this_thread::get_id();

    const char* gname =
        (group == 0) ? "改尺寸后什么都不做"
      : (group == 1) ? "改尺寸后立刻 SDL_SyncWindow"
                     : "改尺寸后 SDL_SyncWindow + SDL_PumpEvents";

    // 两个"预设"（模拟分辨率档位）
    const int preset_a_w = 1280, preset_a_h = 720;
    const int preset_b_w = 1600, preset_b_h = 900;

    std::printf("=============================================================\n");
    std::printf(" 脚本式改尺寸探针\n");
    std::printf(" 组 %d：%s\n", group, gname);
    std::printf(" 预设 = %dx%d  <->  %dx%d，每 2 秒换一次档\n",
                preset_a_w, preset_a_h, preset_b_w, preset_b_h);
    std::printf(" 跑 %.1f 秒 —— 本探针**不需要你拖窗口**（请不要拖）\n", seconds);
    std::printf("=============================================================\n\n");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("scriptresize probe",
                                       preset_a_w, preset_a_h,
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

    SDL_AddEventWatch(event_watch, &g_st);

    // ---------------------------------------------------------------- 主循环
    const Uint64 t0 = SDL_GetTicks();
    const Uint64 t_end = t0 + static_cast<Uint64>(seconds * 1000.0);

    int frames = 0;
    int resize_count = 0;
    int mismatch_count = 0;
    int shown_mismatch = 0;
    unsigned long long pump_blocked_ms = 0;
    int pump_blocked_frames = 0;

    // 基准：非 resize 帧的 SwapWindow 耗时
    unsigned long long swap_total_ns = 0;
    unsigned long long swap_max_ns = 0;
    int swap_samples = 0;

    // resize 帧的三段耗时
    unsigned long long set_total_ns = 0, set_max_ns = 0;
    unsigned long long sync_total_ns = 0, sync_max_ns = 0;
    unsigned long long pump2_total_ns = 0, pump2_max_ns = 0;
    int sync_fail = 0;

    bool use_b = false;
    Uint64 next_switch = t0 + 2000;

    while (SDL_GetTicks() < t_end) {
        // ---- 阶段 1：读尺寸 ----
        int wL = 0, hL = 0;
        SDL_GetWindowSize(win, &wL, &hL);

        // ---- 阶段 3：泵事件（计时；> 50ms 视为阻塞）----
        const Uint64 pa = SDL_GetTicks();
        int ev = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { /* 靠时间结束 */ }
            if (e.type == SDL_EVENT_WINDOW_RESIZED ||
                e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) { ++ev; }
        }
        const unsigned long long pump_ms = SDL_GetTicks() - pa;
        if (pump_ms > 50) { pump_blocked_ms += pump_ms; ++pump_blocked_frames; }

        // ---- 阶段 7：清屏 + Swap（计时 Swap）----
        int p7 = 0, q7 = 0;
        SDL_GetWindowSizeInPixels(win, &p7, &q7);
        if (p7 > 0 && q7 > 0) {
            glDisable(GL_SCISSOR_TEST);
            glViewport(0, 0, p7, q7);
            glClearColor(0.0f, 0.0f, 0.25f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            const Uint64 sa = now_ns();
            SDL_GL_SwapWindow(win);
            const unsigned long long sn = now_ns() - sa;
            swap_total_ns += sn;
            if (sn > swap_max_ns) { swap_max_ns = sn; }
            ++swap_samples;
        }

        // ---- 判定：阶段 1 与阶段 7 是否不一致 ----
        int w7 = 0, h7 = 0;
        SDL_GetWindowSize(win, &w7, &h7);
        if (wL != w7 || hL != h7) {
            ++mismatch_count;
            if (shown_mismatch < 20) {
                std::printf("    [帧 %d] 阶段1 = %dx%d   阶段7 = %dx%d   像素 = %dx%d   事件 = %d\n",
                            frames, wL, hL, w7, h7, p7, q7, ev);
                ++shown_mismatch;
            }
        }

        // ---- 帧末：到点就换档（模拟阶段 11 的待应用值）----
        if (SDL_GetTicks() >= next_switch) {
            next_switch = SDL_GetTicks() + 2000;
            use_b = !use_b;
            const int tw = use_b ? preset_b_w : preset_a_w;
            const int th = use_b ? preset_b_h : preset_a_h;
            ++resize_count;

            const int before_w = wL, before_h = hL;
            const Uint64 a = now_ns();
            SDL_SetWindowSize(win, tw, th);
            const unsigned long long set_n = now_ns() - a;
            set_total_ns += set_n;
            if (set_n > set_max_ns) { set_max_ns = set_n; }

            unsigned long long sync_n = 0, pump2_n = 0;
            if (group >= 1) {
                const Uint64 b = now_ns();
                if (!SDL_SyncWindow(win)) { ++sync_fail; }
                sync_n = now_ns() - b;
                sync_total_ns += sync_n;
                if (sync_n > sync_max_ns) { sync_max_ns = sync_n; }
            }
            if (group >= 2) {
                const Uint64 c = now_ns();
                SDL_PumpEvents();
                pump2_n = now_ns() - c;
                pump2_total_ns += pump2_n;
                if (pump2_n > pump2_max_ns) { pump2_max_ns = pump2_n; }
            }

            int aw = 0, ah = 0, apw = 0, aph = 0;
            SDL_GetWindowSize(win, &aw, &ah);
            SDL_GetWindowSizeInPixels(win, &apw, &aph);
            std::printf("    [换档 #%d] 请求 %dx%d（原 %dx%d）\n", resize_count, tw, th, before_w, before_h);
            std::printf("        SetWindowSize = %.3f ms", ms(set_n));
            if (group >= 1) { std::printf("   SyncWindow = %.3f ms", ms(sync_n)); }
            if (group >= 2) { std::printf("   PumpEvents = %.3f ms", ms(pump2_n)); }
            std::printf("\n");
            std::printf("        改完后立刻读：窗口 = %dx%d   像素 = %dx%d   %s\n",
                        aw, ah, apw, aph,
                        (aw == tw && ah == th) ? "**已是请求值**" : "**还不是请求值**");
        }

        ++frames;
        SDL_Delay(4);
    }

    const double secs = static_cast<double>(SDL_GetTicks() - t0) / 1000.0;

    std::printf("\n=============================================================\n");
    std::printf("【小结】组 %d：%s\n", group, gname);
    std::printf("    运行时长                      = %.1f 秒\n", secs);
    std::printf("    主循环帧数                    = %d   （≈ %.1f fps）\n", frames,
                static_cast<double>(frames) / (secs > 0 ? secs : 1.0));
    std::printf("    换档次数                      = %d\n", resize_count);
    std::printf("\n");
    std::printf("    ① 事件泵阻塞                  = %llu ms（>50ms 的帧数 = %d）\n",
                pump_blocked_ms, pump_blocked_frames);
    std::printf("    ② 阶段1 != 阶段7              = %d 帧\n", mismatch_count);
    std::printf("    ③ WINDOW_EXPOSED              = %d 次\n", g_st.exposed.load());
    std::printf("        其中 live-resize(data1!=0) = %d\n", g_st.exposed_live.load());
    std::printf("        其中 其它(data1==0)        = %d\n", g_st.exposed_plain.load());
    std::printf("       WINDOW_RESIZED / PIXEL_SIZE = %d 次\n", g_st.resized_events.load());
    std::printf("       ⚠️ 回调不在主线程           = %d\n", g_st.off_thread.load());
    std::printf("\n");
    std::printf("    ④ 代价\n");
    if (swap_samples > 0) {
        std::printf("        SwapWindow（普通帧，%d 次）平均 = %.3f ms   最大 = %.3f ms   ← 基准\n",
                    swap_samples,
                    ms(swap_total_ns) / static_cast<double>(swap_samples), ms(swap_max_ns));
    }
    if (resize_count > 0) {
        std::printf("        SetWindowSize（%d 次）   平均 = %.3f ms   最大 = %.3f ms\n",
                    resize_count, ms(set_total_ns) / static_cast<double>(resize_count), ms(set_max_ns));
        if (group >= 1 && resize_count > 0) {
            std::printf("        SyncWindow（%d 次）      平均 = %.3f ms   最大 = %.3f ms   （失败 %d 次）\n",
                        resize_count, ms(sync_total_ns) / static_cast<double>(resize_count),
                        ms(sync_max_ns), sync_fail);
        }
        if (group >= 2 && resize_count > 0) {
            std::printf("        PumpEvents（%d 次）      平均 = %.3f ms   最大 = %.3f ms\n",
                        resize_count, ms(pump2_total_ns) / static_cast<double>(resize_count),
                        ms(pump2_max_ns));
        }
    }
    std::printf("\n");

    std::printf("【怎么读】\n");
    std::printf("  · ① 若显著大于 0 ⇒ **脚本路径也会阻塞** ⇒ 补丁仍需考虑\n");
    std::printf("  · ② 若 > 0 ⇒ 同帧两个尺寸 ⇒ 应当「帧末应用 + 一次同步」\n");
    std::printf("  · ③ 若有 live-resize ⇒ 说明程序化改尺寸也走那条路（补丁会真的被触发）\n");
    std::printf("        若全是 data1 == 0 ⇒ 那是「普通暴露」，补丁若只认 live-resize 就不会响应\n");
    std::printf("  · ④ 与基准比：改尺寸那一帧比普通帧贵多少\n");
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
//  g++ -std=c++20 -O1 -g -IC:/msys64/ucrt64/include sdl3_scriptresize_probe.cpp \
//      -o bin/sdl3_scriptresize_probe.exe -LC:/msys64/ucrt64/lib -lSDL3 -lopengl32
//
//  ./bin/sdl3_scriptresize_probe.exe 0 20 > sr0.txt 2>&1; cat sr0.txt
//  ./bin/sdl3_scriptresize_probe.exe 1 20 > sr1.txt 2>&1; cat sr1.txt
//  ./bin/sdl3_scriptresize_probe.exe 2 20 > sr2.txt 2>&1; cat sr2.txt
// =============================================================================
