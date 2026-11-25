// =============================================================================
//  wag/engine/engine.hpp
//
//  内核：引擎实例与全局状态。
//
//  【这一章解决的问题】
//      两条硬约定：
//
//        ① **"传的必须显式"** —— 每帧 / 每实例的可变状态必须显式传，
//           不得放在"谁都能取到的全局"里。
//        ② **可变状态只有两团**，各有一个结构体：
//             · 帧级可变状态 ⇒ 一个**帧结构体**（见 wag/time/frame.hpp）
//             · 引擎级长驻状态 ⇒ 一个**引擎实例结构体**（本文件）
//
//  【为什么危险的不是"全局"这个词，而是"可变 + 隐式"】
//      全局有三种风险：
//        ① **隐式契约**：签名里看不见 ⇒ 影响面不可枚举（这是主要的）
//        ② 初始化/销毁顺序：与显式的启动/关闭阶段冲突
//        ③ 不可注入 ⇒ 不可测
//
//      而**一个只读的全局，与一个沿调用链传的只读引用，在风险上没有区别**。
//      因此本文件把全部可变状态收进一个实例，并显式传下去。
//
//  【全局的唯一正当用法】
//      只放"整个进程一份、且初始化后不变"的东西：
//        · 编译期常量
//        · 日志目标（见 wag/log/log.hpp —— 它是全局的，且这是已定的）
//        · 致命错误出口（同上）
//      ⇒ **判据：它会不会在运行期变？会 ⇒ 不进全局。**
// =============================================================================

#pragma once

#include "log/log.hpp"
#include "engine/control.hpp"
#include "engine/reload_status.hpp"
#include "audio/audio.hpp"
#include "script/reload_preflight.hpp"
#include "text/font.hpp"
#include "space/viewport.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// -----------------------------------------------------------------------------
//  前向声明
//
//  【为什么这里只有前向声明】
//      因为内核要描述"引擎实例由哪些部件组成"，而不需要知道那些部件的内部。
//      window / renderer / lua 状态的完整定义属于它们的模块（平台层、渲染层、
//      脚本层），内核只持有它们并显式传递。
//
//  【这也是内核边界的一处体现】
//      内核知道"有一个窗口、一个渲染器、一个脚本状态"，但不知道 GL、不知道
//      SDL3、不知道 LuaJIT 的具体类型。
// -----------------------------------------------------------------------------
namespace wag::platform {
class window;
enum class fullscreen_mode : std::uint8_t;
}

namespace wag::draw {
class renderer;
}

namespace wag::script {
class state;
}

namespace wag::rng {
class registry;
}

// 容器池集合（定义在 container/pool.hpp）；引擎只持有引用，因此这里前向声明即可。
namespace wag::container {
class container_pool_set;
class collision_world;
}

namespace wag::time {
struct frame_pipeline;
}

namespace wag::input {
class input;
}

// 【命名空间】原 `wag::core`，现为 `wag::`（变更记录见 log/log.hpp 文件头）。
namespace wag {

// -----------------------------------------------------------------------------
//  碰撞请求
//
/// 脚本在帧内声明"这一帧检哪两组"；引擎**不持久化**规则。
/// 请求在阶段 6（Main 分支）末尾由 script::run_collisions 执行，执行/丢弃后
/// 立即 unref 回调。
// -----------------------------------------------------------------------------
struct collision_request {
    enum class kind : std::uint8_t { pair, one };
    kind k = kind::pair;
    std::uint8_t group_a = 0;
    std::uint8_t group_b = 0;
    std::uint64_t handle = 0;   ///< 仅 check_one 用
    int callback_ref = -1;      ///< Lua registry 引用
};

/// 上一次 run_collisions 的统计（只读；wag.collide.stats() 用）。
struct collision_stats {
    double last_ms = 0.0;         ///< rebuild + 宽相 + 窄相 + 回调 的总耗时
    std::uint64_t requests = 0;   ///< 本帧请求数
    std::uint64_t items = 0;      ///< 侧表条目数（rebuild 后）
    std::uint64_t candidates = 0; ///< 宽相候选对总数（各请求累加）
    std::uint64_t hits = 0;       ///< 命中对总数（各请求累加）
};


// -----------------------------------------------------------------------------
//  render_stage —— 当前渲染阶段（绘制收集的"阶段"排序键来源）
//
//  【它为什么在 wag 作用域而不是 engine 类里】渲染器（draw/renderer.hpp）
//    的绘制收集签名要接收它作为排序键 —— 放类里会让渲染层为了一个枚举
//    包含整个引擎头；放 wag 作用域则可以前置声明（带底层类型的枚举
//    前置声明是合法的）⇒ 依赖只剩"知道有这么个类型"。
// -----------------------------------------------------------------------------
enum class render_stage : std::uint8_t {
    none,   ///< 渲染管线未开闸（收集被拒绝）
    scene,  ///< 阶段 8：场景渲染（on_render_main / on_render_pause）
    ext,    ///< 阶段 9：Ext 渲染与收尾（画在最上层）
};

// -----------------------------------------------------------------------------
//  engine_state —— 引擎的运行状态
//
//  【它与 frame_mode 的关系 —— 最容易混的一处】
//
//      engine_state 是**活状态**：它在本帧末尾被改变。
//      frame_mode   是**本帧的形态快照**：它在帧开始时从活状态取一次。
//
//      为什么要分开？因为"按下暂停的那一帧"必须仍然完整地按 Main 运行：
//      它要跑 Main 的逻辑、按 Main 渲染、把世界画出来。若渲染时读活状态，
//      那一帧的世界渲染会被整体跳过，而玩家看到的是"上一帧的画面 + 暂停
//      菜单"—— 那不是按下暂停时应当看到的画面。
//
//      ⇒ 所以：**渲染分支用 frame_mode，不用 engine_state。**
//      ⇒ 这条与"读到的必须是快照，不是活值"是同一个手法。
// -----------------------------------------------------------------------------
enum class engine_state {
    /// 正常：一帧 = 一次逻辑推进，帧号递增。
    normal,

