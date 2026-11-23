// =============================================================================
//  sdl3_frameorder_probe —— 阶段 1 与阶段 7 之间，窗口尺寸会不会变？
//
//  ============================================================================
//  【为什么问这个（第 0 步读代码得到的线索）】
//      引擎 `frame.cpp` 的阶段顺序是：
//          阶段 1  stage_frame_begin    ← 取 **DIP 尺寸**（:246）⇒ recompute(视口)（:279）
//          阶段 3  stage_event_consume  ← **泵事件**（窗口可能在这里变）
//          阶段 4～6  输入 / 逻辑（脚本也可能调 SetWindowSize）
//          阶段 7  stage_render_begin   ← **另取 像素尺寸**（:474）+ 清屏 + glViewport/glScissor
//          阶段 12 呈现
//      ⇒ ⇒ 于是存在一个窗口期：**阶段 1 之后、阶段 7 之前**
//         · 若窗口在这段里变大 ⇒ 阶段 7 取到的**像素尺寸已是新的**（清屏够），
//           而**视口矩形**来自阶段 1 的旧 DIP 尺寸 ⇒ **视野几何是旧的**（内容偏，但不留白条）
//         · 若窗口在**阶段 7 之后**变大 ⇒ **本帧清屏用的是旧尺寸**
//           ⇒ 新露出的那一条**没被清过** ⇒ **右侧一条竖向白条**（高度没变 ⇒ 上下不留条）
//           ⇒ 而下一帧阶段 1 读到新尺寸就补上了 ⇒ **只出现一帧**
//      ⇒ 本探针就是去测"这两种情形到底会不会发生"，以及**发生的确切时刻**。
//  ============================================================================
//
//  【本探针怎么测】
//      复刻上面那个顺序，逐帧记录：
//          size_L   = 阶段 1 读的 SDL_GetWindowSize
//          size_7   = 阶段 7 再读的 SDL_GetWindowSize
//          pix_7    = 阶段 7 读的 SDL_GetWindowSizeInPixels
//          evt_3    = 阶段 3 泵到的尺寸事件数（RESIZED / PIXEL_SIZE_CHANGED）
//      ⇒ 报告**每一个"两个时刻不一致"的帧** ⇒ 那就是白条会在的那一帧
//
//  【三种触发尺寸变化的方式（都要试，因为它们对应不同的真实场合）】
//      ① **自己改**：每 N 帧在**帧末**（阶段 12 之后）调一次 SDL_SetWindowSize
//         ⇒ 可复现，且正好复刻"脚本在钩子里改窗口"这一路
//      ② **人手拖边**：由使用者拖窗口（探针只记录）
//      ③ **启动时**：窗口刚创建后的头几帧（这可能是"第一次启动"那一例的来源）
//
//  【它不进引擎源码、不进 CMake】
//  编译与运行见本文件末尾。
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>
#include <string>

namespace {

struct frame_record {
    int index = 0;
    int w_L = 0, h_L = 0;          // 阶段 1
    int w_7 = 0, h_7 = 0;          // 阶段 7 的窗口坐标
    int p_7 = 0, q_7 = 0;          // 阶段 7 的像素尺寸
    int evt = 0;                   // 阶段 3 泵到的尺寸事件数
    bool mismatch = false;         // size_L != size_7
    bool pix_ne_L = false;         // pix_7 != size_L
};

/// 复刻引擎的两段清屏（① 清黑到 clear_w x clear_h，② 清蓝到给定矩形）
void two_stage_clear(int clear_w, int clear_h, int vx, int vy, int vw, int vh) {
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, clear_w, clear_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (vw > 0 && vh > 0) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(vx, vy, vw, vh);
        glViewport(vx, vy, vw, vh);
        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    }
}

} // namespace

