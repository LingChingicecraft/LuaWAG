// =============================================================================
//  sdl3_whitebar_probe —— 复现并定位"窗口右侧一条竖向白条"
//
//  【现象（使用者报告）】
//      "第一次和最后一次引擎启动的窗口右侧一定距离，存在竖向上下贯通的白条。"
//      ⇒ 三个特征：① **竖向、上下贯通** ② 在**右侧、距边一定距离**
//                   ③ 只出现在**第一 / 最后一次**（与尺寸变化相关）
//
//  【一个相容的机理假设（本探针就是去证实/否证它）】
//      我们的 begin_frame 做**两段清屏**（renderer.cpp:176-190 与 233-257）：
//          ① 关 scissor，glViewport(0,0,fb_w,fb_h)，清**黑**（信箱区）
//          ② 开 scissor，清**蓝**（render 区）
//      ⇒ **两段都只清到 (fb_w, fb_h)**，而它来自"平台报的像素尺寸"。
//      ⇒ 若某一帧"我们以为的尺寸"**小于窗口**（例如窗口刚变大、而尺寸还没追上来），
//         那么窗口右边那一条就**从没被清过** ⇒ 里面留着**上一帧 / 驱动初始值**
//         ⇒ 而"白"正是常见的初始值 ⇒ **一条竖向贯通的条**，位置与宽度
//           恰好等于"窗口宽度 − 我们清到的宽度"
//
//  【本探针怎么判】
//      每个变体按 same 顺序做：
//          ① 把窗口尺寸改成新的（触发"尺寸变化"这件事）
//          ② 用**指定的清屏范围**画一帧
//          ③ **两色差分**：同一个范围清红 vs 清绿，在横跨窗口的若干 x 处各读一点
//             · 值**随颜色改变** ⇒ 那个像素**确实在我们清屏范围内**
//             · 值**不变**       ⇒ 那个像素**没被我们清到** ⇒ 白条就在那儿
//          ④ 报出"**被清到的最大 x**" ⇒ 与窗口宽度对照
//
//  【三个变体（这正是要对照的三条实现路）】
//      A：清到**旧尺寸**（= 复刻"尺寸没追上来"的那种帧）
//      B：清到**新尺寸**（= 我们现在实际做的）
//      C：清到**比窗口大一圈的候选范围**（= 建议的修法，若 A/B 都出现白条）
//
//  【它不进引擎源码、不进 CMake】
//  编译与运行见本文件末尾。
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>

namespace {

void read_point(int x, int y, unsigned char out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

/// 复刻引擎的两段清屏（①清黑到 clear_w x clear_h，②清蓝到 box）。
void frame_two_stage_clear(int clear_w, int clear_h,
                           int vx, int vy, int vw, int vh) {
    // 第一段：关 scissor，清黑（信箱区）
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, clear_w, clear_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // 第二段：开 scissor，清蓝（render 区）
    if (vw > 0 && vh > 0) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(vx, vy, vw, vh);
        glViewport(vx, vy, vw, vh);
        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    }
}

/// 两色差分探针：返回"被清到的最大 x"，同时把每个采样点的结果打到 stdout。
/// 用来判断"窗口这一条有没有被清过"。
int probe_cleared_extent(int clear_w, int clear_h,
                         int vx, int vy, int vw, int vh,
                         const int* xs, int n_xs, int y) {
    unsigned char red[64][4] = {};
    unsigned char grn[64][4] = {};
    const int n = (n_xs > 64) ? 64 : n_xs;

    // 清红
    frame_two_stage_clear(clear_w, clear_h, vx, vy, vw, vh);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, clear_w, clear_h);
    glScissor(0, 0, clear_w, clear_h);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glFinish();
    for (int i = 0; i < n; ++i) { read_point(xs[i], y, red[i]); }

    // 清绿
    frame_two_stage_clear(clear_w, clear_h, vx, vy, vw, vh);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, clear_w, clear_h);
    glScissor(0, 0, clear_w, clear_h);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glFinish();
    for (int i = 0; i < n; ++i) { read_point(xs[i], y, grn[i]); }

    int last_following = -1;
    std::printf("        %-8s %-14s %-14s %s\n", "x", "清红后", "清绿后", "被清到？");
    for (int i = 0; i < n; ++i) {
        const bool follows = (red[i][0] != grn[i][0]) ||
                             (red[i][1] != grn[i][1]) ||
                             (red[i][2] != grn[i][2]);
        std::printf("        %-8d %-14d %-14d %s\n", xs[i], red[i][0] + red[i][1] + red[i][2],
                    grn[i][0] + grn[i][1] + grn[i][2], follows ? "是" : "**否**");
        if (follows) { last_following = xs[i]; }
    }
    return last_following;
}

} // namespace

