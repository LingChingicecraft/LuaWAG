// =============================================================================
// light/occluder_pool.hpp —— 遮挡者池（2026-11-18，设计见D1）
//
// 【它是什么】住着"会挡光的四边形"的专用池：
// · 容量固定 32767（同规），池满 ⇒ 调用方报 L3
// · 句柄 = (gen << 15) | index（53 位以内；pool 位留给多池时再切）
// · 稳定下标：不做 swap-remove（句柄永远有效）
// · 每帧由引擎遍历存活条目，画进遮蔽图（depth 分桶、从远到近）
// 【本文件是 header-only】它没有 GL 依赖；画遮蔽图在 renderer。
// =============================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace wag::light {

/// 一个遮挡者（约 32 B）。
struct occluder {
    std::uint64_t image = 0;   ///< 贴图句柄；0 = 无贴图（覆盖率按 1 算）
    float x = 0.0f;            ///< 中心 x（世界坐标）
    float y = 0.0f;            ///< 中心 y
    float w = 0.0f;            ///< 宽（世界单位）
    float h = 0.0f;            ///< 高
    float rot = 0.0f;          ///< 旋转（度，逆时针，绕中心）
    std::uint8_t depth = 0;    ///< 0–255（越小越"近"？见下）
    std::uint8_t transmission = 0; ///< 0=全挡，255=全透
    std::uint32_t gen = 1;     ///< 代数
    bool alive = false;        ///< 存活
};

/// 遮挡者池。
class occluder_pool {
public:
    static constexpr std::uint32_t capacity = 32767;

    occluder_pool() : data_(capacity), free_(capacity) {
        for (std::uint32_t i = 0; i < capacity; ++i) {
            free_[i] = capacity - 1 - i; // 从 0 开始分配
        }
    }

    [[nodiscard]] std::uint32_t alive_count() const noexcept { return alive_; }
    [[nodiscard]] static bool valid_handle(std::uint64_t h) noexcept {
        return (h >> 15) != 0; // gen 至少为 1
    }

 /// 生成。满池 ⇒ 返回 0（调用方报 L3）。
    std::uint64_t spawn(const occluder& src) {
        if (free_.empty()) {
            return 0;
        }
        const std::uint32_t index = free_.back();
        free_.pop_back();
        occluder& o = data_[index];
        const std::uint32_t gen = o.gen;
        o = src;
        o.gen = gen;
        o.alive = true;
        ++alive_;
        return (static_cast<std::uint64_t>(gen) << 15) | index;
    }

 /// 改写（句柄失效 ⇒ false）。
    bool write(std::uint64_t handle, const occluder& src) {
        const std::uint32_t index = static_cast<std::uint32_t>(handle & 0x7FFFu);
        const std::uint32_t gen = static_cast<std::uint32_t>(handle >> 15);
        if (index >= capacity || !data_[index].alive || data_[index].gen != gen) {
            return false;
        }
        occluder& o = data_[index];
        const std::uint32_t keep_gen = o.gen;
        o = src;
        o.gen = keep_gen;
        o.alive = true;
        return true;
    }

 /// 销毁（句柄失效 ⇒ false）。稳定下标：不搬动其它条目。
    bool kill(std::uint64_t handle) {
        const std::uint32_t index = static_cast<std::uint32_t>(handle & 0x7FFFu);
        const std::uint32_t gen = static_cast<std::uint32_t>(handle >> 15);
        if (index >= capacity || !data_[index].alive || data_[index].gen != gen) {
            return false;
        }
        occluder& o = data_[index];
        o.alive = false;
        ++o.gen; // 旧句柄失效
        if (o.gen == 0) {
            o.gen = 1;
        }
        free_.push_back(index);
        --alive_;
        return true;
    }

 /// 遍历存活条目（渲染遮蔽图用）。O(容量) 扫描（与容器池第一版同型）。
    template <typename Fn>
    void for_each_live(Fn&& fn) const {
        for (std::uint32_t i = 0; i < capacity; ++i) {
            if (data_[i].alive) {
                fn(data_[i]);
            }
        }
    }

 /// 只读取一条（调试/测试用）。
    [[nodiscard]] const occluder& at(std::uint32_t index) const { return data_[index]; }

private:
    std::vector<occluder> data_;
    std::vector<std::uint32_t> free_;
    std::uint32_t alive_ = 0;
};

} // namespace wag::light
