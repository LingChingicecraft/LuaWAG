// =============================================================================
// wag/script/state.cpp
//
// 脚本状态的实现。
//
// 【本文件包含 LuaJIT 的头】
// 这是内核里唯一允许（也必然）包含 Lua 头的 .cpp。别的模块通过
// state.hpp 的不透明句柄与它交互。
//
// 【实现完整程度 —— 这份列表就是本文件的验收清单】
//
// 已实现：
// · `create`：逐个装 8 个库（**不用 `luaL_openlibs`** —— 它会带进完整的
// debug）；刻意**不装** `jit` / `ffi`
// · `destroy`：`lua_close`（会触发 `__gc` ⇒ 必须在引擎资源仍在时调用）
// · `do_file`：**读入与执行分开报错**，并取出 Lua 错误原文
// · `seal_environment`：`ffi` / `jit` 经 `package.preload` 封成**报错函数**
// （用报错函数而不是 nil —— nil 会让 `require` 继续走搜索路径），
// 并把 `debug` 重建为"只含 traceback"
// · `install_hook_noops`（步骤 0）与 `verify_hooks_are_functions`（自检）
// · `prepend_package_path` / `run_script_file`
//
// 未实现 / 已知待收：
// · `call_global()` 与 `stack_depth()` **目前没有调用者**：
// 前者已被 `call_hook` 取代，后者是排查栈漂移时的临时探针。
// ⇒ 是否删除尚未决策（先如实记下它们没有消费者）。
// · ⚠️ **封堵在 C++ 与 Lua 两侧各有一份**：本文件的 `seal_environment`
// 在 `boot_sequence` 步骤 4 先跑，而 `WagLib/harden.lua` 里"清注册表"
// 那一步依赖 `debug.getregistry()` —— 它此刻已被折中掉 ⇒
// **那一步永不执行**（静默跳过）。归属需要收口：
// 封堵的唯一权威应当是 **C++ 侧**（脚本文件是可被作者改写的）。
//
// =============================================================================

#include "script/state.hpp"

#include "error/error.hpp"
#include "log/log.hpp"
#include "script/hook.hpp"
#include "platform/paths.hpp"
#include "platform/utf8_path.hpp"