int main(int argc, char** argv) {
    int w0 = 1280, h0 = 720;
    int w1 = 1500, h1 = 720;          // 故意加宽（白条是右侧的，所以要**变宽**）
    if (argc >= 5) {
        w0 = std::atoi(argv[1]); h0 = std::atoi(argv[2]);
        w1 = std::atoi(argv[3]); h1 = std::atoi(argv[4]);
    }

    std::printf("=============================================================\n");
    std::printf(" 白条探针：清屏范围 vs 窗口，谁少了一块\n");
    std::printf(" 初始 = %d x %d     之后改成 = %d x %d\n", w0, h0, w1, h1);
    std::printf(" 判据：两色差分 —— 像素是否随清屏颜色变化\n");
    std::printf("=============================================================\n\n");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("whitebar probe", w0, h0,
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
    SDL_Delay(400);

    // ---------------------------------------------------------------- 先画一帧正常内容，制造"上一帧"
    int cur_pix_w = 0, cur_pix_h = 0;
    SDL_GetWindowSizeInPixels(win, &cur_pix_w, &cur_pix_h);
    std::printf("【零】先画一帧正常内容（把整屏清成**白**，模拟「上一帧残留 / 初始值」）\n");
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, cur_pix_w, cur_pix_h);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(win);
    SDL_Delay(200);
    std::printf("     尺寸 = %d x %d\n\n", cur_pix_w, cur_pix_h);

    // ---------------------------------------------------------------- 三个变体
    // 采样点：横跨"旧宽度 .. 新宽度"那一段（白条若存在就在这儿）
    const int xs[] = {
        w0 - 2,            // 旧宽度内（应当被清到）
        w0 + 1,            // 刚过旧宽度
        (w0 + w1) / 2,     // 中间
        w1 - 2,            // 接近新宽度
        w1,                // 新宽度处（一般越界）
    };
    const int n_xs = static_cast<int>(sizeof(xs) / sizeof(xs[0]));

    struct variant { const char* name; int clear_w; int clear_h; const char* note; };
    const variant variants[] = {
        { "A 清到**旧尺寸**", w0, h0, "复刻「尺寸还没追上来」的那一帧" },
        { "B 清到**新尺寸**", w1, h1, "我们现在实际做的" },
        { "C 清到**比窗口大一圈**", 4096, 4096, "建议的修法（下界 0,0 起，往右/上多清）" },
    };

    for (int v = 0; v < 3; ++v) {
        std::printf("【变体 %s】清屏范围 = %d x %d\n", variants[v].name,
                    variants[v].clear_w, variants[v].clear_h);
        std::printf("    （%s）\n", variants[v].note);

        // 先把整屏清白（= 制造"残留白"），再改尺寸，然后用本变体画一帧
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, cur_pix_w, cur_pix_h);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(win);

        SDL_SetWindowSize(win, w1, h1);
        SDL_PumpEvents();
        SDL_Delay(120);                      // 给平台一点时间；白条正是"这段窗口期"的产物

        int now_w = 0, now_h = 0;
        SDL_GetWindowSizeInPixels(win, &now_w, &now_h);
        std::printf("    SDL 报的像素尺寸 = %d x %d\n", now_w, now_h);

        // render 区（字母箱）就取整个窗口，简化
        const int last = probe_cleared_extent(variants[v].clear_w, variants[v].clear_h,
                                              0, 0, now_w, now_h,
                                              xs, n_xs, 0);
        std::printf("    ⇒ 被清到的**最大 x** = %d\n", last);
        if (last >= now_w - 1) {
            std::printf("    ⇒ ✅ 整个窗口宽度都被清到了 ⇒ **不会出现白条**\n");
        } else if (last >= 0) {
            std::printf("    ⇒ ⚠️ **只清到 %d，而窗口宽 %d** ⇒ 右侧 %d 像素没被清\n",
                        last, now_w, now_w - 1 - last);
            std::printf("       ⇒ 那一条就是白条所在之处\n");
        } else {
            std::printf("    ⇒ ⚠️ **一个采样点都没被清到** ⇒ 清屏范围与窗口不相交？\n");
        }

        SDL_GL_SwapWindow(win);
        SDL_Delay(400);
        std::printf("\n");
    }

    // ---------------------------------------------------------------- 可见性提示
    std::printf("【提示】现在窗口里显示的应当是**最后一帧**的样子。\n");
    std::printf("    若你在窗口右侧看到一条竖向白条 ⇒ 把它与上面 C 变体的结论对照：\n");
    std::printf("      · 若 C 变体报「整个宽度都被清到」而白条仍在 ⇒ 白条不是清屏范围造成的\n");
    std::printf("      · 若 A/B 报「只清到 X」而 C 报「都清到」 ⇒ 机理成立，修法就是清到大一圈\n");
    std::printf("\n");
    std::printf("    你也可以**边看边按回车**：每按一次，本程序会把整屏清成白再清成黑，\n");
    std::printf("    便于对照白条是否随清屏消失。\n");
    std::printf("    按 ESC 或等 30 秒退出。\n");

    {
        const Uint64 deadline = SDL_GetTicks() + 30000;
        bool quit = false;
        while (!quit && SDL_GetTicks() < deadline) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) { quit = true; }
                if (e.type == SDL_EVENT_KEY_DOWN) {
                    // SDL3 的键码在 e.key.key
                    if (e.key.key == SDLK_ESCAPE) { quit = true; }
                    else if (e.key.key == SDLK_RETURN) {
                        int pw = 0, ph = 0;
                        SDL_GetWindowSizeInPixels(win, &pw, &ph);
                        glDisable(GL_SCISSOR_TEST);
                        glViewport(0, 0, pw, ph);
                        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
                        glClear(GL_COLOR_BUFFER_BIT);
                        SDL_GL_SwapWindow(win);
                        SDL_Delay(300);
                        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                        glClear(GL_COLOR_BUFFER_BIT);
                        SDL_GL_SwapWindow(win);
                        std::printf("    [清屏一次] 窗口 %d x %d\n", pw, ph);
                    }
                }
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
//  g++ -std=c++20 -O1 -g -IC:/msys64/ucrt64/include sdl3_whitebar_probe.cpp \
//      -o bin/sdl3_whitebar_probe.exe -LC:/msys64/ucrt64/lib -lSDL3 -lopengl32
//
//  ./bin/sdl3_whitebar_probe.exe > whitebar.txt 2>&1; cat whitebar.txt
//  ./bin/sdl3_whitebar_probe.exe 1280 720 1500 720
// =============================================================================
