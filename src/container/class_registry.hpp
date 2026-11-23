// =============================================================================
// container/class_registry.hpp
//
// 的类表：引擎侧只存"回调存在位"，不存任何 Lua 函数。
//
// 【为什么只有位】
//：引擎手里只有 (class_id, handle)，拿不到 self ⇒ 它无法直接调
// class.frame(self)。所以它只需要知道"这个类有没有某个回调"，
// 用来决定要不要把事件交给库层分派器。
//
// 【位布局】
// bit0 = 已注册（：spawn 用它判"未注册 ⇒ L3"）
// bit1..6 = init / frame / render / colli / kill / del（的六个回调）
// ⇒ 一旦写进说明，位序不得再改。
// =============================================================================

#pragma once

#include <array>
#include <cstdint>

namespace wag::container {

/// 六个回调。枚举值即"回调序号"，不参与位运算。
enum class class_callback : std::uint8_t {
    init = 0,
    frame = 1,
    render = 2,
    colli = 3,
    kill = 4,
    del = 5,
};

/// class id 是 u16 ⇒ 65536 项。
inline constexpr std::uint32_t class_capacity = 65536;

class class_registry {
public:
 /// bit0：已注册。
    static constexpr std::uint8_t kRegistered = 0x01u;

 /// 回调 → 位（bit1..6）。
    [[nodiscard]] static constexpr std::uint8_t callback_bit(class_callback cb) noexcept {
        return static_cast<std::uint8_t>(1u << (static_cast<unsigned>(cb) + 1u));
    }

 /// 注册/覆盖。mask 只含回调位；"已注册"位由这里强制置上。
    void register_class(std::uint16_t id, std::uint8_t mask) noexcept {
        bits_[id] = static_cast<std::uint8_t>(mask | kRegistered);
    }

    [[nodiscard]] bool is_registered(std::uint16_t id) const noexcept {
        return (bits_[id] & kRegistered) != 0;
    }

    [[nodiscard]] bool has(std::uint16_t id, class_callback cb) const noexcept {
        return (bits_[id] & callback_bit(cb)) != 0;
    }

    [[nodiscard]] std::uint8_t mask(std::uint16_t id) const noexcept {
        return bits_[id];
    }

 /// 只给"状态重建"用；运行期不应调用（会让已注册的类全部消失）。
    void clear() noexcept { bits_.fill(0); }

private:
    std::array<std::uint8_t, class_capacity> bits_{};
};

} // namespace wag::container
