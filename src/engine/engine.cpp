// =============================================================================
// wag/engine/engine.cpp
//
// 引擎实例的实现。
//
// 【本文件实现的四件事】
// ① wag.engine 的四个成员（请求语义：不立即生效）
// ② 状态切换的原子性（apply_pending_state 只做字段赋值，不做 GPU 操作）
// ③ 部件的持有与显式访问
// ④ 启动 / 关闭序列的形状
//
// 【本文件刻意不做的事】
// 它不创建窗口、不建 Lua 状态、不初始化渲染器。那些属于启动序列，
// 而"每一步在 C++ 侧做多少、在脚本侧做多少"已按模块切开：
// 引擎只备基础设施，**层与脚本内容归 Lua 侧**（见 engine.hpp 的注释）。
// =============================================================================

#include "engine/engine.hpp"

#include "container/collide.hpp"
#include "container/pool.hpp"
#include "error/error.hpp"
#include "input/input.hpp"
#include "log/log.hpp"
#include "draw/renderer.hpp"
#include "platform/window.hpp"
#include "script/hook.hpp"
#include "script/state.hpp"
#include "rng/rng.hpp"

#include <cstdio>
#include <cstdint>
#include <ctime>

#if defined(WAG_HAVE_SDL3)
#include <SDL3/SDL.h>
// 【2026-10-18 · 消息钩子探针需要 Win32 的 MSG 与 WM_*】
// SDL_system.h 只 `typedef struct tagMSG MSG;`（前向声明），**不含 windows.h**
// ⇒ 要用 msg->message 与 WM_ENTERSIZEMOVE，必须自己引。
// NOMINMAX / WIN32_LEAN_AND_MEAN 是惯例：不引 min/max 宏、少引无关头。
#if defined(SDL_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#endif

