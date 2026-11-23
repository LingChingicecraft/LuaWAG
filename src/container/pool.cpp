// =============================================================================
// container/pool.cpp
//
// 单池与多池的实现。规则出处见 pool.hpp 顶部（/ /）。
//
// 【本文件只做四件事】
// 分配、查询、标记 dead、帧末回收。
// 它**不**知道 class、img、碰撞、渲染 —— 那些是上层的事。
// =============================================================================

#include "container/pool.hpp"

#include "error/error.hpp"
#include "log/log.hpp"

#include <string>
#include <cstdint>
#include <cstdlib>
#include <utility>

namespace wag::container {
namespace {

/// 新槽位的初始值。
/// 【默认值来自字段表】sx/sy 默认 1.0；visible 默认可见；
/// 其余为 0（img=0 表示无贴图，class_id 由 spawn 写入）。
void reset_container(container& c) noexcept {
    c = container{};
    c.sx = 1.0f;
    c.sy = 1.0f;
    c.rgba = 0xFFFFFFFFu;
    c.visible = 1;
}

/// 80% 阈值：32767 × 0.8 = 26213.6 ⇒ 向上取整 26214（"达 80%"）。
constexpr std::uint32_t warning_threshold =
    (pool_capacity * 8u + 9u) / 10u;

std::string pool_full_message(const std::string& name, std::uint32_t alive) {
 //：错误信息必须含池名 / 容量 / 当前存活数
    return "[池] " + name + " 已满：容量 32767，存活 " + std::to_string(alive) +
           "（多池是强制的结构：请为该类容器另开一池）";
}

[[noreturn]] void fail_bad_handle(const container_pool& pool, handle h,
                                  const char* why) {
    fail_internal("容器句柄失效：池「" + pool.name() + "」，下标 " +
                  std::to_string(handle_index(h)) + "，" + why +
                  "（handle=" + std::to_string(h) + "）");
}

} // namespace

const char* handle_failure_name(handle_failure f) noexcept {
 // 【措辞与 get() 的诊断逐条对齐】两处说的是同一件事，就不该有两种说法。
 // get() 那边是 fail_bad_handle(*this, h, "容器已销毁") 之类；
 // 这里是给它一个"不报错"的读法。将来 get() 改用本函数时，措辞自然统一。
    switch (f) {
    case handle_failure::none:
        return "有效";
    case handle_failure::invalid_handle:
        return "句柄为 0（invalid_handle）";
    case handle_failure::bad_pool:
        return "池 id 不存在，或与所属池不符";
    case handle_failure::bad_index:
        return "下标越界";
    case handle_failure::dead:
        return "容器已销毁";
    case handle_failure::stale_generation:
        return "代号不匹配（旧句柄）";
    }
    return "未知的失效原因";
}

// -----------------------------------------------------------------------------
// container_pool
// -----------------------------------------------------------------------------

container_pool::container_pool(std::uint32_t pool_id, std::string name)
    : pool_id_(pool_id), name_(std::move(name)) {
 // 一次性分配：容量固定 ⇒ 之后没有任何扩容/搬迁
 // 【data_ 必须与 meta_ 同长】try_allocate 直接 data_[idx]；
 // 只 resize meta_ 会让第一次 spawn 就下标访问空 vector（灾难恢复时丢过这一行）。
    data_.resize(pool_capacity);
    meta_.resize(pool_capacity);

 // 空闲表从大到小填，于是 back() 先给出下标 0（诊断时更直观）
    free_.reserve(pool_capacity);
    for (std::uint32_t i = pool_capacity; i > 0; --i) {
        free_.push_back(i - 1);
    }
}

container* container_pool::try_allocate(handle& out) noexcept {
    out = invalid_handle;
    if (free_.empty()) {
 // 没有空位：可能是"全活"也可能是"全活+待回收"。
 // 两种都算满（：待死槽不算空位）。
        return nullptr;
    }

    const std::uint32_t idx = free_.back();
    free_.pop_back();

    slot_meta& m = meta_[idx];
    m.state = 1; // 存活

    container& c = data_[idx];
    reset_container(c);

    ++alive_;
    if (alive_ > peak_) {
        peak_ = alive_;
    }

    out = make_handle(pool_id_, idx, m.gen);
    check_warning_();
    return &c;
}

container* container_pool::allocate(handle& out) {
    container* c = try_allocate(out);
    if (c == nullptr) {
        fail_internal(full_message());
    }
    return c;
}

std::string container_pool::full_message() const {
 // 【消息只在这里拼一次】allocate 与交界处都从这里取（见 pool.hpp 的说明）。
    return pool_full_message(name_, alive_);
}

container* container_pool::try_get(handle h) noexcept {
    if (h == invalid_handle) {
        return nullptr;
    }
    if (handle_pool(h) != pool_id_) {
        return nullptr;
    }
    const std::uint32_t idx = handle_index(h);
    if (idx >= pool_capacity) {
        return nullptr;
    }
    const slot_meta& m = meta_[idx];
    if (m.state != 1) {
        return nullptr;
    }
    if (m.gen != handle_gen(h)) {
        return nullptr;
    }
    return &data_[idx];
}

handle_failure container_pool::probe(handle h) const noexcept {
 // 【分支顺序必须与 get() 完全一致】否则同一个坏句柄在交界处与深层
 // 会得到两种说法：深层说"已销毁"，交界处说"代号不匹配"。
    if (h == invalid_handle) {
        return handle_failure::invalid_handle;
    }
    if (handle_pool(h) != pool_id_) {
        return handle_failure::bad_pool;
    }
    const std::uint32_t idx = handle_index(h);
    if (idx >= pool_capacity) {
        return handle_failure::bad_index;
    }
    const slot_meta& m = meta_[idx];
    if (m.state != 1) {
        return handle_failure::dead;
    }
    if (m.gen != handle_gen(h)) {
        return handle_failure::stale_generation;
    }
    return handle_failure::none;
}

container* container_pool::get(handle h) {
    container* c = try_get(h);
    if (c != nullptr) {
        return c;
    }

 // 逐种原因报清楚：同一个"取不到"有四种完全不同的成因
    if (h == invalid_handle) {
        fail_bad_handle(*this, h, "句柄为 0（invalid_handle）");
    }
    if (handle_pool(h) != pool_id_) {
        fail_bad_handle(*this, h, "池 id 与本池不符");
    }
    const std::uint32_t idx = handle_index(h);
    if (idx >= pool_capacity) {
        fail_bad_handle(*this, h, "下标越界");
    }
    if (meta_[idx].state != 1) {
        fail_bad_handle(*this, h, "容器已销毁");
    }
    fail_bad_handle(*this, h, "代号不匹配（旧句柄）");
}

void container_pool::kill(handle h) {
 // 经 get 校验：无效句柄在这里就 L3，不会静默漏过
    (void)get(h);
    const std::uint32_t idx = handle_index(h);

    slot_meta& m = meta_[idx];
    if (m.state == 2) {
 // 同一帧重复 kill：第二次是脚本错误，必须报出来（不静默）
        fail_internal("容器被重复销毁：池「" + name_ + "」，下标 " +
                      std::to_string(idx) + "（同一帧 kill 两次）");
    }
    m.state = 2;
    dead_.push_back(idx);
 // alive_ 不变：标记 dead 的槽在帧末回收前仍然占用
}

object_counts container_pool::count_objects(
    std::optional<std::uint8_t> group) const noexcept {
    object_counts n;
    for (std::uint32_t i = 0; i < pool_capacity; ++i) {
        const std::uint8_t st = meta_[i].state;
        if (st == 0) {
            continue;
        }
        const container& c = data_[i];
        if (group && c.group != *group) {
            continue;
        }
        ++n.occupied;
        if (st == 1) {
            ++n.live;
            if (c.visible != 0 && c.img != 0) {
                ++n.renderable;
            }
        }
    }
    return n;
}

object_counts container_pool_set::count_objects(
    std::uint32_t pool_id, std::optional<std::uint8_t> group) const {
    return pool(pool_id).count_objects(group);
}

object_counts container_pool_set::count_objects_all(
    std::optional<std::uint8_t> group) const noexcept {
    object_counts total;
    for (const auto& p : pools_) {
        if (p->alive_count() == 0) {
            continue;
        }
        const object_counts n = p->count_objects(group);
        total.live += n.live;
        total.occupied += n.occupied;
        total.renderable += n.renderable;
    }
    return total;
}

void container_pool::recycle_dead(std::vector<recycled_record>* out) {
    for (const std::uint32_t idx : dead_) {
        slot_meta& m = meta_[idx];
        if (out != nullptr) {
 //：del 拿到的是"回收前"仍然有效的句柄（gen 尚未 +1）。
            out->push_back(recycled_record{data_[idx].class_id,
                                           make_handle(pool_id_, idx, m.gen)});
        }
        m.state = 0;
        m.gen = next_gen(m.gen);
        free_.push_back(idx);
        --alive_;
    }
    dead_.clear();
    check_warning_();
}

void container_pool::check_warning_() {
    if (alive_ >= warning_threshold) {
        if (!warned_) {
            warned_ = true;
            ++warn_entries_;
 //：每次进入该区间只记一次；信息含池名与 n / 32767
            log::record("池", "" + name_ + " 达 80%（" + std::to_string(alive_) +
                        " / 32767）");
        }
    } else {
        warned_ = false;
    }
}

// -----------------------------------------------------------------------------
// container_pool_set
// -----------------------------------------------------------------------------

std::uint32_t container_pool_set::create_pool(std::string name) {
    const std::uint32_t id = static_cast<std::uint32_t>(pools_.size());
    if (id >= max_pools) {
        fail_internal("容器池数量已达上限 4096（池 id 为 u12，且永不复用）");
    }
    pools_.push_back(std::make_unique<container_pool>(id, std::move(name)));
    return id;
}

void container_pool_set::clear_all() noexcept {
 // /：只在热重载代际边界调用。
 // pools_.size() 就是下一个池 id 的来源 ⇒ clear 后 id 从 0 重新开始。
 // 不派发 del / kill：旧 Lua 即将关闭，回调没有落脚点。
    pools_.clear();
    recycled_.clear();
}

void container_pool_set::reset_classes() noexcept {
 // /：只清类存在位；分派器引用由 script::state 的重载事务清。
    classes_.clear();
}

container* container_pool_set::allocate(std::uint32_t pool_id, handle& out) {
    if (pool_id >= pools_.size()) {
        out = invalid_handle;
        fail_internal("容器池 id 不存在：" + std::to_string(pool_id) +
                      "（当前池数 " + std::to_string(pools_.size()) + "）");
    }
    return pools_[pool_id]->allocate(out);
}

container* container_pool_set::get(handle h) {
    const std::uint32_t id = handle_pool(h);
    if (id >= pools_.size()) {
        fail_internal("容器句柄失效：池 id " + std::to_string(id) +
                      " 不存在（handle=" + std::to_string(h) + "）");
    }
    return pools_[id]->get(h);
}

container* container_pool_set::try_get(handle h) noexcept {
    const std::uint32_t id = handle_pool(h);
    if (id >= pools_.size()) {
        return nullptr;
    }
    return pools_[id]->try_get(h);
}

handle_failure container_pool_set::probe(handle h) const noexcept {
 // 【与 try_get 同构，只是把"没有"换成"为什么没有"】
    if (h == invalid_handle) {
        return handle_failure::invalid_handle;
    }
    const std::uint32_t id = handle_pool(h);
    if (id >= pools_.size()) {
        return handle_failure::bad_pool;
    }
    return pools_[id]->probe(h);
}

void container_pool_set::kill(handle h) {
    const std::uint32_t id = handle_pool(h);
    if (id >= pools_.size()) {
        fail_internal("容器句柄失效：池 id " + std::to_string(id) +
                      " 不存在（handle=" + std::to_string(h) + "）");
    }
    pools_[id]->kill(h);
}

void container_pool_set::recycle_dead() {
    recycled_.clear();
    for (auto& p : pools_) {
        p->recycle_dead(&recycled_);
    }
}

container_pool& container_pool_set::pool(std::uint32_t id) {
    if (id >= pools_.size()) {
        fail_internal("容器池 id 不存在：" + std::to_string(id) + "（当前池数 " +
                      std::to_string(pools_.size()) + "）");
    }
    return *pools_[id];
}

const container_pool& container_pool_set::pool(std::uint32_t id) const {
    if (id >= pools_.size()) {
        fail_internal("容器池 id 不存在：" + std::to_string(id) + "（当前池数 " +
                      std::to_string(pools_.size()) + "）");
    }
    return *pools_[id];
}

} // namespace wag::container
