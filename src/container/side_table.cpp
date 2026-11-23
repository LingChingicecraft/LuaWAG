// =============================================================================
// container/side_table.cpp
//
// 侧表的实现。只有三个不变量，别的一概不管：
// ① slot[i] 指向的条目，其 container 必须 == i（双向一致）
// ② entries 是密集的（0..size-1 无洞）
// ③ 任何不一致 ⇒ L3（这是引擎内部记账，坏了不能带错继续）
// =============================================================================

#include "container/side_table.hpp"

#include "error/error.hpp"

#include <string>

namespace wag::container {

container_side_table::container_side_table() {
    slot_.assign(pool_capacity, none);
}

void container_side_table::add(std::uint32_t index, const hit_entry& e) {
    if (index >= pool_capacity) {
        fail_internal("判定侧表：容器下标越界 " + std::to_string(index) +
                      "（容量 32767）");
    }
    if (slot_[index] != none) {
        fail_internal("判定侧表：容器下标 " + std::to_string(index) +
                      " 已有条目（重复写入，内部不变量被破坏）");
    }
    if (entries_.size() >= pool_capacity) {
 // 每个容器最多一条 ⇒ 理论上到不了这里；到了就是计数错
        fail_internal("判定侧表：条目数超过容器容量（32767）");
    }
    slot_[index] = static_cast<std::uint16_t>(entries_.size());
    entries_.push_back(e);
 // 条目的 container 必须与下标一致（双向一致是 swap-remove 的前提）
    entries_.back().container = index;
}

void container_side_table::remove(std::uint32_t index) {
    if (index >= pool_capacity) {
        fail_internal("判定侧表：容器下标越界 " + std::to_string(index));
    }
    const std::uint16_t ei = slot_[index];
    if (ei == none) {
        fail_internal("判定侧表：容器下标 " + std::to_string(index) +
                      " 没有条目，不能删除");
    }

 // swap-remove：把最后一条挪到被删的位置。
 // 【删除最后一条时】moved 就是它自己：slot_ 先写成 ei、随后被置 none —— 结果正确，
 // 因此不需要为"删最后一条"单开分支。
    const hit_entry moved = entries_.back();
    entries_[ei] = moved;
    slot_[moved.container] = ei;
    entries_.pop_back();
    slot_[index] = none;
}

void container_side_table::update(std::uint32_t index, const hit_entry& e) {
    const std::uint16_t ei = slot_[index];
    if (ei == none) {
        fail_internal("判定侧表：更新一个没有条目的容器 " +
                      std::to_string(index));
    }
    entries_[ei] = e;
    entries_[ei].container = index;
}

const hit_entry* container_side_table::find(std::uint32_t index) const noexcept {
    if (index >= pool_capacity) {
        return nullptr;
    }
    const std::uint16_t ei = slot_[index];
    if (ei == none) {
        return nullptr;
    }
    return &entries_[ei];
}

} // namespace wag::container
