// =============================================================================
//  rng.cpp —— splitmix64 与流登记表的实现
//
//  【本文件不依赖任何其它模块】不 include log / error / SDL。
//    理由：错误"怎么报"是**调用方**的事（Lua 层转 L3），
//    本模块只回答"这个操作成功了吗"（返回 bool）。
//    ⇒ 于是它能在无依赖构建（WAG_NO_EXTERNAL_DEPS）里独立编译。
// =============================================================================

#include "rng/rng.hpp"

namespace wag::rng {

namespace {

/// 黄金分割增量：2^64 / φ（取奇数）。
///
/// 【为什么是它】连续调用时状态在 2^64 上均匀铺开，
///   不会像 `state += 1` 那样让低位变化极慢。
constexpr std::uint64_t kGamma = 0x9E3779B97F4A7C15ull;

/// 两个混合常数。
///
/// 【为什么是这两个】它们是 splitmix64 原版的常数（来自 Stafford 的混合函数族），
///   经过统计检验；换成随便一个奇数会显著退化（见 rng.hpp 顶部关于“低位”的说明）。
constexpr std::uint64_t kMix1 = 0xBF58476D1CE4E5B9ull;
constexpr std::uint64_t kMix2 = 0x94D049BB133111EBull;

} // namespace

std::uint64_t splitmix64_next(std::uint64_t& state) noexcept {
    std::uint64_t z = (state += kGamma);
    z = (z ^ (z >> 30)) * kMix1;
    z = (z ^ (z >> 27)) * kMix2;
    return z ^ (z >> 31);
}

// -----------------------------------------------------------------------------
//  主种子与流
// -----------------------------------------------------------------------------

void registry::set_master_seed(std::uint64_t seed) noexcept {
    master_ = seed;
}

std::uint64_t registry::master_seed() const noexcept {
    return master_;
}

std::uint64_t registry::new_stream(std::uint32_t index) {
    if (index >= kStreamCount) {
        return 0; // 槽位越界 ⇒ 调用方转 L3
    }

    // 【派生】先把"主种子 + 槽位"混成一个计数器，再走一次完整混合。
    //   `(index + 1)` 而不是 `index`：避免 index=0 时乘出来是 0，
    //   也避免"三条流的输入只差 0/1/2"这种可预测的相邻关系。
    std::uint64_t counter = master_ ^ (kGamma * (static_cast<std::uint64_t>(index) + 1u));

    // 取两次：第一次丢掉（它是计数器的线性函数），第二次作为流的状态。
    //   【为什么取两次】与 Sub 的 `seed_seq_from<splitmix64>` 同一意图 ——
    //     先热一次，让状态不是"种子的直接函数"。
    static_cast<void>(splitmix64_next(counter));
    const std::uint64_t derived = splitmix64_next(counter);

    const std::uint64_t handle = next_id_++;
    streams_.emplace(handle, stream{derived});
    return handle;
}

void registry::reset() noexcept {
    // 【id 不回收】`next_id_` 继续往上走 ⇒ 旧句柄即使被"重新发明"也不会撞上。
    streams_.clear();
}

// -----------------------------------------------------------------------------
//  取数
// -----------------------------------------------------------------------------

registry::stream* registry::find(std::uint64_t handle) noexcept {
    const auto it = streams_.find(handle);
    return (it == streams_.end()) ? nullptr : &it->second;
}

const registry::stream* registry::find(std::uint64_t handle) const noexcept {
    const auto it = streams_.find(handle);
    return (it == streams_.end()) ? nullptr : &it->second;
}

bool registry::next_u32(std::uint64_t handle, std::uint32_t& out) noexcept {
    stream* const s = find(handle);
    if (s == nullptr) {
        return false;
    }
    // 【取高 32 位】splitmix64 的高低位都混过，但"取高位"是通用惯例
    //   （低位即使经过了 `>>31` 的异或，仍比高位更接近计数器结构）。
    out = static_cast<std::uint32_t>(splitmix64_next(s->state) >> 32);
    return true;
}

bool registry::next_int(std::uint64_t handle, std::int64_t lo, std::int64_t hi,
                        std::int64_t& out) noexcept {
    stream* const s = find(handle);
    if (s == nullptr) {
        return false;
    }
    if (hi < lo) {
        return false; // 区间反向 ⇒ 调用方转 L3（已定：不自动交换）
    }
    // 【含两端】区间长度 = hi - lo + 1。
    //   ⚠️ 用无符号算长度，避免 hi - lo 在极端值下溢出。
    const std::uint64_t span = static_cast<std::uint64_t>(hi - lo) + 1u;
    const std::uint64_t r = splitmix64_next(s->state);
    // 【取模的偏置】量级 = span / 2^64 ≤ 2^32 / 2^64 = 2^-32
    //   ⇒ 四十亿次里有一次多算一点点，对游戏完全可忽略。
    out = lo + static_cast<std::int64_t>(r % span);
    return true;
}

bool registry::next_float(std::uint64_t handle, double& out) noexcept {
    stream* const s = find(handle);
    if (s == nullptr) {
        return false;
    }
    // 【53 位】double 的尾数正好 53 位 ⇒ 取高 53 位再乘 2^-53：
    //   结果是 [0,1)，且**不会**出现 1.0（取模/除法的常见错误就是会溢出到 1.0）。
    out = static_cast<double>(splitmix64_next(s->state) >> 11) * 0x1.0p-53;
    return true;
}

// -----------------------------------------------------------------------------
//  状态快照
// -----------------------------------------------------------------------------

bool registry::get_state(std::uint64_t handle, std::uint32_t& lo,
                         std::uint32_t& hi) const noexcept {
    const stream* const s = find(handle);
    if (s == nullptr) {
        return false;
    }
    lo = static_cast<std::uint32_t>(s->state & 0xFFFFFFFFull);
    hi = static_cast<std::uint32_t>(s->state >> 32);
    return true;
}

bool registry::set_state(std::uint64_t handle, std::uint32_t lo,
                         std::uint32_t hi) noexcept {
    stream* const s = find(handle);
    if (s == nullptr) {
        return false;
    }
    s->state = (static_cast<std::uint64_t>(hi) << 32) | static_cast<std::uint64_t>(lo);
    return true;
}

} // namespace wag::rng
