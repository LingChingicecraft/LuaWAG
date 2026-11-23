// =============================================================================
//  sdl_gl_grab.hpp —— 像素采样的 driver（唯一平台相关的一层）
//
//  【它只做三件事】建上下文 / 把"默认帧缓冲"读成 imgprobe::image / 演示用清屏。
//  【纪律】grab() 必须在 present() 之前调用。
//      原因：双缓冲下 swap 之后后缓冲内容未定义（驱动可能返回垃圾）——
//      这是渲染调试里最经典的一条假失败来源。
//
//  【坐标】本文件的 rect 参数一律是"帧缓冲像素、左上原点"（与 image 同域）。
//      GL 的 scissor 是左下原点 ⇒ 换算只在本文件发生（一处）。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

#include "img_probe.hpp"

namespace imgprobe::sdl_gl {

struct config {
    int w = 320;
    int h = 240;
    bool hidden = false;         ///< 无头模式：不显示窗口（仍建 GL 上下文）
    const char* title = "LuaWAG devtools · img_probe";
};

/// 建窗口 + GL 3.3 Core 上下文。失败时把原因写进 err 并返回 false。
bool create(const config& cfg, std::string& err);

/// 关上下文与窗口（幂等）。
void destroy();

[[nodiscard]] bool alive() noexcept;

/// 帧缓冲像素尺寸（可能 != 窗口 DIP 尺寸）。
[[nodiscard]] int fb_w() noexcept;
[[nodiscard]] int fb_h() noexcept;

/// 把整个默认帧缓冲读成 image（左上原点）。
/// 【前置条件】必须在 present() 之前调用。
imgprobe::image grab(std::string& err);

/// 交换缓冲。
void present();

/// 清整屏（会临时关掉 scissor）。
void clear_all(std::uint32_t argb);

/// 用 scissor + clear 填一个矩形（帧缓冲像素、左上原点）。
/// 这不是"绘制"，是"造一个已知图案"——driver 里唯一需要的图案生成手段。
void clear_rect(imgprobe::rect r, std::uint32_t argb);

/// 恢复整屏 viewport / 关 scissor（grab 之前调一次，保证状态干净）。
void reset_state();

/// 最近一次 GL 错误（0 表示无）。
[[nodiscard]] unsigned last_gl_error() noexcept;

} // namespace imgprobe::sdl_gl
