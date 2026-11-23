// =============================================================================
//  devtools/seam.hpp —— 开发期工具接进帧管线的**唯一入口**
//
//  【为什么要有这一层】引擎不该认识工具。
//      frame.cpp / engine.cpp 只 include 本头；工具的头（trace.hpp / img_probe.hpp）
//      只在本头里出现，而且**只在编译期开关打开时**才出现。
//
//  【关掉时是零】两个开关都没定义时：
//      · WAG_SEAM_* 宏展开成 ((void)0)  ⇒ 热路径零分支、零调用；
//      · on_frame_begin / on_before_present 是 inline 空函数 ⇒ 零链接依赖。
//      这是"观测不得改变被观测者"的落点：不是运行期判断后不走，是编译出来就是空。
//
//  【接缝范围（刻意不越界）】
//      接：12 个阶段的**边界**、present 之前的那一瞬。
//      不接：阶段内部（例如阶段 6 的碰撞派发位置将来可能变）。
//      理由：边界是规格（"阶段的执行顺序就是内核的规格"），内部还不是。
//
//  【WAG_SEAM_ENTER 的参数是阶段名（裸 token）】
//      例：WAG_SEAM_ENTER(frame_begin)  ⇒  ::wag::trace::stage::frame_begin
//      这么写是为了让 frame.cpp 里的那一行**读起来就是阶段名**，
//      而不是一句 ::wag::trace::stage::... 的噪声。
// =============================================================================
#pragma once

#include <cstdint>

#if defined(WAG_DEVTOOLS_TRACE)
#    include <trace.hpp> // 来自 devtools/trace_probe（include 路径由 CMake 在开启时提供）
#endif

// ---- 阶段边界（trace）-------------------------------------------------------
#if defined(WAG_DEVTOOLS_TRACE)
#    define WAG_SEAM_ENTER(s)       ::wag::trace::enter(::wag::trace::stage::s)
#    define WAG_SEAM_EXIT(s)        ::wag::trace::exit(::wag::trace::stage::s)
#    define WAG_SEAM_EVENT(d, a, b) ::wag::trace::event(::wag::trace::domain::d, a, b)
#    define WAG_SEAM_MARK(x)        ::wag::trace::mark(x)
#else
#    define WAG_SEAM_ENTER(s)       ((void)0)
#    define WAG_SEAM_EXIT(s)        ((void)0)
#    define WAG_SEAM_EVENT(d, a, b) ((void)0)
#    define WAG_SEAM_MARK(x)        ((void)0)
#endif
// 注意：WAG_SEAM_SET_FRAME 不在这一组里 —— 它必须同时服务 probe（帧计数），
//   所以定义在下面"两个工具共用"的那一段。分开放会重定义（踩过）。

// ---- 帧开始 / present 之前（两个工具共用）-----------------------------------
#if defined(WAG_DEVTOOLS_TRACE) || defined(WAG_DEVTOOLS_PROBE)
namespace wag::devtools {
/// 一次性初始化（读环境变量、注册 atexit dump、给 trace 定时间原点）。
/// 【必须在任何记录之前调用】否则 reset 会把已经写下的记录擦掉（踩过）。
void ensure_init() noexcept;
/// 记下本帧帧号（放在阶段 1 之后：帧号在阶段 1 内才递增）。每帧一次。
void set_frame(std::uint64_t frame_count) noexcept;
} // namespace wag::devtools

#    define WAG_SEAM_INIT()      ::wag::devtools::ensure_init()
#    define WAG_SEAM_SET_FRAME(n) ::wag::devtools::set_frame(n)
#else
#    define WAG_SEAM_INIT()       ((void)0)
#    define WAG_SEAM_SET_FRAME(n) ((void)0)
#endif

// 【present 之前的探针】刻意留成调用点上的 #if 块，而不是宏：
//   它需要先问窗口要像素尺寸（一次平台调用），而"关掉时连这次调用都不该发生"
//   这件事必须让读 frame.cpp 的人一眼看见，不能藏进宏里。
#if defined(WAG_DEVTOOLS_PROBE)
namespace wag::devtools {
void on_before_present(double fb_w, double fb_h) noexcept;
} // namespace wag::devtools
#endif
