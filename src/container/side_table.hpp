// =============================================================================
// container/side_table.hpp
//
// 的判定侧表：slot[容器容量] + entries[]。
//
// 【为什么需要它】
// 宽相要按 AABB 把容器塞进格子，而 AABB 需要判定形状（a,b,ox,oy）；
// 判定数据在 ④，但 ④ 是 Lua 表 —— 240 Hz 下每帧跨界读 2 万次约 0.6–1.2 ms。
// ⇒ 引擎侧留一份**投影**：创建时写、销毁时删（写时同步，）。
//
// 【为什么 slot 是 u16】
// 每池容量 32767 ⇒ entries 下标最大 32766 ⇒ u16 够（0xFFFF 当"无"）。
//
// 【为什么销毁是 swap-remove】
// entries 是密集数组；换进来的那条的 container 字段要同步回它的 slot。
// 这正是 说的"创建时写入；销毁时 swap-remove"。
//
// 【它不拥有容器】
// 条目里的 container 是池内下标；池身份由"这张表属于哪个池"提供。
//
// ✅【已接线（2026-11-18 核对）】容器销毁时侧表条目必须被 remove（的 swap-remove），
// 而这一步**已经在两条 kill 路径上**：`l_container_kill` 与 `replace` 的旧句柄路径
// （`src/script/primitives.cpp` 里 `side_table().remove(...)` 紧接 `kill(...)`）。
// ⚠️ 新增任何**绕过原语**的 kill 路径时，必须同样先 remove ——
// 否则条目会指向已回收的槽（rebuild 时会 L3，不会静默）。
// =============================================================================

#pragma once

#include "container/container.hpp"
#include "container/hit.hpp"

#include <cstdint>
#include <vector>

namespace wag::container {

class container_side_table {
public:
    static constexpr std::uint16_t none = 0xFFFF;

    container_side_table();

 /// 新增一条；该下标已有条目 ⇒ L3（不变量被破坏）。
    void add(std::uint32_t index, const hit_entry& e);

 /// 删除一条（swap-remove）；没有条目 ⇒ L3（不静默）。
    void remove(std::uint32_t index);

 /// 原地更新（下标必须已有条目）。
    void update(std::uint32_t index, const hit_entry& e);

 /// 查询；没有条目返回 nullptr。
    [[nodiscard]] const hit_entry* find(std::uint32_t index) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] const hit_entry& at(std::size_t i) const noexcept {
        return entries_[i];
    }

private:
    std::vector<std::uint16_t> slot_; ///< 长 = pool_capacity；值 = entries 下标 / none
    std::vector<hit_entry> entries_;  ///< 密集
};

} // namespace wag::container
