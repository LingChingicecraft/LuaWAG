// =============================================================================
// wag/script/primitives.cpp
//
// 内核：引擎原语的注册（跨交界的另一侧）。
//
// 【它做什么】
// 把引擎的能力注册成 Lua 侧可见的函数。这是"脚本调用引擎"那个方向的
// 实现位置。
//
// 【它不做什么】
// 它不负责"引擎调用脚本"（那是 hook.cpp），也不负责脚本的装载
//（那是引导层）。
//
// 【每个注册点的三件事 —— 缺一不可】
// ① 校验：跨边界处校验参数（见下）
// ② 落地：调用对应的引擎函数
// ③ 错误：非法数据在**这里**报错，而不是让它流进引擎内部
//
// 【为什么校验必须在这里，而不是在引擎内部各函数里】
// 因为这里是**唯一还有全部上下文**的位置：它知道是哪个原语、第几个参数、
// 调用方给的是什么。错过这里，错误就变成散落各处的防御性检查。
// =============================================================================

#include "draw/renderer.hpp"
#include "container/hit.hpp"
#include "container/pool.hpp"
#include "error/error.hpp"
#include "engine/engine.hpp"
#include "input/input.hpp"
#include "log/log.hpp"
#include "rng/rng.hpp"
#include "script/hook.hpp"
#include "script/state.hpp"
#include "text/font.hpp"
#include "platform/paths.hpp"
#include "platform/utf8_path.hpp"
#include "space/viewport.hpp"

#include <string_view>
#include <algorithm>
#include <filesystem>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>
// 【为什么需要 cstring】`set_state` / `reset_state` 要按名字比较键
//（`std::strcmp`）—— 名字表在渲染层，这里只做字符串分派。
#include <cstring>

// 写 luajit-2.1/ 版本目录以钉死身份（系统里还有一份 Lua 5.5 的 lua.h）。
#if defined(WAG_HAVE_LUAJIT)
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

// -----------------------------------------------------------------------------
// WAG_SET_FN —— 注册一个 C 函数，**名字从符号派生**（手写暴露 结论 A1 对策1）
//
// WAG_SET_FN(L, draw_vertices)
// 展开：pushcfunction(&l_draw_vertices) → setfield(-2, "draw_vertices")
//
// 【它治什么】事实 1：C 函数在 Lua 侧的名字是字符串，编译器零参与。
// 宏把符号字符串化 ⇒ 字符串由符号生成 ⇒ 改名即同步，
// "字符串写对、符号写错"（或反过来）这类缺陷在编译期消失。
//
// 【栈契约（净零）—— 两次真实缺陷换来的形状（2026-10-15，子目标 3）】
// 调用前：**目标表在栈顶**。宏内两步：
// push 函数（表落到 -2）→ setfield(-2)（写入并**自动弹出**函数）。
// ⇒ 调用后：表仍在栈顶，栈严格净零 —— 调用处没有任何索引可传错。
//
// 两个被否决的早期版本，都死于同一个边界（手写暴露 边界1：
// "栈相关缺陷的症状与原因相隔至少一次调用"）：
// · v1（带 tbl 参数）：调用处传 -1 —— 而 push 之后 -1 已经是**函数**，
// setfield 对函数取字段 ⇒ Lua 错误；
// · v2（改成"表在栈顶"约定后多加了 pop）：setfield 本来就会弹掉
// 函数，末尾再 pop(L,1) 把**目标表**也弹掉了 ⇒ 下一条
// setfield(L, -2) 越过栈底 ⇒ Lua 错误。
// 两次症状完全相同：注册期没有 pcall 接 ⇒ 进程无声崩溃，表现为
// "步骤 0 之后的日志全部消失"，离真正的错隔了半屏代码；
// 且换用任何一个已知良好的函数冒名都照样崩 —— 因为错的不是函数，
// 是栈。定位手段：在每个注册子块后插临时标记日志（五个块二分），
// 再配合 gdb 的异常码（0xE24C4A02 = LuaJIT 在 Win64 抛 Lua 错误）。
// ⇒ 这两个形状决定都是这两轮换来的：**索引参数拿掉**（无索引可
// 传错）+ **不自带 pop**（setfield 的隐式弹栈只算一次）。
//
// ⚠️【本文件其余注册点仍是手写字符串】它们早于这条纪律写下；
// 全文件统一改造（含"注册清单可枚举、与词汇表机械对账"，A2/A7）
// 归"绑定层对账"那一批。在那之前，**新增 API 一律走本宏**。
// -----------------------------------------------------------------------------
#define WAG_SET_FN(L, fn) \
    (lua_pushcfunction(L, &l_##fn), lua_setfield(L, -2, #fn))

// -----------------------------------------------------------------------------
// WAG_SET_FN_AS —— 同上，但**显式给 Lua 侧名字**
//
// 【什么时候用它】当 C 符号需要带**族前缀**、而 Lua 侧已经在子表里时。
// 例：`wag.image.load_from_file` 的符号是 `l_image_load_from_file`
//（避免将来 `l_unload` 这类通用名撞车），而 Lua 侧名字只要 `load_from_file`。
// 【为什么仍然比裸 setfield 好】符号与字符串写在同一行 ⇒ 改名时看得见；
// 这比"两处分别写、谁也不知道对方"安全一个档次。
// ⚠️ 纪律不变：**能用 WAG_SET_FN 的地方一律用它**（名字从符号派生）。
// -----------------------------------------------------------------------------
#define WAG_SET_FN_AS(L, fn, lua_name) \
    (lua_pushcfunction(L, &l_##fn), lua_setfield(L, -2, lua_name))
#endif

namespace wag::script {

// 【命名空间】原先这里有 `namespace core = wag::core;` 别名。`wag::core` 已拆除
//（见 log/log.hpp 文件头）；引擎实例与错误架构在 `wag::`，日志在 `wag::log`。

// -----------------------------------------------------------------------------
// engine_of —— 从 Lua 状态取回引擎实例
//
// 每个原语的 C 函数只收到 lua_State*，而它要操作 wag::engine，所以两者之间
// 必然有一个关联点。这里采用"把 engine* 存进 Lua 注册表"。
//
// 不选文件级静态指针的理由：那把"只有一个实例"写死，而可测性希望将来能有
// 两个实例。
// -----------------------------------------------------------------------------

#if defined(WAG_HAVE_LUAJIT)
namespace {

// 注册表里存引擎指针用的键。lightuserdata 作键，不参与 GC。
char kEngineRegistryKey = 0;

/// 把引擎实例存进 Lua 注册表。register_primitives 调用它一次。
void bind_engine(lua_State* L, wag::engine& eng) {
    lua_pushlightuserdata(L, &kEngineRegistryKey);
    lua_pushlightuserdata(L, &eng);
    lua_settable(L, LUA_REGISTRYINDEX);
}

/// 从 Lua 状态取回引擎实例。
wag::engine& engine_of(lua_State* L) {
    lua_pushlightuserdata(L, &kEngineRegistryKey);
    lua_gettable(L, LUA_REGISTRYINDEX);
    auto* eng = static_cast<wag::engine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (eng == nullptr) {
        wag::fail_internal("引擎原语被调用时没有绑定引擎实例");
    }
    return *eng;
}

} // namespace
#endif

namespace {

// =============================================================================
// 辅助：从 Lua 栈上读出并校验一个 double
// =============================================================================
//
// 【为什么校验放在这里而不是在引擎内部】
// 见文件头。这里是唯一还有全部上下文的地方。
//
// 【不确定代码 · 待定】参数的序号与名字如何传给报错信息
// 已定："错误信息要带哪个参数、它的值"。而"参数的序号"在 Lua 的 C API 里
// 不是自动可得的（栈上只有位置）。两种做法：
// (甲) 每个 C 函数在自己的开头手工写死参数名（啰嗦但精确）
// (乙) 提供一个宏，把参数名与栈索引绑在一起
// 在收口之前按 (甲) 的形状写。

/// 取栈上第 idx 个位置的 double，并校验它是有限值。
///
/// @param L Lua 状态
/// @param idx 栈索引
/// @param where 参数名（用于报错定位）
/// @return 合法的 double
///
/// 非法时终止：跨边界校验的级别是 L3，不能带错继续。
[[maybe_unused]] double checked_double(lua_State* L, int idx, const char* where) {
#if defined(WAG_HAVE_LUAJIT)
    if (!lua_isnumber(L, idx)) {
        wag::error_value e;
        e.cls = wag::error_class::boundary_validation;
        e.where = where;
        e.got = luaL_typename(L, idx);
        e.want = "一个数";
        wag::fail_boundary(e);
    }
    const double v = static_cast<double>(lua_tonumber(L, idx));
    if (!wag::is_finite(v)) {
        wag::error_value e;
        e.cls = wag::error_class::boundary_validation;
        e.where = where;
        e.got = "不是有限值";
        e.want = "有限实数";
        wag::fail_boundary(e);
    }
    return v;
#else
    (void)L;
    (void)idx;
    (void)where;
    return 0.0;
#endif
}

} // namespace

// =============================================================================
// wag.engine
//
// 【四个成员，一个字都不多】
// 判据是"控制引擎**是否运行**"。而"进入帧循环"那个入口**不属于**这里
// —— 它是**启动**，不是运行期控制（见 engine/engine.hpp）。
//
// 【四个的完整性】
// 由"**一个事件不能是自己的前提**"证成：任何第五个控制操作都必须能在
// 引擎运行之前被调用，而那就与"引擎还没开始运行"矛盾。
// =============================================================================

#if defined(WAG_HAVE_LUAJIT)
namespace {

// -----------------------------------------------------------------------------
// 句柄参数的统一校验（2026-11-10）
//
// 【规则】
// · 命令类（kill / set_hit / replace / write / draw_*）：句柄必须是
// 非负、**精确整数**、且能精确放进 Lua number（<= 2^53-1）；
// 否则 L3（：失效/不可解释的句柄不许静默操作）。
// · 查询类（alive / image.size）：不合法 ⇒ false / nil，不终止。
// 【为什么不能只靠 luaL_checkinteger】
// Lua 5.1 的 luaL_checkinteger 会把 1.5 截断成 1 ——
// 那会静默操作“相邻的另一个对象”，比直接报错糟得多。
// -----------------------------------------------------------------------------

/// 把 double 转成"精确的非负整数句柄"；不合法 ⇒ L3。
lua_Integer handle_from_number(double n, const char* where) {
    constexpr double kMaxExactHandle = 9007199254740991.0; // 2^53-1
    if (!std::isfinite(n) || n < 0.0 || n > kMaxExactHandle ||
        std::floor(n) != n) {
        char got_buf[64];
        std::snprintf(got_buf, sizeof(got_buf), "%g", n);
        wag::error_value e;
        e.cls = wag::error_class::boundary_validation;
        e.where = where;
        e.got = got_buf;
        e.want = "一个非负整数句柄（不能是小数、NaN、Inf 或超出 2^53-1）";
        e.sentence = "句柄不是精确整数";
        wag::fail_boundary(e);
    }
    return static_cast<lua_Integer>(n);
}

/// 读命令类句柄参数；不合法 ⇒ L3。
lua_Integer read_handle_arg(lua_State* L, int idx, const char* where) {
    return handle_from_number(
        static_cast<double>(luaL_checknumber(L, idx)), where);
}

/// 查询类用：能解释成精确句柄就写入 out 并返回 true；否则 false。
bool exact_handle(double n, lua_Integer& out) {
    constexpr double kMaxExactHandle = 9007199254740991.0;
    if (!std::isfinite(n) || n < 0.0 || n > kMaxExactHandle ||
        std::floor(n) != n) {
        return false;
    }
    out = static_cast<lua_Integer>(n);
    return true;
}

/// 命令类入口的**唯一取容器方式**：活着就返回，否则**在交界处**报 boundary。
///
/// 【它替换什么】原本各入口写的是
/// container* c = eng.containers().get(h); // 死句柄 ⇒ fail_internal ⇒ 终止
/// 而"脚本传了个旧句柄"是**调用者数据**（E2），不是引擎 bug ⇒ 该抛，不该杀进程。
///
/// 【为什么必须先 probe 再 try_get】
/// `try_get` 只回答"没有"，不回答"为什么没有"。直接用它会退化成
/// 「句柄无效」，而深层原本能说清是"已销毁"还是"代号不匹配"。
/// ⇒ probe 给出原因，这里拼成 error_value；try_get 只负责取指针。
/// 两次查询看起来很傻，但两者都是 O(1) 位运算，且换来的信息质量是本次大修的目标之一。
///
/// 【报错点必须仍在副作用之前】本函数只读，不写容器 ⇒ 天然满足 E3。
///
/// =============================================================================
/// 【还差什么 —— 本轮"接上了"，但"可捕获"还没生效】
///
/// ① **`fail_boundary` 现在仍然终止**（`error.cpp:128` 走 `log::terminate`）。
/// ⇒ 接线后这些点位的**外部行为看起来没变**：依然杀进程。
/// ⇒ `pcall` 要等到 E8 的"有交还者就抛"落地之后才接得住。
/// **判据**：现在 `pcall(wag.container.kill, 0)` 仍返回 false **且进程死亡**；
/// E8 之后应返回 false **且进程活着、帧继续**。
///
/// ② **3 个 A 类点位走的是 `require_pool_id`**（池 id 存在性 / 池数上限），
/// 与句柄无关 —— 本函数管不了那一类，别在这里加池 id 判断。
///
/// ③ **C 类（池满 ×2）本轮没做**：`spawn` / `replace` 里的 `allocate()`
/// 仍是 `fail_internal` ⇒ 池满依然终止。要换成 `try_allocate` + 交界处报。
///
/// ④ **D 类（`write_batch` 先写后查）本轮没做**：
/// 它仍是"先写 17 个字段、再查 blend/vcolor/tex_alpha"，命中 E3
///（副作用已发生）⇒ 按 E3 它**必须保持 L4**，直到改成两个 pass。
///
/// ⑤ 深层的 `get()` / `fail_bad_handle` 一行未动 —— 它们从此成为
/// **真正的不可达断言**（附录 A 的架构立场由此恢复）。若哪天再被触发，
/// 说明交界处又漏了一个入口。
/// =============================================================================
wag::container::container* require_container(wag::engine& eng,
                                            wag::container::handle h,
                                            const char* where) {
    using namespace wag::container;

    const handle_failure f = eng.containers().probe(h);
    if (f == handle_failure::none) {
        return eng.containers().try_get(h);
    }

 // 只有"池存在但句柄不对"时，才说得清池名与下标。
 // bad_pool / invalid_handle 没有可指的池，硬凑会把错误信息说错。
    std::string got = handle_failure_name(f);
    got += "（handle=";
    got += std::to_string(h);
    if (f != handle_failure::bad_pool && f != handle_failure::invalid_handle) {
        got += "，池「";
        got += eng.containers().pool(handle_pool(h)).name();
        got += "」下标 ";
        got += std::to_string(handle_index(h));
    }
    got += "）";

 // 【可观测点 —— 单步调试与运行日志都能证明"真的进了 helper"】
 // 只放在失败分支：成功路径是热路径（每次 write 都走），不能有日志。
    wag::log::record("交界", "require_container 拒绝了一个坏句柄", got);

    wag::error_value e;
    e.cls = wag::error_class::boundary_validation;
    e.where = where;
    e.got = got;
    e.want = "一个存活的容器句柄（：旧句柄被使用必须报错）";
    e.sentence = "容器句柄无效";
    wag::fail_boundary(e); // E8 之后这里会抛；现在仍终止（见上面"还差什么"①）
}

/// 命令类入口的**池 id 预检**：只检查"这个池存在吗"，越界 ⇒ 在交界处报 boundary。
///
/// 【为什么它不查 0xFFFFFFFF】调用方已经查过了（`spawn` / `write_batch` 各有一处
/// `raw_pool > 0xFFFFFFFFLL` 的守卫），这里再查一遍会让"谁负责校验"变成两个答案。
///
/// 【为什么它和 require_container 分开】两者查的是**不同的东西**：
/// 一个是"池存在吗"，一个是"句柄活着吗"。混成一个函数会让调用点看不出在查哪一层。
[[nodiscard]] std::uint32_t require_pool_id(wag::engine& eng, std::uint32_t id,
                                            const char* where) {
    if (id < eng.containers().pool_count()) {
        return id;
    }
    wag::error_value e;
    e.cls = wag::error_class::boundary_validation;
    e.where = where;
    const std::string got =
        std::to_string(id) + "（当前只有 " +
        std::to_string(eng.containers().pool_count()) + " 个池）";
    e.got = got;
    e.want = "一个已存在的池 id（见 wag.container.new_pool）";
    e.sentence = "容器池 id 不存在";
    wag::log::record("交界", "require_pool_id 拒绝了越界的池 id", got);
    wag::fail_boundary(e); // E8 之后会抛
}

// ---- wag.engine ----
//
// 四个成员，一个字都不多。判据是"控制引擎是否运行"。
// 注意 exit_requested() 不在这里 —— 它是给主循环读的，不是脚本的控制面。

int l_engine_pause(lua_State* L) {
    engine_of(L).request_pause();
    return 0;
}

int l_engine_resume(lua_State* L) {
    engine_of(L).request_resume();
    return 0;
}

/// 返回的是"本帧开始时的状态"，不是刚请求的变更。
int l_engine_get_state(lua_State* L) {
    lua_pushstring(L, wag::state_name(engine_of(L).get_state()));
    return 1;
}

int l_engine_exit(lua_State* L) {
    engine_of(L).request_exit();
    return 0;
}

// ---- wag.space.set_viewport{ render = {...}, strategy = "..." } ----
//
// render **可省略**（表示保持不变）
// strategy **不可省略**，省略即报错
int l_space_set_viewport(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

 // strategy（必需）
    lua_getfield(L, 1, "strategy");
    if (!lua_isstring(L, -1)) {
        wag::error_value e;
        e.cls = wag::error_class::api_misuse;
        e.where = "set_viewport 的 strategy";
        e.got = lua_isnil(L, -1) ? "省略" : luaL_typename(L, -1);
        e.want = "\"letterbox\" 或 \"stretch\"";
        wag::fail_boundary(e);
    }
    space::fit_strategy strat{};
    if (!space::parse_strategy(lua_tostring(L, -1), strat)) {
        wag::error_value e;
        e.cls = wag::error_class::api_misuse;
        e.where = "set_viewport 的 strategy";
        e.got = lua_tostring(L, -1);
        e.want = "\"letterbox\" 或 \"stretch\"";
        wag::fail_boundary(e);
    }
    lua_pop(L, 1);

 // render（可选）
    lua_getfield(L, 1, "render");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);

        lua_getfield(L, -1, "l"); const double l = checked_double(L, -1, "render.l"); lua_pop(L, 1);
        lua_getfield(L, -1, "b"); const double b = checked_double(L, -1, "render.b"); lua_pop(L, 1);
        lua_getfield(L, -1, "w"); const double w = checked_double(L, -1, "render.w"); lua_pop(L, 1);
        lua_getfield(L, -1, "h"); const double h = checked_double(L, -1, "render.h"); lua_pop(L, 1);

 // 宽高必须 > 0：映射里有一个除以它的步骤。
        if (!(w > 0.0) || !(h > 0.0)) {
            wag::error_value e;
            e.cls = wag::error_class::boundary_validation;
            e.where = "set_viewport 的 render 宽高";
            e.want = "> 0";
            wag::fail_boundary(e);
        }
        engine_of(L).viewport().set_render(space::rect{l, b, w, h});
    }
    lua_pop(L, 1);

    engine_of(L).viewport().set_strategy(strat);
    return 0;
}

/// 返回只读的一份：只有 render 与 strategy。变换不跨 Lua。
int l_space_get_viewport(lua_State* L) {
    const auto& vp = engine_of(L).viewport();
    const space::rect r = vp.get_render();

    lua_newtable(L);

    lua_newtable(L);
    lua_pushnumber(L, static_cast<lua_Number>(r.l)); lua_setfield(L, -2, "l");
    lua_pushnumber(L, static_cast<lua_Number>(r.b)); lua_setfield(L, -2, "b");
    lua_pushnumber(L, static_cast<lua_Number>(r.w)); lua_setfield(L, -2, "w");
    lua_pushnumber(L, static_cast<lua_Number>(r.h)); lua_setfield(L, -2, "h");
    lua_setfield(L, -2, "render");

    lua_pushstring(L, space::strategy_name(vp.get_strategy()));
    lua_setfield(L, -2, "strategy");

    return 1;
}

// ---- wag.time.set_target_fps(n) ----
//
// 只判断取值是否在定义域内（> 0），不做"多少算太小"的合理性判断。
int l_time_set_target_fps(lua_State* L) {
    const double n = checked_double(L, 1, "set_target_fps 的参数");
 // 用 !(n > 0.0) 而不是 n <= 0.0：NaN 与两者都比较为 false。
    if (!(n > 0.0)) {
        wag::error_value e;
        e.cls = wag::error_class::boundary_validation;
        e.where = "set_target_fps 的参数";
        e.got = "不是正数";
        e.want = "> 0";
        wag::fail_boundary(e);
    }
    engine_of(L).request_target_fps(n);
    return 0;
}

// ---- wag.space.set_window_size(w, h) ----
//
// 两个**位置参数**（不是表）：因为只有两个必需值、没有可选值。
// 而 set_viewport 用表是因为那里有可选字段（render 可省）—— 形状由需要决定。
//
// 【单位】客户区的像素尺寸（与窗口尺寸同一套），引擎不做任何换算。
//
// 【它是"请求"】平台可能给不出来 ⇒ 引擎记一条 L2 并继续。
// 所以接口说明里必须写清：这是**申请**，不是"设完就一定是这么大"。
int l_space_set_window_size(lua_State* L) {
    const double w = checked_double(L, 1, "set_window_size 的宽");
    const double h = checked_double(L, 2, "set_window_size 的高");

 // 宽高必须 > 0：0 或负数属于数据矛盾（窗口尺寸无定义）。
 // 用 !(x > 0.0) 而不是 x <= 0.0：NaN 与两者都比较为 false。
    if (!(w > 0.0) || !(h > 0.0)) {
        wag::error_value e;
        e.cls = wag::error_class::boundary_validation;
        e.where = "set_window_size 的宽高";
        e.want = "> 0";
        wag::fail_boundary(e);
    }

    engine_of(L).request_window_size(w, h);
    return 0;
}

/// 返回引擎侧记住的"最后一次请求的窗口尺寸"，或 nil（从未请求过）。
///
/// 【它不是"窗口现在多大"】窗口多大要新查询，而本函数不给 —— 那属于
/// "读取外部事实"，与"读取我们请求了什么"是两件事，不该混进同一个入口。
int l_space_get_window_size(lua_State* L) {
    auto& eng = engine_of(L);
    if (!eng.has_requested_window_size()) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, static_cast<lua_Number>(eng.requested_window_w()));
    lua_pushnumber(L, static_cast<lua_Number>(eng.requested_window_h()));
    return 2;
}

/// wag.space.get_frame_size() -> w, h —— 帧缓冲的**物理像素**尺寸（C1，2026-11-16）
///
/// 【它与别的尺寸的区别】见：
/// render = 逻辑可见范围（DIP） window = 客户区逻辑尺寸（DIP）
/// frame = 帧缓冲整帧（像素） ← **就是本函数**
/// box = render 映射进帧缓冲后的矩形（像素，只给着色器 `u_box`）
///
/// 【时机】**阶段 1 定格** ⇒ 一帧之内值不变（与 `frame_info.dragging` 同一条纪律）。
///
/// 【用途边界】只用于**整屏效果**（着色器全屏四边形、全屏遮罩、截图取景）。
/// ⚠️ **不得用于游戏布局** —— 布局一律用逻辑坐标；用像素布局会在换窗口/改 render
/// 时整体崩掉，且崩得很隐蔽。
///
/// 【无 SDL 档】返回 0, 0（"没有屏幕"是事实，不是错误）。
int l_space_get_frame_size(lua_State* L) {
    auto& eng = engine_of(L);
    lua_pushnumber(L, static_cast<lua_Number>(eng.frame_pixel_w()));
    lua_pushnumber(L, static_cast<lua_Number>(eng.frame_pixel_h()));
    return 2;
}

// =============================================================================
// wag.argv / wag.control —— 控制通道的三条原语（，2026-11-17）
//
// 【三条都不认识结点】它们只搬字符串：
// · `wag.argv.start_node()` 取 `--start-node` 的原样值，脚本自己解释
// · `wag.control.take()` 取一条命令（动词 + 参数），每帧最多 8 条
// · `wag.control.publish(k,v)` 标一个键值，引擎 1 Hz 写 debug/state.json
//
// 【`--control` 没给时】take 恒 nil；publish 静默无操作；引擎不碰磁盘。
// =============================================================================

/// wag.argv.start_node() -> string | nil
int l_argv_start_node(lua_State* L) {
    auto& eng = engine_of(L);
    const std::string& v = eng.control().start_node();
    if (v.empty()) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, v.data(), v.size());
    return 1;
}

/// wag.control.take() -> verb, arg | nil
///
/// 【每帧 8 条的上限在引擎侧】这里连续调用即可取完本帧额度；第 9 次返回 nil。
int l_control_take(lua_State* L) {
    auto& eng = engine_of(L);
    std::string verb;
    std::string arg;
    while (eng.control().take(verb, arg)) {
 // 热重载是引擎命令；消费掉，不交给脚本。
        if (eng.handle_reload_command(verb, arg)) {
            continue;
        }
        lua_pushlstring(L, verb.data(), verb.size());
        lua_pushlstring(L, arg.data(), arg.size());
        return 2;
    }
    lua_pushnil(L);
    return 1;
}

/// wag.control.publish(key, value) -> nil
///
/// 【值先转成字符串】引擎不认识类型，只在写 JSON 时按"像不像数字"决定加不加引号。
int l_control_publish(lua_State* L) {
    auto& eng = engine_of(L);
    if (!lua_isstring(L, 1) && !lua_isnumber(L, 1)) {
        return luaL_error(L, "publish 的 key 必须是字符串，got %s",
                          luaL_typename(L, 1));
    }
    if (lua_isnil(L, 2)) {
        return 0;   // nil = 这次不发布（不报错）
    }
    std::string key;
    std::string value;
    {
        std::size_t n = 0;
        const char* s = lua_tolstring(L, 1, &n);
        if (s != nullptr) { key.assign(s, n); }
 // lua_tolstring 就地转换，不需要 pop
    }
    {
        std::size_t n = 0;
        const char* s = lua_tolstring(L, 2, &n);
        if (s != nullptr) { value.assign(s, n); }
 // lua_tolstring 就地转换，不需要 pop
    }
    eng.control().publish(key, value);
    return 0;
}

// =============================================================================
// wag.audio —— 音频子系统（，2026-11-17）
//
// 【形状】资源句柄（load/duration/unload）+ voice（play/stop/set_volume/...）。
// 资源与 voice **分离**：同一个音效可以同时响多次。
// 【线程】这些函数只在主线程调用；它们只写队列，不碰音频线程。
// =============================================================================

int l_audio_load(lua_State* L) {
    auto& eng = engine_of(L);
    const char* name = luaL_checkstring(L, 1);
    const char* path = luaL_checkstring(L, 2);
    const std::uint64_t h = eng.audio().load(name, path);
    lua_pushnumber(L, static_cast<lua_Number>(h));
    return 1;
}

int l_audio_unload(lua_State* L) {
    auto& eng = engine_of(L);
    eng.audio().unload(luaL_checkstring(L, 1));
    return 0;
}

/// wag.audio.lookup(name) -> handle | nil（查询语义，不报错）
int l_audio_lookup(lua_State* L) {
    auto& eng = engine_of(L);
    const char* name = luaL_checkstring(L, 1);
    std::uint64_t handle = 0;
    if (!eng.audio().lookup(name, handle)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, static_cast<lua_Number>(handle));
    return 1;
}

int l_audio_duration(lua_State* L) {
    auto& eng = engine_of(L);
    if (!lua_isnumber(L, 1)) {
        lua_pushnil(L);
        return 1;
    }
    const double sec = eng.audio().duration(
        static_cast<std::uint64_t>(lua_tonumber(L, 1)));
    if (sec < 0.0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, sec);
    return 1;
}

/// wag.audio.play(handle, { volume=, pan=, loop=, music= }) -> voice_id
/// 【pan 的语义是**平衡（balance）**】∈ [-1,1]：-1 纯左 / 0 居中（−3 dB）/ +1 纯右；
/// 两声道各乘系数、**不折叠成立体声**（、）。
int l_audio_play(lua_State* L) {
    auto& eng = engine_of(L);
    const std::uint64_t h = static_cast<std::uint64_t>(luaL_checknumber(L, 1));
    float volume = 1.0f;
    float pan = 0.0f;
    bool loop = false;
    bool music = false;
    std::uint32_t loop_start = 0;
    std::uint32_t loop_end = 0;
    std::uint32_t start_frame = 0;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "volume");
        if (lua_isnumber(L, -1)) { volume = static_cast<float>(lua_tonumber(L, -1)); }
        lua_pop(L, 1);
        lua_getfield(L, 2, "pan");
        if (lua_isnumber(L, -1)) { pan = static_cast<float>(lua_tonumber(L, -1)); }
        lua_pop(L, 1);
        lua_getfield(L, 2, "loop");
        loop = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, 2, "music");
        music = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
 // 【循环区间：采样帧】0 表示"未给"（loop_end = 0 ⇒ 到结尾）
        lua_getfield(L, 2, "loop_start");
        if (lua_isnumber(L, -1)) {
            loop_start = static_cast<std::uint32_t>(lua_tointeger(L, -1));
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "loop_end");
        if (lua_isnumber(L, -1)) {
            loop_end = static_cast<std::uint32_t>(lua_tointeger(L, -1));
        }
        lua_pop(L, 1);
 // 【起点，不是 seek】脚本侧"跳到某处继续播"= 再调一次 play(start_frame=…)
        lua_getfield(L, 2, "start_frame");
        if (lua_isnumber(L, -1)) {
            start_frame = static_cast<std::uint32_t>(lua_tointeger(L, -1));
        }
        lua_pop(L, 1);
    }
    const std::uint32_t id = eng.audio().play(h, volume, pan, loop, music,
                                             loop_start, loop_end, start_frame);
    if (id == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    return 1;
}

