// =============================================================================
//  rng.hpp —— 可序列化的随机流（splitmix64，Steele / Lea / Flood 2014）
//
//  【它为什么在引擎侧，而不是纯 Lua】
//    · 状态是 u64，而 Lua 的数字是 double（**只有 53 位精确**）
//      ⇒ u64 状态一旦经过 Lua 就会丢位 ⇒ 序列静默改变 ⇒ 回放失效
//    · "同一个种子 ⇒ 同一串数"是**回放承诺**的一部分，属于引擎规格
//    ⇒ 所以：算法与状态都在 C++，Lua 只拿一个**整数句柄**。
//
//  【三条流的存在理由 —— 不是"三种随机"，而是"一条真随机 + 两个隔离槽"】
//      ran_main  ：唯一的真随机。它的序列是 rep 的一部分。
//      ran_pause ：隔离槽。给暂停菜单/暂停特效用，防止它们去动主流。
//      ran_ext   ：隔离槽。给与种子无关的背景随机用。
//    ⇒ 隔离的目的：让"不影响回放的随机"有指定去处，而不去污染主流。
//    ⇒ 唯一纪律：**ran_main 只许在 on_frame_main 里消耗**。
//       （任何渲染钩子里用它都会破坏 rep：asleep 帧"跑逻辑但不跑渲染"。）
//
//  【为什么是 splitmix64 而不是 xorshift/xoshiro】
//    · 一个 u64 状态 ⇒ 存档只要 8 字节（多字状态要 32 字节）
//    · 5 条运算一个数 ⇒ 我们的量级（每帧几百次）绰绰有余
//    · 它同时能当"种子展开器"（把主种子展开成三条流的初始状态）
//    ⇒ 少一个算法 = 少一处要维护的回放承诺
//
//  【为什么"低位丢随机"在这里不是问题】
//    splitmix64 的每一步都有 `>>` + 大奇数乘 ⇒ 高位被灌回低位。
//    而"乘常数再取模 2^32"那类做法（例如 ×7、×197）低位周期只有 1~2，
//    正是因为它缺少这个"回灌"。实测：a=7 时 bit0 恒定、bit1/bit2 周期 2。
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace wag::rng {

/// 流槽位的数量：0 = main，1 = pause，2 = ext。
///
/// 【为什么是编译期常量】"三条流"是**平台层预设**，不是运行期可变的配置。
///   ⇒ 槽位越界（index >= 3）属于调用方写错，由 Lua 层转 L3。
inline constexpr std::uint32_t kStreamCount = 3;

/// splitmix64 单步：推进 `state` 并返回一个混合后的 u64。
///
/// 【标准形态】
///     state += 0x9E3779B97F4A7C15      （黄金分割增量）
///     z = 混合三次（两次 `>>` + 大奇数乘，最后一次只异或 `>>31`）
/// 【周期】2^64（状态就是一个计数器）。
[[nodiscard]] std::uint64_t splitmix64_next(std::uint64_t& state) noexcept;

/// 流的登记表：持有主种子与所有活的流。
///
/// 【为什么句柄是"单调递增的 id"而不是"槽位下标"】
///   下标会被复用 ⇒ 重载脚本后，一个**旧句柄会悄悄指向一条新流** ⇒ 静默错乱。
///   单调 id 永不复用 ⇒ 旧句柄要么还在（同一条流），要么查不到（明确的失效）。
///
/// 【与脚本重载的关系（⑤）】
///   脚本重载时由引导脚本调 `reset()` ⇒ 表清空 ⇒ **旧句柄查不到 ⇒ 调用方转 L3**。
///   这就是"重建而非复用"的落地形态。
class registry {
public:
    /// 设定主种子。【调用点】boot_sequence，且必须在脚本装载之前。
    void set_master_seed(std::uint64_t seed) noexcept;

    /// 取主种子（给 `wag.rng.master_seed()` 拆 lo/hi 用）。
    [[nodiscard]] std::uint64_t master_seed() const noexcept;

    /// 按槽位派生一条新流，返回句柄。
    ///
    /// 【派生方式】`master ^ (kGamma * (index + 1))` 再走一次 splitmix64。
    ///   ⇒ 三条流互不相关（它们的初始状态经过完整混合，彼此没有代数关系）。
    ///
    /// 【返回 0 表示 index 越界】调用方负责转 L3；本模块不报错、不依赖 log。
    [[nodiscard]] std::uint64_t new_stream(std::uint32_t index);

    /// 清空所有流（句柄全部失效）。【调用点】脚本重载。
    void reset() noexcept;

    // ---- 取数（返回 false = 句柄无效，由调用方转 L3）----

    [[nodiscard]] bool next_u32(std::uint64_t handle, std::uint32_t& out) noexcept;

    /// 区间 [lo, hi]，**含两端**。`hi < lo` ⇒ 返回 false（由调用方转 L3）。
    [[nodiscard]] bool next_int(std::uint64_t handle, std::int64_t lo,
                                std::int64_t hi, std::int64_t& out) noexcept;

    /// [0, 1)，53 位精度（double 的尾数位），**不会返回 1.0**。
    [[nodiscard]] bool next_float(std::uint64_t handle, double& out) noexcept;

    // ---- 状态快照（存档用；u64 拆两个 32 位）----

    [[nodiscard]] bool get_state(std::uint64_t handle, std::uint32_t& lo,
                                 std::uint32_t& hi) const noexcept;
    [[nodiscard]] bool set_state(std::uint64_t handle, std::uint32_t lo,
                                 std::uint32_t hi) noexcept;

    [[nodiscard]] std::size_t stream_count() const noexcept {
        return streams_.size();
    }

private:
    struct stream {
        std::uint64_t state = 0;
    };

    [[nodiscard]] stream* find(std::uint64_t handle) noexcept;
    [[nodiscard]] const stream* find(std::uint64_t handle) const noexcept;

    std::uint64_t master_ = 0;

    /// 下一个要发的 id。**从 1 开始**：0 保留表示"无效句柄"。
    std::uint64_t next_id_ = 1;

    std::unordered_map<std::uint64_t, stream> streams_;
};

} // namespace wag::rng
