// =============================================================================
//  sdl3_chain_probe —— 把整条坐标链的**方向**钉死（纯算术，不需要窗口）
//
//  ============================================================================
//  【为什么要做它 —— 一次真实的文档冲突】
//      两处源码对"窗口坐标的 +y 朝哪"给出了**相反**的说法：
//
//        ① `src/space/rect.hpp` 的 `from_rects`：
//               m11 = **-s.y**        （y 缩放取负 ⇒ 有一次镜像）
//           而它的注释却写着"世界坐标 +y 向上（已定）；窗口坐标 +y 向上（已定）"
//           ⇒ **两条 +y 同向，就不该出现负号** ⇒ 注释与代码互相矛盾。
//
//        ② `src/space/viewport.cpp` 的 `recompute`：
//               dst.b = (window_dip.h - dst.h) * 0.5;   // 注释写"居中（窗口原点在左下角）"
//           而 `renderer.cpp` 的 scissor 又写 `fb_h - (vy + vh)`（一次 y 翻转）
//           ⇒ 若窗口坐标真是"左下原点、+y 上"，那这次翻转是**多余**的（方向已一致）。
//
//      ⇒ 所以必须判定：**窗口坐标到底是 +y 向上还是向下？**（也就是 `rect.b`
//        是"下边"还是"上边"。）
//
//  【本探针的判据 —— 全部是"同一个量用两条独立路径算，看是否相等"】
//      路径甲：**用 `from_rects` 把逻辑坐标映到窗口坐标**（引擎的变换）
//      路径乙：**用 `framebuffer_box_for` 把视口矩形的顶点映到 GL 帧缓冲坐标**
//      路径丙：**从逻辑坐标直接映到 GL 帧缓冲坐标**（中心原点 +y 上 → GL 左下 +y 上）
//
//      ⇒ 若"甲 + 乙"与"丙"**处处相等**，则整条链**自洽**，而且它等价于
//        "逻辑(+y 上) → GL(左下 +y 上)"这一条**干净的两步映射** ——
//        也就是说：**窗口坐标是一次 y 镜像，而 scissor 那次翻转把它镜像回去**。
//      ⇒ 若不等，则链上有一步方向错了，而本探针会打出**差在哪**。
//
//  【它同时回答"窗口坐标的 b 是上边还是下边"】
//      甲 的输出 `y_window` 是"窗口坐标的 y"。把 乙 用在它上面时，
//      若我们**假定 `b` 是下边**（+y 上）应得到某个值；
//      而"乙 实际做的"是 `fb_h - (b + h)`。两者一比就知道引擎把 `b` 当成了什么。
//      ⇒ 探针会把这两种解释都算出来并对照，**不做猜测**。
//
//  ============================================================================
//  编译：注意 `-I src`
// =============================================================================

#include "space/rect.hpp"
#include "space/viewport.hpp"

#include <cstdio>

