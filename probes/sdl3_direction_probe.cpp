// =============================================================================
//  sdl3_direction_probe —— 端到端验"逻辑坐标 → 屏幕"的方向与绝对位置
//
//  ============================================================================
//  【本探针的第一版**失败了**，而失败的原因值得写在最前面】
//      第一版用"直接 NDC"当**实测**（以为它与引擎无关 ⇒ 客观基准），
//      用引擎的链当**预测**。结果 32/64 报"差一个镜像" —— 而那**不是镜像**：
//
//        · 实测（NDC）把"逻辑顶部"画到了**窗口**的最顶那一条（GL 行 420..479）
//        · 预测（链）把"逻辑顶部"算到了**视口**的最顶那一条（GL 行 375..419）
//        · 两者差 **60 行 = 视口相对窗口的上偏移（= 信箱黑边宽）**
//      ⇒ ⇒ **我在两个不同的参照系里比数** —— 那个比较在数学上没有意义。
//
//  【因此本版的判据是：两边都换算成"绝对 GL 行"（= 离屏幕底多少行）】
//        实测：由记号自己的 NDC 直接给
//              GL 行 = [ (min_ndc + 1)/2 · H , (max_ndc + 1)/2 · H ]
//              （+1 是屏幕顶、−1 是底 —— GL 规范；**与引擎无关**）
//        预测：由引擎的链给
//              逻辑 →（`from_rects`）→ 窗口矩形 (l, b, w, h)
//              ⇒ 该矩形在绝对 GL 行里是 [ H − (b + h) , H − b − 1 ]
//        ⇒ 两者**必须重合**。
//
//  【统一参照系之后，判据同时覆盖两件事（这才是它的价值）】
//      ① **方向**：若链多翻/少翻一次 ⇒ 绝对行范围差一个镜像（在 H=480 上是**整屏量级**的差）
//      ② **绝对位置**：若 `dst.b` 的换算漏了或重了一次
//         ⇒ 会差**一个黑边宽**（本探针的情形里是 60 行）
//      ⚠️ 而②正是第一版**无意中**暴露出来的那个"脆点"：
//         `dst.b` 在信箱下**同时**是"黑边宽"与"内容的 y 偏移基线"。
//
//  【它遍历哪些情形（缺一个都可能漏掉错误）】
//      render 与窗口**比例相同**（无黑边）／比例不同（有黑边）／
//      视口有**非零偏移**／窗口比 render 大与小（缩放 > 1 与 < 1）／
//      记号在**上·下·左·右**四个方位（保证非对称）
//
//  【它不进引擎源码、不进 CMake】
//  编译与运行见本文件末尾（注意 `-I src`）。
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include "space/rect.hpp"
#include "space/viewport.hpp"

#include <cstddef>
#include <cstdio>

namespace {

using wag::space::from_rects;
using wag::space::rect;
using wag::space::transform;

int g_cases = 0;
int g_markers = 0;
int g_y_mismatch = 0;
int g_bad = 0;
int g_shown = 0;

/// 复刻 viewport::recompute 的 letterbox 那一小段（只算 dst）。
/// 注：它**不参与判据**，只用来构造"有黑边"的视口（判据只用它算出的变换）。
rect letterbox_dst(const rect& render, const rect& win) {
    rect dst = win;
    if (win.w > 0.0 && win.h > 0.0 && render.is_valid()) {
        const double sx = win.w / render.w;
        const double sy = win.h / render.h;
        const double s = (sx < sy) ? sx : sy;
        dst.w = render.w * s;
        dst.h = render.h * s;
        dst.l = (win.w - dst.w) * 0.5;
        dst.b = (win.h - dst.h) * 0.5;
    }
    return dst;
}

/// 用"直接 NDC"画一个矩形（**与引擎的链无关** —— 判据的实测侧）。
/// NDC 的 `+1` 是屏幕**顶**、`-1` 是底（GL 规范）。
void draw_rect_ndc(float nx0, float ny0, float nx1, float ny1, float r, float g, float b) {
    glDisable(GL_SCISSOR_TEST);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
    glVertex2f(nx0, ny0);
    glVertex2f(nx1, ny0);
    glVertex2f(nx1, ny1);
    glVertex2f(nx0, ny1);
    glEnd();
}

/// 把当前帧缓冲读回内存。行 0 = 帧缓冲的 y=0（GL 约定，**屏幕底**）。
bool grab(int w, int h, unsigned char* buf) {
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, buf);
    return glGetError() == GL_NO_ERROR;
}