    /// 已暂停：逻辑时间不推进，帧号不递增。
    paused,
};

/// 引擎状态的可读名字（用于诊断输出）。
[[nodiscard]] const char* state_name(engine_state s) noexcept;

// -----------------------------------------------------------------------------
//  pause_source —— "**谁**按住了暂停"
//
//  【为什么不是一个布尔 —— 这一条修的是一个必然发生的错误】
//      旧形态：request_pause() / request_resume() **直接覆盖**一个 pending 布尔。
//      于是"玩家按 Esc 暂停 → 拖窗口 → 松手"这条路径：
//          拖拽开始  request_pause()   （无变化）
//          拖拽结束  request_resume()  （**把玩家的暂停也放了**）
//      ⇒ 撤销一个源，永远不该等于"把状态设回默认"。
//
//  【正确模型：多源取或，状态是派生值】
//      paused = (任一源按住)
//      "normal" 不是一条独立规则，它就是 `!paused` —— **代码里不写出来**。
//      ⇒ 单一真值来源：判断只有 `pause_sources_ != 0` 这一处。
//
//  【为什么是位掩码而不是几个散落布尔】
//      · 解除必须**指名**：release(drag) 结构上不可能清掉 user（防误伤）
//      · 新增源 = 加一位，判断处零改动
//      · 原因集合免费（诊断日志可以直接打 "drag|focus"）
//      · 可断言"没有未知位"：sources & ~all == 0（抓"加了源但忘了处理"）
//  【它的边界】它管"不误伤"，**不管"别忘了按住"**；且它假设所有源**等价**
//      （若将来需要优先级，掩码就不够，得换成有序列表）。
// -----------------------------------------------------------------------------
enum class pause_source : std::uint8_t {
    /// 脚本请求的暂停（`wag.engine.pause()` / `request_pause()`）。
    user = 1u << 0,

    /// 窗口拖拽 / 改变大小（**预留**：等平台钩子验证后启用）。
    drag = 1u << 1,

    /// 窗口失去输入焦点。
    focus = 1u << 2,

    /// 全屏模式切换中（引擎自己按住的过渡源）。
    fullscreen = 1u << 3,
};

/// 全部已知源（用于"没有未知位"的断言）。
inline constexpr std::uint8_t pause_source_all =
    static_cast<std::uint8_t>(pause_source::user) |
    static_cast<std::uint8_t>(pause_source::drag) |
    static_cast<std::uint8_t>(pause_source::focus) |
    static_cast<std::uint8_t>(pause_source::fullscreen);

/// **脚本可以解除**的源。
///
/// 【它存在的意义不是"限制"，是"让这条规则只有一处"】
///   脚本侧唯一的恢复入口是 `request_resume()`，它松的就是这一位。
///   把集合写成具名常量之后：
///     · 新加一个暂停源时，它**默认不在这里**（fail-closed）；
///     · 谁想让脚本能解新位，就必须改这个名字 —— 而这个名字是 search 得到的，
///       改它的人一定会看到"脚本不该解外部条件位"那段注释。
inline constexpr std::uint8_t pause_source_script_releasable =
    static_cast<std::uint8_t>(pause_source::user);

/// 位掩码的可读名字（诊断用；如 `"user|focus"`，空集为 `"none"`）。
[[nodiscard]] const char* pause_sources_name(std::uint8_t sources) noexcept;

// -----------------------------------------------------------------------------
//  engine —— 引擎实例
//
//  【它持有的是"长驻"的东西】
//      窗口、图形上下文、渲染器、脚本状态、视口。
//      而"每帧会变的东西"（帧形态、帧号、本帧变换、窗口尺寸快照、DPI、
//      暂存快照、待应用的状态）**不在本结构体里** —— 它们在帧结构体里。
//
//  【为什么视口在这里】
//      因为视口是长驻状态：它跨帧存在，只有调用方显式设置时才变。
//      而"本帧的变换"是它每帧重算的派生量（见 space::viewport::recompute）。
//
//  【为什么 Lua 状态折叠进实例，而不是每个签名都带一个 lua_State*】
//      否则几乎每个签名都要多一个参数。折叠进实例之后，签名只多一个
//      `engine&`，而显式性不变 —— 读者看得见它。
//
//  【不确定代码 · 待定】是"引擎实例"，但当前设计只允许一个窗口 / 一个
//      上下文。因此本结构体目前的实例化次数必然是 1。
//      保留"实例"这个形状（而不是做成全局）的理由是：**可测性** ——
//      将来若需要两个实例（例如测试），形状不用改。
// -----------------------------------------------------------------------------
class engine {
public:
    engine();
    ~engine();

    /// 不可复制：它持有窗口与图形上下文的唯一所有权。
    engine(const engine&) = delete;
    engine& operator=(const engine&) = delete;