// LuaJIT 的公开头。
//
// 【为什么写 luajit-2.1/ 这个版本目录】
// MSYS2 里同时存在两份 lua.h：
// include/lua.h —— Lua 5.5（lua 包）
// include/luajit-2.1/lua.h —— LuaJIT
// 写 <lua.h> 会先命中 Lua 5.5 的那一份，而链接的是 LuaJIT 的库。
// 加上版本目录是**钉死身份**，不依赖 -I 顺序。
//
// 【为什么加 extern "C"】Lua 的头不自动带它。
#if defined(WAG_HAVE_LUAJIT)
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#endif

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace wag::script {

// =============================================================================
// 内部工具
// =============================================================================

namespace {

/// 错误对象的字符串表示的最大长度。与 state::error_buf_ 一致。
constexpr std::size_t kErrorBufSize = 256;

/// 最近一次错误文本。单线程（Lua 只在主线程），所以一个文件级缓冲就够。
char g_last_error[kErrorBufSize] = {};

#if defined(WAG_HAVE_LUAJIT)
/// 把栈顶的错误对象取成文本，存进 g_last_error 并弹掉它。
void capture_error(lua_State* L) {
    const char* text = nullptr;

    if (lua_type(L, -1) == LUA_TSTRING) {
        text = lua_tostring(L, -1);
    } else {
 // 错误对象不保证是字符串（error({code=1}) 抛的是表）。
 // 不在这里调 tostring —— 那可能再次触发 __tostring 并报错。
        text = "(非字符串的错误对象)";
    }

    if (text == nullptr) {
        text = "(无法取出错误文本)";
    }
 // 【消毒】把文本里出现的"安装根前缀"删掉（规则 2）。
 // Lua 的错误消息/require 的"找不到模块"/traceback 都会把绝对路径写进字符串，
 // 而那些字符串不经过 errors.jsonl 的 `file` 字段 —— 只能就地洗。
    const std::string cleaned = wag::paths::scrub(text);
    std::snprintf(g_last_error, kErrorBufSize, "%s", cleaned.c_str());

 // 【E11 ③b：捕获点落盘】这就是"L3 未捕获才写"里的**捕获点**：
 // pcall 失败 ⇒ 这个 raise 一路上没被脚本自己的 pcall 接住。
 // 有 pending 才写；普通 Lua 报错（没有 pending）不在这里写 ——
 // 它会由随后的 `fail_script` 以 warn 级记下（乙 的规则）。
    wag::flush_pending_raise();
}

/// Lua `print` 的替代实现：L1 [lua]，进两份普通日志，不写 stdout。
///
/// 【为什么不保留 stdout】Release 是 GUI 子系统，stdout 根本看不见；
/// D12 已定“print 接管”。io.write 不接管，避免变成通用 stdout 重定向。
int lua_print_to_log(lua_State* L) {
    const int n = lua_gettop(L);
    std::string text;
    lua_getglobal(L, "tostring");
    for (int i = 1; i <= n; ++i) {
        lua_pushvalue(L, -1);     // tostring
        lua_pushvalue(L, i);      // 参数
        lua_call(L, 1, 1);
        const char* s = lua_tostring(L, -1);
        if (s == nullptr) {
            s = luaL_typename(L, i);
        }
        if (i > 1) {
            text += '\t';
        }
        text += wag::paths::scrub(s);
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // tostring
    log::record("lua", text);
    log::flush();
    return 0;
}

/// 自检用：遍历一格表，检查每个值是不是函数。
///
/// @param L Lua 状态
/// @param idx 表在栈上的索引（绝对索引，内部会复制一份来处理）
/// @return 表为空，或存在非函数的成员时为 false
bool table_values_are_all_functions(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) {
        return false;
    }

 // Lua 5.1 / LuaJIT **没有 lua_absindex**（它是 5.2 引入的），自己算：
 // idx > 0 或 idx <= LUA_REGISTRYINDEX 时，它已经是绝对索引。
    const int abs_idx = (idx > 0 || idx <= LUA_REGISTRYINDEX)
                            ? idx
                            : lua_gettop(L) + idx + 1;
    bool ok = true;
    int count = 0;

    lua_pushnil(L);
    while (lua_next(L, abs_idx) != 0) {
        ++count;
        if (!lua_isfunction(L, -1)) {
            ok = false;
 // 报出实际看到的成员名 —— 它来自表本身，不是一张抄来的清单。
            log::record("script", log::format(
                "wag.hook 上有成员不是函数",
                lua_tostring(L, -2) != nullptr ? lua_tostring(L, -2) : "(非字符串键)"));
        }
        lua_pop(L, 1); // 弹掉 value，留下 key 供 lua_next 继续
    }

 // 空表不算通过 —— 它意味着预置没发生。
    return ok && count > 0;
}

/// 被封死模块的统一报错函数。require("ffi") 会落到这里。
int lua_sealed_module_error(lua_State* L) {
    const char* name = lua_tostring(L, lua_upvalueindex(1));
    return luaL_error(L,
                      "模块 '%s' 已被 LuaWAG 禁用：它允许脚本绕过引擎边界。",
                      name != nullptr ? name : "?");
}

/// 13 个钩子槽的默认实现：接受任何实参，不做任何事，不返回。
int lua_hook_noop(lua_State*) {
    return 0;
}
#endif // WAG_HAVE_LUAJIT

} // namespace

// =============================================================================
// 生命周期
// =============================================================================

// 【临时探针 · 未来可能删掉】读 Lua 值栈深度。见 state.hpp 的声明。
int state::stack_depth() const noexcept {
#if defined(WAG_HAVE_LUAJIT)
    return (L_ != nullptr) ? lua_gettop(L_) : 0;
#else
    return 0;
#endif
}

state::~state() {
 // 【析构为什么调 destroy】
 // 因为"持有 lua_State 但不关它"是资源泄漏，而泄漏在这里没有正当理由
 // （不像窗口/渲染器那样需要有序的关闭序列）。
 // 显式调 destroy 也让"重复调 destroy"成为一个需要被容忍的情形。
    destroy();
}

namespace {

/// 交还通道的实现（E8）：把边界错误抛成一个 Lua 错误。
///
/// 【为什么不用 `luaL_error`】它内部会先构造 `std::string`；而 `lua_error`
/// 是 `longjmp` —— **不执行 C++ 析构** ⇒ 那个临时字符串的堆块会漏掉。
/// 这里改用 `luaL_where + lua_pushlstring + lua_concat + lua_error`，
/// 全程只碰 Lua 栈，**一个需要析构的 C++ 对象都不产生**，
/// 同时保住 `luaL_error` 白送的那段 `file:line:` 前缀。
///
/// 【返回值是 void，但实际有两条出路】
/// 有 `lua_State` ⇒ `lua_error` 长跳转，永不返回；
/// 没有 ⇒ 直接返回，让 `fail_boundary` 去终止（"没有交还者"那条路）。
lua_State* g_lua_for_raise = nullptr;

void raise_to_script(std::string_view message) {
    if (g_lua_for_raise == nullptr) {
        return; // 没装/已拆 ⇒ 交不出去
    }
    lua_State* const L = g_lua_for_raise;

 // 【为什么要逐层试】`luaL_where` 只认 **Lua 帧**，而错误可能从很深的
 // C++ 调用链里发出（原语 → helper → fail_boundary → 这里）。
 // level 1 未必就是脚本那一层 ⇒ 实测它返回空串，位置就丢了。
 // ⇒ 从 1 起往上找，取第一个**非空**的位置；找不到就退化成没有前缀。
    bool placed = false;
    for (int level = 1; level <= 3; ++level) {
        luaL_where(L, level);
        if (lua_objlen(L, -1) > 0) {
            placed = true;
            break;
        }
        lua_pop(L, 1); // 空的那一层丢掉，继续往上
    }
    if (!placed) {
        lua_pushliteral(L, ""); // 统一形态：栈顶永远是"前缀"
    }

 // 【E11 ③b：把脚本位置交给 pending 记录】
 // 上面的 `luaL_where` 已经把 "file:line: " 拼进了错误文本，
 // 但 errors.jsonl 要的是**分开的** file 与 line 两个字段。
 // ⇒ 再走一次 getinfo 取 source/currentline（与 luaL_where 同一种查找规则）。
 // 【为什么位置要在这里取】`fail_boundary` 抛错时还不知道脚本位置；
 // 只有真正"抛"的这一层能看见调用栈。
    for (int level = 1; level <= 3; ++level) {
        lua_Debug ar{};
        if (lua_getstack(L, level, &ar) == 0) {
            continue;
        }
        if (lua_getinfo(L, "Sl", &ar) == 0) {
            continue;
        }
        if (ar.currentline > 0) {
            wag::set_raise_position(ar.source, ar.currentline);
            break;
        }
    }

    lua_pushlstring(L, message.data(), message.size());
    lua_concat(L, 2);
    lua_error(L); // longjmp：回到最近的那个 lua_pcall
}

} // namespace

bool state::create() {    if (L_ != nullptr) {
        log::warn("script", "脚本状态已经建立，重复的 create() 被忽略");
        return false;
    }

#if defined(WAG_HAVE_LUAJIT)
    L_ = luaL_newstate();
    if (L_ == nullptr) {
        log::warn("script", "lua_State 建立失败（内存不足）");
        return false;
    }

 // ---- E8：把"交还通道"接上 ----
 // 【为什么在这里】此刻 lua_State 刚建立、还没有任何脚本跑过；
 // 从这里到 destroy 之间，边界错误都能抛回脚本。
 // 【为什么不常驻】它只是一个函数指针；"有没有落点"由 error 的深度计数判断。
    g_lua_for_raise = L_;
    wag::set_boundary_raiser(&raise_to_script);

 // 逐个装入已定的库。不用 luaL_openlibs —— 它会把**完整的 debug** 也装进来，
 // 而 debug 的折中版由 seal_environment 建立。
 //
 // 【注意 coroutine 没有对应的 luaopen_】Lua 5.1 / LuaJIT 里它不是独立库，
 // 而是由 luaopen_base 一并装入。所以"能用的库有九个"与"要调用的 luaopen
 // 有八个"说的是不同的事 —— 前者是清单，后者是调用次数。
    lua_pushcfunction(L_, luaopen_base);      lua_call(L_, 0, 1); lua_pop(L_, 1);
 // D12：print 接管为 L1 [lua]；不写 stdout。
    lua_pushcfunction(L_, lua_print_to_log);
    lua_setglobal(L_, "print");
    lua_pushcfunction(L_, luaopen_string);    lua_call(L_, 0, 1); lua_pop(L_, 1);
    lua_pushcfunction(L_, luaopen_table);     lua_call(L_, 0, 1); lua_pop(L_, 1);
    lua_pushcfunction(L_, luaopen_math);      lua_call(L_, 0, 1); lua_pop(L_, 1);
    lua_pushcfunction(L_, luaopen_io);        lua_call(L_, 0, 1); lua_pop(L_, 1);
    lua_pushcfunction(L_, luaopen_os);        lua_call(L_, 0, 1); lua_pop(L_, 1);
    lua_pushcfunction(L_, luaopen_package);   lua_call(L_, 0, 1); lua_pop(L_, 1);
    lua_pushcfunction(L_, luaopen_bit);       lua_call(L_, 0, 1); lua_pop(L_, 1);

 // 刻意不调用 luaopen_jit 与 luaopen_ffi（LuaJIT 的扩展库，lualib.h 里有）。
 // 不装它们，ffi 就只能在 require("ffi") 时尝试注册 —— 而那正是运行期
 // 五路封堵要挡的地方。

    log::record("script", "LuaJIT 状态已建立，标准库装配完成");
    return true;
#else
    log::record("script", "未接 LuaJIT，脚本状态未建立");
    return false;
#endif
}

void state::destroy() {
    if (L_ == nullptr) {
        return;
    }

#if defined(WAG_HAVE_LUAJIT)
 // ---- E8：先拆交还通道，再关状态 ----
 // 【顺序不可反】`g_lua_for_raise` 指向 L_；若在 lua_close 之后再拆，
 // 中间那段窗口里的边界错误会往一个已释放的 state 上抛。
    wag::set_boundary_raiser(nullptr);
    g_lua_for_raise = nullptr;

 //：分派器是唯一允许持有的 Lua 引用；关闭前必须 unref。
    if (dispatcher_ref_ >= 0) {
        luaL_unref(L_, LUA_REGISTRYINDEX, dispatcher_ref_);
        dispatcher_ref_ = -1;
    }
 // lua_close 会触发所有未回收 userdata 的 __gc，所以它必须在引擎资源仍然
 // 存在的时候调用（或在此之前先跑一次显式 GC，见关闭序列第 3 步）。
    lua_close(L_);
#endif

    L_ = nullptr;
    error_buf_[0] = '\0';
}

bool state::call_dispatcher(const char* kind, std::uint64_t handle,
                            std::uint32_t a, std::uint32_t b, std::string* error) {
#if defined(WAG_HAVE_LUAJIT)
    if (L_ == nullptr || dispatcher_ref_ < 0) {
        if (error != nullptr) {
            *error = "分派器未安装";
        }
        return false;
    }
    const int base = lua_gettop(L_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, dispatcher_ref_);
    lua_pushstring(L_, kind);
    lua_pushinteger(L_, static_cast<lua_Integer>(handle));
    lua_pushinteger(L_, static_cast<lua_Integer>(a));
    lua_pushinteger(L_, static_cast<lua_Integer>(b));
 // 【E8 记账】这一行到 pcall 返回之间，边界错误能抛回脚本（有落点）
    wag::hand_back_scope hand_back;
    const int rc = lua_pcall(L_, 4, 0, 0);
    if (rc != 0) {
        const char* msg = lua_tostring(L_, -1);
        if (error != nullptr) {
            *error = (msg != nullptr) ? msg : "分派器调用失败";
        }
        lua_settop(L_, base);
        return false;
    }
    lua_settop(L_, base);
    return true;
#else
    (void)kind;
    (void)handle;
    (void)a;
    (void)b;
    if (error != nullptr) {
        *error = "未启用 LuaJIT";
    }
    return false;
#endif
}

bool state::call_collision(int ref, const std::uint64_t* flat, std::size_t n,
                           std::string* error) {
#if defined(WAG_HAVE_LUAJIT)
    if (L_ == nullptr || ref < 0) {
        if (error != nullptr) {
            *error = "碰撞回调引用无效";
        }
        return false;
    }
    const int base = lua_gettop(L_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    lua_createtable(L_, static_cast<int>(n), 0);
    for (std::size_t i = 0; i < n; ++i) {
        lua_pushinteger(L_, static_cast<lua_Integer>(flat[i]));
        lua_rawseti(L_, -2, static_cast<int>(i + 1));
    }
 // 【E8 记账】碰撞回调里也可能触到边界错误
    wag::hand_back_scope hand_back;
    const int rc = lua_pcall(L_, 1, 0, 0);
    if (rc != 0) {
        const char* msg = lua_tostring(L_, -1);
        if (error != nullptr) {
            *error = (msg != nullptr) ? msg : "碰撞回调失败";
        }
        lua_settop(L_, base);
        return false;
    }
    lua_settop(L_, base);
    return true;
#else
    (void)ref;
    (void)flat;
    (void)n;
    if (error != nullptr) {
        *error = "未启用 LuaJIT";
    }
    return false;
#endif
}

void state::unref(int ref) noexcept {
#if defined(WAG_HAVE_LUAJIT)
    if (L_ != nullptr && ref >= 0) {
        luaL_unref(L_, LUA_REGISTRYINDEX, ref);
    }
#else
    (void)ref;
#endif
}

// =============================================================================
// 执行脚本
// =============================================================================

// -----------------------------------------------------------------------------
// 读整个文件（二进制，原样字节）
//
// 【为什么不用 luaL_loadfile】见 do_file 里的说明：它会拿路径当 chunk 名。
// -----------------------------------------------------------------------------
namespace {

bool read_whole_file(const char* path, std::string& out) {
    std::FILE* const file = wag::paths::fopen_utf8(path, "rb");
    if (file == nullptr) {
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    const long size = std::ftell(file);
    if (size < 0) {
        std::fclose(file);
        return false;
    }
    if (std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    const std::size_t got = size > 0
        ? std::fread(&out[0], 1, out.size(), file)
        : 0;
    std::fclose(file);
    return got == out.size();
}

} // namespace


bool state::do_file(const char* path) {
    if (L_ == nullptr) {
        log::warn("script", "脚本站未建立，无法执行文件");
        return false;
    }
    if (path == nullptr || path[0] == '\0') {
        log::warn("script", "脚本路径为空");
        return false;
    }

#if defined(WAG_HAVE_LUAJIT)
 // -------------------------------------------------------------------------
 // 自己读文件 + 给相对 chunk 名
 //
 // 【为什么不再用 luaL_loadfile】它会把**传进去的路径**当 chunk 名，
 // 于是 Lua 的错误消息、debug.traceback、errors.jsonl 的 file
 // 全是绝对路径（会泄漏开发机的用户名与目录结构）。
 // ⇒ 改成 loadbuffer + 自定义名：`@Luminosa/Project/main.lua`
 // · 相对名短（24 字符）⇒ 不会被 LUA_IDSIZE=60 截断成 `...`
 // · 根外的文件 ⇒ 只留 basename（error.cpp 会补 external 标记）
 // -------------------------------------------------------------------------
    std::string source;
    if (!read_whole_file(path, source)) {
 // 【相对化】失败路径同样不得打印绝对路径（规则 2）。
        log::warn("script", log::format("脚本读入失败", wag::paths::relativize(path)));
        return false;
    }
 // 【BOM】luaL_loadfile 会跳过 UTF-8 BOM，loadbuffer 不会 ⇒ 这里手动剥掉。
    if (source.size() >= 3 &&
        static_cast<unsigned char>(source[0]) == 0xEF &&
        static_cast<unsigned char>(source[1]) == 0xBB &&
        static_cast<unsigned char>(source[2]) == 0xBF) {
        source.erase(0, 3);
    }

    const std::string chunk_name =
        "@" + wag::paths::relativize(path);
    if (luaL_loadbuffer(L_, source.data(), source.size(), chunk_name.c_str()) != 0) {
        capture_error(L_);
        log::warn("script", log::format("脚本编译失败", chunk_name));
        lua_pop(L_, 1);
        return false;
    }
 // 【E8 记账】执行阶段也可能触发边界错误（脚本调原语）
    wag::hand_back_scope hand_back;
    if (lua_pcall(L_, 0, 0, 0) != 0) {
        capture_error(L_);
        log::warn("script", log::format("脚本执行失败", wag::paths::relativize(path)));
        lua_pop(L_, 1);
        return false;
    }
    return true;
#else
    log::warn("script", log::format("未接 LuaJIT，无法执行脚本", wag::paths::relativize(path)));
    return false;
#endif
}

bool state::call_global(const char* global_name) {
    if (L_ == nullptr || global_name == nullptr) {
        return false;
    }

#if defined(WAG_HAVE_LUAJIT)
    lua_getglobal(L_, global_name);
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 1);
        log::warn("script", log::format("全局函数不存在", global_name));
        return false;
    }
 // 【E8 记账】全局函数（on_game_init 等）内部同样可能触发边界错误
    wag::hand_back_scope hand_back;
    if (lua_pcall(L_, 0, 0, 0) != 0) {
        capture_error(L_);
        log::warn("script", log::format("全局函数执行失败", global_name));
        lua_pop(L_, 1);
        return false;
    }
    return true;
#else
    return false;
#endif
}

const char* state::last_error_string() const {
 // 错误对象在 pcall 返回之后可能被回收，因此 capture_error 已经把它复制
 // 进文件级缓冲；这里再复制进本对象的缓冲，让"取出文本"与"处理错误"解耦。
    std::snprintf(error_buf_, sizeof(error_buf_), "%s", g_last_error);
    return error_buf_;
}

// =============================================================================
// 环境封堵
// =============================================================================

#if defined(WAG_HAVE_LUAJIT)
namespace {

/// 把某个模块名设成"被封死"：require 它必然报错。
///
/// 用报错函数而不是 nil —— nil 会让 require 继续走搜索路径，从而可能在文件
/// 系统上找到一个同名文件。
void seal_module(lua_State* L, const char* name) {
 // 报错函数（带名字一个 upvalue）。
    lua_pushstring(L, name);
    lua_pushcclosure(L, lua_sealed_module_error, 1);

 // package.preload[name] = 报错函数
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushvalue(L, -3); // 报错函数
    lua_setfield(L, -2, name);
    lua_pop(L, 2);        // 弹掉 preload 与 package

 // package.loaded[name] = nil。require 先查这张表，非 nil 就直接返回它。
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "loaded");
    lua_pushnil(L);
    lua_setfield(L, -2, name);
    lua_pop(L, 2);

 // 注册表的 _LOADED[name] = nil。Lua 5.1 里它就是 package.loaded，
 // 但显式写一遍，以免对实现的假设出错。
    lua_pushvalue(L, LUA_REGISTRYINDEX);
    lua_getfield(L, -1, "_LOADED");
    lua_pushnil(L);
    lua_setfield(L, -2, name);
    lua_pop(L, 2);

    lua_pop(L, 1); // 弹掉报错函数
}

} // namespace
#endif

void seal_environment(state& st) {
    if (!st.is_created()) {
        wag::fail_internal("环境封堵被调用时脚本状态尚未建立");
    }

#if defined(WAG_HAVE_LUAJIT)
    lua_State* L = st.raw();

    seal_module(L, "ffi");
    seal_module(L, "jit");

 // debug 折中成只含 traceback。
 // 注意：没有"先装完整 debug 再删"—— create() 里从来没装过它。
    lua_newtable(L);
    lua_getglobal(L, "debug");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "traceback");
        lua_setfield(L, -3, "traceback");
    }
    lua_pop(L, 1);
    lua_setglobal(L, "debug");

    log::record("script", "环境已封堵（ffi / jit 封死，debug 只留 traceback）");