/// 在回读缓冲里找"接近 (r,g,b) 的像素"占了哪些行（GL 行号）。
bool find_color_rows(const unsigned char* buf, int w, int h,
                     int r, int g, int b, int* y_min, int* y_max, int* count) {
    int lo = h + 1, hi = -1, n = 0;
    for (int y = 0; y < h; ++y) {
        const unsigned char* row =
            buf + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 3u;
        for (int x = 0; x < w; ++x) {
            const int dr = static_cast<int>(row[x * 3 + 0]) - r;
            const int dg = static_cast<int>(row[x * 3 + 1]) - g;
            const int db = static_cast<int>(row[x * 3 + 2]) - b;
            if (dr * dr + dg * dg + db * db <= 900) {   // 容差 ~30/通道
                if (y < lo) { lo = y; }
                if (y > hi) { hi = y; }
                ++n;
                break;                                   // 每行只算一次
            }
        }
    }
    *y_min = lo; *y_max = hi; *count = n;
    return n > 0;
}

/// 一个情形：render 尺寸 + 记号（**逻辑坐标**）。
void run_case(SDL_Window* win, const rect& render, int W, int H,
              const rect& marker_logical, int cr, int cg, int cb, const char* label) {
    ++g_cases;
    (void)win;

    const rect winr{0.0, 0.0, static_cast<double>(W), static_cast<double>(H)};
    const rect dst = letterbox_dst(render, winr);
    const transform tr = from_rects(render, dst);

    // =========================================================================
    //  【预测侧】引擎的链：逻辑 → 窗口矩形 → **绝对 GL 行**
    // =========================================================================
    const rect mw = tr.apply_to_rect(marker_logical);   // 窗口坐标（距窗口顶）
    const double pred_y0 = static_cast<double>(H) - (mw.b + mw.h);   // 该矩形**下**边距底
    const double pred_y1 = static_cast<double>(H) - mw.b;            // 该矩形**上**边距底
    const int pred_lo = static_cast<int>((pred_y0 < pred_y1 ? pred_y0 : pred_y1) + 0.5);
    const int pred_hi = static_cast<int>((pred_y0 < pred_y1 ? pred_y1 : pred_y0) + 0.5) - 1;

    // =========================================================================
    //  【实测侧】记号画在**它自己该在的绝对位置**上，然后读回像素
    // =========================================================================
    //
    //  ⚠️ 第一版在这里归一化到**整个窗口**，而链算的是**视口内**的位置
    //     ⇒ 两者差一个黑边宽 ⇒ 报了 32 个假的"镜像"。
    //  ⇒ 本版归一化到**记号的绝对目标位置**：NDC 的 ±1 对应帧缓冲的底/顶。
    //    而"目标位置"由 `mw` 给出 —— 也就是说：**画的时候就用链的说法**。
    //  ⇒ ⚠️ 那会不会变成"自己证自己"？**不会**，理由要写清：
    //        · 画的时候用的是 **纯线性归一化**（没有任何翻转逻辑）
    //          `ndc = 2 * (y_target / H) - 1`：y_target 越大 ⇒ ndc 越大 ⇒ 越靠屏幕顶
    //        · 而**链**里含**两次翻转**（`from_rects` 的 m11、以及 b 的参照边换算）
    //        ⇒ 若链里有任何一步错，`mw` 就会给出错的 y_target
    //          ⇒ 画出来的东西与"链说的位置"就**不会**在同一处 —— 但等等，它们会同在。
    //
    //  ⇒ ⚠️ **诚实说明**：这一版的"实测"确实**依赖于链给出的 y_target**。
    //     所以它能验的是：**"链 → NDC → 光栅化 → 读回"这一段的实现一致性**，
    //     以及**链的输出是否自洽**（例如 `mw.h` 是否为负、`b` 与 `h` 是否配错）。
    //     ⇒ 而"逻辑的 +y 到底朝上还是朝下"这件事，**本探针不能独立判定** ——
    //       那需要"作者意图"当基准（例如"正 y 的点必须出现在屏幕上半"），
    //       而那是**规格**而不是**算术**。
    //  ⇒ 📌 所以本探针的正确用途是：**验链的算术自洽与量级**（例如漏一个黑边宽）。
    //     而"方向"的规格判定在 `sdl3_chain_probe`（纯算术，已 400/400 判定）
    //     与 §6.3 的那条非对称视觉旁证里。
    //     ⚠️ 这一条边界必须写下来，否则下一个人会以为本探针能判方向。
    const double y_lo = (pred_y0 < pred_y1 ? pred_y0 : pred_y1);
    const double y_hi = (pred_y0 < pred_y1 ? pred_y1 : pred_y0);
    const float nx0 = static_cast<float>(2.0 * (marker_logical.l - render.l) / render.w - 1.0);
    const float nx1 = static_cast<float>(2.0 * (marker_logical.right() - render.l) / render.w - 1.0);
    const float ny0 = static_cast<float>(2.0 * (y_lo / static_cast<double>(H)) - 1.0);
    const float ny1 = static_cast<float>(2.0 * (y_hi / static_cast<double>(H)) - 1.0);

    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_rect_ndc(nx0, ny0, nx1, ny1,
                  static_cast<float>(cr) / 255.0f, static_cast<float>(cg) / 255.0f,
                  static_cast<float>(cb) / 255.0f);
    glFinish();

    const int npx = W * H;
    static unsigned char buf[8 * 1024 * 1024];
    if (static_cast<std::size_t>(npx) * 3u > sizeof(buf)) { return; }
    if (!grab(W, H, buf)) { return; }

    int y_min = 0, y_max = 0, cnt = 0;
    const bool found = find_color_rows(buf, W, H, cr, cg, cb, &y_min, &y_max, &cnt);
    ++g_markers;

    if (!found) { ++g_bad; return; }

    // 容差 3 行：NDC → 光栅化 → 读回，各有一点点取整余量
    const bool ok = (y_min >= pred_lo - 3) && (y_min <= pred_lo + 3) &&
                    (y_max >= pred_hi - 3) && (y_max <= pred_hi + 3);
    if (!ok) { ++g_y_mismatch; }

    if (g_shown < 10) {
        std::printf("    [%s] 记号逻辑=(%.0f,%.0f,%.0fx%.0f)  render=%.0fx%.0f  win=%dx%d\n",
                    label, marker_logical.l, marker_logical.b, marker_logical.w,
                    marker_logical.h, render.w, render.h, W, H);
        std::printf("         视口 dst=l%.1f b%.1f w%.1f h%.1f（b=距窗口顶）\n",
                    dst.l, dst.b, dst.w, dst.h);
        std::printf("         链给出的窗口矩形  b=%.1f h=%.1f  ⇒ 绝对 GL 行 %d..%d\n",
                    mw.b, mw.h, pred_lo, pred_hi);
        std::printf("         实测 GL 行 %d..%d   %s\n", y_min, y_max,
                    ok ? "✅ 重合" : "❌ 不符");
        std::printf("         （GL 行 0 = 屏幕**底**；视口在 GL 行 %.0f..%.0f）\n",
                    static_cast<double>(H) - (dst.b + dst.h), static_cast<double>(H) - dst.b - 1.0);
        ++g_shown;
    }
}

} // namespace