namespace wag {

namespace { bool initialize_script_runtime(engine& eng); }

#if defined(WAG_HAVE_SDL3)
namespace {

// =============================================================================
// 窗口暴露（EXPOSED）的观察者 —— "白条"补丁（2026-10-15 启用）
//
// 【它治什么 —— 已定位的根因，不是猜测】
// 手拖窗口边缘期间，SDL_PollEvent 会长时间阻塞（实测：20 秒里阻塞 3.6 秒）
// ⇒ 我们的帧发不出来 ⇒ 而 Windows 在持续暴露新长出来的客户区像素
// ⇒ 那些像素没有人画 ⇒ 显示窗口背景（白条，向右/向下拖时出现）。
// 出路是 SDL 官方写的：SDL_AddEventWatch + 监听 SDL_EVENT_WINDOW_EXPOSED，
// 在回调里做一次最小重画（wiki）。
//
// 【为什么现在启用】
// “不加补丁”只在一个前提下成立：拖拽被禁用、尺寸只走脚本
// （那样 live-resize 恒为 0，补丁是死代码）。而“禁用拖拽”至今没有决策、
// 没有实现 —— **前提当前不成立** ⇒
// 补丁是当前唯一有效的解法，应当启用。
// 而启用它对脚本路径**零代价**：补丁只认 live-resize（data1 != 0），
// 实测脚本改尺寸 9 次换档 live-resize 全部为 0 ⇒ 回调在正常路径上
// 永不触发。等将来真的禁了拖拽，它自动退化为死代码，届时可再撤。
//
// 【回调里的三条约束 —— 全部来自实测，缺一不可】
// ① **主线程判断**：SDL 明说观察者回调可能不在主线程（本机实测全在
// 主线程，但不能依赖）⇒ 不是主线程就什么都不做。
// ② **只做"重画上一帧 + 呈现"**：回调是渲染层的第二个入口，它不碰场景、
// 不碰状态机、不碰钩子、不跑帧。实测开销几乎全在 SwapWindow 上
// ⇒ 60fps 的帧预算 16.7 ms，多做的每一件事都直接吃掉它。
 // 回调必须重画上一帧，而不是只清成信箱黑；否则拖拽期间客户区会全黑。
 // 现有做法是 `renderer::redraw_last_frame()`：清屏之后把上一帧的
 // staged 快照 1:1 画上去 ⇒ 拖拽期间看到的是定格画面。
// staged 快照**1:1 画上去 ⇒ 拖拽期间看到的是定格画面。
// ③ **必须呈现**：实测"只画不 Swap"等于什么都没做 —— 合成器认为这一帧
// 没有被提交，会把上一帧拉伸着显示。这是五组对照实验里信息量最大
// 的一条反证（探针 sdl3_expose_probe 组 3）。
//
// 【userdata 为什么是 engine*】
// 回调需要渲染器（清屏）与窗口（呈现）；engine 同时拥有它们，且有公开
// 访问器 ⇒ 不需要为它新开一个全局（可变状态必须显式）。
// =============================================================================

bool SDLCALL expose_watch(void* userdata, SDL_Event* event) {
 // 【返回值被忽略】SDL_events.h 明写观察者的返回值没有语义，
 // 所以这里返回什么都行；false 表示"没有处理"，只为可读。
    if (userdata == nullptr || event == nullptr) {
        return false;
    }

 // 【认哪些事件 —— 2026-10-18 扩过一次，理由是被实测逼出来的】
 //
 // ① `EXPOSED` 且 `data1 != 0`：SDL 用 data1 标"live-resize 期间的暴露"。
 // 实测：**拖标题栏移动时也是 data1 = 1**（连续 20 条），因为 Windows 的
 // `WM_ENTERSIZEMOVE` 对"移动"和"缩放"是同一条消息。
 // ② `MOVED` / `RESIZED` / `PIXEL_SIZE_CHANGED`：**必须也认**。
 // 原因：只靠 EXPOSED 会有"黑窗期" —— 窗口挪动之后、下一次 WM_PAINT
 // 之前，客户区是空的（实测：同构建连跑三次，拖拽中非黑像素占比
 // 81.4% / 73.0% / 72.9%，剩下的是没赶上重画的时刻）。
 // 这三条事件在拖拽期间每一步都会来 ⇒ 每一步都重画一次 ⇒ 黑窗期消失。
 //
 // 【为什么这样做是安全的】重画的只是"上一张完整画面"，幂等；
 // 主循环正常跑时它最多把旧画面显示几毫秒，肉眼不可见。
    const bool is_exposed = (event->type == SDL_EVENT_WINDOW_EXPOSED &&
                             event->window.data1 != 0);
    const bool is_moved = (event->type == SDL_EVENT_WINDOW_MOVED);
    const bool is_resized = (event->type == SDL_EVENT_WINDOW_RESIZED ||
                             event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED);
    if (!is_exposed && !is_moved && !is_resized) {
        return false;
    }

 // 【约束①】主线程判断。不是主线程 ⇒ 什么都不做（碰 GL 与窗口都不安全）。
    if (!SDL_IsMainThread()) {
        return false;
    }

    auto& eng = *static_cast<engine*>(userdata);
    draw::renderer& rend = eng.renderer();
    platform::window& win = eng.window();

 // 【渲染器是否可用 —— 廉价、防御性】
 // 实测回调不会在主循环绘制中触发（阻塞时主循环卡在事件泵里），
 // 但这个判断的成本是零，而它的失效形态（对未就绪的 GL 提交）很贵。
    if (!rend.has_context() || !win.is_created()) {
        return false;
    }

 // 【动态拖拽（2026-11-14）：拖拽/缩放期间**只由这里补跑整帧**】
 //
 // 拖拽期间 Windows 在自己的模态循环里，`SDL_WaitEventTimeout` 会长时间阻塞
 // ⇒ 主循环发不出帧；而 OS 在持续暴露新长出来的客户区像素 ⇒ 白条/黑窗。
 // 唯一还在到的东西就是这些暴露/尺寸事件，所以把它们当**节拍器**用。
 //
 // 【为什么现在才敢这么做】2026-11-14 把"泵+等"搬出了 `run_one_frame`
 // ⇒ 从事件观察者里跑一帧不再重入 12 阶段管线。三条门控在 main 装的
 // `drag_tick` 里：**在拖拽中**、**不在帧内**、**到点了**（不睡不提前）。
 //
 // 【这里显式豁免了“回调里只许清屏/blit、成本 ≤5ms”那两条约束】
 // 那两条要求"回调里只许清屏/blit、成本 ≤5ms"，前提是"主循环被卡死，
 // 回调是唯一出口"。今天前提变了：回调里跑的是**完整 12 阶段**。
 // 豁免的理由是：回调里跑的是完整 12 阶段，那两条约束的前提已经不成立。
 //
 // 【没到点就什么都不画】屏幕留着上一帧，最多 16 ms 后就有新的整帧。
 // **绝不能用快照顶**：快照是"按请求才拍"的（B 方案），可能比屏幕旧很多，
 // 拿它去盖一张刚画好的整帧 = 画面倒退。
    if (eng.window_sizemove()) {
        eng.tick_frame();
        return false;
    }

 // 【帧缓冲多大 —— 在回调那一刻读】
 // 拖拽期间窗口尺寸在平台上已经是新值，读数跟上即可；
 // 读到 0（罕见的最小化瞬间）就没有可画的东西，直接放弃这一次重画。
    const platform::dimensions px = win.get_pixel_size();
    const int fw = static_cast<int>(px.w + 0.5);
    const int fh = static_cast<int>(px.h + 0.5);
    if (fw <= 0 || fh <= 0) {
        return false;
    }

 // 【没有快照 ⇒ 不清屏、不呈现】（2026-11-14 修复 · 拖拽闪黑的根因）
 //
 // `redraw_last_frame` 的实现是"**清成信箱黑** + 画快照"，
 // 快照为空时它只剩那块黑。而 B 方案（2026-10-22）之后快照是"按请求才拍"的
 // ⇒ **正常游戏里它永远是空的** ⇒ 不拦的话，每一次普通暴露都会把一块黑
 // 推上屏幕（实测：拖拽期间的回退呈现有约 15% 是 100% 黑，且每次都伴随
 // `staged_valid=0`）。
 //
 // 【为什么是"什么都不做"而不是"清成别的颜色"】屏幕上现在那一帧是**真的**，
 // 再过 ≤16 ms 就会有新的；任何"替代画面"都比它旧。
    if (!rend.has_staged_snapshot()) {
        return false;
    }

 // 【约束② + ③：画一张"上一帧"，然后必须呈现】顺序固定，两者缺一不可。
 //
 // ⚠️【这一处从"清屏"改成了"重画上一帧"（2026-10-18）】
 // 旧行为是 `clear_framebuffer` ⇒ 清成**信箱黑** ⇒ 拖拽期间客户区**全黑**
 // （实测：客户区 100% 黑像素、0 个场景像素）。
 // 现在它把**上一帧的完整结果**（staged 快照）1:1 画上去 ⇒ 看到的是定格画面。
 //
 // 【路径已收窄】拖拽/缩放走上面那条（补跑整帧）；这里只剩"非拖拽的普通暴露"，
 // 而且**必须先有快照**。
    rend.redraw_last_frame(fw, fh);
    win.present();
    return false;
}

/// 安装观察者。【调用点】boot_sequence：窗口与渲染器都就位之后。
void install_expose_watch(engine& eng) {
    if (!SDL_AddEventWatch(&expose_watch, &eng)) {
 // 【为什么是 L2 而不是 L3】观察者缺失的后果是"拖拽时白条"，不是
 // "引擎跑不了" —— 它是可降级的失败，记下来继续跑。
        log::warn("engine", log::format("窗口暴露观察者安装失败",
                                "拖拽窗口边缘期间可能出现白条（详见坐标系链条 6.2）"));
        return;
    }
    log::record("boot", "窗口暴露观察者已安装（拖拽时重画上一帧 + 呈现）");
}

// =============================================================================
// 拖拽状态的消息钩子
//
// 【为什么用消息钩子而不是 SDL 事件】
// SDL 没有暴露"开始拖拽 / 结束拖拽"这两件事，而 Windows 的
// `WM_ENTERSIZEMOVE` / `WM_EXITSIZEMOVE` 就是它们的原文。
// 探针（2026-10-18）已实测：`SDL_SetWindowsMessageHook` **能看到**这两条。
//
// 【它只做一件事】记下"现在在不在拖拽"。
// **不跑帧、不碰渲染、不调脚本** —— 补跑帧由暴露观察者发起（三条门控在那里）。
//
// 【为什么 return true】不吞消息 ⇒ SDL 与 DefWindowProc 照常处理，
// 窗口的非客户区与重绘行为保持系统默认。吞掉等于"我们自己实现拖拽"。
// =============================================================================
#if defined(SDL_PLATFORM_WINDOWS)
bool SDLCALL sizemove_hook(void* userdata, MSG* msg) {
    if (userdata == nullptr || msg == nullptr) {
        return true;
    }
    auto& eng = *static_cast<engine*>(userdata);
    if (msg->message == WM_ENTERSIZEMOVE) {
        eng.set_window_sizemove(true);
 // 只在**转移**时记一条：拖拽期间每条消息都记会刷屏。
        log::record("engine", "拖拽开始", "拖拽/缩放期间由窗口暴露事件补跑整帧");
    } else if (msg->message == WM_EXITSIZEMOVE) {
        eng.set_window_sizemove(false);
        log::record("engine", "拖拽结束");
    }
    return true;
}
#endif

/// 安装拖拽状态钩子。【调用点】boot_sequence：与暴露观察者同一处（窗口就位之后）。
void install_sizemove_hook(engine& eng) {
#if defined(SDL_PLATFORM_WINDOWS)
 // 【SDL3 的这个 API 返回 void】没有失败可判 —— 它只是把回调存进 SDL 的全局槽位。
 // 若将来换成有返回值的版本，这里应当补一条"装不上就退回重画上一帧"的日志。
    SDL_SetWindowsMessageHook(&sizemove_hook, &eng);
    log::record("boot", "拖拽状态钩子已安装（WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE）");
#else
    (void)eng;
#endif
}

// =============================================================================
// 【探针 · 2026-10-18】SDL_SetWindowsMessageHook 能不能看到 WM_ENTERSIZEMOVE
//
// 【为什么必须验】当时押的是"**拖拽 → 走 Pause 线**"这个方案：
// 若钩子能看到 WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE ⇒ 方案是 S/M 级；
// 若看不到 ⇒ 要做 Win32 子类化（SetWindowLongPtr 换 WndProc），升到 M/L。
// ⇒ 这是"便宜的信息、昂贵的假设"，所以先测。
//
// ⚠️【2026-11-14：方案换了，但这个结论仍然有用】
// 现在不做"拖拽就暂停"了 —— 改成**动态拖拽**（拖拽期间照常跑帧，见
// `sizemove_hook` 与暴露观察者里的补跑）。而那条路**同样**依赖"钩子能看到
// 这两条消息"这个事实 ⇒ 探针的结论被第二次用上了，只是用途变了。
//
// 【这条钩子和 expose_watch 的位置差别】
// expose_watch 挂在 SDL 的**事件派发**之后（拿到的是 SDL_Event）；
// 本钩子挂在 SDL 的 **WndProc 内部、原始消息处理之前**（拿到的是 MSG）
// ⇒ 后者更早、更原始，能看到 SDL 自己会吞掉的消息。
// =============================================================================
#if defined(SDL_PLATFORM_WINDOWS)
// 【开关注释 2026-10-18】探针已完成使命：确认 SDL_SetWindowsMessageHook 能看到
// WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE（实测 #2 与 #8，且窗口确实移动了）。
// ⇒ 默认关闭，需要复测时改 true。
constexpr bool kWindowsMessageProbe = false;

[[maybe_unused]] bool SDLCALL message_probe_hook(void* userdata, MSG* msg) {
    (void)userdata;
    if (msg == nullptr) {
        return true;
    }
 // 只关心拖拽/绘制相关的几条，且只打前 60 条，避免刷屏。
    const bool interesting = (msg->message == WM_ENTERSIZEMOVE ||  // 0x0231
                              msg->message == WM_EXITSIZEMOVE ||   // 0x0232
                              msg->message == WM_NCLBUTTONDOWN ||  // 0x00A1
                              msg->message == WM_NCLBUTTONUP ||    // 0x00A2
                              msg->message == WM_SYSCOMMAND ||     // 0x0112
                              msg->message == WM_SIZE ||           // 0x0005
                              msg->message == WM_MOVE);            // 0x0003
 // 【WM_PAINT 不记】它在启动阶段就会连发，会把真正要看的消息淹没。
 // 本次要验的是 WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE。
    if (!interesting) {
        return true;
    }
    static int n = 0;
    if (n >= 60) {
        return true;
    }
    ++n;
    const char* name = "其它";
    switch (msg->message) {
    case WM_ENTERSIZEMOVE: name = "WM_ENTERSIZEMOVE"; break;
    case WM_EXITSIZEMOVE:  name = "WM_EXITSIZEMOVE";  break;
    case WM_NCLBUTTONDOWN: name = "WM_NCLBUTTONDOWN"; break;
    case WM_NCLBUTTONUP:   name = "WM_NCLBUTTONUP";   break;
    case WM_SYSCOMMAND:    name = "WM_SYSCOMMAND";    break;
    case WM_PAINT:         name = "WM_PAINT";         break;
    case WM_SIZE:          name = "WM_SIZE";          break;
    case WM_MOVE:          name = "WM_MOVE";          break;
    default: break;
    }
    log::warn("engine", log::format("诊断·MSG",
                            std::string(name) + " wParam=" +
                                std::to_string(static_cast<unsigned long long>(msg->wParam)) +
                                " #" + std::to_string(n)));
    return true; // 继续处理（true = 不丢弃）
}
#endif // SDL_PLATFORM_WINDOWS

/// 卸载观察者。【调用点】shutdown_sequence：SDL_Quit 之前。
void remove_expose_watch(engine& eng) {
    SDL_RemoveEventWatch(&expose_watch, &eng);
}

} // namespace
#endif // WAG_HAVE_SDL3

// 本文件里的 `log::` 就是 `wag::log`（同在 wag 内，直接查找即可，不需要别名）。

// =============================================================================
// 名字表
// =============================================================================

const char* state_name(engine_state s) noexcept {
    switch (s) {
    case engine_state::normal:
        return "normal";
    case engine_state::paused:
        return "paused";
    }
    return "?";
}

const char* pause_sources_name(std::uint8_t sources) noexcept {
    // 【不用拼接 std::string】调用点只用于日志；这里用线程局部缓冲，
    // 避免在 noexcept 诊断函数里分配。
    static thread_local char out[64];
    struct item {
        pause_source bit;
        const char* name;
    };
    constexpr item items[] = {
        {pause_source::user, "user"},
        {pause_source::drag, "drag"},
        {pause_source::focus, "focus"},
        {pause_source::fullscreen, "fullscreen"},
    };

    const std::uint8_t active = static_cast<std::uint8_t>(sources & pause_source_all);
    if (active == 0) {
        return "none";
    }

    std::size_t used = 0;
    out[0] = '\0';
    for (const item& it : items) {
        if ((active & static_cast<std::uint8_t>(it.bit)) == 0) {
            continue;
        }
        if (used != 0 && used + 1 < sizeof(out)) {
            out[used++] = '|';
        }
        const char* p = it.name;
        while (*p != '\0' && used + 1 < sizeof(out)) {
            out[used++] = *p++;
        }
    }
    out[used] = '\0';
    return out;
}

// =============================================================================
// 部件
//
// 【为什么用 pimpl】
// 因为 window / renderer / script::state 的完整定义属于别的模块，而
// engine.hpp 不希望把它们全部拖进来。"内核持有它们"与"内核不依赖它们的
// 定义"这两件事，由 pimpl 同时满足。
// =============================================================================

struct engine::impl {
 // 窗口。所有权在这里。
    platform::window win;