int l_audio_stop(lua_State* L) {
    engine_of(L).audio().stop(static_cast<std::uint32_t>(luaL_checkinteger(L, 1)));
    return 0;
}

// ---- 暂停 / 继续（**不用布尔开关参数**，两个方向两个动词）----

int l_audio_pause(lua_State* L) {
    engine_of(L).audio().pause(static_cast<std::uint32_t>(luaL_checkinteger(L, 1)));
    return 0;
}

int l_audio_resume(lua_State* L) {
    engine_of(L).audio().resume(static_cast<std::uint32_t>(luaL_checkinteger(L, 1)));
    return 0;
}

int l_audio_pause_all(lua_State* L) {
    engine_of(L).audio().pause_all();
    return 0;
}

int l_audio_resume_all(lua_State* L) {
    engine_of(L).audio().resume_all();
    return 0;
}

int l_audio_is_paused(lua_State* L) {
    lua_pushboolean(L, engine_of(L).audio().is_paused(
                            static_cast<std::uint32_t>(luaL_checkinteger(L, 1))) ? 1 : 0);
    return 1;
}

// ---- 频谱 / 波形----
//
// 【为什么复用同一张表】每秒调用 60 次，每次都建表 = 每秒 60 张垃圾。
// 引擎在注册表里持有一张表，每次覆盖它的 32 / 128 个元素。
// 代价：调用方**不能同时持有两份快照**。

static int push_reused_table(lua_State* L, const char* key, const float* values, int count) {
    lua_getfield(L, LUA_REGISTRYINDEX, key);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_createtable(L, count, 0);
        for (int i = 0; i < count; ++i) {
            lua_pushnumber(L, 0.0);
            lua_rawseti(L, -2, i + 1);
        }
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, key);
    }
    for (int i = 0; i < count; ++i) {
        lua_pushnumber(L, static_cast<lua_Number>(values[i]));
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_audio_fft_bins(lua_State* L) {
    wag::audio::spectrum_frame f;
    engine_of(L).audio().read_spectrum(f);
    return push_reused_table(L, "wag.audio.fft_bins", f.bands,
                             wag::audio::spectrum_frame::kBands);
}

int l_audio_waveform(lua_State* L) {
    wag::audio::spectrum_frame f;
    engine_of(L).audio().read_spectrum(f);
    return push_reused_table(L, "wag.audio.waveform", f.wave,
                             wag::audio::spectrum_frame::kWavePoints);
}

int l_audio_set_volume(lua_State* L) {
    engine_of(L).audio().set_voice_volume(
        static_cast<std::uint32_t>(luaL_checkinteger(L, 1)),
        static_cast<float>(luaL_checknumber(L, 2)));
    return 0;
}

int l_audio_set_pan(lua_State* L) {
    engine_of(L).audio().set_voice_pan(
        static_cast<std::uint32_t>(luaL_checkinteger(L, 1)),
        static_cast<float>(luaL_checknumber(L, 2)));
    return 0;
}

int l_audio_is_active(lua_State* L) {
    lua_pushboolean(L, engine_of(L).audio().is_active(
                            static_cast<std::uint32_t>(luaL_checkinteger(L, 1))) ? 1 : 0);
    return 1;
}

int l_audio_stop_all(lua_State* L) {
    engine_of(L).audio().stop_all();
    return 0;
}

/// wag.audio.current_music() -> voice | nil, handle | nil
///
///：只查询当前 BGM 专用槽，不改变播放状态；没有 BGM ⇒ nil, nil。
int l_audio_current_music(lua_State* L) {
    std::uint32_t voice = 0;
    std::uint64_t handle = 0;
    if (!engine_of(L).audio().current_music(voice, handle)) {
        lua_pushnil(L);
        lua_pushnil(L);
        return 2;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(voice));
    lua_pushinteger(L, static_cast<lua_Integer>(handle));
    return 2;
}

int l_audio_set_master_volume(lua_State* L) {
    engine_of(L).audio().set_master_volume(static_cast<float>(luaL_checknumber(L, 1)));
    return 0;
}

int l_audio_get_master_volume(lua_State* L) {
    lua_pushnumber(L, engine_of(L).audio().master_volume());
    return 1;
}

int l_audio_set_music_volume(lua_State* L) {
    engine_of(L).audio().set_music_volume(static_cast<float>(luaL_checknumber(L, 1)));
    return 0;
}

int l_audio_get_music_volume(lua_State* L) {
    lua_pushnumber(L, engine_of(L).audio().music_volume());
    return 1;
}

int l_audio_set_se_volume(lua_State* L) {
    engine_of(L).audio().set_se_volume(static_cast<float>(luaL_checknumber(L, 1)));
    return 0;
}

int l_audio_get_se_volume(lua_State* L) {
    lua_pushnumber(L, engine_of(L).audio().se_volume());
    return 1;
}

// ---- wag.boot.run() ----
//
// 【它为什么在 wag.boot 而不是 wag.engine】
// 因为 `wag.engine` 的四个成员负责"引擎**是否在运行**"，而"开始运行"是
// 启动流程的一部分。把它加进 wag.engine 会破坏那四个成员的完整性证成
//（"一个事件不能是自己的前提"）。
//
// 【它为什么只置标志，不跑循环（已决：乙方案）】
// 若它同步驱动帧循环，那么"引擎的主循环"与"一个脚本可调用的函数"就成了
// 同一个东西，而 main 不再是唯一拥有循环的地方。
// 而只置标志还有一个好处：init.lua 中途报错时标志不会置上 ⇒ main 能分辨
// "装载完成"与"装载失败"，而不是"静默进了循环"。
int l_boot_run(lua_State* L) {
    engine_of(L).request_frame_loop();
    return 0;
}

// -----------------------------------------------------------------------------
// 辅助：读顶点表的一个数字字段
// -----------------------------------------------------------------------------

/// 读第 vtx 个顶点表（栈上 tbl）的 field 字段，按 float 落盘。
///
/// 【缺字段 / 类型不对 ⇒ Lua 错误】缺参数与值不合法都是数据矛盾
///（与 set_viewport 的 strategy 省略即报错同一条立场）。
///
/// 【lua_getfield 会触发元方法】作者给顶点表挂 __index 是他的选择，
/// 那份成本可见地落在他的调用点上（交界频率判据：每对象每帧一次档）。
/// 而"渲染决策二 2.4.3 ② 用 rawgeti 避免元方法"针对的是**顶点数组的
/// 数组序访问**——这里按名读字段是 lstg 顶点结构的既定形态。
///
/// 【有限值不在这里查】x/y/z/u/v 的 NaN / 无穷统一交给渲染层的
/// find_vertex_defect（校验逻辑只有一份）；本函数只保证"是数字"。
float read_vertex_number(lua_State* L, int tbl, const char* field, int vtx) {
    lua_getfield(L, tbl, field);
    if (!lua_isnumber(L, -1)) {
        const char* tn = luaL_typename(L, -1);
        lua_pop(L, 1);
 // luaL_error 是 longjmp，不返回；cast 只为满足返回类型。
        return static_cast<float>(luaL_error(
            L, "draw_vertices 的第 %d 个顶点缺少数字字段 %s，got %s",
            vtx + 1, field, tn));
    }
    const double v = static_cast<double>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return static_cast<float>(v);
}

// -----------------------------------------------------------------------------
// wag.draw.draw_image(image, layer, z, x, y, w, h) —— 纯位置版（2026-10-18）
//
// 【它为什么参数全是数字】它是"整张图按位置与大小画出来"那条最常用的路：
// 不需要 UV（UV 从图片尺寸推出来 ⇒ "页与 UV 同源"自动成立），
// 也不需要顶点表 ⇒ 每次绘制的表构造开销为零。
//
// 【锚点】图片**左上角**落在逻辑 (x, y)；逻辑 +y 向上 ⇒ 矩形占 x..x+w、y-h..y。
// 将来 `SetImageCenter` 落地时锚点变成"图片中心落在 (x, y)"，
// 而 center 现在恒为 (0,0) ⇒ 那时调用方语义不变（向后兼容）。
//
// 【错误档位：与 draw_vertices 一致】全部走 luaL_error（L2）。
// ⚠️ 但**深度校验在渲染层**：z/位置的有限性、UV 越界、失效句柄
// 都是渲染层用 `fail_boundary` 报的（L3）。两处不一样，原因是
// "引擎拒绝数据矛盾"那条判据落在渲染层，而参数形状（是不是数字）落在这一层。
// ⇒ 这个不一致是**既有事实**，全文件统一裁决归"错误分级"那一批。
// -----------------------------------------------------------------------------
int l_draw_image(lua_State* L) {
    auto& eng = engine_of(L);

 // ---- ⓪ 相位守卫（与 draw_vertices 同一条）----
    if (!eng.draws_allowed()) {
        return luaL_error(L,
                          "draw_image 在渲染相位之外被调用 —— "
                          "所有绘制必须发生在渲染相位的钩子里"
                          "（on_render_begin/main/pause/ext/end）");
    }

 // ---- ① 逐个参数：先只保证"是数字"（深度校验在渲染层）----
    struct { const char* name; int index; } args[] = {
        {"image", 1}, {"layer", 2}, {"z", 3}, {"x", 4}, {"y", 5},
        {"w", 6},     {"h", 7},
    };
    for (const auto& a : args) {
        if (!lua_isnumber(L, a.index)) {
            return luaL_error(L, "draw_image 的 %s 必须是数字，got %s", a.name,
                              luaL_typename(L, a.index));
        }
    }

 // image 是句柄：必须是精确的非负整数（小数/NaN/Inf ⇒ L3）
    const lua_Integer raw_image =
        read_handle_arg(L, 1, "draw_image 的 image");

 // ---- ② 可选取图矩形：要么四个都给，要么一个都不给 ----
 //
 // 【为什么不允许"给一半"】半个矩形没有语义，而引擎最不该做的事就是
 // 猜剩下的那两个（默认 0？默认整张图？两种都说得通 ⇒ 两种都不该选）。
 // ⇒ 有/无是**一个**决定，四个数必须一起出现。
 //
 // 【级别】它是调用者给的数据 ⇒ boundary_validation ⇒ L3（可被 pcall 接住）。
 // 这正是 要验证的东西：**新参数的校验直接用新四级约定**。
    wag::draw::source_rect src{};
    bool has_src = false;
    {
        const int top = lua_gettop(L);
        if (top > 7) {
            if (top != 11) {
                wag::error_value e;
                e.cls = wag::error_class::boundary_validation;
                e.where = "draw_image 的参数个数";
                const std::string got = std::to_string(top) + " 个";
                e.got = got;
                e.want = "7 个（画整张图）或 11 个（多给 u, v, w, h）";
                e.sentence = "draw_image 的取图矩形要么四个都给、要么一个都不给";
                wag::fail_boundary(e);
            }
            struct { const char* name; int index; } rect[] = {
                {"u", 8}, {"v", 9}, {"w", 10}, {"h", 11},
            };
            for (const auto& a : rect) {
                if (!lua_isnumber(L, a.index)) {
                    wag::error_value e;
                    e.cls = wag::error_class::boundary_validation;
                    char where_buf[64];
                    std::snprintf(where_buf, sizeof(where_buf),
                                  "draw_image 的取图矩形 %s", a.name);
                    e.where = where_buf;
                    e.got = luaL_typename(L, a.index);
                    e.want = "一个数字（纹理像素）";
                    wag::fail_boundary(e);
                }
            }
            src.u = static_cast<double>(lua_tonumber(L, 8));
            src.v = static_cast<double>(lua_tonumber(L, 9));
            src.w = static_cast<double>(lua_tonumber(L, 10));
            src.h = static_cast<double>(lua_tonumber(L, 11));
            has_src = true;
        }
    }

    eng.renderer().draw_image(static_cast<std::uint64_t>(raw_image),
                              lua_tointeger(L, 2),
                              static_cast<double>(lua_tonumber(L, 3)),
                              static_cast<double>(lua_tonumber(L, 4)),
                              static_cast<double>(lua_tonumber(L, 5)),
                              static_cast<double>(lua_tonumber(L, 6)),
                              static_cast<double>(lua_tonumber(L, 7)),
                              has_src ? &src : nullptr);
    return 0;
}

// -----------------------------------------------------------------------------
// wag.draw.draw_quad(image, layer, z, x1,y1,z1, x2,y2,z2, x3,y3,z3, x4,y4,z4)
// —— 四任意点 + **整张图** UV（的"纯位置版"）
//
// 【它不是新管线】引擎侧只做"参数 → 4 个顶点"，然后走 `collect_quad`
//（与 `draw_image` 同一个实现）⇒ 顶点校验 / UV / 变换 / 收集只有一份。
//
// 【逐顶点的 z：接受但不参与渲染】深度测试关闭；排序键是参数 `layer` / `z`。
// 保留这三个数是为了源系统的 `Render4V` 调用能**原样搬过来**。
//
// 【形状：恰好 4 个顶点】更多顶点用 `draw_vertices`；
// 要逐顶点 UV / 逐顶点颜色也用 `draw_vertices`。
// -----------------------------------------------------------------------------
int l_draw_quad(lua_State* L) {
    auto& eng = engine_of(L);

    if (!eng.draws_allowed()) {
        return luaL_error(L,
                          "draw_quad 在渲染相位之外被调用 —— "
                          "所有绘制必须发生在渲染相位的钩子里"
                          "（on_render_begin/main/pause/ext/end）");
    }

 // ---- ① image / layer / z ----
    const lua_Integer raw_image = read_handle_arg(L, 1, "draw_quad 的 image");
    if (!lua_isnumber(L, 2)) {
        return luaL_error(L, "draw_quad 的 layer 必须是整数，got %s",
                          luaL_typename(L, 2));
    }
    if (!lua_isnumber(L, 3)) {
        return luaL_error(L, "draw_quad 的 z 必须是数字，got %s",
                          luaL_typename(L, 3));
    }
    const std::int64_t layer = lua_tointeger(L, 2);
    const double z = static_cast<double>(lua_tonumber(L, 3));

 // ---- ② 恰好 12 个数 ----
    const int n = lua_gettop(L) - 3;
    if (n != 12) {
        return luaL_error(L,
                          "draw_quad 需要 12 个数字（x1,y1,z1 … x4,y4,z4），got %d 个参数",
                          n);
    }
    double pts[12];
    for (int k = 0; k < 12; ++k) {
        const int idx = 4 + k;
        if (!lua_isnumber(L, idx)) {
            return luaL_error(L, "draw_quad 的第 %d 个数必须是数字，got %s",
                              k + 1, luaL_typename(L, idx));
        }
        pts[k] = static_cast<double>(lua_tonumber(L, idx));
    }

    eng.renderer().draw_quad(static_cast<std::uint64_t>(raw_image), layer, z,
                             pts);
    return 0;
}

// -----------------------------------------------------------------------------
// wag.draw.draw_vertices(image, v1, ..., vn)
//
// 【Lua 侧签名】image = 资源句柄（数字）；其后 3..32 个顶点表
// { x, y, z, u, v, color }
// 照抄 lstg.RenderTexture 的顶点结构；**顶点数 = 参数个数**
// ⇒ "n 与数组长度不符"这个错误在用户面不存在（渲染决策二 2.5）。
//
// 【本函数的错误全部走 luaL_error（Lua 错误），不走 fail_boundary —— 已决】
// 2.6："void；出错走 Lua 错误"。luaL_error 是
// longjmp ⇒ 被 call_hook 的 lua_pcall 接住 ⇒ 按"脚本自身的错"（L2）
// 处理：记录 + 弹窗，然后继续推进帧结构
// ⇒ "一个错误的顶点最多让画面少一块"。
// ⚠️ 与本文件既有注册点（fail_boundary 直接终止）不一致 —— 那些先于
// 这条决策写下；全文件统一裁决归"错误分级"那一批。
//
// 【校验顺序】与渲染层 draw_vertices 的四步同构：
// ① 上限（读任何顶点表之前）→ ② 退化（<3 静默）→ ③ 逐表读出与类型
// 校验 → ④ 整批有限值（复用 find_vertex_defect —— 只有一份）。
// ⇒ 读出全部进**栈上暂存**、最后才交给渲染层：中途任何错误抛出时，
// 唯一发生的事是暂存被丢弃，引擎状态零接触 —— 事务性。
// =============================================================================
// =============================================================================
// wag.draw.draw_glyphs —— 文字提交原语（2026-11-16，阶段1）
//
// 【它是什么】把 n 个**已经排好的**字形四边形一次提交。
// 名字里有 "glyph"，但**引擎内部的行为不带字形语义**
//：不排版、不换行、不查字体。
//
// 【参数形态】扁平平元数组，每图元 **9 个数**：
// x, y, w, h, u, v, uw, uh, color
// 与渲染器的 draw_glyphs 声明一致（renderer.hpp）。
//
// 【错误全走 luaL_error】与 draw_vertices 同一条缝：
// 脚本侧校验失败 ⇒ Lua 错误 ⇒ 被 hook 的 pcall 接住 ⇒ L2（帧继续）。
// ⇒ 这里**先把渲染层会查的全部查一遍**，保证 fail_boundary 不可达。
//
// 【为什么要一份暂存数组】渲染层只认 `const double*`（它不认识 Lua）。
// 暂存取**函数内静态**并复用：一次分配，之后不再分配。
// =============================================================================
int l_draw_glyphs(lua_State* L) {
    auto& eng = engine_of(L);

 // ---- ⓪ 相位守卫（与 draw_image / draw_vertices 同一条）----
    if (!eng.draws_allowed()) {
        return luaL_error(L,
                          "draw_glyphs 在渲染相位之外被调用 —— "
                          "所有绘制必须发生在渲染相位的钩子里"
                          "（on_render_begin/main/pause/ext/end）");
    }

 // ---- ① 前三个参数 ----
    const lua_Integer raw_image =
        read_handle_arg(L, 1, "draw_glyphs 的 image");
    if (!lua_isnumber(L, 2)) {
        return luaL_error(L, "draw_glyphs 的 layer 必须是数字，got %s",
                          luaL_typename(L, 2));
    }
    if (!lua_isnumber(L, 3)) {
        return luaL_error(L, "draw_glyphs 的 z 必须是数字，got %s",
                          luaL_typename(L, 3));
    }
    const lua_Integer layer = lua_tointeger(L, 2);
    const double z = static_cast<double>(lua_tonumber(L, 3));

 // ---- ② 第四个参数：扁平数组 ----
    if (!lua_istable(L, 4)) {
        return luaL_error(L, "draw_glyphs 的第 4 个参数必须是数组，got %s",
                          luaL_typename(L, 4));
    }
    const std::size_t n = static_cast<std::size_t>(lua_objlen(L, 4));
    if (n % 9u != 0u) {
        return luaL_error(
            L, "draw_glyphs 的数组长度必须是 9 的倍数（每图元 x,y,w,h,u,v,uw,uh,color），got %zu",
            n);
    }
    if (n == 0u) {
        return 0; // 空数组 = 什么都不画（不是错误）
    }

 // ---- ③ 逐个数读出（类型 + 颜色范围；有限值交给渲染层同一份实现）----
    static std::vector<double> scratch;
    scratch.clear();
    scratch.reserve(n);
    for (std::size_t i = 1; i <= n; ++i) {
        lua_rawgeti(L, 4, static_cast<lua_Integer>(i));
        if (!lua_isnumber(L, -1)) {
            const char* tn = luaL_typename(L, -1);
            lua_pop(L, 1);
            return static_cast<int>(luaL_error(
                L, "draw_glyphs 的第 %zu 个数必须是数字，got %s", i, tn));
        }
        const double v = static_cast<double>(lua_tonumber(L, -1));
        lua_pop(L, 1);

 // 每 9 个的最后一个 = 颜色（0xAARRGGBB），必须落在 uint32 范围。
        if ((i % 9u) == 0u && !(v >= 0.0 && v <= 4294967295.0)) {
            return static_cast<int>(luaL_error(
                L, "draw_glyphs 的第 %zu 个图元的 color 超出 ARGB 范围 [0, 4294967295]，got %g",
                i / 9u, v));
        }
        scratch.push_back(v);
    }

 // ---- ④ 交给渲染层（它再做一次整批有限值校验 —— 那是唯一的一份）----
    eng.renderer().draw_glyphs(static_cast<std::uint64_t>(raw_image), layer, z,
                               scratch.data(), scratch.size());
    return 0;
}

// =============================================================================
// wag.draw.set_state / get_state / reset_state —— 的"入口乙（当前状态）"
//
// 【为什么这一批先只开"混合状态"一维】
// 8 个混合状态是**固定功能**（改的只是 glBlendFuncSeparate / glBlendEquationSeparate）
// ⇒ 零着色器变体成本。而顶点色模式 / 纹理 alpha / 雾都要着色器变体，
// 随那批一起做。
// ⇒ 未实现的那三维**明确报错**，绝不静默忽略 ——
// "写进去没生效"是最坏的一类缺陷（作者会以为画面正常）。
//
// 【三态与粘性】见 renderer.hpp 的 draw_state_override：
// · 未设置 ⇒ 用该维默认（混合维默认 = Normal）
// · 已设置 ⇒ 一直生效，直到再改或 reset（**不**帧末重置）
// 【状态栈在 Lua 侧】引擎只保存这一个"当前值"。
// =============================================================================

#if defined(WAG_HAVE_LUAJIT)
namespace {

/// 把 8 个合法混合状态名拼成一段可读清单（报错时用）。
/// 写错名字必须响亮 —— 静默按 Normal 处理会让作者以为"混合没效果"。
const char* blend_state_catalog() {
    return "Normal / Add / AddNoAlpha / Sub / SubNoAlpha / RevSub / MulAlpha / MulColor";
}

/// 2 个纹理 alpha 类型的可读清单。
const char* tex_alpha_catalog() {
    return "Normal / Replace";
}

/// 4 个顶点色模式的可读清单。
const char* vertex_color_catalog() {
    return "Mul / Add / Sub / Alphablend";
}

/// wag.draw.set_state{ blend = "Add", ... }
/// 只认 `blend` 一个键；其余键（顶点色模式/纹理 alpha/雾）**明确报"尚未落地"**。
int l_draw_set_state(lua_State* L) {
    if (!lua_istable(L, 1)) {
        return luaL_error(L, "set_state 的参数必须是表，got %s",
                          luaL_typename(L, 1));
    }

    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
 // 键在 -2，值在 -3（键）之后是 -1
        if (!lua_isstring(L, -2)) {
            lua_pop(L, 2);
            return luaL_error(L, "set_state 的键必须是字符串");
        }
        const char* key = lua_tostring(L, -2);

 // ---- `texture_alpha`（2026-10-18 落地：2 档，进着色器变体）----
        if (std::strcmp(key, "texture_alpha") == 0) {
            if (!lua_isstring(L, -1)) {
                const char* tn = luaL_typename(L, -1);
                lua_pop(L, 2);
                return luaL_error(
                    L, "set_state 的 texture_alpha 必须是字符串，got %s（2 种：%s）",
                    tn, tex_alpha_catalog());
            }
            wag::draw::tex_alpha t{};
            if (!wag::draw::parse_tex_alpha(lua_tostring(L, -1), t)) {
                char got_copy[48];
                std::snprintf(got_copy, sizeof(got_copy), "%s", lua_tostring(L, -1));
                lua_pop(L, 2);
                return luaL_error(
                    L, "set_state 的 texture_alpha 不认识 %s；合法值（大小写敏感）：%s",
                    got_copy, tex_alpha_catalog());
            }
            engine_of(L).renderer().set_tex_alpha(t);
            lua_pop(L, 1);
            continue;
        }

 // ---- `vertex_color`（2026-10-18 落地：4 档，进着色器变体）----
        if (std::strcmp(key, "vertex_color") == 0) {
            if (!lua_isstring(L, -1)) {
                const char* tn = luaL_typename(L, -1);
                lua_pop(L, 2);
                return luaL_error(
                    L, "set_state 的 vertex_color 必须是字符串，got %s（4 种：%s）",
                    tn, vertex_color_catalog());
            }
            wag::draw::vertex_color_mode m{};
            if (!wag::draw::parse_vertex_color(lua_tostring(L, -1), m)) {
                char got_copy[48];
                std::snprintf(got_copy, sizeof(got_copy), "%s", lua_tostring(L, -1));
                lua_pop(L, 2);
                return luaL_error(
                    L, "set_state 的 vertex_color 不认识 %s；合法值（大小写敏感）：%s",
                    got_copy, vertex_color_catalog());
            }
            engine_of(L).renderer().set_vertex_color(m);
            lua_pop(L, 1);
            continue;
        }

 // ---- `shader`（2026-10-19）：句柄；0 或 nil = 回到内建变体 ----
 // 【2026-11-10 】句柄必须有限、整数、1..UINT32_MAX，且**仍有效**；
 // 否则 L3（：失效句柄使用 ⇒ L3）。绝不把无效句柄留到发射时
 // 静默回退到内建变体。
        if (std::strcmp(key, "shader") == 0) {
            if (lua_isnil(L, -1) || (lua_isnumber(L, -1) && lua_tonumber(L, -1) == 0.0)) {
                engine_of(L).renderer().reset_shader();
                lua_pop(L, 1);
                continue;
            }
            if (!lua_isnumber(L, -1)) {
                const char* tn = luaL_typename(L, -1);
                lua_pop(L, 2);
                return luaL_error(L, "set_state 的 shader 必须是句柄或 nil，got %s", tn);
            }
            const double raw = static_cast<double>(lua_tonumber(L, -1));
            char got_buf[64];
            if (!std::isfinite(raw) || raw < 1.0 || raw > 4294967295.0 ||
                std::floor(raw) != raw) {
                std::snprintf(got_buf, sizeof(got_buf), "shader=%g", raw);
                error_value e;
                e.cls = error_class::boundary_validation;
                e.where = "set_state 的 shader";
                e.got = got_buf;
                e.want = "一个正整数句柄（1..4294967295），或 0/nil 表示内建变体";
                e.sentence = "set_state 的 shader 不是合法句柄";
                fail_boundary(e);
            }
            const std::uint32_t handle = static_cast<std::uint32_t>(raw);
            if (!engine_of(L).renderer().valid_shader_handle(handle)) {
                std::snprintf(got_buf, sizeof(got_buf), "handle=%u",
                              static_cast<unsigned>(handle));
                error_value e;
                e.cls = error_class::boundary_validation;
                e.where = "set_state 的 shader";
                e.got = got_buf;
                e.want = "一个仍有效的 load_shader 句柄（0/nil = 内建）；"
                         "卸载或同名覆盖后旧句柄失效";
                e.sentence = "使用失效的着色器句柄";
                fail_boundary(e);
            }
            engine_of(L).renderer().set_shader(handle);
            lua_pop(L, 1);
            continue;
        }

 // ---- `shader_params`（2026-10-19）：4 个浮点，进 `u_params`（vec4）----
        if (std::strcmp(key, "shader_params") == 0) {
            if (!lua_istable(L, -1)) {
                const char* tn = luaL_typename(L, -1);
                lua_pop(L, 2);
                return luaL_error(L, "set_state 的 shader_params 必须是表，got %s", tn);
            }
            float p[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int k = 1; k <= 4; ++k) {
                lua_rawgeti(L, -1, k);
                if (lua_isnumber(L, -1)) {
                    p[k - 1] = static_cast<float>(lua_tonumber(L, -1));
                }
                lua_pop(L, 1);
            }
            engine_of(L).renderer().set_shader_params(p);
            lua_pop(L, 1);
            continue;
        }

 // ---- `fog`（2026-10-18 落地：线性雾，只贡献 1 位变体）----
 //
 // 形态：`fog = false`（显式关闭）/ `fog = { color=, near=, far=, strength= }`
 // 【为什么雾用表、其余维用字符串】其余三维的取值域是"几个名字"，
 // 雾的取值域是**一组连续参数**（：颜色/near/far/强度走 uniform）
 // ⇒ 名字表达不了它，只能是一个参数表。
 // 【四个字段都可省】缺省：color=0x000000（黑）、near=0、far=1000、strength=1。
 // ⚠️ 省字段不是"推荐用法"：`near == far` 会让公式除零，
 // 所以作者应当显式给 near/far。
        if (std::strcmp(key, "fog") == 0) {
            if (lua_isboolean(L, -1)) {
                wag::draw::fog_state f{};
                f.enabled = (lua_toboolean(L, -1) != 0);
                engine_of(L).renderer().set_fog(f);
                lua_pop(L, 1);
                continue;
            }
            if (!lua_istable(L, -1)) {
                const char* tn = luaL_typename(L, -1);
                lua_pop(L, 2);
                return luaL_error(
                    L,
                    "set_state 的 fog 必须是表或布尔，got %s"
                    "（表字段：color / near / far / strength）",
                    tn);
            }
            wag::draw::fog_state f{};
            f.enabled = true;
            const int tbl = lua_gettop(L);

            lua_getfield(L, tbl, "color");
            if (lua_isnumber(L, -1)) {
                const std::uint32_t argb =
                    static_cast<std::uint32_t>(lua_tointeger(L, -1));
                f.color[0] = static_cast<float>(((argb >> 16) & 0xFFu) / 255.0);
                f.color[1] = static_cast<float>(((argb >> 8) & 0xFFu) / 255.0);
                f.color[2] = static_cast<float>((argb & 0xFFu) / 255.0);
            }
            lua_pop(L, 1);

            lua_getfield(L, tbl, "near");
            if (lua_isnumber(L, -1)) {
                f.near_z = lua_tonumber(L, -1);
            }
            lua_pop(L, 1);

            lua_getfield(L, tbl, "far");
            if (lua_isnumber(L, -1)) {
                f.far_z = lua_tonumber(L, -1);
            }
            lua_pop(L, 1);

            lua_getfield(L, tbl, "strength");
            if (lua_isnumber(L, -1)) {
                f.strength = lua_tonumber(L, -1);
            }
            lua_pop(L, 1);

            engine_of(L).renderer().set_fog(f);
            lua_pop(L, 1);
            continue;
        }

        if (std::strcmp(key, "blend") != 0) {
 // 【明确报错，不忽略】到这里说明四个维都不匹配
            const char* tn = luaL_typename(L, -1);
            lua_pop(L, 2);
            return luaL_error(
                L,
                "set_state 不认识的键 %s（got %s）；当前支持 blend（8 种：%s）/ "
                "texture_alpha（2 种：%s）/ vertex_color（4 种：%s）/ fog（表）",
                key, tn, blend_state_catalog(), tex_alpha_catalog(),
                vertex_color_catalog());
        }
        if (!lua_isstring(L, -1)) {
            const char* tn = luaL_typename(L, -1);
            lua_pop(L, 2);
            return luaL_error(L, "set_state 的 blend 必须是字符串，got %s（8 种：%s）",
                              tn, blend_state_catalog());
        }
        wag::draw::blend_state s{};
        if (!wag::draw::parse_blend_state(lua_tostring(L, -1), s)) {
            const char* got = lua_tostring(L, -1);
            char got_copy[48];
            std::snprintf(got_copy, sizeof(got_copy), "%s", got);
            lua_pop(L, 2);
            return luaL_error(L, "set_state 的 blend 不认识 %s；合法值（大小写敏感）：%s",
                              got_copy, blend_state_catalog());
        }
        engine_of(L).renderer().set_blend_state(s);
        lua_pop(L, 1); // 弹值，保留键给 lua_next
    }
    return 0;
}

/// wag.draw.get_state() -> { blend = "Add", texture_alpha = "Replace" }
/// **只返回已显式设置的维**（未设置的维不出现）
int l_draw_get_state(lua_State* L) {
    lua_newtable(L);
    auto& r = engine_of(L).renderer();
    wag::draw::blend_state s{};
    if (r.get_blend_state(s)) {
        lua_pushstring(L, wag::draw::blend_state_name(s));
        lua_setfield(L, -2, "blend");
    }
    wag::draw::tex_alpha t{};
    if (r.get_tex_alpha(t)) {
        lua_pushstring(L, wag::draw::tex_alpha_name(t));
        lua_setfield(L, -2, "texture_alpha");
    }
    wag::draw::vertex_color_mode m{};
    if (r.get_vertex_color(m)) {
        lua_pushstring(L, wag::draw::vertex_color_name(m));
        lua_setfield(L, -2, "vertex_color");
    }
    wag::draw::fog_state f{};
    if (r.get_fog(f) && f.enabled) {
        lua_newtable(L);
        const std::uint32_t argb =
            (static_cast<std::uint32_t>(f.color[0] * 255.0f + 0.5f) << 16) |
            (static_cast<std::uint32_t>(f.color[1] * 255.0f + 0.5f) << 8) |
            static_cast<std::uint32_t>(f.color[2] * 255.0f + 0.5f);
        lua_pushinteger(L, argb);
        lua_setfield(L, -2, "color");
        lua_pushnumber(L, f.near_z);
        lua_setfield(L, -2, "near");
        lua_pushnumber(L, f.far_z);
        lua_setfield(L, -2, "far");
        lua_pushnumber(L, f.strength);
        lua_setfield(L, -2, "strength");
        lua_setfield(L, -2, "fog");
    }
    return 1;
}

/// wag.draw.reset_state([name]) —— 无参 = 清全部；给名字 = 只清那一维
///
/// 【它回到的是"未设置"，不是"设成 Normal"】两者在行为上今天等价，
/// 但概念不同：未设置意味着"将来容器落地后，跟着容器字段走"。
int l_draw_reset_state(lua_State* L) {
    auto& r = engine_of(L).renderer();
    if (lua_gettop(L) >= 1 && lua_isstring(L, 1)) {
        const char* key = lua_tostring(L, 1);
        if (std::strcmp(key, "blend") == 0) {
            r.reset_blend_state();
            return 0;
        }
        if (std::strcmp(key, "texture_alpha") == 0) {
            r.reset_tex_alpha();
            return 0;
        }
        if (std::strcmp(key, "vertex_color") == 0) {
            r.reset_vertex_color();
            return 0;
        }
        if (std::strcmp(key, "fog") == 0) {
            r.reset_fog();
            return 0;
        }
        return luaL_error(L, "reset_state 不认识 %s；当前可清 blend / texture_alpha / vertex_color / fog", key);
    }
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
        return luaL_error(L, "reset_state 的参数必须是字符串或 nil，got %s",
                          luaL_typename(L, 1));
    }
    r.reset_blend_state();
    r.reset_tex_alpha();
    r.reset_vertex_color();
    r.reset_fog();
    return 0;
}

} // namespace
#endif

