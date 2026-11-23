// =============================================================================
//  container/pool.hpp
//
//  单池（container_pool）+ 多池（container_pool_set）。
//
//  【这个模块负责什么】
//    池负责容器的身份、分配、回收和句柄有效性。没有池，spawn / kill /
//    replace / 碰撞条目都无从谈起。
//
//  【两条固定规则】
//    · 每池容量固定为 32767，不可配置、不可扩容；存活数超过上限 ⇒ L3。
//    · 已标记 dead 的槽在帧末之前不算空位，不能复用。
//      所以帧内“杀 N 个再建 N 个”要求帧初就有 N 个空槽。
//    · 存活数达到 80% 时记一次 L1；回落后再升上来才会再记。
//
//  【为什么“多池”是结构，而不是可选优化】
//    32767 是硬上限，10 万个对象至少需要 4 个池。
//    池 id 单调分配、永不复用；池对象建立后不销毁，因此句柄里的池 id
//    永远指向同一个池。
// =============================================================================

#pragma once

#include "container/class_registry.hpp"
#include "container/container.hpp"
#include "container/side_table.hpp"

#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace wag::container {

/// 帧末回收时记下的一条（del 派发需要 class 与回收前的句柄）。
struct recycled_record {
    std::uint16_t class_id = 0;
    handle h = invalid_handle;
};

/// 句柄失效的原因。
///
/// 【为什么需要它 —— 这是"换通道"的前提】
///   交界处要把"死句柄"从"终止"改成"抛"，而 `try_get` 只回答成功/失败，
///   **不告诉原因**。若交界处只拿 bool，错误信息会从
///       「容器句柄失效：池「X」，下标 42，容器已销毁（handle=…）」
///   退化成「句柄无效」——那是**信息质量下降**，而本次大修的目标之一正是可归因。
///   ⇒ 于是加这个**纯查询**：不改任何状态，只回答"为什么不行"。
///
/// 【为什么不把 get() 改成返回 optional】
///   那会动 20+ 个调用点，而且会让"引擎内部路径"失去"到这里就是 bug"的断言性质。
///   加一个只读探针，比改 get() 的签名便宜得多。
enum class handle_failure : std::uint8_t {
    none = 0,         ///< 句柄有效（查询成功）
    invalid_handle,   ///< 句柄为 0
    bad_pool,         ///< 池 id 不存在，或与所属池不符
    bad_index,        ///< 下标越界
    dead,             ///< 容器已销毁（state != 1）
    stale_generation  ///< 代号不匹配（旧句柄）
};

/// 失效原因的可读名。**与 `handle_failure` 一一对应，措辞与 `get()` 的诊断保持一致。**
[[nodiscard]] const char* handle_failure_name(handle_failure f) noexcept;

// -----------------------------------------------------------------------------
//  单池
// -----------------------------------------------------------------------------
/// 容器计数的三个口径。
struct object_counts {
    std::uint32_t live = 0;        ///< state == 1
    std::uint32_t occupied = 0;    ///< state != 0（含帧末待回收）
    std::uint32_t renderable = 0;  ///< state == 1 && visible && img（字段层面）
};

class container_pool {
public:
    /// @param pool_id 由 container_pool_set 分配（单调、永不复用）
    /// @param name    仅用于诊断与调试视图，不参与游戏逻辑
    container_pool(std::uint32_t pool_id, std::string name);

    container_pool(const container_pool&) = delete;
    container_pool& operator=(const container_pool&) = delete;
    container_pool(container_pool&&) = delete;
    container_pool& operator=(container_pool&&) = delete;

    /// 尝试分配；成功返回容器指针并写出句柄，池满返回 nullptr（不报错）。
    /// 【为什么有它】L3 会终止进程，验证程序需要一个"不终止"的入口来丈量边界。
    [[nodiscard]] container* try_allocate(handle& out) noexcept;

    /// 分配；池满 ⇒ L3。错误信息必须含池名 / 容量 / 当前存活数。
    [[nodiscard]] container* allocate(handle& out);