    /// 不可移动。
    ///
    /// 【理由】窗口与上下文是平台资源，且上下文在创建时被"交出"过一次。
    ///   移动一个已经被别处引用过的引擎实例会让那些引用悬空。禁止移动是
    ///   最省的防错方式。
    engine(engine&&) = delete;
    engine& operator=(engine&&) = delete;

    // -------------------------------------------------------------------------
    //  四个成员（与脚本侧的 wag.engine 一一对应）
    //
    //  【为什么恰好是四个】
    //      判据是"控制引擎**是否运行**"。而"进入帧循环"那个入口**不属于**
    //      这里 —— 它是**启动**，不是运行期控制。
    //
    //      这四个的完整性由"**一个事件不能是自己的前提**"证成：任何第五个
    //      控制操作都必须能在引擎运行之前被调用，而那就与"引擎还没开始运行"
    //      矛盾。
    //
    //  【它们的共同点：都不立即生效】
    //      pause/resume 写的是**暂停源位掩码**（2026-10-18 起），exit 写一个标志；
    //      两者的真正应用都发生在帧末（见 frame.hpp 的阶段 11）。
    // -------------------------------------------------------------------------

    /// 请求进入暂停。（= 按住 `pause_source::user`）
    ///
    /// 【不立即生效】本帧仍然完整地按 Main 运行（含渲染）。从下一帧起生效。
    /// 【重复请求安全】同一帧内多次调用与一次等价。
    void request_pause() noexcept;

    /// 请求从暂停恢复。（= 松开 `pause_source::user`）
    ///
    /// ⚠️【只松开 user 这一位】它不是"把状态设成 normal"。
    ///   若此时引擎仍被别的源按住（失焦 / 拖拽），状态**依然是暂停**。
    ///   这正是"撤销一个源 ≠ 设回默认"。
    void request_resume() noexcept;

    /// 按住 / 松开某一个暂停源（引擎内部用：失焦、拖拽）。
    ///
    /// 【为什么解除必须给源名】结构上杜绝"兜底代码把玩家的暂停一起放掉"。
    ///   拖拽被系统中断时应当 `release_pause(drag)`，而**绝不能** `request_resume()`。
    ///
    /// 【谁能解除哪一位】
    ///   每一位的语义是「**某个外部条件当前成立**」还是「**脚本自己表达的意思**」：
    ///     · 外部条件（`focus` / `drag`）⇒ **只能由按下它的那一方解除**；
    ///       脚本**不能**解 —— 否则 `pause_sources_` 说"没暂停"而窗口仍失焦，
    ///       状态与现实不符；而且 `frame.cpp` 下一帧就会按回去（假的、只持续一帧）。
    ///     · 脚本自己的意思（`user`）⇒ 脚本当然能松（`request_resume`）。
    ///   ⇒ 这不是"列一个黑名单"，而是一条**判据**：当前实例是 `{focus, drag}`。
    ///     按名字列会 fail-open（新增一位默认放行），判据天然 fail-closed。
    void hold_pause(pause_source s) noexcept;
    void release_pause(pause_source s) noexcept;

    /// 失焦是否作为暂停源。**默认 true**；只能由启动期定一次
    /// （命令行 `--no-focus-pause`），**没有脚本入口**（理由见 main.cpp 的字段注释）。
    void set_focus_pause(bool on) noexcept { focus_pause_ = on; }
    [[nodiscard]] bool focus_pause() const noexcept { return focus_pause_; }

    // -------------------------------------------------------------------------
    //  动态拖拽（2026-11-14）：拖拽期间由**暴露事件**补跑整帧
    //
    //  【问题】Windows 的拖拽是**模态循环**：`SDL_WaitEventTimeout` 会在里面阻塞
    //    很久（实测 20 秒里阻塞 3.6 秒）⇒ 主循环发不出帧 ⇒ OS 持续暴露新长出来的
    //    客户区像素 ⇒ 白条/黑窗。
    //  【Sub 的做法】它在 `WM_PAINT` 里直接跑整帧。**前提是消息泵在帧外** ——
    //    我们已在 2026-11-14 把泵+等搬出 `run_one_frame`（见 frame.hpp），
    //    所以这条路现在走得通。
    //  【我们的接法】消息钩子只做一件事：**记下"是否在拖拽"**；
    //    补跑由已有的暴露观察者发起（它本来就在拖拽期间被连续调用）。
    // -------------------------------------------------------------------------

    /// 控制通道：命令入口 + `state.json` 出口。默认关闭。
    ///
    /// 【为什么它挂在 engine 而不是 diagnostics】那边是"输出"，这边是"输入"；
    ///   混在一起会让"关掉 `--diagnostics` 就什么都不写"这条判据失效。
    /// 【它不认识结点】只按"第一个空格"切动词与参数，原样交给脚本。
    [[nodiscard]] control::channel& control() noexcept { return control_; }
    [[nodiscard]] const control::channel& control() const noexcept { return control_; }

    /// 音频子系统。
    ///
    /// 【线程】混音跑在 **SDL 的音频线程**；本对象的公开方法**只能在主线程**调用。
    /// 【阶段 1】frame.cpp 调 `commit()` 提交命令、`pump()` 排空回执。
    [[nodiscard]] audio::system& audio() noexcept { return audio_; }
    [[nodiscard]] const audio::system& audio() const noexcept { return audio_; }