// -----------------------------------------------------------------------------
int l_draw_vertices(lua_State* L) {
    auto& eng = engine_of(L);

 // ---- ⓪ 相位守卫（2026 绘序管线）：绘制只发生在渲染相位的钩子里 ----
 // 闸外调用 = 接口误用（L2：该调用不生效，帧结构继续）。
 // "引擎只在渲染相位收集"由帧结构的开/关闸结构性地保证；
 // 这里的检查让脚本拿到一条可读的错误，而不是一笔无声消失的绘制。
    if (!eng.draws_allowed()) {
        return luaL_error(L,
                          "draw_vertices 在渲染相位之外被调用 —— "
                          "所有绘制必须发生在渲染相位的钩子里"
                          "（on_render_begin/main/pause/ext/end）");
    }

 // ---- ① image：资源句柄（数字）----
 // 【image 现在有消费者了（2026-10-18）】它被解析成纹理 + 尺寸，
 // 尺寸用来把**像素 UV** 归一化（`u_tex_inv_size`），纹理进批状态
 //（换纹理 ⇒ flush）。`0` 表示"白纹理"（纯顶点色绘制）。
 // 类型契约现在就定：非负数字。"名字在赋值时解析成句柄"的资源系统
 // 落地后，句柄从那一侧取得，本参数不变。
 // 句柄必须是精确的非负整数（小数/NaN/Inf ⇒ L3）。
    const lua_Integer raw_image =
        read_handle_arg(L, 1, "draw_vertices 的 image");
    const std::uint64_t image = static_cast<std::uint64_t>(raw_image);

 // ---- ①b 绘序键：layer（整数层）与 z（层内顺序键）----
 // 2026 定案：
 // · layer 整数，分组键，更大者后画（在上）
 // · z 浮点，顺序键，更大者更靠前=后画=在上；精确相等才可合并
 // 【按调用给出】"每次绘制各带自己的 layer/z/状态"。
    if (!lua_isnumber(L, 2)) {
        return luaL_error(L, "draw_vertices 的 layer 必须是整数，got %s",
                          luaL_typename(L, 2));
    }
    if (!lua_isnumber(L, 3)) {
        return luaL_error(L, "draw_vertices 的 z 必须是数字，got %s",
                          luaL_typename(L, 3));
    }
    const std::int64_t layer = lua_tointeger(L, 2);
    const double z = static_cast<double>(lua_tonumber(L, 3));

 // ---- ② 上限：必须在读任何顶点表之前（已定）----
 // 否则一个越界的 count 会先把海量数据读完才发现放不下。
    const int count = lua_gettop(L) - 3; // 顶点数 = 参数个数 − image − layer − z
    if (count > static_cast<int>(wag::draw::shape_vertex_limit)) {
        return luaL_error(
            L,
            "draw_vertices 的单笔图元顶点数是 %d，超过上限 %d"
            "（溢出报错，不自动拆分 —— 批边界由调用方决定）",
            count, static_cast<int>(wag::draw::shape_vertex_limit));
    }

 // ---- ③ 退化输入：不足 3 个顶点围不成面 ⇒ 静默（已定，不报错）----
    if (count < 3) {
        return 0;
    }

 // ---- ④ 逐表读出（类型校验在这里；有限值在下一步统一查）----
    wag::draw::vertex gpu[wag::draw::shape_vertex_limit];
    for (int i = 0; i < count; ++i) {
        const int tbl = 4 + i; // 第 i 个顶点表在栈上的绝对索引
        if (!lua_istable(L, tbl)) {
            return luaL_error(L, "draw_vertices 的第 %d 个顶点必须是表，got %s",
                              i + 1, luaL_typename(L, tbl));
        }
        gpu[i].x = read_vertex_number(L, tbl, "x", i);
        gpu[i].y = read_vertex_number(L, tbl, "y", i);
        gpu[i].z = read_vertex_number(L, tbl, "z", i);
        gpu[i].u = read_vertex_number(L, tbl, "u", i);
        gpu[i].v = read_vertex_number(L, tbl, "v", i);

 // 颜色：ARGB 的 u32（0xAARRGGBB）。Lua 侧以位运算构造，到这里是
 // 一个 double ⇒ 范围必须落在 [0, 2^32−1]（该值 double 下精确）。
 // 【为什么查范围】越界的颜色不是"看起来没用"，是数据矛盾；
 // 静默截断会把一个写错的颜色悄悄变成另一个颜色 —— 最难查的那类。
        lua_getfield(L, tbl, "color");
        if (!lua_isnumber(L, -1)) {
            const char* tn = luaL_typename(L, -1);
            lua_pop(L, 1);
            return static_cast<int>(luaL_error(
                L, "draw_vertices 的第 %d 个顶点缺少数字字段 color，got %s",
                i + 1, tn));
        }
        const double raw_color = static_cast<double>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        if (!(raw_color >= 0.0 && raw_color <= 4294967295.0)) {
            return luaL_error(
                L,
                "draw_vertices 的第 %d 个顶点的 color 超出 ARGB 范围"
                " [0, 4294967295]，got %g",
                i + 1, raw_color);
        }
        gpu[i].color = static_cast<std::uint32_t>(raw_color);
    }

 // ---- ⑤ 整批有限值校验（复用渲染层的实现 —— 校验逻辑只有一份）----
    wag::draw::vertex_defect d;
    if (eng.renderer().find_vertex_defect(gpu, static_cast<std::size_t>(count),
                                          d)) {
        return luaL_error(L,
                          "draw_vertices 第 %d 个顶点的 %c 不是有限值"
                          "（got = %g）—— 整批不发",
                          static_cast<int>(d.index) + 1, d.field, d.value);
    }

 // ---- ⑥ 提交（校验全部结束 ⇒ 此后无失败路径）----
 // 【渲染相位守卫是显式的待定项】已定"所有绘制都发生在渲染相位的钩子里"
 //，而"引擎在非渲染相位拒绝绘制"需要帧结构
 // 把当前阶段暴露给原语层 —— 那条接口尚未立项。
 // 第一批的作者是我们自己；且在逻辑钩子里画的任何东西都会被下一帧的
 // 清屏盖掉（GL 状态每帧由 begin_frame 重设），不产生不可恢复的污染。
 // 守卫落地时，它应该长在本函数的开头（校验段之前）。
    eng.renderer().draw_vertices(image, layer, z, gpu,
                                 static_cast<std::size_t>(count));
    return 0;
}

