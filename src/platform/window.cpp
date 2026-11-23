// =============================================================================
//  wag/platform/window.cpp
//
//  平台层：窗口的实现。
//
//  【本文件是内核里唯一包含平台头的地方】
//      平台相关的细节（SDL3 的具体 API）全部被挡在这里。别的模块只看见
//      window.hpp 里那几个函数。
//
//  【本文件最重要的职责不是"调用 SDL3"，而是保证契约】
//      window.hpp 写下的语义（DIP 尺寸、DPI 比例、不缓存、零尺寸合法）是
//      契约；而 SDL3 的实际语义**可能与之不完全一致**。
//      因此本文件的职责是：**若平台语义与契约不符，就在这里做一次换算**，
//      使对外的语义仍然是契约所写。契约不因平台的怪癖而变。
//
//  【两套实现由 WAG_HAVE_SDL3 分开】
//      打开依赖（-DWAG_NO_EXTERNAL_DEPS=OFF）时执行真实调用；
//      关闭时每个函数退化成"记录一条并返回失败"，于是整个项目仍能编译。
// =============================================================================

#include "platform/window.hpp"

#include "log/log.hpp"

#if defined(_WIN32)
// 【为什么这里可以直接包含 windows.h】
//   因为本文件就是平台层 —— 硬约束是"Win32 细节不得外泄到别的模块"，
//   而不是"平台层自己不能用 Win32"。
//
// 【为什么要加 ifndef 守卫】
//   MinGW 的 libstdc++ **已经**在 `bits/os_defines.h` 里定义了 `NOMINMAX 1`
//   （它包含在 `<cstdint>` 那一串里，而本文件早早就包含了 window.hpp）。
//   ⇒ 不守卫就会得到 'NOMINMAX' redefined 警告。
//   而**也不能直接删掉**：那会依赖"那个头一定先被包含"，顺序一变 min/max 宏
//   就会回来与 std::min 打架。
//   `NOMINMAX` 的作用是阻止 windows.h 定义 min/max 宏。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <array>
#include <cstdio>
#include <iterator>
#include <string>

#include "platform/input_event.hpp"
#include "platform/icon_png.hpp"

#if defined(WAG_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif

namespace wag::platform {

// 把契约里的 double（DIP）收成 SDL3 要的 int。超范围时钳到边界。
namespace {

[[maybe_unused]] int to_pixels(double v) noexcept {
    constexpr double kMax = 1000000.0;
    if (!(v > 0.0)) {
        return 0;
    }
    return static_cast<int>(v < kMax ? v : kMax);
}

// 不透明句柄 → SDL3 的具体类型。
//
// window.hpp 刻意把两个句柄写成 void*（平台头不进公开头），代价就是每次调用
// SDL3 都要显式转换。void* 与 T* 之间只能用 reinterpret_cast（static_cast 不行）。
#if defined(WAG_HAVE_SDL3)
[[maybe_unused]] SDL_Window* as_window(void* h) noexcept {
    return reinterpret_cast<SDL_Window*>(h);
}

[[maybe_unused]] SDL_GLContext as_gl_context(void* h) noexcept {
    return reinterpret_cast<SDL_GLContext>(h);
}

// 图标随 exe 一起编译进二进制：不依赖安装目录里是否存在 icon.png。
[[maybe_unused]] void apply_window_icon(SDL_Window* window) noexcept {
    SDL_IOStream* io = SDL_IOFromConstMem(kIconPng, kIconPngSize);
    if (io == nullptr) {
        return;
    }
    SDL_Surface* surface = SDL_LoadPNG_IO(io, true);
    if (surface == nullptr) {
        return;
    }
    SDL_SetWindowIcon(window, surface);
    SDL_DestroySurface(surface);
}
#endif

} // namespace

// =============================================================================
//  可执行文件所在目录
// =============================================================================

std::string executable_dir() {
#if defined(_WIN32)
    // 【为什么用 W 版并显式转 UTF-8】
    //   路径可能含非 ANSI 字符（中文目录、用户名）。A 版会按当前代码页转换,
    //   于是"同一个路径"在不同机器上可能得不到同一个字符串 —— 而我们要用它
    //   去拼脚本路径，那个字符串会被 Lua 的 require 用到。
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }

    // 去掉文件名，只留目录（不含末尾分隔符）。
    std::wstring path(buf, n);
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    path.resize(slash);

    // 宽 → UTF-8。
    const int need = WideCharToMultiByte(CP_UTF8, 0, path.c_str(),
                                         static_cast<int>(path.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (need <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), out.data(),
                        need, nullptr, nullptr);
    return out;
#else
    // 【待定】其它平台怎么取。**尚未决策** —— 而"平台无关性方案"本身也待定
    //   （见 window.hpp 里的候选甲/乙）。这里返回空串 ⇒ 调用方报"必需件缺失"。
    return {};
#endif
}

// -----------------------------------------------------------------------------
//  show_message_box —— 模态消息框（只给 L4 用）
//
//  【它是"尽力而为"】弹不出来不报错：日志已经落盘，那是更强的保证。
//  【为什么在这里而不是 log 里】log 是最底层模块，不依赖任何 UI 库；
//    它通过 log::set_dialog_handler 注册本函数（见 log.hpp）。
// -----------------------------------------------------------------------------
void show_message_box(std::string_view title, std::string_view text) noexcept {
#if defined(WAG_HAVE_SDL3)
    // 【为什么用 SDL 的】跨平台、且 SDL3 已经是依赖。
    //   parent 传 nullptr：L4 可能发生在窗口建立之前。
    //   失败（无图形会话等）⇒ 忽略，只当没弹。
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             std::string(title).c_str(),
                             std::string(text).c_str(),
                             nullptr);
#else
    (void)title;
    (void)text;
#endif
}

// =============================================================================
//  show_log_notice —— 非模态提示条（安装根不可写时）
// =============================================================================

#if defined(_WIN32)
namespace {

std::wstring widen_utf8(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int need = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (need <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), need);
    return out;
}

LRESULT CALLBACK log_notice_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        wchar_t text[2048]{};
        GetWindowTextW(hwnd, text, static_cast<int>(std::size(text)));
        RECT text_rc = rc;
        InflateRect(&text_rc, -16, -12);
        DrawTextW(dc, text, -1, &text_rc,
                  DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        KillTimer(hwnd, 1);
        DestroyWindow(hwnd);
        return 0;
    case WM_LBUTTONUP:
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace
#endif

void show_log_notice(std::string_view text) noexcept {
#if defined(_WIN32)
    const std::wstring wide = widen_utf8(text);
    if (wide.empty()) {
        return;
    }

    static const wchar_t* const kClass = L"LuaWAGLogNotice";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &log_notice_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = CreateSolidBrush(RGB(40, 40, 40));
        wc.lpszClassName = kClass;
        if (RegisterClassExW(&wc) == 0) {
            std::fprintf(stderr, "[platform] 日志提示条窗口类注册失败\n");
            return;
        }
        registered = true;
    }

    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int width = (screen_w > 960) ? 900 : (screen_w - 40);
    const int height = 76;
    const int x = (screen_w - width) / 2;
    const int y = 24;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClass, wide.c_str(), WS_POPUP | WS_BORDER,
        x, y, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) {
        std::fprintf(stderr, "[platform] 日志提示条创建失败\n");
        return;
    }
    SetTimer(hwnd, 1, 8000, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
#else
    (void)text;
#endif
}

// =============================================================================
//  创建与销毁
// =============================================================================

window::~window() {
    // 与 state::~state() 一致：把清理交给 destroy()。
    destroy();
}