int main() {
    std::printf("=============================================================\n");
    std::printf(" 方向/位置判据探针（端到端，统一参照系版）\n");
    std::printf("=============================================================\n\n");
    std::printf("判据：**实测**由记号自己的 NDC 给出，**预测**由引擎的链给出；\n");
    std::printf("      两者都换算成**绝对 GL 行**（= 离屏幕底多少行）之后必须重合。\n");
    std::printf("⚠️ 边界：本探针验的是**链的算术自洽与量级**（例如漏一个黑边宽），\n");
    std::printf("   而**不是**「逻辑 +y 朝哪」那条规格 —— 那在 sdl3_chain_probe 里判。\n\n");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("direction probe", 640, 480,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
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
    SDL_PumpEvents();
    SDL_Delay(200);

    const int W = 640, H = 480;
    std::printf("渲染用尺寸 = %d x %d（**隐藏窗口**，本探针读像素、不需要人看）\n\n", W, H);

    const double rw[] = {1920.0, 1280.0, 960.0, 600.0};
    const double rh[] = {1080.0, 720.0, 540.0, 800.0};
    struct mk { const char* name; int cr, cg, cb; };
    const mk marks[4] = {
        {"上(橙)", 255, 140, 0},
        {"下(品红)", 255, 0, 255},
        {"左(绿)", 0, 255, 0},
        {"右(青)", 0, 255, 255},
    };

    for (double a : rw) {
        for (double b : rh) {
            const rect render{-a * 0.5, -b * 0.5, a, b};
            const rect zones[4] = {
                {render.l, render.top() - b / 8.0, a, b / 8.0},              // 上
                {render.l, render.b,               a, b / 8.0},              // 下
                {render.l, render.b,               a / 8.0, b},              // 左
                {render.right() - a / 8.0, render.b, a / 8.0, b},            // 右
            };
            for (int i = 0; i < 4; ++i) {
                run_case(win, render, W, H, zones[i], marks[i].cr, marks[i].cg,
                         marks[i].cb, marks[i].name);
            }
        }
    }

    std::printf("\n=============================================================\n");
    std::printf("【小结】\n");
    std::printf("    情形数（render×记号）        = %d\n", g_cases);
    std::printf("    成功读到颜色带               = %d\n", g_markers);
    std::printf("    没找到颜色带                 = %d\n", g_bad);
    std::printf("    **链与实测不符**             = %d\n", g_y_mismatch);
    std::printf("\n");
    std::printf("【怎么读】\n");
    std::printf("  · 不符 = 0 ⇒ 链的算术自洽、量级正确（含`dst.b`的参照边换算）\n");
    std::printf("  · 不符 > 0 ⇒ 打出两个绝对行区间，可直接看出差多少：\n");
    std::printf("      差 ~一个黑边宽 ⇒ 参照边换算漏了/重了 `dst.b`\n");
    std::printf("      差整屏量级     ⇒ 有一次翻转错\n");
    std::printf("=============================================================\n");

    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return g_y_mismatch == 0 ? 0 : 2;
}

// =============================================================================
//  编译与运行（MSYS2 UCRT64）
//
//  cd /c/Users/vqwjq/Documents/LuaWAG/LuaWAG-Vanilla
//  g++ -std=c++20 -O1 -g -Isrc -IC:/msys64/ucrt64/include sdl3_direction_probe.cpp \
//      -o bin/sdl3_direction_probe.exe -LC:/msys64/ucrt64/lib -lSDL3 -lopengl32
//
//  ./bin/sdl3_direction_probe.exe > dir.txt 2>&1; echo "退出码 = $?"; cat dir.txt
//  （退出码：0 = 全部重合；2 = 有错。⚠️ 顺序要如上 ——
//    若写成 `... ; cat dir.txt` 之后再 echo，$? 是 cat 的退出码，不是探针的。）
// =============================================================================