// -----------------------------------------------------------------------------
// wag.draw.draw_mesh(image, layer, z, v1, ..., vn)
// —— 任意顶点列表 + 固定拓扑（三角形列表）
//
// 【Lua 侧签名】`image` 图片句柄（**0 = 白纹理**）；`layer` 整数、`z` 浮点；
// 其后是顶点表，格式与 draw_vertices 完全相同（{ x, y, z, u, v, color }；
// page 随纹理数组批加入）。
// ⚠️ **image 是必需的**：UV 归一化要除以纹理尺寸，而尺寸只能从图片句柄查到
//（见 renderer.hpp 的 `u_tex_inv_size`）。没有它，网格就贴不了图 ——
// 而"给一张图片做非仿射变形"正是这个入口的主要用途。
// 单次上限 **65535**（= 2^16 - 1，顶点计数落在 u16 内），超限报错不截断。
// 不是 3 的倍数 ⇒ 末尾不足 3 个的顶点围不成三角形，**静默忽略**
//（与 draw_vertices 的退化输入同一条判据）。
//
// 【它仍是纯几何】引擎只吃顶点，不认"网格结构"：不要求规则网格、不要求
// 连通、不要求顶点共享 ⇒ 引擎侧没有任何"网格语义"
//（与 draw_glyphs 不认字形语义同族；"网格"只是名字带给作者的说法）。
//
// 【批次与事务】与 draw_vertices 同一条：校验（含全量有限值）先完整走完
// 才开始入批 ⇒ 一次调用可能跨多次 flush，但非法数据一笔都不发出。
// -----------------------------------------------------------------------------
int l_draw_mesh(lua_State* L) {
    auto& eng = engine_of(L);

 // ---- ⓪ 相位守卫（同 l_draw_vertices 的说明）----
    if (!eng.draws_allowed()) {
        return luaL_error(L,
                          "draw_mesh 在渲染相位之外被调用 —— "
                          "所有绘制必须发生在渲染相位的钩子里"
                          "（on_render_begin/main/pause/ext/end）");
    }

 // ---- ① 图片句柄（UV 归一化要除以纹理尺寸 ⇒ 必须先拿到纹理）----
 // 0 = 白纹理（纯顶点色网格）。非 0 ⇒ 由渲染层校验代数并解析出纹理与尺寸。
 // 句柄必须是精确的非负整数（小数/NaN/Inf ⇒ L3）。
    const lua_Integer raw_image =
        read_handle_arg(L, 1, "draw_mesh 的 image");
    const std::uint64_t image = static_cast<std::uint64_t>(raw_image);

 // ---- ② 绘序键：layer 与 z（语义见 l_draw_vertices 处的引用）----
    if (!lua_isnumber(L, 2)) {
        return luaL_error(L, "draw_mesh 的 layer 必须是整数，got %s",
                          luaL_typename(L, 2));
    }
    if (!lua_isnumber(L, 3)) {
        return luaL_error(L, "draw_mesh 的 z 必须是数字，got %s",
                          luaL_typename(L, 3));
    }
    const std::int64_t layer = lua_tointeger(L, 2);
    const double z = static_cast<double>(lua_tonumber(L, 3));

 // ---- ③ 上限：必须在读任何数据之前 ----
    const int count = lua_gettop(L) - 3; // 顶点数 = 参数个数 − image − layer − z
    if (count > static_cast<int>(wag::draw::mesh_vertex_limit)) {
        return luaL_error(
            L,
            "draw_mesh 的顶点个数是 %d，超过上限 %d"
            "（= 2^16 - 1；超限报错，不静默截断）",
            count, static_cast<int>(wag::draw::mesh_vertex_limit));
    }

 // ---- ④ 退化输入：不足 3 顶点围不成三角形 ⇒ 静默 ----
    if (count < 3) {
        return 0;
    }

 // ---- ⑤ 逐表读出（堆上暂存 —— 65535 × 24B ≈ 1.5 MB，不能放栈上）----
 // 【为什么 vector 而不是逐顶点入批】事务边界 = 一次调用：
 // 校验必须扫全量之后才能入批 ⇒ 先暂存、后校验、再分块入批。
 // vector 的 RAII 让中途 luaL_error（longjmp）不泄漏 —— 这是刻意
 // 接受的一处例外（1.5 MB 不能上栈），见 renderer.hpp 的栈纪律注记。
    std::vector<wag::draw::vertex> verts;
    verts.reserve(static_cast<std::size_t>(count));
    for (int i = 4; i <= count + 3; ++i) {
        if (!lua_istable(L, i)) {
            return luaL_error(L, "draw_mesh 的第 %d 个顶点必须是表，got %s",
                              i - 3, luaL_typename(L, i));
        }
        wag::draw::vertex v;
        v.x = read_vertex_number(L, i, "x", i - 4);
        v.y = read_vertex_number(L, i, "y", i - 4);
        v.z = read_vertex_number(L, i, "z", i - 4);
        v.u = read_vertex_number(L, i, "u", i - 4);
        v.v = read_vertex_number(L, i, "v", i - 4);

        lua_getfield(L, i, "color");
        if (!lua_isnumber(L, -1)) {
            const char* tn = luaL_typename(L, -1);
            lua_pop(L, 1);
            return static_cast<int>(luaL_error(
                L, "draw_mesh 的第 %d 个顶点缺少数字字段 color，got %s",
                i - 3, tn));
        }
        const double raw_color = static_cast<double>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        if (!(raw_color >= 0.0 && raw_color <= 4294967295.0)) {
            return luaL_error(
                L,
                "draw_mesh 的第 %d 个顶点的 color 超出 ARGB 范围"
                " [0, 4294967295]，got %g",
                i - 3, raw_color);
        }
        v.color = static_cast<std::uint32_t>(raw_color);
        verts.push_back(v);
    }

 // ---- ⑥ 整批有限值校验（复用渲染层的实现 —— 校验逻辑只有一份）----
    wag::draw::vertex_defect d;
    if (eng.renderer().find_vertex_defect(verts.data(), verts.size(), d)) {
        return luaL_error(L,
                          "draw_mesh 第 %d 个顶点的 %c 不是有限值"
                          "（got = %g）—— 整批不发",
                          static_cast<int>(d.index) + 1, d.field, d.value);
    }

 // ---- ⑦ 提交（校验全部结束 ⇒ 此后无失败路径；可能跨多次 flush）----
    eng.renderer().draw_mesh(image, layer, z, verts.data(), verts.size());
    return 0;
}
} // namespace
#endif

// =============================================================================
// wag.log / wag.fail —— Lua 侧的错误架构两扇门
//
// 【为什么是两扇门，而不是一个带级别的接口】
// 记日志：**不打断**（"发生过什么"）
// 抛错误：**打断当前调用**，构造一个带上下文的故障值交给引擎
// 这两件事的性质不同，合成一个会让"我要记一条"与"我要报一个错"没法分辨。
//
// 【级别为什么是参数，而不是"每级一个函数"】
// C++ 侧的 `fail_boundary` / `fail_script` / `fail_shutdown` **各自硬编码级别**,
// 于是 `class_severity()` 那张决策表成了装饰 —— 这正是"类别没有参与决策"
// 的机制本体。新入口若再按"每级一个函数"，等于把那个错误照抄一遍。
// ⇒ 所以这里级别是**参数**，而级别→行为的映射仍然只有一处（log 的决策点）。
//
// 【它落到哪几条 C++ 入口】
// 脚本侧报的故障，级别由 `level` 字段表达，落到 C++ 侧时**逐级分流**：
// L1/L2 ⇒ `log::record` / `log::warn`（**调用方要求记一条**，不是错误）
// L3 ⇒ `fail_boundary`（有落点就抛）
// L4 ⇒ `fail_host`（无条件终止）
// 【E10】原语层**不许**再借 `fail_api_misuse`（那是 L2 的兜底入口）：
// L2 的"继续"在原语层必然带着一个错的值返回 —— 那正是"宽容版"。
// 四条路都复用同一份字段拼装（`error::format_error`），所以文本形状一致。
// =============================================================================

#if defined(WAG_HAVE_LUAJIT)
namespace {

/// 把一个 severity 常量挂进 `wag.log.level`。
void set_level(lua_State* L, const char* name, log::severity sev) {
    lua_pushinteger(L, static_cast<lua_Integer>(sev));
    lua_setfield(L, -2, name);
}

/// 读表里一个字符串字段，返回它的一份**视图**（指向 Lua 栈上的字符串）。
///
/// 【所有权】这是一次**借用**：字符串仍在 Lua 栈上，调用方必须在本函数返回后
/// 立刻把视图用掉（拷进 error_value 并交给报错入口），中间不得再对 Lua 栈
/// 做任何操作。这与已定的"Lua → C++ 借用"方向一致。
///
/// 【为什么可以在同一作用域里调四次】每次调用都把临时值弹掉，因此四次调用
/// 之后栈是平衡的；而四个视图指向的字符串都还活着（它们由 Lua 管理，
/// 只要那个表还在，字符串就不会被回收）。
std::string_view borrow_string(lua_State* L, int table_idx, const char* field) {
    lua_getfield(L, table_idx, field);
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        return {};
    }
    const char* s = lua_tostring(L, -1);
    lua_pop(L, 1);
    return std::string_view{s};
}

// ---- wag.log.* 的实现 ----
//
// 【为什么既要有 `record(level, ...)` 又要有 `info` / `warn`】
// `record` 是**通用**入口（级别是参数 ⇒ 满足"级别不编码在函数名里"那条），
// 而 `info` / `warn` 只是它的两个**便捷别名** —— 让最常见的两种调用短一点。
// ⇒ 省掉的那一层是 Lua 侧的包装，**C++ 侧仍然只有一条路**（record_message）。
//
// 【它们各自对应哪个严重级别】
// info → L1（record）：正常的过程记录
// warn → L2（warn）：可降级的问题（调用方写错了、资源缺了但能继续）
// L3（raise）/ L4（terminate）**都没有便捷别名**：那两个都是"这一次/这一个进程
// 到此为止"的动作，应当是显眼的，而不是一个顺手就能打出来的短名字。
namespace {

/// 记一条日志的共享实现。level 已经过范围检查。
void log_one(lua_State* L, int text_idx, int where_idx, log::severity sev) {
 // 【为什么在这里 scrub】脚本完全可能把 pcall 的错误串原样打出来，而 Lua 的
 // chunkid 是在 raise **之后**才加上 `...<路径尾巴>` 的 —— 那一段不经过
 // fail 路径的 scrub。这里是**所有 `wag.log.*` 的唯一汇合点**，在这里洗一次，
 // "日志里不出现绝对路径 / 半截目录名"才算真的成立。
    const std::string text_clean = wag::paths::scrub(luaL_checkstring(L, text_idx));

    const char* where = nullptr;
    std::string where_clean;
    if (!lua_isnoneornil(L, where_idx)) {
        where = luaL_checkstring(L, where_idx);
        where_clean = wag::paths::scrub(where);
    }

    if (where != nullptr) {
        log::record_message(sev, "script", log::format(text_clean, where_clean));
    } else {
        log::record_message(sev, "script", text_clean);
    }
}

} // namespace

// ---- wag.log.emit(level, text, where?) ----
//
// 【E14：为什么叫 emit 而不是 record】
// 原来它叫 `wag.log.record`，而 `wag.log.level` 里也有一个 `record`（L1 常量）。
// ⇒ 同一个名字在一个表里既是"函数"又是"级别常量旁边的键"，读起来必然错一次。
// ⇒ 函数改成 `emit`（"发一条"这个动作），**级别常量 `level.record` 不动**。
int l_log_emit(lua_State* L) {
 // 级别：只接受整数。**不接受字符串** —— 拼错级别会当场失败，
 // 而不是被当成"未知级别"静默记成别的档。
    const lua_Integer raw = luaL_checkinteger(L, 1);
 // 【E1：不再写死 1..3】范围由枚举两端导出，加一档不必回来改这里。
    constexpr lua_Integer kMinLevel =
        static_cast<lua_Integer>(log::severity::record);
    constexpr lua_Integer kMaxLevel =
        static_cast<lua_Integer>(log::severity::terminate);
    if (raw < kMinLevel || raw > kMaxLevel) {
        wag::error_value e;
        e.cls = wag::error_class::boundary_validation;
        e.where = "wag.log.emit 的 level";
        const std::string got = "不在 " + std::to_string(kMinLevel) + ".." +
                                std::to_string(kMaxLevel);
        e.got = got;
        e.want = "wag.log.level 里的常量";
 // 【E10：原语层不许走 L2】"越界的 level"是调用者的数据 ⇒ 该抛。
        wag::fail_boundary(e);
    }
    log_one(L, 2, 3, static_cast<log::severity>(raw));
    return 0;
}

// ---- wag.log.info(text, where?) —— L1 的便捷别名 ----
int l_log_info(lua_State* L) {
    log_one(L, 1, 2, log::severity::record);
    return 0;
}

// ---- wag.log.warn(text, where?) —— L2 的便捷别名 ----
int l_log_warn(lua_State* L) {
    log_one(L, 1, 2, log::severity::warn);
    return 0;
}

// ---- wag.fail{ level?, where?, want?, got?, sentence? } ----
int l_fail(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

 // level：省略时按 L2（"调用方写错了"是最常见的情形）。
 //
 // 【不确定代码 · 待定】默认值取 L2 是否合适
 // 已定的立场是"错误分类别、类别决定级别"，而"默认 L2"等于引擎替调用方
 // 猜了一个类别。另一种写法是**要求必填**（省略即报错）。
 // 倾向：先按可选 + 默认 L2 接线（少一处必填），待"错误分级"那一批一并定。
 //
 // 【注意：这里不能用 lua_isinteger】
 // 它是 **Lua 5.3+** 的 API（5.3 才有整数子类型），而 LuaJIT 是 **Lua 5.1
 // 语义** ⇒ 那个函数不存在，5.1 里所有 number 都是 double。
 // ⇒ 所以只问"是不是数"（LUA_TNUMBER），再取值。
    lua_Integer level = static_cast<lua_Integer>(log::severity::warn);
    lua_getfield(L, 1, "level");
    if (lua_type(L, -1) == LUA_TNUMBER) {
        level = lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    constexpr lua_Integer kMinLevel =
        static_cast<lua_Integer>(log::severity::record);
    constexpr lua_Integer kMaxLevel =
        static_cast<lua_Integer>(log::severity::terminate);
    if (level < kMinLevel || level > kMaxLevel) {
        wag::error_value e;
        e.cls = wag::error_class::boundary_validation;
        e.where = "wag.fail 的 level";
        const std::string got = "不在 " + std::to_string(kMinLevel) + ".." +
                                std::to_string(kMaxLevel);
        e.got = got;
        e.want = "wag.log.level 里的常量";
 // 【E10：原语层不许走 L2】同上
        wag::fail_boundary(e);
    }

 // 字段是借用（视图指向 Lua 栈上的字符串）⇒ 必须在同一个调用里用掉。
 //
 // 【2026-11-18 消毒】`got` / `sentence` 里常常是 `tostring(err)`，而 Lua 的
 // 错误消息里带着**源名**（模块是经 require 装载的，chunk 名仍是绝对路径）
 // ⇒ 这里统一洗一遍，避免绝对路径落进日志与 errors.jsonl。
 // 注意：洗过的结果是 std::string（自有存储），必须活过下面的 fail_* 调用。
    const std::string where_clean = wag::paths::scrub(borrow_string(L, 1, "where"));
    const std::string want_clean = wag::paths::scrub(borrow_string(L, 1, "want"));
    const std::string got_clean = wag::paths::scrub(borrow_string(L, 1, "got"));
    const std::string sentence_clean = wag::paths::scrub(borrow_string(L, 1, "sentence"));

    wag::error_value e;
    e.where = where_clean;
    e.want = want_clean;
    e.got = got_clean;
    e.sentence = sentence_clean;

 // 【四级分派：枚举位移之后这里必须逐级写清】
 // L1 (record) ⇒ 调用方**要求**记一条（不是错误）⇒ 按 L1 写，继续
 // L2 (warn) ⇒ 同上，按 L2 写，继续
 // L3 (raise) ⇒ fail_boundary：有落点就抛，没有就终止
 // L4 (terminate) ⇒ fail_host：**无条件**终止（脚本明确要求"这是致命的"）
 //
 // 【为什么 L1/L2 不再借 fail_api_misuse】E10：那个入口是 L2 的兜底，
 // 已把它赶出原语层。而 `wag.fail{level=warn}` 本来就**不是**错误，
 // 它是"要求记一条" ⇒ 直接按调用方要的级别写出去，语义反而更正。
 // 注意它还顺手修正了 level=record 的行为：以前也被记成 L2。
 //
 // 【为什么 L4 不能走 fail_boundary】E8 之后它会先看"有没有落点"，
 // 而 `wag.fail{level=terminate}` 的语义是"无论如何都别继续"——
 // 交给它就会变成"钩子里能 pcall 接住"，与调用方的意图相反。
    if (level == static_cast<lua_Integer>(log::severity::raise)) {
        e.cls = wag::error_class::boundary_validation;
 // `fail_boundary` 是 [[noreturn]] ⇒ 它之后不得再有语句。
        wag::fail_boundary(e);
    }

    if (level == static_cast<lua_Integer>(log::severity::terminate)) {
 // 【类别要说实话】这是"脚本自己声明致命"，不是引擎内部不变量被破坏。
 // `fail_host` 不看 cls 决定级别，但 errors.jsonl 会把它写进 `cls` 字段 ——
 // 留成默认的 internal_invariant 会让控制台把脚本的错误显示成"引擎 bug"。
        e.cls = wag::error_class::script_error;
 // `fail_host` 也是 [[noreturn]]
        wag::fail_host(e);
    }

 // 到这里只剩 L1 / L2：按调用方要的级别写一条，然后正常返回。
    if (level == static_cast<lua_Integer>(log::severity::record)) {
        log::record("script", wag::format_error(e));
    } else {
        log::warn("script", wag::format_error(e));
    }
    return 0;
}

} // namespace
#endif

// =============================================================================
// wag.hook / wag.debug
// =============================================================================
//
// wag.hook 是一张**可写的普通表**，由步骤 0 预置 13 个空函数。它不需要注册
// 任何 C 函数 —— 引擎按名从中取（见 hook.cpp 的 call_hook）。
//
// wag.debug.* 只在开发模式下可用。它的实现需要平台时钟与加载子系统，两者
// 都尚未落地。

// =============================================================================
// wag.image —— 图片族的入口（2026-10-18）
//
// 【为什么现在只开放这几个】
// 引擎侧目前只有"**一个图片 = 一张独立纹理**"这个最小形态（捕获与文件加载
// 都是它）。纹理族 / 图集子矩形 / 纹理数组（page）随 3b 落地后，
// `load_from_texture` 之类才会加进来。
// 【名字纪律】它是**图片族**的入口，所以前缀是 `wag.image.`；
// 不叫 `wag.draw.*` —— 资源不是绘制。
// =============================================================================

#if defined(WAG_HAVE_LUAJIT)
namespace {

/// 取一个必填字符串参数（名字 / 路径）。
const char* required_string(lua_State* L, int idx, const char* what,
                            const char* fn) {
    if (!lua_isstring(L, idx)) {
        luaL_error(L, "%s 的 %s 必须是字符串，got %s", fn, what,
                   luaL_typename(L, idx));
        return nullptr; // 不可达（luaL_error 是 longjmp），仅为消警告
    }
    return lua_tostring(L, idx);
}

/// wag.draw.load_shader(name, path) -> handle
///
/// 【片元来自文件，顶点着色器由引擎内嵌】（2026-10-19 已定）
/// 【路径由脚本给】：引擎不解析路径。
/// 【失败 = L3】编译/链接/读不到/槽满 ⇒ 终止（与图片一致）。
/// 【返回 0】只可能来自"未接 SDL"的构建；正常构建不会用 0 表示失败。
int l_draw_load_shader(lua_State* L) {
    auto& eng = engine_of(L);
    const char* name = required_string(L, 1, "name", "draw.load_shader");
    const char* path = required_string(L, 2, "path", "draw.load_shader");
    const std::uint32_t h = eng.renderer().load_shader(std::string(name), path);
    lua_pushnumber(L, static_cast<lua_Number>(h));
    return 1;
}

/// wag.draw.unload_shader(name) —— 帧末生效（本帧已收集的绘制项仍能画完）
int l_draw_unload_shader(lua_State* L) {
    auto& eng = engine_of(L);
    const char* name = required_string(L, 1, "name", "draw.unload_shader");
    eng.renderer().unload_shader(std::string(name));
    return 0;
}

/// wag.time.set(n) —— 把\"当前帧\"重新定义为游戏第 n 帧。
///
/// 【它只挪 0 点，不改帧号本身】（与 RNG 的 master seed 同一条纪律）
/// 【生效时机】立即（下一次取 u_time 就用新值）；不影响已收集的绘制项。
int l_time_set(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer n = static_cast<lua_Integer>(luaL_checknumber(L, 1));
    eng.renderer().set_time_frame(static_cast<std::int64_t>(n));
    return 0;
}

/// wag.time.get() -> 当前游戏帧号
int l_time_get(lua_State* L) {
    auto& eng = engine_of(L);
    lua_pushnumber(L, static_cast<lua_Number>(eng.renderer().time_frame()));
    return 1;
}

/// wag.draw.set_probe_shaders(dir) —— 【临时探针入口 · 交付前删除】
///
/// 【为什么参数是"目录"而不是"文件名清单"】文件名清单是引擎侧的实验细节；
/// 脚本只需要告诉引擎"外置着色器在哪"。目录本身由脚本用
/// `resolve.luminosa_dir() .. "/Project/shaders"` 拼 —— 这是 的要求
///（**引擎不解析路径**，拼接在 Lua 侧）。
int l_draw_set_probe_shaders(lua_State* L) {
    auto& eng = engine_of(L);
    const char* dir = required_string(L, 1, "dir", "draw.set_probe_shaders");
    eng.renderer().set_shader_probe_dir(std::string(dir));
    return 0;
}

/// wag.image.load_from_file(name, path) -> handle
int l_image_load_from_file(lua_State* L) {
    auto& eng = engine_of(L);
    const char* name = required_string(L, 1, "name", "image.load_from_file");
    const char* path = required_string(L, 2, "path", "image.load_from_file");
    if (wag::draw::renderer::is_reserved_image_name(name)) {
        return luaL_error(L, "image.load_from_file 拒绝引擎保留名（__ 前缀）：%s", name);
    }
    const std::uint64_t h = eng.renderer().load_image_from_file(name, path);
    lua_pushnumber(L, static_cast<lua_Number>(h));
    return 1;
}

/// wag.image.load_from_snapshot(name, x, y, w, h) -> handle
int l_image_load_from_snapshot(lua_State* L) {
    auto& eng = engine_of(L);
    const char* name = required_string(L, 1, "name", "image.load_from_snapshot");
    if (wag::draw::renderer::is_reserved_image_name(name)) {
        return luaL_error(L, "image.load_from_snapshot 拒绝引擎保留名（__ 前缀）：%s", name);
    }
    const double x = static_cast<double>(luaL_checknumber(L, 2));
    const double y = static_cast<double>(luaL_checknumber(L, 3));
    const double w = static_cast<double>(luaL_checknumber(L, 4));
    const double h = static_cast<double>(luaL_checknumber(L, 5));
    const std::uint64_t handle =
        eng.renderer().load_image_from_snapshot(name, x, y, w, h);
    lua_pushnumber(L, static_cast<lua_Number>(handle));
    return 1;
}

/// wag.image.unload(name) -> nil（幂等：卸载不存在的东西不算错）
int l_image_unload(lua_State* L) {
    auto& eng = engine_of(L);
    const char* name = required_string(L, 1, "name", "image.unload");
    if (wag::draw::renderer::is_reserved_image_name(name)) {
        return luaL_error(L, "image.unload 拒绝引擎保留名（__ 前缀）：%s", name);
    }
    eng.renderer().unload_image(name);
    return 0;
}

/// wag.image.lookup(name) -> handle | nil（"查询"语义 ⇒ 不存在返回 nil，不报错）
int l_image_lookup(lua_State* L) {
    auto& eng = engine_of(L);
    const char* name = required_string(L, 1, "name", "image.lookup");
    std::uint64_t handle = 0;
    if (!eng.renderer().lookup_image_handle(name, handle)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, static_cast<lua_Number>(handle));
    return 1;
}

/// wag.image.size(handle) -> w, h | nil（"查询"语义 ⇒ 失效返回 nil）
///
/// 【2026-11-10 】句柄必须是**有限、非负、整数**且 <= 2^53-1；
/// 否则它不是句柄 ⇒ 按"查不到"返回 nil（查询 API 不终止）。
// -----------------------------------------------------------------------------
// wag.font.* —— TTF 字体与字形（2026-11-18）
// -----------------------------------------------------------------------------

/// wag.font.load_ttf(name, path, pixel_size) -> handle
int l_font_load_ttf(lua_State* L) {
    auto& eng = engine_of(L);
    const char* name = required_string(L, 1, "name", "wag.font.load_ttf");
    const char* path = required_string(L, 2, "path", "wag.font.load_ttf");
    const double size = static_cast<double>(luaL_checknumber(L, 3));
    const std::uint64_t handle = eng.fonts().load(name, path, size);
    lua_pushnumber(L, static_cast<lua_Number>(handle));
    return 1;
}

/// 把一个字形字段写进表（-1 是表）。
void set_num_field(lua_State* L, const char* key, double v) {
    lua_pushnumber(L, static_cast<lua_Number>(v));
    lua_setfield(L, -2, key);
}

/// 复用表的取法（registry 里一张，全程序共用）。
void push_reused_font_table(lua_State* L, const char* registry_key) {
 // 【2026-11-18 简化：每次新建】原计划用 registry 复用，实测在 LuaJIT 上
 // 复用路径抛 "attempt to index a number value"（registry 伪索引处理有坑）。
 // 而 Lua 侧（Lumi/font.lua）已经缓存了字形 ⇒ 引擎调用只在首次 miss 时发生，
 // 这里的分配可以忽略。若将来要优化，先补一个能复现的探针。
    (void)registry_key;
    lua_createtable(L, 0, 12);
}

/// wag.font.glyph(font, codepoint) -> table | nil
int l_font_glyph(lua_State* L) {
    auto& eng = engine_of(L);
    const std::uint64_t font = static_cast<std::uint64_t>(read_handle_arg(L, 1, "wag.font.glyph 的 font"));
    const double cp_num = static_cast<double>(luaL_checknumber(L, 2));
    if (!std::isfinite(cp_num) || cp_num < 0.0 || cp_num > 1114111.0 ||
        std::floor(cp_num) != cp_num) {
        lua_pushnil(L);
        return 1;
    }
    text::glyph_info g;
 // 【句柄失效 ⇒ L3】取字形是"使用"，不是"查询"。
    if (!eng.fonts().valid(font)) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "wag.font.glyph 的 font";
        e.got = "一个失效的字体句柄（已卸载 / 被同名覆盖）";
        e.want = "一个仍有效的字体句柄（载入后未被卸载或同名覆盖）";
        fail_boundary(e);
    }
    if (!eng.fonts().glyph(font, static_cast<std::uint32_t>(cp_num), g)) {
 // 图集页满（L2，只记过一次）⇒ 返回 nil：调用方跳过这个字形
        lua_pushnil(L);
        return 1;
    }
    push_reused_font_table(L, "__wag_font_glyph_table");
    set_num_field(L, "image", static_cast<double>(g.image));
    set_num_field(L, "u", g.u);
    set_num_field(L, "v", g.v);
    set_num_field(L, "uw", g.uw);
    set_num_field(L, "uh", g.uh);
    set_num_field(L, "advance", g.advance);
    set_num_field(L, "bearing_x", g.bearing_x);
    set_num_field(L, "bearing_y", g.bearing_y);
    set_num_field(L, "w", g.w);
    set_num_field(L, "h", g.h);
    lua_pushboolean(L, g.missing ? 1 : 0);
    lua_setfield(L, -2, "missing");
    return 1;
}