bool window::create(double dip_w, double dip_h, const char* title) {
    if (native_handle_ != nullptr) {
        log::warn("platform", "窗口已创建，重复的 create() 被忽略");
        return false;
    }

#if defined(WAG_HAVE_SDL3)
    // 图形属性必须在创建窗口之前设置 —— 用 OpenGL 时上下文类型与版本要作为
    // 创建属性给出，而不是创建之后另设。这是"上下文由窗口创建并交出"能成立
    // 的原因：唯一能创建它的位置就是这一刻。
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    native_handle_ = SDL_CreateWindow(title,
                                      to_pixels(dip_w),
                                      to_pixels(dip_h),
                                      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (native_handle_ == nullptr) {
        log::warn("platform", "平台窗口创建失败");
        return false;
    }

    apply_window_icon(as_window(native_handle_));

    ctx_.handle = SDL_GL_CreateContext(as_window(native_handle_));
    if (ctx_.handle == nullptr) {
        SDL_DestroyWindow(as_window(native_handle_));
        native_handle_ = nullptr;
        log::warn("platform", "图形上下文创建失败");
        return false;
    }

    // 垂直同步默认关闭。开启的入口属于配置体系（尚未收口）。
    SDL_GL_SetSwapInterval(0);

    log::record("platform", "窗口已创建");
    return true;
#else
    (void)dip_w;
    (void)dip_h;
    (void)title;
    log::record("platform", "窗口创建流程（未接 SDL3）");
    return false;
#endif
}

bool window::set_size(double w, double h) {
    if (native_handle_ == nullptr) {
        return false;
    }

#if defined(WAG_HAVE_SDL3)
    const int want_w = to_pixels(w);
    const int want_h = to_pixels(h);

    if (!SDL_SetWindowSize(as_window(native_handle_), want_w, want_h)) {
        return false;
    }

    // 【"接受"的判据不是返回值，而是**读回来的值**】
    //   SDL_SetWindowSize 的返回值只表示"这次调用没有失败"，它**不表示**
    //   平台真的采纳了那个尺寸（窗口最大化、屏幕放不下、系统约束都可能让它
    //   落到别的值上）。
    //   ⇒ 所以这里立刻读一次，与请求值比较；不等就如实报 false，由调用方记 L2。
    //
    //   【为什么这里立刻读是安全的（不会自己骗自己）】
    //     因为我们不缓存：get_dip_size / get_pixel_size 每次都重新向平台取。
    //     若平台是"延迟采纳"，那这里读到的就是旧值 ⇒ 大概率与请求不等 ⇒
    //     报一条 L2（提示"你的请求没被满足"）—— 这比"静默地看它一眼就当成
    //     成功"要诚实。而"到底延迟多久"由引擎在下一帧读到真实尺寸后自然纠正。
    int got_w = 0;
    int got_h = 0;
    SDL_GetWindowSize(as_window(native_handle_), &got_w, &got_h);

    // 【为什么允许 1 像素的差】有些平台在换算单位时会取整（例如把请求值
    //   按显示缩放来回换算一次）。差 1 像素不是"没被满足"，而"差很多"才是。
    const int dw = (got_w > want_w) ? (got_w - want_w) : (want_w - got_w);
    const int dh = (got_h > want_h) ? (got_h - want_h) : (want_h - got_h);
    return dw <= 1 && dh <= 1;
#else
    (void)w;
    (void)h;
    return false;
#endif
}

namespace { void gamepad_shutdown() noexcept; }

void window::destroy() {
#if defined(WAG_HAVE_SDL3)
    gamepad_shutdown();
#endif
    if (native_handle_ == nullptr && !ctx_.is_valid()) {
        return;
    }

    // 顺序：先上下文，后窗口 —— 上下文依附于窗口。
    // 若上下文已经被交出（ctx_ 为空），销毁它的责任在渲染层，本函数不能碰它。
#if defined(WAG_HAVE_SDL3)
    if (ctx_.is_valid()) {
        SDL_GL_DestroyContext(as_gl_context(ctx_.handle));
    }
    if (native_handle_ != nullptr) {
        SDL_DestroyWindow(as_window(native_handle_));
    }
#endif

    native_handle_ = nullptr;
    ctx_ = gl_context{};
    pending_events_ = event_bits::none;
}

bool window::is_created() const noexcept {
    return native_handle_ != nullptr;
}

// =============================================================================
//  图形上下文的交出（形态乙）
// =============================================================================

std::optional<gl_context> window::take_gl_context() {
    if (!ctx_.is_valid()) {
        // 重复交出是编程错误，但不是崩溃理由：返回 nullopt 让调用方报出
        // 一条清楚的错误，而不是拿到一个悬空句柄。
        log::warn("platform", "图形上下文已经被交出过（或尚未创建）");
        return std::nullopt;
    }

    // 交出 = 复制一份值 + 清空自己。清空这一步是"窗口不再引用上下文"这条
    // 不变量的代码保证，而不是纪律。
    gl_context out = ctx_;
    ctx_ = gl_context{};
    return out;
}

// =============================================================================
//  每帧的动作
// =============================================================================

void window::present() {
#if defined(WAG_HAVE_SDL3)
    // vsync 默认关闭，因此本调用不阻塞。
    if (native_handle_ != nullptr) {
        SDL_GL_SwapWindow(as_window(native_handle_));
    }
#endif
}

#if defined(WAG_HAVE_SDL3)
namespace {

// =============================================================================
//  gamepad 设备管理（2026-11-19，V1）
// =============================================================================

struct gamepad_slot {
    SDL_Gamepad* handle = nullptr;
    SDL_JoystickID joystick_id = 0;
    std::uint16_t generation = 0;
};

std::array<gamepad_slot, gamepad_slot_count> g_gamepad_slots{};
input_sink* g_gamepad_sink = nullptr;
bool g_gamepad_focused = true;

void gamepad_close_slot(gamepad_slot& slot) noexcept {
    if (slot.handle != nullptr) {
        SDL_CloseGamepad(slot.handle);
        slot.handle = nullptr;
    }
    slot.joystick_id = 0;
}

gamepad_slot* gamepad_find_by_id(SDL_JoystickID id) noexcept {
    for (auto& slot : g_gamepad_slots) {
        if (slot.handle != nullptr && slot.joystick_id == id) {
            return &slot;
        }
    }
    return nullptr;
}

gamepad_slot* gamepad_free_slot() noexcept {
    for (auto& slot : g_gamepad_slots) {
        if (slot.handle == nullptr) {
            return &slot;
        }
    }
    return nullptr;
}

std::uint16_t gamepad_slot_index(const gamepad_slot* slot) noexcept {
    return static_cast<std::uint16_t>(slot - g_gamepad_slots.data());
}

void gamepad_feed(const input_event& ev) noexcept {
    if (g_gamepad_sink != nullptr) {
        g_gamepad_sink->feed(ev);
    }
}

float gamepad_axis_norm(std::uint8_t axis, Sint16 raw) noexcept {
    float v = static_cast<float>(raw) / 32767.0f;
    if (axis >= 4) { // triggers: [0,1]
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
    } else { // sticks: [-1,1]
        if (v < -1.0f) v = -1.0f;
        if (v > 1.0f) v = 1.0f;
    }
    return v;
}

void gamepad_resync_one(gamepad_slot& slot) noexcept {
    if (slot.handle == nullptr || g_gamepad_sink == nullptr) {
        return;
    }

    const std::uint16_t device = gamepad_slot_index(&slot);
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
        input_event ev{};
        ev.type = SDL_GetGamepadButton(slot.handle,
                                       static_cast<SDL_GamepadButton>(b)) != 0
                      ? input_event_type::gamepad_button_down
                      : input_event_type::gamepad_button_up;
        ev.device = device;
        ev.generation = slot.generation;
        ev.button = static_cast<std::uint8_t>(b);
        ev.flags = input_flag_resync;
        gamepad_feed(ev);
    }
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
        input_event ev{};
        ev.type = input_event_type::gamepad_axis;
        ev.device = device;
        ev.generation = slot.generation;
        ev.axis = static_cast<std::uint8_t>(a);
        ev.value = gamepad_axis_norm(
            static_cast<std::uint8_t>(a),
            SDL_GetGamepadAxis(slot.handle, static_cast<SDL_GamepadAxis>(a)));
        ev.flags = input_flag_resync;
        gamepad_feed(ev);
    }
}

