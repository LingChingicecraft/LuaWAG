// =============================================================================
//  sdl3_pattern_probe.cpp —— **独立的最小对照实验**，不属于引擎源码。
//
//  【它为什么存在 —— 以及它为什么取代了"再打印一个尺寸"】
//      前面已经打印了四个尺寸来源：SDL_GetWindowSize / SDL_GetWindowSizeInPixels /
//      SDL_GetWindowDisplayScale / SDL_GetWindowPixelDensity。它们彼此**自洽**
//      （像素密度 1.0、无高密度标志 ⇒ "逻辑尺寸 == 像素尺寸"符合约定），
//      却与我们自己的截图**冲突**（截图里客户区约 1920 物理像素，而报的是 1280）。
//
//      冲突的两种解释在截图上都是"纯色铺满"，无法分辨：
//          甲：帧缓冲 = 1920×1080，直接铺满客户区           （放大倍率 1.0）
//          乙：帧缓冲 = 1280×720，被合成器放大 1.5 倍       （放大倍率 1.5）
//
//      ⇒ 所以本次不"问某个 API"，而是**画已知像素尺寸的东西**，
//        再从截图里**数像素**反推放大倍率。测量，而不是询问。
//
//  【它画什么】
//      底色：深蓝
//      ① 左下角：**100×100 白色实心方块**
//             ⇒ 量它占多少像素：100 ⇒ 甲；150 ⇒ 乙
//      ② 四角：各一个 20×20 的点，位置见下（四色不同，便于叙述）
//             ⇒ 都可见 ⇒ 视口覆盖整个帧缓冲；缺一个 ⇒ 尺寸方向不对
//      ③ 上边缘中点红、下边缘中点绿（各 20×20）
//             ⇒ 分辨 y 原点方向（GL 规范：视口原点在左下角 ⇒ 绿点应在屏幕**下**方）
//
//  【第一版的两个错误（记下来，避免重犯）】
//      ① 图案画在 `if (尺寸变了)` 里 ⇒ **只在第一帧画**。而双缓冲下每帧都要交换，
//         后缓冲里留着的是最后一次 glClear 的颜色 ⇒ 画面在"图案"与"纯绿"之间闪。
//         ⇒ 修正：**每帧重画整套图案**（清屏是每帧的事，不是一次的事）。
//      ② 角点只有 3×3 ⇒ 在截图里几乎点不准。改成 20×20。
//
//  【它刻意不做的事】
//      不包含引擎的任何头、不链接引擎的任何目标文件。只有 SDL3 与 GL 1.1。
//      （3.3 的函数一律不用 —— 见引擎 renderer.cpp 里"GL 入口点尚未决策"。）
//
//  【它是一次性的】结论拿到之后可以删掉。
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>