/// wag.font.has_glyph(font, codepoint) -> boolean（句柄失效 ⇒ L3）
int l_font_has_glyph(lua_State* L) {
    auto& eng = engine_of(L);
    const std::uint64_t font = static_cast<std::uint64_t>(read_handle_arg(L, 1, "wag.font.has_glyph 的 font"));
    const double cp_num = static_cast<double>(luaL_checknumber(L, 2));
    text::font_metrics m;
    if (!eng.fonts().metrics(font, m)) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "wag.font.has_glyph 的 font";
        e.got = "一个失效的字体句柄（已卸载 / 被同名覆盖）";
        e.want = "一个仍有效的字体句柄";
        fail_boundary(e);
    }
    const bool has = std::isfinite(cp_num) && cp_num >= 0.0 && cp_num <= 1114111.0 &&
                     std::floor(cp_num) == cp_num &&
                     eng.fonts().has_glyph(font, static_cast<std::uint32_t>(cp_num));
    lua_pushboolean(L, has ? 1 : 0);
    return 1;
}

/// wag.font.metrics(font) -> table（句柄失效 ⇒ L3）
int l_font_metrics(lua_State* L) {
    auto& eng = engine_of(L);
    const std::uint64_t font = static_cast<std::uint64_t>(read_handle_arg(L, 1, "wag.font.metrics 的 font"));
    text::font_metrics m;
    if (!eng.fonts().metrics(font, m)) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "wag.font.metrics 的 font";
        e.got = "一个失效的字体句柄（已卸载 / 被同名覆盖）";
        e.want = "一个仍有效的字体句柄";
        fail_boundary(e);
    }
    push_reused_font_table(L, "__wag_font_metrics_table");
    set_num_field(L, "pixel_size", m.pixel_size);
    set_num_field(L, "ascent", m.ascent);
    set_num_field(L, "descent", m.descent);
    set_num_field(L, "line_gap", m.line_gap);
    set_num_field(L, "line_height", m.line_height);
    set_num_field(L, "num_glyphs", static_cast<double>(m.num_glyphs));
    return 1;
}

/// wag.font.unload(name) -> nil（幂等）
int l_font_unload(lua_State* L) {
    auto& eng = engine_of(L);
    const char* name = required_string(L, 1, "name", "wag.font.unload");
    eng.fonts().unload(name);
    return 0;
}

int l_image_size(lua_State* L) {
    auto& eng = engine_of(L);
    const double raw = static_cast<double>(luaL_checknumber(L, 1));
 // 2^53-1：Lua number 能精确表示的最大整数。
    constexpr double kMaxExactInt = 9007199254740991.0;
    if (!std::isfinite(raw) || raw < 0.0 || raw > kMaxExactInt ||
        std::floor(raw) != raw) {
        lua_pushnil(L);
        return 1;
    }
    double w = 0.0;
    double h = 0.0;
    if (!eng.renderer().image_pixel_size(static_cast<std::uint64_t>(raw), w, h)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, static_cast<lua_Number>(w));
    lua_pushnumber(L, static_cast<lua_Number>(h));
    return 2;
}

// -----------------------------------------------------------------------------
// wag.image.define_frames(handle, { {x,y,w,h}, ... }) —— 给图片挂帧表（/）
//
// 【为什么只有"显式矩形"这一种形态】"按行列切网格"是**排版概念**，引擎不认识
//（与"引擎不认识动画/叙事"同族）。网格切片在库层用 `wag.image.size()` 现算即可。
// 【为什么句柄而不是名字】与 `size` 同一条：句柄能自证失效；名字在覆盖后指向新图，
// 给旧名字挂帧表会悄悄改到新图上。
// 【错误】句柄失效 / 空表 / 矩形越界 / 帧数 > 65535 ⇒ L3（引擎侧 `define_frames` 抛）。
// -----------------------------------------------------------------------------
int l_image_define_frames(lua_State* L) {
    auto& eng = engine_of(L);

 // ---- ① 句柄（与 size 同一套精确整数校验）----
    const double raw = static_cast<double>(luaL_checknumber(L, 1));
    constexpr double kMaxExactInt = 9007199254740991.0;
    if (!std::isfinite(raw) || raw < 0.0 || raw > kMaxExactInt ||
        std::floor(raw) != raw) {
        return luaL_error(L, "define_frames 的 handle 必须是精确的非负整数");
    }

 // ---- ② 帧表：数组，每个元素是 { x, y, w, h } ----
    luaL_checktype(L, 2, LUA_TTABLE);
    const int n = static_cast<int>(lua_objlen(L, 2));
    std::vector<wag::draw::frame_rect> rects;
    rects.reserve(static_cast<std::size_t>(n));
    for (int k = 1; k <= n; ++k) {
        lua_rawgeti(L, 2, k);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return luaL_error(L, "define_frames 的第 %d 帧必须是表 { x, y, w, h }", k);
        }
        const int t = lua_gettop(L);
        wag::draw::frame_rect f{};
        const char* const names[4] = {"x", "y", "w", "h"};
        double vals[4] = {0.0, 0.0, 0.0, 0.0};
        for (int a = 0; a < 4; ++a) {
            lua_getfield(L, t, names[a]);
            if (!lua_isnumber(L, -1)) {
                const char* tn = luaL_typename(L, -1);
                lua_pop(L, 2);
                return luaL_error(L,
                                  "define_frames 的第 %d 帧缺少数字字段 %s，got %s",
                                  k, names[a], tn);
            }
            vals[a] = static_cast<double>(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        for (int a = 0; a < 4; ++a) {
            if (!std::isfinite(vals[a]) || vals[a] < 0.0 ||
                vals[a] > 65535.0 || std::floor(vals[a]) != vals[a]) {
                return luaL_error(L,
                                  "define_frames 的第 %d 帧的 %s 必须是 [0, 65535] 内的整数，got %g",
                                  k, names[a], vals[a]);
            }
        }
        f.x = static_cast<std::uint32_t>(vals[0]);
        f.y = static_cast<std::uint32_t>(vals[1]);
        f.w = static_cast<std::uint32_t>(vals[2]);
        f.h = static_cast<std::uint32_t>(vals[3]);
        rects.push_back(f);
    }

    if (eng.renderer().is_reserved_image_handle(static_cast<std::uint64_t>(raw))) {
        return luaL_error(L, "define_frames 拒绝引擎保留图片（字体图集页）");
    }
    eng.renderer().define_frames(static_cast<std::uint64_t>(raw), rects);
    return 0;
}

// -----------------------------------------------------------------------------
// wag.image.frame_count(handle) —— 帧数 n（**普通图片 = 1**，）
//
// 【查询语义】失效句柄 ⇒ nil（与 size 同一条：查询返回 nil，使用才报错）。
// -----------------------------------------------------------------------------
int l_image_frame_count(lua_State* L) {
    auto& eng = engine_of(L);
    const double raw = static_cast<double>(luaL_checknumber(L, 1));
    constexpr double kMaxExactInt = 9007199254740991.0;
    if (!std::isfinite(raw) || raw < 0.0 || raw > kMaxExactInt ||
        std::floor(raw) != raw) {
        lua_pushnil(L);
        return 1;
    }
    const std::uint32_t n =
        eng.renderer().image_frame_count(static_cast<std::uint64_t>(raw));
    if (n == 0u) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(n));
    return 1;
}

// -----------------------------------------------------------------------------
// wag.image.anchor(handle, ax, ay) —— 设锚点（，归一化 0..1）
// wag.image.anchor(handle) —— 查锚点 → ax, ay（失效句柄 ⇒ nil）
//
// 【语义】`draw_image(img, L, Z, x, y, w, h)` 的 (x, y) = **锚点落在哪**。
// 默认 (0,0) = 左上 ⇒ 与加锚点之前逐字节相同。
// 【整张图一个】不逐帧；容器恒以中心为锚（那条不受影响）。
// 【范围】任意有限值（1.5 = 轴心在图外，合法）；非有限 ⇒ L3。
// -----------------------------------------------------------------------------
int l_image_anchor(lua_State* L) {
    auto& eng = engine_of(L);

    const double raw = static_cast<double>(luaL_checknumber(L, 1));
    constexpr double kMaxExactInt = 9007199254740991.0;
    if (!std::isfinite(raw) || raw < 0.0 || raw > kMaxExactInt ||
        std::floor(raw) != raw) {
 // 句柄本身不是精确整数 ⇒ 查询返回 nil、设置报错（与 size / define_frames 一致）
        if (lua_gettop(L) <= 1) {
            lua_pushnil(L);
            return 1;
        }
        return luaL_error(L, "anchor 的 handle 必须是精确的非负整数");
    }
    const std::uint64_t handle = static_cast<std::uint64_t>(raw);
    if (eng.renderer().is_reserved_image_handle(handle)) {
        return luaL_error(L, "image.anchor 拒绝引擎保留图片（字体图集页）");
    }

 // ---- 查询形态：只给句柄 ----
    if (lua_gettop(L) <= 1) {
        double ax = 0.0;
        double ay = 0.0;
        if (!eng.renderer().image_anchor(handle, ax, ay)) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushnumber(L, static_cast<lua_Number>(ax));
        lua_pushnumber(L, static_cast<lua_Number>(ay));
        return 2;
    }

 // ---- 设置形态：ax, ay 都必须给 ----
    if (!lua_isnumber(L, 2) || !lua_isnumber(L, 3)) {
        return luaL_error(L, "anchor(handle, ax, ay) 的 ax / ay 必须是数字");
    }
    const double ax = static_cast<double>(lua_tonumber(L, 2));
    const double ay = static_cast<double>(lua_tonumber(L, 3));
    eng.renderer().set_image_anchor(handle, ax, ay);
    return 0;
}

} // namespace
#endif

// =============================================================================
// wag.rng —— 随机流（splitmix64）
//
// 【为什么状态在引擎、Lua 只拿句柄】
// 流的状态是 u64，而 Lua 的数字是 double（**只有 53 位精确**）
// ⇒ 状态一旦经过 Lua 就丢位 ⇒ 序列静默改变 ⇒ 回放失效。
// ⇒ 所以状态住在 `rng::registry`（engine 的部件），Lua 只持有一个整数句柄。
//
// 【错误等级：一律 L3】
// 已定：除 `Shuffle(空表)` 之外，随机原语的非法输入全部终止。
// 理由：这些错误**从不合法**（区间反向、空表、概率越界、句柄失效），
// 而且它们的"宽容版"会静默产出一个错误的世界
//（例如 `Chance(30)` 恒真 ⇒ 掉落率 100%，不报错、只表现为"运气好"）。
// ⇒ 与 `checked_double` 同款：`boundary_validation` + `fail_boundary`。
//
// 【句柄的纪律（⑤）】
// 句柄是**单调递增的 id**，永不复用。脚本重载时引导脚本调 `reset()`
// ⇒ 表清空 ⇒ 旧句柄查不到 ⇒ 本层报 L3（而不是悄悄指向一条新流）。
// =============================================================================

#if defined(WAG_HAVE_LUAJIT)
namespace {

/// 取一个"必须是数字"的参数；否则 L3。
lua_Integer rng_integer(lua_State* L, int idx, const char* where) {
    if (!lua_isnumber(L, idx)) {
        wag::error_value e;
        e.cls = wag::error_class::boundary_validation;
        e.where = where;
        e.got = luaL_typename(L, idx);
        e.want = "一个整数";
        wag::fail_boundary(e);
    }
    return static_cast<lua_Integer>(lua_tonumber(L, idx));
}

/// 取一个流句柄；非数字 ⇒ L3。
std::uint64_t rng_handle(lua_State* L, int idx, const char* where) {
    if (!lua_isnumber(L, idx)) {
        wag::error_value e;
        e.cls = wag::error_class::boundary_validation;
        e.where = where;
        e.got = luaL_typename(L, idx);
        e.want = "一个随机流句柄（wag.rng.new_stream 的返回值）";
        wag::fail_boundary(e);
    }
    return static_cast<std::uint64_t>(static_cast<double>(lua_tonumber(L, idx)));
}

/// 句柄无效（未创建、或脚本重载前留下的旧句柄）⇒ L3。
void rng_bad_handle(const char* where) {
    wag::error_value e;
    e.cls = wag::error_class::boundary_validation;
    e.where = where;
    e.got = "无效句柄（不存在，或属于脚本重载之前）";
    e.want = "wag.rng.new_stream 在当前脚本代际里返回的句柄";
    wag::fail_boundary(e);
}

/// wag.rng.master_seed() -> lo, hi
///
/// 【为什么拆两个 32 位】u64 过不了 Lua 的 53 位精度。
/// 【它是"这一局"的身份】存档存这两个数 ⇒ 重放能重建整局随机。
int l_rng_master_seed(lua_State* L) {
    const std::uint64_t m = engine_of(L).rng().master_seed();
    lua_pushnumber(L, static_cast<lua_Number>(m & 0xFFFFFFFFull));
    lua_pushnumber(L, static_cast<lua_Number>(m >> 32));
    return 2;
}

/// wag.rng.new_stream(index) -> handle
///
/// 【index 只能是 0/1/2】0 = main，1 = pause，2 = ext
/// —— 这是**平台层预设**（已定），越界属于调用方写错 ⇒ L3。
int l_rng_new_stream(lua_State* L) {
    const lua_Integer idx = rng_integer(L, 1, "wag.rng.new_stream 的 index");
    if (idx < 0 || idx >= static_cast<lua_Integer>(wag::rng::kStreamCount)) {
        wag::error_value e;
        e.cls = wag::error_class::boundary_validation;
        e.where = "wag.rng.new_stream 的 index";
        e.got = "越界";
        e.want = "0（main）/ 1（pause）/ 2（ext）";
        wag::fail_boundary(e);
    }
    const std::uint64_t h =
        engine_of(L).rng().new_stream(static_cast<std::uint32_t>(idx));
    if (h == 0) {
        wag::fail_internal("new_stream 拒绝了已校验过的槽位");
    }
    lua_pushnumber(L, static_cast<lua_Number>(h));
    return 1;
}

/// wag.rng.uint32(handle) -> [0, 2^32)
int l_rng_uint32(lua_State* L) {
    const std::uint64_t h = rng_handle(L, 1, "wag.rng.uint32 的 handle");
    std::uint32_t out = 0;
    if (!engine_of(L).rng().next_u32(h, out)) {
        rng_bad_handle("wag.rng.uint32 的 handle");
    }
    lua_pushnumber(L, static_cast<lua_Number>(out));
    return 1;
}

/// wag.rng.int(handle, lo, hi) -> [lo, hi]（**含两端**）
int l_rng_int(lua_State* L) {
    const std::uint64_t h = rng_handle(L, 1, "wag.rng.int 的 handle");
    const lua_Integer lo = rng_integer(L, 2, "wag.rng.int 的 lo");
    const lua_Integer hi = rng_integer(L, 3, "wag.rng.int 的 hi");
    std::int64_t out = 0;
    if (!engine_of(L).rng().next_int(h, static_cast<std::int64_t>(lo),
                                     static_cast<std::int64_t>(hi), out)) {
        if (hi < lo) {
 // 【不自动交换（已定）】"区间写反"是从不合法的输入，
 // 宽容版会静默给出一段反过来的区间，比报错更难查。
            wag::error_value e;
            e.cls = wag::error_class::boundary_validation;
            e.where = "wag.rng.int 的区间";
            e.got = "hi < lo";
            e.want = "lo <= hi";
            wag::fail_boundary(e);
        }
        rng_bad_handle("wag.rng.int 的 handle");
    }
    lua_pushnumber(L, static_cast<lua_Number>(out));
    return 1;
}

/// wag.rng.float(handle) -> [0, 1)，53 位精度
int l_rng_float(lua_State* L) {
    const std::uint64_t h = rng_handle(L, 1, "wag.rng.float 的 handle");
    double out = 0.0;
    if (!engine_of(L).rng().next_float(h, out)) {
        rng_bad_handle("wag.rng.float 的 handle");
    }
    lua_pushnumber(L, static_cast<lua_Number>(out));
    return 1;
}

/// wag.rng.state(handle) -> lo, hi
int l_rng_state(lua_State* L) {
    const std::uint64_t h = rng_handle(L, 1, "wag.rng.state 的 handle");
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    if (!engine_of(L).rng().get_state(h, lo, hi)) {
        rng_bad_handle("wag.rng.state 的 handle");
    }
    lua_pushnumber(L, static_cast<lua_Number>(lo));
    lua_pushnumber(L, static_cast<lua_Number>(hi));
    return 2;
}

/// wag.rng.set_state(handle, lo, hi)
int l_rng_set_state(lua_State* L) {
    const std::uint64_t h = rng_handle(L, 1, "wag.rng.set_state 的 handle");
    const lua_Integer lo = rng_integer(L, 2, "wag.rng.set_state 的 lo");
    const lua_Integer hi = rng_integer(L, 3, "wag.rng.set_state 的 hi");
    if (!engine_of(L).rng().set_state(h, static_cast<std::uint32_t>(lo),
                                      static_cast<std::uint32_t>(hi))) {
        rng_bad_handle("wag.rng.set_state 的 handle");
    }
    return 0;
}

/// wag.rng.reset() —— 清空所有流（句柄全部失效）。
///
/// 【调用点】脚本重载时由引导脚本调用（⑤ 已定：重载后重建而非复用）。
/// 【现在没有调用者】脚本热重载还没有落地；这个入口就是它的挂点。
int l_rng_reset(lua_State* L) {
    engine_of(L).rng().reset();
    return 0;
}

// =============================================================================
// wag.container —— 容器原语（2026-10-20）
//
// 【本组做六件事】建池 / 生成 / 写回 / 销毁，外加查询与**类登记**：
// new_pool(name) -> pool_id
// spawn(pool_id, params) -> handle
// write(handle, params) 逐字段写回（库层每帧调用）
// kill(handle) 标记 dead（帧末回收）
// alive(handle) -> bool
// set_hit(handle, hit) 创建后改判定（：frame 里可改）
// set_dispatcher(fn) 安装库层分派器（，唯一入口）
// register_class(id, t) 登记"类有没有回调"（/）
//
// 【为什么类只登记存在位】
//：引擎拿不到 self，回调由库层分派器调；这一层只存位掩码，
// 不持有任何类级/实例级 Lua 引用（分派器是唯一的服务级引用）。
//
// 【参数校验的口径】
// · spawn：**拒绝未知键**（拼写错误必须当场报，不能静默忽略）
// · write：忽略未知键（它们是 ④ 字段）；但 img/collides/group/class
// 出现即 L2 —— 它们创建后不可变（///）
// =============================================================================
// 容器原语的类型（container / handle / hit_entry / container_pool）都在
// wag::container 里；本文件的匿名命名空间在 wag::script 下，故显式引入。
using namespace wag::container;

/// 从 Lua 表按名字读字段；缺省返回 fallback，出现但不是合法类型 ⇒ L2。
struct table_reader {
    lua_State* L;
    int t;

    double number(const char* name, double fallback) {
        lua_getfield(L, t, name);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return fallback;
        }
        if (!lua_isnumber(L, -1)) {
            luaL_error(L, "参数 %s 必须是数字", name);
        }
        const double v = static_cast<double>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        if (!std::isfinite(v)) {
            luaL_error(L, "参数 %s 必须是有限数", name);
        }
        return v;
    }

    lua_Integer integer(const char* name, lua_Integer fallback, lua_Integer lo,
                        lua_Integer hi) {
        lua_getfield(L, t, name);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return fallback;
        }
        if (!lua_isnumber(L, -1)) {
            luaL_error(L, "参数 %s 必须是整数", name);
        }
        const lua_Number n = lua_tonumber(L, -1);
        lua_pop(L, 1);
        if (!std::isfinite(static_cast<double>(n)) ||
            std::floor(static_cast<double>(n)) != static_cast<double>(n)) {
            luaL_error(L, "参数 %s 必须是整数", name);
        }
        const lua_Integer v = static_cast<lua_Integer>(n);
        if (v < lo || v > hi) {
            luaL_error(L, "参数 %s 超出范围", name);
        }
        return v;
    }

    bool boolean(const char* name, bool fallback) {
        lua_getfield(L, t, name);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return fallback;
        }
        if (!lua_isboolean(L, -1)) {
            luaL_error(L, "参数 %s 必须是布尔", name);
        }
        const bool v = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return v;
    }
};

/// 读容器 `img`：句柄必须能放进 u32（container.img 的宽度）。
///
/// 【为什么单独一条】通用 table_reader::integer 只报"参数 img 超出范围"，
/// 看不出是"图片代数上限"。这条把原因、gen/slot、规避方法写进消息，
/// 并先记一条 L2 —— 即使脚本 pcall 吞掉 Lua 错误，日志里仍有原因。
std::uint32_t read_container_img(lua_State* L, table_reader& r,
                                 const char* what, lua_Integer fallback) {
    constexpr lua_Integer kMaxImageHandle = 9007199254740991LL; // 2^53-1
    const lua_Integer raw = r.integer("img", fallback, 0, kMaxImageHandle);
    if (raw <= 0xFFFFFFFFLL) {
        return static_cast<std::uint32_t>(raw);
    }
    const std::uint64_t handle = static_cast<std::uint64_t>(raw);
    const std::uint32_t gen = static_cast<std::uint32_t>(handle >> 21);
    const std::uint32_t slot = static_cast<std::uint32_t>(handle & 0x1FFFFFu);
    const std::string detail =
        std::string(what) + " | handle=" + std::to_string(handle) +
        " gen=" + std::to_string(gen) + " slot=" + std::to_string(slot);
    const std::string message =
        "容器 img 超出 u32（" + std::string(what) +
        "）：handle=" + std::to_string(handle) +
        " gen=" + std::to_string(gen) + " slot=" + std::to_string(slot) +
        "；container.img 是 u32，图片槽位代数需 <= 2047；"
        "重启编辑器/引擎，或不要把该图片赋给容器";
    log::warn("script", log::format(
        "容器 img 超出 u32（图片代数上限）",
        detail + "；container.img 是 u32（图片槽位代数需 <= 2047）；"
                 "重启编辑器/引擎可归零；draw_image / draw_vertices 不受影响"));
    luaL_error(L, "%s", message.c_str());
    return 0;
}

/// spawn 允许的键（结构体字段 + hit）；其余一律拒绝
bool is_spawn_key(std::string_view k) {
    return k == "x" || k == "y" || k == "vx" || k == "vy" || k == "rot" ||
           k == "sx" || k == "sy" || k == "rgba" || k == "layer" || k == "z" ||
           k == "frame" || k == "blend" || k == "vcolor" || k == "tex_alpha" ||
           k == "visible" || k == "motion" ||
           k == "group" || k == "class" || k == "img" || k == "collides" ||
           k == "hit";
}

/// `frame` 的合法集合 = **[0, n)**：
/// · 带帧表的图片：n = 帧矩形个数；
/// · **普通图片：n = 1**（整张图就是第 0 帧）⇒ 只有 frame == 0 合法。
///
/// 【越界 ⇒ L2 + 不生效】与 的 `collides`（非法写入 ⇒ L2、不生效）同一档：
/// 不钳制、不当 0 —— 两者都是"静默画错"，而"在静态图上写 frame = 3"
/// 恰恰最容易被误认为"在动"。
///
/// 【两种不报的情形】
/// · `img == 0`：没有贴图 ⇒ frame 根本不参与发射（不制造噪音）；
/// · 帧数为 0：句柄已失效 ⇒ 那是 那条错误，不在这里重复报。
///
/// @return true = 合法（可以写入）；false = 越界（已记 L2，调用方**不要写**）
bool container_frame_is_legal(wag::engine& eng, std::uint32_t img,
                              std::uint32_t frame) {
    if (img == 0u) {
        return true;
    }
    const std::uint32_t n = eng.renderer().image_frame_count(img);
    if (n == 0u || frame < n) {
        return true;
    }
    log::warn("script", log::format(
        "frame 越界（本次写入不生效）",
        "图片句柄 " + std::to_string(img) + " 的帧数 n = " + std::to_string(n) +
            "，frame = " + std::to_string(frame) +
            "；合法集合是 [0, n)（普通图片 n = 1，）"));
    return false;
}