    /// 池满时的**标准错误文本**（必须含池名 / 容量 / 当前存活数）。
    ///
    /// 【为什么把它暴露出来】交界处要在"不终止"的前提下报**同一条**信息
    ///   （`spawn` / `replace` 改成 `try_allocate` 之后由 primitives 报）。
    ///   消息只能有一处来源，否则 `allocate` 与交界处会各说各话 ——
    ///   而"两处说同一件事"正是这次大修要消灭的形态。
    [[nodiscard]] std::string full_message() const;

    /// 查询；句柄无效 ⇒ nullptr（不报错）。
    [[nodiscard]] container* try_get(handle h) noexcept;

    /// 查询；句柄无效 ⇒ L3（旧句柄被使用必须报错）。
    [[nodiscard]] container* get(handle h);

    /// 查询失效**原因**；有效返回 `handle_failure::none`。**不报错、不改状态。**
    ///
    /// 【与 `get()` 的关系】诊断顺序必须与 `get()` **完全一致**
    ///   （0 → 池不符 → 下标越界 → 已销毁 → 代号不匹配），
    ///   否则同一个坏句柄在交界处与深层会得到两种说法。
    ///   两者同步的责任写在这里：改 `get()` 的分支顺序时，必须同改本函数。
    [[nodiscard]] handle_failure probe(handle h) const noexcept;

    /// 标记 dead（**不回收**）。句柄无效 ⇒ L3。
    /// 【为什么不在这里回收】本帧内仍视为"占用"，
    ///   回收统一发生在帧末的 recycle_dead()。
    void kill(handle h);

    /// 帧末回收：把本帧标记 dead 的槽真正放回空闲表（代号 +1）。
    /// @param out 非空时，按回收顺序追加 (class_id, 回收前的句柄)，
    ///            供 del 派发使用；为空则只回收。
    void recycle_dead(std::vector<recycled_record>* out = nullptr);

    [[nodiscard]] std::uint32_t alive_count() const noexcept { return alive_; }
    /// 按组/全组统计三个口径；group 为空 = 全组。
    [[nodiscard]] object_counts count_objects(
        std::optional<std::uint8_t> group) const noexcept;
    [[nodiscard]] std::uint32_t peak_alive() const noexcept { return peak_; }
    [[nodiscard]] std::uint32_t capacity() const noexcept { return pool_capacity; }
    [[nodiscard]] std::uint32_t pool_id() const noexcept { return pool_id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /// 进入 80% 区间的次数（诊断/验证用；正常游戏里读它没有意义）。
    [[nodiscard]] std::uint32_t warn_entries() const noexcept { return warn_entries_; }

    // ---- 供碰撞侧使用 ----
    //   【为什么 at() 不校验句柄】它只接受"来自本池侧表的下标"，
    //   而侧表条目的写入是引擎自己做的；这里再校验一次等于把
    //   "内部一致"当成"外部输入"来防，会把热路径变慢而没有收益。
    [[nodiscard]] const container& at(std::uint32_t index) const noexcept {
        return data_[index];
    }
    [[nodiscard]] std::uint32_t generation_of(std::uint32_t index) const noexcept {
        return meta_[index].gen;
    }
    [[nodiscard]] container_side_table& side_table() noexcept { return side_; }
    [[nodiscard]] const container_side_table& side_table() const noexcept {
        return side_;
    }

    /// 遍历存活容器（渲染发射用）。
    /// 【口径】只回调 state == 1 的槽；dead（待回收）与空闲都不算。
    /// 【代价】O(池容量) 的扫描（32767）；这是第一版，之后可以换成存活链表。
    template <typename Fn>
    void for_each_live(Fn&& fn) const {
        for (std::uint32_t i = 0; i < pool_capacity; ++i) {
            if (meta_[i].state == 1) {
                fn(data_[i]);
            }
        }
    }

private:
    struct slot_meta {
        std::uint32_t gen = 1;   ///< 该槽当前代号（从 1 起；见 container.hpp）
        std::uint8_t state = 0;  ///< 0 空闲 / 1 存活 / 2 已标记 dead
        std::uint8_t pad[3] = {0, 0, 0};
    };

    void check_warning_();

    std::uint32_t pool_id_;
    std::string name_;
    std::vector<container> data_;      ///< 容量恰为 pool_capacity（构造时一次性分配）
    std::vector<slot_meta> meta_;      ///< 与 data_ 同长
    std::vector<std::uint32_t> free_;  ///< 空闲下标（栈）
    std::vector<std::uint32_t> dead_;  ///< 本帧标记 dead 的下标（帧末回收）
    std::uint32_t alive_ = 0;          ///< 存活数 = 存活 + 已标记 dead
    std::uint32_t peak_ = 0;           ///< 历史峰值（诊断）
    std::uint32_t warn_entries_ = 0;   ///< 进入 80% 区间的次数（诊断）
    bool warned_ = false;              ///< 当前是否已在 80% 区间内
    container_side_table side_;        ///< 这个池的判定侧表（slot + entries）
};

// -----------------------------------------------------------------------------
//  多池
// -----------------------------------------------------------------------------
class container_pool_set {
public:
    container_pool_set() = default;