void gamepad_resync_all() noexcept {
    for (auto& slot : g_gamepad_slots) {
        gamepad_resync_one(slot);
    }
}

void gamepad_set_focus(bool focused) noexcept {
    if (g_gamepad_focused == focused) {
        return;
    }
    g_gamepad_focused = focused;

    if (!focused) {
        // 内部事件：input 清手柄状态，不进入 on_event。
        input_event ev{};
        ev.type = input_event_type::gamepad_focus_lost;
        gamepad_feed(ev);
        return;
    }

    // 得焦：把当前物理状态重新灌进 input；resync 标志让 input 不补 press。
    gamepad_resync_all();
}

void gamepad_open(SDL_JoystickID id) noexcept {
    if (gamepad_find_by_id(id) != nullptr) {
        return;
    }
    gamepad_slot* const slot = gamepad_free_slot();
    if (slot == nullptr) {
        log::warn("input", "手柄数量超过 8，忽略新设备");
        return;
    }

    SDL_Gamepad* const handle = SDL_OpenGamepad(id);
    if (handle == nullptr) {
        log::warn("input", log::format("手柄打开失败", SDL_GetError()));
        return;
    }

    slot->handle = handle;
    slot->joystick_id = id;
    ++slot->generation;
    if (slot->generation == 0) {
        slot->generation = 1;
    }

    input_event ev{};
    ev.type = input_event_type::gamepad_added;
    ev.device = gamepad_slot_index(slot);
    ev.generation = slot->generation;
    gamepad_feed(ev);

    if (g_gamepad_focused) {
        gamepad_resync_one(*slot);
    }
}