/// 解析 hit 表（的三种形状）；不合法 ⇒ L2
hit_entry parse_hit(lua_State* L, int t) {
    table_reader r{L, t};

    lua_getfield(L, t, "kind");
    if (!lua_isstring(L, -1)) {
        luaL_error(L, "hit.kind 必须是字符串：point / ellipse / rect");
    }
    const std::string_view kind{lua_tostring(L, -1)};
    lua_pop(L, 1);

    hit_entry e{};
    if (kind == "point") {
        e.kind = hit_kind::point;
    } else if (kind == "ellipse") {
        e.kind = hit_kind::ellipse;
        const double rx = r.number("rx", -1.0);
        const double ry = r.number("ry", -1.0);
        if (!(rx >= 0.0) || !(ry >= 0.0)) {
            luaL_error(L, "hit.rx / hit.ry 必须是 ≥0 的数");
        }
        e.a = static_cast<float>(rx);
        e.b = static_cast<float>(ry);
    } else if (kind == "rect") {
        e.kind = hit_kind::rect;
        const double w = r.number("w", -1.0);
        const double h = r.number("h", -1.0);
        if (!(w >= 0.0) || !(h >= 0.0)) {
            luaL_error(L, "hit.w / hit.h 必须是 ≥0 的数");
        }
        e.a = static_cast<float>(w);
        e.b = static_cast<float>(h);
    } else {
        luaL_error(L, "hit.kind 只能是 point / ellipse / rect，got %s",
                   std::string(kind).c_str());
    }

    e.ox = static_cast<float>(r.number("ox", 0.0));
    e.oy = static_cast<float>(r.number("oy", 0.0));
    return e;
}

// -----------------------------------------------------------------------------
// wag.draw.emit_containers() —— 从 64 B 结构体直接发射（b 方案默认渲染路径）
//
// 【为什么需要它】逐对象 draw_image 是每帧 6 万次 Lua→C 跨界 + 6 万次顶点拼装；
// 而结构体里已经有 x/y/img/sx/sy/layer/z/rgba —— 渲染的正确数据源是它。
// 本入口在 C 里一次遍历所有池的存活容器，一容器 4 顶点，零 Lua 回调。
//
// 【它处理】每容器 4 顶点：rot 绕中心、负 sx/sy 镜像、
// blend/vcolor/tex_alpha 逐容器；layer/z 参与排序。
// 【它不处理】自定义 render 回调（那是作者主动要的形状，仍走 Lua）；
// frame 的渲染（依赖 的图像组，尚未落地）。
// 【可见性】visible == 0 或 img == 0 的容器直接跳过。
// -----------------------------------------------------------------------------
int l_draw_emit_containers(lua_State* L) {
    auto& eng = engine_of(L);
    auto& pools = eng.containers();
    auto& rend = eng.renderer();

    for (std::uint32_t p = 0; p < static_cast<std::uint32_t>(pools.pool_count());
         ++p) {
        pools.pool(p).for_each_live([&rend](const wag::container::container& c) {
            if (c.visible == 0 || c.img == 0) {
                return;
            }
 //：三态从容器字段来；：负 sx/sy 镜像；rot 绕中心旋转。
 // /：`frame` 交给渲染层查帧矩形（越界在那里 L3，不静默取第 0 帧）。
            rend.draw_container_quad(
                c.img, static_cast<std::uint32_t>(c.frame), c.layer,
                static_cast<double>(c.z),
                static_cast<double>(c.x), static_cast<double>(c.y),
                static_cast<double>(c.sx), static_cast<double>(c.sy),
                static_cast<double>(c.rot), c.rgba,
                static_cast<wag::draw::blend_state>(c.blend),
                static_cast<wag::draw::vertex_color_mode>(c.vcolor),
                static_cast<wag::draw::tex_alpha>(c.tex_alpha));
        });
    }
    return 0;
}

// -----------------------------------------------------------------------------
// wag.draw.capture_snapshot() —— B 方案：显式请求"本帧阶段 10 拍快照"
//
// 【语义】本帧请求；阶段 10（渲染之后）拍；**下一帧起**可被
// wag.image.load_from_snapshot 读取。
// 【为什么需要它】B 方案后快照不再每帧自动拍；截图/自定义暂停背景必须显式请求。
// -----------------------------------------------------------------------------
int l_draw_capture_snapshot(lua_State* L) {
    engine_of(L).renderer().request_staged_snapshot();
    return 0;
}

// -----------------------------------------------------------------------------
// wag.container.set_dispatcher(fn) ——
//
// 【它是唯一允许持有的 Lua 引用】：引擎拿不到 self，只能经库层分派器回调。
// 【重复 set】同一个函数 ⇒ 幂等；不同函数 ⇒ L2 且**不改变**当前分派器
//（引擎不替作者决定"谁赢"；替换必须先 set_dispatcher(nil)）。
// -----------------------------------------------------------------------------
int l_container_set_dispatcher(lua_State* L) {
    auto& eng = engine_of(L);
    auto& sc = eng.script();

    if (lua_isnoneornil(L, 1)) {
        if (sc.has_dispatcher()) {
            luaL_unref(L, LUA_REGISTRYINDEX, sc.dispatcher_ref());
            sc.set_dispatcher_ref(-1);
            log::record("容器", "分派器已清空（已注册的类在重装前不会收到回调）");
        }
        return 0;
    }

    if (!lua_isfunction(L, 1)) {
        wag::fail_script("set_dispatcher 的参数必须是函数或 nil");
        return 0;
    }

    if (sc.has_dispatcher()) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, sc.dispatcher_ref());
        const bool same = (lua_rawequal(L, -1, 1) != 0);
        lua_pop(L, 1);
        if (same) {
            return 0; // 同一函数：幂等
        }
        wag::fail_script("分派器已安装；要替换请先 set_dispatcher(nil)");
        return 0; // 不改变当前分派器
    }

    lua_pushvalue(L, 1);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    sc.set_dispatcher_ref(ref);
    return 0;
}

// -----------------------------------------------------------------------------
// wag.container.register_class(id, { init, frame, render, colli, kill, del })
//
// 【只存存在位】：引擎不持有任何类级/实例级 Lua 引用；函数留在库层。
// 【顺序】：没有分派器时注册 ⇒ L3。
// -----------------------------------------------------------------------------
int l_container_register_class(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer raw = luaL_checkinteger(L, 1);
    if (raw < 0 || raw > 65535) {
        wag::fail_script("register_class 的 class id 必须在 0..65535");
        return 0;
    }
    luaL_checktype(L, 2, LUA_TTABLE);

    if (!eng.script().has_dispatcher()) {
        wag::error_value e;
        e.cls = wag::error_class::boundary_validation;
        e.where = "register_class 的调用顺序";
        e.got = "尚未 set_dispatcher";
        e.want = "先 wag.container.set_dispatcher(dispatch)";
        wag::fail_boundary(e);
    }

    static constexpr const char* names[] = {"init", "frame", "render",
                                            "colli", "kill",  "del"};
    std::uint8_t mask = 0;
    for (int i = 0; i < 6; ++i) {
        lua_getfield(L, 2, names[i]);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            wag::fail_script(std::string("register_class 的 ") + names[i] + " 必须是函数");
            return 0;
        }
        mask |= class_registry::callback_bit(static_cast<class_callback>(i));
        lua_pop(L, 1);
    }

    auto& reg = eng.containers().classes();
    const auto id = static_cast<std::uint16_t>(raw);
    if (reg.is_registered(id)) {
        wag::fail_script("类 " + std::to_string(raw) + " 已注册（重复注册）");
        return 0; // 不改变已有登记
    }
    reg.register_class(id, mask);
    return 0;
}

int l_container_new_pool(lua_State* L) {
    auto& eng = engine_of(L);
    const char* name = luaL_checkstring(L, 1);
 // 【A 类 · 池数上限】原本靠 create_pool 内部的 fail_internal（⇒ 终止）。
 // 池数完全由脚本的调用次数决定 ⇒ 是**调用者数据** ⇒ 该在交界处报。
    if (eng.containers().pool_count() >= wag::container::max_pools) {
        wag::error_value e;
        e.cls = wag::error_class::boundary_validation;
        e.where = "new_pool";
        e.got = "已达上限 " + std::to_string(wag::container::max_pools);
        e.want = "更少的池（：池 id 为 u12 且永不复用）";
        e.sentence = "容器池数量已达上限";
        wag::log::record("交界", "new_pool 拒绝了超出上限的池数", e.got);
        wag::fail_boundary(e); // E8 之后会抛
    }
    const std::uint32_t id = eng.containers().create_pool(name);
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    return 1;
}

int l_container_spawn(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer raw_pool =
        read_handle_arg(L, 1, "spawn 的 pool id");
    luaL_checktype(L, 2, LUA_TTABLE);
    if (raw_pool > 0xFFFFFFFFLL) {
        return luaL_error(L, "spawn 的 pool id 越界");
    }

 // 未知键：拒绝（拼错一个字母必须当场报，而不是"这个字段没生效"）
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            luaL_error(L, "spawn 的参数名必须是字符串");
        }
        const std::string_view key{lua_tostring(L, -2)};
        if (!is_spawn_key(key)) {
            luaL_error(L, "spawn 不认识参数 %s（④ 字段不应传给引擎）",
                       std::string(key).c_str());
        }
        lua_pop(L, 1);
    }

    wag::container::container c{};
    c.sx = 1.0f;
    c.sy = 1.0f;
    c.rgba = 0xFFFFFFFFu;
    c.visible = 1;

    table_reader r{L, 2};
    c.x = static_cast<float>(r.number("x", 0.0));
    c.y = static_cast<float>(r.number("y", 0.0));
    c.vx = static_cast<float>(r.number("vx", 0.0));
    c.vy = static_cast<float>(r.number("vy", 0.0));
    c.rot = static_cast<float>(r.number("rot", 0.0));
    c.sx = static_cast<float>(r.number("sx", 1.0));
    c.sy = static_cast<float>(r.number("sy", 1.0));
    c.z = static_cast<float>(r.number("z", 0.0));
    c.layer = static_cast<std::int64_t>(r.integer(
        "layer", 0, std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max()));
    c.rgba = static_cast<std::uint32_t>(r.integer("rgba", 0xFFFFFFFF, 0, 0xFFFFFFFF));
    c.frame = static_cast<std::uint16_t>(r.integer("frame", 0, 0, 65535));
 //：blend 8 档 / vcolor 4 档 / tex_alpha 2 档 —— 越界即拒（不静默取模）
    c.blend = static_cast<std::uint8_t>(r.integer("blend", 0, 0, 7));
    c.vcolor = static_cast<std::uint8_t>(r.integer("vcolor", 0, 0, 3));
    c.tex_alpha = static_cast<std::uint8_t>(r.integer("tex_alpha", 0, 0, 1));
    c.motion = static_cast<std::uint8_t>(r.integer("motion", 0, 0, 255));
    c.group = static_cast<std::uint8_t>(r.integer("group", 0, 0, 255));
    c.class_id = static_cast<std::uint16_t>(r.integer("class", 0, 0, 65535));

 //：未注册的 class ⇒ L3（"用了不存在的东西"，与句柄失效同性质）
    if (!eng.containers().classes().is_registered(c.class_id)) {
        wag::error_value e;
        e.cls = wag::error_class::boundary_validation;
        e.where = "spawn 的 class";
        const std::string got = std::to_string(c.class_id) + "（未注册）";
        e.got = got;
        e.want = "一个已注册的类（/：先 register_class）";
        wag::fail_boundary(e);
    }
    c.img = read_container_img(L, r, "spawn", 0);
 //：frame 越界 ⇒ L2 + 不生效（这里"不生效" = 保持默认 0）
    if (!container_frame_is_legal(eng, c.img, static_cast<std::uint32_t>(c.frame))) {
        c.frame = 0;
    }
    c.collides = r.boolean("collides", false) ? 1 : 0;
    c.visible = r.boolean("visible", true) ? 1 : 0;

    bool has_hit = false;
    hit_entry hit{};
    lua_getfield(L, 2, "hit");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
    } else if (lua_istable(L, -1)) {
        hit = parse_hit(L, lua_gettop(L));
        has_hit = true;
        lua_pop(L, 1);
    } else {
        luaL_error(L, "spawn 的 hit 必须是表或 nil");
    }

    if (c.collides != 0 && !has_hit) {
        luaL_error(L, "collides=true 但没有 hit（：两个问题都必须显式回答）");
    }

 // 【A 类 · 池 id 存在性】原本靠 pool() 内部的 fail_internal（⇒ 终止）
    const std::uint32_t pool_id = require_pool_id(
        eng, static_cast<std::uint32_t>(raw_pool), "spawn 的 pool id");
    container_pool& pool = eng.containers().pool(pool_id);
 // 【C 类 · 池满】本来走 pool.allocate ⇒ fail_internal（⇒ 终止）。
 // 但"池满"是**调用者的行为结果**（spawn 太多），不是引擎 bug ⇒ 该抛。
 // 【为什么此时报错是安全的】槽还没分配、容器一个字节都没写 ⇒ 副作用未发生（E3）。
    handle h;
    wag::container::container* slot = pool.try_allocate(h);
    if (slot == nullptr) {
        const std::string got = pool.full_message();
        wag::error_value e;
        e.cls = wag::error_class::boundary_validation;
        e.where = "spawn 的 pool id（池已满）";
        e.got = got;
        e.want = "更少的同时存活容器，或为该类另开一池";
        e.sentence = "容器池已满";
        wag::log::record("交界", "spawn 拒绝了已满的池", got);
        wag::fail_boundary(e); // E8 之后会抛
    }
    *slot = c;
    if (has_hit) {
        pool.side_table().add(handle_index(h), hit);
    }

    lua_pushinteger(L, static_cast<lua_Integer>(h));
    return 1;
}

int l_container_write(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer raw = read_handle_arg(L, 1, "write 的 handle");
    luaL_checktype(L, 2, LUA_TTABLE);

    wag::container::container* c =
        require_container(eng, static_cast<handle>(raw), "write 的 handle");

 // 【为什么逐字段读，而不是遍历表】
 // 库层每帧把 ④ 表 b 整个传进来；b 里有 img/class/hp 这些 ④ 字段。
 // 遍历就会把"img"当成"运行时改图"来报错（只禁改图，不禁 b 里存着图名）。
 // ⇒ 这里只读**可变的结构体字段**，其余键一概不碰。
    table_reader r{L, 2};
    c->x = static_cast<float>(r.number("x", c->x));
    c->y = static_cast<float>(r.number("y", c->y));
    c->vx = static_cast<float>(r.number("vx", c->vx));
    c->vy = static_cast<float>(r.number("vy", c->vy));
    c->rot = static_cast<float>(r.number("rot", c->rot));
    c->sx = static_cast<float>(r.number("sx", c->sx));
    c->sy = static_cast<float>(r.number("sy", c->sy));
    c->z = static_cast<float>(r.number("z", c->z));
    c->rgba = static_cast<std::uint32_t>(r.integer("rgba", c->rgba, 0, 0xFFFFFFFF));
 //：越界 ⇒ L2 + 不生效（保持原值）；合法集合 [0, n)
    {
        const std::uint16_t want = static_cast<std::uint16_t>(
            r.integer("frame", c->frame, 0, 65535));
        if (container_frame_is_legal(eng, c->img, static_cast<std::uint32_t>(want))) {
            c->frame = want;
        }
    }
    c->blend = static_cast<std::uint8_t>(r.integer("blend", c->blend, 0, 7));
    c->vcolor = static_cast<std::uint8_t>(r.integer("vcolor", c->vcolor, 0, 3));
    c->tex_alpha = static_cast<std::uint8_t>(
        r.integer("tex_alpha", c->tex_alpha, 0, 1));
    c->motion = static_cast<std::uint8_t>(r.integer("motion", c->motion, 0, 255));
    c->visible = r.boolean("visible", c->visible != 0) ? 1 : 0;
    return 0;
}

// -----------------------------------------------------------------------------
// 批量回写（b 方案的"每池一次回写"）
//
// write_batch(pool, n, flat) -> 0
//
// flat 的布局：每对象 17 个数，紧挨着放
// handle, x, y, vx, vy, rot, sx, sy, z, rgba, frame, blend, motion, visible,
// layer, vcolor, tex_alpha
//
// 【为什么不是 FFI】工程的 harden 已经把 ffi/jit 封死（WagLib/harden.lua：
// "ffi 允许脚本直接读写内存 ⇒ 同一项目在不同机器上行为可能不同"）。
// 因此批量回写只能由 C 侧读 Lua 数组 —— 每个数一次 rawgeti/tonumber。
// 这比逐对象 write 省在：**一次跨界** + 没有 13 次哈希 getfield + 没有逐字段判型。
// 【句柄失效 ⇒ L3】：批量里混进一个死句柄，是库层列表没维护对，
// 不能跳过（跳过等于静默丢对象）。
// -----------------------------------------------------------------------------
int l_container_write_batch(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer raw_pool =
        read_handle_arg(L, 1, "write_batch 的 pool id");
    const lua_Integer n = luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    if (raw_pool > 0xFFFFFFFFLL) {
        return luaL_error(L, "write_batch 的 pool id 越界");
    }
    if (n < 0) {
        return luaL_error(L, "write_batch 的行数不能为负");
    }
    const lua_Integer need = n * 17;
 // LuaJIT（5.1）只有 lua_objlen；lua_rawlen 是 5.2 的名字
    if (static_cast<lua_Integer>(lua_objlen(L, 3)) < need) {
        return luaL_error(L, "write_batch：flat 长度不足（需要 %d 个数）",
                          static_cast<int>(need));
    }

 // 【A 类 · 池 id 存在性】原本靠 pool() 内部的 fail_internal（⇒ 终止）
 // **只校验、不取值**：下面的循环改走 require_container 按句柄路由，
 // 所以这里既不需要 pool_id 也不需要 pool 引用（留着会变成未使用变量）。
    (void)require_pool_id(eng, static_cast<std::uint32_t>(raw_pool),
                          "write_batch 的 pool id");

 // =========================================================================
 // D 类 · 两趟走（E3：报错必须在副作用之前）
 //
 // 【为什么不一趟走完】原先"边写边查"：17 个字段已写进容器之后，才检查
 // blend/vcolor/tex_alpha。命中 E3（副作用已发生）⇒ 那一处只能 L4（终止）。
 // 要让它变成可捕获的 L3，必须先把**所有会失败的事**做完，再统一写。
 //
 // 【第一趟为什么只读 4 个数】它要 handle（解析容器）+ 三个范围字段。
 // 其余 13 个字段在任何情况下都写不失败 ⇒ 不必预读。
 //
 // 【顺带修掉一个洞】旧检查拿**已截断成 u8 的值**去比（`c->blend > 7`）：
 // `blend = 256` 截断成 0 ⇒ 静默通过。第一趟拿原始值比，256 会被拦下。
 // 这正是原注释"越界即 L3，不静默取模"想要而没做到的。
 // =========================================================================
    auto int_at = [L](lua_Integer idx) -> lua_Integer {
        lua_rawgeti(L, 3, idx);
        const lua_Integer v = lua_tointeger(L, -1);
        lua_pop(L, 1);
        return v;
    };

 // ---- 第一趟：解析容器 + 校验范围。**一个字段都不写。**
    std::vector<wag::container::container*> targets;
    targets.reserve(static_cast<std::size_t>(n));
    for (lua_Integer i = 0; i < n; ++i) {
        const lua_Integer b = i * 17;

        lua_rawgeti(L, 3, b + 1);
        const lua_Integer raw_handle = handle_from_number(
            static_cast<double>(lua_tonumber(L, -1)), "write_batch 的 handle");
        lua_pop(L, 1);

 // 【B 类 · 死句柄】原本 try_get 失败后 fail_internal ⇒ 终止
        targets.push_back(require_container(
            eng, static_cast<handle>(raw_handle), "write_batch 的 handle"));

        const lua_Integer blend = int_at(b + 12);
        const lua_Integer vcolor = int_at(b + 16);
        const lua_Integer tex_alpha = int_at(b + 17);
        if (blend < 0 || blend > 7 || vcolor < 0 || vcolor > 3 ||
            tex_alpha < 0 || tex_alpha > 1) {
 // 【D 类 · 原本是"写完之后"的 fail_internal（⇒ 终止）】
            std::string got = "第 " + std::to_string(i) + " 行：blend=" +
                              std::to_string(blend) + " vcolor=" +
                              std::to_string(vcolor) + " tex_alpha=" +
                              std::to_string(tex_alpha);
            wag::error_value e;
            e.cls = wag::error_class::boundary_validation;
            e.where = "write_batch 的 flat（ 三态）";
            e.got = got;
            e.want = "blend 0..7 / vcolor 0..3 / tex_alpha 0..1";
            e.sentence = "批量回写：三态字段越界（整批未写入）";
            wag::log::record("交界", "write_batch 拒绝越界的三态字段", got);
            wag::fail_boundary(e); // E8 之后会抛
        }
    }

 // ---- 第二趟：写入。**这一趟不可能失败**（只读 Lua 栈 + 赋值）
 // ⇒ "整批检查全对才放行"在结构上成立，不靠纪律。
    for (lua_Integer i = 0; i < n; ++i) {
        const lua_Integer b = i * 17;
        wag::container::container* c = targets[static_cast<std::size_t>(i)];

        lua_rawgeti(L, 3, b + 2);
        c->x = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 3, b + 3);
        c->y = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 3, b + 4);
        c->vx = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 3, b + 5);
        c->vy = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 3, b + 6);
        c->rot = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 3, b + 7);
        c->sx = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 3, b + 8);
        c->sy = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 3, b + 9);
        c->z = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 3, b + 10);
        c->rgba = static_cast<std::uint32_t>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 3, b + 11);
        c->frame = static_cast<std::uint16_t>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        c->blend = static_cast<std::uint8_t>(int_at(b + 12));
        c->motion = static_cast<std::uint8_t>(int_at(b + 13));
        c->visible = static_cast<std::uint8_t>(int_at(b + 14));
        c->layer = static_cast<std::int64_t>(int_at(b + 15));
        c->vcolor = static_cast<std::uint8_t>(int_at(b + 16));
        c->tex_alpha = static_cast<std::uint8_t>(int_at(b + 17));
    }
    return 0;
}

int l_container_kill(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer raw = read_handle_arg(L, 1, "kill 的 handle");
    const handle h = static_cast<handle>(raw);

 // 【B 类 · 死句柄】原本 get ⇒ fail_internal ⇒ 终止
    (void)require_container(eng, h, "kill 的 handle");

    container_pool& pool = eng.containers().pool(handle_pool(h));
    const std::uint32_t index = handle_index(h);
    if (pool.side_table().find(index) != nullptr) {
        pool.side_table().remove(index);
    }
    eng.containers().kill(h); // 标记 dead；帧末 recycle
    return 0;
}

// -----------------------------------------------------------------------------
//：容器计数查询（当前帧；pool / group 非法 ⇒ L3）
// -----------------------------------------------------------------------------

namespace {

std::optional<std::uint8_t> read_count_group(lua_State* L, int idx,
                                             const char* where) {
    if (lua_isnoneornil(L, idx)) {
        return std::nullopt;
    }
    const lua_Number v = luaL_checknumber(L, idx);
    if (!std::isfinite(static_cast<double>(v)) ||
        std::floor(static_cast<double>(v)) != static_cast<double>(v) ||
        v < 0.0 || v > 255.0) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = where;
        e.got = std::to_string(static_cast<double>(v));
        e.want = "0..255 的整数；省略 / nil = 全组";
        fail_boundary(e);
    }
    return static_cast<std::uint8_t>(v);
}

std::uint32_t read_count_pool(lua_State* L, int idx, const char* where) {
    const lua_Integer raw = read_handle_arg(L, idx, where);
    if (raw < 0 || raw > 0xFFFFFFFFLL) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = where;
        e.got = std::to_string(static_cast<long long>(raw));
        e.want = "new_pool 返回的池 id（0..2^32-1）";
        fail_boundary(e);
    }
    return require_pool_id(engine_of(L), static_cast<std::uint32_t>(raw), where);
}

} // namespace

int l_container_count_live(lua_State* L) {
    auto& eng = engine_of(L);
    const std::uint32_t pool = read_count_pool(L, 1, "count_live 的 pool");
    const auto group = read_count_group(L, 2, "count_live 的 group");
    const auto n = eng.containers().count_objects(pool, group);
    lua_pushinteger(L, static_cast<lua_Integer>(n.live));
    return 1;
}

