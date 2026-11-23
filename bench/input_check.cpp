// =============================================================================
//  bench/input_check.cpp
//
//  输入状态机的验证程序（不依赖 SDL / 图形 / 脚本；只链接
//  wag_input + wag_error + wag_log）。
//
//  【它验证什么】《LuaWAG 输入子系统设计》I1–I14 的语义：
//    ① Down/Press/Release 三态
//    ② P 占帧：同帧点按 ⇒ Press=true、Release=false、Down=false
//    ③ 先松后按：Press=true、Release=false
//    ④ 重复事件不产生 Press
//    ⑤ 多键同帧：A Press + B Release 同时成立
//    ⑥ 鼠标：按钮三态、位置 DIP 换算、退化变换保留旧值、滚轮本帧增量
//
//  【怎么跑】先构建（release-with-deps），再手工编译：
//      g++ -O2 -std=c++20 -I src bench/input_check.cpp -o bench/input_check.exe \
//          -Wl,--start-group \
//          build/release-with-deps/lib/libwag_input.a \
//          build/release-with-deps/lib/libwag_error.a \
//          build/release-with-deps/lib/libwag_log.a \
//          -Wl,--end-group
//    然后 `bench/input_check.exe`（返回码 0 = 全过）。
// =============================================================================

#include "input/input.hpp"
#include "platform/input_event.hpp"
#include "space/rect.hpp"

#include <cmath>
#include <cstdio>