    /// 字体库（TTF 第一版，2026-11-18）。
    ///
    /// 【谁用】`wag.font.*` 原语；Lua 侧只通过它拿字形。
    /// 【生命周期】引擎持有；关闭序列第 4 步调 `shutdown()`。
    [[nodiscard]] text::font_library& fonts() noexcept { return fonts_; }
    [[nodiscard]] const text::font_library& fonts() const noexcept { return fonts_; }

    /// 用户是否正在拖拽 / 缩放窗口（`WM_ENTERSIZEMOVE` .. `WM_EXITSIZEMOVE`）。
    [[nodiscard]] bool window_sizemove() const noexcept { return window_sizemove_; }

    /// **只由窗口消息钩子调用**（engine.cpp 的 `sizemove_hook`）。
    void set_window_sizemove(bool on) noexcept { window_sizemove_ = on; }

    /// 拖拽期间的"补跑一帧"入口（由 main 安装；签名故意是裸函数指针 —— 内核不拖 std::function）。
    ///
    /// 【返回值】true = 真的跑了一帧（调用方据此跳过"重画上一帧"）。
    using frame_tick_fn = bool (*)(void* user);
    void set_frame_tick(frame_tick_fn fn, void* user) noexcept {
        frame_tick_fn_ = fn;
        frame_tick_user_ = user;
    }
    [[nodiscard]] bool has_frame_tick() const noexcept { return frame_tick_fn_ != nullptr; }

    /// 调一次补跑入口。没装 / 没跑成 ⇒ false。
    bool tick_frame() noexcept {
        return frame_tick_fn_ != nullptr && frame_tick_fn_(frame_tick_user_);
    }

    // -------------------------------------------------------------------------
    //  整帧像素尺寸（C1，2026-11-16）
    // -------------------------------------------------------------------------
    //
    //  【为什么在 engine 里存一份】`wag.space.get_frame_size()` 要能在**阶段 1 之后**
    //    的任何钩子里读到"本帧的"值；而渲染器里那份 `frame_pixel_` 要到**渲染阶段**
    //    才写。⇒ 在阶段 1 从 `window::get_pixel_size()` 定格一份，与 `dragging`
    //    同一条纪律（帧内一个值，不随外部事件在帧中途变化）。
    //
    //  【唯一来源】仍然是 `window::get_pixel_size()`（平台报的物理像素）；
    //    这里只是**快照**，不是第二处推导。
    void set_frame_pixel_size(int w, int h) noexcept {
        frame_px_w_ = w;
        frame_px_h_ = h;
    }
    [[nodiscard]] int frame_pixel_w() const noexcept { return frame_px_w_; }
    [[nodiscard]] int frame_pixel_h() const noexcept { return frame_px_h_; }


    /// 当前按住了哪些源（诊断用；位掩码见 pause_source）。
    [[nodiscard]] std::uint8_t pause_sources() const noexcept {
        return pause_sources_;
    }

    /// 查询当前状态。
    ///
    /// 【返回的是"本帧开始时的状态"，也就是本帧真正在按哪个形态运行】
    ///   而不是刚请求的变更 —— 请求要到帧末才生效。
    ///   这与"读到的必须是快照"是同一原则：帧内不存在第二个状态真相。
    [[nodiscard]] engine_state get_state() const noexcept { return state_; }

    /// 请求退出。
    ///
    /// 【它是退出游戏的唯一通道】
    ///   回调的返回值不表示退出，也没有别的方式。理由：若回调返回 true/false
    ///   表示退出，就会出现"两个相反的约定"，而且它会绕过帧末的原子性。
    /// 【不立即生效】与 pause/resume 相同。
    void request_exit() noexcept;

    /// 是否已经有退出请求被提交。
    ///
    /// 【它不是第五个成员】
    ///   那四个成员的判据是"**控制**引擎是否运行"，完整性由"一个事件不能是
    ///   自己的前提"证成。而本函数**不控制任何东西** —— 它读的是一个已经被
    ///   request_exit() 写入的标志。因此它落不进那条判据，也不增加第五个
    ///   控制操作。
    ///
    /// 【它与 get_state 的分工】
    ///   get_state     读的是"四个成员共同作用的**唯一结果**"（运行状态）
    ///   本函数        读的是"**某一个成员**提交的请求"
    ///
    /// 【它不注册进脚本侧的 wag.engine】
    ///   脚本侧那张表是**控制面**，仍然是四个。本函数是引擎内部的读，给运行
    ///   驱动（main 的帧循环）用。C++ 类上的方法数不等于 Lua 表上的成员数。
    ///
    /// 【为什么要有这条判据】否则"每加一个请求就加一个 getter"会让四要素
    ///   渐进膨胀成七八个 —— 那才是真正的违反。
    [[nodiscard]] bool exit_requested() const noexcept;

    // -------------------------------------------------------------------------
    //  "进入帧循环"的请求（已决：乙方案）
    //
    //  【它为什么不是"立刻跑循环"】
    //      已定的语义是"帧循环由引导脚本**显式请求**"。而把"跑循环"做成一个
    //      脚本可调用的函数，会让"引擎的主循环"与"一个脚本可调用的函数"变成
    //      同一个东西 —— 于是 main 不再是唯一拥有循环的地方。
    //    ⇒ 所以脚本侧只表达**意图**（置一个标志），循环仍然归 main。
    //
    //  【它比"同步跑循环"好在哪】
    //      若 init.lua 中途报错，标志不会置上 ⇒ main 一看就知道"装载未完成"，
    //      而不是"静默进了循环"。这正是那条原始顾虑（"返回与跑完了是同一个
    //      信号"）要防的事。
    // -------------------------------------------------------------------------