    /// 新建一个池并分配池 id（单调、永不复用）。
    /// 池数达到 4096（u12 上限）⇒ L3 —— 这是"池 id 不复用"的硬边界。
    [[nodiscard]] std::uint32_t create_pool(std::string name);

    /// 在指定池分配；池 id 不存在或池满 ⇒ L3。
    [[nodiscard]] container* allocate(std::uint32_t pool_id, handle& out);

    /// 按句柄路由到所属池；任何无效 ⇒ L3。
    [[nodiscard]] container* get(handle h);

    /// 按句柄路由；无效 ⇒ nullptr（不报错）。
    /// 【为什么需要它】Lua 侧要回答"这个句柄还活着吗"，那是**正常查询**，
    ///   不是错误（脚本完全可能持有已销毁的句柄去问一句）。
    [[nodiscard]] container* try_get(handle h) noexcept;

    /// 按句柄路由并回答失效**原因**（见 `container_pool::probe`）。**不报错、不改状态。**
    [[nodiscard]] handle_failure probe(handle h) const noexcept;

    /// 按句柄路由并标记 dead；任何无效 ⇒ L3。
    void kill(handle h);

    /// 帧末回收所有池；同时把"本帧回收掉的 (class_id, 回收前句柄)"记进
    /// recycled()，供 del 派发使用。
    void recycle_dead();

    /// 热重载代际边界：销毁全部池并重置池 id 空间（热重载）。
    ///
    /// 【只在重载提交时调用】正常游戏路径不得调用。
    /// 【不可失败】不分配、不调 Lua、不派发 del；内部不变量破坏 ⇒ L4。
    void clear_all() noexcept;

    /// 热重载代际边界：清空类存在位（热重载）。
    ///
    /// 【分派器引用不在这里】它在 script::state 里，由重载协调器一起清。
    /// 【可重复调用】不分配、不调 Lua。
    void reset_classes() noexcept;

    /// 本帧回收的记录（按回收顺序；每次 recycle_dead 先清空）。
    [[nodiscard]] const std::vector<recycled_record>& recycled() const noexcept {
        return recycled_;
    }

    /// 类表（引擎侧只存回调存在位；函数在库层）。
    [[nodiscard]] class_registry& classes() noexcept { return classes_; }
    [[nodiscard]] const class_registry& classes() const noexcept { return classes_; }

    [[nodiscard]] std::size_t pool_count() const noexcept { return pools_.size(); }
    /// 指定池的三个口径计数（池 id 非法 ⇒ L3，与 pool() 同口径）。
    [[nodiscard]] object_counts count_objects(
        std::uint32_t pool_id, std::optional<std::uint8_t> group) const;

    /// 所有池求和；空池直接跳过。
    [[nodiscard]] object_counts count_objects_all(
        std::optional<std::uint8_t> group) const noexcept;
    [[nodiscard]] container_pool& pool(std::uint32_t id);
    [[nodiscard]] const container_pool& pool(std::uint32_t id) const;

private:
    std::vector<std::unique_ptr<container_pool>> pools_;
    std::vector<recycled_record> recycled_;
    class_registry classes_;
};

} // namespace wag::container
