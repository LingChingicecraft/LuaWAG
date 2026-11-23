// =============================================================================
// wag/space/viewport.cpp
//
// 视口与策略的实现。
//
// 【为什么这个文件里几乎什么都没有】
// 因为视口的全部语义都是"数据"：它存两个字段、算一个派生量。真正需要
// 实现的只有两件小事 —— 策略字符串的解析、以及重新计算变换。
//
// 【为什么解析要写成函数而不是一张表】
// 只有两个取值。手写比较比建表更直接，也更容易看出"大小写敏感"这条
// 规则是怎么落实的（就是不用 strcasecmp）。
// =============================================================================

#include "space/viewport.hpp"

#include <algorithm>
#include <cstring>

namespace wag::space {

// -----------------------------------------------------------------------------
// parse_strategy
//
// 【大小写敏感是刻意保留的】
// 没有使用大小写不敏感的比较。理由：引擎不在"调用方想说什么"这件事上
// 做猜测 —— 同一个立场也出现在别处（例如不做按位置的自动换流）。
// 写错大小写应当得到一个明确的错误，而不是被悄悄接受。
//
// 【为什么用 strcmp 而不是 std::string】
// 因为本函数会被交界处用 C 字符串调用（Lua 给的字符串是 const char*），
// 而转换一次 std::string 是纯粹的浪费。strcmp 在两个字面量上的成本是
// 纳秒级。
// -----------------------------------------------------------------------------
bool parse_strategy(const char* name, fit_strategy& out) noexcept {
 // 空指针与空串都不是合法的策略名。
    if (name == nullptr) {
        return false;
    }

    if (std::strcmp(name, "letterbox") == 0) {
        out = fit_strategy::letterbox;
        return true;
    }
    if (std::strcmp(name, "stretch") == 0) {
        out = fit_strategy::stretch;
        return true;
    }

 // 未定义的名字：不改 out，返回 false。
 // 【调用方必须检查返回值】忽略它会让一个写错的策略名变成一个"看起来
 // 正常"的视口 —— 而那是静默的。
    return false;
}

const char* strategy_name(fit_strategy s) noexcept {
    switch (s) {
    case fit_strategy::letterbox:
        return "letterbox";
    case fit_strategy::stretch:
        return "stretch";
    }
 // 到达这里意味着枚举里出现了未处理的值。返回一个明确说"不知道"的字符串,
 // 而不是编一个名字出来 —— 诊断输出里出现 "?" 比出现一个错误的名字好。
    return "?";
}

// -----------------------------------------------------------------------------
// viewport
// -----------------------------------------------------------------------------

viewport::viewport()
 // 默认视口（已定）：1920 × 1080，信箱。
 //
 // 【render 矩形以**逻辑原点**为中心来表达】
 // 逻辑坐标的原点在窗口中心（见第八章），因此"可见区域的
 // 中心在逻辑原点"就是 l = −w/2, b = −h/2。
 // ⇒ 于是逻辑点 (0,0) 经变换后落在**窗口中心**，而不是某个角。
 //
 // 【为什么必须这样】若 render 从 (0,0) 开始，那么逻辑原点会被映到
 // "render 矩形的左下角" —— 于是"作者写 (0,0) = 屏幕中心"这件事不成立，
 // 而所有内容的位置都会依 render 的尺寸而漂移。
 //
 // ⇒ 一句话：**render 矩形的原点就是逻辑原点**；它在哪里，世界的 (0,0)
 // 就在屏幕上的哪里（信箱策略下就是窗口中心）。
    : render_{-960.0, -540.0, 1920.0, 1080.0},
      strategy_(fit_strategy::letterbox),
      viewport_{-960.0, -540.0, 1920.0, 1080.0},
      source_{-960.0, -540.0, 1920.0, 1080.0},
      transform_(transform::identity()) {
 // 构造时不重算变换。理由：那时还没有窗口尺寸快照，重算没有输入。
 // 帧结构的阶段 1 会在第一次 recompute() 时把它填对。
 // 在那之前 transform_ 是恒等变换 —— 而它在阶段 1 之前不会被任何绘制使用。
}

void viewport::recompute(const rect& window_dip) noexcept {
 // 唯一的写入点。transform_ 只能在这里被改。
 //
 // =========================================================================
 // 按策略算出**变换的目标矩形**与**屏幕上那个矩形**（两者相同）
 // =========================================================================
 //
 // 【为什么不是"改源矩形" —— 这一条我先前搞错了两轮】
 // `from_rects` 的语义是"把 src **非等比**铺满 dst"（两个方向各自缩放）。
 // 因此：
 // · src 与 dst 比例不同 ⇒ 变形（那就是 stretch 的行为）
 // · src 与 dst 比例相同 ⇒ 不变形，但**铺满** ⇒ 结果必然覆盖整个 dst
 // ⇒ 所以"把源矩形改成与窗口同比例"**不可能**产生黑边：它只会让内容
 // 铺满整窗而不变形。
 // ⇒ 信箱只能这样做：**让 dst 比窗口小**，多出来的部分不画。
 //
 // 【正确的形态】
 // letterbox：等比缩放 render_ 直到**装进**窗口 ⇒ dst = 居中的那个矩形
 // ⇒ 变换把 render_ 铺满 dst（此时比例相同 ⇒ 不变形、无黑边）
 // ⇒ 而窗口里 dst 之外的部分从未被写过 ⇒ 黑边
 // stretch：dst = 整个窗口 ⇒ 两个方向各自缩放 ⇒ 铺满且变形
 // =========================================================================
    rect dst = window_dip;

    if (strategy_ == fit_strategy::letterbox && window_dip.w > 0.0 && window_dip.h > 0.0 &&
        render_.is_valid()) {
 // 等比缩放系数：取"装得下"的那一个（取小 ⇒ contain）。
        const double s_fit = std::min(window_dip.w / render_.w, window_dip.h / render_.h);

        dst.w = render_.w * s_fit;
        dst.h = render_.h * s_fit;
 // 居中。
 //
 // 【窗口坐标 +y 向下】
 // `b` 在这个域里是“距顶”的距离。这里的“居中”是“上下边距相等”，
 // 这个条件在两种方向下都成立，所以这行代码本身不用改；
 // 但不要据此以为窗口坐标是 +y 向上。
        dst.l = (window_dip.w - dst.w) * 0.5;
        dst.b = (window_dip.h - dst.h) * 0.5;
    }
 // stretch：dst 保持 = 整个窗口。

 // ---- 唯一的映射：把 render_ 铺满 dst ----
 //
 // 【两种策略共用这一句，差别只在 dst 是什么】m01 与 m10 恒为 0 这条不变量
 // 由 from_rects 保证。
    transform_ = from_rects(render_, dst);

 // ---- 把"屏幕上那个矩形"留下（渲染层要用它设视口与 scissor）----
    viewport_ = dst;
    source_ = render_;
}

} // namespace wag::space
