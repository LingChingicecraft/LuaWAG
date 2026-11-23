// =============================================================================
//  audio/loop_logic.hpp
//
//  循环推进的唯一实现（混音线程每帧调用它；测试也直接测它）。
//
//  【语义】
//      首次从 0 播到 loop_end，之后在 [loop_start, loop_end) 之间循环。
//      这就是 STG BGM 的“前奏 + 循环体”。
//
//  【为什么单位是“采样帧”而不是秒】
//      秒要乘采样率、再四舍五入 ⇒ 长曲累积漂移。按帧对齐是精确的。
// =============================================================================

#pragma once

#include <cstdint>

namespace wag::audio {

/// **检查位置是否越界并回绕**；返回 false 表示"这条 voice 该结束了"。
///
/// @param pos        当前位置（帧）。**只在回绕时被改写**；递增由调用方做
/// @param loop_start 循环体起点（帧）；非循环时无意义
/// @param loop_end   循环体终点（帧，**独占**）；等于总帧数即"循环到结尾"
/// @param loop       是否循环
///
/// 【调用形状】`while (… ) { if (!wrap_position(pos,…)) break; 读 pos; ++pos; }`
/// 【注意】调用方必须保证 `loop_start < loop_end <= 总帧数`
///   （`play` 在入口处校验并 clamp；这里不做防御，以免每帧多一次比较）。
inline bool wrap_position(std::uint64_t& pos, std::uint32_t loop_start,
                          std::uint32_t loop_end, bool loop) {
    if (pos < loop_end) {
        return true;            // 还在播放区间内
    }
    if (!loop) {
        return false;           // 播完了
    }
    pos = loop_start;           // 回绕到循环体起点（采样级，无淡出、无间隙）
    return true;
}

} // namespace wag::audio
