// =============================================================================
//  sdl3_order2_probe —— 白条：**逐字复刻引擎的两段清屏** + **受控复现**
//
//  ============================================================================
//  【前一版为什么测不出白条（必须记住）】
//      前一版把"第一段清屏的范围"按模式改成了 `clear_w`（旧宽度那一块）
//      ⇒ 那**等于把引擎的第一段改掉了** ⇒ "新露出的那一条"永远被清成青
//      ⇒ 于是永远看不到白条 ⇒ 那组数据**不能否证任何东西**（我收回当时的结论）
//
//      引擎真正的写法（renderer.cpp:187-190 与 246-254）：
//          第一段：glDisable(scissor); glViewport(0,0,**fb_w**,**fb_h**); 清信箱色
//                  ⇒ `fb_w/fb_h` = **阶段 7 取的像素尺寸**（frame.cpp:474）
//          第二段：glEnable(scissor); glScissor(vx, fb_h-(vy+vh), vw, vh); 清场景色
//                  ⇒ 那个矩形 = **阶段 1 算出**的视口（旧窗口尺寸，frame.cpp:279/476）
//      ⇒ **两个尺寸来自不同瞬间** —— 这才是问题的形状
//  ============================================================================
//
//  【本版怎么"必然复现"】
//      关键：任何一次尺寸读取都是**快照** ⇒ 读完再变大，快照就过期。
//      所以本探针在**阶段 7 之后**、**清屏之前**插入一个**受控的增长**：
//          阶段 7：读 pix_7 = SDL_GetWindowSizeInPixels()
//          阶段 7.5（新增）：SDL_SetWindowSize(当前 + Δ)   ← Δ = 1..8，像人手拖
//          清屏：第一段按**pix_7**⇒ 于是"新露出的 Δ 像素"没被清到
//      ⇒ 那 Δ 像素里留着什么，就是白条的**真身**。
//
//  【本版怎么"看清"那一条】
//      不再用"两色差分"那种间接判据（它在 sdl3_whitebar_probe 里自相矛盾过）。
//      改用**逐像素扫描**：横向扫一条线，把颜色变化的区间按 x 打出来；
//      并单独判定"x 在 render 区之外有没有既不是青也不是洋红的像素"。
//          第一段清**青(0,255,255)** · 第二段清**洋红(255,0,255)**
//      ⇒ 于是输出形如：
//            x  0..  899 : 洋红
//            x 900.. 1211 : 青
//            x1212.. 1214 : **白**  ← 这一条就是白条（未被任何一段清到）
//
//  【模式（同一组实验里就能看出哪个修法有效）】
//      0 = 引擎现状：第一段用 pix_7（快照）              ⇒ 期望出现白条
//      1 = 修法A：第一段用 max(阶段1, pix_7) + 余量       ⇒ 期望白条消失
//      2 = 修法B（同源）：像素尺寸也在阶段 1 取快照
//                          ⇒ 两个尺寸同为旧值 ⇒ 白条**仍在**（它治的是"两个真相"）
//      3 = 修法C：改尺寸后立刻 SDL_SyncWindow 再读像素尺寸
//                          ⇒ 快照不过期 ⇒ 期望白条消失
//
//  【它不进引擎源码、不进 CMake】
//  编译与运行见本文件末尾。
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>