    /// 由 `wag.boot.run()` 调用：记录"脚本已请求进入帧循环"。
    ///
    /// 【它只置标志】不驱动任何东西、不做任何 GPU 操作。
    void request_frame_loop() noexcept;

    /// 脚本是否已经请求进入帧循环。
    ///
    /// 【它是引擎内部的读】与 `exit_requested()` 同类：给 main 的循环驱动用，
    ///   **不注册进脚本侧的 `wag.engine`**（那张表仍然是四个成员）。
    [[nodiscard]] bool frame_loop_requested() const noexcept;

    /// 请求改变目标帧率的上限。
    ///
    /// @param fps  必须 > 0。== 0 或负数属于**数据矛盾**（帧间隔无定义），
    ///             因此由交界处报错，本函数不做兜底。
    ///
    /// 【它改的是上限，不是时间颗粒】
    ///   它不改变"一帧代表多少游戏时间"，只改变"每秒最多跑多少帧"。
    ///   于是压低它 ⇒ 游戏时间推进得变慢。
    ///
    /// 【不立即生效】与 pause/resume 相同：帧末应用，从下一帧起可见。
    ///   这样"一帧内一个目标帧率"成为结构性的保证。
    ///
    /// 【它为什么也是"请求"而不是"立即设置"】
    ///   因为帧率控制器的"睡觉"判据是"目标帧率的值与上一帧不同"。若本函数
    ///   立刻改值，那么控制器在阶段 2 读到的可能是本帧中途改过的值 ——
    ///   于是"这一帧的节拍"会有两个来源。
    void request_target_fps(double fps) noexcept;

    /// 当前生效的目标帧率上限。
    [[nodiscard]] double get_target_fps() const noexcept;

    /// **请求**把窗口客户区改成这个尺寸。
    ///
    /// @param w  宽，单位与"窗口尺寸"同一套（客户区的像素尺寸）
    /// @param h  高，同上
    ///
    /// 【为什么它挂在 engine 上，而落地在 window 上】
    ///   因为"什么时候应用"是**帧结构**的事，而帧结构只认识 engine。
    ///   window 是一个纯平台对象，它不知道帧的存在 —— 这条分工与
    ///   "平台层不得依赖帧结构"那条硬约束一致。
    ///
    /// 【命令式，不是状态式 —— 这是已定的】
    ///   · 引擎**不**记住"当前请求"，也**不**在外部改尺寸时纠正回来
    ///   · 重复请求同一个值 ⇒ 幂等（与上次已应用的请求比较，相同则不动平台）
    ///   · 外部改变（最大化 / 被系统改尺寸）⇒ **不纠正**（尊重外部）
    ///   ⇒ 好处：不会自激（引擎从不因事件回写窗口）
    ///
    /// 【不吸附】请求什么就申请什么。平台给不出来 ⇒ 记一条 L2（可降级，继续）。
    ///   引擎**不**替调用方改值 —— 那是决策，不是执行。
    ///
    /// 【不立即生效】与 pause / resume / request_target_fps 相同：帧末应用。
    ///   于是"一帧一个窗口尺寸"成立 —— 否则帧中途改尺寸会让阶段 1 的快照与
    ///   阶段 7 之后的实际情况不一致。
    void request_window_size(double w, double h) noexcept;

    /// 当前**已应用**的窗口尺寸请求（供引擎内部去重与诊断）。
    ///
    /// 【它不是"窗口现在多大"】窗口多大要问 `window()`；本值是"我们最后
    ///   一次请求的是什么"。命令式下这两者**允许不一致**（平台可能没满足，
    ///   或者外部把它改了）。
    [[nodiscard]] bool has_requested_window_size() const noexcept;
    [[nodiscard]] double requested_window_w() const noexcept;
    [[nodiscard]] double requested_window_h() const noexcept;

    // -------------------------------------------------------------------------
    //  全屏模式请求（命令式；帧末应用）
    // -------------------------------------------------------------------------

    /// 请求切换全屏模式。
    ///
    /// 【请求】帧末应用；切换期间由 `pause_source::fullscreen` 按住暂停。
    /// 【失败】平台拒绝 ⇒ 记 L2 并回退窗口模式，不终止游戏。
    void request_fullscreen(platform::fullscreen_mode mode) noexcept;

    /// 当前窗口实际的全屏模式。
    [[nodiscard]] platform::fullscreen_mode current_fullscreen() const noexcept;

    /// 是否有尚未应用的全屏请求。
    [[nodiscard]] bool has_pending_fullscreen() const noexcept;

    // -------------------------------------------------------------------------
    //  部件的访问（显式传递的落点）
    // -------------------------------------------------------------------------

    /// 窗口。所有权在本实例。
    [[nodiscard]] platform::window& window() noexcept;

    /// 渲染器。所有权在本实例。
    [[nodiscard]] draw::renderer& renderer() noexcept;

    /// 脚本状态。所有权在本实例。
    [[nodiscard]] script::state& script() noexcept;

    /// 视口（长驻状态）。
    [[nodiscard]] space::viewport& viewport() noexcept;