 // 渲染器。它在窗口创建之后接收 gl_context 并独占它（形态乙）。
    draw::renderer rend;

 // 脚本状态。单 lua_State（已定：禁止多 state）。
    script::state sc;

 // 视口。长驻状态：跨帧存在，只有显式设置时才变。
    space::viewport vp;

 // 随机流登记表。主种子在 boot 时采集（见 boot_sequence 的步骤 -1.5）。
    rng::registry rngs;

 // 容器池集合。池在脚本调用 wag.container.new_pool 时按需建立；
 // 池 id 单调分配、永不复用，所以放在 impl 里跨帧长驻。
    container::container_pool_set pools;

 // 输入状态机（阶段 4 采样；消息路径由 window 的 sink 喂事件）。
 // 【为什么它在这里】它是跨帧长驻的状态；而"谁能写它"由 window 的
 // set_input_sink 接线唯一决定（消息路径只写数据，不调脚本）。
    input::input inp;

 // 碰撞：本帧请求 + 懒创建的世界（rebuild 每帧一次）。
    std::vector<collision_request> col_requests;
    std::unique_ptr<container::collision_world> collide;
    collision_stats col_stats;
};

engine::engine() : impl_(new impl()) {
 // 【构造不做事】这是一个刻意的性质：构造不会失败，因此启动序列里的失败
 // 只可能来自"动作"（创建窗口、建 Lua 状态），而那些动作有明确的返回值
 // 或明确的报错路径。
 //
 // 这与 platform::window 的处理方式一致（构造一个尚未创建的窗口）。
 //
 // 【唯一例外：把渲染器接给字体库】它只是存一个指针（不碰 GL），
 // 放在这里是为了避免"每次 glyph 调用都传 renderer"。
 // 注意 renderer 住在 impl 里，而字体库是 engine 的直接成员（见 engine.hpp）。
    fonts_.attach(impl_->rend);
}

engine::~engine() {
 // 【析构只释放 pimpl】主关闭路径由 shutdown_sequence() 显式执行。
 //
 // 正常路径下 shutdown_sequence() 已经先调用 script().destroy()；
 // 这里 delete impl_ 时 script::state 的析构只是幂等兜底。
 //
 // 为什么不在析构里承担主关闭：构造/析构顺序在 C++ 里不保证跨编译单元一致，
 // 而关闭序列是一个**有序**的过程（on_game_exit 必须在任何东西被拆之前）。
 // 把它交给一个显式函数，顺序就成了代码而不是约定。
    delete impl_;
    impl_ = nullptr;
}

// =============================================================================
// 四个成员
//
// 【它们的共同点：都只写请求，不改变活状态】
// 真正的应用发生在阶段 11（见 frame.cpp 的 stage_state_apply）。
// 这一个共同点就是"状态变更在帧末生效"那句话在代码上的全部内容。
// =============================================================================

void engine::request_pause() noexcept {
 // 【它只按下一个源】状态不是被"设置"的，是被多个源**按住**的。
 // 见 engine.hpp 里 pause_source 的说明。
    hold_pause(pause_source::user);
}

void engine::request_resume() noexcept {
 // 【只松开 user 这一位】若引擎仍被别的源按住（失焦 / 拖拽），
 // 状态**依然是暂停** —— "撤销一个源 ≠ 设回默认"。
    release_pause(pause_source::user);
}

void engine::hold_pause(pause_source s) noexcept {
    const std::uint8_t bit = static_cast<std::uint8_t>(s);
    if ((bit & static_cast<std::uint8_t>(~pause_source_all)) != 0) {
 // 未知位：说明有人传了一个不在 pause_source 里的值（枚举被扩展但
 // pause_source_all 忘了同步）。不静默——记一条 L1（不终止：
 // 状态机不该因为一个日志问题停机）。
        log::warn("engine", "hold_pause() 收到未知的暂停源位（pause_source_all 可能忘了同步）");
        return;
    }
    pause_sources_ = static_cast<std::uint8_t>(pause_sources_ | bit);
}

void engine::release_pause(pause_source s) noexcept {
    const std::uint8_t bit = static_cast<std::uint8_t>(s);
    if ((bit & static_cast<std::uint8_t>(~pause_source_all)) != 0) {
        log::warn("engine", "release_pause() 收到未知的暂停源位（pause_source_all 可能忘了同步）");
        return;
    }
    pause_sources_ = static_cast<std::uint8_t>(pause_sources_ & ~bit);
}

void engine::request_exit() noexcept {
    if (impl_ == nullptr) {
        return;
    }
 // 【标记即可】它不需要"应用"到活状态 —— 退出不是一种状态，是一个事件。
 //
 // 【它的生效时机与状态变更相同】帧末。理由：那样"这一帧"就永远是完整的
 // 一帧（它照常渲染、照常呈现），而不是"走到一半就断了"。
 //
 // 【为什么它不在这里直接进关闭序列】
 // 因为关闭序列是 shutdown_sequence()，而它需要引擎已经停下来。
 // 在帧中途直接进关闭序列会打断帧结构的有序关系。
    exit_requested_ = true;
}

bool engine::exit_requested() const noexcept {
    return exit_requested_;
}

void engine::request_frame_loop() noexcept {
 // 【它只置一个标志】见 engine.hpp 那段说明：循环仍然归 main，
 // 脚本侧只表达"我装载完了、可以开始跑"这个意图。
    frame_loop_requested_ = true;
}

bool engine::frame_loop_requested() const noexcept {
    return frame_loop_requested_;
}

void engine::request_target_fps(double fps) noexcept {
    if (impl_ == nullptr) {
        return;
    }
 // 【不立即生效】与 pause / resume / exit 相同：帧末应用，从下一帧起可见。
 //
 // 【为什么也必须延后 —— 这一条容易被当成多余的谨慎】
 // 帧率控制器的"睡觉"判据是"目标帧率的值与上一帧不同"。若本函数立刻改
 // 当前值，那么控制器在阶段 2 读到的可能是本帧中途改过的值 ⇒
 // "这一帧的节拍"会有两个来源。
 // 延后之后，"本帧一个目标帧率"与"本帧一个变换"是同一个手法。
 //
 // 【为什么不做"非法值就忽略"的兜底】
 // "<= 0" 属于数据矛盾，而它的处理位置是交界处（script/primitives.cpp），
 // 那里会报错并终止。本函数是引擎内部的写入点，它假定调用方已经校验过。
 // 在内部再校验一遍会让"谁负责校验"变成两个答案。
    pending_target_fps_ = fps;
}

double engine::get_target_fps() const noexcept {
    return target_fps_;
}

// =============================================================================
// 窗口尺寸的请求（命令式）
// =============================================================================

void engine::request_window_size(double w, double h) noexcept {
    if (impl_ == nullptr) {
        return;
    }
 // 【只写 pending，不动平台】应用由引擎在帧末统一做（apply_pending_window_size）。
 //
 // 【为什么不在这里判"与当前相同就跳过"】
 // 因为"当前"有两个候选（上一次请求 / 窗口现在的实际尺寸），而它们语义
 // 不同：
 // · 与上一次**请求**相同 ⇒ 幂等，应当跳过（命令式）
 // · 与窗口**实际**尺寸相同 ⇒ 若外部改过窗口，这就是"要纠正回去"
 // —— 那是**状态式**的语义，已定不用
 // ⇒ 判断放在 apply 里，对象是"上一次申请的值"。
 //
 // 【为什么不做合法性判断】
 // "<= 0" 属于数据矛盾，处理位置是交界处（script/primitives.cpp）。
 // 在内部再校验一遍会让"谁负责校验"变成两个答案。
    has_pending_window_size_ = true;
    pending_window_w_ = w;
    pending_window_h_ = h;
}

// =============================================================================
// 全屏模式的请求（命令式）
// =============================================================================

void engine::request_fullscreen(platform::fullscreen_mode mode) noexcept {
    if (impl_ == nullptr) {
        return;
    }
    if (!has_pending_fullscreen_ && impl_->win.get_fullscreen() == mode) {
        return;
    }
    has_pending_fullscreen_ = true;
    pending_fullscreen_ = mode;
    // 切换期间按住暂停；apply 完成后释放。
    hold_pause(pause_source::fullscreen);
}

bool engine::has_pending_fullscreen() const noexcept {
    return has_pending_fullscreen_;
}

platform::fullscreen_mode engine::current_fullscreen() const noexcept {
    if (impl_ == nullptr) {
        return platform::fullscreen_mode::none;
    }
    return impl_->win.get_fullscreen();
}

void engine::apply_pending_fullscreen() noexcept {
    if (!has_pending_fullscreen_) {
        return;
    }

    const platform::fullscreen_mode mode = pending_fullscreen_;
    has_pending_fullscreen_ = false;

    const bool accepted = impl_->win.set_fullscreen(mode);
    if (!accepted && mode != platform::fullscreen_mode::none) {
        // 独占/无边框失败：回退窗口模式 + L2，游戏继续。
        impl_->win.set_fullscreen(platform::fullscreen_mode::none);
        log::warn("engine", "全屏切换失败，回退窗口模式");
    } else if (!accepted) {
        log::warn("engine", "退出全屏失败，保持当前窗口状态");
    }

    release_pause(pause_source::fullscreen);
}

bool engine::has_requested_window_size() const noexcept {
    return has_applied_window_size_;
}

double engine::requested_window_w() const noexcept {
    return applied_window_w_;
}

double engine::requested_window_h() const noexcept {
    return applied_window_h_;
}

void engine::apply_pending_window_size() noexcept {
    if (!has_pending_window_size_) {
        return;
    }

    const double w = pending_window_w_;
    const double h = pending_window_h_;
    has_pending_window_size_ = false;

 // ---- 幂等去重：与"上一次申请的值"比较 ----
 //
 // 【为什么不是与"窗口现在的尺寸"比较】见 request_window_size 的说明：
 // 那会变成状态式（外部改过窗口就会被纠正回来）。
    if (has_applied_window_size_ && w == applied_window_w_ && h == applied_window_h_) {
        return;
    }

    const bool accepted = impl_->win.set_size(w, h);

 // 无论接受与否都记下来：命令式的语义是"请求一次、申请一次"，
 // 同值重复请求不该再打扰平台。
    has_applied_window_size_ = true;
    applied_window_w_ = w;
    applied_window_h_ = h;

    if (!accepted) {
 // ---- L2：请求未被平台满足 ----
 //
 // 【为什么是 L2 而不是 L3】它是一个**可降级**的失败：画面照跑，
 // 只是尺寸不是请求的那个（内容靠信箱兜底）。
 //
 // 【为什么不在这里"吸附到最近的可用尺寸"】已定：引擎不替调用方改值。
 // 那是决策，不是执行。
        char buf[128];
        std::snprintf(buf, sizeof(buf), "请求 %.0fx%.0f", w, h);
        log::warn("engine", log::format("窗口尺寸请求未被平台满足", buf));
    }
}

// =============================================================================
// 部件访问
// =============================================================================

platform::window& engine::window() noexcept {
    return impl_->win;
}

draw::renderer& engine::renderer() noexcept {
    return impl_->rend;
}

script::state& engine::script() noexcept {
    return impl_->sc;
}

space::viewport& engine::viewport() noexcept {
    return impl_->vp;
}

rng::registry& engine::rng() noexcept {
    return impl_->rngs;
}

container::container_pool_set& engine::containers() noexcept {
    return impl_->pools;
}

input::input& engine::input_system() noexcept {
    return impl_->inp;
}

std::vector<collision_request>& engine::collision_requests() noexcept {
    return impl_->col_requests;
}

const collision_stats& engine::collision_stats_ref() const noexcept {
    return impl_->col_stats;
}

collision_stats& engine::collision_stats_ref() noexcept {
    return impl_->col_stats;
}

container::collision_world& engine::collision_world_ref() {
    if (!impl_->collide) {
        impl_->collide = std::make_unique<container::collision_world>(impl_->pools, 32.0f);
    }
    return *impl_->collide;
}

// =============================================================================
// 绘制相位门（2026，子目标 2：绘序统一管线）
// =============================================================================

bool engine::draws_allowed() const noexcept {
    return draws_allowed_;
}

render_stage engine::current_render_stage() const noexcept {
    return render_stage_;
}

void engine::begin_reload_gate() noexcept {
 // 重载事务期间旧 Lua 不得再跑钩子/绘制/control。
 // 同时关绘制相位门；解除闸门后由下一帧的阶段 7 重新打开。
    reloading_ = true;
    set_draw_phase(false, render_stage::none);
}

void engine::end_reload_gate() noexcept {
    reloading_ = false;
}

engine::preflight_outcome engine::preflight_reload_scripts(
    const std::string& install_root, const std::string& session) {
    if (!reload_status_ || reload_status_root_ != install_root ||
        reload_status_session_ != session) {
        reload_status_.emplace(install_root, session);
        reload_status_root_ = install_root;
        reload_status_session_ = session;
    }

    reload_status_->begin("scripts");

    auto result = script::preflight_scripts(script(), install_root);
    const bool ok = result.ok;
    const std::size_t checked = result.checked;
    const std::size_t error_count = result.errors.size();

    std::vector<reload::issue> issues;
    issues.reserve(result.errors.size());
    for (const auto& e : result.errors) {
        issues.push_back(reload::issue{e.path, e.message});
    }

    reload_status_->set_progress(checked, checked);
    reload_status_->set_errors(std::move(issues), error_count);
    reload_status_->set_phase(
        ok ? reload::phase::ready : reload::phase::rejected,
        ok ? "预检通过，允许重载" : "预检失败，旧状态未改动", true);
    const bool written = reload_status_->write();

    last_preflight_ = std::move(result);
    return preflight_outcome{ok, written, checked, error_count};
}

const script::preflight_result* engine::last_preflight() const noexcept {
    return last_preflight_ ? &*last_preflight_ : nullptr;
}

void engine::configure_reload(std::string install_root, std::string session) {
    reload_install_root_ = std::move(install_root);
    reload_session_ = std::move(session);
}

bool engine::commit_reload_scripts() {
    reload_commit_pending_ = false;

    if (!last_preflight_ || !last_preflight_->ok) {
        return false;
    }
    if (reload_install_root_.empty()) {
        return false;
    }

    const script::reload_source* bootstrap = nullptr;
    for (const auto& s : last_preflight_->sources) {
        if (s.path == "WagLib/init.lua") {
            bootstrap = &s;
            break;
        }
    }
    if (bootstrap == nullptr) {
        log::warn("reload", "预检缓存缺少 WagLib/init.lua，拒绝提交");
        return false;
    }

    if (!reload_status_ || reload_status_root_ != reload_install_root_ ||
        reload_status_session_ != reload_session_) {
        reload_status_.emplace(reload_install_root_, reload_session_);
        reload_status_root_ = reload_install_root_;
        reload_status_session_ = reload_session_;
    }

 // 旧 Lua 停止一切钩子/绘制/control。
    begin_reload_gate();
    reload_status_->set_phase(reload::phase::committing,
                              "提交中", false);
    static_cast<void>(reload_status_->write());

 // 只保留玩家自己的暂停源；focus/drag 由当前窗口状态下一帧重算。
    const std::uint8_t saved_user_pause =
        static_cast<std::uint8_t>(pause_sources_ &
                                  static_cast<std::uint8_t>(pause_source::user));

 // 先清容器，再关状态。
    containers().clear_all();
    containers().reset_classes();

 // 清旧边沿/事件，保留物理按住与鼠标位置。
    input_system().prepare_reload();

 // 停 SE；BGM 专用槽保留。
    audio().stop_all_se();

 // RNG 与绘制粘性状态重置。
    rng().reset();
    renderer().reset_for_reload();

 // 关旧状态。destroy() 负责 unref 分派器并把 Lua 引用一起释放。
    script().destroy();

    if (!initialize_script_runtime(*this)) {
        reload_status_->set_phase(reload::phase::runtime_failed,
                                  "新 Lua 状态建立失败，请重启引擎", false);
        static_cast<void>(reload_status_->write());
        end_reload_gate();
        exit_requested_ = true;
        log::warn("reload", "提交失败：新 Lua 状态建立失败");
        return false;
    }

 // 新状态需要与首次启动相同的安装根与 package.path。
    script::install_cached_modules(script(), last_preflight_->sources);
    script::publish_install_root(script(),
                                 reload_install_root_);
    script::prepend_package_path(
        script(), reload_install_root_ + "/Luminosa");

    if (!script::run_script_source(
            script(), bootstrap->source,
            "@Luminosa/WagLib/init.lua")) {
        reload_status_->set_phase(reload::phase::runtime_failed,
                                  "新状态引导脚本执行失败，请重启引擎", false);
        static_cast<void>(reload_status_->write());
        end_reload_gate();
        exit_requested_ = true;
        log::warn("reload", "提交失败：新状态引导脚本执行失败");
        return false;
    }

 // 恢复玩家暂停源；新状态在 on_game_init 里显式设置的值优先。
    pause_sources_ = saved_user_pause;
    state_ = (pause_sources_ != 0) ? engine_state::paused
                                   : engine_state::normal;

    end_reload_gate();

    reload_status_->set_phase(reload::phase::committed,
                              "已提交；运行时错误见错误面板", false);
    static_cast<void>(reload_status_->write());
    log::record("reload", "reload scripts 已提交");
    return true;
}

bool engine::handle_reload_command(std::string_view verb, std::string_view arg) {
    if (verb != "reload") {
        return false;
    }

    if (reload_install_root_.empty()) {
        log::warn("reload", "收到 reload 命令，但热重载入口未配置安装根");
        return true;
    }

    if (arg != "scripts") {
        if (!reload_status_ || reload_status_root_ != reload_install_root_ ||
            reload_status_session_ != reload_session_) {
            reload_status_.emplace(reload_install_root_, reload_session_);
            reload_status_root_ = reload_install_root_;
            reload_status_session_ = reload_session_;
        }
        reload_status_->begin("scripts");
        reload_status_->set_errors(
            {reload::issue{"scope", "v1 只支持 reload scripts"}}, 1);
        reload_status_->set_phase(reload::phase::rejected,
                                  "不支持的 reload scope", true);
        static_cast<void>(reload_status_->write());
        log::warn("reload", log::format("拒绝 reload scope", std::string(arg)));
        return true;
    }

    const auto r = preflight_reload_scripts(reload_install_root_, reload_session_);
    reload_commit_pending_ = r.ok;
    log::record("reload", log::format(
        "reload scripts 预检", std::string(r.ok ? "通过" : "失败") +
            " checked=" + std::to_string(r.checked) +
            " errors=" + std::to_string(r.error_count)));
    return true;
}

void engine::poll_reload_commands() {
    control::command c;
    while (control_.take_if(
        [](const control::command& cmd) { return cmd.verb == "reload"; }, c)) {
        static_cast<void>(handle_reload_command(c.verb, c.arg));
    }
}

void engine::set_draw_phase(bool allowed, render_stage stage) noexcept {
 // 【一个写入点】开/关与阶段切换绑成一次调用，帧结构在阶段边界各调一次
 // （阶段 7 开 scene；阶段 9 的 Ext 前切 ext；阶段 9 结束关闸）。
 // 不允许"门开着但阶段是 none"的组合出现 —— 那是调用方的笔误。
    draws_allowed_ = allowed;
    render_stage_ = stage;
}

// =============================================================================
// 帧末的状态应用
// =============================================================================

bool engine::apply_pending_state() noexcept {
 // 【本函数不做任何 GPU 操作】只做字段赋值。
 //
 // 这一点是重要的：早期设计里这里有"拍一张快照"的 GPU 操作，于是它可能
 // 失败，于是"冻结画面与状态切换是同一个原子事件"这条就不成立了。
 // 现在快照在阶段 10 就已经拍好（暂存），这里只是一次赋值。
 //
 // 【快照的"取用"在哪里】见 frame.cpp 的 stage_state_apply：那里在
 // 本函数返回"发生过切换"之后，把 staged 换成 frozen（一次指针交换）。

 // ---- 先应用目标帧率的请求（它独立于状态切换）----
 //
 // 【为什么它与状态切换在同一个阶段应用】
 // 因为两者的性质相同：都是"脚本在这一帧请求的变更"，都遵循
 // "帧末应用、下一帧生效"。把它们放在同一个位置，那条规则就只需要
 // 被实现一次。
 //
 // 【为什么它不能与状态混成一个 optional】
 // 因为"改帧率"与"改状态"是两件独立的事，它们可以同时被请求，
 // 也可以只请求其中一件。用一个可选项表达两件事会让"只改帧率"变成
 // "顺便把状态也设成某个值"。
    if (pending_target_fps_ > 0.0) {
        target_fps_ = pending_target_fps_;
        pending_target_fps_ = 0.0;
    }

 // ---- 暂停：**从源派生**，不是"应用一个待写的值" ----
 //
 // 【为什么这里不是 has_pending_ 的门控】
 // 旧形态是"有请求才改状态"，于是"请求"与"源"是一个东西。
 // 现在是多源：每一帧都要重新求值一次，因为**源可以在任何时候被按住/松开**
 // （失焦发生在消息循环里，拖拽发生在 WndProc 里，脚本请求发生在帧内）。
 // ⇒ 派生比"覆盖写"多算一次比较，但换来"不可能两个源互相覆盖"。
    const engine_state before = state_;
    state_ = (pause_sources_ != 0) ? engine_state::paused : engine_state::normal;

 // 【为什么返回"是否发生了切换"而不是新状态】
 // 因为调用方只关心"要不要取用冻结画面"。返回新状态会诱使调用方开始
 // 判断"现在是哪一支"，而那时帧结构已经明确要求：本帧用 frame_mode,
 // 不用活状态。
    const bool changed = (before != state_);

    if (changed) {
 // 【记录一次状态切换】这不是"日志噪音"，它是排查"某一帧为什么不对"
 // 时最有用的两条线索之一（另一条是帧号）。
 // ⚠️【必须带来源】只打 "paused" 会让人无法回答"是谁按的" ——
 // 而这个模型里"谁按的"正是唯一重要的信息（多源）。
        log::record("engine", log::format("状态切换", std::string(state_name(state_)) +
                                               "（来源: " +
                                               pause_sources_name(pause_sources_) +
                                               "）"));
    }

    return changed;
}

// =============================================================================
// 脚本运行时的建立（启动与热重载共用）
// =============================================================================

namespace {

/// 建立 Lua 状态 + 钩子槽 + 原语 + 封堵 + 自检。
/// 【调用者】boot_sequence（首次）与热重载事务（新状态）。
/// 【不跑引导脚本】引导脚本由调用者决定何时执行。
bool initialize_script_runtime(engine& eng) {
    if (!eng.script().create()) {
        return false;
    }

 // 步骤 3：预置 13 个钩子槽。
    script::install_hook_noops(eng.script());

 // 步骤 3b：注册引擎原语。
    script::register_primitives(eng.script(), eng);

 // 步骤 4：环境封堵。
    script::seal_environment(eng.script());

 // 步骤 5：启动期自检。
    return script::verify_hooks_are_functions(eng.script());
}

} // namespace

// =============================================================================
// 启动序列
// =============================================================================

void boot_sequence(engine& eng) {
#if defined(WAG_HAVE_SDL3)
 // 步骤 -1：平台子系统初始化。它要先于窗口创建 —— 而且它初始化的是视频
 // 之外的子系统（定时器、事件），那些不属于窗口。
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        log::terminate("engine", log::format("平台子系统初始化失败"));
    }
    log::record("boot", "平台子系统已初始化");