#endif
}

// =============================================================================
// 步骤 0 与自检
// =============================================================================

void install_hook_noops(state& st) {
    if (!st.is_created()) {
        wag::fail_internal("预置钩子槽时脚本状态尚未建立");
    }

#if defined(WAG_HAVE_LUAJIT)
    lua_State* L = st.raw();

 // wag 表：不存在就建，存在就复用（register_primitives 还会往里加东西）。
    lua_getglobal(L, "wag");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "wag");
    }

 // wag.hook 子表。
    lua_newtable(L);
    for (std::size_t i = 0; i < hook_count; ++i) {
        lua_pushcfunction(L, lua_hook_noop);
        lua_setfield(L, -2, all_hook_names()[i]);
    }
    lua_setfield(L, -2, "hook");

    lua_pop(L, 1); // 弹掉 wag

    log::record("script", "已预置 13 个钩子槽为空函数（步骤 0）");
#endif
}

bool verify_hooks_are_functions(state& st) {
    if (!st.is_created()) {
        return false;
    }

#if defined(WAG_HAVE_LUAJIT)
    lua_State* L = st.raw();

    lua_getglobal(L, "wag");
    lua_getfield(L, -1, "hook");
    const bool ok = table_values_are_all_functions(L, -1);
    lua_pop(L, 2);
    return ok;
#else
 // 未接 LuaJIT 时没有表可查 —— 报失败，而不是报"通过"。
    return false;
#endif
}

