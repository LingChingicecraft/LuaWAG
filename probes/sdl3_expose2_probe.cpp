// =============================================================================
//  sdl3_expose2_probe —— 白条解法的两处尚未实测的事
//
//  ============================================================================
//  【上一轮已经确认的（`sdl3_expose_probe`）】
//      · 拖拽期间 `SDL_PollEvent` **长时间阻塞**（20 秒里阻塞 3.6 秒）⇒ 帧发不出来
//      · 装事件观察者、在 `SDL_EVENT_WINDOW_EXPOSED` 回调里"清屏 + Swap"
//        ⇒ **白条与黑边都消失、实时**（使用者肉眼确认）
//      · 回调**全在主线程**（611/1108 次），一次"清屏 + Swap"约 **5.0 ms**
//  ============================================================================
//
//  【本探针要测的两件事（都是落地前必须知道的）】
//
//      Q1 **回调里能不能"贴上一帧的成品"**（而不是"清成某一色"）？
//         做法：主循环把成品渲染进一个 **FBO**，回调里把 FBO **blit** 到当前帧缓冲。
//         ⇒ 若可行，则拖拽时露出的那一条会是**上一帧的真实内容**（观感最好），
//           而不是黑条/纯色条。
//         ⇒ 而它的代价要测：**blit 一次 vs 清一次**，谁便宜。
//
//      Q2 **回调里 Swap 的时序**：回调可能在主循环**帧中**触发
//         （引擎里事件泵在阶段 3，而绘制在阶段 7 之后）
//         ⇒ 必须知道：此时 Swap 会不会与主循环的绘制打架（GL 错误？观感？）
//         ⇒ 本探针用一个"正在绘制中"的守卫来**量化**这件事（计数 + GL 错误数）。
//
//  【四组（每次运行跑一组）】
//      0 = 基线：回调里**什么都不做**（对照组）
//      1 = 回调里 **清屏 + Swap**（上一轮验证过的做法）
//      2 = 回调里 **blit 上一帧（FBO）+ Swap**   ← Q1
//      3 = 回调里 **只 blit，不 Swap**           ← 用于分离"blit 的代价"与"Swap 的代价"
//      4 = 回调里 清屏 + Swap，**并开启"帧中"守卫统计** ← Q2
//
//  【⚠️ GL 3.3 的 FBO 函数必须自己取】
//      实测：本机 `GL/gl.h` 里 **`glGenFramebuffers` / `glBindFramebuffer` /
//      `glFramebufferTexture2D` / `glCheckFramebufferStatus` / `glBlitFramebuffer`
//      以及它们用到的常量全都没有**（opengl32.dll 只导出 GL 1.1）
//      ⇒ 所以本探针自己声明常量 + 用 `SDL_GL_GetProcAddress` 取函数指针。
//      ⇒ 这同时**顺带验证了那条已定项**："GL 3.3 的函数必须自己取回来"。
//
//  【它不进引擎源码、不进 CMake】
//  编译与运行见本文件末尾。
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <atomic>
#include <cstdio>
#include <thread>

// -----------------------------------------------------------------------------
//  GL 3.3 里本机 gl.h **没有**的那些声明（实测确认过，见文件头说明）
// -----------------------------------------------------------------------------
#ifndef GL_FRAMEBUFFER
#  define GL_FRAMEBUFFER          0x8D40
#endif
#ifndef GL_READ_FRAMEBUFFER
#  define GL_READ_FRAMEBUFFER     0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#  define GL_DRAW_FRAMEBUFFER     0x8CA9
#endif
#ifndef GL_COLOR_ATTACHMENT0
#  define GL_COLOR_ATTACHMENT0    0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#  define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#  define GL_FRAMEBUFFER_BINDING  0x8CA6
#endif

typedef void (APIENTRY *PFN_glGenFramebuffers)(GLsizei, GLuint*);
typedef void (APIENTRY *PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *PFN_glCheckFramebufferStatus)(GLenum);
typedef void (APIENTRY *PFN_glDeleteFramebuffers)(GLsizei, const GLuint*);
typedef void (APIENTRY *PFN_glBlitFramebuffer)(GLint, GLint, GLint, GLint,
                                               GLint, GLint, GLint, GLint,
                                               GLbitfield, GLenum);