 // 步骤 -0.5：音频子系统（设备 + 混音器）。
 // 【为什么在窗口之前】它不依赖窗口；而失败时只静音，不影响后续任何步骤。
 // 【失败语义】设备打不开（CI / 无声卡）⇒ 静音模式 + 一条 L2，**不终止**。
    eng.audio().init();
#endif

 // 步骤 -1.5：随机主种子（④ 已定：性能计数器 ⊕ time() ⊕ 进程独有量）。
 //
 // 【为什么必须在脚本之前】主种子是「这一局」的身份；脚本一装载就可能要
 // 用它建流（wag.rng.new_stream）。晚于脚本 ⇒ 脚本拿到的是 0。
 // 【为什么混一个进程独有量】同一秒启动两个实例时，性能计数器与 time()
 // 都可能撞上 ⇒ 混一个栈地址（受 ASLR 影响）就能把两个进程分开。
 // 它**不是**安全用途，只用于避免同种子。
 // 【采一次就存住】绝不能在每次 master_seed() 时重采 ——
 // 那样「一局」就没有唯一的主种子了。
    {
        std::uint64_t entropy = static_cast<std::uint64_t>(std::time(nullptr));
#if defined(WAG_HAVE_SDL3)
        entropy ^= static_cast<std::uint64_t>(SDL_GetPerformanceCounter());
#endif
        int local_marker = 0;
        entropy ^= static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(&local_marker));
        eng.rng().set_master_seed(entropy);

