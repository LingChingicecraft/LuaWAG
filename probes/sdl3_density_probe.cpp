// =============================================================================
//  sdl3_density_probe —— "高像素密度到底有没有生效"的判定探针
//
//  【它回答的那个问题（而它与我们的坐标链是同一个问题）】
//      实测（`sdl3_size_probe`）：在 150% 缩放的显示器上
//          SDL_GetWindowSize == SDL_GetWindowSizeInPixels == 1920×1080
//      而 SDL_GetWindowDisplayScale == 1.5。
//
//      而我们的窗口是这样建的（src/platform/window.cpp:145）：
//          SDL_CreateWindow(title, ..., SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
//      也就是说 —— **没有** SDL_WINDOW_HIGH_PIXEL_DENSITY。
//
//      于是"两者相等"有两种解释，而**API 报告无法区分**：
//          甲：那台显示器在本进程看来不是高像素密度 ⇒ 相等是对的
//          乙：没有那个标志 ⇒ SDL 给的是低密度后备缓冲 ⇒ 绘图面被系统放大 ⇒ 画面发虚
//
//  【SDL3 头文件自己的话（SDL_video.h）】
//      "The window pixel size may differ from its window coordinate size
//       **if the window is on a high pixel density display**.
//       Use SDL_GetWindowSize() to query the client area's size in window coordinates,
//       and SDL_GetWindowSizeInPixels() ... to query the drawable size in pixels."
//      ⇒ 它把差异归给**显示器**，而**不提那个标志** —— 所以只能实测。
//
//  【判据（主）】
//      同一个进程里开**两个**窗口，请求**同一个尺寸**：
//          A：不带 SDL_WINDOW_HIGH_PIXEL_DENSITY
//          B：带   SDL_WINDOW_HIGH_PIXEL_DENSITY
//      若 B 的像素尺寸 ≈ A × 1.5（或至少与 A **不同**）⇒ 那个标志是有效的开关。
//      若 A 与 B 完全相同 ⇒ 该标志在本平台无效（或另有开关）⇒ 要换一条路。
//
//  【判据（交叉）】
//      · `glGetIntegerv(GL_VIEWPORT)` 的初值 —— GL 眼里**默认帧缓冲**多大
//        （SDL3 **删掉了** `SDL_GL_GetDrawableSize`，它的替代就是
//          `SDL_GetWindowSizeInPixels`；所以"帧缓冲多大"由这一条独立佐证）
//      · `glGetIntegerv(GL_VIEWPORT)` 的初值 —— GL 眼里默认帧缓冲多大
//      · `SDL_GetWindowPixelDensity` 与 `SDL_GetWindowDisplayScale` 是否相等
//        （它们是**两个**函数，而我们的 `get_dpi_scale()` 用的是后者）
//
//  【判据（单位语义）】
//      对每个窗口调一次 `SDL_SetWindowSize(它当前的"像素"尺寸)`，看四个数怎么变：
//          · 若"窗口坐标尺寸"变成那个值 ⇒ SetWindowSize 收的是**窗口坐标**
//          · 若"像素尺寸"变成那个值    ⇒ 它收的是**像素**
//      ⇒ 这一条决定**分辨率库（lumi.screen 的 presets）里的数字是什么单位**。
//
//  【它不进引擎源码、不进 CMake】与 build/ 下那几个探针同性质，用完可删。
//
//  编译与运行（MSYS2 UCRT64）见本文件末尾。
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cmath>
#include <cstdio>

namespace {

// -----------------------------------------------------------------------------
//  一个窗口的测量结果
// -----------------------------------------------------------------------------
struct measurement {
    const char* label = "";

    // 平台报的四个量
    int   coord_w = 0, coord_h = 0;   // SDL_GetWindowSize（文档：window coordinates）
    int   pixel_w = 0, pixel_h = 0;   // SDL_GetWindowSizeInPixels（文档：pixels）
    float pixel_density = 0.0f;       // SDL_GetWindowPixelDensity
    float display_scale = 0.0f;       // SDL_GetWindowDisplayScale

    // 实际 GL 帧缓冲（比 SDL 报的更直接）
    int   drawable_w = 0, drawable_h = 0;

    // GL 眼里的默认视口（前两个是 x,y，后两个是 w,h）
    int   gl_vp[4] = {0, 0, 0, 0};