namespace {

PFN_glGenFramebuffers        p_glGenFramebuffers = nullptr;
PFN_glBindFramebuffer        p_glBindFramebuffer = nullptr;
PFN_glFramebufferTexture2D   p_glFramebufferTexture2D = nullptr;
PFN_glCheckFramebufferStatus p_glCheckFramebufferStatus = nullptr;
PFN_glDeleteFramebuffers     p_glDeleteFramebuffers = nullptr;
PFN_glBlitFramebuffer        p_glBlitFramebuffer = nullptr;

/// 用 SDL 取一个 GL 入口。
template <typename T>
bool load_gl(T& fn, const char* name) {
    fn = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    if (fn == nullptr) {
        std::printf("    !! SDL_GL_GetProcAddress(\"%s\") 失败\n", name);
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
//  观察者状态
// -----------------------------------------------------------------------------
std::thread::id g_main_thread_id;

struct stats {
    std::atomic<int> cb_total{0};
    std::atomic<int> exposed{0};
    std::atomic<int> exposed_live{0};
    std::atomic<int> did_work{0};          // 回调里成功做了事的次数
    std::atomic<int> off_thread{0};
    std::atomic<int> during_draw{0};       // 回调触发时"主循环正在绘制中"的次数
    std::atomic<unsigned long long> work_ns{0};   // 回调里那件事花掉的总时间
    std::atomic<unsigned long long> work_max_ns{0};
};

stats g_st;
SDL_Window* g_win = nullptr;
int g_group = 0;

GLuint g_fbo = 0;
GLuint g_fbo_tex = 0;
int g_fbo_w = 0, g_fbo_h = 0;
std::atomic<bool> g_in_draw{false};        // "主循环正在绘制中"的守卫

/// 保证 FBO 的尺寸 = 给定尺寸（变了就重建纹理）。
bool ensure_fbo(int w, int h) {
    if (w <= 0 || h <= 0) { return false; }
    if (g_fbo != 0 && g_fbo_w == w && g_fbo_h == h) { return true; }

    if (g_fbo_tex != 0) { glDeleteTextures(1, &g_fbo_tex); g_fbo_tex = 0; }
    glGenTextures(1, &g_fbo_tex);
    glBindTexture(GL_TEXTURE_2D, g_fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (g_fbo == 0) { p_glGenFramebuffers(1, &g_fbo); }
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbo_tex, 0);
    const GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);

    g_fbo_w = w;
    g_fbo_h = h;
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        std::printf("    !! FBO 不完整（状态 0x%04X）⇒ blit 组不可用\n",
                    static_cast<unsigned>(st));
        return false;
    }
    return true;
}

/// 把 FBO 的整张内容 blit 到默认帧缓冲（尺寸取当前帧缓冲）。
void blit_fbo_to_window(int dst_w, int dst_h) {
    if (g_fbo == 0) { return; }
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    p_glBlitFramebuffer(0, 0, g_fbo_w, g_fbo_h,
                        0, 0, dst_w, dst_h,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool SDLCALL event_watch(void* userdata, SDL_Event* event) {
    stats* st = static_cast<stats*>(userdata);
    if (st == nullptr || event == nullptr) { return true; }
    st->cb_total.fetch_add(1);

    if (event->type != SDL_EVENT_WINDOW_EXPOSED) { return true; }
    st->exposed.fetch_add(1);
    if (event->window.data1 != 0) { st->exposed_live.fetch_add(1); }

    if (g_win == nullptr) { return true; }
    if (std::this_thread::get_id() != g_main_thread_id) { st->off_thread.fetch_add(1); return true; }
    if (g_in_draw.load()) { st->during_draw.fetch_add(1); }        // Q2：帧中触发

    if (g_group == 0) { return true; }                             // 基线：什么都不做

    const unsigned long long t0 = SDL_GetTicksNS();
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_win, &pw, &ph);
    if (pw <= 0 || ph <= 0) { return true; }

    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, pw, ph);

    if (g_group == 1 || g_group == 4) {
        glClearColor(0.0f, 0.35f, 0.0f, 1.0f);                     // 绿
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(g_win);
    } else if (g_group == 2) {
        blit_fbo_to_window(pw, ph);                                // 上一帧的内容
        SDL_GL_SwapWindow(g_win);
    } else if (g_group == 3) {
        blit_fbo_to_window(pw, ph);                                // 只 blit，不 Swap
    }

    const unsigned long long dt = SDL_GetTicksNS() - t0;
    st->work_ns.fetch_add(dt);
    if (dt > st->work_max_ns.load()) { st->work_max_ns.store(dt); }
    st->did_work.fetch_add(1);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    int group = 2;
    double seconds = 20.0;
    if (argc >= 2) { group = std::atoi(argv[1]); }
    if (argc >= 3) { seconds = std::atof(argv[2]); }

    g_group = group;
    g_main_thread_id = std::this_thread::get_id();

    const char* gname =
        (group == 0) ? "基线（回调里什么都不做）"
      : (group == 1) ? "回调：清屏 + Swap"
      : (group == 2) ? "回调：blit 上一帧（FBO）+ Swap"
      : (group == 3) ? "回调：只 blit，不 Swap"
                     : "回调：清屏 + Swap，并统计「帧中触发」";

    std::printf("=============================================================\n");
    std::printf(" 白条解法 · 第二阶段探针\n");
    std::printf(" 组 %d：%s\n", group, gname);
    std::printf(" 跑 %.1f 秒 —— 请反复拖动窗口的右边缘与下边缘\n", seconds);
    std::printf("=============================================================\n\n");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }

    // 请求 3.3（FBO / blit 需要 3.0+）
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);

    SDL_Window* win = SDL_CreateWindow("expose2 probe", 900, 600,
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

    // 报出实际拿到的版本（顺便验证 3.3 是否真的到手）
    const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    std::printf("    GL 版本 = %s\n", ver ? ver : "(null)");
    std::printf("    厂商    = %s\n", vendor ? vendor : "(null)");
    std::printf("\n");

    // ---- 取 GL 3.3 的入口（本机 gl.h 里没有，必须自己取）----
    std::printf("    取 GL 3.3 入口（本机 GL/gl.h 里没有这些声明）:\n");
    bool ok = true;
    ok &= load_gl(p_glGenFramebuffers, "glGenFramebuffers");
    ok &= load_gl(p_glBindFramebuffer, "glBindFramebuffer");
    ok &= load_gl(p_glFramebufferTexture2D, "glFramebufferTexture2D");
    ok &= load_gl(p_glCheckFramebufferStatus, "glCheckFramebufferStatus");
    ok &= load_gl(p_glDeleteFramebuffers, "glDeleteFramebuffers");
    ok &= load_gl(p_glBlitFramebuffer, "glBlitFramebuffer");
    if (!ok) {
        std::printf("    ⇒ 有入口没取到 ⇒ 本组（需要 FBO 的）不可用\n");
    } else {
        std::printf("    ⇒ 全部取到 ✓（这顺带验证了那条已定项：GL 3.3 必须自己取）\n");
    }
    std::printf("\n");

    // ---- 建 FBO（组 2/3 用得到）----
    if (ok) {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        if (!ensure_fbo(pw, ph)) {
            std::printf("    ⇒ FBO 不可用 ⇒ 组 2/3 的结论不成立\n");
        } else {
            std::printf("    FBO 已建（%d x %d）\n\n", pw, ph);
        }
    }

    // ---- 装观察者 ----
    if (!SDL_AddEventWatch(event_watch, &g_st)) {
        std::printf("    SDL_AddEventWatch 失败：%s\n", SDL_GetError());
    } else {
        std::printf("    已装事件观察者\n\n");
    }

    std::printf("    颜色约定：主循环渲染内容 = **橙色旋转方块 + 深蓝底**\n");
    std::printf("              组 1/4 回调画的是 **绿**\n");
    std::printf("              组 2/3 回调贴的是 **上一帧**（应当看不出突变）\n\n");

    // ---------------------------------------------------------------- 主循环
    const Uint64 t0 = SDL_GetTicks();
    const Uint64 t_end = t0 + static_cast<Uint64>(seconds * 1000.0);
    int frames = 0;
    unsigned long long pump_blocked_ms = 0;
    GLenum first_gl_error = GL_NO_ERROR;
    int gl_error_count = 0;

    while (SDL_GetTicks() < t_end) {
        // ---- 阶段 3（引擎里事件泵在这里；本探针就是靠它触发回调）----
        const Uint64 a = SDL_GetTicks();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { /* 靠时间结束 */ }
        }
        {
            const unsigned long long took = SDL_GetTicks() - a;
            if (took > 50) { pump_blocked_ms += took; }
        }

        // ---- 阶段 7：渲染进 FBO（"成品"）----
        g_in_draw.store(true);
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        if (ok && ensure_fbo(pw, ph)) {
            p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
            glViewport(0, 0, pw, ph);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.0f, 0.0f, 0.35f, 1.0f);        // 深蓝底
            glClear(GL_COLOR_BUFFER_BIT);

            // 一个会动的橙色方块（用来判断"贴的是不是上一帧"）
            const float t = static_cast<float>(SDL_GetTicks() % 2000) / 2000.0f;
            const int n = 12;
            const int s = (pw < ph ? pw : ph) / 6;
            const int x0 = static_cast<int>(t * static_cast<float>(pw - s));
            const int y0 = ph / 2 - s / 2;
            glMatrixMode(GL_PROJECTION); glLoadIdentity();
            glOrtho(0.0, static_cast<double>(pw), 0.0, static_cast<double>(ph), -1.0, 1.0);
            glMatrixMode(GL_MODELVIEW); glLoadIdentity();
            glColor3f(1.0f, 0.6f, 0.1f);
            glBegin(GL_QUADS);
            for (int i = 0; i < n; ++i) {
                const int cx = x0 + i * 6;
                const int cy = y0 + i * 4;
                glVertex2i(cx, cy);
                glVertex2i(cx + s / 2, cy);
                glVertex2i(cx + s / 2, cy + s / 2);
                glVertex2i(cx, cy + s / 2);
            }
            glEnd();

            // ---- 贴到窗口（正常一帧）----
            blit_fbo_to_window(pw, ph);
        }
        SDL_GL_SwapWindow(win);
        g_in_draw.store(false);

        // 统计 GL 错误（只记第一个与总数）
        for (;;) {
            const GLenum err = glGetError();
            if (err == GL_NO_ERROR) { break; }
            if (first_gl_error == GL_NO_ERROR) { first_gl_error = err; }
            ++gl_error_count;
        }

        ++frames;
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
    std::printf("    ⚠️ 回调触发时「正在绘制中」 = %d   ← Q2 的关键数字\n",
                g_st.during_draw.load());
    std::printf("    回调累计耗时                = %.2f ms\n", total_ms);
    if (worked > 0) {
        std::printf("    回调单次平均                = %.3f ms\n", total_ms / static_cast<double>(worked));
    }
    std::printf("    回调单次最大                = %.3f ms\n",
                static_cast<double>(g_st.work_max_ns.load()) / 1000000.0);
    std::printf("\n");
    std::printf("    GL 错误数                   = %d", gl_error_count);
    if (gl_error_count > 0) {
        std::printf("   （第一个 = 0x%04X）", static_cast<unsigned>(first_gl_error));
    }
    std::printf("\n\n");

    std::printf("【怎么读】\n");
    std::printf("  · 组 1 vs 组 2 的「回调单次平均」 ⇒ **清屏 vs blit 谁便宜**\n");
    std::printf("  · 组 3（只 blit 不 Swap）与组 2 的差 ⇒ **Swap 占了多少**\n");
    std::printf("  · 组 4 的「正在绘制中」> 0 ⇒ **回调确实会在主循环帧中触发** ⇒");
    std::printf(" 需要守卫（本探针用 g_in_draw）\n");
    std::printf("  · 任何一组的 GL 错误数 > 0 ⇒ 那条做法有状态问题，要查\n");
    std::printf("=============================================================\n");

    SDL_RemoveEventWatch(event_watch, &g_st);
    g_win = nullptr;

    if (g_fbo != 0 && p_glDeleteFramebuffers != nullptr) { p_glDeleteFramebuffers(1, &g_fbo); }
    if (g_fbo_tex != 0) { glDeleteTextures(1, &g_fbo_tex); }

    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

// =============================================================================
//  编译与运行（MSYS2 UCRT64）
//
//  cd /c/Users/vqwjq/Documents/LuaWAG/LuaWAG-Vanilla
//  g++ -std=c++20 -O1 -g -IC:/msys64/ucrt64/include sdl3_expose2_probe.cpp \
//      -o bin/sdl3_expose2_probe.exe -LC:/msys64/ucrt64/lib -lSDL3 -lopengl32
//
//  ./bin/sdl3_expose2_probe.exe 0 20 > e2_base.txt   2>&1; cat e2_base.txt
//  ./bin/sdl3_expose2_probe.exe 1 20 > e2_clear.txt  2>&1; cat e2_clear.txt
//  ./bin/sdl3_expose2_probe.exe 2 20 > e2_blit.txt   2>&1; cat e2_blit.txt
//  ./bin/sdl3_expose2_probe.exe 3 20 > e2_blitns.txt 2>&1; cat e2_blitns.txt
//  ./bin/sdl3_expose2_probe.exe 4 20 > e2_guard.txt  2>&1; cat e2_guard.txt
// =============================================================================
