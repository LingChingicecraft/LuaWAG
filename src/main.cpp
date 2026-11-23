// =============================================================================
//  main.cpp
//
//  装配。
//
//  【它在内核里的位置】
//      它是**唯一**知道"整个引擎由哪些部件组成、按什么次序装起来"的地方。
//      它不做任何具体的事 —— 它只按启动序列调用那些做事的人。
//
//  【它为什么被刻意写得很薄】
//      因为"引擎的逻辑"不应住在 main 里。main 里每多一行逻辑，就多一行
//      无法被单测、也无法被复用的代码。本文件里全部的实质内容都是
//      "顺序"与"失败处理"。
//
//  【本文件里唯一一处"看起来像逻辑"的东西是安装根的定位】
//      而它其实属于目录解析（引导层），只是 C++ 侧必须知道"从哪里开始找"。
//      见下面的 constexpr 与注释。
// =============================================================================

#include "diagnostics/diagnostics.hpp"
#include "engine/engine.hpp"
#include "error/error.hpp"
#include "log/log.hpp"
#include "platform/window.hpp"
#include "platform/paths.hpp"
#include "platform/system_info.hpp"
#include "script/state.hpp"
#include "script/hook.hpp"
#include "time/frame.hpp"

#include <cstring>
#include <string>

// 【为什么本文件不包含任何 Lua 头】
//   main 是**装配层**：它只负责"按启动序列调用那些做事的人"。
//   而"设 package.path"与"读引导脚本"都是"与 Lua 相接"的动作 ⇒ 它们住在
//   `wag::script`（那一层的职责就是“与 Lua 相接的一切”）。
//
//   【这一条是有代价换来的】原先这两件事写在 main 里，后果是：
//     ① main 必须直接 include lua.h（装配层知道了具体技术）
//     ② main 必须能看到 `WAG_HAVE_LUAJIT` 这个编译期宏 —— 而它由 `wag_luajit`
//        这个 INTERFACE 目标携带，当时只有 wag_script 链了它
//        ⇒ main 走了 `#else` 降级分支 ⇒ **package.path 静默地没被设置**
//        ⇒ 引导脚本第一行 require 就失败（而报出来的是"读不到引导脚本"）

// 【命名空间】main.cpp 在全局作用域，因此一律写完整限定名（`wag::` / `wag::log::` /
//   `wag::time::`）。原先的 `namespace core = wag::core;` 别名随 `wag::core` 一起拆除
//   （变更记录见 log/log.hpp 文件头）。

namespace {

/// 供"L4 终止前调 on_game_exit"使用的引擎指针（在进入帧循环前设好）。
wag::engine* g_engine_for_exit = nullptr;

/// 终止前回调：调一次 on_game_exit（内部幂等，正常关闭已调过则什么都不做）。
void call_exit_hook_before_terminate() noexcept {
    if (g_engine_for_exit != nullptr) {
        wag::script::call_exit_hook_once(g_engine_for_exit->script());
    }
}

// -----------------------------------------------------------------------------
//  安装根与 Luminosa 目录
//
//  【已定的定位规则】
//      · 安装根：**先按"exe 所在目录"**，并允许被命令行覆盖
//      · Luminosa：**固定路径** `<安装根>/Luminosa/`
//
//  【为什么这两条在 C++ 侧也出现一次，而不是只写在引导脚本里】
//      因为 C++ 侧必须知道"从哪里开始找引导脚本" —— 那是它的第一个动作。
//      而**目录之下的解析**（四个层、模块名 → 路径）全部在脚本侧（resolve.lua），
//      C++ 侧不参与。
//
//  【exe 所在目录的实现状态】
//      Windows 上已经实现：GetModuleFileNameW + 宽字符转 UTF-8；
//      非 Windows 分支当前返回空串，路径超过 MAX_PATH 时也返回空串。
//      平台无关性方案尚未收口；Win32 细节关在 platform::window 内部。
constexpr const char* kLuminosaDirName = "Luminosa";

// 【注意：这里刻意不再有"引导脚本的相对路径"那个常量】
//   因为路径必须由**安装根**拼出来，而安装根可能来自命令行覆盖。
//   留一个写死的相对路径就会成为"第二个来源"，与拼出来的那个分歧。

// -----------------------------------------------------------------------------
//  命令行
//
//  【当前支持的参数】
//      --non-interactive
//      --diagnostics
//      --no-focus-pause
//      --no-log
//      --control=<dir>
//      --root=<path>
//      --root <path>
//      --start-node=<id>
//      未知参数忽略，不报错。
//
//  【这份集合还不是最终协议】新参数可以继续加，但已有名字不要随意改。
// -----------------------------------------------------------------------------
struct command_line {
    /// 被覆盖的安装根；为空表示用默认（exe 所在目录）。
    const char* install_root = nullptr;

