// =============================================================================
// wag/script/hook.cpp
//
// 钩子协议的名字表与调用点。
//
// 【本文件的职责】
//
// 已实现：
// · `kHookNames`：13 个协议名的**唯一来源**（个数由 static_assert 钉住）
// · `hook_name` / `all_hook_names`
// · `make_frame_info`：dtg（1/60，锁死）、循环帧号、焦点与事件批次
// · `call_hook`：取 `wag.hook[name]` → 压实参表 → `lua_pcall`（1 参 0 返回）
// · 槽不是函数 ⇒ `fail_internal`（L3：白名单与表不一致 = 引擎完整性损坏）
// · 脚本抛错 ⇒ `fail_script`（L2：记日志 + 通知，帧结构继续）
// · 栈纪律：压 4 弹 2（`lua_pcall` 自己弹函数与实参）。
//
// 事件与焦点由输入采样/帧结构写入 `frame_info`：
//   `event` 是本帧事件数组，没有事件时为 nil；
//   `has_focus` 是本帧开始时窗口是否有焦点。
// =============================================================================

#include "script/hook.hpp"
#include "platform/paths.hpp"

#include "error/error.hpp"
#include "log/log.hpp"
#include "time/frame.hpp"
#include "script/state.hpp"

// 同 state.cpp：写 luajit-2.1/ 版本目录以钉死身份（系统里还有一份 Lua 5.5 的 lua.h）。
#if defined(WAG_HAVE_LUAJIT)
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <string>
}
#endif

namespace wag::script {

// =============================================================================
// 名字表
// =============================================================================

namespace {

// -----------------------------------------------------------------------------
// 协议名 —— **全项目唯一一处**
//
// 【改动这里的后果】所有用旧名字写的脚本会失效。这不是可以"顺手改一下"的
// 地方。每个名字的语义见 hook 枚举的注释。
//
// 【顺序必须与 hook 枚举一致】下面有 static_assert 检查个数，但**顺序**
// 无法在编译期检查。因此本表与枚举必须一起改 —— 这是唯一一处需要靠
// 纪律的地方，所以它被刻意放在紧挨着枚举的地方。
// -----------------------------------------------------------------------------
constexpr const char* kHookNames[] = {
    "on_game_init",    // 0
    "on_game_exit",    // 1
    "on_frame_ext",    // 2
    "on_frame_main",   // 3
    "on_frame_pause",  // 4
    "on_render_begin", // 5
    "on_render_main",  // 6
    "on_render_pause", // 7
    "on_render_ext",   // 8
    "on_render_end",   // 9
    "on_event",        // 10
    "on_focus_gain",   // 11
    "on_focus_lose",   // 12
};

// 个数检查：若枚举里加了钩子而忘了往表里加，这里会编译失败。
static_assert(sizeof(kHookNames) / sizeof(kHookNames[0]) == hook_count,
              "kHookNames 与 hook_count 不一致：枚举与名字表必须一起改。"
              "另一方面，这个断言也说明名字表是引擎的白名单来源。");

} // namespace

#if defined(WAG_HAVE_LUAJIT)
namespace {

/// 把一帧的信息压成一张表 —— 钩子的唯一实参。
/// 只放脚本该知道的量，不放引擎内部状态（休眠标志、DPI 等）。
namespace {

/// 事件种类的协议名（**这些字符串是协议**，脚本按它们分支）。
const char* input_event_kind(platform::input_event_type t) noexcept {
    switch (t) {
    case platform::input_event_type::key_down:
        return "key_down";
    case platform::input_event_type::key_up:
        return "key_up";
    case platform::input_event_type::mouse_move:
        return "mouse_move";
    case platform::input_event_type::mouse_button_down:
        return "mouse_down";
    case platform::input_event_type::mouse_button_up:
        return "mouse_up";
    case platform::input_event_type::wheel:
        return "wheel";

    case platform::input_event_type::gamepad_added:
        return "gamepad_added";
    case platform::input_event_type::gamepad_removed:
        return "gamepad_removed";
    case platform::input_event_type::gamepad_button_down:
        return "gamepad_button_down";
    case platform::input_event_type::gamepad_button_up:
        return "gamepad_button_up";
    case platform::input_event_type::gamepad_axis:
        return "gamepad_axis";
    case platform::input_event_type::gamepad_focus_lost:
        return "gamepad_focus_lost";
    }
    return "?";
}

/// 压一个事件表（2026-11-16）：只放该类事件**有意义**的字段。
void push_one_input_event(lua_State* L, const platform::input_event& ev) {
    lua_createtable(L, 0, 4);

    lua_pushstring(L, input_event_kind(ev.type));
    lua_setfield(L, -2, "kind");

    switch (ev.type) {
    case platform::input_event_type::key_down:
    case platform::input_event_type::key_up:
        lua_pushinteger(L, static_cast<lua_Integer>(ev.scancode));
        lua_setfield(L, -2, "scancode");
 // repeat 位：脚本想区分"自动重复"与"真按下"时用得到。
        lua_pushboolean(L, (ev.flags & 0x01u) != 0u ? 1 : 0);
        lua_setfield(L, -2, "repeat");
        break;

    case platform::input_event_type::mouse_button_down:
    case platform::input_event_type::mouse_button_up:
        lua_pushinteger(L, static_cast<lua_Integer>(ev.button));
        lua_setfield(L, -2, "button");
        lua_pushnumber(L, static_cast<lua_Number>(ev.x));
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, static_cast<lua_Number>(ev.y));
        lua_setfield(L, -2, "y");
        break;

    case platform::input_event_type::mouse_move:
        lua_pushnumber(L, static_cast<lua_Number>(ev.x));
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, static_cast<lua_Number>(ev.y));
        lua_setfield(L, -2, "y");
        break;

