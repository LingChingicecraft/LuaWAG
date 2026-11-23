// =============================================================================
// wag/script/state.hpp
//
// 内核：脚本状态（Lua 虚拟机）的生命周期。
//
// 【本文件管什么、不管什么】
// 管：Lua 状态的创建与销毁、标准库的装配、以及"封堵必须在任何脚本之前"
// 这条次序保证。
// 不管：具体某个钩子怎么调（在 hook.hpp）、引擎原语的注册（在 primitives）、
// 目录解析与装载（在引导脚本与会话侧）。
//
// 【已定的三条硬约束】
// ① **单 lua_State**，并**明令禁止多 state**
// 理由：LuaJIT 的 GC 是 stop-the-world 且跨 state 共享；userdata 的
// `__gc` 可能在任意 state 的线程触发；而已定的白名单、钩子协议、
// RNG 路由**全部是单 state 假设**。
// ② **FFI 彻底封死**，方案是"五路封堵 + debug 折中"
// ③ **封堵必须在任何脚本被执行之前完成**
// 一旦有脚本跑过，`package.loaded["ffi"]` 与注册表 `_LOADED["ffi"]`
// 就可能已经有值，而"谁已经拿到过引用"是收不回来的。
//
// 【本文件与 LuaJIT 的关系】
// 本头文件**不包含 LuaJIT 的头**，它把 Lua 状态表示为一个不透明的句柄。
// 理由：内核的其余部分（帧结构、错误架构、空间层）不需要知道 Lua 的
// 具体类型，而它们都会被这些头文件间接包含。把 Lua 头挡在 .cpp 里,
// 是内核边界在编译期的一处体现。
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// -----------------------------------------------------------------------------
// Lua 状态的前向声明
//
// 【为什么写成 struct lua_State 而不是 typedef void*】
// 因为 LuaJIT 的公开头文件里它就是 `struct lua_State`。这样写让本头文件
// 与真实类型在链接期兼容，而在编译期不需要包含任何 Lua 头。
// -----------------------------------------------------------------------------
struct lua_State;

// -----------------------------------------------------------------------------
// 引擎实例的前向声明
//
// register_primitives 需要一个引擎实例（原语的 C 函数只收到 lua_State*，
// 所以注册时要把引擎绑到状态上）。这里只前向声明，不包含 engine/engine.hpp ——
// 那是为了不让本头文件把 engine 的头也拖出来。
// -----------------------------------------------------------------------------
namespace wag { class engine; }

namespace wag::script {

// -----------------------------------------------------------------------------
// state —— 脚本状态
//
// 【它拥有一个 lua_State】
// 创建时建立，销毁时关闭。它是引擎实例的成员之一（见 wag::engine）。
//
// 【为什么"创建"与"装配标准库"是两步】
// 因为封堵必须在**库装好之后、任何脚本之前**进行。拆成两步让那个次序
// 在代码上看得见：
// create() —— 建立状态，装入标准库
// seal_environment() —— 封堵（必须在任何脚本之前）
// （此后才可以读脚本）
// -----------------------------------------------------------------------------
class state {
public:
    state() = default;
    ~state();

 /// 不可复制：它持有 lua_State 的唯一所有权。
    state(const state&) = delete;
    state& operator=(const state&) = delete;
    state(state&&) = delete;
    state& operator=(state&&) = delete;

 // -------------------------------------------------------------------------
 // 生命周期
 // -------------------------------------------------------------------------

 /// 建立 Lua 状态，并装入标准库。
 ///
 /// @return 成功时 true
 ///
 /// 【装入哪些库（已定，由 FFI 封死那条政策直接推出）】
 /// base / string / table / math / io / os / coroutine / package / bit
 ///
 /// 【不装入哪些】
 /// ffi —— 而 `require("ffi")` 会**报错**（不是返回 nil，见 seal）
 /// jit —— 同上
 ///
 /// 【debug 的特殊处理】
 /// 它不是"装或不装"，而是**折中**：只留 `traceback`。
 /// 理由：`getregistry` / `getupvalue` / `setmetatable` 是绕过封堵的
 /// 入口；而 `traceback` 让错误信息带上调用栈，对定位问题有用。
    bool create();

 /// 关闭 Lua 状态。
 ///
 /// 【调用时机】关闭序列的第 2~3 步附近。注意它必须在释放引擎资源之前/
 /// 之后的顺序由关闭序列决定（见 wag::shutdown_sequence）。
    void destroy();

