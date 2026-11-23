#include "input/input.hpp"

#include "error/error.hpp"
#include "log/log.hpp"

#include <cmath>
#include <string>

namespace wag::input {

namespace {

// 边界校验的文本要活到 fail_boundary 返回 —— 它不返回，但 string_view 的
// 生命周期仍必须覆盖调用。用一个局部 std::string 即可。
[[noreturn]] void fail_scancode(std::uint16_t scancode) {
    const std::string got = std::to_string(static_cast<unsigned>(scancode));
    error_value e;
    e.cls = error_class::boundary_validation;
    e.where = "wag.input 的 scancode";
    e.got = got;
    e.want = "0..511（SDL scancode；名字请用 Lua 侧 Lkeycode）";
    fail_boundary(e);
}

[[noreturn]] void fail_button(std::uint8_t button) {
    const std::string got = std::to_string(static_cast<unsigned>(button));
    error_value e;
    e.cls = error_class::boundary_validation;
    e.where = "wag.input 的鼠标按钮";
    e.got = got;
    e.want = "1..5（LEFT=1 / MIDDLE=2 / RIGHT=3 / X1=4 / X2=5）";
    fail_boundary(e);
}

[[noreturn]] void fail_gamepad_device(std::uint16_t device) {
    const std::string got = std::to_string(static_cast<unsigned>(device));
    error_value e;
    e.cls = error_class::boundary_validation;
    e.where = "wag.input 的 gamepad device";
    e.got = got;
    e.want = "0..7（gamepad slot）";
    fail_boundary(e);
}

[[noreturn]] void fail_gamepad_button(std::uint8_t button) {
    const std::string got = std::to_string(static_cast<unsigned>(button));
    error_value e;
    e.cls = error_class::boundary_validation;
    e.where = "wag.input 的 gamepad button";
    e.got = got;
    e.want = "0..25（SDL_GAMEPAD_BUTTON_*）";
    fail_boundary(e);
}

[[noreturn]] void fail_gamepad_axis(std::uint8_t axis) {
    const std::string got = std::to_string(static_cast<unsigned>(axis));
    error_value e;
    e.cls = error_class::boundary_validation;
    e.where = "wag.input 的 gamepad axis";
    e.got = got;
    e.want = "0..5（SDL_GAMEPAD_AXIS_*）";
    fail_boundary(e);
}

} // namespace

// =============================================================================
//  校验（只在查询点做；feed 由平台保证，但仍防御）
// =============================================================================

void input::check_scancode(std::uint16_t scancode) {
    if (scancode >= platform::scancode_count) {
        fail_scancode(scancode);
    }
}

void input::check_button(std::uint8_t button) {
    if (button < 1u || button > platform::mouse_button_count) {
        fail_button(button);
    }
}

void input::check_gamepad_device(std::uint16_t device) {
    if (device >= platform::gamepad_slot_count) {
        fail_gamepad_device(device);
    }
}

void input::check_gamepad_button(std::uint8_t button) {
    if (button >= platform::gamepad_button_count) {
        fail_gamepad_button(button);
    }
}

void input::check_gamepad_axis(std::uint8_t axis) {
    if (axis >= platform::gamepad_axis_count) {
        fail_gamepad_axis(axis);
    }
}

// =============================================================================
//  feed —— 消息路径：只写数据，不做 P/R 裁决
// =============================================================================

void input::feed(const platform::input_event& ev) {
    // 【先记进批次】`on_event` 是按**本帧发生了什么**通知脚本的，
    //   所以每个事件不管是否改变状态，都要进这一批。
    //   focus_lost 与 resync 是引擎内部事件，不交给脚本。
    const bool internal =
        (ev.type == platform::input_event_type::gamepad_focus_lost) ||
        ((ev.flags & platform::input_flag_resync) != 0u);
    if (!internal) {
        pending_events_.push_back(ev);
    }

    switch (ev.type) {
    case platform::input_event_type::gamepad_focus_lost:
        clear_gamepads();
        break;

    case platform::input_event_type::gamepad_added: {
        check_gamepad_device(ev.device);
        gamepad_state& gp = gamepads_[ev.device];
        gp.connected = true;
        gp.faulted = false;
        gp.generation = ev.generation;
        gp.down_live.reset();
        gp.press_acc.reset();
        gp.release_acc.reset();
        gp.down_frame.reset();
        gp.press_frame.reset();
        gp.release_frame.reset();
        gp.axis_live.fill(0.0f);
        gp.axis_frame.fill(0.0f);
        break;
    }

    case platform::input_event_type::gamepad_removed: {
        check_gamepad_device(ev.device);
        gamepad_state& gp = gamepads_[ev.device];
        if (!gp.connected || gp.generation != ev.generation) {
            break; // 旧 generation 的迟到事件：丢弃
        }
        gp = gamepad_state{};
        break;
    }

    case platform::input_event_type::gamepad_button_down:
    case platform::input_event_type::gamepad_button_up: {
        check_gamepad_device(ev.device);
        check_gamepad_button(ev.button);
        gamepad_state& gp = gamepads_[ev.device];
        if (!gp.connected || gp.faulted || gp.generation != ev.generation) {
            break;
        }
        const std::size_t idx = static_cast<std::size_t>(ev.button);
        const bool resync = (ev.flags & platform::input_flag_resync) != 0u;
        if (ev.type == platform::input_event_type::gamepad_button_down) {
            gp.down_live.set(idx);
            if (!resync) {
                gp.press_acc.set(idx);
            }
        } else {
            if (!resync && gp.down_live.test(idx)) {
                gp.release_acc.set(idx);
            }
            gp.down_live.reset(idx);
        }
        break;
    }

    case platform::input_event_type::gamepad_axis: {
        check_gamepad_device(ev.device);
        check_gamepad_axis(ev.axis);
        gamepad_state& gp = gamepads_[ev.device];
        if (!gp.connected || gp.faulted || gp.generation != ev.generation) {
            break;
        }
        const float v = ev.value;
        const bool finite = std::isfinite(v);
        const bool trigger = ev.axis >= 4;
        const float lo = trigger ? 0.0f : -1.0f;
        const float hi = 1.0f;
        if (!finite || v < lo - 0.0001f || v > hi + 0.0001f) {
            // G6/G7：硬件异常 ⇒ 隔离该设备，不污染快照。
            log::warn("input", log::format("手柄轴值异常，隔离设备",
                                           std::to_string(ev.device) + " / " +
                                               std::to_string(ev.axis) + " = " +
                                               std::to_string(v)));
            gp.faulted = true;
            gp.down_live.reset();
            gp.press_acc.reset();
            gp.release_acc.reset();
            gp.down_frame.reset();
            gp.press_frame.reset();
            gp.release_frame.reset();
            gp.axis_live.fill(0.0f);
            gp.axis_frame.fill(0.0f);
            break;
        }
        gp.axis_live[ev.axis] = v;
        break;
    }

    case platform::input_event_type::key_down:
        check_scancode(ev.scancode);
        down_live_.set(ev.scancode);
        // I4：repeat 不产生 Press（按住只算第一次）。
        if ((ev.flags & 0x01u) == 0u) {
            press_acc_.set(ev.scancode);
        }
        break;

    case platform::input_event_type::key_up:
        check_scancode(ev.scancode);
        // 【伪 up 不产生 Release】没有按下过的“松开”不构成语义上的松开；
        //   若照单全收，一次错序事件会让消费方看到“凭空松开”。
        if (down_live_.test(ev.scancode)) {
            release_acc_.set(ev.scancode);
        }
        down_live_.reset(ev.scancode);
        break;

    case platform::input_event_type::mouse_move:
        // 只要最后位置：中间位置的序列对“本帧状态”没有意义。
        mouse_pos_x_ = ev.x;
        mouse_pos_y_ = ev.y;
        break;

    case platform::input_event_type::mouse_button_down:
        check_button(ev.button);
        button_down_live_.set(button_index(ev.button));
        button_press_acc_.set(button_index(ev.button));
        break;

    case platform::input_event_type::mouse_button_up:
        check_button(ev.button);
        if (button_down_live_.test(button_index(ev.button))) {
            button_release_acc_.set(button_index(ev.button));
        }
        button_down_live_.reset(button_index(ev.button));
        break;

    case platform::input_event_type::wheel:
        // I7：本帧增量；归一化已在平台层完成。
        wheel_acc_x_ += static_cast<double>(ev.wheel_x);
        wheel_acc_y_ += static_cast<double>(ev.wheel_y);
        break;
    }
}

// =============================================================================
//  sample —— 阶段 4：冻结快照
// =============================================================================

void input::sample(const space::transform& t) {
    // ---- ① P 占帧（I2）：先算 press，再用它否决 release ----
    //   顺序不能反：若先清 press_acc 再算 release，就会把“同帧点按”算成 Release。
    press_frame_ = press_acc_;
    release_frame_ = release_acc_ & ~press_acc_;

    // ---- ② Down 取采样时刻的物理状态 ----
    //   注意：同帧点按 ⇒ down=false 且 press=true —— 这是 I2 的合法组合。
    down_frame_ = down_live_;

    // ---- ③ 鼠标按钮同理 ----
    button_press_frame_ = button_press_acc_;
    button_release_frame_ = button_release_acc_ & ~button_press_acc_;
    button_down_frame_ = button_down_live_;

    // ---- ③b 手柄：每设备冻结；断开/隔离设备保持清零 ----
    for (auto& gp : gamepads_) {
        if (!gp.connected || gp.faulted) {
            gp.down_frame.reset();
            gp.press_frame.reset();
            gp.release_frame.reset();
            gp.axis_frame.fill(0.0f);
            continue;
        }
        gp.press_frame = gp.press_acc;
        gp.release_frame = gp.release_acc & ~gp.press_acc;
        gp.down_frame = gp.down_live;
        gp.axis_frame = gp.axis_live;
        gp.press_acc.reset();
        gp.release_acc.reset();
    }

    // ---- ④ 鼠标：窗口坐标 → 逻辑坐标（DIP）----
    //   退化变换（窗口最小化，m00/m11 == 0）**不换算**：保留上一次的值。
    //   除零会得到 NaN/Inf，而那会顺着输入流进游戏逻辑 —— 比“旧值”危险得多。
    if (t.m00 != 0.0 && t.m11 != 0.0) {
        mouse_dip_x_ = t.unapply_x(static_cast<double>(mouse_pos_x_));
        mouse_dip_y_ = t.unapply_y(static_cast<double>(mouse_pos_y_));
    }

    // ---- ⑤ 滚轮：冻结本帧增量，累加器清零 ----
    wheel_frame_x_ = wheel_acc_x_;
    wheel_frame_y_ = wheel_acc_y_;
    wheel_acc_x_ = 0.0;
    wheel_acc_y_ = 0.0;

    // ---- ⑥ 清边沿累加器（down_live 不清：它是物理状态）----
    press_acc_.reset();
    release_acc_.reset();
    button_press_acc_.reset();
    button_release_acc_.reset();

    // ---- ⑦ 换出本帧事件批次（2026-11-16）----
    //   与上面几步**同一个时刻**冻结 ⇒ `on_event` 拿到的就是"本帧这一批"。
    //   用 move 而不是 swap：swap 之后 pending 里会留着上一批，还得再清一次。
    frame_events_ = std::move(pending_events_);
    pending_events_.clear();

    // ---- ⑧ 把鼠标事件的坐标换成**逻辑坐标**（与 mouse_dip 同一套逆变换）----
    //   【为什么在这里换】`input_event::x/y` 是**窗口坐标**（平台填的）；
    //     而脚本看到的一切坐标都是逻辑坐标（`35-参考系/平台给的参考系.md`）。
    //     换在冻结之后：批次已经脱离"平台事件"，与本帧的其它快照同一时刻。
    //   【退化变换不换】与 mouse_dip 同一条纪律：窗口最小化时不除零。
    if (t.m00 != 0.0 && t.m11 != 0.0) {
        for (auto& ev : frame_events_) {
            const bool is_mouse =
                ev.type == platform::input_event_type::mouse_move ||
                ev.type == platform::input_event_type::mouse_button_down ||
                ev.type == platform::input_event_type::mouse_button_up;
            if (!is_mouse) {
                continue;
            }
            ev.x = static_cast<float>(t.unapply_x(static_cast<double>(ev.x)));
            ev.y = static_cast<float>(t.unapply_y(static_cast<double>(ev.y)));
        }
    }
}

void input::clear_gamepads() noexcept {
    for (auto& gp : gamepads_) {
        gp.down_live.reset();
        gp.press_acc.reset();
        gp.release_acc.reset();
        gp.down_frame.reset();
        gp.press_frame.reset();
        gp.release_frame.reset();
        gp.axis_live.fill(0.0f);
        gp.axis_frame.fill(0.0f);
    }
}

void input::prepare_reload() noexcept {
    // 清掉所有“旧 Lua 可能已见过的瞬间状态”。
    //   down_live / down_frame / axis_live / axis_frame / 鼠标位置保留。
    press_acc_.reset();
    release_acc_.reset();
    press_frame_.reset();
    release_frame_.reset();

    button_press_acc_.reset();
    button_release_acc_.reset();
    button_press_frame_.reset();
    button_release_frame_.reset();

    wheel_acc_x_ = 0.0;
    wheel_acc_y_ = 0.0;
    wheel_frame_x_ = 0.0;
    wheel_frame_y_ = 0.0;

    for (auto& gp : gamepads_) {
        gp.press_acc.reset();
        gp.release_acc.reset();
        gp.press_frame.reset();
        gp.release_frame.reset();
    }

    pending_events_.clear();
    frame_events_.clear();
}

void input::clear_down() noexcept {
    // 【只清"按住"，不动边沿】失焦时补一次保险：SDL 可能已经合成过松开，
    //   但我们不依赖那个行为。
    down_live_.reset();
    button_down_live_.reset();
    // 本帧快照也一起清：失焦发生在帧前，本帧不该再看见"按住"。
    down_frame_.reset();
    button_down_frame_.reset();
    clear_gamepads();
}

// =============================================================================
//  查询 —— 只读本帧快照（阶段 5/6/7 的钩子）
// =============================================================================

bool input::down(std::uint16_t scancode) const {
    check_scancode(scancode);
    return down_frame_.test(scancode);
}

bool input::press(std::uint16_t scancode) const {
    check_scancode(scancode);
    return press_frame_.test(scancode);
}

bool input::release(std::uint16_t scancode) const {
    check_scancode(scancode);
    return release_frame_.test(scancode);
}

bool input::mouse_down(std::uint8_t button) const {
    check_button(button);
    return button_down_frame_.test(button_index(button));
}

bool input::mouse_press(std::uint8_t button) const {
    check_button(button);
    return button_press_frame_.test(button_index(button));
}

bool input::mouse_release(std::uint8_t button) const {
    check_button(button);
    return button_release_frame_.test(button_index(button));
}

std::vector<std::uint16_t> input::gamepad_devices() const {
    std::vector<std::uint16_t> out;
    for (std::uint16_t i = 0; i < platform::gamepad_slot_count; ++i) {
        const auto& gp = gamepads_[i];
        if (gp.connected && !gp.faulted) {
            out.push_back(i);
        }
    }
    return out;
}

bool input::gamepad_connected(std::uint16_t device) const {
    check_gamepad_device(device);
    const auto& gp = gamepads_[device];
    return gp.connected && !gp.faulted;
}

bool input::gamepad_down(std::uint16_t device, std::uint8_t button) const {
    check_gamepad_device(device);
    check_gamepad_button(button);
    const auto& gp = gamepads_[device];
    return gp.connected && !gp.faulted &&
           gp.down_frame.test(static_cast<std::size_t>(button));
}

bool input::gamepad_press(std::uint16_t device, std::uint8_t button) const {
    check_gamepad_device(device);
    check_gamepad_button(button);
    const auto& gp = gamepads_[device];
    return gp.connected && !gp.faulted &&
           gp.press_frame.test(static_cast<std::size_t>(button));
}

bool input::gamepad_release(std::uint16_t device, std::uint8_t button) const {
    check_gamepad_device(device);
    check_gamepad_button(button);
    const auto& gp = gamepads_[device];
    return gp.connected && !gp.faulted &&
           gp.release_frame.test(static_cast<std::size_t>(button));
}

double input::gamepad_axis(std::uint16_t device, std::uint8_t axis) const {
    check_gamepad_device(device);
    check_gamepad_axis(axis);
    const auto& gp = gamepads_[device];
    if (!gp.connected || gp.faulted) {
        return 0.0;
    }
    return static_cast<double>(gp.axis_frame[axis]);
}

} // namespace wag::input