    case platform::input_event_type::wheel:
        lua_pushnumber(L, static_cast<lua_Number>(ev.wheel_x));
        lua_setfield(L, -2, "dx");
        lua_pushnumber(L, static_cast<lua_Number>(ev.wheel_y));
        lua_setfield(L, -2, "dy");
        break;

    case platform::input_event_type::gamepad_added:
    case platform::input_event_type::gamepad_removed:
        lua_pushinteger(L, static_cast<lua_Integer>(ev.device));
        lua_setfield(L, -2, "device");
        break;

    case platform::input_event_type::gamepad_button_down:
    case platform::input_event_type::gamepad_button_up:
        lua_pushinteger(L, static_cast<lua_Integer>(ev.device));
        lua_setfield(L, -2, "device");
        lua_pushinteger(L, static_cast<lua_Integer>(ev.button));
        lua_setfield(L, -2, "button");
        break;

    case platform::input_event_type::gamepad_axis:
        lua_pushinteger(L, static_cast<lua_Integer>(ev.device));
        lua_setfield(L, -2, "device");
        lua_pushinteger(L, static_cast<lua_Integer>(ev.axis));
        lua_setfield(L, -2, "axis");
        lua_pushnumber(L, static_cast<lua_Number>(ev.value));
        lua_setfield(L, -2, "value");
        break;

    case platform::input_event_type::gamepad_focus_lost:
 // 内部事件；input 不会把它放进 frame_events。
        break;
    }
}

} // namespace

void push_frame_info_table(lua_State* L, const frame_info& info) {
    lua_createtable(L, 0, 5);

    lua_pushnumber(L, info.dtg);
    lua_setfield(L, -2, "dtg");

    lua_pushnumber(L, static_cast<lua_Number>(info.frame));
    lua_setfield(L, -2, "frame");

 // 拖拽状态（2026-11-15）：本帧常量，移动与缩放都算。
    lua_pushboolean(L, info.dragging ? 1 : 0);
    lua_setfield(L, -2, "dragging");

 // 焦点（2026-11-16）：阶段 1 定格的真实值（以前写死 false）。
    lua_pushboolean(L, info.has_focus ? 1 : 0);
    lua_setfield(L, -2, "has_focus");

 // 事件批次（2026-11-16）：数组；没有事件 ⇒ nil（"没有"与"空表"要能区分）。
    if (info.events != nullptr && !info.events->empty()) {
        lua_createtable(L, static_cast<int>(info.events->size()), 0);
        int i = 1;
        for (const auto& ev : *info.events) {
            push_one_input_event(L, ev);
            lua_rawseti(L, -2, i);
            ++i;
        }
        lua_setfield(L, -2, "event");
    } else {
        lua_pushnil(L);
        lua_setfield(L, -2, "event");
    }
}

} // namespace
#endif