    /// 是否要求非交互模式（不弹窗）。
    ///
    /// 【它为什么现在就存在】
    ///   因为弹窗是模态的，而模态弹窗在无人值守环境里会挂住进程。
    ///   没有这一档，内核就无法自测 —— 这是它必须现在就在的原因。
    bool non_interactive = false;

    /// 本次运行是否开启**诊断导出**（`--diagnostics`）。
    ///
    /// 【它不是"开发模式"】名字刻意避开 "dev"：
    ///   它**只改输出，不改行为**。判据：把 diagnostics/ 整段删掉重编，
    ///   发布版行为必须逐字节相同。叫 "dev" 会暗示行为分叉。
    bool diagnostics = false;

    /// 失焦是否**不**作为暂停源（`--no-focus-pause`）。
    ///
    /// 【为什么它是一个启动期开关，而不是脚本能调的东西】
    ///   "失焦要不要暂停"是**世界法则级**的选择：同一份脚本在玩家机器（失焦该停）
    ///   与无人值守环境（窗口可能从来没有焦点，停了就永远不动）里必须行为一致。
    ///   ⇒ 它只能在启动时定一次，不能由游戏逻辑在运行中翻转。
    ///
    /// 【为什么现在就需要】没有它是**必挂**的：CI 机器上窗口可能从未获得焦点，
    ///   而 `frame.cpp` 每帧读 `has_focus()` ⇒ 永久暂停 ⇒ 帧数永远数不满，
    ///   `WAG_EXIT_AFTER` 之类的自测全部挂死。
    bool no_focus_pause = false;

    /// 控制通道的**控制台目录**（`--control=<dir>`）。空 ⇒ 通道关闭（默认）。
    ///
    /// 【为什么是“控制台目录”而不是引擎自己的 debug/】
    ///   两边各自只写自己的目录，读取对方的那一份。命令文件归控制台，
    ///   状态文件归引擎 —— 谁都不改对方的文件。
    const char* control_dir = nullptr;

    /// `--start-node=<id>`：**原样交给脚本**，引擎不解释。
    const char* start_node = nullptr;

    /// `--no-log`：不创建/不写两份普通日志文件（测试/CI 用）。
    /// 【为什么不是玩家开关】默认两份都写；这个开关只给自动化测试。
    bool no_log = false;
};

command_line parse_command_line(int argc, char** argv) {
    command_line out;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (a == nullptr) {
            continue;
        }
        if (std::strcmp(a, "--non-interactive") == 0) {
            out.non_interactive = true;
        } else if (std::strcmp(a, "--diagnostics") == 0) {
            out.diagnostics = true;
        } else if (std::strcmp(a, "--no-focus-pause") == 0) {
            out.no_focus_pause = true;
        } else if (std::strcmp(a, "--no-log") == 0) {
            out.no_log = true;
        } else if (std::strncmp(a, "--control=", 10) == 0) {
            // `--control=<控制台目录>`：空值等同于不给。
            out.control_dir = (a[10] != '\0') ? (a + 10) : nullptr;
        } else if (std::strncmp(a, "--root=", 7) == 0) {
            out.install_root = (a[7] != '\0') ? (a + 7) : nullptr;
        } else if (std::strncmp(a, "--start-node=", 13) == 0) {
            out.start_node = (a[13] != '\0') ? (a + 13) : nullptr;
        } else if (std::strcmp(a, "--root") == 0 && i + 1 < argc) {
            out.install_root = argv[++i];
        }
        // 未知参数忽略；新增参数时在这里追加分支。
    }
    return out;
}

// -----------------------------------------------------------------------------
//  引导脚本
// -----------------------------------------------------------------------------

