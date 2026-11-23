#pragma once

// =============================================================================
// platform/input_event.hpp
//
// 输入事件的**窄 POD**：平台层把它产生出来，输入层（src/input）消费它。
//
// 【为什么 POD 在 platform，而不在 input】
// 事件由平台产生 ⇒ 事件词汇归**生产者**。
// 于是依赖方向是：
// platform（定义 POD + sink）
// ↑
// input（实现 sink、解释事件）
// 而 window 只认 `input_sink`，**不认识 input 模块** ——
// 这保证"窗口层不知道输入语义"（与E.2 同一条纪律）。
//
// 【为什么不含任何 SDL 类型】
// · 让 input 模块完全不依赖 SDL；
// · 让单元测试可以脱离 SDL 直接构造事件（bench/input_check.cpp）。
//
// 【它只描述"发生了什么"，不描述"游戏怎么解释"】
// 不区分动作、不区分设备语义、不归一化到世界坐标；
// 这些都是上层的事。
// =============================================================================

#include <cstdint>

namespace wag::platform {

/// 事件种类。
///
/// 【为什么只有这五种】本期输入只做键盘/鼠标；
/// 文本、手柄、触摸**不在本期**（见I12）。
/// 将来加类型时，input 的 switch 会得到一条编译期提醒（无 default 时）。
enum class input_event_type : std::uint8_t {
    key_down,
    key_up,
    mouse_move,
    mouse_button_down,
    mouse_button_up,
    wheel,

 // ---- gamepad（2026-11-19，V1）----
    gamepad_added,
    gamepad_removed,
    gamepad_button_down,
    gamepad_button_up,
    gamepad_axis,

 /// 内部事件：失焦时清手柄状态；不进 on_event。
    gamepad_focus_lost,
};

/// gamepad slot 数量（V1 固定 8；G2）。
inline constexpr std::uint16_t gamepad_slot_count = 8;

/// gamepad 按钮/轴容量。实际 SDL 枚举比这个少；
/// 容量留余量，越界仍由 input/Lua 边界校验。
inline constexpr std::uint8_t gamepad_button_capacity = 32;
inline constexpr std::uint8_t gamepad_axis_capacity = 8;

/// 当前 SDL 3.4 的 gamepad 枚举个数（0-based 索引范围见下）。
/// 若 SDL 升级后枚举变化，必须同步这两个数。
inline constexpr std::uint8_t gamepad_button_count = 26;
inline constexpr std::uint8_t gamepad_axis_count = 6;

/// input_event::flags 的位定义。
inline constexpr std::uint8_t input_flag_repeat = 0x01;
inline constexpr std::uint8_t input_flag_resync = 0x02;

/// 一次输入事件。
///
/// 【字段的"谁填、谁读"】
/// · 平台填：type / scancode / button / flags / x / y / wheel_*；
/// · input 读：全部；`device` 本期**不读**（见下）。
///
/// 【x / y 的单位是"窗口坐标"（DIP），不是物理像素】
/// SDL3 的鼠标事件坐标是窗口坐标；而引擎的 `space::transform`
/// 把逻辑坐标映到**窗口坐标**（喂给 recompute 的是 DIP 尺寸）。
/// ⇒ 两边同单位，逆变换直接得到 DIP。
/// ⚠️ 若将来改喂像素，这里必须跟着改 —— 否则高 DPI 下会偏一倍。
struct input_event {
    input_event_type type{};

 /// key_*：SDL scancode（0..511，物理位置，布局无关）。
    std::uint16_t scancode = 0;

 /// mouse_button_*：1..5（与 SDL 的 LEFT/MIDDLE/RIGHT/X1/X2 一致）。
    std::uint8_t button = 0;

 /// key_* 的标志位。bit0 = repeat（SDL 的自动重复）。
 ///
 /// 【为什么把 repeat 传下来，而不是在平台层过滤】
 /// "重复不产生 Press" 是**输入语义**（I4），归输入层；
 /// 平台层只负责把 SDL 告诉它的事原样带下来。
    std::uint8_t flags = 0;

 /// 设备维度。
 /// · 键盘/鼠标：0（仍合并）
 /// · gamepad：slot 0..7
    std::uint16_t device = 0;

 /// gamepad 的 generation（键盘/鼠标恒 0）。
 /// 重连后旧 generation 的事件应被丢弃。
    std::uint16_t generation = 0;

 /// gamepad_axis：0-based SDL axis 索引。
    std::uint8_t axis = 0;

 /// gamepad_axis：原始归一化值（摇杆 [-1,1]，扳机 [0,1]）。
    float value = 0.0f;

 /// mouse_move：窗口坐标（DIP）。
    float x = 0.0f;
    float y = 0.0f;

 /// wheel：**已归一化**的增量（+x 右 / +y 上）。
 ///
 /// 【为什么归一化在平台层】
 /// 平台知道 SDL 的 `direction == FLIPPED`（系统自然滚动）；
 /// 输入层不应该知道 SDL 的枚举，只该看到稳定的引擎约定。
    float wheel_x = 0.0f;
    float wheel_y = 0.0f;
};

/// 输入事件的接收者（由 input 模块实现）。
///
/// 【为什么是接口而不是 std::function】
/// · 消息路径每个事件一次调用 ⇒ 虚调用比 std::function 的间接调用更直接；
/// · 不需要分配；
/// · 生命周期是显式的（window 持有一个裸指针，engine 负责接/断）。
struct input_sink {
    virtual ~input_sink() = default;

 /// 接收一个事件。
 ///
 /// 【调用点】window::note_event()（消息路径）。
 /// 【硬约束】实现方**不得在这里调用脚本** ——
 /// 消息路径可以在任意时刻被系统调用（见 window::pump_events 的说明）。
 /// input 的 feed 只做状态更新，不碰 Lua。
    virtual void feed(const input_event& ev) = 0;
};

/// 键盘 scancode 的个数（SDL3：0..511）。
///
/// 【为什么要这个常量】input 的位图大小与 Lua 的越界校验都用它；
/// 写死 512 会在 SDL 改变时变成一个"看起来对"的错。
inline constexpr std::uint16_t scancode_count = 512;

/// 鼠标按钮个数（1..5）。
inline constexpr std::uint8_t mouse_button_count = 5;

} // namespace wag::platform
