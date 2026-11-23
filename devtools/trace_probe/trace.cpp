// =============================================================================
//  trace.cpp —— 静态环 + 帧末输出
//
//  WAG_TRACE == 0 时整个翻译单元是空的（连符号都不产生）。
// =============================================================================
#include "trace.hpp"

#if WAG_TRACE

#include <array>
#include <chrono>
#include <cstdio>
#include <ostream>

namespace wag::trace {
namespace {

constexpr std::size_t kCapacity = 4096;

std::array<record, kCapacity> g_ring{};
std::size_t g_head = 0;      ///< 下一条要写的位置
std::size_t g_count = 0;     ///< 有效条数
std::uint64_t g_dropped = 0; ///< 环满丢掉的条数
std::uint32_t g_frame = 0;
std::uint16_t g_stage = 0;   ///< 当前阶段（在 enter 里设，不在 exit 里清）
std::uint64_t g_t0 = 0;

std::uint64_t now_ns() {
    using clock = std::chrono::steady_clock;
    const auto d = clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
}

void push(std::uint16_t kind, std::uint16_t domain, std::uint32_t a, std::uint32_t b) {
    if (g_count == kCapacity) {
        ++g_dropped;
        // 环满：覆盖最老的一条（head 已经指向它）。
    } else {
        ++g_count;
    }
    record& r = g_ring[g_head];
    r.t_ns = now_ns() - g_t0;
    r.frame = g_frame;
    r.stage = g_stage;
    r.kind = kind;
    r.domain = domain;
    r.pad = 0;
    r.a = a;
    r.b = b;
    g_head = (g_head + 1) % kCapacity;
}

} // namespace

void reset(std::uint32_t frame0) {
    g_head = 0;
    g_count = 0;
    g_dropped = 0;
    g_frame = frame0;
    g_stage = 0;
    g_t0 = now_ns();
}

void set_frame(std::uint32_t f) { g_frame = f; }

void enter(stage s) {
    g_stage = static_cast<std::uint16_t>(s);
    push(static_cast<std::uint16_t>(kind::stage_enter), 0, static_cast<std::uint32_t>(s), 0);
}

void exit(stage s) {
    // 先按"仍在 s 里"写下退出记录（这样读日志时能配对），再回到"不在阶段内"。
    push(static_cast<std::uint16_t>(kind::stage_exit), 0, static_cast<std::uint32_t>(s), 0);
    g_stage = 0;
}

void event(domain d, std::uint32_t a, std::uint32_t b) {
    push(static_cast<std::uint16_t>(kind::event), static_cast<std::uint16_t>(d), a, b);
}

void mark(std::uint32_t tag) {
    push(static_cast<std::uint16_t>(kind::mark), 0, tag, 0);
}

std::size_t size() noexcept { return g_count; }
std::size_t capacity() noexcept { return kCapacity; }
std::uint64_t dropped() noexcept { return g_dropped; }

const record* at(std::size_t i) noexcept {
    if (i >= g_count) {
        return nullptr;
    }
    const std::size_t oldest = (g_head + kCapacity - g_count) % kCapacity;
    return &g_ring[(oldest + i) % kCapacity];
}

const char* stage_name(stage s) noexcept {
    switch (s) {
    // 【0 = 不在任何阶段内】落在两个阶段之间的记录（例如 end_frame 的 mark）。
    //   显示成 "-" 而不是 "?"：前者是"合法但不在阶段内"，后者读起来像错误。
    case static_cast<stage>(0):   return "-";
    case stage::frame_begin:      return "frame_begin";
    case stage::rate_control:     return "rate_control";
    case stage::event_consume:    return "event_consume";
    case stage::input_sample:     return "input_sample";
    case stage::ext_logic:        return "ext_logic";
    case stage::main_logic:       return "main_logic";
    case stage::render_begin:     return "render_begin";
    case stage::scene_render:     return "scene_render";
    case stage::ext_render_end:   return "ext_render_end";
    case stage::snapshot:         return "snapshot";
    case stage::state_apply:      return "state_apply";
    case stage::present:          return "present";
    case stage::count:            break;
    }
    return "?";
}

const char* domain_name(domain d) noexcept {
    switch (d) {
    case domain::core:      return "core";
    case domain::input:     return "input";
    case domain::draw:      return "draw";
    case domain::container: return "container";
    case domain::collide:   return "collide";
    case domain::rng:       return "rng";
    case domain::resource:  return "resource";
    case domain::script:    return "script";
    case domain::count:     break;
    }
    return "?";
}

const char* kind_name(kind k) noexcept {
    switch (k) {
    case kind::stage_enter: return "enter";
    case kind::stage_exit:  return "exit";
    case kind::event:       return "event";
    case kind::mark:        return "mark";
    }
    return "?";
}

void dump_text(std::ostream& os, std::size_t max_rows) {
    const std::size_t n = g_count < max_rows ? g_count : max_rows;
    os << "── trace: " << g_count << " 条";
    if (g_dropped != 0) {
        os << "（环满丢弃 " << g_dropped << " 条）";
    }
    os << "，显示前 " << n << " 条 ──\n";
    os << "      t_us  frame  stage             kind   domain     a          b\n";
    for (std::size_t i = 0; i < n; ++i) {
        const record* r = at(i);
        if (r == nullptr) {
            break;
        }
        char line[256];
        std::snprintf(line, sizeof(line), "%10llu  %5u  %-16s  %-6s %-9s  0x%08X 0x%08X",
                      static_cast<unsigned long long>(r->t_ns / 1000u), r->frame,
                      stage_name(static_cast<stage>(r->stage)), kind_name(static_cast<kind>(r->kind)),
                      domain_name(static_cast<domain>(r->domain)), r->a, r->b);
        os << line << '\n';
    }
}

void dump_csv(std::ostream& os, bool header) {
    if (header) {
        os << "t_ns,frame,stage,kind,domain,a,b\n";
    }
    for (std::size_t i = 0; i < g_count; ++i) {
        const record* r = at(i);
        if (r == nullptr) {
            break;
        }
        os << r->t_ns << ',' << r->frame << ',' << stage_name(static_cast<stage>(r->stage)) << ','
           << kind_name(static_cast<kind>(r->kind)) << ','
           << domain_name(static_cast<domain>(r->domain)) << ',' << r->a << ',' << r->b << '\n';
    }
}

} // namespace wag::trace

#endif // WAG_TRACE
