// =============================================================================
//  sdl3_yflip2_probe —— **定义性地**测出"引擎那条链用的是哪个 y 方向"
//
//  ============================================================================
//  【前两版为什么都不成立（留在这里，免得重犯）】
//      第一版：`glReadPixels` 越界**不会**报错（规范里没有那条错误条件）
//              ⇒ 用它量帧缓冲尺寸的整套判据**不存在** ⇒ 已删。
//      第二版：`glOrtho(left, right, bottom, top, ...)` 的第 3、4 个参数是
//              **bottom 与 top**，而我在两版里都写成了 `glOrtho(0, w, h, 0, ...)`
//              ⇒ bottom=h、top=0 ⇒ **那是 y 向下**；而实测值连"y 向下"都对不上
//               （只找到 36 个洋红像素而非 100）⇒ 结论不可用 ⇒ 已作废。
//  ============================================================================
//
//  【本版的三条原则】
//      ① **不用 glOrtho**。改用**显式 NDC**，把我要的窗口坐标自己算出来：
//             ndc_x = 2*(x - vx)/vw - 1
//             ndc_y = 2*(y - vy)/vh - 1          （对 y 向上的窗口坐标成立）
//          ⇒ 于是"世界→窗口"的映射由本探针**手算**，不依赖任何投影函数的参数顺序
//      ② **打印 GL 的读回值**（`GL_VIEWPORT` / `GL_SCISSOR_BOX`）
//          ⇒ 确认 GL 真的收到了我以为的参数
//      ③ **画一个非对称的"十字"**而不是方块：
//             横条：宽 20、高 4      竖条：宽 4、高 20     交叉点 = (qx, qy)
//          ⇒ 于是**方向立刻可辨**：横条若变竖条、或交叉点跑掉，一眼可见
//          ⇒ 而方块只给位置、不给方向
//
//  【判据（这是本探针的核心）】
//      用**两种映射各画一次十字**，看哪一次的交叉点落在预期的窗口坐标：
//          甲：按 **y 向上** 算 NDC
//          乙：按 **y 向下** 算 NDC
//      ⇒ 哪一个命中 ⇒ **引擎那条链用的就是那个方向**
//      ⇒ 甲命中 ⇒ 与本文档的"窗口坐标 = 左下原点、+y 向上"一致，
//                 且 `fb_h - (vy + vh)`（renderer.cpp:254）是**正确的那一次翻转**
//      ⇒ 乙命中 ⇒ 引擎链是 y 向下 ⇒ 那么 scissor 那一次减法
//                 在"内容"与"裁剪"之间**多翻了一次** ⇒ **真 bug**
//
//  【它不进引擎源码、不进 CMake】与 build/ 下其它探针同性质。
//  编译与运行见本文件末尾。
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>
#include <vector>

namespace {

struct win_rect { int l = 0; int b = 0; int w = 0; int h = 0; };
struct gl_box  { int x = 0; int y = 0; int w = 0; int h = 0; };

/// 原样复刻 renderer.cpp:216-219 + 226-231
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
    box.y = fb_h - (vy + vh);
    box.w = vw;
    box.h = vh;
    return box;
}

/// 把窗口坐标 (x, y) 算成 NDC。up = true 表示窗口坐标是 +y 向上。
void to_ndc(int x, int y, const gl_box& box, bool up, float& nx, float& ny) {
    nx = 2.0f * static_cast<float>(x - box.x) / static_cast<float>(box.w) - 1.0f;
    const float t = 2.0f * static_cast<float>(y - box.y) / static_cast<float>(box.h) - 1.0f;
    ny = up ? t : -t;
}

/// 一个轴对齐矩形，顶点用 NDC 给（左下、右下、右上、左上）。
void quad_ndc(const gl_box& box, bool up,
              int x0, int y0, int x1, int y1) {
    float ax, ay, bx, by, cx, cy, dx, dy;
    to_ndc(x0, y0, box, up, ax, ay);
    to_ndc(x1, y0, box, up, bx, by);
    to_ndc(x1, y1, box, up, cx, cy);
    to_ndc(x0, y1, box, up, dx, dy);
    glBegin(GL_QUADS);
    glVertex2f(ax, ay);
    glVertex2f(bx, by);
    glVertex2f(cx, cy);
    glVertex2f(dx, dy);
    glEnd();
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

bool is_marker(const unsigned char* p) {
    return p[0] > 200 && p[1] < 60 && p[2] > 200;
}

struct bounds { int min_x = 0, max_x = -1, min_y = 0, max_y = -1, count = 0; };

bounds scan_marker(const std::vector<unsigned char>& buf, int w, int h) {
    bounds b{};
    b.min_x = w; b.min_y = h;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w)
                              + static_cast<size_t>(x)) * 4u;
            if (is_marker(&buf[i])) {
                if (x < b.min_x) b.min_x = x;
                if (x > b.max_x) b.max_x = x;
                if (y < b.min_y) b.min_y = y;
                if (y > b.max_y) b.max_y = y;
                ++b.count;
            }
        }
    }
    return b;
}

} // namespace