void gamepad_remove(SDL_JoystickID id) noexcept {
    gamepad_slot* const slot = gamepad_find_by_id(id);
    if (slot == nullptr) {
        return;
    }

    input_event ev{};
    ev.type = input_event_type::gamepad_removed;
    ev.device = gamepad_slot_index(slot);
    ev.generation = slot->generation;
    gamepad_feed(ev);

    gamepad_close_slot(*slot);
}

void gamepad_init(input_sink* sink) noexcept {
    if (g_gamepad_sink != nullptr) {
        return;
    }
    g_gamepad_sink = sink;
    g_gamepad_focused = true;

    int count = 0;
    SDL_JoystickID* const ids = SDL_GetGamepads(&count);
    if (ids != nullptr) {
        for (int i = 0; i < count; ++i) {
            gamepad_open(ids[i]);
        }
        SDL_free(ids);
    }
}

void gamepad_shutdown() noexcept {
    for (auto& slot : g_gamepad_slots) {
        gamepad_close_slot(slot);
    }
    g_gamepad_sink = nullptr;
}

bool gamepad_handle_event(const SDL_Event& e) noexcept {
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        gamepad_open(e.gdevice.which);
        return true;

    case SDL_EVENT_GAMEPAD_REMOVED:
        gamepad_remove(e.gdevice.which);
        return true;

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        if (!g_gamepad_focused) {
            return true;
        }
        gamepad_slot* const slot = gamepad_find_by_id(e.gbutton.which);
        if (slot == nullptr) {
            return true;
        }
        input_event ev{};
        ev.type = (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                      ? input_event_type::gamepad_button_down
                      : input_event_type::gamepad_button_up;
        ev.device = gamepad_slot_index(slot);
        ev.generation = slot->generation;
        ev.button = static_cast<std::uint8_t>(e.gbutton.button);
        gamepad_feed(ev);
        return true;
    }

    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!g_gamepad_focused) {
            return true;
        }
        gamepad_slot* const slot = gamepad_find_by_id(e.gaxis.which);
        if (slot == nullptr) {
            return true;
        }
        input_event ev{};
        ev.type = input_event_type::gamepad_axis;
        ev.device = gamepad_slot_index(slot);
        ev.generation = slot->generation;
        ev.axis = static_cast<std::uint8_t>(e.gaxis.axis);
        ev.value = gamepad_axis_norm(ev.axis, e.gaxis.value);
        gamepad_feed(ev);
        return true;
    }

    default:
        return false;
    }
}

/// SDL 输入事件 → 引擎 POD。返回 false 表示"不是输入事件"。
///
/// 【它不做的两件事】
///   · 不判断"要不要"：repeat 过滤是输入层的语义（I4），这里只把标志带下去；
///   · 不调脚本：与 note_event 同一条纪律（消息路径只写数据）。
///
/// 【坐标单位】SDL3 的鼠标事件坐标就是**窗口坐标**（DIP）；
///   而 space::transform 的目标域也是窗口坐标（喂给 recompute 的是 DIP 尺寸）
///   ⇒ 两边同单位。**不要**在平台层再乘 DPI，否则高 DPI 下会偏一倍。
[[nodiscard]] bool translate_input_event(const SDL_Event& e,
                                         input_event& out) noexcept {
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        out.type = (e.type == SDL_EVENT_KEY_DOWN)
                       ? input_event_type::key_down
                       : input_event_type::key_up;
        out.scancode = static_cast<std::uint16_t>(e.key.scancode);
        out.flags = e.key.repeat ? 1u : 0u;
        return true;

    case SDL_EVENT_MOUSE_MOTION:
        out.type = input_event_type::mouse_move;
        out.x = e.motion.x;
        out.y = e.motion.y;
        return true;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        out.type = (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                       ? input_event_type::mouse_button_down
                       : input_event_type::mouse_button_up;
        out.button = e.button.button; // 1..5（SDL 的 LEFT/MIDDLE/RIGHT/X1/X2）
        return true;

    case SDL_EVENT_MOUSE_WHEEL: {
        out.type = input_event_type::wheel;
        float dx = e.wheel.x;
        float dy = e.wheel.y;
        // 归一化：+x 右 / +y 上（I7）。
        // 系统"自然滚动"时 SDL 报 FLIPPED ⇒ 取反，让游戏看到的约定与机器设置无关。
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            dx = -dx;
            dy = -dy;
        }
        out.wheel_x = dx;
        out.wheel_y = dy;
        return true;
    }

    default:
        return false;
    }
}