 /// 是否已建立。
    [[nodiscard]] bool is_created() const noexcept { return L_ != nullptr; }

 /// 底层的 Lua 状态句柄。
 ///
 /// 【为什么要暴露它】因为引擎原语的注册、钩子的调用、以及引导脚本的
 /// 执行都需要它。而**它的使用范围应当被限制在 script/ 目录内** ——
 /// 别的模块不应该拿着它去调 Lua C API。
    [[nodiscard]] lua_State* raw() noexcept { return L_; }
    [[nodiscard]] const lua_State* raw() const noexcept { return L_; }

 /// 当前 Lua 值栈的深度（栈顶元素个数）。
 ///
 /// 【它为什么在这里】Lua 的头被限制在 script/ 目录内，所以"读栈深"这件事
 /// 必须由本模块提供一个不暴露 Lua 类型的入口。启动序列与自检都要用它来
 /// 确认"进去几个、出来几个"。
 ///
 /// 【临时探针 · 未来可能删掉】它是为排查"栈上留下东西"而加的。
    [[nodiscard]] int stack_depth() const noexcept;

 // -------------------------------------------------------------------------
 // 分派器
 //
 // 【它是什么】引擎侧唯一持有的 Lua 引用：库层安装的 dispatch(kind, handle)。
 // 引擎不持有任何**类级/实例级**引用；分派器是唯一的"服务级"引用。
 // 【安装顺序】：先 set_dispatcher，再 register_class。
 // -------------------------------------------------------------------------

    [[nodiscard]] bool has_dispatcher() const noexcept { return dispatcher_ref_ >= 0; }
    [[nodiscard]] int dispatcher_ref() const noexcept { return dispatcher_ref_; }
    void set_dispatcher_ref(int ref) noexcept { dispatcher_ref_ = ref; }

 /// 调一次分派器（kind, handle, a, b 四个参数）。
 /// @param error 非空时，失败原因写进这里（Lua 错误文本或"未安装"）
 /// @return true = 调用成功；false = 没有分派器或回调报错
    bool call_dispatcher(const char* kind, std::uint64_t handle,
                         std::uint32_t a, std::uint32_t b, std::string* error);

 /// 调一次碰撞回调：参数是一个扁平数组 [a1,b1,a2,b2,…]。
 /// @param ref Lua registry 引用（由 primitives 在声明时建立）
    bool call_collision(int ref, const std::uint64_t* flat, std::size_t n,
                        std::string* error);

 /// 释放一个 registry 引用（碰撞请求执行/丢弃后必须调用）。
    void unref(int ref) noexcept;

 // -------------------------------------------------------------------------
 // 执行脚本
 // -------------------------------------------------------------------------

 /// 读入并执行一个脚本文件。
 ///
 /// @param path 文件路径（UTF-8）
 /// @return 成功时 true；文件读不到或执行出错时 false（错误已记日志）
 ///
 /// 【失败时的行为】记日志（级别取决于失败的性质），但不终止。
 /// 理由：引导脚本的读取失败**会让帧循环无法开始**（因为没有脚本请求
 /// 进入循环），因此它已经是一条自终止的路径 —— 不需要额外终止。
 ///
 /// 【它为什么不做路径解析】路径由调用方给出。目录解析属于引导层的职责。
    bool do_file(const char* path);

 /// 设定**安装根**（exe 所在目录）。由 `publish_install_root` 调用一次。
 ///
 /// 【为什么存在】`do_file` 要把它转成相对 chunk 名（见 `install_root_`）。
    void set_install_root(std::string_view root) noexcept {
        try {
            install_root_.assign(root.data(), root.size());
        } catch (...) {
            install_root_.clear();   // 存不下 ⇒ 退回"只写 basename"，不泄漏
        }
    }

 /// 用零参数的 Lua 函数调用一个全局函数。
 ///
 /// @param global_name 全局函数名
 /// @return 成功时 true；函数不存在或执行出错时 false
 ///
 /// 【用途】引导序列里调用那几个不带帧信息的入口（例如"请求进入帧循环"）。
 /// 带帧信息的钩子不走这里 —— 它们走 hook.hpp 的 call_hook。
    bool call_global(const char* global_name);

 // -------------------------------------------------------------------------
 // 诊断
 // -------------------------------------------------------------------------

 /// 取出栈顶的错误对象的字符串表示。
 ///
 /// 【它为什么在这里而不是在报错模块里】因为错误对象在 Lua 栈上,
 /// 只有持有 lua_State 的一方才能取它。取出来之后交给 wag::fail_script。
    [[nodiscard]] const char* last_error_string() const;

private:
 /// 底层的 Lua 状态。空表示尚未建立。
    lua_State* L_ = nullptr;