namespace {

/// 判色：给出一个可读的名字
const char* classify(unsigned char r, unsigned char g, unsigned char b) {
    if (r < 20 && g < 20 && b < 20) { return "黑"; }
    if (r < 20 && g > 235 && b > 235) { return "青"; }
    if (r > 235 && g < 20 && b > 235) { return "洋红"; }
    if (r > 235 && g > 235 && b > 235) { return "**白**"; }
    if (r > 235 && g < 20 && b < 20) { return "**红**"; }
    if (r < 20 && g > 235 && b < 20) { return "绿"; }
    if (r < 20 && g < 20 && b > 235) { return "蓝"; }
    return "其它";
}

bool same_color(unsigned char ar, unsigned char ag, unsigned char ab,
                unsigned char br, unsigned char bg, unsigned char bb) {
    const int dr = static_cast<int>(ar) - static_cast<int>(br);
    const int dg = static_cast<int>(ag) - static_cast<int>(bg);
    const int db = static_cast<int>(ab) - static_cast<int>(bb);
    const int d = (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);
    return d <= 12;
}

/// 横向扫 y 那一行，把颜色变化的区间打出来（游程编码）。
void scan_row_runs(int y, int fb_w, int max_runs) {
    unsigned char prev[4] = {0, 0, 0, 0};
    glReadPixels(0, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, prev);
    int run_start = 0;
    int runs = 0;

    for (int x = 1; x < fb_w; ++x) {
        unsigned char c[4] = {0, 0, 0, 0};
        glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
        if (!same_color(c[0], c[1], c[2], prev[0], prev[1], prev[2])) {
            if (runs < max_runs) {
                std::printf("        x %5d..%5d : %s  (%d,%d,%d)\n",
                            run_start, x - 1, classify(prev[0], prev[1], prev[2]),
                            prev[0], prev[1], prev[2]);
            }
            ++runs;
            prev[0] = c[0]; prev[1] = c[1]; prev[2] = c[2];
            run_start = x;
        }
    }
    if (runs < max_runs) {
        std::printf("        x %5d..%5d : %s  (%d,%d,%d)\n",
                    run_start, fb_w - 1, classify(prev[0], prev[1], prev[2]),
                    prev[0], prev[1], prev[2]);
    } else {
        std::printf("        ...（段数超过 %d，已截断）\n", max_runs);
    }
}

/// 复刻引擎的两段清屏。
///   clear_w/clear_h = 第一段的范围（引擎里 = **阶段 7 的像素尺寸**）
///   vx/vy/vw/vh     = 第二段的 scissor（引擎里 = **阶段 1 算出的视口**）
void two_stage_clear(int clear_w, int clear_h, int vx, int vy, int vw, int vh) {
    // 第一段：信箱色 = **青**（关 scissor，视口设成 clear_w x clear_h）
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, clear_w, clear_h);
    glClearColor(0.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // 第二段：场景色 = **洋红**（scissor 限在 render 区）
    if (vw > 0 && vh > 0) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(vx, vy, vw, vh);
        glViewport(vx, vy, vw, vh);
        glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    }
    glViewport(0, 0, clear_w, clear_h);
}

} // namespace