 // 日志里记 lo/hi：它是「这一局」的可复现标识 ——
 // 复现某个 bug 时，只要这一个数就能重建整局随机。
        const std::uint32_t lo = static_cast<std::uint32_t>(entropy & 0xFFFFFFFFull);
        const std::uint32_t hi = static_cast<std::uint32_t>(entropy >> 32);
        char detail[64];
        static_cast<void>(std::snprintf(detail, sizeof(detail),
                                        "lo=0x%08X hi=0x%08X", lo, hi));
        log::record("boot", "随机主种子已采集", detail);
    }

 // 步骤 0：创建窗口。
 //
 // 【待定】默认尺寸。1920×1080 与默认视口一致；它应当来自配置体系。
    constexpr double kDefaultWindowW = 1920.0;
    constexpr double kDefaultWindowH = 1080.0;

    if (!eng.window().create(kDefaultWindowW, kDefaultWindowH, "LuaWAG")) {
 // ⚠️【这里的报错文案要分档，否则它会自相矛盾】
 // 无依赖档（`WAG_NO_EXTERNAL_DEPS=ON`，默认）下 `window::create()` 走空实现、
 // **必然返回 false**（它的注释写着"未接线"）⇒ 于是本档**永远跑不到帧循环**。
 // ⇒ 若这里只说"窗口创建失败"，下一个人会去查引擎代码，
 // 而真实原因是"**这一档没接 SDL3，它只用来验语法**"。
 // ⇒ 所以两种构建给两种文案（见下面 `#if`）。
#if defined(WAG_HAVE_SDL3)
        log::terminate("engine", log::format("窗口创建失败"));
#else
 // ⚠️ 文案里**不要**用半角引号做引述 —— 它会与外层的字符串定界撞上，
 // 于是 `能否编译` 会掉到字符串外面去（这一处我第一版真写错了一次）。
 // ⇒ 引述一律用「」。
        log::terminate("engine", log::format(
            "窗口创建失败 —— 本可执行未接 SDL3（WAG_NO_EXTERNAL_DEPS=ON）。"
            "它只用于验证能否编译，不可运行；"
            "要跑引擎请用 cmake --preset debug-with-deps 构建的产物"));
#endif
    }