namespace {

using wag::space::framebuffer_box;
using wag::space::framebuffer_box_for;
using wag::space::from_rects;
using wag::space::rect;
using wag::space::transform;

int g_cases = 0;
int g_ab_mismatch = 0;
int g_interpret_says_top = 0;
int g_interpret_says_bottom = 0;
int g_discriminating = 0;      // 两条解释本身不等的组合数（也就是有区分力的那些）

/// 复刻 viewport::recompute 的 letterbox 那一小段（只算 dst）。
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

/// 跑一组：render 尺寸 + 窗口尺寸。
void run_case(double rw, double rh, double ww, double wh) {
    const rect render{-rw * 0.5, -rh * 0.5, rw, rh};
    const rect win{0.0, 0.0, ww, wh};
    const rect dst = letterbox_dst(render, win);
    const transform tr = from_rects(render, dst);

    // ---- 取逻辑坐标里的两个点：源矩形的下边中点与上边中点 ----
    const double xmid = render.l + render.w * 0.5;
    const double y_src_bottom = render.b;          // 逻辑上的"下"（-h/2）
    const double y_src_top = render.top();         // 逻辑上的"上"（+h/2）

    // ---- 路径甲：引擎变换 ----
    const double y_win_bottom = tr.apply_y(xmid, y_src_bottom);
    const double y_win_top = tr.apply_y(xmid, y_src_top);

    // ---- 路径乙：framebuffer_box_for（喂整个视口矩形，看它把哪条边放哪）----
    const framebuffer_box box = framebuffer_box_for(dst, win.w, win.h);

    // ---- 路径丙：直接从逻辑坐标映到 GL 帧缓冲坐标 ----
    //      中心原点 +y 上  →  左下原点 +y 上：
    //          x_gl = x_logical + win.w/2
    //          y_gl = y_logical + win.h/2
    //      （这是"干净的两步映射"的断言；下面用它反推"窗口坐标应当长什么样"）
    const double y_gl_bottom_direct = y_src_bottom + wh * 0.5;
    const double y_gl_top_direct = y_src_top + wh * 0.5;

    // =========================================================================
    //  判据 1：甲 与 丙 是否一致（在"窗口坐标就是左下原点 +y 上"的解释下）
    // =========================================================================
    //
    //  若窗口坐标 = 左下原点 +y 上，那它**就是** GL 帧缓冲坐标（本平台两者 1:1），
    //  于是 `y_win_*` 应当等于 `y_gl_*_direct`。
    const bool ab_equal = (y_win_bottom == y_gl_bottom_direct) &&
                          (y_win_top == y_gl_top_direct);
    if (!ab_equal) { ++g_ab_mismatch; }

    // =========================================================================
    //  判据 2：`framebuffer_box_for` 把 `dst.b` 当成哪条边？
    // =========================================================================
    //
    //  盒子给 GL 的 y 是"该矩形下边**距底**的距离"。
    //  ⚠️ 本判据的第一版**写错了**（把"b 是下边"的预测写成 `dst.b`，看似合理），
    //     而真正的两条解释是：
    //        A（`b` 是"距**顶**" ⇒ 窗口坐标 +y **下**）：
    //              下边距底 = **fb_h − (b + h)**
    //        B（`b` 是"距**底**" ⇒ 窗口坐标 +y **上**）：
    //              下边距底 = **b**
    //  ⚠️ 而且当 `fb_h == wh` 时 A 与 B **本来就相等**（见下面那条恒等式）
    //     ⇒ 真正有区分力的只有 `fb_h != wh` 的组合 ⇒ 那个计数单独报（g_discriminating）。
    const double box_y = static_cast<double>(box.y);
    const double pred_A_b_is_top = static_cast<double>(box.fb_h) - (dst.b + dst.h);
    const double pred_B_b_is_bottom = dst.b;

    if (box_y == pred_A_b_is_top) { ++g_interpret_says_top; }
    if (box_y == pred_B_b_is_bottom) { ++g_interpret_says_bottom; }
    if (pred_A_b_is_top != pred_B_b_is_bottom) { ++g_discriminating; }

    if (g_cases < 6) {
        std::printf("  [%d] render=%.0fx%.0f win=%.0fx%.0f  dst(l=%.1f b=%.1f w=%.1f h=%.1f)\n",
                    g_cases, rw, rh, ww, wh, dst.l, dst.b, dst.w, dst.h);
        std::printf("      变换: y逻辑 %.1f(下) -> 窗口 %.1f   |   y逻辑 %.1f(上) -> 窗口 %.1f\n",
                    y_src_bottom, y_win_bottom, y_src_top, y_win_top);
        std::printf("      直映: y逻辑 %.1f -> GL %.1f        |   y逻辑 %.1f -> GL %.1f\n",
                    y_src_bottom, y_gl_bottom_direct, y_src_top, y_gl_top_direct);
        std::printf("      甲==丙 ? %s       盒子 y = %d\n",
                    ab_equal ? "是" : "**否**", box.y);
        std::printf("      盒子 y 与「b 是距顶」的预测(%.1f) %s；与「b 是距底」的预测(%.1f) %s\n",
                    pred_A_b_is_top,
                    (box_y == pred_A_b_is_top) ? "**相同**" : "不同",
                    pred_B_b_is_bottom,
                    (box_y == pred_B_b_is_bottom) ? "**相同**" : "不同");
        std::printf("\n");
    }
    ++g_cases;
}

} // namespace