    // 标志
    bool  has_high_density_flag = false;
    Uint64 flags = 0;
};

void measure(SDL_Window* win, SDL_GLContext gl, const char* label, measurement& m) {
    m.label = label;

    SDL_GetWindowSize(win, &m.coord_w, &m.coord_h);
    SDL_GetWindowSizeInPixels(win, &m.pixel_w, &m.pixel_h);
    m.pixel_density = SDL_GetWindowPixelDensity(win);
    m.display_scale = SDL_GetWindowDisplayScale(win);

    m.flags = SDL_GetWindowFlags(win);
    m.has_high_density_flag = (m.flags & SDL_WINDOW_HIGH_PIXEL_DENSITY) != 0;

    // GL 那一路要有上下文才能读。若上下文没建成，就报 -1（而不是编一个数）。
    if (gl != nullptr) {
        SDL_GL_MakeCurrent(win, gl);
        glGetIntegerv(GL_VIEWPORT, m.gl_vp);
        m.drawable_w = m.gl_vp[2];
        m.drawable_h = m.gl_vp[3];
    } else {
        m.gl_vp[0] = m.gl_vp[1] = m.gl_vp[2] = m.gl_vp[3] = -1;
        m.drawable_w = m.drawable_h = -1;
    }
}

void print_one(const measurement& m) {
    const double calc_display = static_cast<double>(m.coord_w) * static_cast<double>(m.display_scale);
    const double calc_density = static_cast<double>(m.coord_w) * static_cast<double>(m.pixel_density);

    std::printf("  ---- %s ----\n", m.label);
    std::printf("    窗口坐标(报)   = %d x %d     (SDL_GetWindowSize)\n", m.coord_w, m.coord_h);
    std::printf("    像素(报)       = %d x %d     (SDL_GetWindowSizeInPixels)\n", m.pixel_w, m.pixel_h);
    std::printf("    GL 帧缓冲      = w=%d h=%d  (x=%d y=%d)  glGetIntegerv(GL_VIEWPORT)\n",
                m.gl_vp[2], m.gl_vp[3], m.gl_vp[0], m.gl_vp[1]);
    std::printf("    pixel_density  = %.6f      (SDL_GetWindowPixelDensity)\n",
                static_cast<double>(m.pixel_density));
    std::printf("    display_scale  = %.6f      (SDL_GetWindowDisplayScale)\n",
                static_cast<double>(m.display_scale));
    std::printf("    高密度标志     = %s\n", m.has_high_density_flag ? "**有**" : "无");
    std::printf("    坐标 x display = %.1f x %.1f\n", calc_display,
                static_cast<double>(m.coord_h) * static_cast<double>(m.display_scale));
    std::printf("    坐标 x density = %.1f x %.1f\n", calc_density,
                static_cast<double>(m.coord_h) * static_cast<double>(m.pixel_density));
    std::printf("    像素 == 坐标 ? %s\n",
                (m.pixel_w == m.coord_w && m.pixel_h == m.coord_h) ? "**是（相等）**" : "否（不同）");
    std::printf("    GL == 像素 ?   %s\n",
                (m.drawable_w == m.pixel_w && m.drawable_h == m.pixel_h) ? "是" : "**否**");
}

// -----------------------------------------------------------------------------
//  SetWindowSize 的单位语义：喂进去"像素"值，看谁跟着变
// -----------------------------------------------------------------------------
void probe_set_size_unit(SDL_Window* win, SDL_GLContext gl, const char* label) {
    measurement before{};
    measure(win, gl, label, before);

    // 故意喂一个与"当前窗口坐标"不同的值：用当前的**像素**宽 + 40，
    // 高度沿用当前像素高。于是：
    //   · 若窗口坐标尺寸变成这个值 ⇒ SetWindowSize 收的是**窗口坐标**
    //   · 若像素尺寸变成这个值     ⇒ 它收的是**像素**
    const int req_w = before.pixel_w + 40;
    const int req_h = before.pixel_h;

    std::printf("  ---- %s · SetWindowSize 单位探针 ----\n", label);
    std::printf("    请求 = %d x %d（= 当前像素 w + 40）\n", req_w, req_h);

    SDL_SetWindowSize(win, req_w, req_h);
    SDL_PumpEvents();
    SDL_Delay(60); // 让平台把新尺寸落实（实测 SetWindowSize 是同步的，这里只多给一点余量）

    measurement after{};
    measure(win, gl, label, after);

    std::printf("    之后 窗口坐标 = %d x %d\n", after.coord_w, after.coord_h);
    std::printf("    之后 像素     = %d x %d\n", after.pixel_w, after.pixel_h);
    std::printf("    之后 GL 帧缓冲 = %d x %d\n", after.drawable_w, after.drawable_h);

    if (after.coord_w == req_w) {
        std::printf("    ⇒ **它收的是「窗口坐标」**（坐标尺寸变成了请求值）\n");
    } else if (after.pixel_w == req_w) {
        std::printf("    ⇒ **它收的是「像素」**（像素尺寸变成了请求值）\n");
    } else {
        std::printf("    ⇒ 两者都不是请求值 —— 看上面的数自己判断\n");
    }
    std::printf("    ------------------------------------------------------------\n");
}

// -----------------------------------------------------------------------------
//  拖动/跨屏时的变化：只报事件，不改窗口
// -----------------------------------------------------------------------------
void drain_events() {
    SDL_Event e;
    int n_pixel_size = 0;
    int n_resized = 0;
    int n_display_changed = 0;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: ++n_pixel_size; break;
        case SDL_EVENT_WINDOW_RESIZED:            ++n_resized; break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:    ++n_display_changed; break;
        default: break;
        }
    }
    if (n_pixel_size || n_resized || n_display_changed) {
        std::printf("    [事件] PIXEL_SIZE_CHANGED=%d  RESIZED=%d  DISPLAY_CHANGED=%d\n",
                    n_pixel_size, n_resized, n_display_changed);
    }
}