const char* hook_name(hook h) noexcept {
    const auto i = static_cast<std::size_t>(h);
    if (i >= hook_count) {
 // 到达这里意味着枚举越界。返回一个明确说"不知道"的字符串，
 // 而不是越界读表 —— 诊断输出里出现 "?" 比一次内存越界好。
        return "?";
    }
    return kHookNames[i];
}

const char* const* all_hook_names() noexcept {
    return kHookNames;
}

// =============================================================================
// frame_info 的构造
// =============================================================================

frame_info make_frame_info(const time::frame_state& fs) noexcept {
    frame_info info;

 // 【dtg 的取值】时间颗粒 = 1/60 秒，已定且锁死。
 // 它是**项目级常数**，不随帧率或机器性能变。
 // ⇒ 它就在定义里带了初值（frame_info::dtg），这里不再“填占位值”。

 // 帧号直接来自帧状态。它在阶段 1 已经按“仅 normal 帧递增”的规则算好。
 // 【注意它是"循环帧号"】见 hook.hpp 里 frame 字段的说明。
    info.frame = fs.frame_count;

 // 拖拽状态（2026-11-15）：同样是阶段 1 定格的"本帧常量"。
 // 移动与缩放都算 —— 见 hook.hpp 里 dragging 字段的说明。
    info.dragging = fs.dragging;

 // 焦点状态（2026-11-16）：由阶段 1 从 `window::has_focus()` 定格。
 // ⚠️ 以前这里是写死的 false —— 那时输入模块还没落地。
    info.has_focus = fs.has_focus;

 // 事件批次（2026-11-16）：阶段 4 采样之后才指向本帧那一批；
 // 阶段 3 的焦点钩子之前它是 nullptr ⇒ Lua 侧读到 nil（见 frame_state::events）。
    info.events = fs.events;

    return info;
}

// =============================================================================
// 调用点
// =============================================================================

void call_hook(state& st, hook h, const time::frame_state& fs) {
#if !defined(WAG_HAVE_LUAJIT)
 // 【无依赖档】fs 与下面的 name 只在接 LuaJIT 的那一支里被消费
 // （make_frame_info(fs)、取槽与报错的 name）。
 // 在这里显式忽略，让两种构建都不出警告
 // （项目纪律：警告一多，真问题就会被淹掉 —— 与 begin_frame /
 // wait_events_or_timeout 的 #else 分支是同一条纪律）。
    (void)fs;
#endif
    const char* const name = hook_name(h);

    if (!st.is_created()) {
 // 未接 LuaJIT 时没有槽可调。这不是脚本的错，是引擎还没就位。
        return;
    }

#if defined(WAG_HAVE_LUAJIT)
    lua_State* L = st.raw();

    lua_getglobal(L, "wag");
    lua_getfield(L, -1, "hook");
    lua_getfield(L, -1, name);

    if (!lua_isfunction(L, -1)) {
 // 白名单与表不一致 ⇒ 引擎内部完整性错误。信息里必须带上是哪个槽。
        wag::fail_internal(log::format("钩子槽不是函数", name));
    }

 // 实参是一个表（已定：实参以表传入）。
 //
 // 【栈形态的正确写法 —— 这一处是先前那个错误的根因】
 // lua_call / lua_pcall 要求：
 // 被调函数在 -(nargs+1) = **-2**
 // 实参从 -1 往上
 // ⇒ 也就是"**函数在下面、实参在它上面**"。
 //
 // 而 `lua_getfield(L, -1, name)` 取到槽之后，**函数已经在栈顶**，
 // 所以紧接着把实参压上去，形态就正好是 [.., func, info] ⇒ 够了。
 //
 // 先前我用 lua_insert(L, -2) / pushvalue+remove 把函数**顶到栈顶**，
 // 得到的却是 [.., info, func] ⇒ -2 变成了实参表 ⇒
 // Lua 从 -2 取到那个表 ⇒ "attempt to call a table value"。
    push_frame_info_table(L, make_frame_info(fs));

 // 【E8 记账】钩子是"引擎→脚本"最主要的落点：从这里进去之后，
 // 边界错误才抛得回脚本（`fail_boundary` 靠 error 的深度计数判断）。
    wag::hand_back_scope hand_back;
    if (lua_pcall(L, 1, 0, 0) != 0) {
 // 脚本错误 ⇒ L2：记日志 + 通知，然后继续推进帧结构。
 // "带错继续"指的是继续帧结构，不是继续那个出错的调用。
 //
 // 【临时诊断】错误文本里带上钩子名 —— 否则那几行 L2 看不出是哪个槽。
        const char* const why =
            (lua_tostring(L, -1) != nullptr) ? lua_tostring(L, -1)
                                             : "(非字符串的错误对象)";
 // 【E11 ③b：捕获点落盘】钩子是最主要的"引擎→脚本"落点，
 // 也是 raise 最常被捕获成"未捕获"的地方 ⇒ 在这里落盘。
        wag::flush_pending_raise();
 // 【消毒】把 Lua 错误文本里的绝对路径洗掉（规则 2）。
 // 这里**不能**用 capture_error 那条路：它是"取栈顶存进 g_last_error"，
 // 而钩子这条路径直接读栈顶 ⇒ 必须就地洗一次。
        const std::string why_clean = wag::paths::scrub(why);
        wag::fail_script(log::format(name, why_clean));
        lua_pop(L, 1); // 弹掉错误对象
    }

 // 弹掉 wag 与 wag.hook 这两个表。
 //
 // 【为什么是 2 而不是 3 —— 这一处修的是"栈漂移"】
 // 压进去的是 4 个：wag(+1)、wag.hook(+1)、槽(+1)、实参表(+1)。
 // 而 lua_pcall(1, 0) 会**弹出函数与那 1 个实参**（净 -2）
 // ⇒ 之后栈上剩下的只有 wag 与 wag.hook 两个。
 //
 // 先前这里写 `lua_pop(L, 3)`，多弹一格。而 LuaJIT **不会把它截断到栈底**，
 // 而是把栈基抬高 ⇒ 每次调用让 gettop 少 1 ⇒ 每帧 6 个钩子 ⇒ **每帧 -6**。
 // 独立对照实验（的 [F] 节）里：
 // pop(2) × 100 次 ⇒ gettop 仍为 0
 // pop(3) × 100 次 ⇒ gettop = -100（每次 -1）
    lua_pop(L, 2);
#else
 // 【无依赖档】name 在这里补一次显式忽略（理由见函数开头那条注释）。
    (void)name;
#endif
}

