// =============================================================================
//  sdl3_yflip_probe —— 验证"窗口坐标(+y 向上) → 帧缓冲/裁剪坐标"的那一次翻转
//
//  【要验证的那一句（引擎 src/draw/renderer.cpp:254 的原文）】
//        glScissor(vx, fb_h - (vy + vh), vw, vh);
//      其中（renderer.cpp:216-219）
//        vx = win_render.l        vy = win_render.b
//        vw = win_render.w        vh = win_render.h
//      而 `rect::b` 是**下边**（rect.hpp:56 + top() = b + h）⇒
//        `win_render` 是 **+y 向上的窗口坐标**里的矩形
//      ⇒ 所以 `fb_h - (vy + vh)` 这一次减法是**唯一的那次 y 翻转**
//
//  ============================================================================
//  【第一版为什么没跑起来（留在这里，免得重犯）】
//      ① 默认视口矩形 `b=200, h=300` ⇒ 上边 500 > 帧缓冲高 480 ⇒ 被自己的
//         守卫挡下，**根本没走到验证**。而那条守卫用的还是下面那个错方法。
//      ② `measure_fb_*` 用"`glReadPixels` 越界会报错"来量帧缓冲 —— **该判据不存在**
//         （越界读的值是未定义的，但调用成功、不报错）⇒ 已整段删除。
//      ③ 我给的第二组命令行参数与默认值**完全相同** ⇒ 两次输出一样。
//  ============================================================================
//
//  【现在怎么做】
//      ① 默认矩形 = 装得进 640×480 且**刻意不对称**：l=60, b=60, w=400, h=300
//         ⇒ 上边 360 < 480 ✓，且上下明显不同（y 翻转做错会被抓到）
//      ② **原样复刻引擎那段夹取**（renderer.cpp:226-231），并打印夹取前后
//      ③ 用与引擎**完全相同**的公式算 glViewport / glScissor
//      ④ 用与引擎**完全相同**的正交约定 glOrtho(0, w, h, 0, -1, 1)
//         （⇒ y=0 在底部、+y 向上；因为 bottom/top 是裁剪面坐标）
//      ⑤ 在 (l+5, b+10) 画 10x10 洋红方块
//      ⑥ `glReadPixels` **整个帧缓冲**，逐像素找方块 ⇒ 报它**实际**落在哪几行/列
//      ⑦ 与预期对照 ⇒ 若正好差一次镜像 ⇒ 报"**翻转做了两次**"
//      ⑧ 另加：视口外是否为黑（验证"限制清屏范围的是 scissor"）+ 纯算术自检
//
//  【它不进引擎源码、不进 CMake】
//  编译与运行见本文件末尾。
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>
#include <vector>

namespace {

// 引擎的 dst 是 +y 向上的窗口坐标矩形（`b` 是**下边**）
struct win_rect { int l = 0; int b = 0; int w = 0; int h = 0; };
struct gl_box  { int x = 0; int y = 0; int w = 0; int h = 0; };

/// 原样复刻 renderer.cpp:216-219 + 226-231（取整与夹取）
gl_box engine_box(win_rect r, int fb_w, int fb_h) {
    int vx = r.l;
    int vy = r.b;
    int vw = r.w;
    int vh = r.h;

    if (vx < 0) { vw += vx; vx = 0; }
    if (vy < 0) { vh += vy; vy = 0; }
    if (vx + vw > fb_w) { vw = fb_w - vx; }
    if (vy + vh > fb_h) { vh = fb_h - vy; }
    if (vw < 0) { vw = 0; }
    if (vh < 0) { vh = 0; }

    gl_box box{};
    box.x = vx;
    box.y = fb_h - (vy + vh);      // ⚠️ **唯一的那次 y 翻转**
    box.w = vw;
    box.h = vh;
    return box;
}

/// 对照：若有人以为 `b` 是**上边**（+y 向下），他会这么算
gl_box alt_box(win_rect r) {
    gl_box box{};
    box.x = r.l; box.y = r.b; box.w = r.w; box.h = r.h;
    return box;
}

std::vector<unsigned char> read_all(int w, int h) {
    std::vector<unsigned char> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
    while (glGetError() != GL_NO_ERROR) { }
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    const GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        std::printf("    !! glReadPixels 报错 0x%04X\n", static_cast<unsigned>(e));
    }
    return buf;
}