    /// 随机流登记表。所有权在本实例。
    ///
    /// 【为什么它是引擎的部件，而不是脚本侧的东西】
    ///   流的状态是 u64，而 Lua 的数字只有 53 位精确 ⇒ 状态过不了 Lua 边界
    ///   ⇒ 必须住在引擎里，Lua 只拿整数句柄（`wag.rng.*`）。
    /// 【主种子的采集时机】boot_sequence 的"步骤 -1.5"，**在任何脚本之前**。
    [[nodiscard]] rng::registry& rng() noexcept;

    /// 容器池集合。所有权在本实例。
    ///
    /// 【为什么池是引擎的部件】
    ///   容器结构体、句柄、侧表都是跨帧长驻的引擎对象；Lua 只拿句柄
    ///   （`wag.container.*`），拿不到指针，也拿不到"池"里的数组。
    [[nodiscard]] container::container_pool_set& containers() noexcept;

    /// 输入状态机。所有权在本实例。
    ///
    /// 【名字为什么不是 input()】类里若有一个名为 `input` 的成员函数，
    ///   返回类型 `input::input&` 里的 `input` 会被类作用域遮蔽（C++ 名字
    ///   查找），写法既难读又容易错。用 input_system() 明确"这是输入子系统"。
    /// 【谁调它】
    ///   · 帧结构阶段 4：sample(viewport 的本帧变换)；
    ///   · 启动序列：把 window 的 sink 接到它上面；
    ///   · 脚本绑定：读 down/press/release/mouse_*/wheel。
    [[nodiscard]] input::input& input_system() noexcept;

    // -------------------------------------------------------------------------
    //  碰撞
    //
    //  【请求】脚本在帧内声明"这一帧检哪两组"；引擎**不持久化**规则。
    //  【执行】阶段 6 的 Main 分支末尾（on_frame_main 之后），
    //    由 script::run_collisions 统一执行；Pause 分支丢弃。
    // -------------------------------------------------------------------------
    [[nodiscard]] std::vector<collision_request>& collision_requests() noexcept;

    /// 碰撞世界（懒创建；cell_size = 32）。
    [[nodiscard]] container::collision_world& collision_world_ref();

    /// 上一次碰撞执行的可读统计（由 script::run_collisions 写）。
    [[nodiscard]] collision_stats& collision_stats_ref() noexcept;
    [[nodiscard]] const collision_stats& collision_stats_ref() const noexcept;

    // -------------------------------------------------------------------------
    //  绘制相位门（2026，子目标 2：绘序统一管线）
    //
    //  【它管什么】所有绘制都发生在渲染相位的钩子里（见渲染说明
    //    第一节）。绘制收集的入口（renderer 的 draw_*）由此门看守：
    //    阶段 7 开闸（scene）/ 阶段 9 的 Ext 钩子前切到 ext / 阶段 9 结束关闸。
    //    闸外的绘制调用 = 接口误用（L2：该调用不生效，帧结构继续）。
    //  【谁读写】帧结构写（friend）；脚本交界（primitives 绑定）读。
    // -------------------------------------------------------------------------

    /// 绘制是否被允许（脚本交界的相位守卫读它）。
    [[nodiscard]] bool draws_allowed() const noexcept;

    /// 热重载闸门：为 true 时帧管线拒绝推进。
    [[nodiscard]] bool reloading() const noexcept { return reloading_; }

    /// 进入/离开热重载闸门。只允许重载协调器在帧边界调用。
    void begin_reload_gate() noexcept;
    void end_reload_gate() noexcept;

    /// 预检 scripts，并把结果写成 reload_status。
    ///
    /// 【v1 只做预检】不冻结帧、不清容器、不关 Lua。
    /// @return ok = 预检通过；status_written = 状态文件是否写成功
    struct preflight_outcome {
        bool ok = false;
        bool status_written = false;
        std::size_t checked = 0;
        std::size_t error_count = 0;
    };
    preflight_outcome preflight_reload_scripts(const std::string& install_root,
                                               const std::string& session);

    /// 配置热重载命令入口（安装根 + 会话）；启动期调用一次。
    void configure_reload(std::string install_root, std::string session);

    /// 处理一条控制命令；返回 true 表示已被引擎消费（调用方不要再交给脚本）。
    /// v1 只识别 `reload scripts`；其它 reload scope 写 rejected，不执行。
    bool handle_reload_command(std::string_view verb, std::string_view arg);

    /// 每帧控制通道 poll 之后调用：消费队列里的 reload 命令。
    void poll_reload_commands();

    /// 是否有一笔已通过预检、等待帧末提交的 reload。
    [[nodiscard]] bool reload_commit_pending() const noexcept {
        return reload_commit_pending_;
    }

    /// 在帧边界提交上一笔 reload。返回是否成功。
    /// 失败 ⇒ status=runtime_failed，并请求退出。
    bool commit_reload_scripts();

    /// 上一次成功的预检结果（含源码缓存）；未预检 ⇒ nullptr。
    [[nodiscard]] const script::preflight_result* last_preflight() const noexcept;

    /// 当前渲染阶段（绑定层把它传给收集入口，作为"阶段"排序键）。
    [[nodiscard]] render_stage current_render_stage() const noexcept;

    // -------------------------------------------------------------------------
    //  帧末的状态应用
    //
    //  【它不是公开 API】它只由帧结构的阶段 11 调用。
    //  【为什么不做成 public 又注释"别调"】
    //      因为"别调"是纪律，而 friend 是结构。这里用 friend 把纪律变成结构。
    // -------------------------------------------------------------------------

private:
    friend struct time::frame_pipeline;