int l_container_count_occupied(lua_State* L) {
    auto& eng = engine_of(L);
    const std::uint32_t pool = read_count_pool(L, 1, "count_occupied 的 pool");
    const auto group = read_count_group(L, 2, "count_occupied 的 group");
    const auto n = eng.containers().count_objects(pool, group);
    lua_pushinteger(L, static_cast<lua_Integer>(n.occupied));
    return 1;
}

int l_container_count_renderable(lua_State* L) {
    auto& eng = engine_of(L);
    const std::uint32_t pool = read_count_pool(L, 1, "count_renderable 的 pool");
    const auto group = read_count_group(L, 2, "count_renderable 的 group");
    const auto n = eng.containers().count_objects(pool, group);
    lua_pushinteger(L, static_cast<lua_Integer>(n.renderable));
    return 1;
}

int l_container_count_live_all(lua_State* L) {
    auto& eng = engine_of(L);
    const auto group = read_count_group(L, 1, "count_live_all 的 group");
    const auto n = eng.containers().count_objects_all(group);
    lua_pushinteger(L, static_cast<lua_Integer>(n.live));
    return 1;
}

int l_container_count_occupied_all(lua_State* L) {
    auto& eng = engine_of(L);
    const auto group = read_count_group(L, 1, "count_occupied_all 的 group");
    const auto n = eng.containers().count_objects_all(group);
    lua_pushinteger(L, static_cast<lua_Integer>(n.occupied));
    return 1;
}

int l_container_count_renderable_all(lua_State* L) {
    auto& eng = engine_of(L);
    const auto group = read_count_group(L, 1, "count_renderable_all 的 group");
    const auto n = eng.containers().count_objects_all(group);
    lua_pushinteger(L, static_cast<lua_Integer>(n.renderable));
    return 1;
}

int l_container_alive(lua_State* L) {
    auto& eng = engine_of(L);
    const double n = static_cast<double>(luaL_checknumber(L, 1));
    lua_Integer raw = 0;
    const bool exact = exact_handle(n, raw);
    lua_pushboolean(L, exact &&
                           eng.containers().try_get(static_cast<handle>(raw)) != nullptr);
    return 1;
}

int l_container_set_hit(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer raw = read_handle_arg(L, 1, "set_hit 的 handle");
    luaL_checktype(L, 2, LUA_TTABLE);

    const handle h = static_cast<handle>(raw);
 // 【B 类 · 死句柄】原本 get ⇒ fail_internal ⇒ 终止
    wag::container::container* c = require_container(eng, h, "set_hit 的 handle");
    if (c->collides == 0) {
        return luaL_error(L, "collides=false 的容器不能设 hit（：免不免疫用 ④ 的 hit_enabled）");
    }

    const hit_entry e = parse_hit(L, 2);
    container_pool& pool = eng.containers().pool(handle_pool(h));
    const std::uint32_t index = handle_index(h);
    if (pool.side_table().find(index) != nullptr) {
        pool.side_table().update(index, e);
    } else {
        pool.side_table().add(index, e);
    }
    return 0;
}

// -----------------------------------------------------------------------------
// wag.container.replace(old, params) -> new_handle
//
// 【语义】① 字段全量拷贝旧容器；img 由 params 给（不给则沿用）；
// 先建后毁（新容器分配成功才 kill 旧的，满池 ⇒ L3，旧容器不丢）；
// 旧句柄立即失效；碰撞条目重新登记。
// 【不可变字段】class / group / collides 从旧容器拷；params 里出现 ⇒ L2。
// -----------------------------------------------------------------------------
int l_container_replace(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer raw = read_handle_arg(L, 1, "replace 的 old_handle");
    luaL_checktype(L, 2, LUA_TTABLE);
    const handle old_h = static_cast<handle>(raw);

 // 旧容器必须有效（无效 ⇒ 交界处报 boundary，）
 // 【B 类 · 死句柄】原本 get ⇒ fail_internal ⇒ 终止
    wag::container::container* old_c =
        require_container(eng, old_h, "replace 的 old_handle");

 // 不可变字段：拒绝并**不改任何东西**（L2，可继续）
    static constexpr const char* forbidden[] = {"class", "group", "collides"};
    for (const char* key : forbidden) {
        lua_getfield(L, 2, key);
        const bool present = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (present) {
            wag::fail_script(std::string("replace 不接受 ") + key +
                             "（创建后不可变，///）");
            return 0;
        }
    }

 // 未知键：拒绝（与 spawn 同口径；img 与 hit 在 is_spawn_key 里）
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            luaL_error(L, "replace 的参数名必须是字符串");
        }
        const std::string_view key{lua_tostring(L, -2)};
        if (!is_spawn_key(key)) {
            luaL_error(L, "replace 不认识参数 %s（④ 字段不应传给引擎）",
                       std::string(key).c_str());
        }
        lua_pop(L, 1);
    }

 // ① 字段：从旧容器全量拷贝，params 覆盖
    wag::container::container c = *old_c;
    table_reader r{L, 2};
    c.x = static_cast<float>(r.number("x", old_c->x));
    c.y = static_cast<float>(r.number("y", old_c->y));
    c.vx = static_cast<float>(r.number("vx", old_c->vx));
    c.vy = static_cast<float>(r.number("vy", old_c->vy));
    c.rot = static_cast<float>(r.number("rot", old_c->rot));
    c.sx = static_cast<float>(r.number("sx", old_c->sx));
    c.sy = static_cast<float>(r.number("sy", old_c->sy));
    c.z = static_cast<float>(r.number("z", old_c->z));
    c.layer = static_cast<std::int64_t>(r.integer(
        "layer", old_c->layer, std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max()));
    c.rgba = static_cast<std::uint32_t>(r.integer("rgba", old_c->rgba, 0, 0xFFFFFFFF));
    c.frame = static_cast<std::uint16_t>(r.integer("frame", old_c->frame, 0, 65535));
    c.blend = static_cast<std::uint8_t>(r.integer("blend", old_c->blend, 0, 255));
    c.motion = static_cast<std::uint8_t>(r.integer("motion", old_c->motion, 0, 255));
    c.img = read_container_img(L, r, "write", old_c->img);
 //：换图之后 frame 也要重新对一次合法集合（越界 ⇒ L2 + 回落到 0）
    if (!container_frame_is_legal(eng, c.img, static_cast<std::uint32_t>(c.frame))) {
        c.frame = 0;
    }
    c.visible = r.boolean("visible", old_c->visible != 0) ? 1 : 0;

 // hit：params 给则用；不给则沿用旧形状（的碰撞条目重登记）
    bool has_hit = false;
    hit_entry hit{};
    lua_getfield(L, 2, "hit");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            luaL_error(L, "replace 的 hit 必须是表或 nil");
        }
        hit = parse_hit(L, lua_gettop(L));
        has_hit = true;
    }
    lua_pop(L, 1);

    if (c.collides == 0 && has_hit) {
        wag::fail_script("replace：collides=false 的容器不能给 hit");
        return 0;
    }

    container_pool& pool = eng.containers().pool(handle_pool(old_h));
    const std::uint32_t old_index = handle_index(old_h);

 // collides 从旧容器拷；若为 true，必须有一个判定形状
    hit_entry old_hit{};
    bool had_hit = false;
    if (c.collides != 0) {
        const hit_entry* found = pool.side_table().find(old_index);
        if (found != nullptr) {
            old_hit = *found; // 先拷贝：add 可能让 entries 重新分配
            had_hit = true;
        }
        if (!has_hit && !had_hit) {
            wag::fail_internal("replace：collides=true 但旧容器没有判定条目");
        }
    }

 // 先建：满池 ⇒ 交界处报（C 类），**此时旧容器仍在、新槽未分配 ⇒ 副作用未发生**
    handle new_h;
    wag::container::container* slot = pool.try_allocate(new_h);
    if (slot == nullptr) {
        const std::string got = pool.full_message();
        wag::error_value e;
        e.cls = wag::error_class::boundary_validation;
        e.where = "replace 的新容器分配（池已满）";
        e.got = got;
        e.want = "更少的同时存活容器，或为该类另开一池";
        e.sentence = "容器池已满（replace 未改动旧的）";
        wag::log::record("交界", "replace 拒绝了已满的池", got);
        wag::fail_boundary(e); // E8 之后会抛
    }
    *slot = c;

 // 碰撞条目：新条目先加，旧条目随旧容器移除
    if (c.collides != 0) {
        pool.side_table().add(handle_index(new_h), has_hit ? hit : old_hit);
    }
    if (pool.side_table().find(old_index) != nullptr) {
        pool.side_table().remove(old_index);
    }

 // 后毁：旧句柄立即失效
    eng.containers().kill(old_h);

    lua_pushinteger(L, static_cast<lua_Integer>(new_h));
    return 1;
}

// -----------------------------------------------------------------------------
// wag.collide.check(gA, gB, cb) ——
// wag.collide.check_one(handle, gB, cb) —— 的快路径
//
// 【只声明，不执行】引擎在阶段 6 的 Main 分支末尾统一执行：
// 那时本帧的位置已全部写好，宽相/窄相看到的才是最终位置。
// 【回调】cb 收到一个扁平数组 [a1,b1,a2,b2,…]（的批量报告）；
// "删不删/掉不掉血"由脚本决定，引擎不处置。
// -----------------------------------------------------------------------------
int l_collide_check(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer ga = luaL_checkinteger(L, 1);
    const lua_Integer gb = luaL_checkinteger(L, 2);
    if (ga < 0 || ga > 255 || gb < 0 || gb > 255) {
        return luaL_error(L, "collide.check 的组号必须在 0..255");
    }
    if (!lua_isfunction(L, 3)) {
        return luaL_error(L, "collide.check 的第三个参数必须是函数");
    }
    for (const auto& r : eng.collision_requests()) {
        if (r.k == collision_request::kind::pair && r.group_a == ga &&
            r.group_b == gb) {
            wag::fail_script(
                "同一帧重复声明同一组对（：每帧每组对最多一次回调）");
            return 0;
        }
    }
    lua_pushvalue(L, 3);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    eng.collision_requests().push_back(collision_request{
        collision_request::kind::pair, static_cast<std::uint8_t>(ga),
        static_cast<std::uint8_t>(gb), 0, ref});
    return 0;
}

int l_collide_check_one(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer raw = read_handle_arg(L, 1, "check_one 的 handle");
    const lua_Integer gb = luaL_checkinteger(L, 2);
    if (gb < 0 || gb > 255) {
        return luaL_error(L, "collide.check_one 的组号必须在 0..255");
    }
    if (!lua_isfunction(L, 3)) {
        return luaL_error(L, "collide.check_one 的第三个参数必须是函数");
    }
    const handle h = static_cast<handle>(raw);
 // 【B 类 · 死句柄】原本 get ⇒ fail_internal ⇒ 终止
    const wag::container::container* self =
        require_container(eng, h, "check_one 的 handle");
 // 【A 类（附在本入口内）· 没有判定条目】原本走到 collide.cpp 的 fail_internal ⇒ 终止。
 // 它必须在**取到容器之后、任何副作用之前**判 —— 这里正是那个位置。
    if (self->collides == 0) {
        wag::error_value e;
        e.cls = wag::error_class::boundary_validation;
        e.where = "check_one 的 handle";
        e.got = "该容器 collides=false（没有判定条目）";
        e.want = "一个 collides=true 的容器";
        e.sentence = "check_one 需要一个有判定形状的容器";
        wag::log::record("交界", "check_one 拒绝了 collides=false 的容器", std::to_string(h));
        wag::fail_boundary(e); // E8 之后会抛
    }
    for (const auto& r : eng.collision_requests()) {
        if (r.k == collision_request::kind::one && r.handle == h &&
            r.group_b == gb) {
            wag::fail_script("同一帧重复声明同一容器/组对");
            return 0;
        }
    }
    lua_pushvalue(L, 3);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    eng.collision_requests().push_back(collision_request{
        collision_request::kind::one, 0, static_cast<std::uint8_t>(gb),
        static_cast<std::uint64_t>(h), ref});
    return 0;
}

// -----------------------------------------------------------------------------
// wag.collide.stats() -> { ms, requests, items, candidates, hits }
//
// 【只读统计】最近一次 run_collisions 的墙钟与四段计数；不改变碰撞语义。
// 没有请求的帧会把它清零（所以读到的永远是"上一帧"）。
// -----------------------------------------------------------------------------
int l_collide_stats(lua_State* L) {
    const collision_stats& st = engine_of(L).collision_stats_ref();
    lua_createtable(L, 0, 5);
    lua_pushnumber(L, st.last_ms);
    lua_setfield(L, -2, "ms");
    lua_pushinteger(L, static_cast<lua_Integer>(st.requests));
    lua_setfield(L, -2, "requests");
    lua_pushinteger(L, static_cast<lua_Integer>(st.items));
    lua_setfield(L, -2, "items");
    lua_pushinteger(L, static_cast<lua_Integer>(st.candidates));
    lua_setfield(L, -2, "candidates");
    lua_pushinteger(L, static_cast<lua_Integer>(st.hits));
    lua_setfield(L, -2, "hits");
    return 1;
}

} // namespace
#endif // WAG_HAVE_LUAJIT

// =============================================================================
// wag.input —— 输入查询（I1–I14）
//
// 【为什么是函数，不是每帧一张表】
// · 每帧建表 = 每帧分配 + 构造；函数是一次跨界、零分配；
// · 三态各一个函数，语义与 I1 一一对应。
//
// 【为什么只收数字】
// 名字表在 Lua 侧（Lkeycode）；引擎如果也认名字，就会出现第二份映射，
// 而"名字 → scancode"必须只有一个来源（否则改键会两边不一致）。
//
// 【越界为什么是 L3】
// 用错 scancode/button 是作者错误；静默返回 false 会让它永远查不出来
//（公理 20：可判定）。与绘制入口的边界校验同一口径。
// =============================================================================

namespace {

void check_input_scancode(lua_Integer v) {
    if (v < 0 || v >= static_cast<lua_Integer>(platform::scancode_count)) {
        const std::string got = std::to_string(v);
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "wag.input 的 scancode";
        e.got = got;
        e.want = "0..511（SDL scancode；名字请用 Lua 侧 Lkeycode）";
        fail_boundary(e);
    }
}

void check_input_button(lua_Integer v) {
    if (v < 1 || v > static_cast<lua_Integer>(platform::mouse_button_count)) {
        const std::string got = std::to_string(v);
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "wag.input 的鼠标按钮";
        e.got = got;
        e.want = "1..5（LEFT=1 / MIDDLE=2 / RIGHT=3 / X1=4 / X2=5）";
        fail_boundary(e);
    }
}

void check_input_gamepad_device(lua_Integer v) {
    if (v < 0 || v >= static_cast<lua_Integer>(platform::gamepad_slot_count)) {
        const std::string got = std::to_string(v);
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "wag.input 的 gamepad device";
        e.got = got;
        e.want = "0..7（gamepad slot）";
        fail_boundary(e);
    }
}

void check_input_gamepad_button(lua_Integer v) {
    if (v < 0 || v >= static_cast<lua_Integer>(platform::gamepad_button_count)) {
        const std::string got = std::to_string(v);
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "wag.input 的 gamepad button";
        e.got = got;
        e.want = "0..25（SDL_GAMEPAD_BUTTON_*）";
        fail_boundary(e);
    }
}

void check_input_gamepad_axis(lua_Integer v) {
    if (v < 0 || v >= static_cast<lua_Integer>(platform::gamepad_axis_count)) {
        const std::string got = std::to_string(v);
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "wag.input 的 gamepad axis";
        e.got = got;
        e.want = "0..5（SDL_GAMEPAD_AXIS_*）";
        fail_boundary(e);
    }
}

} // namespace

int l_input_down(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer raw = luaL_checkinteger(L, 1);
    check_input_scancode(raw);
    lua_pushboolean(L, eng.input_system().down(static_cast<std::uint16_t>(raw)) ? 1 : 0);
    return 1;
}

int l_input_press(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer raw = luaL_checkinteger(L, 1);
    check_input_scancode(raw);
    lua_pushboolean(L, eng.input_system().press(static_cast<std::uint16_t>(raw)) ? 1 : 0);
    return 1;
}

int l_input_release(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer raw = luaL_checkinteger(L, 1);
    check_input_scancode(raw);
    lua_pushboolean(L, eng.input_system().release(static_cast<std::uint16_t>(raw)) ? 1 : 0);
    return 1;
}

int l_input_mouse_pos(lua_State* L) {
    auto& eng = engine_of(L);
    lua_pushnumber(L, eng.input_system().mouse_x());
    lua_pushnumber(L, eng.input_system().mouse_y());
    return 2;
}

int l_input_mouse_down(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer raw = luaL_checkinteger(L, 1);
    check_input_button(raw);
    lua_pushboolean(L, eng.input_system().mouse_down(
                           static_cast<std::uint8_t>(raw)) ? 1 : 0);
    return 1;
}

int l_input_mouse_press(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer raw = luaL_checkinteger(L, 1);
    check_input_button(raw);
    lua_pushboolean(L, eng.input_system().mouse_press(
                           static_cast<std::uint8_t>(raw)) ? 1 : 0);
    return 1;
}

int l_input_mouse_release(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer raw = luaL_checkinteger(L, 1);
    check_input_button(raw);
    lua_pushboolean(L, eng.input_system().mouse_release(
                           static_cast<std::uint8_t>(raw)) ? 1 : 0);
    return 1;
}

int l_input_wheel(lua_State* L) {
    auto& eng = engine_of(L);
    lua_pushnumber(L, eng.input_system().wheel_x());
    lua_pushnumber(L, eng.input_system().wheel_y());
    return 2;
}

int l_input_gamepad_devices(lua_State* L) {
    auto& eng = engine_of(L);
    const auto devices = eng.input_system().gamepad_devices();
    lua_createtable(L, static_cast<int>(devices.size()), 0);
    for (std::size_t i = 0; i < devices.size(); ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(devices[i]));
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int l_input_gamepad_connected(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer raw = luaL_checkinteger(L, 1);
    check_input_gamepad_device(raw);
    lua_pushboolean(L, eng.input_system().gamepad_connected(
                           static_cast<std::uint16_t>(raw)) ? 1 : 0);
    return 1;
}

int l_input_gamepad_down(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer device = luaL_checkinteger(L, 1);
    const lua_Integer button = luaL_checkinteger(L, 2);
    check_input_gamepad_device(device);
    check_input_gamepad_button(button);
    lua_pushboolean(L, eng.input_system().gamepad_down(
                           static_cast<std::uint16_t>(device),
                           static_cast<std::uint8_t>(button)) ? 1 : 0);
    return 1;
}

int l_input_gamepad_press(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer device = luaL_checkinteger(L, 1);
    const lua_Integer button = luaL_checkinteger(L, 2);
    check_input_gamepad_device(device);
    check_input_gamepad_button(button);
    lua_pushboolean(L, eng.input_system().gamepad_press(
                           static_cast<std::uint16_t>(device),
                           static_cast<std::uint8_t>(button)) ? 1 : 0);
    return 1;
}

int l_input_gamepad_release(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer device = luaL_checkinteger(L, 1);
    const lua_Integer button = luaL_checkinteger(L, 2);
    check_input_gamepad_device(device);
    check_input_gamepad_button(button);
    lua_pushboolean(L, eng.input_system().gamepad_release(
                           static_cast<std::uint16_t>(device),
                           static_cast<std::uint8_t>(button)) ? 1 : 0);
    return 1;
}

int l_input_gamepad_axis(lua_State* L) {
    auto& eng = engine_of(L);
    const lua_Integer device = luaL_checkinteger(L, 1);
    const lua_Integer axis = luaL_checkinteger(L, 2);
    check_input_gamepad_device(device);
    check_input_gamepad_axis(axis);
    lua_pushnumber(L, eng.input_system().gamepad_axis(
                          static_cast<std::uint16_t>(device),
                          static_cast<std::uint8_t>(axis)));
    return 1;
}

// =============================================================================
// wag.light —— 光照原语（2026-11-18，Part 6）
//
// 【校验在这里】字段必填性、类型、非有限值都在这一层判（D6.4 / D6.5b）：
// · 非有限值（NaN / ±Inf）⇒ 报错（L2；档位映射见后续）
// · 池满 / 池不存在 / 死句柄 ⇒ 报错，不静默
// 【本组只做"递交"】光的渲染在渲染器帧末（render_light_pass_）。
// =============================================================================

double l_light_num(lua_State* L, int idx, const char* name) {
    lua_getfield(L, idx, name);
    if (!lua_isnumber(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "light 字段 %s 必须是数字", name);
        return 0.0;
    }
    const double v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (!std::isfinite(v)) {
        luaL_error(L, "light 字段 %s 必须有限（NaN/Inf 非法）", name);
        return 0.0;
    }
    return v;
}

double l_light_opt_num(lua_State* L, int idx, const char* name, double def) {
    lua_getfield(L, idx, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return def;
    }
    if (!lua_isnumber(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "light 字段 %s 必须是数字", name);
        return def;
    }
    const double v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (!std::isfinite(v)) {
        luaL_error(L, "light 字段 %s 必须有限（NaN/Inf 非法）", name);
    }
    return v;
}

/// wag.light.add{...}
int l_light_add(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    light::light l;

    lua_getfield(L, 1, "kind");
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "light.add 缺少 kind（point/cone/beam/quad）");
    }
    const std::string kind = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (kind == "point") {
        l.k = light::kind::point;
    } else if (kind == "cone") {
        l.k = light::kind::cone;
    } else if (kind == "beam") {
        l.k = light::kind::beam;
    } else if (kind == "quad") {
        l.k = light::kind::quad;
    } else {
        return luaL_error(L, "light.add 的 kind 未知：%s", kind.c_str());
    }

    l.x = static_cast<float>(l_light_num(L, 1, "x"));
    l.y = static_cast<float>(l_light_num(L, 1, "y"));

    lua_getfield(L, 1, "radius");
    const bool has_radius = lua_isnumber(L, -1) != 0;
    lua_pop(L, 1);
    lua_getfield(L, 1, "width");
    const bool has_width = lua_isnumber(L, -1) != 0;
    lua_pop(L, 1);
    if (!has_radius && !has_width) {
        return luaL_error(L, "light.add 需要 radius 或 width");
    }
    if (has_radius) {
        l.radius = static_cast<float>(l_light_num(L, 1, "radius"));
    }
    if (has_width) {
        l.width = static_cast<float>(l_light_num(L, 1, "width"));
    }

    lua_getfield(L, 1, "color");
    if (!lua_isnumber(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "light.add 缺少 color（0xAARRGGBB）");
    }
    l.color = static_cast<std::uint32_t>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    l.intensity = static_cast<float>(l_light_num(L, 1, "intensity"));

    lua_getfield(L, 1, "layer");
    if (!lua_isnumber(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "light.add 缺少 layer");
    }
    l.layer = static_cast<std::int64_t>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, 1, "reach");
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "light.add 缺少 reach（below/self_only）");
    }
    const std::string reach = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (reach == "below") {
        l.r = light::reach::below;
    } else if (reach == "self_only") {
        l.r = light::reach::self_only;
    } else {
        return luaL_error(L, "light.add 的 reach 未知：%s", reach.c_str());
    }

    const double depth = l_light_num(L, 1, "depth");
    if (depth < 0.0 || depth > 255.0) {
        return luaL_error(L, "light.add 的 depth 必须在 0..255");
    }
    l.depth = static_cast<std::uint8_t>(depth);

    l.angle = static_cast<float>(l_light_opt_num(L, 1, "angle", 0.0));
    l.spread = static_cast<float>(l_light_opt_num(L, 1, "spread", 0.0));
    l.softness = static_cast<float>(l_light_opt_num(L, 1, "softness", 2.0));
    l.falloff = static_cast<float>(l_light_opt_num(L, 1, "falloff", 1.0));

    if (!engine_of(L).renderer().light_layer_in_band(l.layer)) {
        return luaL_error(L, "light.add 的 layer 不在任何已声明的带内");
    }
    if (engine_of(L).renderer().light_count() >= 4096) {
        return luaL_error(L, "光源数超过硬上限 4096");
    }
    engine_of(L).renderer().light_add(l);
    return 0;
}

/// wag.light.exposure(v)
/// wag.light.band{ id, from, to }
int l_light_band(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "id");
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "light.band 缺少 id（字符串）");
    }
    const std::string id = lua_tostring(L, -1);
    lua_pop(L, 1);
    const double from = l_light_num(L, 1, "from");
    const double to = l_light_num(L, 1, "to");
    std::string err;
    if (!engine_of(L).renderer().light_band_declare(
            id, static_cast<std::int64_t>(from), static_cast<std::int64_t>(to), err)) {
        return luaL_error(L, "light.band 失败：%s", err.c_str());
    }
    return 0;
}