std::vector<unsigned char> read_rect(int x, int y, int w, int h) {
    std::vector<unsigned char> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
    while (glGetError() != GL_NO_ERROR) { }
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    return buf;
}

bool is_marker(const unsigned char* p) {
    return p[0] > 200 && p[1] < 60 && p[2] > 200;   // 洋红
}

} // namespace

int main(int argc, char** argv) {
    int win_w = 640, win_h = 480;

    // 刻意不对称、且装得进 640x480（上边 360 < 480）
    win_rect dst{};
    dst.l = 60; dst.b = 60; dst.w = 400; dst.h = 300;

    if (argc >= 3) { win_w = std::atoi(argv[1]); win_h = std::atoi(argv[2]); }
    if (argc >= 7) {
        dst.l = std::atoi(argv[3]); dst.b = std::atoi(argv[4]);
        dst.w = std::atoi(argv[5]); dst.h = std::atoi(argv[6]);
    }

    std::printf("=============================================================\n");
    std::printf(" y 翻转（窗口坐标 -> 帧缓冲）验证探针（第二版）\n");
    std::printf("=============================================================\n\n");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("yflip probe", win_w, win_h,
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

    // 帧缓冲尺寸：SDL 与 GL 初值两条来源对照
    int fb_w = 0, fb_h = 0;
    SDL_GetWindowSizeInPixels(win, &fb_w, &fb_h);
    int vp_init[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp_init);

    std::printf("【一】输入\n");
    std::printf("    帧缓冲（SDL_GetWindowSizeInPixels） = %d x %d\n", fb_w, fb_h);
    std::printf("    帧缓冲（GL 视口初值）               = %d x %d\n", vp_init[2], vp_init[3]);
    std::printf("    ⇒ 两条来源一致？ %s\n",
                (fb_w == vp_init[2] && fb_h == vp_init[3]) ? "是 ✓" : "**否** ⚠️");
    std::printf("    视口矩形(+y 向上) = l=%d b=%d w=%d h=%d\n", dst.l, dst.b, dst.w, dst.h);
    std::printf("      ⇒ 它的上边 = %d（= b + h）\n", dst.b + dst.h);
    std::printf("\n");

    // 取一来源作为 fb 尺寸（若两者不一致，用较小的那个，保证不越界）
    if (vp_init[2] > 0 && vp_init[3] > 0) {
        if (vp_init[2] < fb_w) { fb_w = vp_init[2]; }
        if (vp_init[3] < fb_h) { fb_h = vp_init[3]; }
    }
    if (fb_w <= 0 || fb_h <= 0) {
        std::printf("帧缓冲尺寸无效 ⇒ 退出\n");
        SDL_GL_DestroyContext(gl);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    const gl_box box = engine_box(dst, fb_w, fb_h);
    const gl_box alt = alt_box(dst);
    const bool clamped = (box.w != dst.w) || (box.h != dst.h) || (box.x != dst.l) || (box.y != fb_h - (dst.b + dst.h));

    std::printf("【二】按引擎公式算参数（renderer.cpp:216-219 + 226-231 + 254）\n");
    std::printf("    引擎公式：            glScissor(x=%d, y=%d, w=%d, h=%d)\n", box.x, box.y, box.w, box.h);
    std::printf("    对照（把 b 当上边）： glScissor(x=%d, y=%d, w=%d, h=%d)\n", alt.x, alt.y, alt.w, alt.h);
    std::printf("    夹取是否改变了矩形：%s\n", clamped ? "**是**（矩形超出帧缓冲）" : "否（矩形装得下）");
    std::printf("\n");

    if (box.w <= 0 || box.h <= 0) {
        std::printf("⚠️ 夹取后矩形为空 ⇒ 请给一个与帧缓冲相交的矩形\n");
        SDL_GL_DestroyContext(gl);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    // ---------------------------------------------------------------- 画
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_SCISSOR_TEST);
    glScissor(box.x, box.y, box.w, box.h);
    glViewport(box.x, box.y, box.w, box.h);
    glClearColor(0.0f, 0.0f, 0.30f, 1.0f);   // 视口区：深蓝
    glClear(GL_COLOR_BUFFER_BIT);

    // 与引擎一致的正交约定：glOrtho(0, fb_w, fb_h, 0, ...)
    //   ⇒ bottom/top 是裁剪面坐标 ⇒ y=0 落在**底部**、+y 向上
    glDisable(GL_SCISSOR_TEST);
    glViewport(box.x, box.y, box.w, box.h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, static_cast<double>(fb_w),
            static_cast<double>(fb_h), 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // 方块：+y 向上坐标 (dst.l+5, dst.b+10)，10x10
    const int qx = dst.l + 5;
    const int qy = dst.b + 10;
    glColor3f(1.0f, 0.0f, 1.0f);            // 洋红
    glBegin(GL_QUADS);
    glVertex2i(qx, qy);
    glVertex2i(qx + 10, qy);
    glVertex2i(qx + 10, qy + 10);
    glVertex2i(qx, qy + 10);
    glEnd();
    glFinish();

    std::printf("【三】画了一个 10x10 的洋红方块\n");
    std::printf("    +y 向上坐标 = (%d, %d) .. (%d, %d)\n", qx, qy, qx + 10, qy + 10);
    std::printf("    ⇒ 预期：下边距帧缓冲底 %d 像素、上边距底 %d 像素\n", qy, qy + 10);
    std::printf("    ⇒ 预期：列 = %d .. %d\n", qx, qx + 10);
    std::printf("\n");

    // ---------------------------------------------------------------- 量
    std::printf("【四】测量（glReadPixels 全帧缓冲，逐像素找方块）\n");
    const std::vector<unsigned char> full = read_all(fb_w, fb_h);

    int min_x = fb_w, max_x = -1, min_y = fb_h, max_y = -1, count = 0;
    for (int y = 0; y < fb_h; ++y) {
        for (int x = 0; x < fb_w; ++x) {
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(fb_w)
                              + static_cast<size_t>(x)) * 4u;
            if (is_marker(&full[i])) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
                ++count;
            }
        }
    }
    // ⚠️ glReadPixels 的 y=0 是**帧缓冲最下面那一行**（GL 规范）
    std::printf("    洋红像素个数 = %d\n", count);
    if (count > 0) {
        std::printf("    实测列 = %d .. %d     （预期 %d .. %d）\n", min_x, max_x, qx, qx + 10);
        std::printf("    实测行 = %d .. %d     （y=0 是最下面那一行）\n", min_y, max_y);
        std::printf("                          （预期 %d .. %d）\n", qy, qy + 10);
    } else {
        std::printf("    ⚠️ 找不到洋红像素 ⇒ 方块被画到别处或没画出来\n");
    }
    std::printf("\n");

    const std::vector<unsigned char> one = read_rect(qx + 2, qy + 2, 1, 1);
    std::printf("【五】独立核对：只读 (x=%d, y=%d) 这一个像素\n", qx + 2, qy + 2);
    std::printf("    RGBA = %d, %d, %d, %d\n", one[0], one[1], one[2], one[3]);
    std::printf("    ⇒ %s\n", is_marker(one.data()) ? "是洋红 ✓" : "**不是洋红** ✗");
    std::printf("\n");

    // ---------------------------------------------------------------- 判据
    std::printf("【六】判据\n");
    const bool ok_cols = (count > 0) && (min_x == qx) && (max_x == qx + 9);
    const bool ok_rows = (count > 0) && (min_y == qy) && (max_y == qy + 9);
    if (ok_rows) {
        std::printf("    ✅ 行数吻合（下边距帧缓冲底 = %d，与 +y 向上坐标里的 b+10 一致）\n", qy);
        std::printf("       ⇒ 即：**引擎那次 fb_h - (vy + vh) 的翻转是正确的**\n");
        std::printf("       ⇒ 且它把矩形整体搬对了位置（不是只搬了一个点）\n");
    } else if (count > 0) {
        std::printf("    ⚠️ 行数不吻合（实测 %d .. %d，预期 %d .. %d）\n", min_y, max_y, qy, qy + 10);
        const int mirrored = fb_h - (max_y + 1);
        std::printf("       实测下边 = %d；按 fb_h 镜像 = %d\n", min_y, mirrored);
        if (mirrored == qy) {
            std::printf("       ⇒ **正好差一次 y 翻转** ⇒ 说明翻转做了两次（或一次都没做对）\n");
        } else {
            std::printf("       ⇒ 既不是吻合也不是单纯镜像 ⇒ 另有原因\n");
        }
    }
    if (!ok_cols && count > 0) {
        std::printf("    ⚠️ 列数也不吻合（实测 %d .. %d，预期 %d .. %d）\n", min_x, max_x, qx, qx + 10);
    }
    std::printf("\n");

    // ---------------------------------------------------------------- 裁剪范围
    std::printf("【七】裁剪是否真的生效（视口外有没有被写到）\n");
    const int out_x = dst.l + 50;
    const int above_fb_y = fb_h - (dst.b + dst.h + 20);
    const int below_fb_y = fb_h - (dst.b - 20);
    if (above_fb_y >= 0 && above_fb_y < fb_h) {
        const std::vector<unsigned char> p = read_rect(out_x, above_fb_y, 1, 1);
        std::printf("    视口之上 (x=%d, y=%d) = %d, %d, %d ⇒ 应是黑：%s\n",
                    out_x, above_fb_y, p[0], p[1], p[2],
                    (p[0] == 0 && p[1] == 0 && p[2] == 0) ? "✓" : "**不是黑** ⚠️");
    } else {
        std::printf("    视口之上超出帧缓冲 ⇒ 跳过\n");
    }
    if (below_fb_y >= 0 && below_fb_y < fb_h) {
        const std::vector<unsigned char> p = read_rect(out_x, below_fb_y, 1, 1);
        std::printf("    视口之下 (x=%d, y=%d) = %d, %d, %d ⇒ 应是黑：%s\n",
                    out_x, below_fb_y, p[0], p[1], p[2],
                    (p[0] == 0 && p[1] == 0 && p[2] == 0) ? "✓" : "**不是黑** ⚠️");
    } else {
        std::printf("    视口之下超出帧缓冲 ⇒ 跳过\n");
    }
    std::printf("    （这一项验证 renderer.cpp 里那句「真正限制清屏范围的是 scissor」）\n");
    std::printf("\n");

    // ---------------------------------------------------------------- 纯算术
    std::printf("【八】纯算术自检（不依赖任何 API，可手工核对）\n");
    std::printf("    fb_h                = %d\n", fb_h);
    std::printf("    dst.b（下边）       = %d\n", dst.b);
    std::printf("    dst.h               = %d\n", dst.h);
    std::printf("    dst.b + dst.h（上边）= %d\n", dst.b + dst.h);
    std::printf("    fb_h - (b + h)      = %d   ⇒ glScissor 的 y\n", fb_h - (dst.b + dst.h));
    std::printf("    scissor 覆盖的行    = %d .. %d（从底往上数）\n", box.y, box.y + box.h);
    std::printf("    ⇒ 它应当与「上边 %d、下边 %d」这个区间对应\n", dst.b + dst.h, dst.b);
    std::printf("\n");

    std::printf("按回车退出（或等 20 秒）。\n");
    {
        const Uint64 deadline = SDL_GetTicks() + 20000;
        bool quit = false;
        while (!quit && SDL_GetTicks() < deadline) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_KEY_DOWN ||
                    e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) { quit = true; }
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
//  g++ -std=c++20 -O1 -g -IC:/msys64/ucrt64/include sdl3_yflip_probe.cpp \
//      -o bin/sdl3_yflip_probe.exe -LC:/msys64/ucrt64/lib -lSDL3 -lopengl32
//
//  ./bin/sdl3_yflip_probe.exe > yflip.txt 2>&1; cat yflip.txt
//  ./bin/sdl3_yflip_probe.exe 640 480 60 60 400 300
//  ./bin/sdl3_yflip_probe.exe 800 600 100 100 500 400
// =============================================================================