 /// **安装根**（exe 所在目录）。由 `publish_install_root` 顺手记下。
 ///
 /// 【用它干什么】把要装载的脚本路径转成**相对安装根**的名字，作为 Lua 的
 /// chunk 名（`@Luminosa/Project/main.lua`）—— 于是：
 /// ① `errors.jsonl` 的 `file` 天然是相对的
 /// ② Lua 的错误消息、`debug.traceback` 里的路径也是相对的
 /// ③ 相对名短（24 字符），不再被 `LUA_IDSIZE=60` 截断成 `...`
 ///
 /// 【它只是"一个目录"】绝不按目录名匹配（`LuaWAG-Luminosa-ver…` 这种名字
 /// 随时会变）——锚点永远是"exe 所在的那个目录"。
    std::string install_root_;

 /// 分派器的 registry 引用。-1 = 尚未安装。
 /// 【它为什么是唯一允许持有的 Lua 引用】：引擎拿不到 self，
 /// 只能经库层分派器回调；且只允许一个入口。
    int dispatcher_ref_ = -1;

 /// 上一次错误的字符串表示（由 last_error_string 返回）。
 ///
 /// 【为什么缓存一份】因为 Lua 的错误对象在 pcall 返回之后可能被回收,
 /// 而报错流程可能在那之后才读它。缓存一份让"取出错误文本"与"处理错误"
 /// 解耦。
 ///
 /// 【为什么是 mutable】last_error_string() 是 const 方法，而它要往这里写。
 /// mutable 的用意正是"const 方法里也要修改的缓存"。不加它的话，
 /// 在这个 const 方法里 error_buf_ 的类型是 const char[256]，
 /// 于是它无法传给 snprintf（要 char*）。
    mutable char error_buf_[256] = {};
};

// -----------------------------------------------------------------------------
// 环境封堵与引导
//
// 【它们是自由函数而不是 state 的成员】
// 因为它们是**对状态做的动作**，而不是状态自身的性质。放在 free 函数
// 里让"封堵发生在哪一步"在调用序列上看得很清楚。
// -----------------------------------------------------------------------------

/// 把 FFI（以及 jit）封死，并把 debug 折中为只含 traceback。
///
/// 【必须在任何脚本被执行之前调用】
///
/// 【五路封堵（已定）】
/// ① `package.preload["ffi"] = 报错函数`（**显式报错**，不是返回 nil）
/// ② `package.loaded["ffi"] = nil`
/// ③ 清注册表 `_LOADED["ffi"]`
/// ④ `debug` 折中成只含 `traceback` 的表
/// ⑤ `package.preload["jit"] = 报错函数`
///
/// 【为什么"只把 package.loaded.ffi 设成报错"这一路不够】
/// `require` 先查 `package.loaded`，非 nil 就**直接返回它**。于是
/// `local ffi = require("ffi")` 会拿到一个**函数**而不是报错 —— 更难查。
///
/// 【若将来自建 LuaJIT】
/// 走**编译期不编译 FFI**，而上面五路降级为双保险。
void seal_environment(state& st);

/// 预置 13 个钩子槽为空函数（启动序列的**步骤 0**）。
///
/// 【它全在 C++ 侧】
/// 按已定的启动次序，步骤 0 发生在任何脚本之前，因此脚本侧不可能参与它。
/// 这一点曾与"WagLib/hooks.lua 做步骤 0 的脚本侧一半"的说法冲突，现已
/// 改为：**hooks.lua 只做启动期自检，不重列名字**。
void install_hook_noops(state& st);

/// 启动期自检：`wag.hook` 上的现有成员是否都是函数。
///
/// @return 全部是函数时 true
///
/// 【它只检查"是不是函数"，不重列那 13 个名字】
/// 重列会造成"同一个契约写两遍，而只有 C++ 那份是权威"，失效形态是
/// 两份清单悄悄分歧 —— 而那只在某个钩子"永远不被调用"时暴露。
///
/// 【它不是必要机制】
/// C++ 侧已有三道检查：预置后的核对、调用点的判定、名字清单唯一。
/// 它买到的只是"启动时一次清清楚楚的失败"，而不是"某个钩子第一次被
/// 调用时的一炸"。因此**它缺失时引擎照跑**。
bool verify_hooks_are_functions(state& st);

/// 注册引擎原语（wag.engine / wag.space / wag.time）。
///
/// 【它需要引擎实例】原语的 C 函数只收到 lua_State*，所以必须在注册时把引擎
/// 实例绑定到那个状态上（见 primitives.cpp 的 engine_of）。
///
/// 【调用点】启动序列的步骤 0 之后 —— 那时 wag 表已经由 install_hook_noops
/// 建立。
void register_primitives(state& st, engine& eng);

// -----------------------------------------------------------------------------
// 引导脚本的定位与读入
//
// 【它们为什么在这里，而不在 main.cpp】
// 说明：`wag::script` 是"与 Lua 相接的一切"。
// 而"设 package.path"要用 Lua 的 C API、"读引导脚本"要读 Lua 的错误文本
// ⇒ 两件事都是"与 Lua 相接"，属于本层。
//
// 【它们原来在 main.cpp 的两个后果（都是真实发生过的）】
// ① main.cpp 必须直接 include lua.h，而它是**装配层**，不该知道具体技术
// ② 它必须能看到 `WAG_HAVE_LUAJIT` 这个编译期的宏 —— 而那个宏只由
// `wag_luajit` 这个 INTERFACE 目标携带，且当时只有 wag_script 链了它
// ⇒ main.cpp 走了 `#else` 降级分支 ⇒ **package.path 静默地没被设置**
// ⇒ 引导脚本里第一行 require 就失败
// ⇒ 搬进来之后，那个宏的问题自然消失（本层本来就链了它）。
// -----------------------------------------------------------------------------

/// 把 `<dir>/?.lua` 与 `<dir>/?/init.lua` 前置到 `package.path`。
///
/// @param dir 形如 `<安装根>/Luminosa`（**不含**末尾分隔符）
///
/// 【为什么这件事必须由 C++ 做，而不是写在脚本里】
/// 因为引导脚本自己第一行就要 `require("WagLib.harden")` —— 若那条模式由脚本
/// 设置，就成了"要先能 require 才能设置 require 的路径"。
///
/// 【为什么只设这一条模式，四层枚举不在这里】
/// 已定：C++ 只保证"能找到 WagLib 自己"，而"有哪四层、每层有哪些模块"是
/// **目录形状**，属于脚本侧的 resolve.lua。
/// 理由：1.2 说"目录可以变"⇒ 把形状写进 C++ 会让目录变动要重编引擎。
void prepend_package_path(state& st, std::string_view dir);

/// 把**安装根**（只读）发布到 Lua 侧：`wag.install_root`。
///
/// @param root 安装根，**不带末尾斜杠**（已定：五名固定、路径相对安装根）
///
/// 【为什么由 C++ 发布，而不是让脚本自己猜】
/// 已定候选 (甲)：引擎启动时把安装根交给脚本。候选 (乙)（脚本从 package.path
/// 或 debug 推）不可靠 —— 它依赖装载方式；而"当前工作目录 ≠ exe 所在目录"
/// 是常态（从快捷方式、资源管理器、IDE 启动都可能不同）。
///
/// 【为什么是"只读"】脚本只读它来**拼路径**；写它没有意义（引擎每帧不从 Lua 取
/// 任何路径）。已定："引擎不解析路径" ⇒ 拼接发生在脚本侧。
///
/// 【它必须早于引导脚本】resolve.lua 的 `install_root()` 读它；若引导脚本
/// 已经要加载资源，晚发布就意味着"第一次加载拿不到根"。
void publish_install_root(state& st, std::string_view root);

/// 读入并执行一个脚本文件，失败时把 **Lua 的错误文本**带进日志。
///
/// @return 成功时 true
///
/// 【它与 `state::do_file` 的区别 —— 为什么两个都要有】
/// `do_file` 是**执行**（它只回答成败），而本函数是**引导的一次动作**：
/// 它额外做一件事 —— 把 `last_error_string()` 里的 Lua 原文附到日志上。
/// ⇒ 那一条很重要：`do_file` 有两种失败（读入失败 / 执行失败），
/// 而**"文件不存在"与"脚本里有语法错"会给出同一句人话**。
/// 没有 Lua 原文，"引导脚本缺失"这个结论就掩盖了真实原因。
/// （这正是本轮真的踩到的：真实原因是一个 require 失败。）
bool run_script_file(state& st, std::string_view path);

/// 执行一段**已缓存**的源码（热重载提交用；chunk_name 带 `@`）。
bool run_script_source(state& st, std::string_view source,
                       std::string_view chunk_name);

} // namespace wag::script