/// 一个 SDL 事件 → 事件位的映射。
///
/// 【为什么抽成函数】因为"泵一次队列"与"等消息时收到的事件"都必须用**同一套**
///   映射。写两遍会让它们悄悄分歧 —— 而分歧的形态是"某个事件在一条路径上被
///   忽略"，那很难被发现。
///
/// 【它绝不调用任何脚本】只写位。理由见 window::pump_events 的硬约束。
///
/// 【为什么还带一个 `focused` 出参 —— 这是"事件 vs 状态"的区分】
///   `bits` 记的是**本帧发生过什么**（事件，每帧被取走清零）；
///   而焦点是一个**状态**：失焦之后的每一帧都仍然失焦，但只有那一帧有事件。
///   ⇒ 若消费方读 bits 来推状态，失焦的下一帧就会"恢复"
///     （症状：暂停闪一下）。
///   ⇒ 所以焦点状态单独记在 `window::focused_` 上，由本函数顺手更新。
void note_event(event_bits& bits, bool& focused, input_sink* sink,
                const SDL_Event& e) noexcept {
    // ---- gamepad 事件：先交给设备管理 ----
    if (gamepad_handle_event(e)) {
        return;
    }

    // ---- 输入事件：先喂给 sink ----
    //   【为什么在 switch 之前】要先把"原始事件"完整地带给输入层；
    //   下面的 switch 只负责写窗口事件位（它不关心按了什么键）。
    //   【它仍然只写数据】sink->feed 不调脚本（见 input::feed 的注释）。
    if (sink != nullptr) {
        input_event ie{};
        if (translate_input_event(e, ie)) {
            sink->feed(ie);
        }
    }

    switch (e.type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
        bits |= event_bits::resized;
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        bits |= event_bits::focus_gained;
        focused = true;
        gamepad_set_focus(true);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        bits |= event_bits::focus_lost;
        focused = false;
        gamepad_set_focus(false);
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        // 不是直接退出，而是请求退出：与实际退出之间隔着帧末的状态应用
        // 与随后的关闭序列。
        bits |= event_bits::close_requested;
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        // 【2026-11-16：这里不再写事件位】
        //   键/鼠数据由输入采样统一提供（上面的 sink->feed 已经把事件交给输入层）；
        //   原来那个 `input_dirty` 位从未被读过，已删。
        break;
    default:
        break;
    }
}

} // namespace
#endif

void window::pump_events() {
    // 硬约束：本函数绝不调用任何脚本。它只把"发生了什么事"记成位。
    // 窗口消息路径可以在任意时刻被系统调用；若它直接回调脚本，脚本就会在
    // 任意时刻重入引擎状态。
#if defined(WAG_HAVE_SDL3)
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        note_event(pending_events_, focused_, input_sink_, e);
    }
#endif
    // DPI 变化不需要设位：get_dpi_scale() 每次调用都会重新取。
}