// =============================================================================
// 引导脚本的定位与读入
// =============================================================================

void publish_install_root(state& st, std::string_view root) {
 // 【顺手记下安装根】do_file 要拿它把脚本路径转成相对 chunk 名。
 // 安装根由 wag::paths 全局持有（main.cpp 已设），这里不再存一份。

#if defined(WAG_HAVE_LUAJIT)
    if (!st.is_created()) {
        wag::fail_internal("发布安装根时脚本状态尚未建立");
    }
    lua_State* L = st.raw();

 // wag 表必须已经存在（步骤 0 建它）。不存在 ⇒ 启动次序被改了。
    lua_getglobal(L, "wag");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        wag::fail_internal("发布安装根时 wag 表不存在（启动次序被改过？）");
    }
 // 【用 pushlstring 而不是 pushstring】安装根是**路径**，可能是 UTF-8；
 // pushstring 以 '\0' 结尾，而 string_view 不保证 —— 显式给长度最稳。
    lua_pushlstring(L, root.data(), root.size());
    lua_setfield(L, -2, "install_root");
    lua_pop(L, 1);

    log::record("script", log::format("安装根已发布到 wag.install_root",
                            std::string(".") + "（相对 exe 所在目录）"));
#else
    (void)st;
    (void)root;
#endif
}

