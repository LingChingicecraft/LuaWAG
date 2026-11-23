// =============================================================================
//  trace.hpp —— 时序打点（"确定性系统的黑匣子"）
//
//  【它不是性能分析器】Tracy 回答"慢在哪"；本工具回答"这一帧发生了什么"。
//      所以载荷是"事件 + 两个数"，不是 span 树。
//
//  【唯一不可替代的设计：关的时候是**编译期**空的】
//      WAG_TRACE == 0 时所有宏展开成 ((void)0)，实现文件也不产生任何代码。
//      这不是"运行期判断后不走"——那是另一种观测扰动。
//
//  【为什么是静态环 + 帧末一次输出】
//      打点不能改变帧时间 ⇒ 热路径只做"写一条 32 字节"，没有格式化、没有 I/O、没有分配。
//      输出统一发生在帧末（present 之后），与引擎已有的"每秒一行统计"同一个位置。
//
//  【域是独立定义的，不侵入引擎阶段】
//      stage / domain 都在本文件里定义；引擎想用就 include，不想用也不受影响。
//      （这是"不与引擎强耦合"的落点。）
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>

// 编译期开关：0 = 全部消失（默认）；1 = 打进静态环。
#ifndef WAG_TRACE
#    define WAG_TRACE 0
#endif

#if WAG_TRACE

namespace wag::trace {

// -----------------------------------------------------------------------------
//  词汇
// -----------------------------------------------------------------------------
/// 12 个阶段（与《时间线与时序设计》的 12 阶段同名同序）。
/// 【为什么镜像一份而不是 include 引擎】本工具要能独立编译、独立运行。
enum class stage : std::uint16_t {
    frame_begin = 1,
    rate_control,
    event_consume,
    input_sample,
    ext_logic,
    main_logic,
    render_begin,
    scene_render,
    ext_render_end,
    snapshot,
    state_apply,
    present,
    count
};

inline constexpr std::uint16_t stage_count = static_cast<std::uint16_t>(stage::count) - 1;

/// 打点域：只用于过滤/分类，不参与任何逻辑。
enum class domain : std::uint16_t {
    core = 0,
    input,
    draw,
    container,
    collide,
    rng,
    resource,
    script,
    count
};

enum class kind : std::uint16_t {
    stage_enter = 0,
    stage_exit,
    event,
    mark,   ///< 用户自定义标记（tag 在 a 里）
};

// -----------------------------------------------------------------------------
//  记录（32 字节，热路径只写这一条）
// -----------------------------------------------------------------------------
struct record {
    std::uint64_t t_ns = 0;   ///< 相对 trace 起点的单调时间
    std::uint32_t frame = 0;  ///< 逻辑帧号
    std::uint16_t stage = 0;  ///< 写入时的当前阶段（0 = 不在阶段内）
    std::uint16_t kind = 0;   ///< kind
    std::uint16_t domain = 0; ///< domain
    std::uint16_t pad = 0;
    std::uint32_t a = 0;      ///< 载荷 1
    std::uint32_t b = 0;      ///< 载荷 2
};

static_assert(sizeof(record) == 32, "record 必须是 32 字节（热路径只写一条）");

// -----------------------------------------------------------------------------
//  生命周期与写入
// -----------------------------------------------------------------------------
void reset(std::uint32_t frame0 = 0);
void set_frame(std::uint32_t f);

void enter(stage s);
void exit(stage s);
void event(domain d, std::uint32_t a, std::uint32_t b = 0);
void mark(std::uint32_t tag);

// -----------------------------------------------------------------------------
//  读取
// -----------------------------------------------------------------------------
[[nodiscard]] std::size_t size() noexcept;      ///< 已写下的条数（<= capacity）
[[nodiscard]] std::size_t capacity() noexcept;  ///< 静态环容量
[[nodiscard]] std::uint64_t dropped() noexcept; ///< 因环满而丢掉的条数
[[nodiscard]] const record* at(std::size_t i) noexcept; ///< 0 = 最老，按时间顺序

[[nodiscard]] const char* stage_name(stage s) noexcept;
[[nodiscard]] const char* domain_name(domain d) noexcept;
[[nodiscard]] const char* kind_name(kind k) noexcept;

// -----------------------------------------------------------------------------
//  输出（只在帧末调用）
// -----------------------------------------------------------------------------
void dump_text(std::ostream& os, std::size_t max_rows = 64);
void dump_csv(std::ostream& os, bool header = true);

} // namespace wag::trace

#    define WAG_TRACE_RESET(f)     ::wag::trace::reset(f)
#    define WAG_TRACE_FRAME(f)     ::wag::trace::set_frame(f)
#    define WAG_TRACE_ENTER(s)     ::wag::trace::enter(s)
#    define WAG_TRACE_EXIT(s)      ::wag::trace::exit(s)
#    define WAG_TRACE_EVENT(d, a, b) ::wag::trace::event(d, a, b)
#    define WAG_TRACE_MARK(t)      ::wag::trace::mark(t)

#else // WAG_TRACE == 0

// 【全部编译成空】——不产生分支、不产生符号引用。
#    define WAG_TRACE_RESET(f)       ((void)0)
#    define WAG_TRACE_FRAME(f)       ((void)0)
#    define WAG_TRACE_ENTER(s)       ((void)0)
#    define WAG_TRACE_EXIT(s)        ((void)0)
#    define WAG_TRACE_EVENT(d, a, b) ((void)0)
#    define WAG_TRACE_MARK(t)        ((void)0)

#endif // WAG_TRACE