void window::wait_events_or_timeout(int timeout_ms) {
    // 【它为什么替代 sleep】
    //   帧率控制的"等"占了一帧里的大部分时间。用 sleep 过去的话，那段时间
    //   里一条窗口消息都不被处理 ⇒ 系统把窗口判成"未响应"（加载圈光标、
    //   拖不动边框）。改成"等消息或超时"之后，消息在整帧的空闲时间里都被
    //   处理，而等待的总时长不变 ⇒ 帧率不受影响。
    //
    // 【为什么不需要额外的忙碌循环】SDL_WaitEventTimeout 在超时或收到一个
    //   事件后返回。收到事件时我们把它记成位（用同一个映射），然后**继续等**
    //   到时刻用尽 —— 否则"事件多的时候帧率会变成满速"。
#if defined(WAG_HAVE_SDL3)
    if (timeout_ms <= 0) {
        // 不等待：只抽一次队列。这与 pump_events 同义，但走同一条映射。
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            note_event(pending_events_, focused_, input_sink_, e);
        }
        return;
    }

    const Uint64 deadline = SDL_GetTicks() + static_cast<Uint64>(timeout_ms);
    for (;;) {
        const Uint64 now = SDL_GetTicks();
        if (now >= deadline) {
            break;
        }
        const int remain = static_cast<int>(deadline - now);
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, remain)) {
            // 超时（没有事件）⇒ 等到了，收工。
            break;
        }
        note_event(pending_events_, focused_, input_sink_, e);
    }
#else
    // 【这一支**必须存在** —— 它修的是一个真实的警告，不是装饰】
    //   本函数的**整个函数体**在上面那个 `#if` 里。于是无依赖档下函数体消失、
    //   只剩一个空壳 ⇒ 形参 `timeout_ms` **没有人用** ⇒ `-Wunused-parameter` 报警。
    //   ⇒ 而"两种构建都不出警告"是本项目写下来的纪律（见 `begin_frame` 的
    //     `#else` 分支里那条注释）：**警告一多，真问题就会被淹掉**。
    //   ⇒ ⚠️ 同文件里 `begin_frame` 早就有这一支，而本函数**漏了** ——
    //     也就是说这条警告与语法无关，纯属"同类不一致"。
    (void)timeout_ms;
#endif
}

// =============================================================================
//  输入接线
// =============================================================================

void window::set_input_sink(input_sink* sink) noexcept {
    input_sink_ = sink;

#if defined(WAG_HAVE_SDL3)
    if (sink == nullptr) {
        gamepad_shutdown();
        return;
    }

    // 【播种鼠标初始位置】见 window.hpp 的说明。
    //   · 有焦点：SDL_GetMouseState 返回**相对聚焦窗口**的窗口坐标（DIP），
    //     与 input 的期望同单位，直接喂；
    //   · 无焦点：喂 (0,0)。不读 —— 否则读到的是别的窗口的坐标，
    //     比"未初始化"更糟（会让菜单以为鼠标在某个位置）。
    if (native_handle_ == nullptr) {
        return;
    }
    input_event ev{};
    ev.type = input_event_type::mouse_move;
    if (focused_) {
        float x = 0.0f;
        float y = 0.0f;
        SDL_GetMouseState(&x, &y);
        ev.x = x;
        ev.y = y;
    }
    sink->feed(ev);

    // gamepad 设备：打开已连接设备并把 added 事件喂给 input。
    gamepad_init(sink);
#endif
}

// =============================================================================
//  两个查询
// =============================================================================

dip_size window::get_dip_size() const {
    if (native_handle_ == nullptr) {
        // 零尺寸是一个**合法**的值（帧结构据此判定休眠），因此不需要特判。
        return dip_size{0.0, 0.0};
    }

#if defined(WAG_HAVE_SDL3)
    int w = 0;
    int h = 0;
    // SDL_GetWindowSize 返回 bool（失败时 w/h 保持原值）。
    if (!SDL_GetWindowSize(as_window(native_handle_), &w, &h)) {
        return dip_size{0.0, 0.0};
    }
    return dip_size{static_cast<double>(w), static_cast<double>(h)};
#else
    return dip_size{0.0, 0.0};
#endif
}