namespace {

int g_fail = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::printf("FAIL  line %d  %s\n", line, what);
        ++g_fail;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

using wag::platform::input_event;
using wag::platform::input_event_type;

input_event key(std::uint16_t scancode, bool down, bool repeat = false) {
    input_event e;
    e.type = down ? input_event_type::key_down : input_event_type::key_up;
    e.scancode = scancode;
    e.flags = repeat ? 1u : 0u;
    return e;
}

input_event mouse_move(float x, float y) {
    input_event e;
    e.type = input_event_type::mouse_move;
    e.x = x;
    e.y = y;
    return e;
}

input_event mouse_button(std::uint8_t button, bool down) {
    input_event e;
    e.type = down ? input_event_type::mouse_button_down
                  : input_event_type::mouse_button_up;
    e.button = button;
    return e;
}

input_event wheel(float dx, float dy) {
    input_event e;
    e.type = input_event_type::wheel;
    e.wheel_x = dx;
    e.wheel_y = dy;
    return e;
}

// 恒等变换：窗口坐标 == 逻辑坐标。
wag::space::transform identity() {
    return wag::space::transform{};
}

// 一个含缩放、y 翻转、平移的变换（模拟 letterbox 的常见形态）：
//   窗口 x = 2 * 逻辑 x + 10
//   窗口 y = -2 * 逻辑 y + 100
wag::space::transform scaled() {
    wag::space::transform t;
    t.m00 = 2.0;
    t.m11 = -2.0;
    t.m02 = 10.0;
    t.m12 = 100.0;
    return t;
}

void test_key_states() {
    wag::input::input in;
    const auto t = identity();
    constexpr std::uint16_t k = 4; // SDL_SCANCODE_A

    // ① 按下首帧：Down + Press
    in.feed(key(k, true));
    in.sample(t);
    CHECK(in.down(k));
    CHECK(in.press(k));
    CHECK(!in.release(k));

    // 按住后续帧：Down，不再 Press
    in.sample(t);
    CHECK(in.down(k));
    CHECK(!in.press(k));
    CHECK(!in.release(k));

    // 正常松开：Release
    in.feed(key(k, false));
    in.sample(t);
    CHECK(!in.down(k));
    CHECK(!in.press(k));
    CHECK(in.release(k));
}

void test_tap_p_occupies_frame() {
    wag::input::input in;
    const auto t = identity();
    constexpr std::uint16_t k = 5;

    // ② 同帧点按：Down=false、Press=true、Release=false（P 占帧）
    in.feed(key(k, true));
    in.feed(key(k, false));
    in.sample(t);
    CHECK(!in.down(k));
    CHECK(in.press(k));
    CHECK(!in.release(k));
}

void test_release_then_press() {
    wag::input::input in;
    const auto t = identity();
    constexpr std::uint16_t k = 6;

    in.feed(key(k, true));
    in.sample(t);

    // ③ 先松后按（同一帧，结束按住）：Press 占帧，Release 被吞
    in.feed(key(k, false));
    in.feed(key(k, true));
    in.sample(t);
    CHECK(in.down(k));
    CHECK(in.press(k));
    CHECK(!in.release(k));
}

void test_repeat() {
    wag::input::input in;
    const auto t = identity();
    constexpr std::uint16_t k = 7;

    in.feed(key(k, true));
    in.sample(t);
    CHECK(in.press(k));

    // ④ 重复事件：Down 保持，不产生 Press
    in.feed(key(k, true, /*repeat=*/true));
    in.sample(t);
    CHECK(in.down(k));
    CHECK(!in.press(k));
    CHECK(!in.release(k));
}

void test_multi_key() {
    wag::input::input in;
    const auto t = identity();
    constexpr std::uint16_t a = 8;
    constexpr std::uint16_t b = 9;

    in.feed(key(a, true));
    in.feed(key(b, true));
    in.sample(t);
    CHECK(in.down(a));
    CHECK(in.down(b));

    // ⑤ 不同键同帧：A 松开、B 保持
    in.feed(key(a, false));
    in.sample(t);
    CHECK(!in.down(a));
    CHECK(in.release(a));
    CHECK(!in.press(a));
    CHECK(in.down(b));
    CHECK(!in.release(b));
}

void test_mouse_buttons() {
    wag::input::input in;
    const auto t = identity();

    in.feed(mouse_button(1, true));
    in.sample(t);
    CHECK(in.mouse_down(1));
    CHECK(in.mouse_press(1));
    CHECK(!in.mouse_release(1));

    in.feed(mouse_button(1, false));
    in.sample(t);
    CHECK(!in.mouse_down(1));
    CHECK(!in.mouse_press(1));
    CHECK(in.mouse_release(1));

    // 右键同帧点按：P 占帧
    in.feed(mouse_button(3, true));
    in.feed(mouse_button(3, false));
    in.sample(t);
    CHECK(!in.mouse_down(3));
    CHECK(in.mouse_press(3));
    CHECK(!in.mouse_release(3));
}

void test_mouse_dip() {
    wag::input::input in;
    const auto t = scaled();

    // ⑥ 窗口 (210, 0) → 逻辑 (100, 50)
    in.feed(mouse_move(210.0f, 0.0f));
    in.sample(t);
    CHECK(std::fabs(in.mouse_x() - 100.0) < 1e-9);
    CHECK(std::fabs(in.mouse_y() - 50.0) < 1e-9);

    // 没有新事件时，用**同一个窗口位置**按新变换重算：
    //   这一步验证"位置存的是窗口坐标，DIP 是每帧算的"。
    wag::space::transform t2;
    t2.m00 = 1.0;
    t2.m11 = 1.0;
    in.sample(t2);
    CHECK(std::fabs(in.mouse_x() - 210.0) < 1e-9);
    CHECK(std::fabs(in.mouse_y() - 0.0) < 1e-9);
}

void test_degenerate_transform_keeps_last() {
    wag::input::input in;
    const auto t = scaled();

    in.feed(mouse_move(210.0f, 0.0f));
    in.sample(t);
    const double x = in.mouse_x();
    const double y = in.mouse_y();

    // 退化变换（窗口最小化）：m00 = 0 ⇒ 不换算、保留旧值，不产生 NaN
    wag::space::transform degenerate;
    degenerate.m00 = 0.0;
    degenerate.m11 = 0.0;
    in.sample(degenerate);
    CHECK(in.mouse_x() == x);
    CHECK(in.mouse_y() == y);
    CHECK(std::isfinite(in.mouse_x()));
    CHECK(std::isfinite(in.mouse_y()));
}

void test_wheel() {
    wag::input::input in;
    const auto t = identity();

    // ⑦ 滚轮：本帧累加
    in.feed(wheel(1.0f, 2.0f));
    in.feed(wheel(3.0f, 4.0f));
    in.sample(t);
    CHECK(std::fabs(in.wheel_x() - 4.0) < 1e-9);
    CHECK(std::fabs(in.wheel_y() - 6.0) < 1e-9);

    // 采样后清零
    in.sample(t);
    CHECK(in.wheel_x() == 0.0);
    CHECK(in.wheel_y() == 0.0);
}

void test_focus_like_synthetic_up() {
    wag::input::input in;
    const auto t = identity();
    constexpr std::uint16_t k = 10;

    // 失焦时 SDL 会抬起按键 ⇒ 输入侧看到的是普通 KEY_UP ⇒ Release。
    // 输入模块**不需要知道焦点**（E.2）：这里只是验证"普通 up 的语义"。
    in.feed(key(k, true));
    in.sample(t);
    in.feed(key(k, false));
    in.sample(t);
    CHECK(!in.down(k));
    CHECK(in.release(k));
}

} // namespace

int main() {
    test_key_states();
    test_tap_p_occupies_frame();
    test_release_then_press();
    test_repeat();
    test_multi_key();
    test_mouse_buttons();
    test_mouse_dip();
    test_degenerate_transform_keeps_last();
    test_wheel();
    test_focus_like_synthetic_up();

    if (g_fail == 0) {
        std::printf("input_check: all passed\n");
        return 0;
    }
    std::printf("input_check: %d failure(s)\n", g_fail);
    return 1;
}