#if defined(WAG_HAVE_SDL3)
    log::record("boot", "窗口已创建");
#else
 // 日志必须与上面的 #if 分支同档：没接 SDL3 时不要宣称窗口已创建。
    log::record("boot", "窗口创建流程结束（未接 SDL3）");
#endif

 // 步骤 0b：把窗口的事件出口接到输入状态机。
 //
 // 【为什么在窗口创建之后、图形上下文交接之前】
 // 窗口此刻已经能泵事件；而输入不依赖图形。
 // 【它同时做鼠标初始位置的播种】见 window::set_input_sink 的说明：
 // 有焦点则读一次 SDL_GetMouseState，否则喂 (0,0)。
 // 【接线是唯一允许写 input 的通道】消息路径只写数据（不调脚本）；
 // 阶段 4 的 sample() 是唯一"冻结快照"的地方。
    eng.window().set_input_sink(&eng.input_system());
    log::record("boot", "输入事件已接线（窗口 → input）");

 // 步骤 1：从窗口取出图形上下文，交给渲染器独占。
    {
        auto ctx = eng.window().take_gl_context();
        if (!ctx.has_value() || !eng.renderer().adopt_context(*ctx)) {
            log::terminate("engine", log::format("图形上下文交接失败"));
        }
    }
    log::record("boot", "渲染器已接收图形上下文");

 // 步骤 1b：安装窗口暴露观察者（"白条"补丁，见文件头的说明）。
 //
 // 【为什么在这一步】它同时需要窗口（呈现）与渲染器（清屏）都已就位；
 // 而它必须在帧循环之前装好 —— 拖拽可能发生在任何时刻。
