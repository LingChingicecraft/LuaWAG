#pragma once

// =============================================================================
// input/input.hpp
//
// 输入采样：把 platform 层喂来的事件解释成 **Down / Press / Release**，
// 并在阶段 4 冻结成本帧快照。
//
// 【规则编号】见I1–I14。
//
// 【两套状态：累加器 + 快照】
// feed() 写**累加器**（down_live / press_acc / release_acc / 滚轮累加）；
// sample() 冻结成**快照**（*_frame）并清掉"边沿"累加器。
// ⇒ 阶段 5/6 的查询读快照 —— 保证"一帧内读到的是同一份值"（公理 13）。
// ⇒ 不在查询时计算（否则查询顺序会影响结果）。
//
// 【为什么位图而不是容器】
// 512 个 scancode 的 down/press/release 各 64 B；bitset 的按位与/取反
// 正好表达 I2 的 "release = release_acc & ~press_acc"（P 占帧）。
//
// 【它不认识 SDL / 窗口 / 焦点 / viewport】
// · 只收 platform::input_event；
// · 鼠标 DIP 换算所需的 transform 由调用方传入（可测试）；
// · 焦点不在这里处理（E.2）。
// =============================================================================

#include "platform/input_event.hpp"
#include "space/rect.hpp"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wag::input {

/// 输入状态机。
///
/// 【它不是引擎部件】它不持有窗口/渲染器；引擎把它当值成员持有，
/// 阶段 4 调 sample()，消息路径调 feed()。
class input : public platform::input_sink {
public:
 // -------------------------------------------------------------------------
 // 事件输入（消息路径调用；只写数据，不调脚本）
 // -------------------------------------------------------------------------

 /// 接收一个平台事件（实现 input_sink）。
 ///
 /// 【调用点】window::note_event()（阶段 2 的等待里 / pump）。
 /// 【它不调脚本】消息路径的硬约束。
 /// 【它不做 P/R 裁决】裁决在 sample() —— 因为 P 占帧需要**整帧**的事件。
    void feed(const platform::input_event& ev) override;

 // -------------------------------------------------------------------------
 // 本帧事件批次
 // -------------------------------------------------------------------------

 /// 本帧收到的输入事件，**按发生顺序**。
 ///
 /// 【什么时候冻结】`sample()` 里一次性换出（与其它快照同一时刻）。
 /// ⇒ 阶段 4 之后读到的，就是"本帧的"那一批；下一帧 sample 会把它换掉。
 ///
 /// 【为什么要这一批】`on_event` 走**每帧一次批量**，
 /// 而不是每事件一次回调 —— 帧内只应有一个快照。
    [[nodiscard]] const std::vector<platform::input_event>& frame_events() const noexcept {
        return frame_events_;
    }

 /// 把"按住"状态整体清零（**失焦时调**；不动本帧的边沿）。
 ///
 /// 【为什么需要它 —— 而且它**不是**修 bug】
 /// 实测（2026-11-16）：SDL 在窗口失焦时会**自己合成松开**，所以卡键不会发生。
 /// 但那是**观测到的行为**，不是契约；清一次的成本是几个 bitset 清零
 /// ⇒ 不把“不卡键”寄托在别人的实现细节上。
    void clear_down() noexcept;

 // -------------------------------------------------------------------------
 // 阶段 4：冻结本帧快照
 // -------------------------------------------------------------------------

 /// 把累加器冻结成本帧快照，并清掉边沿累加器。
 ///
 /// @param t 本帧的"逻辑→窗口坐标"变换（阶段 1 由 viewport 算好）。
 /// 鼠标位置用它做逆变换得到逻辑坐标（DIP）。
 ///
 /// 【顺序固定】见 .cpp 里的注释：先 P/R 裁决，再冻结 down，再换算鼠标，
 /// 最后清累加器。顺序错了会出现"本帧的 Release 用了下一帧的 Press"。
 ///
 /// 【退化变换】t.m00/t.m11 为 0（窗口最小化）时**不换算**，
 /// 保留上一次的逻辑坐标 —— 不能除零。
    /// @param target_box 内部渲染目标在输出上的矩形（窗口像素；letterbox/stretch）
    /// @param target_w 内部渲染目标宽（像素）
    /// @param target_h 内部渲染目标高（像素）
    /// 传空矩形时表示不做输出→目标映射（旧行为）。
    void sample(const space::transform& t,
                const space::rect& target_box = space::rect{},
                int target_w = 0, int target_h = 0);

 // -------------------------------------------------------------------------
 // 查询（阶段 5/6/7 的钩子读；O(1)）
 // -------------------------------------------------------------------------

 /// 本帧采样时是否按住（I1 的 Down）。
    [[nodiscard]] bool down(std::uint16_t scancode) const;

 /// 本帧是否发生过"按下"（I1 的 Press；P 占帧 ⇒ 同帧无 Release）。
    [[nodiscard]] bool press(std::uint16_t scancode) const;

 /// 本帧是否发生过"松开"（I1 的 Release；被 P 占帧时恒 false）。
    [[nodiscard]] bool release(std::uint16_t scancode) const;