// -----------------------------------------------------------------------------
//  建一个窗口 + GL 上下文
// -----------------------------------------------------------------------------
struct win_pair {
    SDL_Window*   win = nullptr;
    SDL_GLContext gl  = nullptr;
};

win_pair make_window(const char* title, int req_w, int req_h, bool high_density) {
    win_pair p{};
    Uint64 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    if (high_density) {
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }

    p.win = SDL_CreateWindow(title, req_w, req_h, flags);
    if (p.win == nullptr) {
        std::printf("  !! SDL_CreateWindow 失败：%s\n", SDL_GetError());
        return p;
    }
    p.gl = SDL_GL_CreateContext(p.win);
    if (p.gl == nullptr) {
        std::printf("  !! SDL_GL_CreateContext 失败：%s\n", SDL_GetError());
    }
    return p;
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;

    // 请求尺寸：用"我们引擎那个启动尺寸"。
    //   ⚠️ 而它的单位本身也待定 —— 所以两个窗口都用同一个数请求，便于对比。
    int req_w = 1920;
    int req_h = 1080;
    if (argc >= 3) {
        req_w = std::atoi(argv[1]);
        req_h = std::atoi(argv[2]);
    }

    std::printf("=============================================================\n");
    std::printf(" 高像素密度判定探针\n");
    std::printf(" 请求尺寸（两个窗口相同） = %d x %d\n", req_w, req_h);
    std::printf("=============================================================\n\n");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }

    std::printf("SDL 版本（编译期） = %d.%d.%d\n",
                SDL_VERSIONNUM_MAJOR(SDL_VERSION),
                SDL_VERSIONNUM_MINOR(SDL_VERSION),
                SDL_VERSIONNUM_MICRO(SDL_VERSION));
    {
        const char* drv = SDL_GetCurrentVideoDriver();
        std::printf("视频驱动 = %s\n\n", (drv != nullptr) ? drv : "(未知)");
    }

    // ---- 显示器一级的信息（先看这台屏怎么被描述）----
    int ndisp = 0;
    SDL_DisplayID* disps = SDL_GetDisplays(&ndisp);
    std::printf("显示器数 = %d\n", ndisp);
    for (int i = 0; i < ndisp; ++i) {
        // ⚠️ SDL3 与 SDL2 在这里**形态不同**：它**返回一个指针**，
        //    而不是"传一个 SDL_DisplayMode* 让它填"。所以要先判空，再复制。
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(disps[i]);
        SDL_Rect bounds{};
        const bool has_bounds = SDL_GetDisplayBounds(disps[i], &bounds);
        const char* dname = SDL_GetDisplayName(disps[i]);
        std::printf("  [%d] 名称=%s\n", i, (dname != nullptr) ? dname : "(未知)");
        if (mode != nullptr) {
            // ⚠️ 注意这个 `pixel_density` 是 **SDL_DisplayMode** 的字段，它的语义是
            //    "把该模式的尺寸换算成像素的比例"（例如 1920x1080 的模式若为 2.0，
            //     则实际 3840x2160 像素）—— 它**不是**窗口的像素密度。
            std::printf("      当前模式 = %d x %d @ %.2fHz   模式->像素比例=%.4f   content_scale=%.4f\n",
                        mode->w, mode->h, static_cast<double>(mode->refresh_rate),
                        static_cast<double>(mode->pixel_density),
                        static_cast<double>(SDL_GetDisplayContentScale(disps[i])));
        } else {
            std::printf("      （取当前模式失败：%s）\n", SDL_GetError());
        }
        if (has_bounds) {
            std::printf("      桌面范围 = x=%d y=%d w=%d h=%d\n",
                        bounds.x, bounds.y, bounds.w, bounds.h);
        }
    }
    SDL_free(disps);
    std::printf("\n");

    // ---- A：不带高密度标志（= 我们引擎现在的做法）----
    win_pair a = make_window("probe A (no HIGH_PIXEL_DENSITY)", req_w, req_h, false);
    // ---- B：带高密度标志 ----
    win_pair b = make_window("probe B (HIGH_PIXEL_DENSITY)", req_w, req_h, true);

    if (a.win != nullptr) { SDL_ShowWindow(a.win); }
    if (b.win != nullptr) { SDL_ShowWindow(b.win); }
    SDL_PumpEvents();
    SDL_Delay(300); // 让窗口真正映射，否则尺寸可能还没落实

    measurement ma{}, mb{};
    if (a.win != nullptr && a.gl != nullptr) { measure(a.win, a.gl, "A · 不带高密度标志", ma); }
    if (b.win != nullptr && b.gl != nullptr) { measure(b.win, b.gl, "B · 带高密度标志",   mb); }

    std::printf("【一】两个窗口的对照\n");
    if (a.win != nullptr && a.gl != nullptr) { print_one(ma); }
    if (b.win != nullptr && b.gl != nullptr) { print_one(mb); }
    std::printf("\n");

    // ---- 主判据 ----
    std::printf("【二】主判据\n");
    if (a.win != nullptr && a.gl != nullptr && b.win != nullptr && b.gl != nullptr) {
        const bool pixel_differ = (ma.pixel_w != mb.pixel_w) || (ma.pixel_h != mb.pixel_h);
        const bool drawable_differ = (ma.drawable_w != mb.drawable_w) || (ma.drawable_h != mb.drawable_h);

        std::printf("    A 像素 = %d x %d\n", ma.pixel_w, ma.pixel_h);
        std::printf("    B 像素 = %d x %d\n", mb.pixel_w, mb.pixel_h);
        std::printf("    A GL   = %d x %d\n", ma.drawable_w, ma.drawable_h);
        std::printf("    B GL   = %d x %d\n", mb.drawable_w, mb.drawable_h);
        std::printf("    A 坐标 = %d x %d\n", ma.coord_w, ma.coord_h);
        std::printf("    B 坐标 = %d x %d\n", mb.coord_w, mb.coord_h);

        if (mb.pixel_w > 0) {
            std::printf("    B像素 / A像素 = %.4f\n",
                        static_cast<double>(mb.pixel_w) / static_cast<double>(ma.pixel_w));
        }

        if (pixel_differ || drawable_differ) {
            std::printf("\n    ⇒ **那个标志是有效的开关**（B 与 A 不同）\n");
            std::printf("       ⇒ 可以按物理分辨率渲染：设上标志，用像素尺寸算视口。\n");
        } else {
            std::printf("\n    ⇒ **A 与 B 完全相同** ⇒ 该标志在本平台没有产生差别。\n");
            std::printf("       此时要继续分辨「甲/乙」两种解释，只能靠【三】的物理尺寸测量：\n");
            std::printf("         甲：这台屏在本进程看来不是高像素密度 ⇒ 相等是对的（无发虚）\n");
            std::printf("         乙：绘图面被系统放大 ⇒ 相等是假象（有发虚）\n");
        }
    } else {
        std::printf("    （有窗口创建失败，跳过）\n");
    }
    std::printf("\n");

    // ---- SetWindowSize 的单位语义 ----
    std::printf("【三】SDL_SetWindowSize 收的是「窗口坐标」还是「像素」\n");
    if (a.win != nullptr && a.gl != nullptr) { probe_set_size_unit(a.win, a.gl, "A"); }
    if (b.win != nullptr && b.gl != nullptr) { probe_set_size_unit(b.win, b.gl, "B"); }
    std::printf("\n");

    // ---- 剩余事件（拖动/跨屏时会有）----
    std::printf("【四】期间收到的事件\n");
    drain_events();
    std::printf("    （若上面没有 [事件] 行，说明这段时间里平台没发相关事件）\n");
    std::printf("\n");

    // ---- 留下图案窗口，便于人工判断"发虚" ----
    std::printf("=============================================================\n");
    std::printf(" 【五】人工判据 —— 现在两个窗口里各画了一个 1 像素棋盘格\n");
    std::printf("   · 若某个窗口里的棋盘格**糊成一片灰** ⇒ 那个窗口的绘图面被系统放大了（发虚）\n");
    std::printf("   · 若某个窗口里能看到**清晰的细网格** ⇒ 它按物理像素在画（清晰）\n");
    std::printf("   ⇒ 两个窗口**并排看**，差别一眼可辨。\n");
    std::printf("   按回车退出（或等 30 秒自动退出）。\n");
    std::printf("=============================================================\n");

    auto draw_grid = [](win_pair& p) {
        if (p.win == nullptr || p.gl == nullptr) { return; }
        SDL_GL_MakeCurrent(p.win, p.gl);
        int dw = 0, dh = 0;
        SDL_GetWindowSizeInPixels(p.win, &dw, &dh);
        if (dw <= 0 || dh <= 0) { return; }
        glViewport(0, 0, dw, dh);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, static_cast<double>(dw), 0.0, static_cast<double>(dh), -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        // 【先画 16 像素的方块棋盘】它的**边缘**是判断"是否被放大"的主信号：
        //   若绘图面被系统放大，块边缘会出现 1.5 倍的不均匀过渡带。
        const int kBlock = 16;
        glBegin(GL_QUADS);
        for (int by = 0; by < dh; by += kBlock) {
            for (int bx = 0; bx < dw; bx += kBlock) {
                if (((bx / kBlock) + (by / kBlock)) & 1) { continue; }
                glColor3f(0.35f, 0.35f, 0.35f);
                glVertex2i(bx, by);
                glVertex2i(bx + kBlock, by);
                glVertex2i(bx + kBlock, by + kBlock);
                glVertex2i(bx, by + kBlock);
            }
        }
        glEnd();

        // 【再叠一层 1 像素棋盘点】它是"绘图面是否等于物理像素"最灵敏的试纸：
        //   若显示面就是物理像素 ⇒ 这层点呈**均匀的灰**；
        //   若被放大 1.5 倍   ⇒ 会出现**粗大的摩尔纹**。
        //   用 GL_POINTS（每像素一个点）而不是四边形，避免百万级立即模式几何。
        glPointSize(1.0f);
        glColor3f(1.0f, 1.0f, 1.0f);
        glBegin(GL_POINTS);
        for (int y = 0; y < dh; ++y) {
            for (int x = (y & 1); x < dw; x += 2) {
                glVertex2i(x, y);
            }
        }
        glEnd();

        SDL_GL_SwapWindow(p.win);
    };

    draw_grid(a);
    draw_grid(b);

    // 等回车或超时
    bool quit = false;
    const Uint64 deadline = SDL_GetTicks() + 30000;
    while (!quit && SDL_GetTicks() < deadline) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_KEY_DOWN ||
                e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                quit = true;
            }
        }
        SDL_Delay(50);
    }

    if (a.gl != nullptr) { SDL_GL_DestroyContext(a.gl); }
    if (b.gl != nullptr) { SDL_GL_DestroyContext(b.gl); }
    if (a.win != nullptr) { SDL_DestroyWindow(a.win); }
    if (b.win != nullptr) { SDL_DestroyWindow(b.win); }
    SDL_Quit();
    return 0;
}

// =============================================================================
//  编译与运行（MSYS2 UCRT64）
//
//  cd /c/Users/vqwjq/Documents/LuaWAG/LuaWAG-Vanilla
//  g++ -std=c++20 -O1 -g -IC:/msys64/ucrt64/include sdl3_density_probe.cpp \
//      -o bin/sdl3_density_probe.exe -LC:/msys64/ucrt64/lib -lSDL3 -lopengl32
//
//  ./bin/sdl3_density_probe.exe
//  ./bin/sdl3_density_probe.exe 1920 1080      # 指定请求尺寸（默认就是它）
//
//  【为什么不用 pkg-config】它的输出里带 -mwindows，会把程序变成窗口子系统，
//  控制台输出就没了 —— 而本探针全靠 printf。
// =============================================================================