int main(int argc, char** argv) {
    int w0 = 900, h0 = 600;
    int resize_every = 40;             // 每 40 帧自己改一次尺寸（0 = 不改）
    int grow = 120;                    // 每次加宽多少
    if (argc >= 3) { w0 = std::atoi(argv[1]); h0 = std::atoi(argv[2]); }
    if (argc >= 4) { resize_every = std::atoi(argv[3]); }
    if (argc >= 5) { grow = std::atoi(argv[4]); }

    std::printf("=============================================================\n");
    std::printf(" 阶段顺序探针：阶段 1 与阶段 7 之间窗口会不会变\n");
    std::printf(" 初始 = %d x %d，每 %d 帧加宽 %d（0 = 不改，只记录）\n",
                w0, h0, resize_every, grow);
    std::printf(" 同时请**用手拖一下窗口边缘**，看是否出现 mismatch 帧\n");
    std::printf("=============================================================\n\n");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("frameorder probe", w0, h0,
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

    std::printf("    %-6s %-12s %-12s %-12s %-6s %s\n",
                "帧", "阶段1尺寸", "阶段7尺寸", "阶段7像素", "事件", "判定");
    std::printf("    %-6s %-12s %-12s %-12s %-6s %s\n",
                "----", "----------", "----------", "----------", "----", "----");

    int mismatch_frames = 0;
    int pix_ne_frames = 0;
    int shown = 0;
    const int max_show = 80;
    bool quit = false;
    int frame = 0;

    // 前 10 帧无条件打印（启动那几帧最可疑）
    while (!quit && frame < 1200) {
        // ---------------- 阶段 3 在阶段 1 之后，所以这里先做阶段 1 ----------------
        frame_record r{};
        r.index = frame;

        // ---- 阶段 1：取 DIP 尺寸 ----
        SDL_GetWindowSize(win, &r.w_L, &r.h_L);
        // （引擎在这里还会 recompute 视口；本探针不需要几何，只要尺寸）

        // ---- 阶段 3：泵事件 ----
        int evt = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { quit = true; }
            if (e.type == SDL_EVENT_WINDOW_RESIZED ||
                e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) { ++evt; }
        }
        r.evt = evt;

        // ---- 阶段 7：再取窗口尺寸与像素尺寸（引擎在 frame.cpp:474 取像素尺寸）----
        SDL_GetWindowSize(win, &r.w_7, &r.h_7);
        SDL_GetWindowSizeInPixels(win, &r.p_7, &r.q_7);

        r.mismatch = (r.w_L != r.w_7) || (r.h_L != r.h_7);
        r.pix_ne_L = (r.p_7 != r.w_L) || (r.q_7 != r.h_L);
        if (r.mismatch) { ++mismatch_frames; }
        if (r.pix_ne_L) { ++pix_ne_frames; }

        // ---- 清屏（复刻引擎：清到**阶段 7 取的像素尺寸**，视野用**阶段 1 的尺寸**）----
        two_stage_clear(r.p_7, r.q_7, 0, 0, r.w_L, r.h_L);

        // ---- 呈现 ----
        SDL_GL_SwapWindow(win);

        // ---- 需要时打印 ----
        const bool interesting = r.mismatch || r.pix_ne_L || evt > 0 || frame < 10;
        if (interesting && shown < max_show) {
            char szL[32], sz7[32], szP[32];
            std::snprintf(szL, sizeof(szL), "%dx%d", r.w_L, r.h_L);
            std::snprintf(sz7, sizeof(sz7), "%dx%d", r.w_7, r.h_7);
            std::snprintf(szP, sizeof(szP), "%dx%d", r.p_7, r.q_7);
            std::printf("    %-6d %-12s %-12s %-12s %-6d %s\n",
                        r.index, szL, sz7, szP, r.evt,
                        r.mismatch ? "**不一致**"
                                   : (r.pix_ne_L ? "**像素!=阶段1**" : "一致"));
            ++shown;
        }

        // ---- 帧末（阶段 12 之后）：需要时自己改尺寸 ----
        if (resize_every > 0 && frame > 0 && (frame % resize_every) == 0) {
            int cw = 0, ch = 0;
            SDL_GetWindowSize(win, &cw, &ch);
            const int nw = cw + grow;
            std::printf("    [帧末] 自己改尺寸：%d -> %d\n", cw, nw);
            SDL_SetWindowSize(win, nw, ch);
        }

        ++frame;
        SDL_Delay(10);
    }

    std::printf("\n=============================================================\n");
    std::printf("【小结】共 %d 帧\n", frame);
    std::printf("    阶段1 与 阶段7 尺寸**不一致**的帧数：%d\n", mismatch_frames);
    std::printf("    阶段7 像素尺寸 != 阶段1 尺寸的帧数：%d\n", pix_ne_frames);
    std::printf("\n");

    if (mismatch_frames > 0) {
        std::printf("    ⇒ ✅ **路径存在**：窗口确实会在「阶段 1 之后、阶段 7 之前」改变尺寸\n");
        std::printf("       ⇒ 那几帧里，**清屏范围与视野几何来自两个不同的测量瞬间**\n");
        std::printf("       ⇒ 而若窗口是在**阶段 7 之后**变的，多出来的那一条就**没被清过**\n");
        std::printf("          ⇒ 那正是「右侧一条竖向白条」的形态（高度没变 ⇒ 只留右侧一条）\n");
        std::printf("       ⇒ 修法：**把像素尺寸也在阶段 1 取一次快照**，与 DIP 尺寸同一时刻\n");
        std::printf("          （这正是那段代码已经在用的原则：本帧常量只在阶段 1 取一次；\n");
        std::printf("            而现在像素尺寸破了这条原则 —— 它在阶段 7 又取了一次）\n");
    } else if (pix_ne_frames > 0) {
        std::printf("    ⇒ ⚠️ 窗口尺寸本身没变，而**像素尺寸与阶段 1 的尺寸不相等**\n");
        std::printf("       ⇒ 那是另一类问题（例如 DPI 缩放变化）⇒ 把上面的表发回来\n");
    } else {
        std::printf("    ⇒ ❌ 本次运行**没有**观察到不一致 ⇒ 我的假设在本次条件下**未被证实**\n");
        std::printf("       ⇒ 请试：① 运行中**用手拖窗口边缘**（本探针只记录）\n");
        std::printf("                ② 把「每 N 帧自己改尺寸」调小（例如第 3 个参数给 5）\n");
        std::printf("                ③ 试 0（完全不改尺寸），看**启动那几帧**是否本来就不一致\n");
    }
    std::printf("=============================================================\n");

    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

// =============================================================================
//  编译与运行（MSYS2 UCRT64）
//
//  cd /c/Users/vqwjq/Documents/LuaWAG/LuaWAG-Vanilla
//  g++ -std=c++20 -O1 -g -IC:/msys64/ucrt64/include sdl3_frameorder_probe.cpp \
//      -o bin/sdl3_frameorder_probe.exe -LC:/msys64/ucrt64/lib -lSDL3 -lopengl32
//
//  ./bin/sdl3_frameorder_probe.exe > order.txt 2>&1; cat order.txt
//  ./bin/sdl3_frameorder_probe.exe 900 600 5 120 > order2.txt 2>&1; cat order2.txt
//  ./bin/sdl3_frameorder_probe.exe 900 600 0 0   > order3.txt 2>&1; cat order3.txt
// =============================================================================