int main(int argc, char** argv) {
    int win_w = 640, win_h = 480;
    win_rect dst{};
    dst.l = 60; dst.b = 60; dst.w = 400; dst.h = 300;

    if (argc >= 3) { win_w = std::atoi(argv[1]); win_h = std::atoi(argv[2]); }
    if (argc >= 7) {
        dst.l = std::atoi(argv[3]); dst.b = std::atoi(argv[4]);
        dst.w = std::atoi(argv[5]); dst.h = std::atoi(argv[6]);
    }

    std::printf("=============================================================\n");
    std::printf(" y 方向定义性测试（第三版：显式 NDC + 非对称十字）\n");
    std::printf("=============================================================\n\n");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("yflip2 probe", win_w, win_h,
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

    int fb_w = 0, fb_h = 0;
    SDL_GetWindowSizeInPixels(win, &fb_w, &fb_h);
    int vp_init[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp_init);

    std::printf("【一】帧缓冲尺寸（两条来源）\n");
    std::printf("    SDL_GetWindowSizeInPixels = %d x %d\n", fb_w, fb_h);
    std::printf("    GL 视口初值               = %d x %d\n", vp_init[2], vp_init[3]);
    if (vp_init[2] > 0 && vp_init[2] < fb_w) { fb_w = vp_init[2]; }
    if (vp_init[3] > 0 && vp_init[3] < fb_h) { fb_h = vp_init[3]; }
    std::printf("    本探针实际采用            = %d x %d\n", fb_w, fb_h);
    std::printf("\n");

    const gl_box box = engine_box(dst, fb_w, fb_h);
    std::printf("【二】视口盒（引擎公式 renderer.cpp:216-219 + 226-231 + 254）\n");
    std::printf("    输入（+y 向上窗口坐标）= l=%d b=%d w=%d h=%d，上边=%d\n",
                dst.l, dst.b, dst.w, dst.h, dst.b + dst.h);
    std::printf("    算出 glViewport / glScissor = (x=%d, y=%d, w=%d, h=%d)\n",
                box.x, box.y, box.w, box.h);
    if (box.w <= 0 || box.h <= 0) {
        std::printf("    ⚠️ 盒子为空 ⇒ 退出\n");
        SDL_GL_DestroyContext(gl);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    std::printf("\n");

    // ⚠️【关键修正】先把矩阵状态**显式设成单位矩阵**。
    //   上一版（以及第二版）两次都失败，最可能的原因就是**残留的矩阵状态**：
    //   本探针从不设 `glMatrixMode` / `glLoadIdentity`，而**同一个进程里先前
    //   的绘制路径设过**（GL 是状态机，矩阵属于上下文）。
    //   ⇒ 于是"我按 NDC 送的顶点"被一个非单位矩阵又变了一次。
    //   ⇒ 本探针只用**显式 NDC**（不依赖任何投影函数），所以投影与模型视图
    //     都必须是单位矩阵 —— 这是本探针成立的前提，必须写死。
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // 之后所有绘制都在这个盒子里；十字的交叉点放在 (qx, qy)
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(box.x, box.y, box.w, box.h);
    glViewport(box.x, box.y, box.w, box.h);
    glClearColor(0.0f, 0.0f, 0.30f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // 把当前矩阵读回来（矩阵在核心 GL 1.1 里可以用 glGetFloatv(GL_PROJECTION_MATRIX)）
    float proj[16] = {0};
    float mv[16] = {0};
    glGetFloatv(GL_PROJECTION_MATRIX, proj);
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    const bool proj_is_identity = (proj[0] == 1.0f) && (proj[5] == 1.0f) &&
                                  (proj[10] == 1.0f) && (proj[15] == 1.0f) &&
                                  (proj[1] == 0.0f) && (proj[4] == 0.0f);
    const bool mv_is_identity = (mv[0] == 1.0f) && (mv[5] == 1.0f) &&
                                (mv[10] == 1.0f) && (mv[15] == 1.0f) &&
                                (mv[1] == 0.0f) && (mv[4] == 0.0f);

    int vp_now[4] = {0, 0, 0, 0};
    int sc_now[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp_now);
    glGetIntegerv(GL_SCISSOR_BOX, sc_now);
    std::printf("【三】GL 的读回值（确认它真的收到了我以为的参数）\n");
    std::printf("    投影矩阵是单位矩阵：%s\n", proj_is_identity ? "是 ✓" : "**否** ⚠️");
    std::printf("    模型视图是单位矩阵：%s\n", mv_is_identity ? "是 ✓" : "**否** ⚠️");
    std::printf("    glGetIntegerv(GL_VIEWPORT)    = (x=%d, y=%d, w=%d, h=%d)\n",
                vp_now[0], vp_now[1], vp_now[2], vp_now[3]);
    std::printf("    glGetIntegerv(GL_SCISSOR_BOX) = (x=%d, y=%d, w=%d, h=%d)\n",
                sc_now[0], sc_now[1], sc_now[2], sc_now[3]);
    const bool vp_ok = (vp_now[0] == box.x) && (vp_now[1] == box.y) &&
                       (vp_now[2] == box.w) && (vp_now[3] == box.h);
    std::printf("    ⇒ 视口读回与设定一致：%s\n", vp_ok ? "是 ✓" : "**否** ⚠️");
    std::printf("\n");

    // 十字的几何：交叉点 (qx, qy)
    const int qx = dst.l + 5;
    const int qy = dst.b + 10;
    const int arm = 20;
    const int thick = 4;
    const int half = thick / 2;

    std::printf("【四】十字几何（把两个映射各画一次，互不干扰地各测一次）\n");
    std::printf("    交叉点（+y 向上窗口坐标）= (%d, %d)\n", qx, qy);
    std::printf("    横条：x %d .. %d，y %d .. %d（宽 %d、高 %d）\n",
                qx - arm, qx + arm, qy - half, qy - half + thick, arm * 2, thick);
    std::printf("    竖条：x %d .. %d，y %d .. %d（宽 %d、高 %d）\n",
                qx - half, qx - half + thick, qy - arm, qy + arm, thick, arm * 2);
    std::printf("    ⇒ 预期（y=0 在帧缓冲**最下面**）：\n");
    std::printf("         交叉点窗口 x = %d、y = %d\n", qx, qy);
    std::printf("         横向范围 %d .. %d（比纵向宽）\n", qx - arm, qx + arm);
    std::printf("         纵向范围 %d .. %d（比横向高）\n", qy - arm, qy + arm);
    std::printf("\n");

    struct trial { const char* name; bool up; };
    const trial trials[2] = { { "甲 · 按 y 向上算 NDC", true },
                              { "乙 · 按 y 向下算 NDC", false } };

    int hit_x[2] = {-1, -1}, hit_y[2] = {-1, -1};
    int cnt[2] = {0, 0};

    for (int t = 0; t < 2; ++t) {
        // ⚠️ 每一轮都重设矩阵状态：本探针只用显式 NDC，所以投影与模型视图
        //   都必须是单位矩阵。GL 是状态机 ⇒ 不能在两次试验之间假定状态没变。
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        // 清成深蓝（盒内），盒外黑
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(box.x, box.y, box.w, box.h);
        glViewport(box.x, box.y, box.w, box.h);
        glClearColor(0.0f, 0.0f, 0.30f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // 画十字（洋红）
        glColor3f(1.0f, 0.0f, 1.0f);
        // 横条
        quad_ndc(box, trials[t].up, qx - arm, qy - half, qx + arm, qy - half + thick);
        // 竖条
        quad_ndc(box, trials[t].up, qx - half, qy - arm, qx - half + thick, qy + arm);
        glFinish();

        const std::vector<unsigned char> buf = read_all(fb_w, fb_h);
        const bounds b = scan_marker(buf, fb_w, fb_h);
        cnt[t] = b.count;
        if (b.count > 0) {
            // 交叉点：横条与竖条的重叠区 = 一条 thick x thick 的块
            // 它的中心应最接近 (qx, qy)。这里用"重叠区"推断：
            //   横条的 y 范围 = 唯一那个高度为 thick 的带
            //   竖条的 x 范围 = 唯一那个宽度为 thick 的带
            // 简化：直接取"整体包围盒"，再报出它的宽高比
            hit_x[t] = (b.min_x + b.max_x) / 2;
            hit_y[t] = (b.min_y + b.max_y) / 2;
        }

        std::printf("  【%s】\n", trials[t].name);
        std::printf("      洋红像素数 = %d\n", b.count);
        if (b.count > 0) {
            std::printf("      整体列 = %d .. %d （宽 %d）\n", b.min_x, b.max_x, b.max_x - b.min_x + 1);
            std::printf("      整体行 = %d .. %d （高 %d）（y=0 是最下面）\n",
                        b.min_y, b.max_y, b.max_y - b.min_y + 1);
            std::printf("      包围盒中心 = (%d, %d)\n", hit_x[t], hit_y[t]);
            const int w_span = b.max_x - b.min_x + 1;
            const int h_span = b.max_y - b.min_y + 1;
            if (w_span > h_span) {
                std::printf("      ⇒ 横向更宽 ⇒ 十字**方向正确**（横条仍是横的）\n");
            } else {
                std::printf("      ⇒ ⚠️ 纵向更高 ⇒ **方向被翻了**（横条变成了竖条）\n");
            }
            const int want_cx = qx;
            const int want_cy = qy;
            const int dx = hit_x[t] - want_cx;
            const int dy = hit_y[t] - want_cy;
            std::printf("      与预期中心 (%d, %d) 的差 = (%+d, %+d)\n", want_cx, want_cy, dx, dy);
            std::printf("      ⇒ %s\n",
                        (dx >= -4 && dx <= 4 && dy >= -4 && dy <= 4)
                            ? "**命中**（差在容差内）✓"
                            : "**未命中**");
        } else {
            std::printf("      ⚠️ 找不到洋红像素\n");
        }
        std::printf("\n");
    }

    // ---------------------------------------------------------------- 判据
    std::printf("【五】判据（哪一个映射命中 ⇒ 那就是引擎链用的方向）\n");
    const bool ok_up = (cnt[0] > 0) &&
                       (hit_x[0] >= qx - 4 && hit_x[0] <= qx + 4) &&
                       (hit_y[0] >= qy - 4 && hit_y[0] <= qy + 4);
    const bool ok_down = (cnt[1] > 0) &&
                         (hit_x[1] >= qx - 4 && hit_x[1] <= qx + 4) &&
                         (hit_y[1] >= qy - 4 && hit_y[1] <= qy + 4);

    std::printf("    甲（y 向上）命中：%s\n", ok_up ? "**是**" : "否");
    std::printf("    乙（y 向下）命中：%s\n", ok_down ? "**是**" : "否");
    std::printf("\n");
    if (ok_up && !ok_down) {
        std::printf("    ⇒ **引擎链用 y 向上** ⇒ 与文档的窗口坐标定义一致\n");
        std::printf("       ⇒ 即 renderer.cpp:254 的 `fb_h - (vy + vh)` 是**正确的那一次翻转**\n");
    } else if (ok_down && !ok_up) {
        std::printf("    ⇒ ⚠️ **引擎链用 y 向下** ⇒ 与文档定义**相反**\n");
        std::printf("       ⇒ 那么 scissor 那一次减法在「内容」与「裁剪」之间**多翻了一次** ⇒ **真 bug**\n");
    } else if (ok_up && ok_down) {
        std::printf("    ⇒ ⚠️ 两者都命中 ⇒ 本探针失去区分力（十字太对称？）⇒ 请换更不对称的矩形\n");
    } else {
        std::printf("    ⇒ ⚠️ 两者都不命中 ⇒ 本探针的假设仍不对（这与上一版同一个症状）\n");
        std::printf("       ⇒ 请把【三】的读回值与【四】的两组实测范围发回来\n");
    }
    std::printf("\n");

    std::printf("【六】纯算术自检\n");
    std::printf("    fb_h = %d\n", fb_h);
    std::printf("    dst.b = %d（下边）   dst.h = %d   上边 = %d\n", dst.b, dst.h, dst.b + dst.h);
    std::printf("    fb_h - (b + h) = %d    ⇒ glScissor 的 y\n", fb_h - (dst.b + dst.h));
    std::printf("    ⇒ scissor 覆盖的行 = %d .. %d（从底往上数）\n", box.y, box.y + box.h);
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
//  g++ -std=c++20 -O1 -g -IC:/msys64/ucrt64/include sdl3_yflip2_probe.cpp \
//      -o bin/sdl3_yflip2_probe.exe -LC:/msys64/ucrt64/lib -lSDL3 -lopengl32
//
//  ./bin/sdl3_yflip2_probe.exe > yflip3.txt 2>&1; cat yflip3.txt
//  ./bin/sdl3_yflip2_probe.exe 800 600 100 100 500 400 > yflip4.txt 2>&1; cat yflip4.txt
// =============================================================================