 /// 鼠标位置：**逻辑坐标（DIP）**。
    [[nodiscard]] double mouse_x() const noexcept { return mouse_dip_x_; }
    [[nodiscard]] double mouse_y() const noexcept { return mouse_dip_y_; }

 /// 鼠标按钮三态（button = 1..5）。
    [[nodiscard]] bool mouse_down(std::uint8_t button) const;
    [[nodiscard]] bool mouse_press(std::uint8_t button) const;
    [[nodiscard]] bool mouse_release(std::uint8_t button) const;

 /// 滚轮本帧增量（+x 右 / +y 上；采样后清零）。
    [[nodiscard]] double wheel_x() const noexcept { return wheel_frame_x_; }
    [[nodiscard]] double wheel_y() const noexcept { return wheel_frame_y_; }

 /// 输入重载事务：清逻辑/瞬态状态，保留物理按住与鼠标位置。
 ///
 /// 【调用点】热重载闸门内、关闭旧 Lua 之前。
 /// 【不调 Lua】只清位图/数组/事件批次。
 /// 【不清 down】新状态首帧仍应看到"玩家当前按住"，而不是要求松手重按。
    void prepare_reload() noexcept;

 // -------------------------------------------------------------------------
 // 手柄查询（2026-11-19，V1）
 // -------------------------------------------------------------------------

 /// 已连接的 gamepad slot 列表（升序）。
    [[nodiscard]] std::vector<std::uint16_t> gamepad_devices() const;

    [[nodiscard]] bool gamepad_connected(std::uint16_t device) const;
    [[nodiscard]] bool gamepad_down(std::uint16_t device,
                                    std::uint8_t button) const;
    [[nodiscard]] bool gamepad_press(std::uint16_t device,
                                     std::uint8_t button) const;
    [[nodiscard]] bool gamepad_release(std::uint16_t device,
                                       std::uint8_t button) const;
    [[nodiscard]] double gamepad_axis(std::uint16_t device,
                                      std::uint8_t axis) const;

private:
    using key_bits = std::bitset<static_cast<std::size_t>(platform::scancode_count)>;
    using button_bits =
        std::bitset<static_cast<std::size_t>(platform::mouse_button_count)>;

    static void check_scancode(std::uint16_t scancode);
    static void check_button(std::uint8_t button);
    static void check_gamepad_device(std::uint16_t device);
    static void check_gamepad_button(std::uint8_t button);
    static void check_gamepad_axis(std::uint8_t axis);
    [[nodiscard]] static std::size_t button_index(std::uint8_t button) noexcept {
        return static_cast<std::size_t>(button - 1u);
    }

    static constexpr std::size_t kGamepadSlots =
        static_cast<std::size_t>(platform::gamepad_slot_count);
    static constexpr std::size_t kGamepadButtons =
        static_cast<std::size_t>(platform::gamepad_button_capacity);
    static constexpr std::size_t kGamepadAxes =
        static_cast<std::size_t>(platform::gamepad_axis_capacity);
    using gamepad_button_bits = std::bitset<kGamepadButtons>;

    struct gamepad_state {
        bool connected = false;
        bool faulted = false;
        std::uint16_t generation = 0;

        gamepad_button_bits down_live{};
        gamepad_button_bits press_acc{};
        gamepad_button_bits release_acc{};
        gamepad_button_bits down_frame{};
        gamepad_button_bits press_frame{};
        gamepad_button_bits release_frame{};

        std::array<float, kGamepadAxes> axis_live{};
        std::array<float, kGamepadAxes> axis_frame{};
    };

    void clear_gamepads() noexcept;

 // ---- 累加器（feed 写）----
    key_bits down_live_{};
    key_bits press_acc_{};
    key_bits release_acc_{};

 // ---- 本帧快照（sample 写；查询读）----
    key_bits down_frame_{};
    key_bits press_frame_{};
    key_bits release_frame_{};

 // ---- 鼠标按钮（同样的两套）----
    button_bits button_down_live_{};
    button_bits button_press_acc_{};
    button_bits button_release_acc_{};
    button_bits button_down_frame_{};
    button_bits button_press_frame_{};
    button_bits button_release_frame_{};

 // ---- 鼠标 ----
 /// 平台给的**窗口坐标**（DIP）；每次 motion 覆盖（只要最后位置）。
    float mouse_pos_x_ = 0.0f;
    float mouse_pos_y_ = 0.0f;

 /// 本帧快照：逻辑坐标（DIP）。
    double mouse_dip_x_ = 0.0;
    double mouse_dip_y_ = 0.0;

 /// 滚轮累加器 / 本帧快照。
    double wheel_acc_x_ = 0.0;
    double wheel_acc_y_ = 0.0;
    double wheel_frame_x_ = 0.0;
    double wheel_frame_y_ = 0.0;

 // ---- gamepad 每设备状态 ----
    std::array<gamepad_state, kGamepadSlots> gamepads_{};

 // ---- 事件批次（2026-11-16）----
 /// feed 累积的**下一帧**的批次；sample 时换出到 frame_events_。
    std::vector<platform::input_event> pending_events_;

 /// 本帧批次（sample 换入；下一帧 sample 换掉）。
    std::vector<platform::input_event> frame_events_;
};

} // namespace wag::input