int main() {
    std::printf("=============================================================\n");
    std::printf(" 坐标链方向探针（纯算术，用引擎自己的 from_rects 与 framebuffer_box_for）\n");
    std::printf("=============================================================\n\n");

    std::printf("判据 1：`变换` 的输出是否等于「逻辑 +y 上 → GL 左下 +y 上」的直接映射？\n");
    std::printf("判据 2：`framebuffer_box_for` 把视口矩形的 `b` 当成**上边**还是**下边**？\n\n");

    const double rw[] = {1920.0, 1280.0, 960.0, 2560.0};
    const double rh[] = {1080.0, 720.0, 540.0, 1440.0};
    const double ww[] = {1920.0, 1280.0, 900.0, 1366.0, 2560.0};
    const double wh[] = {1080.0, 720.0, 600.0, 768.0, 1440.0};

    for (double a : rw) {
        for (double b : rh) {
            for (double c : ww) {
                for (double d : wh) {
                    run_case(a, b, c, d);
                }
            }
        }
    }

    std::printf("=============================================================\n");
    std::printf("【小结】\n");
    std::printf("    组合数                              = %d\n", g_cases);
    std::printf("    判据1（甲==丙）为**假**的组合数      = %d\n", g_ab_mismatch);
    std::printf("    盒子 y 与「b 是**距顶**」这条预测相同 = %d\n", g_interpret_says_top);
    std::printf("    盒子 y 与「b 是**距底**」这条预测相同 = %d\n", g_interpret_says_bottom);
    std::printf("      其中「两条预测本身不等」（= 真正有区分力） = %d\n", g_discriminating);
    std::printf("\n");
    std::printf("【怎么读】\n");
    std::printf("  · 若「判据1 为假」= 全部组合 ⇒ 变换的输出**不等于**左下原点 +y 上的坐标\n");
    std::printf("      ⇒ 也就是说**窗口坐标不是「左下原点 +y 上」**，而是**+y 向下**\n");
    std::printf("        （这正是 `from_rects` 里 `m11 = -s.y` 那个负号在说的）\n");
    std::printf("  · 若「判据1 为假」= 0 ⇒ 目录里的注释是对的，而 `m11` 的负号多余（那才是 bug）\n");
    std::printf("  · 判据2 的计数告诉我们在**实际算术里** `b` 被当成了哪条边\n");
    std::printf("  · 判据2 只有在「两条预测本身不等」的组合上才有区分力\n");
    std::printf("      ⇒ 那种组合共 %d 个（见上面「真正有区分力」那行）；\n", g_discriminating);
    std::printf("        其余组合下两条预测恰好相等 ⇒ 无区分力，不计入判断\n");
    std::printf("=============================================================\n");
    return 0;
}

// =============================================================================
//  编译与运行（MSYS2 UCRT64）
//
//  cd /c/Users/vqwjq/Documents/LuaWAG/LuaWAG-Vanilla
//  g++ -std=c++20 -O1 -g -Isrc -IC:/msys64/ucrt64/include sdl3_chain_probe.cpp \
//      -o bin/sdl3_chain_probe.exe -LC:/msys64/ucrt64/lib -lSDL3 -lopengl32
//
//  ./bin/sdl3_chain_probe.exe > chain.txt 2>&1; cat chain.txt
// =============================================================================