void prepend_package_path(state& st, std::string_view dir) {
#if defined(WAG_HAVE_LUAJIT)
    if (!st.is_created()) {
        wag::fail_internal("设置 package.path 时脚本状态尚未建立");
    }
    lua_State* L = st.raw();

 // 【为什么用 luaL_dostring 而不是在 C 里拼字符串再 setglobal】
 // 因为拼接要用到 Lua 里的当前值（原 package.path），而"读-拼-写"写成
 // 一行 Lua 最不容易出错，也顺带免掉一次手工的栈管理。
 //
 // 【为什么用 [[...]] 长括号而不是引号】
 // 路径里可能有反斜杠或引号（Windows 上很常见），长括号不解释转义。
 // 代价是"路径里若出现 ]]" 就会破 —— 那种路径在实践里不会出现，
 // 而这一点值得写下来，免得将来有人以为它是通用做法。
 //
 // 【package.path 在封堵之后仍可写】harden.lua 只封 ffi/jit 与折中 debug，
 // 不碰 package.path。
    std::string code = "package.path = [[";
    code.append(dir);
    code += "]] .. \"/?.lua;\" .. [[";
    code.append(dir);
    code += "]] .. \"/?/init.lua;\" .. package.path";

    if (luaL_dostring(L, code.c_str()) != 0) {
        const char* err = lua_tostring(L, -1);
 // 【消毒】Lua 的语法/运行错误会把**整段代码**回显到消息里，
 // 而那段代码里拼着绝对路径 ⇒ 必须洗掉（规则 2）。
        const std::string cleaned =
            wag::paths::scrub(err != nullptr ? err : "(无错误文本)");
        log::record("script", log::format("设置 package.path 失败", cleaned));
        lua_pop(L, 1);
        return;
    }
    log::record("script", log::format("package.path 已前置", std::string(".") + "（相对 exe 所在目录）"));
#else
    (void)st;
    (void)dir;
    log::record("script", "未接 LuaJIT：跳过 package.path 的设置");
#endif
}