    /// 开/关绘制相位门，并同时设定当前阶段（帧结构在阶段 7/9 边界调用）。
    void set_draw_phase(bool allowed, render_stage stage) noexcept;

    /// 绘制相位门（见上面的说明）。
    bool draws_allowed_ = false;
    render_stage render_stage_ = render_stage::none;

    /// 热重载闸门：重载事务期间禁止帧管线/钩子/绘制推进。
    bool reloading_ = false;
    bool reload_commit_pending_ = false;

    /// 预检缓存与状态文件（v1 只做预检；提交事务以后用缓存）。
    std::optional<script::preflight_result> last_preflight_;
    std::optional<reload::status_writer> reload_status_;
    std::string reload_install_root_;
    std::string reload_session_;
    std::string reload_status_root_;
    std::string reload_status_session_;

    /// 把本帧请求的状态变更应用到活状态。
    ///
    /// 【它在阶段 11 被调用一次】而它**不做任何 GPU 操作** —— 只做字段赋值。
    ///   这一点是重要的：早期设计里这里有"拍一张快照"的 GPU 操作，于是它可能
    ///   失败，于是"冻结画面与状态切换是同一个原子事件"这条就不成立了。
    ///   现在快照在阶段 10 就已经拍好（暂存），这里只是一次赋值。
    ///
    /// @return 本帧是否发生了状态切换
    bool apply_pending_state() noexcept;

    /// 活状态：本帧末尾由 `pause_sources_` 派生出来。
    engine_state state_ = engine_state::normal;

    /// **暂停源位掩码**（派生 `state_` 的唯一输入）。
    ///
    /// 【它取代了原来的 has_pending_ / pending_】旧形态是"一个可写的状态 + 覆盖写"，
    ///   于是任何一个源解除都会把别的源一起解除。现在这里是**源**，状态是**派生**：
    ///       state_ = (pause_sources_ != 0) ? paused : normal
    /// 【它在阶段 11 被求值一次】与"帧末生效、下一帧可见"那条规则一致。
    std::uint8_t pause_sources_ = 0;

    /// 失焦是否作为暂停源（默认 true；`--no-focus-pause` 关掉它）。
    /// **启动期定一次**，运行期不再变 ⇒ 不需要与 `pause_sources_` 一起派生。
    bool focus_pause_ = true;

    /// 用户是否正在拖拽 / 缩放窗口（由窗口消息钩子写，见 `set_window_sizemove`）。
    bool window_sizemove_ = false;

    /// 本帧的帧缓冲像素尺寸（阶段 1 定格；C1）。默认 0 = "还没有过一帧"。
    int frame_px_w_ = 0;
    int frame_px_h_ = 0;

    /// 控制通道（命令入口 + state.json 出口）。`configure` 之前是关闭的。
    control::channel control_;

    /// 音频子系统（设备、PCM 资源、voice 池）。init/shutdown 由启动/关闭序列调用。
    audio::system audio_;

    /// 字体库（TTF 第一版，2026-11-18）。
    ///
    /// 【为什么放在 engine 而不是 renderer】它要"文件字节 + 图集 + 缓存"三样，
    ///   而 renderer 只该管"图片与顶点"。字体只在**载入时**碰 renderer
    ///   （建页/更新页），其余时候是纯数据。
    text::font_library fonts_;

    /// 拖拽期间补跑一帧的入口（由 main 安装）。空 = 没装 = 退回"重画上一帧"。
    frame_tick_fn frame_tick_fn_ = nullptr;
    void* frame_tick_user_ = nullptr;

    /// 是否已请求退出。
    bool exit_requested_ = false;

    /// 脚本是否已请求进入帧循环（`wag.boot.run()` 置上）。
    ///
    /// 【为什么它是"置上就不再清"】因为请求只有一次：帧循环开始之后，
    ///   再看这个标志没有意义。清它反而会让"是否已请求"变成一个有历史的量。
    bool frame_loop_requested_ = false;

    /// 当前生效的目标帧率上限。
    ///
    /// 【为什么必须有合法初值】它的定义域是 (0, +∞)，而帧率控制里有一条
    ///   "帧间隔 = 1 / 目标帧率"的除法，用 0 会得到无穷。
    ///   "引擎不定义多少算正常"指的是**不判断大小**（1 与 1000 都合法），
    ///   不是"可以没有初值"。
    ///
    /// 【待定】它是否应当来自配置体系（而不是写死在这里）。
    double target_fps_ = 60.0;

    /// 本帧请求的新目标帧率；<= 0 表示"本帧没有请求"。
    ///
    /// 【为什么用一个哨兵值而不是 optional】
    ///   因为 fps 的定义域本身就是 (0, +∞)，所以 0 天然是一个不在定义域内、
    ///   因而可以安全用作"无请求"的值。用 optional 会多一层分支而没有收益。
    double pending_target_fps_ = 0.0;

    // -------------------------------------------------------------------------
    //  窗口尺寸的请求（命令式）
    //
    //  【为什么是"待生效 + 已应用"两个值，而不是一个】
    //      · pending ：本帧脚本请求的尺寸（帧末被读走）
    //      · applied ：**上一次真正申请过的**尺寸（用于幂等去重）
    //    ⇒ 命令式的语义是"请求一次、申请一次"，所以去重的对象是
    //      "上一次申请的值"，而**不是**"窗口现在多大"
    //      （后者会在外部改变窗口后导致引擎去纠正 —— 那就变成状态式了）
    // -------------------------------------------------------------------------