// =============================================================================
// call_hook_bare —— 生命周期钩子（无帧状态，实参空表）
// =============================================================================

namespace {

/// 全进程只调一次 `on_game_exit` 的闸门。
bool g_exit_hook_called = false;

} // namespace

void call_hook_bare(state& st, hook h) noexcept {
    if (!st.is_created()) {
        return; // 脚本状态没了 ⇒ 没有槽可调（不是错误：关闭后期就是这样）
    }

#if defined(WAG_HAVE_LUAJIT)
    lua_State* L = st.raw();
    const char* const name = hook_name(h);

 // 【栈平衡】压 wag、wag.hook、槽、空表 ⇒ 调完弹 2（与 call_hook 同一条纪律）。
    lua_getglobal(L, "wag");
    lua_getfield(L, -1, "hook");
    lua_getfield(L, -1, name);

    if (!lua_isfunction(L, -1)) {
 // 与 call_hook 同一条：白名单与表不一致 ⇒ 引擎内部完整性错误。
 // 但**生命周期钩子这一侧不终止**：它发生在关闭/启动的收尾阶段，
 // "把一个收尾错误升级成进程终止"没有收益。
        lua_pop(L, 3);
        wag::fail_script(log::format("生命周期钩子槽不是函数", name));
        return;
    }

    lua_createtable(L, 0, 0); // 空表：没有"本帧"

    if (lua_pcall(L, 1, 0, 0) != 0) {
 // **L1**：只记录，不弹窗、不写 warn 行（见 hook.hpp 的说明）。
        const char* const why = (lua_tostring(L, -1) != nullptr)
                                    ? lua_tostring(L, -1)
                                    : "(非字符串的错误对象)";
        const std::string cleaned = wag::paths::scrub(why);
        log::record("script", log::format("脚本钩子执行出错（继续关闭）", std::string(name) + "：" + cleaned));
        lua_pop(L, 1);
    }

    lua_pop(L, 2);
#else
    (void)st;
    (void)h;
#endif
}

bool call_exit_hook_once(state& st) noexcept {
    if (g_exit_hook_called) {
        return false;
    }
    g_exit_hook_called = true;
    call_hook_bare(st, hook::on_game_exit);
    return true;
}

} // namespace wag::script