bool run_script_file(state& st, std::string_view path) {
 // 路径要变成 C 字符串给 do_file。这里拷一次是必要的：
 // do_file 收的是 const char*，而 string_view 不保证以 '\0' 结尾。
    const std::string owned(path);

    if (st.do_file(owned.c_str())) {
        return true;
    }

 // ---- 失败：把 Lua 的错误原文带出来 ----
 //
 // 【为什么这一步不可省】
 // `do_file` 有两种失败（读入失败 / 执行失败），而它给出的是一句人话。
 // ⇒ "文件不存在"与"脚本里有一个 require 失败"会指向同一句结论。
 // 本轮真的踩到了这一点：真实原因是一个 require 失败，而报出来的是
 // "引导脚本缺失"。⇒ 所以这里把 `last_error_string()`（Lua 原文）附上。
    const char* why = st.last_error_string();
    log::record("script", log::format("脚本失败的 Lua 原文",
                            (why != nullptr && why[0] != '\0') ? why : "(无)"));
    return false;
}

bool run_script_source(state& st, std::string_view source,
                       std::string_view chunk_name) {
    if (!st.is_created()) {
        log::warn("script", "脚本状态未建立，无法执行缓存源码");
        return false;
    }
#if defined(WAG_HAVE_LUAJIT)
    lua_State* const L = st.raw();
    const int top = lua_gettop(L);
    const std::string chunk(chunk_name);
    if (luaL_loadbuffer(L, source.data(), source.size(), chunk.c_str()) != 0) {
        capture_error(L);
        log::warn("script", log::format("缓存脚本编译失败", chunk));
        lua_settop(L, top);
        return false;
    }
    wag::hand_back_scope hand_back;
    if (lua_pcall(L, 0, 0, 0) != 0) {
        capture_error(L);
        log::warn("script", log::format("缓存脚本执行失败", chunk));
        lua_settop(L, top);
        return false;
    }
    return true;
#else
    (void)source;
    (void)chunk_name;
    return false;
#endif
}

} // namespace wag::script