/// 读引导脚本，并返回"脚本是否请求了进入帧循环"。
///
/// 【为什么不返回 bool 表示"成功"】
///   因为这里有两种失败与一种成功，而它们的处置不同（见 main 里的注释）：
///     · 取不到安装根  ⇒ 引擎必需件缺失
///     · init.lua 读失败 ⇒ 引擎必需件缺失
///     · 读完但没请求进入帧循环 ⇒ 脚本中途失败
///   ⇒ 所以调用方需要能区分"读完了"与"请求了"。
///
/// 【它只做判断，不做 Lua 调用】实际的读入在 `script::run_script_file` 里。
bool load_bootstrap_script(wag::engine& eng, const std::string& init_path) {
    if (!wag::script::run_script_file(eng.script(), init_path)) {
        // 【相对化】`got` 会进日志与 errors.jsonl ⇒ 不得带绝对路径。
        const std::string got_relative = wag::paths::relativize(init_path);
        wag::error_value e;
        e.cls = wag::error_class::resource_failure;
        e.where = "引导脚本";
        e.got = got_relative;
        e.want = "一个可读、且能跑完的 init.lua";
        e.sentence = "引擎必需件缺失：引导脚本没能跑完";
        // 【注意这句措辞】不能说"读不到" —— 失败可能是"文件不在"，
        //   也可能是"脚本里报错"。真实原因由 run_script_file 记的那条
        //   Lua 原文给出（在它上面几行）。
        wag::fail_boundary(e);
    }
    return eng.frame_loop_requested();
}

// -----------------------------------------------------------------------------
//  动态拖拽：拖拽期间的"补跑一帧"入口（2026-11-14）
//
//  【为什么需要它】拖拽时 Windows 在自己的模态循环里，`SDL_WaitEventTimeout`
//    会长时间阻塞 ⇒ 主循环发不出帧。唯一还在到的是窗口暴露/尺寸事件，
//    而它们足够密（拖拽期间每秒几十条）⇒ 拿来当节拍器。
//
//  【为什么必须是裸函数指针 + void*】它被装进 engine，跨过 engine/时间层的
//    依赖方向（time 依赖 engine，engine 不能反过来依赖 time）。内核不拖
//    std::function：那会引入一个看不见的堆分配与一层间接。
//
//  【三条门控，一条都不能少】
//    ① `window_sizemove()` —— 不在拖拽中就不补（普通暴露由主循环自己跑帧）；
//    ② `!in_frame()`      —— **重入守卫**：事件观察者是在泵里被调的，
//                            而泵本身可能在帧外，也可能不在；不看这一条就会栈递归；
//    ③ `frame_due()`      —— **到点才跑，且不睡**：消息密度不受我们控制，
//                            不看这一条就会按消息密度刷帧。
// -----------------------------------------------------------------------------
struct drag_tick_state {
    wag::engine* eng = nullptr;
    wag::time::frame_pipeline* pipeline = nullptr;
};

bool drag_tick(void* user) {
    auto* const state = static_cast<drag_tick_state*>(user);
    if (state == nullptr || state->eng == nullptr || state->pipeline == nullptr) {
        return false;
    }
    if (state->pipeline->in_frame()) {
        return false;
    }
    if (!state->pipeline->frame_due()) {
        return false;
    }
    state->pipeline->run_one_frame(*state->eng);
    return true;
}

} // namespace

// =============================================================================
//  入口
// =============================================================================