/// wag.light.commit(id)
int l_light_commit(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    std::string err;
    if (!engine_of(L).renderer().light_commit(id, err)) {
        return luaL_error(L, "light.commit 失败：%s", err.c_str());
    }
    return 0;
}

int l_light_exposure(lua_State* L) {
    const double v = luaL_checknumber(L, 1);
    if (!std::isfinite(v)) {
        return luaL_error(L, "light.exposure 必须有限（NaN/Inf 非法）");
    }
    engine_of(L).renderer().light_set_exposure(static_cast<float>(v));
    return 0;
}

/// wag.light.load_shader(dir)
int l_light_load_shader(lua_State* L) {
    const char* dir = luaL_checkstring(L, 1);
    std::string err;
    if (!engine_of(L).renderer().light_load_shader(dir, err)) {
        return luaL_error(L, "light.load_shader 失败：%s", err.c_str());
    }
    return 0;
}

/// wag.light.stats()
int l_light_stats(lua_State* L) {
    auto& r = engine_of(L).renderer();
    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(r.light_frame_lights()));
    lua_setfield(L, -2, "lights");
    lua_pushinteger(L, static_cast<lua_Integer>(r.occluder_count()));
    lua_setfield(L, -2, "occluders");
    lua_pushnumber(L, static_cast<lua_Number>(r.light_exposure()));
    lua_setfield(L, -2, "exposure");
    lua_pushinteger(L, static_cast<lua_Integer>(r.light_band_count()));
    lua_setfield(L, -2, "bands");
    lua_pushinteger(L, static_cast<lua_Integer>(r.light_frame_commits()));
    lua_setfield(L, -2, "commits");
    lua_pushnumber(L, static_cast<lua_Number>(r.light_fill_ratio_now()));
    lua_setfield(L, -2, "fill_ratio");
    return 1;
}

// ---- 遮挡者池 ----

int l_light_occluders_new(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (!engine_of(L).renderer().light_occluders_new(name)) {
        return luaL_error(L, "遮挡者池已存在：%s", name);
    }
    return 0;
}

light::occluder l_light_read_occluder(lua_State* L, int idx) {
    light::occluder o;
    o.x = static_cast<float>(l_light_num(L, idx, "x"));
    o.y = static_cast<float>(l_light_num(L, idx, "y"));
    o.w = static_cast<float>(l_light_num(L, idx, "w"));
    o.h = static_cast<float>(l_light_num(L, idx, "h"));
    o.rot = static_cast<float>(l_light_opt_num(L, idx, "rot", 0.0));
    const double depth = l_light_opt_num(L, idx, "depth", 128.0);
    const double trans = l_light_opt_num(L, idx, "transmission", 0.0);
    if (depth < 0.0 || depth > 255.0 || trans < 0.0 || trans > 255.0) {
        luaL_error(L, "遮挡者的 depth/transmission 必须在 0..255");
    }
    o.depth = static_cast<std::uint8_t>(depth);
    o.transmission = static_cast<std::uint8_t>(trans);
    lua_getfield(L, idx, "image");
    if (lua_isnumber(L, -1)) {
        o.image = static_cast<std::uint64_t>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
    return o;
}

int l_light_occluders_spawn(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const light::occluder o = l_light_read_occluder(L, 2);
    const std::uint64_t h = engine_of(L).renderer().light_occluders_spawn(name, o);
    if (h == 0) {
        return luaL_error(L, "遮挡者池不存在或已满：%s", name);
    }
    lua_pushnumber(L, static_cast<lua_Number>(h));
    return 1;
}

int l_light_occluders_write(lua_State* L) {
    const std::uint64_t h = static_cast<std::uint64_t>(luaL_checknumber(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    const light::occluder o = l_light_read_occluder(L, 2);
    if (!engine_of(L).renderer().light_occluders_write(h, o)) {
        return luaL_error(L, "遮挡者句柄已失效");
    }
    return 0;
}

int l_light_occluders_kill(lua_State* L) {
    const std::uint64_t h = static_cast<std::uint64_t>(luaL_checknumber(L, 1));
    if (!engine_of(L).renderer().light_occluders_kill(h)) {
        return luaL_error(L, "遮挡者句柄已失效");
    }
    return 0;
}

int l_light_occluders_alive(lua_State* L) {
    const std::uint64_t h = static_cast<std::uint64_t>(luaL_checknumber(L, 1));
    lua_pushboolean(L, engine_of(L).renderer().light_occluders_alive(h) ? 1 : 0);
    return 1;
}

// ---- wag.fs.list_files(path, recursive?) ----
//
// 返回目录下普通文件的相对路径（统一 `/`），按字典序排序。
//
// 【为什么需要它】WagLib.resolve 要按层枚举 .lua 文件；纯 Lua 5.1
// io 库不能列目录。目录不存在/不可读 ⇒ 空表（缺失一层是合法状态）。
// 【不跟随符号链接】避免脚本通过链接把扫描带出预期目录。
// 【只返回普通文件】目录本身不返回；调用方自己筛扩展名。
int l_fs_list_files(lua_State* L) {
    const char* raw = luaL_checkstring(L, 1);
    const bool recursive = (lua_gettop(L) >= 2) && (lua_toboolean(L, 2) != 0);

    std::vector<std::string> files;
    try {
        const std::filesystem::path root = wag::paths::path_from_utf8(raw);
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) {
            lua_createtable(L, 0, 0);
            return 1;
        }

        auto add = [&files, &root](const std::filesystem::directory_entry& e) {
            std::error_code sec;
            if (e.is_symlink(sec) || !e.is_regular_file(sec)) {
                return;
            }
            std::string rel = wag::paths::utf8_from_path(
                e.path().lexically_relative(root));
 // 脚本侧约定用 `/`；Windows 原生路径可能给 `\`。
            std::replace(rel.begin(), rel.end(), '\\', '/');
            if (!rel.empty()) {
                files.push_back(rel);
            }
        };

        if (recursive) {
            for (const auto& e : std::filesystem::recursive_directory_iterator(
                     root, std::filesystem::directory_options::skip_permission_denied)) {
                add(e);
            }
        } else {
            for (const auto& e : std::filesystem::directory_iterator(
                     root, std::filesystem::directory_options::skip_permission_denied)) {
                add(e);
            }
        }
    } catch (...) {
        files.clear();
    }

    std::sort(files.begin(), files.end());

    lua_createtable(L, static_cast<int>(files.size()), 0);
    for (std::size_t i = 0; i < files.size(); ++i) {
        lua_pushlstring(L, files[i].data(), files[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

void register_primitives(state& st, wag::engine& eng) {
    if (!st.is_created()) {
        wag::fail_internal("注册引擎原语时脚本状态尚未建立");
    }
#if defined(WAG_HAVE_LUAJIT)
    lua_State* L = st.raw();
    bind_engine(L, eng);


    lua_getfield(L, (-10002), ("wag"));
    if (!(lua_type(L, (-1)) == 5)) {
        wag::fail_internal("注册引擎原语时 wag 表不存在（步骤 0 未生效）");
    }


    lua_createtable(L, 0, 0);
    lua_pushcclosure(L, (l_engine_pause), 0); lua_setfield(L, -2, "pause");
    lua_pushcclosure(L, (l_engine_resume), 0); lua_setfield(L, -2, "resume");
    lua_pushcclosure(L, (l_engine_get_state), 0); lua_setfield(L, -2, "get_state");
    lua_pushcclosure(L, (l_engine_exit), 0); lua_setfield(L, -2, "exit");
    lua_setfield(L, -2, "engine");


    lua_createtable(L, 0, 0);
    lua_pushcclosure(L, (l_space_set_viewport), 0); lua_setfield(L, -2, "set_viewport");
    lua_pushcclosure(L, (l_space_get_viewport), 0); lua_setfield(L, -2, "get_viewport");
    lua_pushcclosure(L, (l_space_set_window_size), 0);
    lua_setfield(L, -2, "set_window_size");
    lua_pushcclosure(L, (l_space_get_window_size), 0);
    lua_setfield(L, -2, "get_window_size");
 // 整帧像素尺寸（C1，2026-11-16）：只给整屏效果用，别拿来做布局。
    lua_pushcclosure(L, (l_space_get_frame_size), 0);
    lua_setfield(L, -2, "get_frame_size");
    lua_setfield(L, -2, "space");

 // ---- wag.argv（控制通道：启动参数原样交给脚本）----
    lua_createtable(L, 0, 1);
    lua_pushcclosure(L, (l_argv_start_node), 0);
    lua_setfield(L, -2, "start_node");
    lua_setfield(L, -2, "argv");

 // ---- wag.fs（只读目录枚举；WagLib 的层扫描用）----
    lua_createtable(L, 0, 1);
    lua_pushcclosure(L, (l_fs_list_files), 0);
    lua_setfield(L, -2, "list_files");
    lua_setfield(L, -2, "fs");

 // ---- wag.control（控制通道：取命令 / 发布键值）----
    lua_createtable(L, 0, 2);
    lua_pushcclosure(L, (l_control_take), 0);
    lua_setfield(L, -2, "take");
    lua_pushcclosure(L, (l_control_publish), 0);
    lua_setfield(L, -2, "publish");
    lua_setfield(L, -2, "control");

 // ---- wag.audio（音频：资源句柄 + voice；2026-11-17）----
 // 【形状】load/unload/lookup 管资源；play/stop/set_volume/is_active 管 voice。
 // 资源与 voice 分离 ⇒ 同一个音效可以同时响多次。
    lua_createtable(L, 0, 17);
    lua_pushcclosure(L, (l_audio_load), 0);            lua_setfield(L, -2, "load");
    lua_pushcclosure(L, (l_audio_unload), 0);          lua_setfield(L, -2, "unload");
    lua_pushcclosure(L, (l_audio_lookup), 0);          lua_setfield(L, -2, "lookup");
    lua_pushcclosure(L, (l_audio_duration), 0);        lua_setfield(L, -2, "duration");
    lua_pushcclosure(L, (l_audio_play), 0);            lua_setfield(L, -2, "play");
    lua_pushcclosure(L, (l_audio_stop), 0);            lua_setfield(L, -2, "stop");
    lua_pushcclosure(L, (l_audio_pause), 0);          lua_setfield(L, -2, "pause");
    lua_pushcclosure(L, (l_audio_resume), 0);         lua_setfield(L, -2, "resume");
    lua_pushcclosure(L, (l_audio_pause_all), 0);      lua_setfield(L, -2, "pause_all");
    lua_pushcclosure(L, (l_audio_resume_all), 0);     lua_setfield(L, -2, "resume_all");
    lua_pushcclosure(L, (l_audio_is_paused), 0);      lua_setfield(L, -2, "is_paused");
    lua_pushcclosure(L, (l_audio_fft_bins), 0);       lua_setfield(L, -2, "fft_bins");
    lua_pushcclosure(L, (l_audio_waveform), 0);       lua_setfield(L, -2, "waveform");
    lua_pushcclosure(L, (l_audio_set_volume), 0);      lua_setfield(L, -2, "set_volume");
    lua_pushcclosure(L, (l_audio_set_pan), 0);         lua_setfield(L, -2, "set_pan");
    lua_pushcclosure(L, (l_audio_is_active), 0);       lua_setfield(L, -2, "is_active");
    lua_pushcclosure(L, (l_audio_stop_all), 0);        lua_setfield(L, -2, "stop_all");
    lua_pushcclosure(L, (l_audio_current_music), 0);  lua_setfield(L, -2, "current_music");
    lua_pushcclosure(L, (l_audio_set_master_volume), 0); lua_setfield(L, -2, "set_master_volume");
    lua_pushcclosure(L, (l_audio_get_master_volume), 0); lua_setfield(L, -2, "get_master_volume");
    lua_pushcclosure(L, (l_audio_set_music_volume), 0);  lua_setfield(L, -2, "set_music_volume");
    lua_pushcclosure(L, (l_audio_get_music_volume), 0);  lua_setfield(L, -2, "get_music_volume");
    lua_pushcclosure(L, (l_audio_set_se_volume), 0);     lua_setfield(L, -2, "set_se_volume");
    lua_pushcclosure(L, (l_audio_get_se_volume), 0);     lua_setfield(L, -2, "get_se_volume");
    lua_setfield(L, -2, "audio");


    lua_createtable(L, 0, 0);
    lua_pushcclosure(L, (l_log_emit), 0); lua_setfield(L, -2, "emit");

    lua_pushcclosure(L, (l_log_info), 0); lua_setfield(L, -2, "info");
    lua_pushcclosure(L, (l_log_warn), 0); lua_setfield(L, -2, "warn");





    lua_createtable(L, 0, 0);
    set_level(L, "record", log::severity::record);
    set_level(L, "warn", log::severity::warn);
    set_level(L, "raise", log::severity::raise);
    set_level(L, "terminate", log::severity::terminate);
    lua_setfield(L, -2, "level");

    lua_setfield(L, -2, "log");


    lua_pushcclosure(L, (l_fail), 0); lua_setfield(L, -2, "fail");



    lua_createtable(L, 0, 0);
    lua_pushcclosure(L, (l_boot_run), 0); lua_setfield(L, -2, "run");
    lua_setfield(L, -2, "boot");


    lua_createtable(L, 0, 0);
    lua_pushcclosure(L, (l_time_set_target_fps), 0); lua_setfield(L, -2, "set_target_fps");

    lua_pushcclosure(L, (l_time_set), 0); lua_setfield(L, -2, "set");
    lua_pushcclosure(L, (l_time_get), 0); lua_setfield(L, -2, "get");
    lua_setfield(L, -2, "time");
    lua_createtable(L, 0, 0);
    lua_pushcclosure(L, (l_rng_master_seed), 0); lua_setfield(L, -2, "master_seed");
    lua_pushcclosure(L, (l_rng_new_stream), 0); lua_setfield(L, -2, "new_stream");
    lua_pushcclosure(L, (l_rng_uint32), 0); lua_setfield(L, -2, "uint32");
    lua_pushcclosure(L, (l_rng_int), 0); lua_setfield(L, -2, "int");
    lua_pushcclosure(L, (l_rng_float), 0); lua_setfield(L, -2, "float");
    lua_pushcclosure(L, (l_rng_state), 0); lua_setfield(L, -2, "state");
    lua_pushcclosure(L, (l_rng_set_state), 0); lua_setfield(L, -2, "set_state");
    lua_pushcclosure(L, (l_rng_reset), 0); lua_setfield(L, -2, "reset");
    lua_setfield(L, -2, "rng");






    lua_createtable(L, 0, 0);
    (lua_pushcclosure(L, (&l_draw_image), 0), lua_setfield(L, -2, "draw_image"));
 // 四任意点（纯位置版）：draw_vertices 的扁平糖，与 draw_image 共用 collect_quad
    (lua_pushcclosure(L, (&l_draw_quad), 0), lua_setfield(L, -2, "draw_quad"));
 // 文字提交原语（2026-11-16，）：扁平平元数组，每图元 9 个数。
    (lua_pushcclosure(L, (&l_draw_glyphs), 0), lua_setfield(L, -2, "draw_glyphs"));
    (lua_pushcclosure(L, (&l_draw_vertices), 0), lua_setfield(L, -2, "draw_vertices"));

    (lua_pushcclosure(L, (&l_draw_set_state), 0), lua_setfield(L, -2, "set_state"));
    (lua_pushcclosure(L, (&l_draw_get_state), 0), lua_setfield(L, -2, "get_state"));
    (lua_pushcclosure(L, (&l_draw_reset_state), 0), lua_setfield(L, -2, "reset_state"));

    (lua_pushcclosure(L, (&l_draw_set_probe_shaders), 0), lua_setfield(L, -2, "set_probe_shaders"));

    lua_pushcclosure(L, (l_draw_load_shader), 0);
    lua_setfield(L, -2, "load_shader");
    lua_pushcclosure(L, (l_draw_unload_shader), 0);
    lua_setfield(L, -2, "unload_shader");
    (lua_pushcclosure(L, (&l_draw_mesh), 0), lua_setfield(L, -2, "draw_mesh"));
 // 容器默认发射：从 64 B 结构体直接画（b 方案的默认渲染路径）
    (lua_pushcclosure(L, (&l_draw_emit_containers), 0),
     lua_setfield(L, -2, "emit_containers"));
 // B 方案：快照改为显式请求（本帧请求，阶段 10 拍）
    (lua_pushcclosure(L, (&l_draw_capture_snapshot), 0),
     lua_setfield(L, -2, "capture_snapshot"));

    lua_setfield(L, -2, "draw");

 // wag.light —— 光照原语（2026-11-18）
    lua_newtable(L);
    (lua_pushcclosure(L, (&l_light_add), 0), lua_setfield(L, -2, "add"));
    (lua_pushcclosure(L, (&l_light_exposure), 0), lua_setfield(L, -2, "exposure"));
    (lua_pushcclosure(L, (&l_light_load_shader), 0), lua_setfield(L, -2, "load_shader"));
    (lua_pushcclosure(L, (&l_light_band), 0), lua_setfield(L, -2, "band"));
    (lua_pushcclosure(L, (&l_light_commit), 0), lua_setfield(L, -2, "commit"));
    (lua_pushcclosure(L, (&l_light_stats), 0), lua_setfield(L, -2, "stats"));
    lua_newtable(L);
    (lua_pushcclosure(L, (&l_light_occluders_new), 0), lua_setfield(L, -2, "new"));
    (lua_pushcclosure(L, (&l_light_occluders_spawn), 0), lua_setfield(L, -2, "spawn"));
    (lua_pushcclosure(L, (&l_light_occluders_write), 0), lua_setfield(L, -2, "write"));
    (lua_pushcclosure(L, (&l_light_occluders_kill), 0), lua_setfield(L, -2, "kill"));
    (lua_pushcclosure(L, (&l_light_occluders_alive), 0), lua_setfield(L, -2, "alive"));
    lua_setfield(L, -2, "occluders");
    lua_setfield(L, -2, "light");

 // wag.container —— 容器原语（2026-10-20）
 // 【本组的边界】只暴露句柄与字段；类/回调由库层负责。
    lua_createtable(L, 0, 0);
    (lua_pushcclosure(L, (&l_container_new_pool), 0),
     lua_setfield(L, -2, "new_pool"));
 // /：先 set_dispatcher，再 register_class
    (lua_pushcclosure(L, (&l_container_set_dispatcher), 0),
     lua_setfield(L, -2, "set_dispatcher"));
    (lua_pushcclosure(L, (&l_container_register_class), 0),
     lua_setfield(L, -2, "register_class"));
    (lua_pushcclosure(L, (&l_container_spawn), 0),
     lua_setfield(L, -2, "spawn"));
    (lua_pushcclosure(L, (&l_container_write), 0),
     lua_setfield(L, -2, "write"));
    (lua_pushcclosure(L, (&l_container_kill), 0),
     lua_setfield(L, -2, "kill"));
    (lua_pushcclosure(L, (&l_container_alive), 0),
     lua_setfield(L, -2, "alive"));
    (lua_pushcclosure(L, (&l_container_count_live), 0),
     lua_setfield(L, -2, "count_live"));
    (lua_pushcclosure(L, (&l_container_count_live_all), 0),
     lua_setfield(L, -2, "count_live_all"));
    (lua_pushcclosure(L, (&l_container_count_occupied), 0),
     lua_setfield(L, -2, "count_occupied"));
    (lua_pushcclosure(L, (&l_container_count_occupied_all), 0),
     lua_setfield(L, -2, "count_occupied_all"));
    (lua_pushcclosure(L, (&l_container_count_renderable), 0),
     lua_setfield(L, -2, "count_renderable"));
    (lua_pushcclosure(L, (&l_container_count_renderable_all), 0),
     lua_setfield(L, -2, "count_renderable_all"));
    (lua_pushcclosure(L, (&l_container_set_hit), 0),
     lua_setfield(L, -2, "set_hit"));
    (lua_pushcclosure(L, (&l_container_replace), 0),
     lua_setfield(L, -2, "replace"));
    (lua_pushcclosure(L, (&l_container_write_batch), 0),
     lua_setfield(L, -2, "write_batch"));
    lua_setfield(L, -2, "container");

 // wag.collide —— 碰撞原语（//）
 // 【边界】只做"声明本帧检哪两组"；规则不持久化，回调只拿批量数组。
    lua_createtable(L, 0, 0);
    (lua_pushcclosure(L, (&l_collide_check), 0),
     lua_setfield(L, -2, "check"));
    (lua_pushcclosure(L, (&l_collide_check_one), 0),
     lua_setfield(L, -2, "check_one"));
    (lua_pushcclosure(L, (&l_collide_stats), 0),
     lua_setfield(L, -2, "stats"));
    lua_setfield(L, -2, "collide");




    lua_createtable(L, 0, 0);
    (lua_pushcclosure(L, (&l_image_load_from_file), 0), lua_setfield(L, -2, "load_from_file"));
    (lua_pushcclosure(L, (&l_image_load_from_snapshot), 0), lua_setfield(L, -2, "load_from_snapshot"));
    (lua_pushcclosure(L, (&l_image_unload), 0), lua_setfield(L, -2, "unload"));
    (lua_pushcclosure(L, (&l_image_lookup), 0), lua_setfield(L, -2, "lookup"));
    (lua_pushcclosure(L, (&l_image_size), 0), lua_setfield(L, -2, "size"));
 // /：帧表（图像组 = 带帧表的图片）
    (lua_pushcclosure(L, (&l_image_define_frames), 0),
     lua_setfield(L, -2, "define_frames"));
    (lua_pushcclosure(L, (&l_image_frame_count), 0),
     lua_setfield(L, -2, "frame_count"));
 //：锚点（设置 / 查询同一函数，按参数个数分形态）
    (lua_pushcclosure(L, (&l_image_anchor), 0), lua_setfield(L, -2, "anchor"));
    lua_setfield(L, -2, "image");

 // wag.font —— TTF 字体与字形（2026-11-18）：只给"字形供给"，不给排版
    lua_createtable(L, 0, 5);
    (lua_pushcclosure(L, (&l_font_load_ttf), 0), lua_setfield(L, -2, "load_ttf"));
    (lua_pushcclosure(L, (&l_font_glyph), 0), lua_setfield(L, -2, "glyph"));
    (lua_pushcclosure(L, (&l_font_has_glyph), 0), lua_setfield(L, -2, "has_glyph"));
    (lua_pushcclosure(L, (&l_font_metrics), 0), lua_setfield(L, -2, "metrics"));
    (lua_pushcclosure(L, (&l_font_unload), 0), lua_setfield(L, -2, "unload"));
    lua_setfield(L, -2, "font");

    lua_createtable(L, 0, 14);
    (lua_pushcclosure(L, (&l_input_down), 0), lua_setfield(L, -2, "down"));
    (lua_pushcclosure(L, (&l_input_press), 0), lua_setfield(L, -2, "press"));
    (lua_pushcclosure(L, (&l_input_release), 0), lua_setfield(L, -2, "release"));
    (lua_pushcclosure(L, (&l_input_mouse_pos), 0), lua_setfield(L, -2, "mouse_pos"));
    (lua_pushcclosure(L, (&l_input_mouse_down), 0), lua_setfield(L, -2, "mouse_down"));
    (lua_pushcclosure(L, (&l_input_mouse_press), 0), lua_setfield(L, -2, "mouse_press"));
    (lua_pushcclosure(L, (&l_input_mouse_release), 0),
     lua_setfield(L, -2, "mouse_release"));
    (lua_pushcclosure(L, (&l_input_wheel), 0), lua_setfield(L, -2, "wheel"));
    (lua_pushcclosure(L, (&l_input_gamepad_devices), 0),
     lua_setfield(L, -2, "gamepad_devices"));
    (lua_pushcclosure(L, (&l_input_gamepad_connected), 0),
     lua_setfield(L, -2, "gamepad_connected"));
    (lua_pushcclosure(L, (&l_input_gamepad_down), 0),
     lua_setfield(L, -2, "gamepad_down"));
    (lua_pushcclosure(L, (&l_input_gamepad_press), 0),
     lua_setfield(L, -2, "gamepad_press"));
    (lua_pushcclosure(L, (&l_input_gamepad_release), 0),
     lua_setfield(L, -2, "gamepad_release"));
    (lua_pushcclosure(L, (&l_input_gamepad_axis), 0),
     lua_setfield(L, -2, "gamepad_axis"));
    lua_setfield(L, -2, "input");

    lua_settop(L, -(1)-1);

    log::record("script", "引擎原语已注册"
                "（engine / space / time / log / fail / boot / draw / image / input）");

#else
    (void)eng;
#endif
}

} // namespace wag::script