int main(int argc, char** argv) {
    int w0 = 900, h0 = 600;
    int mode = 0;
    int grow = 4;
    if (argc >= 3) { w0 = std::atoi(argv[1]); h0 = std::atoi(argv[2]); }
    if (argc >= 4) { mode = std::atoi(argv[3]); }
    if (argc >= 5) { grow = std::atoi(argv[4]); }

    const char* mode_name =
        (mode == 0) ? "引擎现状（第一段用阶段7的像素尺寸快照）"
      : (mode == 1) ? "修法A（第一段用 max(阶段1,阶段7)+余量）"
      : (mode == 2) ? "修法B（像素尺寸也在阶段1取快照 ⇒ 两个尺寸同源）"
                    : "修法C（改尺寸后 SDL_SyncWindow 再读像素尺寸）";

    std::printf("=============================================================\n");
    std::printf(" 白条探针（第三版）：忠实复刻 + 受控复现\n");
    std::printf(" 窗口初始 = %d x %d    模式 = %d（%s）\n", w0, h0, mode, mode_name);
    std::printf(" 阶段7.5 每次增长 = %d 像素\n", grow);
    std::printf("=============================================================\n\n");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("order2 probe v2", w0, h0,
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

    // 先把整个窗口涂成**红**（当作"窗口自己的背景 / 从未被清过的残留"）
    {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, pw, ph);
        glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(win);
        std::printf("【零】先把窗口涂成**红**（%d x %d）\n", pw, ph);
        std::printf("      ⇒ 于是「未被清到」的像素会显示为**红**（或别的残留色），一眼可辨\n\n");
        SDL_Delay(200);
    }

    std::printf("    颜色约定：第一段清**青**(0,255,255)，第二段清**洋红**(255,0,255)\n\n");

    int frame = 0;
    int hit = 0;
    bool quit = false;
    const int max_frames = 400;

    while (!quit && frame < max_frames) {
        // ---------------- 阶段 1：取窗口尺寸（引擎 frame.cpp:246）----------------
        int wL = 0, hL = 0;
        SDL_GetWindowSize(win, &wL, &hL);
        int pL = 0, qL = 0;
        SDL_GetWindowSizeInPixels(win, &pL, &qL);      // 供"修法B（同源）"用

        // ---------------- 阶段 3：泵事件 ----------------
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { quit = true; }
        }

        // ---------------- 阶段 7：读像素尺寸（引擎 frame.cpp:474）----------------
        int p7 = 0, q7 = 0;
        SDL_GetWindowSizeInPixels(win, &p7, &q7);

        // ---------------- 阶段 7.5（**本探针新增**）：受控增长 ----------------
        int wAfter = wL, hAfter = hL;
        bool grew = false;
        if (frame > 3 && grow > 0) {
            int cw = 0, ch = 0;
            SDL_GetWindowSize(win, &cw, &ch);
            wAfter = cw + grow;
            hAfter = ch;
            SDL_SetWindowSize(win, wAfter, hAfter);
            grew = true;

            if (mode == 3) {
                SDL_SyncWindow(win);
                SDL_GetWindowSizeInPixels(win, &p7, &q7);
            }
        }

        // ---------------- 按模式决定两段的参数 ----------------
        int clear_w = p7, clear_h = q7;               // 引擎现状
        if (mode == 1) {
            const int mw = (p7 > pL ? p7 : pL);
            const int mh = (q7 > qL ? q7 : qL);
            clear_w = mw + 512;
            clear_h = mh + 512;
        } else if (mode == 2) {
            clear_w = pL;                              // 同源（旧值）
            clear_h = qL;
        }

        // 第二段：引擎里 = 阶段 1 算出的视口（这里用阶段 1 的窗口尺寸代表）
        const int vx = 0, vy = 0, vw = wL, vh = hL;

        two_stage_clear(clear_w, clear_h, vx, vy, vw, vh);
        glFinish();

        // ---------------- 呈现之前：扫描一行 ----------------
        if (grew) {
            int fbw = 0, fbh = 0;
            SDL_GetWindowSizeInPixels(win, &fbw, &fbh);
            const int scan_y = fbh / 2;

            std::printf("  [帧 %d] 阶段1=%dx%d  阶段7像素=%dx%d  阶段7.5增长到 %dx%d\n",
                        frame, wL, hL, p7, q7, wAfter, hAfter);
            std::printf("        第一段范围 = %dx%d    第二段 scissor = (0,0,%d,%d)\n",
                        clear_w, clear_h, vw, vh);
            scan_row_runs(scan_y, fbw, 16);

            int bad_from = -1, bad_to = -1;
            for (int x = (vw > 0 ? vw : 0); x < fbw; ++x) {
                unsigned char c[4] = {0, 0, 0, 0};
                glReadPixels(x, scan_y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
                const bool is_cyan = (c[0] < 20 && c[1] > 235 && c[2] > 235);
                const bool is_magenta = (c[0] > 235 && c[1] < 20 && c[2] > 235);
                if (!is_cyan && !is_magenta) {
                    if (bad_from < 0) { bad_from = x; }
                    bad_to = x;
                }
            }
            if (bad_from >= 0) {
                ++hit;
                unsigned char c[4] = {0, 0, 0, 0};
                glReadPixels(bad_from, scan_y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
                std::printf("        => !! 发现未被清到的区间 x %d..%d（%d 像素），颜色 = %s (%d,%d,%d)\n",
                            bad_from, bad_to, bad_to - bad_from + 1,
                            classify(c[0], c[1], c[2]), c[0], c[1], c[2]);
            } else {
                std::printf("        => OK 整行都在青/洋红之内（没有未清区间）\n");
            }
            std::printf("\n");
        }

        SDL_GL_SwapWindow(win);
        SDL_Delay(16);
        ++frame;
    }

    std::printf("=============================================================\n");
    std::printf("【小结】模式 %d（%s），共 %d 帧，抓到未清区间 %d 次\n",
                mode, mode_name, frame, hit);
    if (hit > 0) {
        std::printf("    => !! **白条被复现**：清屏没有盖住整块帧缓冲\n");
        std::printf("       => 成因：**第一段的尺寸来自阶段 7 的快照，而窗口在那之后又变大了**\n");
        std::printf("       => 修法是否有效，看别的模式里这个计数是否变成 0\n");
    } else {
        std::printf("    => 本次**没有**未清区间\n");
        std::printf("       * 若模式 = 0 => 复现手法还不够（把第 4 个参数即增长量调大）\n");
        std::printf("       * 若模式 = 1/3 => **说明该修法有效**\n");
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
//  g++ -std=c++20 -O1 -g -IC:/msys64/ucrt64/include sdl3_order2_probe.cpp \
//      -o bin/sdl3_order2_probe.exe -LC:/msys64/ucrt64/lib -lSDL3 -lopengl32
//
//  ./bin/sdl3_order2_probe.exe 900 600 0 4 > o2_mode0.txt 2>&1; cat o2_mode0.txt
//  ./bin/sdl3_order2_probe.exe 900 600 1 4 > o2_mode1.txt 2>&1; cat o2_mode1.txt
//  ./bin/sdl3_order2_probe.exe 900 600 2 4 > o2_mode2.txt 2>&1; cat o2_mode2.txt
//  ./bin/sdl3_order2_probe.exe 900 600 3 4 > o2_mode3.txt 2>&1; cat o2_mode3.txt
// =============================================================================