int main(int argc, char** argv) {
    const command_line cli = parse_command_line(argc, argv);

    // -------------------------------------------------------------------------
    //  安装根：必须先于日志
    //
    //  D7：先解析 exe 路径 / 安装根 / 命令行，再打开两份日志。
    //  安装根解析之前的极早期日志只走 stderr，不做内存缓冲。
    // -------------------------------------------------------------------------
    std::string install_root;
    if (cli.install_root != nullptr) {
        install_root = cli.install_root;
    } else {
        install_root = wag::platform::executable_dir();
    }

    // -------------------------------------------------------------------------
    //  最早的一步：日志
    //
    //  【为什么它必须先于一切】
    //    因为在那之后发生的每一件事（包括环境封堵失败、引导脚本读不到）
    //    都要靠它留下记录。而"启动失败"这件事若没有任何输出，是最难查的。
    //
    //  【两份日志】
    //    <安装根>/engine.log       隐私日志
    //    <安装根>/engine.full.log  完整日志
    //    安装根不可写时各自回退到 %LOCALAPPDATA%/LuaWAG/logs/。
    // -------------------------------------------------------------------------
    wag::log::init(install_root, cli.no_log,
                   cli.non_interactive ? wag::log::mode::non_interactive
                                       : wag::log::mode::interactive);

    // 【弹窗实现交给平台层】日志层不依赖 UI 库（见 log.hpp 的 dialog_handler）。
    //   真正弹不弹由日志层决定：只有 **L4** + 交互模式 + `WAG_SHOW_DIALOG=1`。
    wag::log::set_dialog_handler(&wag::platform::show_message_box);

    // 完整日志的启动硬件段（隐私日志不写）。
    {
        const wag::platform::system_summary sys =
            wag::platform::get_system_summary();
        std::string header;
        header += "系统信息：\n";
        header += "  OS：" + sys.os + "\n";
        header += "  CPU：" + sys.cpu + "\n";
        header += "  GPU：" + sys.gpu + "\n";
        header += "  内存：" + sys.memory;
        wag::log::write_full_header(header);
    }

    wag::log::record("main", "LuaWAG 启动");

    // -------------------------------------------------------------------------
    //  诊断导出开关
    //
    //  【为什么紧跟日志、先于一切动作】
    //    它是"本次运行的性质"，任何将来可能写导出的代码都要能读到它
    //    ⇒ 必须在启动序列之前定下来，且只写一次。
    //
    //  【它只影响输出，不影响行为】判据：把 diagnostics/ 整段删掉重编，
    //    发布版行为必须逐字节相同。所以这里**不做任何动作** ——
    //    现在连目录都不建（目录要到真有东西写时才建）。
    // -------------------------------------------------------------------------
    wag::diagnostics::set_enabled(cli.diagnostics);
    // 【输出根 = exe 所在目录】锚点是 exe，不是进程 CWD
    //   （CWD 随启动方式变化：快捷方式 / 终端 / 控制台调用都不同）。
    //   本模块自己拼 `debug/`；**关掉时它不会建任何目录**。
    wag::diagnostics::set_output_dir(wag::platform::executable_dir());
    // 【会话标识】`started_at` 兼作会话 id：
    //   三个 jsonl 每行的 `session` 就取它，所以它必须在**任何一行写出之前**定下。
    //   `engine` 取自构建配置的同一个变量，不在源码里再硬编码一次。
    if (cli.diagnostics) {
        wag::diagnostics::begin_session(
            wag::diagnostics::session_info{kLuminosaDirName, WAG_ENGINE_VERSION});
    }
    if (cli.diagnostics) {
        wag::log::record("main", "诊断导出已开启（--diagnostics）");
    }

    // -------------------------------------------------------------------------
    //  建立引擎实例
    //
    //  【为什么构造不会失败】
    //    因为它不做事：窗口、渲染器、脚本状态的建立都是后续的显式步骤,
    //    而那些步骤有明确的返回值或明确的报错路径。
    //    ⇒ 于是"启动失败"只可能来自"动作"，不可能来自"构造"。
    // -------------------------------------------------------------------------
    wag::engine eng;

    // 【失焦暂停是**启动期**的选择，不是脚本能调的东西】
    //   理由见 command_line::no_focus_pause 的注释：它必须在 CI 与玩家机器上
    //   由**启动方式**决定，而不是由同一份游戏逻辑在运行中翻转。
    eng.set_focus_pause(!cli.no_focus_pause);

    // 【控制通道】**只搬运字符串**：引擎不认识结点，也不解释命令。
    //   · `--control=<dir>` 给了才启用；不给则本类一个字节也不读写。
    //   · `--start-node` 原样存起来，脚本用 `wag.argv.start_node()` 取。
    //   · `state.json` 写到 `<安装根>/debug/`（与 diagnostics 的出口同一目录）。
    eng.control().configure(cli.control_dir != nullptr ? cli.control_dir : "",
                            cli.start_node != nullptr ? cli.start_node : "",
                            wag::platform::executable_dir());
    if (cli.no_focus_pause) {
        wag::log::record("main", "失焦**不**作为暂停源（--no-focus-pause）");
    }

    // -------------------------------------------------------------------------
    //  启动序列
    //
    //  【顺序（已定的部分）】
    //    步骤 0  预置 13 个钩子槽（C++ 侧）
    //    步骤 1  环境封堵
    //    步骤 2  钩子槽自检
    //    步骤 3~5 引导脚本 / 映射层 / 按层装载（脚本侧）
    //    步骤 6  进入帧循环（由脚本侧**显式请求**）
    //
    //  【步骤 6 的形态为什么重要】
    //    若"引导脚本返回即进入循环"，那么"返回"与"跑完了"是同一个信号,
    //    中途失败也会照进循环 —— 于是"引擎必需"那条规格只在纸面上成立。
    // -------------------------------------------------------------------------
    wag::boot_sequence(eng);
    eng.configure_reload(install_root,
                         std::to_string(wag::diagnostics::session_id()));

    // 回退提示：窗口创建后一次性非模态提示（D1 / D7）。
    if (wag::log::privacy_log_fallback() || wag::log::full_log_fallback()) {
        std::string notice = "无法在引擎目录写入日志，已回退到：\n";
        if (!wag::log::privacy_log_path().empty()) {
            notice += "隐私日志：" + wag::log::privacy_log_path() + "\n";
        }
        if (!wag::log::full_log_path().empty()) {
            notice += "完整日志：" + wag::log::full_log_path();
        }
        wag::platform::show_log_notice(notice);
    }

    // -------------------------------------------------------------------------
    //  引导脚本
    //
    //  【为什么它在启动序列之后、帧循环之前】
    //    因为引导脚本要用的东西（窗口、GL 上下文、Lua 状态、13 个槽、引擎原语、
    //    环境封堵）全部由启动序列准备好。而它必须在帧循环之前 —— 它的职责之一
    //    就是"表达可以开始跑了"。
    //
    //  【安装根已在本文件开头解析（日志初始化之前，D7）】
    //   这里只做空值检查；不再重新解析，避免第二个来源。
    // -------------------------------------------------------------------------
    if (install_root.empty()) {
        wag::error_value e;
        e.cls = wag::error_class::api_misuse;
        e.where = "命令行 --root";
        e.got = "未给出";
        e.want = "一个安装根路径（未给出时应能由 exe 所在目录推出）";
        wag::fail_api_misuse(e);
    }

    const std::string init_path =
        install_root + "/" + kLuminosaDirName + "/WagLib/init.lua";

    // 【发布安装根 —— 必须在引导脚本之前】
    //   resolve.lua 的 install_root() 读 `wag.install_root`（已定候选 (甲)）；
    //   脚本侧的**一切资源路径**都由它拼出来（"引擎不解析路径"）。
    //   ⚠️ 晚发布 = 引导脚本第一次加载资源时拿不到根。
    // 【先把安装根交给全局】错误记录与 Lua chunk 名都要用它做"相对化"
    //   锚点是**exe 所在目录**，不是 CWD，也不是目录名。
    wag::paths::set_install_root(install_root);

    wag::script::publish_install_root(eng.script(), install_root);

    wag::script::prepend_package_path(eng.script(), install_root + "/" + kLuminosaDirName);

    const bool script_ready = load_bootstrap_script(eng, init_path);

    // -------------------------------------------------------------------------
    //  帧循环
    //
    //  【已决：只有脚本**显式请求**了才进循环】
    //    若引导脚本返回即进入循环，那么"返回"与"装载完成"就是同一个信号，
    //    中途失败也会照进循环 —— 于是"引擎必需"那条规格只在纸面上成立。
    //
    //  【没请求时怎么办 —— 记一条 L2 然后正常退出】
    //    它不是数据矛盾（引擎没有处在非法状态），而是"脚本没跑完"。
    //    终止会让一个脚本笔误变成"崩溃"；而继续跑又会跑到一个没有任何钩子实现的
    //    空世界里。⇒ 记下来、退出，并给出退出码 1（shell 能看出"没正常跑完"）。
    //
    //  【不确定代码 · 待定】"没请求"的处置是否该是 L2
    //    另一种立场是把它当 L3（"引擎必需件缺失"）。本处先按 L2 + 退出码 1,
    //    待"错误分级"那一批一并定。
    // -------------------------------------------------------------------------
    if (!script_ready) {
        wag::log::warn("main", wag::log::format(
            "引导脚本没有请求进入帧循环", "（装载可能中途失败；引擎不会自行开始跑）"));
        wag::shutdown_sequence(eng);
        wag::log::record("engine", "引擎关闭（未进入帧循环）");
        wag::log::shutdown();
        return 1;
    }

    // -------------------------------------------------------------------------
    //  L4 终止前也调一次 on_game_exit（2026-11-18 定）
    //
    //  【为什么在这里注册】此时 init 已完成（script_ready），引擎仍然完整；
    //    而 L4 可能发生在帧循环期间或关闭序列中途 —— 后者由幂等闸门覆盖，
    //    不会重复调。
    // -------------------------------------------------------------------------
    g_engine_for_exit = &eng;
    wag::set_before_terminate(&call_exit_hook_before_terminate);

    {
        wag::time::frame_pipeline pipeline;

        // 【装拖拽补跑入口】engine 不持有管线（管线是这里的局部对象），
        //   所以用"函数指针 + 状态"把它接过去。拆管线之前必须撤掉它。
        drag_tick_state tick_state{&eng, &pipeline};
        eng.set_frame_tick(&drag_tick, &tick_state);

        // 【逐帧】
        //
        // 【为什么"等"在帧外（2026-11-14）】
        //   它原来在阶段 2 里。而那段等待里的 `SDL_WaitEventTimeout` 正是窗口拖拽的
        //   模态循环会抢走控制权的地方；只要它还在 `run_one_frame` 内部，
        //   从消息路径补跑一帧就必然是 12 阶段管线的重入。
        //   ⇒ 搬到帧外之后，消息路径才有资格跑帧（第二步已启用）。
        //
        // 【顺序：先等+泵，再跑帧】
        //   泵到的事件会被本帧的阶段 3 消费 ⇒ 事件时序与搬之前一致（同帧可见）。
        //   若反过来（帧后等），所有事件都会晚一帧。
        while (true) {
            pipeline.wait_until_next_due(eng);

            // 【这一格可能已经被"拖拽补跑"吃掉了】
            //   拖拽期间，暴露事件会在 `wait_until_next_due` 内部（泵里）先补跑一帧，
            //   并把下一帧的 due 推后。若这里不看一眼就照常跑，同一格里会跑两帧
            //   —— 实测 1.2 秒拖拽跑出 91 帧（目标 72）。
            //
            // 【首帧没有 due】此时必须照常跑，否则永远起不来。
            if (!pipeline.has_next_due() || pipeline.frame_due()) {
                pipeline.run_one_frame(eng);
            } else {
                continue;
            }

            // reload 命令在帧内被 Lua 消费，但状态切换必须发生在帧边界。
            if (eng.reload_commit_pending()) {
                static_cast<void>(eng.commit_reload_scripts());
            }

            // 【退出的判定在帧末 —— 这一点是刻意的】
            //   那样"这一帧"就永远是完整的一帧（照常渲染、照常呈现），
            //   而不是"走到一半就断了"。
            if (eng.exit_requested()) {
                break;
            }
        }

        // 【必须撤掉】它指着上面的 `pipeline`（栈上的局部对象）。
        //   留着的话，关闭序列期间的任何一次暴露事件都会碰到一个**已析构**的管线
        //   —— 那是典型的 use-after-scope。engine 不拥有管线，所以由这里收尾。
        eng.set_frame_tick(nullptr, nullptr);
    }

    // -------------------------------------------------------------------------
    //  关闭序列
    //
    //  【已定的三条】
    //    ① on_game_exit 必须在任何东西被拆之前
    //    ② 关闭序列里的错误只记日志（不弹窗、不"带错继续"）
    //    ③ on_game_exit 不保证被调用
    //
    //  【第 7 步"退出进程"不在这里做】
    //    本函数把引擎拆干净，然后返回一个退出码。让 shutdown_sequence 负责
    //    "退出进程"会让它无法被测试（测试里不该退出进程）。
    // -------------------------------------------------------------------------
    wag::shutdown_sequence(eng);

    wag::log::record("engine", "引擎关闭");
    wag::log::shutdown();
    return 0;
}