#if defined(WAG_HAVE_SDL3)
    install_expose_watch(eng);
 // 步骤 1c：安装**拖拽状态**钩子（2026-11-14 正式启用；下面的探针已完成使命）。
 // 它只记"在不在拖拽"；补跑帧由暴露观察者发起。
    install_sizemove_hook(eng);
#if defined(SDL_PLATFORM_WINDOWS)
 // 【探针】消息钩子装在 SDL 的 WndProc 内部、原始消息处理之前 ——
 // 已完成使命：确认能看到 WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE。
 // ⇒ 默认关闭（kWindowsMessageProbe = false），要复测时改它。
 // ⚠️ 真钩子（install_sizemove_hook）也是用同一个 API 装的；
 // 两者不要同时开 —— SDL 只有一个消息钩子槽位，后装的会覆盖先装的。
    if (kWindowsMessageProbe) {
        SDL_SetWindowsMessageHook(&message_probe_hook, nullptr);
        log::record("boot", "消息钩子探针已安装（诊断·MSG）");
    }
#endif
#endif

 // 步骤 2~5：建立 Lua 状态、钩子槽、原语、封堵、自检。
 // 【与热重载共用】initialize_script_runtime 是唯一装配点。
    if (!initialize_script_runtime(eng)) {
        log::terminate("engine", log::format("脚本状态建立失败"));
    }
    log::record("boot", "脚本状态已建立");

 // 步骤 6~8（读引导脚本 / 装配映射层 / 按层装载）**由 Lua 侧完成**：
 // `WagLib/init.lua` 的步骤 0–5.7。本函数只把基础设施备好 ——
 // "有哪些层、怎么装"属于工程词汇，引擎不该知道。
 //
 // 步骤 9：进入帧循环，由引导脚本显式请求。入口在 wag.boot.run。

    log::record("boot", "基础设施就位，等待引导脚本请求进入帧循环");
}

