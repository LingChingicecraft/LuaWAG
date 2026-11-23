// =============================================================================
//  sdl3_size_probe.cpp —— **独立的最小对照实验**，不属于引擎源码。
//
//  【它为什么存在】
//      引擎里渲染出的画面"总是偏一点"（偏右 / 偏下），而引擎自己记录的每一个
//      中间量（变换的六个分量、按策略算出的视口矩形、视口与 scissor 的像素值）
//      都自洽。于是怀疑落在一个**没有中间量可打印**的地方：
//
//          **"帧缓冲到底有多大"这件事，我们有三个可能的来源，而它们可能不同。**
//
//          甲：SDL_GetWindowSize            （自称是 DIP）
//          乙：SDL_GetWindowSizeInPixels    （自称是像素）
//          丙：SDL_GetWindowDisplayScale    （比例）
//          丁：dip × scale                  
//
//      本文件用**最小的一段代码**把这四个数一起打印出来，回答一个具体问题：
//
//          **拖动窗口之后，这四个数是否同步变化？**
//
//      若"像素尺寸"不跟着变（或变得不对），那它就是错误的来源 —— 引擎必须换
//      一个真值来源。若四个数都同步，那问题在引擎的视口算术里，需要另找。
//
//  【为什么用轮询而不是监听事件】
//      Windows 在**交互式拖动期间**会把尺寸更新攒到拖动结束（引擎那边已实测）。
//      若只在 WINDOW_RESIZED / PIXEL_SIZE_CHANGED 事件里打印，拖动过程中就一行
//      都没有，无法回答"拖动中这三个数是否不同步"。轮询每帧都读一次，于是
//      拖动中也能看到真实情况。
//
//  【它刻意不做的事】
//      不包含引擎的任何头、不链接引擎的任何目标文件。它只有 SDL3 与 GL 1.1。
//      它不画东西 —— 它只**报告数字**（画东西会引入另一类问题）。
//
//  【关于 GL 入口点】
//      这里只调用 GL 1.1 的函数（glGetString / glGetIntegerv），它们在
//      Windows 的 opengl32.dll 里就有，因此需要链接 -lopengl32。
//      3.3 的函数（例如 glBindFramebuffer）**不在这里用**。
//
//  【它是一次性的】
//      结论拿到之后可以删掉；它不属于交付物。
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>

namespace {

int  g_last_dip_w = -1;
int  g_last_dip_h = -1;
int  g_last_px_w  = -1;
int  g_last_px_h  = -1;
unsigned long long g_change_index = 0;

/// 打印一组尺寸。返回 true 表示这一组与上一组不同。
bool print_sizes(SDL_Window* win) {
    int dip_w = 0;
    int dip_h = 0;
    SDL_GetWindowSize(win, &dip_w, &dip_h);

    int px_w = 0;
    int px_h = 0;
    if (!SDL_GetWindowSizeInPixels(win, &px_w, &px_h)) {
        std::printf("    SDL_GetWindowSizeInPixels 失败: %s\n", SDL_GetError());
    }

    if (dip_w == g_last_dip_w && dip_h == g_last_dip_h && px_w == g_last_px_w &&
        px_h == g_last_px_h) {
        return false;
    }
    g_last_dip_w = dip_w;
    g_last_dip_h = dip_h;
    g_last_px_w  = px_w;
    g_last_px_h  = px_h;
    ++g_change_index;

    const float  scale  = SDL_GetWindowDisplayScale(win);
    const double calc_w = static_cast<double>(dip_w) * static_cast<double>(scale);
    const double calc_h = static_cast<double>(dip_h) * static_cast<double>(scale);

    // ---- 三条判定用探针 ----
    //
    // 【为什么需要它们】
    //   `SDL_GetWindowSizeInPixels` 是否等于 `SDL_GetWindowSize`，取决于窗口
    //   是否被标记为"高像素密度"。若**没有**那个标志，那么"两者相等"就是
    //   **符合约定的行为**（绘制表面就是逻辑分辨率），而不是函数出错。
    //   ⇒ 而这两种解释指向完全不同的修法：
    //         有标志  ⇒ 函数应当返回物理像素 ⇒ 它返回逻辑尺寸就是错的
    //         无标志  ⇒ 绘制表面本来就是逻辑分辨率 ⇒ 应该**加上标志**
    const float density = SDL_GetWindowPixelDensity(win);
    const SDL_WindowFlags flags = SDL_GetWindowFlags(win);
    const bool has_high_dpi = (flags & SDL_WINDOW_HIGH_PIXEL_DENSITY) != 0;

    std::printf("#%llu\n", g_change_index);
    std::printf("    DIP      = %d x %d        (SDL_GetWindowSize)\n", dip_w, dip_h);
    std::printf("    像素(报) = %d x %d        (SDL_GetWindowSizeInPixels)\n", px_w, px_h);
    std::printf("    比例     = %.6f          (SDL_GetWindowDisplayScale)\n",
                static_cast<double>(scale));
    std::printf("    像素密度 = %.6f          (SDL_GetWindowPixelDensity)\n",
                static_cast<double>(density));
    std::printf("    高密度标志 = %s          (SDL_WINDOW_HIGH_PIXEL_DENSITY)\n",
                has_high_dpi ? "**有**" : "无");
    std::printf("    像素(算) = %.1f x %.1f   (DIP x 比例)\n", calc_w, calc_h);
    std::printf("    报 == DIP ? %s\n",
                (px_w == dip_w && px_h == dip_h) ? "**是**（相等）" : "否（不同）");
    std::printf("    差       = 报 - 算 = %.1f x %.1f\n",
                static_cast<double>(px_w) - calc_w, static_cast<double>(px_h) - calc_h);
    std::printf("    宽高比   = DIP %.4f / 报 %.4f\n",
                (dip_h > 0) ? static_cast<double>(dip_w) / static_cast<double>(dip_h) : 0.0,
                (px_h > 0) ? static_cast<double>(px_w) / static_cast<double>(px_h) : 0.0);
    std::printf("    ------------------------------------------------------------\n");
    std::fflush(stdout);
    return true;
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

    SDL_Window* win = SDL_CreateWindow("SDL3 尺寸探针 —— 拖动四边，看数字变化（关窗退出）",
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

    {
        const GLubyte* ver = glGetString(GL_VERSION);
        const GLubyte* ren = glGetString(GL_RENDERER);
        std::printf("==============================================================\n");
        std::printf(" SDL3 尺寸探针\n");
        std::printf(" GL: %s | %s\n", ver ? reinterpret_cast<const char*>(ver) : "?",
                    ren ? reinterpret_cast<const char*>(ren) : "?");
        std::printf(" 请做四件事，每一件之后稍等一两秒：\n");
        std::printf("   1. 拖**右**边往右（明显一点）\n");
        std::printf("   2. 拖**下**边往下\n");
        std::printf("   3. 拖**左**边往左\n");
        std::printf("   4. 拖**上**边往上\n");
        std::printf(" 观察『像素(报)』是否随 DIP 同步变化、以及『差』是否恒为 0。\n");
        std::printf("==============================================================\n");
    }

    print_sizes(win);

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            }
        }

        // 轮询：拖动期间也能看到尺寸的每一次变化。
        print_sizes(win);

        glViewport(0, 0, 1280, 720);
        glClearColor(0.15f, 0.15f, 0.35f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(win);

        SDL_Delay(16);
    }

    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