dip_size window::get_pixel_size() const {
    if (native_handle_ == nullptr) {
        return dip_size{0.0, 0.0};
    }

#if defined(WAG_HAVE_SDL3)
    // 【为什么用 SDL_GetWindowSizeInPixels 而不是 dip × dpi】
    //   ① `dip × dpi` 是一个**推导值**，它与平台报的真值之间会有取整余量
    //      ⇒ 按它算居中的结果是**偏的**（这一条与"禁止用乘积互相推导"同源，
    //        见 window.hpp 头部）。
    //   ② 这个函数直接给"平台认为的绘制表面有多大"。
    //
    //   ⚠️ **而它到底是不是"物理像素"，取决于窗口是否启用了高像素密度**
    //      （`SDL_WINDOW_HIGH_PIXEL_DENSITY`）。实测记录：
    //          引擎窗口上 dip = 像素 = 1920×1080，而 dpi_scale = 1.500
    //      ⇒ 在**未启用**高像素密度时，两者按**约定**相等（绘制表面就是逻辑分辨率），
    //        所以"相等"不是函数出错。
    //      ⇒ 而本函数**只负责上报平台给的数**，不负责解释它的语义 ——
    //        "它是什么单位"由窗口是否启用高像素密度决定。
    int w = 0;
    int h = 0;
    if (!SDL_GetWindowSizeInPixels(as_window(native_handle_), &w, &h)) {
        return dip_size{0.0, 0.0};
    }
    return dip_size{static_cast<double>(w), static_cast<double>(h)};
#else
    return dip_size{0.0, 0.0};
#endif
}

double window::get_dpi_scale() const {
    if (native_handle_ == nullptr) {
        // 1.0 是"不支持 DPI 缩放"的按定义取值。
        return 1.0;
    }

#if defined(WAG_HAVE_SDL3)
    // 用窗口级的比例：窗口可能跨显示器，只有"当前所在那块屏"才是相关的。
    const float s = SDL_GetWindowDisplayScale(as_window(native_handle_));
    // 契约规定返回正实数。平台返回 0 或负数时兜成 1.0 —— 否则除以它会产生
    // 无穷大，而无穷大会顺着顶点数据流进 GPU。
    return (s > 0.0f) ? static_cast<double>(s) : 1.0;
#else
    return 1.0;
#endif
}

// -----------------------------------------------------------------------------
//  get_frame_geometry —— 三个量一次读
//
//  【为什么要有这个函数（而不是让调用方按需调三个 getter）】
//      详见 window.hpp 里 `frame_geometry` 的说明。一句话：
//      把"禁止用乘积互相推导"这句**约定**变成"**难以混用**"的形态 ——
//      三个量绑成一个值，只能一起读、一起传；而想再乘一下就必须先挑出一个分量，
//      那一步在审查里看得见。
//
//  【它不缓存】三个 getter 各自都是"每次向平台取"，这里只是把它们并成一次调用。
//      所以它每帧调一次的成本 = 三次读字段（几十纳秒级）。
//
//  【读的顺序无关紧要】但**必须在同一帧内读** —— 这正是本函数存在的第二个理由:
//      分别调的时候，调用方可能在第 1 处读了 dip、在第 7 处读了 pixel，
//      而窗口恰好在这两点之间变了 ⇒ **一帧内两个瞬间的尺寸**。
//      合成一个值之后，"同一次读取"成为默认形态。
// -----------------------------------------------------------------------------
window::frame_geometry window::get_frame_geometry() const {
    frame_geometry g;
    g.dip = get_dip_size();
    g.pixel = get_pixel_size();
    g.scale = get_dpi_scale();
    return g;
}

// -----------------------------------------------------------------------------
//  geometry_source_convention —— 把"唯一来源"这条约定变成一个**可被调用**的东西
//
//  【为什么一个返回字符串的函数值得存在】
//      因为接口说明里的话没人会在写代码时去翻。而在平台层里放一个**有名字的入口**，
//      意味着：
//        · 读平台层的人会看到它（它就在 getter 旁边）
//        · 将来若要加断言（例如"不得在别处读 Win32 的客户区矩形"），
//          有一处天然的落点
//      ⇒ 它返回的是**约定本身的原文摘要**，不是数据。调用它没有任何副作用。
//      ⚠️ 它不做任何检查 —— 检查 Win32 调用需要代码扫描，那不属于运行时。
// -----------------------------------------------------------------------------
const char* window::geometry_source_convention() noexcept {
    return "窗口几何一律走 SDL 的尺寸 API；输入坐标一律走 SDL 事件。"
           "不得改用 Win32 的 GetClientRect / GetCursorPos（它们只用于交叉验证）。";
}

// =============================================================================
//  事件位
// =============================================================================

event_bits window::consume_events() {
    // 读取即清空：这些位表达的是"自上次消费以来发生过什么"。
    const event_bits out = pending_events_;
    pending_events_ = event_bits::none;
    return out;
}

} // namespace wag::platform