namespace {

/// 用"设 scissor + 清屏"的办法画一个实心矩形。
///
/// 【为什么用 glScissor 而不是 glViewport —— 这是一次实测教训】
///   第一版用 `glViewport(x, y, w, h)` 再 `glClear`，理由是"clear 受视口限制"。
///   而实测：那一版画到最后只剩一个**铺满整屏的绿块**，也就是说**视口并没有
///   限制 clear 的范围**（至少在这个驱动/上下文里不成立）。
///
///   ⇒ 正规的"限制写入范围"手段是 **scissor**：它是纯粹的裁剪，
///     而 viewport 的职责是**坐标映射**。两件事被混为一谈，是这一版的错误来源。
///   ⇒ 所以这里：视口恒定 = 整个帧缓冲，裁剪矩形逐块设置。
void fill_rect(int fb_w, int fb_h, int x, int y, int w, int h, float r, float g, float b) {
    if (w <= 0 || h <= 0) {
        return;
    }
    glViewport(0, 0, fb_w, fb_h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

/// 画一个 s×s 的点，左下角在 (x, y)。
void dot(int fb_w, int fb_h, int x, int y, int s, float r, float g, float b) {
    fill_rect(fb_w, fb_h, x, y, s, s, r, g, b);
}

/// 把整套图案按 W×H 画一遍。**每帧都要调用**。
void draw_pattern(int W, int H) {
    if (W <= 0 || H <= 0) {
        return;
    }
    const int S = 20; // 角点边长

    // ---- 底色：深蓝（整屏，先把 scissor 关掉再清一次，确保没有残留）----
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, W, H);
    glClearColor(0.10f, 0.10f, 0.35f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // ---- ① 左下角 100×100 白方块 ----
    //   GL 的视口原点在**左下角** ⇒ (0,0) 是左下。
    fill_rect(W, H, 0, 0, 100, 100, 1.0f, 1.0f, 1.0f);

    // ---- ② 四角各一个 S×S 的点（四色不同）----
    dot(W, H, 0, 0, S, 1.0f, 1.0f, 1.0f);                  // 左下：白
    dot(W, H, W - S, 0, S, 0.0f, 1.0f, 1.0f);              // 右下：青
    dot(W, H, 0, H - S, S, 1.0f, 0.0f, 0.0f);              // 左上：红
    dot(W, H, W - S, H - S, S, 1.0f, 1.0f, 0.0f);          // 右上：黄

    // ---- ③ 上下边缘中点 ----
    dot(W, H, W / 2 - S / 2, H - S, S, 1.0f, 0.0f, 1.0f);  // 上边中点：品红
    dot(W, H, W / 2 - S / 2, 0, S, 0.0f, 1.0f, 0.0f);      // 下边中点：绿

    glDisable(GL_SCISSOR_TEST);
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* win = SDL_CreateWindow("对照图案探针 —— 请勿拖动，直接截图（关窗退出）",
                                      1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (win == nullptr) {
        std::printf("SDL_CreateWindow 失败: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (ctx == nullptr) {
        std::printf("SDL_GL_CreateContext 失败: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(win, ctx);

    int dip_w = 0;
    int dip_h = 0;
    int px_w = 0;
    int px_h = 0;
    SDL_GetWindowSize(win, &dip_w, &dip_h);
    SDL_GetWindowSizeInPixels(win, &px_w, &px_h);
    const float scale = SDL_GetWindowDisplayScale(win);
    const float density = SDL_GetWindowPixelDensity(win);

    std::printf("==============================================================\n");
    std::printf(" 对照图案探针\n");
    std::printf(" DIP=%dx%d  像素(报)=%dx%d  比例=%.3f  像素密度=%.3f\n", dip_w, dip_h, px_w,
                px_h, static_cast<double>(scale), static_cast<double>(density));
    std::printf(" 图案（**不要拖动窗口**）：\n");
    std::printf("   底色深蓝；**左下角 100x100 白色方块**；\n");
    std::printf("   四角 20x20 点：左下白 / 右下青 / 左上红 / 右上黄；\n");
    std::printf("   上边中点品红 / 下边中点绿。\n");
    std::printf(" 截图后量：白方块边长是 100 还是 150 像素？\n");
    std::printf("==============================================================\n");
    std::fflush(stdout);

    bool running = true;
    int last_w = -1;
    int last_h = -1;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            }
        }

        SDL_GetWindowSizeInPixels(win, &px_w, &px_h);
        const int W = (px_w > 0) ? px_w : 1;
        const int H = (px_h > 0) ? px_h : 1;

        if (W != last_w || H != last_h) {
            last_w = W;
            last_h = H;
            std::printf("重画: W=%d H=%d | 白方块(0,0,100,100) | 左上红(0,%d,20,20) | "
                        "上中品红(%d,%d,20,20) | 下中绿(%d,0,20,20)\n",
                        W, H, H - 20, W / 2 - 10, H - 20, W / 2 - 10);
            std::fflush(stdout);
        }

        // **每帧重画**（第一版把这段放在 if 里 ⇒ 双缓冲下闪成一片纯色）。
        draw_pattern(W, H);

        SDL_GL_SwapWindow(win);
        SDL_Delay(16);
    }

    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