    /// 本帧请求的窗口尺寸。`has_pending_window_size_` 为 false 表示本帧没有请求。
    bool has_pending_window_size_ = false;
    double pending_window_w_ = 0.0;
    double pending_window_h_ = 0.0;

    /// 上一次**已应用**的窗口尺寸请求。`has_applied_window_size_` 为 false 表示
    /// 从未申请过。
    ///
    /// 【为什么初值是"从未申请过"而不是"启动时那个 1920×1080"】
    ///   因为启动时创建窗口**不是**一次"脚本请求"。把它算作一次请求会让
    ///   "脚本请求与启动尺寸相同"这件事被去重掉（于是什么都不做）——
    ///   而那恰好是**正确**的行为。反过来说，若把它算作请求，就等于让引擎
    ///   记住了一个脚本从未表达过的意图。
    bool has_applied_window_size_ = false;
    double applied_window_w_ = 0.0;
    double applied_window_h_ = 0.0;

    // -------------------------------------------------------------------------
    //  全屏模式请求
    // -------------------------------------------------------------------------

    bool has_pending_fullscreen_ = false;
    platform::fullscreen_mode pending_fullscreen_ =
        static_cast<platform::fullscreen_mode>(0);

    /// 应用本帧的全屏请求；失败回退窗口模式并记 L2。
    void apply_pending_fullscreen() noexcept;

    /// 把本帧请求的窗口尺寸应用到平台。
    ///
    /// 【调用点】帧结构的收尾（阶段 12 之后），因此它**不在** 12 个阶段里 ——
    ///   12 个阶段是"逻辑与渲染"的规格，而"改外部设备"是它的收尾动作。
    ///
    /// 【它可能与已应用的请求重复 ⇒ 幂等去重】
    ///   脚本完全可以每帧都喊同一个尺寸；若不幂等，就会每帧都去动窗口，
    ///   在平台上产生无谓的抖动（某些系统会因此反复重绘）。
    ///
    /// 【未满足时的行为】记一条 **L2**（可降级，继续跑），不纠正、不吸附。
    void apply_pending_window_size() noexcept;

    /// 未完成的引擎实例的部件。定义在 .cpp 里（pimpl）。
    ///
    /// 【为什么用 pimpl】
    ///   因为部件的完整类型（window / renderer / script::state）属于别的模块,
    ///   而本头文件不希望把它们全部拖进来。pimpl 让"内核持有它们"与"内核
    ///   不依赖它们的定义"同时成立。
    struct impl;
    impl* impl_ = nullptr;
};

// -----------------------------------------------------------------------------
//  启动与关闭序列
//
//  【为什么这两个函数不放在 engine 类里】
//      因为它们描述的是**进程的生命周期**，而不是"某个引擎实例的操作"。
//      它们的顺序是引擎的规格，而引擎实例只是被它们创建与销毁的东西。
// -----------------------------------------------------------------------------

/// 启动序列的步骤。
///
/// 【已定的次序（脚本侧可见的部分）】
///      步骤 0  引擎预置 13 个钩子槽为空函数（+ 白名单）
///              **（这一步全在 C++ 侧，脚本不参与）**
///      步骤 1  harden   —— 环境封堵（必须在任何用户脚本之前）
///      步骤 2  hooks    —— 启动期自检
///      步骤 3  boot     —— on_game_init（只能用引擎原语）
///      步骤 4  映射层   —— 读配置，把用户词汇装配上
///      步骤 5  按层装载 —— ②Lumi ③Nosa ④Project
///      步骤 6  进入帧循环（由脚本侧显式请求，见下）
///
/// 【步骤 6 的形态是已定的"显式请求"】
///      C++ 调用引导脚本；引导脚本最后调用一个入口；该入口**不返回**，
///      它内部驱动阶段 1~12。
///
///      为什么不用"引导脚本返回即进入循环"（隐式形态）？因为那样的话,
///      "返回"与"跑完了"是同一个信号 ⇒ 中途失败也会照进循环 ⇒
///      "引擎必需"那条规格只在纸面上成立。
///
/// 【不确定代码 · 待定】本函数的具体步骤划分
///      上面那七步是已定的语义，但"它们各自在 C++ 侧做多少、在脚本侧做多少"
///      还没有完全收口。本函数目前只声明形状。
void boot_sequence(engine& eng);

/// 关闭序列。
///
/// 【已定的部分】
///      · on_game_exit 必须在**任何东西被拆之前**
///      · 关闭序列里的错误只记日志，不弹窗，不"带错继续"
///      · on_game_exit **不保证被调用**（强杀、崩溃时没机会）
///
/// 【已定的关闭顺序】
///      1. on_game_exit
///      2. 关闭 Lua 状态（`lua_close` 会触发 `__gc`）
///      3. 第 2 步已包含 finalizer 清理，不再单独跑 full GC
///      4. 释放引擎资源
///      5. 关闭子系统（渲染 → 音频 → 输入 → 窗口）
///      6. 关闭文件系统
///      7. 退出进程
///
///      `__gc` 可能调用引擎 API，因此 Lua 状态必须在字体 / 音频 /
///      SDL / 日志之前关闭。Lua 关闭后的 `~engine` 只作幂等兜底。
///
///      `on_game_exit` 是脚本最后一次可靠使用引擎 API 的时机；
///      `__gc` 只应释放 Lua 自己的资源，不承担存档 / 音频 / 绘制等业务收尾。
void shutdown_sequence(engine& eng);

} // namespace wag