// =============================================================================
// 关闭序列
// =============================================================================

void shutdown_sequence(engine& eng) {
 // 【已定的部分（三条）】
 // ① on_game_exit 必须在**任何东西被拆之前**
 // ② 关闭序列里的错误只记日志（不弹窗、不"带错继续"）
 // ③ on_game_exit **不保证被调用**
 //
 // 【已定：7 步语义】
 // 1. on_game_exit
 // 2. 关闭 Lua 状态（lua_close 会触发 __gc）
 // 3. 第 2 步已包含 finalizer 清理，不再单独跑 full GC
 // 4. 释放引擎资源
 // 5. 关闭子系统（渲染 → 音频 → 输入 → 窗口）
 // 6. 关闭文件系统
 // 7. 退出进程
 //
 // 第 2 步的理由：__gc 可能调用引擎 API，而它在不确定的时刻触发。
 // 若它在引擎资源已经释放之后触发，就会访问已销毁的引擎对象。
 // 所以必须先关闭 Lua 状态，再关闭字体/音频/SDL/日志。
 //
 // 【脚本侧对象的所有权】池、节点、场域由 C++ 持有并随引擎析构；
 // 不存在额外的“脚本侧释放”步骤。Lua 的表/句柄只需随 lua_close 失效。

 // ---- 第 1 步：on_game_exit ----
 //
 // 【为什么它必须在最前】它要做的事（存状态、存配置、写日志、收尾表现）
 // 全部要求引擎仍然完整。把它放在任何资源被释放之前，是唯一能保证
 // 这一点的位置。
 //
 // 【实参是空表】此时没有“本帧”。
 // 【出错只记 L1】由 call_hook_bare 保证：不弹窗、不写 warn 行、继续关。
 // 【幂等】L4 终止前也会调一次 —— 两条路径不会重复调用（call_exit_hook_once）。
    log::record("engine", "关闭序列开始");
    {
        const bool called = script::call_exit_hook_once(eng.script());
        if (called) {
            log::record("engine", "关闭序列：on_game_exit 已调用");
        } else {
            log::record("engine", "关闭序列：on_game_exit 之前已调用（跳过）");
        }
 // 【注意】本调用不保证发生（强杀、崩溃时没有机会），因此它不能被
 // 当作存档的唯一机会。
    }

 // ---- 第 2 步：关闭 Lua 状态 ----
 // 【顺序不能反】lua_close 会触发 __gc；必须在 fonts/audio/SDL/log 之前。
 // 【谁负责释放】容器池、节点、场域由 C++ 持有；这里只关闭 Lua 状态。
 // 【错误策略】退出阶段回不到脚本；__gc 的错误不应阻断关闭流程。
 // 【幂等】destroy() 在 L_ == nullptr 时直接返回，~engine 仍可作为兜底。
    eng.script().destroy();
    log::record("engine", "关闭序列：Lua 状态已关闭");

 // ---- 第 3 步：finalizer 清理已并入第 2 步 ----
 // 不再单独跑一次 full GC；lua_close 本身会处理剩余 finalizer。
 // 若以后需要逐条回执 __gc 错误，再另做受保护的 finalizer 批处理。

 // ---- 第 4 步：释放引擎资源 ----
 // 【字体（2026-11-18）】先于子系统关闭：它要销毁图集页（GL 纹理），
 // 而那是"渲染资源"—— 必须在上文销毁之前做完。
 // 本调用只清表 + 把页纹理推入帧末销毁队列；上下文随后一起消失，不漏。
    eng.fonts().shutdown();
    log::record("engine", "关闭序列：字体表已关闭");
 // 【仍待补】其它纹理、缓冲、着色器（图片族的释放与上下文同生共死）。

 // ---- 第 5 步：关闭子系统（逆着初始化顺序关）----
 // 渲染 → 音频 → 输入 → 窗口
 //
 // 【顺序的理由】渲染依赖窗口，所以它必须先于窗口关闭。
 // 而音频/输入不依赖渲染，所以它们的位置相对自由。
 //
 // 【音频线程的约束】它必须在引擎资源释放之前被停掉并 join。
 // 音频线程必须在引擎资源释放之前停掉。

 // 先断开输入接线：窗口此后不能再往 input 写（sink 生命周期）。
 // 【为什么放在 SDL_Quit 之前】SDL_Quit 不会替我们清这个裸指针；
 // 若之后还有事件泵（不应有），它会写进一个即将销毁的对象。
    eng.window().set_input_sink(nullptr);
    log::record("engine", "关闭序列：输入接线已断开");

#if defined(WAG_HAVE_SDL3)
 // 先卸掉暴露观察者，再关平台子系统。
 // 【为什么显式卸】SDL_Quit 也会清掉全部观察者，但"谁装的谁卸"让
 // 配对关系写在代码里，而不是藏在 SDL 的清理行为里。
    remove_expose_watch(eng);
#if defined(SDL_PLATFORM_WINDOWS)
    SDL_SetWindowsMessageHook(nullptr, nullptr);
#endif

 // 平台子系统关闭。放在日志关闭之前 —— 它之后可能还有收尾动作要记录。
 // 【音频必须在 SDL_Quit 之前】SDL_Quit 会拆掉音频子系统；
 // 而我们的流必须先被显式销毁：先停线程再释放资源。
    eng.audio().shutdown();

    SDL_Quit();
    log::record("engine", "关闭序列：SDL 已关闭");
#endif

 // ---- 第 6 步：关闭文件系统（含日志） ----
 // 【为什么不在本函数里关日志】D8：日志是**整个进程最后一个写文件的人**。
 // main 还要在 shutdown_sequence 返回后记一条“已退出”，因此日志关闭
 // 由 main 在最后一步执行。

 // ---- 第 7 步：退出进程 ----
 // 【它由 main 负责】本函数只负责"把引擎拆干净"。
 // 把"退出进程"留给调用方，是为了让本函数可以被测试（测试里不该退出进程）。
    (void)eng;
}

} // namespace wag
