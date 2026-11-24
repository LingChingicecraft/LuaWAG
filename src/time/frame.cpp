// =============================================================================
// wag/time/frame.cpp
//
// wag/time/frame.cpp
//
// 12 阶段的驱动实现：从帧形态快照、逻辑钩子、渲染钩子，到帧末状态应用。
//
// 【阶段清单】
// · 阶段 1 帧形态快照 / 帧号 / 几何整包 / 休眠判定 / 变换重算
// · 阶段 2 帧率控制（绝对时间线 + “等消息或超时”）
// · 阶段 3 事件位映射与“关闭请求 ⇒ 请求退出”
// · 阶段 4 输入采样
// · 阶段 5/6 Ext / Main（或 Pause）逻辑钩子
// · 阶段 7 begin_frame → 开绘制相位门(scene) → on_render_begin
// · 阶段 8 on_render_main / on_render_pause
// · 阶段 9 切相位门(ext) → on_render_ext → on_render_end → 关闸
// · 阶段 10 capture_staged_snapshot()（休眠帧跳过）
// · 阶段 11 apply_pending_state()
// · 阶段 12 present()
// · 帧末 apply_pending_window_size() + 每秒统计
//
// 【冻结画面】`draw_frozen_snapshot` / `adopt_staged...` 目前没有调用者：
// 暂停背景由 on_render_pause 自己画，引擎不预设“暂停时显示最后一帧”。
// 它是待清理/待显式冻结 API，不是漏接。
// =============================================================================

#include "time/frame.hpp"

#include "container/pool.hpp"
#include "diagnostics/diagnostics.hpp"
#include "error/error.hpp"
#include "input/input.hpp"
#include "log/log.hpp"
#include "draw/renderer.hpp"
#include "platform/window.hpp"
#include "script/hook.hpp"
#include "script/collide_hook.hpp"
#include "script/state.hpp"

// 【开发期工具接缝】本文件**只认识这一个头**，不认识 trace.hpp / img_probe.hpp。
// 两个开关（WAG_DEVTOOLS_TRACE / WAG_DEVTOOLS_PROBE）默认关闭时，
// 这个头只提供空宏 ⇒ 本文件编译结果与接入前完全一致。
#include "devtools/seam.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace wag::time {

namespace {

/// 单调时钟，单位纳秒。
///
/// 【为什么用 steady_clock 而不是 system_clock】单调时钟不受系统时间调整影响
/// —— 帧率控制要的是"过了多久"，不是"现在几点"。用 system_clock 会在用户
/// 改时间或 NTP 校时时让帧率跳变。
///
/// 【它为什么在这里，而不是在 platform 层】
/// 它是阶段 2 的私有需要，目前只有这一个消费者。等到别处也需要时钟时
/// （例如 wag.debug 的帧耗时），再把它提到平台层并收口接口。
[[nodiscard]] long long steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

// 本文件里的 `log::` 就是 `wag::log`（wag::time 嵌在 wag 内，直接查找即可）。

// =============================================================================
// 【为什么本文件必须包含那几个部件的头 —— 这一条修的是一个真实的编译错误】
//
// engine/engine.hpp 对 window / renderer / script::state **只做前向声明**。
// 那是刻意的：内核要描述"引擎实例由哪些部件组成"，但不需要知道它们的内部。
//
// 而**前向声明只够声明指针与引用，不够调用成员函数**。
// 本文件里到处是这种调用：
// eng.window().get_dip_size()
// eng.window().consume_events()

// eng.script()（作为实参传给 script::call_hook，那个签名也需要完整类型）
// ⇒ 而如果只包含 engine.hpp（经 frame.hpp 间接），`window` 就是一个
// **不完整类型** ⇒ GCC 报 "invalid use of incomplete type"。
//
// ⇒ 所以调用方必须自己包含它要用到的那个部件的头。
// 这不是缺陷，而是 pimpl + 前向声明的**必然结果** —— 而它有一个好处：
// 本文件想调窗口的函数，就必须显式 #include window.hpp
// ⇒ 于是"time 依赖 platform"这件事**在源码里看得见**。
// =============================================================================

// -----------------------------------------------------------------------------
// 名字表
// -----------------------------------------------------------------------------

const char* mode_name(frame_mode m) noexcept {
    switch (m) {
    case frame_mode::normal:
        return "normal";
    case frame_mode::paused:
        return "paused";
    }
    return "?";
}

const char* stage_name(stage s) noexcept {
    switch (s) {
    case stage::frame_begin:
        return "frame_begin";
    case stage::rate_control:
        return "rate_control";
    case stage::event_consume:
        return "event_consume";
    case stage::input_sample:
        return "input_sample";
    case stage::ext_logic:
        return "ext_logic";
    case stage::main_logic:
        return "main_logic";
    case stage::render_begin:
        return "render_begin";
    case stage::scene_render:
        return "scene_render";
    case stage::ext_render_and_finish:
        return "ext_render_and_finish";
    case stage::snapshot:
        return "snapshot";
    case stage::state_apply:
        return "state_apply";
    case stage::present:
        return "present";
    }
    return "?";
}

// =============================================================================
// 驱动
// =============================================================================

void frame_pipeline::run_one_frame(engine& eng) {
 // 【热重载闸门】：事务期间帧管线不得推进。
 // 正常情况下重载是同步事务，不会走到这里；这里是防止第二个入口/
 // 消息补跑在闸门内继续跑旧 Lua 的硬守卫。
    if (eng.reloading()) {
        return;
    }

 // 【重入守卫（2026-11-14）】`run_one_frame` 现在有两个可能的入口：主循环、
 // 以及（第二步）消息路径里的补跑。没有守卫的话，第二次进入会让 12 阶段
 // 管线在栈上自我嵌套 —— 状态撕裂、且偶发难复现。
 //
 // 【为什么用 RAII】本函数有多处提前 return（统计基准、休眠分支…），
 // 手写置位/复位一定会漏一处。
 //
 // 【为什么这里用 L2 而不是 L4】按映射表 internal_invariant 本该 terminate；
 // 但**这一次帧根本没跑**，状态没有被破坏 ⇒ 拒绝并记下来即可。
 // 让引擎因为一个"多了一个入口"的诊断问题当场退出，代价更大。
    struct frame_guard {
        bool& flag;
        explicit frame_guard(bool& f) noexcept : flag(f) { flag = true; }
        ~frame_guard() { flag = false; }
    };

    if (in_frame_) {
        log::warn("帧", "run_one_frame 被重入，已拒绝", "说明有第二个入口没有过 in_frame() 守卫");
        return;
    }
    frame_guard guard(in_frame_);

 // 阶段的执行顺序就是内核的规格。**不要重排**。
 //
 // 【顺序不可变的两条实例】
 // ① 阶段 10 必须在阶段 11 之前 —— 快照要在状态切换之前拍好，
 // 这样"冻结画面与状态切换"才是同一个原子事件。
 // ② 阶段 3 必须在阶段 5/6 之前 —— 事件必须先被消费，
 // 逻辑才可能看到"这一帧发生了什么"。

 // 【开发期工具接缝 · 不改变顺序与语义】
 // WAG_SEAM_* 在 WAG_DEVTOOLS_TRACE / WAG_DEVTOOLS_PROBE 都关闭时展开成 ((void)0)
 // ⇒ 这里做的只是"把每次 stage_* 调用包进一对 ENTER/EXIT"，不重排、不插入逻辑。
 // 接缝只接**阶段边界**：阶段内部（例如阶段 6 的碰撞派发位置将来可能变）
 // 刻意不接 —— 边界是规格，内部还不是。
 //
 // 【INIT 必须在任何记录之前】它里面会 trace::reset 定时间原点；
 // 若放在阶段 1 之后，刚写下的 frame_begin 两条会被擦掉（这个坑踩过一次）。
    WAG_SEAM_INIT();

    WAG_SEAM_ENTER(frame_begin);
    stage_frame_begin(eng);
    WAG_SEAM_EXIT(frame_begin);

 // 帧号在阶段 1 内才递增 ⇒ 这一句必须放在 stage_frame_begin **之后**。
    WAG_SEAM_SET_FRAME(fs.frame_count);

 // 【诊断导出的 frame 字段】与接缝同一时机。
 // 它只是一个整数赋值；关掉 --diagnostics 时也不建目录、不开文件。
    wag::diagnostics::set_frame(fs.frame_count);
 // 普通日志的 L3/L4 行带 frame；L1 不带（D3）。
    wag::log::set_frame(fs.frame_count);

 // 【休眠分支从这里开始分叉】
 // 阶段 2、3、4、5、6 在休眠时**照常运行**。
    WAG_SEAM_ENTER(rate_control);
    stage_rate_control(eng);
    WAG_SEAM_EXIT(rate_control);

 // 【perf_log 的测量起点 = 睡眠之后】
 // `frame_ms` 只统计这一帧的**工作耗时**（逻辑 + 渲染 + 呈现），不含阶段 2
 // 的限速睡眠。
 // 【为什么不能含睡眠】vsync 关、`present` 不阻塞 ⇒ 整帧里唯一的阻塞就是这段
 // 睡眠；含了它就恒等于 `1/目标帧率`（日志里已经有一个 `fps=` 行了），尖峰看不见。
 // 【为什么不是"只渲染段"】那会漏掉**逻辑**尖峰（脚本的 frame 回调）。
    const long long work_begin_ns = steady_now_ns();

    WAG_SEAM_ENTER(event_consume);
    stage_event_consume(eng);
    WAG_SEAM_EXIT(event_consume);

    WAG_SEAM_ENTER(input_sample);
    stage_input_sample(eng);
    WAG_SEAM_EXIT(input_sample);

    WAG_SEAM_ENTER(ext_logic);
    stage_ext_logic(eng);
    WAG_SEAM_EXIT(ext_logic);

    WAG_SEAM_ENTER(main_logic);
    stage_main_logic(eng);
    WAG_SEAM_EXIT(main_logic);

 // 阶段 7~10 属于渲染管线。
    if (!fs.pipeline_asleep) {
        WAG_SEAM_ENTER(render_begin);
        stage_render_begin(eng);
        WAG_SEAM_EXIT(render_begin);

        WAG_SEAM_ENTER(scene_render);
        stage_scene_render(eng);
        WAG_SEAM_EXIT(scene_render);

        WAG_SEAM_ENTER(ext_render_end);
        stage_ext_render_and_finish(eng);
        WAG_SEAM_EXIT(ext_render_end);

 // ---- 批次管线的帧末 flush（flush 触发③）----
 // 【位置：阶段 9 之后、快照之前】渲染钩子全部结束 ⇒ 攒的顶点
 // 在此提交；快照拍的是"玩家看到的最终画面"，必须排在 flush 与
 // （将来的）效果回画之后。休眠帧不进这里（缓冲本来就是空的）。
 //
 // 【接缝】end_frame 不是阶段，是阶段 9 之后的帧末 flush ⇒ 用 MARK，不新增阶段。
 // 把它塞进 12 阶段会污染"阶段 = 规格"这条（见 run_one_frame 顶部注释）。
        WAG_SEAM_MARK(0xE0F0u); // "end_frame 的 flush 即将发生"
        eng.renderer().end_frame();

 // 快照也在渲染管线内，但**不属于** 12 阶段里可跳过的那一段：
 // 它是阶段 10，仍在 if 内（休眠时跳过是规格）。
        WAG_SEAM_ENTER(snapshot);
        stage_snapshot(eng);
        WAG_SEAM_EXIT(snapshot);
    }
 // 【休眠时阶段 10 被跳过 —— 这是必须的】
 // 若休眠时仍然更新暂存快照，最小化会把快照清空，而下一帧恢复并进入
 // 暂停时会显示一张空白冻结帧。跳过之后，阶段 11 取到的仍是最后一张
 // 有效渲染结果。

 // 阶段 11 在休眠时**继续**，但只做状态切换。
    WAG_SEAM_ENTER(state_apply);
    stage_state_apply(eng);
    WAG_SEAM_EXIT(state_apply);

    if (!fs.pipeline_asleep) {
#if defined(WAG_DEVTOOLS_PROBE)
 // 【接缝】present 之前的那一瞬 —— 像素探针唯一合法的抓帧点。
 // 之所以写成 #if 而不是宏：它要先花一次平台调用问窗口像素尺寸，
 // 而"关掉时连这次调用都不发生"必须让读到这里的人看得见。
        {
            const platform::dip_size fb = eng.window().get_pixel_size();
            ::wag::devtools::on_before_present(fb.w, fb.h);
        }
#endif
        WAG_SEAM_ENTER(present);
        stage_present(eng);
        WAG_SEAM_EXIT(present);
    }

 // ---- 帧末：容器池回收----
 //
 // 【为什么必须在这里】kill() 只标记 dead；真正释放（代号 +1、槽回空闲表）
 // 发生在帧末。若没有这一步，槽永远不回收 ⇒ 到 32767 就 L3。
 // 【为什么在 present 之后】本帧的画面已经提交；回收不改变本帧任何可见结果，
 // 只影响下一帧的逻辑看到的空闲槽。
    eng.containers().recycle_dead();

 // ---- 帧末：del 派发----
 //
 // 【为什么在这里】：del 在 recycle_dead() 时派发，不在 kill 当帧。
 // 与 一致（dead 不算空位、帧末回收），且避免"同帧先杀后建"的顺序敏感。
 // 【只对 del 位存在的类派发】：无回调的类（子弹/粒子）不付这个成本。
    {
        auto& pools = eng.containers();
        auto& classes = pools.classes();
        for (const auto& r : pools.recycled()) {
            if (!classes.has(r.class_id, wag::container::class_callback::del)) {
                continue;
            }
            if (!eng.script().has_dispatcher()) {
                continue; // 清空分派器时已记过一条；这里不刷屏
            }
            std::string err;
            if (!eng.script().call_dispatcher("del", r.h, 0, 0, &err)) {
                wag::fail_script("del 回调报错：" + err);
            }
        }
    }

 // ---- 帧末：应用本帧请求的外部设备变更 ----
 //
 // 【为什么它不在这 12 个阶段里】
 // 12 个阶段是"逻辑与渲染"的规格；而"改窗口尺寸"是**改外部设备**，
 // 它是这一帧的**收尾动作**，下一帧的阶段 1 会读到新尺寸。
 // 把它塞进某个阶段会让"阶段编号 = 帧内次序"这条被稀释。
 //
 // 【为什么必须在阶段 11 之后（状态应用之后）、阶段 12 之后】
 // 阶段 11 会应用其它待生效值（状态、目标帧率）。窗口尺寸与它们同类，
 // 所以要在同一个"帧末"里一起收口；而放在阶段 12 之后可以让"这一帧
 // 呈现的画面"仍然属于旧尺寸 —— 于是不会出现"画到一半窗口变了"。
 //
 // 【命令式的收口点】引擎在这里只做一次"申请"，不记住、不纠正、不吸附。
    eng.apply_pending_window_size();

 // =========================================================================
 // perf_log：**逐帧一行**
 //
 // 【为什么在帧末】`frame_ms` 只有到这里才知道；`containers` 也只有到
 // 回收之后才等于"这一帧结束时真正存在的对象数"。
 //
 // 【E 计数器所有权】perf **独占** `take_draw_stats()`（取走即清零），
 // 下面的 1 Hz 报告改用本类里的累加值。
 // 没选 `peek_draw_stats()`：那要每个读者各自记基线，而 `take` 会把基线清零
 // ⇒ 混用即崩。**一份基线**是这里唯一指望得住的东西。
 //
 // 【A `draw_calls`】取 `submissions`（`glDrawElements` 次数 = **批次数**）。
 // 图元数 ≈ 容器数、顶点数 = 4 × 图元，都是派生的；只有批次数反映**合批效率**。
 //
 // 【B `frame_ms` 的区间】睡眠之后 → 帧末，见上面的 `work_begin_ns`。
 //
 // 【C 暂停 / 最小化】**暂停写、最小化也写**。
 // 理由是存活检测只有“有没新行”这一个判据：
 // 若最小化不写，控制台一最小化就会把引擎判成"卡住"。
 // 代价是一串 `frame_ms ≈ 0` 的行 —— 但那是**真的**（就是没干活），不是噪声。
 // =========================================================================
    const auto frame_draw = eng.renderer().take_draw_stats();
    acc_submissions_ += frame_draw.submissions;
    acc_flushes_ += frame_draw.flushes;
    acc_vertices_ += frame_draw.vertices;
    acc_primitives_ += frame_draw.primitives;

    if (wag::diagnostics::enabled()) {
        std::uint32_t alive = 0;
        {
            auto& pools = eng.containers();
            for (std::uint32_t i = 0; i < pools.pool_count(); ++i) {
                alive += pools.pool(i).alive_count();
            }
        }

        wag::diagnostics::perf_sample sample;
        sample.target_fps = eng.get_target_fps();
        sample.containers = alive;
        sample.draw_calls = static_cast<std::uint32_t>(frame_draw.submissions);
        sample.frame_ms =
            static_cast<double>(steady_now_ns() - work_begin_ns) / 1'000'000.0;
        const auto ls = eng.renderer().take_light_stats();
        sample.light_lights = ls.lights;
        sample.light_occluders = ls.occluders;
        sample.light_commits = ls.commits;
        sample.light_fill_ratio = ls.fill_ratio;

        if (ls.over_lights || ls.over_fill || ls.over_commits) {
            std::string got;
            std::string want;
            if (ls.over_lights) {
                got += "lights=" + std::to_string(ls.lights) + ";";
                want += "lights<=256;";
            }
            if (ls.over_fill) {
                got += "fill=" + std::to_string(ls.fill_ratio) + ";";
                want += "fill<=32x;";
            }
            if (ls.over_commits) {
                got += "commits=" + std::to_string(ls.commits) + ";";
                want += "commits<=8;";
            }
            wag::diagnostics::error_record er;
            er.level = "warn";
            er.cls = "light_budget";
            er.where = "light";
            er.got = got;
            er.want = want;
            er.sentence = "光照预算超限";
            wag::diagnostics::write_error_line(er);
        }
        wag::diagnostics::write_perf_line(sample);

 // 【容量上限（64 MB）到顶 ⇒ 记一条，只记一次】
 // 诊断模块自己**不依赖日志**（它那条"零模块依赖"的判据），
 // 所以这条日志由这里打。
        if (wag::diagnostics::perf_capped() && !perf_cap_logged_) {
            perf_cap_logged_ = true;
            log::record("诊断", "perf_log 已达 64 MB 上限，停止写入", "文件保留的是**前** 64 MB —— 若要看崩溃现场，请缩短会话");
        }
    }

 // ---- 帧率读数（每秒一行）----
 //
 // 【它为什么在这里，而不是在某个钩子里】
 // 它是**引擎自身的读数**，属于已定的 wag.debug 那一项（"实际逻辑帧数、
 // 每帧的时间去向"）。写在帧结构的收尾处，因此它统计的是**真正的帧**，
 // 与脚本无关、也不受钩子是否被实现影响。
 //
 // 【为什么每秒才一行】每帧一行会（一）刷屏（二）自己成为性能负担 ——
 // 每秒一行的量级足以算出帧率，而它对帧耗时的污染可以忽略。
 //
 // 【为什么用 steady_now_ns】帧率要的是"过了多久"，不是"现在几点"。
    ++stat_frames_;
    const long long now_ns = steady_now_ns();
    if (stat_epoch_ns_ == 0) {
        stat_epoch_ns_ = now_ns;
        stat_frames_ = 0;
        return;
    }
    const long long elapsed_ns = now_ns - stat_epoch_ns_;
    if (elapsed_ns >= 1'000'000'000LL) {
        const double fps =
            static_cast<double>(stat_frames_) * 1'000'000'000.0 /
            static_cast<double>(elapsed_ns);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "fps=%.1f  每帧=%.3f ms",
                      fps, (fps > 0.0) ? (1000.0 / fps) : 0.0);

 // ---- 渲染侧统计（“不设上限 ≠ 不提供证据”）----
 // 计数器已经由 perf_log **逐帧取走**，这里读累加值再清零（见帧末那段）。
 // 数值与"每秒取一次"完全相同：同一窗口内的总量不因分割方式而变。
        char rbuf[128];
        std::snprintf(rbuf, sizeof(rbuf),
                      "提交=%llu flush=%llu 顶点=%llu 图元=%llu",
                      static_cast<unsigned long long>(acc_submissions_),
                      static_cast<unsigned long long>(acc_flushes_),
                      static_cast<unsigned long long>(acc_vertices_),
                      static_cast<unsigned long long>(acc_primitives_));
        acc_submissions_ = 0;
        acc_flushes_ = 0;
        acc_vertices_ = 0;
        acc_primitives_ = 0;

        log::record("debug", log::format("帧率", buf));
        log::record("debug", log::format("渲染统计", rbuf));

 // 光照快照（1 Hz）→ state.json
        {
            const auto& L = eng.renderer().last_light_stats();
            auto& ch = eng.control();
            ch.publish("light.lights", std::to_string(L.lights));
            ch.publish("light.occluders", std::to_string(L.occluders));
            ch.publish("light.bands", std::to_string(L.bands));
            ch.publish("light.commits", std::to_string(L.commits));
            ch.publish("light.fill_ratio", std::to_string(L.fill_ratio));
        }

 // ---- 池子占用（1 Hz）----
 //
 // 【为什么借这里】本块已经以 1 秒为周期运行，而且它统计的是**真正的帧**。
 // 另起一个计时器就会产生第二个"多久写一次"的真相。
 // 【为什么用 name()/capacity()/alive_count()】
 //：池必须命名（诊断用）；：容量固定 32767，不是可配置值；
 // 存活数含"已标 dead 但未到帧末回收"的槽 —— 与 的判据同一个口径。
        if (wag::diagnostics::enabled()) {
            auto& pools = eng.containers();
            std::vector<wag::diagnostics::pool_row> rows;
            rows.reserve(pools.pool_count());
            for (std::uint32_t i = 0; i < pools.pool_count(); ++i) {
                const auto& p = pools.pool(i);
                rows.push_back(
                    {p.name(), p.capacity(), p.alive_count()});
            }
            wag::diagnostics::write_pool_line(rows.data(), rows.size());
        }

        stat_frames_ = 0;
        stat_epoch_ns_ = now_ns;
    }
}

// =============================================================================
// 阶段 1：帧开始
// =============================================================================

void frame_pipeline::stage_frame_begin(engine& eng) {
 // 本阶段做五件事，顺序如下。
 //
 // 【为什么五件事都在这里】
 // 因为它们都是"本帧的常量"：一旦定下，整帧不再变。放在同一个位置
 // 使"本帧一个值"成为结构性的保证，而不是纪律。
 // （第 5 件"拖拽状态"是 2026-11-15 加的；它是外部会改、而脚本要读的量。）

 // ---- 第 1 件：定本帧形态 ----
 //
 // 从**活状态**取一次，此后整帧用这个快照。
    const engine_state live = eng.get_state();
    fs.mode = (live == engine_state::normal) ? frame_mode::normal
                                            : frame_mode::paused;

 // ---- 第 2 件：递增帧号 ----
 //
 // 只在 normal 帧递增。于是：
 // · 暂停不推进逻辑时间
 // · 全帧（含所有钩子）读到一致的帧号
    if (fs.mode == frame_mode::normal) {
        ++fs.frame_count;
    }

 // ---- 第 3 件：取窗口尺寸与 DPI 快照 ----
 //
 // 【为什么必须在这里取，而不能事后取】
 // 它们都是"外部会改、而渲染要用"的量。若不快照，帧中途发生的变化会让
 // 同一个帧内出现两个真相。
 //
 // 【为什么读取成本可以忽略】
 // 两者都只是读平台对象里的字段，几十纳秒级。120/240 Hz 下每秒最多
 // 几百次。因此不需要任何缓存 —— 而缓存恰恰是这里最不该做的事
 // （副本会变脏 ⇒ 画面错而不崩）。
    {
 // ---- 整包几何，一次取回（不要分成三次 getter 调）----
 //
 // 【为什么一次取】见 frame_state::geom 的说明：分别调允许"在不同时刻调",
 // 而窗口恰好在那两点之间变了就会得到**一帧内两个瞬间的尺寸**（实测事实 15）。
 // 一次取回之后，"本帧一个值"对 DIP 与像素**同时**成立。
 //
 // 【为什么不缓存到跨帧】窗口是"外部会改、而渲染要用"的量（拖窗口、全屏、
 // 换显示器都会改），副本会变脏 ⇒ 画面错而不崩。所以**每帧取一次**，
 // 而不是"取一次一直用"。
        const platform::window::frame_geometry g = eng.window().get_frame_geometry();
        fs.geom.dip_w = g.dip.w;
        fs.geom.dip_h = g.dip.h;
        fs.geom.pixel_w = g.pixel.w;
        fs.geom.pixel_h = g.pixel.h;
        fs.geom.scale = g.scale;
        fs.window_dip = space::rect{0.0, 0.0, fs.geom.dip_w, fs.geom.dip_h};
        fs.dpi_scale = fs.geom.scale;

 // ---- 第 4 件的一部分：休眠判定 ----
 //
 // 【判定点就在这里，理由是具体的】
 // 在阶段 1 判定 ⇒ 下面那次 recompute() 里**那条除以窗口尺寸的除法
 // 根本不会执行**（或者执行了也没人用），比"算了之后检查 inf/NaN"
 // 干净。而且整帧行为一致。
 //
 // 【为什么用 !is_valid() 而不是 is_empty()】
 // `fs.window_dip` 是 **space::rect**（它是从 dip_size 的两个分量
 // 构造出来的），而 `is_empty()` 是 **dip_size** 的成员 ——
 // 两者是不同的类型，把它们的方法名混用是一个真实的编译错误。
 //
 // 而语义上两者等价：
 // dip_size::is_empty() ⇒ w <= 0 || h <= 0
 // rect::is_valid() ⇒ w > 0 && h > 0
 // ⇒ 于是 `!is_valid()` 正是"窗口尺寸为零 ⇒ 休眠"。
 // （rect 的 w/h 不可能是负的 —— 它们由 dip_size 那两个 >= 0 的分量填入。）
        fs.pipeline_asleep = !fs.window_dip.is_valid();
    }

 // ---- 第 4 件：算本帧变换 ----
 //
 // 【它是唯一的写入点】viewport 的变换只能由 recompute() 改，而 recompute
 // 只读 render 与传进来的窗口尺寸。于是"变换必须由 {render, 策略,
 // 窗口尺寸快照} 派生，不得单独赋值"这条实施约束在类型上成立。
 //
 // 【休眠时也算】因为窗口尺寸为 0 时 from_rects 会得到一个退化的 scale,
 // 把一切映到同一点 —— 这是一个**有限的**值，不会产生 inf/NaN。
 // 让它可以被算出来，比"算之前判断、算之后判断"少两处分支。
    eng.viewport().recompute(fs.window_dip);

 // ---- 第 5 件：拖拽状态快照（2026-11-15）----
 //
 // 【为什么也在这里定格】它和上面四个量同性质：**本帧的常量**。
 // 一旦帧开始，整帧的钩子必须读到同一个值 —— 否则同一个帧里
 // "主逻辑钩子看到的"和"暂停钩子看到的"可能不同，而脚本据此
 // 做的暂停/恢复决策就会自相矛盾。
 //
 // 【值从哪来】`engine::window_sizemove()`：由窗口消息钩子
 // （`WM_ENTERSIZEMOVE` / `WM_EXITSIZEMOVE`）写，这里只读。
 // **"拖拽"同时包含"拖标题栏移动"与"拖边框缩放"** ——
 // Windows 对两者发的是同一条消息，引擎不区分。
    fs.dragging = eng.window_sizemove();

 // ---- 第 6 件：焦点状态快照（2026-11-16，）----
 //
 // 【与 dragging 同一条纪律】外部会改、脚本要读 ⇒ 帧开始定格。
 // 焦点变化在帧前的泵里发生 ⇒ 这里读到的一定是"本帧生效"的值。
 //
 // 【它同时喂两处】`frame_info::has_focus`（13 个钩子都看得到）与
 // 阶段 3 的焦点钩子 —— 两者读到的是同一个值，不会自相矛盾。
    fs.has_focus = eng.window().has_focus();

 // ---- 第 7 件：整帧像素尺寸（C1，2026-11-16）----
 //
 // 【为什么要在这里定格】`wag.space.get_frame_size()` 要能在阶段 1 之后的任意
 // 钩子里读到"本帧的"值；而渲染器里的 `frame_pixel_` 要到**渲染阶段**才写。
 // 这里从**平台**（`window::get_pixel_size()`，物理像素的唯一来源）取一次。
 //
 // 【与 DPI 的关系】本平台 DIP = 像素（引擎有断言保证）；但这里取的是
 // **像素**那一侧 —— 所以就算将来 DPI ≠ 1，这个值仍然是帧缓冲的真实尺寸。
    {
        const auto px = eng.window().get_pixel_size();
        eng.set_frame_pixel_size(static_cast<int>(px.w + 0.5),
                                 static_cast<int>(px.h + 0.5));
    }

 // ---- 第 8 件：控制通道（，2026-11-17）----
 //
 // 【为什么在阶段 1】命令是"本帧的输入"：钩子要用到它，就必须在同一帧里可见，
 // 且与"帧内一个快照"的其他量同一时机定格。
 //
 // 【它不改变任何行为】`--control` 没给 ⇒ `enabled_` 为假 ⇒ poll 与 flush 立刻返回。
 // ⇒ 判据成立：关掉通道，引擎行为逐字节相同。
 //
 // 【poll 做两件事】收新命令（每帧最多 8 条进队列）+ 重置"本帧已取"计数；
 // flush_state 只在"有未写的 publish 且距上次 ≥ 1 s"时写 state.json。
    eng.control().poll();
    eng.poll_reload_commands();
    eng.control().flush_state(static_cast<double>(steady_now_ns()) / 1.0e9);

 // ---- 第 9 件：音频----
 //
 // 【为什么在阶段 1】与"帧内一个快照"同一时机：
 // pump = 排空**回执**（voice 结束 / 被抢）⇒ is_active 的状态与本帧一致；
 // commit= 把上一帧钩子里排队的命令压入发送队列 ⇒ 延迟 ≤1 帧。
 //
 // 【它不碰音频线程】两条无锁队列是唯一通道；这里只做主线程侧的两步。
    eng.audio().pump();
    eng.audio().commit();

 // 【本帧的报错上下文从这里开始】
    fs.current_hook = nullptr;

}

// =============================================================================
// 阶段 2：帧率控制
// =============================================================================

void frame_pipeline::stage_rate_control(engine& eng) {
 // 本阶段**只做节拍记账**。"等"已经搬到帧外（`wait_until_next_due`，2026-11-14）。
 //
 // 【为什么"等"必须搬走】那段等待里的 `SDL_WaitEventTimeout` 正是窗口拖拽的
 // 模态循环会抢走控制权的地方；只要它还在 `run_one_frame` 内部，
 // 从消息路径补跑一帧就必然是**12 阶段管线重入**。
 //
 // 【它不属于渲染管线，因此休眠时照常运行】
 // 已定：帧率控制也不休眠、不降帧率 ⇒ 引擎不预设最小化时该跑多快。
 // 已知代价：最小化的游戏仍按目标帧率全速跑逻辑（占一个核）。
 //
 // 【它做两件事】
 // ① **消费一格节拍**（`++rate_ticks_`）—— 本帧占用了时间线上的一格。
 // ② **留下"下一帧该在何时开始"**（`next_due_ns_`）—— 帧外的等待读它。
 //
 // 【睡觉的判据】目标帧率的**值与上一帧不同**（不是"被调用了"）。
 // 睡掉的是一整套基于"目标恒定"的假设 —— 现阶段就是长期纠偏那一条。
 //
 // 【已定的规则】纠偏只纠正**非故意**的偏差；故意的偏差（作者改了目标）
 // 交给作者，引擎不得纠正。

 // 【默认不等待】下面每一条"重置基准"的分支都直接 return，
 // 所以先把 due 清掉 —— 否则目标帧率非法/变更时，帧外会拿着**过期的 due** 去等。
    next_due_ns_ = 0.0;

    const double target = eng.get_target_fps();
    if (!(target > 0.0)) {
 // 目标帧率不是正数 ⇒ 帧间隔无定义。这不是"数据矛盾"（那由跨边界校验
 // 拦下），而是引擎还没拿到一个合法初值。此时**不睡也不等**，
 // 只把基准清掉，让下一次拿到合法值时重新建立基准。
        last_target_fps_ = target;
        rate_epoch_ns_ = 0;
        return;
    }

    const long long now = steady_now_ns();

    const bool target_changed = (target != last_target_fps_);
    if (target_changed) {
 // 目标变了 ⇒ 重设基准，且本帧不做纠偏。
        rate_epoch_ns_ = now;
        rate_ticks_ = 0;
        last_target_fps_ = target;
        return;
    }

    if (rate_epoch_ns_ == 0) {
 // 还没有基准（第一帧，或刚从一个非法目标恢复）⇒ 建立它。
        rate_epoch_ns_ = now;
        rate_ticks_ = 0;
 // 【顺手把下一帧的 due 定下来】否则下一帧会立刻开跑（少等一个间隔），
 // 之后的时间线才对齐 —— 那会让启动那一秒的 fps 读数偏高一点点。
 // 这里定成"一个间隔之后"，从第一帧起就对齐。
        next_due_ns_ = static_cast<double>(now) + 1'000'000'000.0 / target;
        return;
    }

 // 本帧应当开始的绝对时刻 = 基准 + 已消费的节拍数 × 帧间隔
 //
 // 【2026-10-15 已决（候选甲）：节拍挂墙钟自计数，不挂 frame_count】
 // 旧式是 `基准 + (frame_count − 基准帧号) × 间隔`，而 frame_count 在
 // 暂停期间停住 ⇒ due 冻住 ⇒ 空转 + 恢复后爆发追赶。
 // 新式每次进入本阶段消费一格节拍：暂停期间 due 照常前进 ⇒ 照常等待
 // （不空转）；恢复时 due 就在眼前 ⇒ 无追赶。
 // 两条时间线由此显式分开：
 // · 逻辑时间线 = frame_count（暂停不推进 ⇒ 游戏时间冻结）
 // · 真实时间线 = rate_ticks_（循环在跑就推进 ⇒ 节拍不停）
 // ⚠️ 而"加载卡顿被当成落后并追赶"**不在本改动范围**：那是"一帧真长了
 // ⇒ due 落到过去 ⇒ 后续帧不等”的同一形态，它已被接受
 // 改动它需要“这次落后是故意的”这个信号，另行决策。
    ++rate_ticks_;
    const double interval_ns = 1'000'000'000.0 / target;

 // 【算的是下一帧的 due，不是本帧的】（2026-11-14 搬出等待时改的）
 // 阶段 2 跑的时候本帧已经开始了（阶段 1 跑过了），
 // "本帧该何时开始"对任何人都没用了；有意义的只有"下一帧该何时开始"。
 // 帧外的 `wait_until_next_due` 读的就是这一个。
    next_due_ns_ =
        static_cast<double>(rate_epoch_ns_) +
        static_cast<double>(rate_ticks_ + 1) * interval_ns;

 // 【落后时不补】已定的"追赶/落后降级"属于那一套基于"目标恒定"的逻辑，
 // 而它们目前没有实现。现阶段的行为是"落后就少等或不 wait"，
 // 由绝对时间线**自然**把节奏拉回去，而不做额外的帧间隔微调。
}

bool frame_pipeline::frame_due() const noexcept {
 // 【0 = 没有 due】首帧 / 目标变更 / 目标非法 ⇒ 不知道节拍，不补跑。
    if (!(next_due_ns_ > 0.0)) {
        return false;
    }
    return static_cast<double>(steady_now_ns()) >= next_due_ns_;
}

void frame_pipeline::wait_until_next_due(engine& eng) {
    const double now = static_cast<double>(steady_now_ns());
    const double wait_ns = next_due_ns_ - now;

    if (!(wait_ns > 0.0)) {
 // 【不等也要泵】搬之前只有"需要等"的帧才泵事件（落后帧完全不泵 ⇒ 事件积压）。
 // 这里补上这一泵，于是"每帧恰好泵一次"成立。
        eng.window().pump_events();
        return;
    }

 // 【为什么不是 sleep_for —— 这一处修的是"窗口未响应"】
 // 用 sleep 过去的话，这段（一帧里最长的一段）里**一条窗口消息都不被
 // 处理** ⇒ 系统把窗口判成"未响应"：光标变成加载圈，拖动边框与点标题栏
 // 都失效。
 // ⇒ 换成"等消息或超时"：消息在整帧的空闲时间里都被处理，而等待的
 // 总时长不变 ⇒ 帧率不受影响，窗口始终是活的。
 //
 // 【毫秒怎么取】向上取整到至少 1 ms：SDL 的等待接口以毫秒为单位，
 // 而 240 Hz 的间隔只有 4.17 ms。取整会略多等一点点，而绝对时间线
 // 会在后续帧把这点误差吸收掉（不累积）。
    int wait_ms = static_cast<int>(wait_ns / 1'000'000.0);
    if (wait_ms < 1) {
        wait_ms = 1;
    }
    eng.window().wait_events_or_timeout(wait_ms);
}

// =============================================================================
// 阶段 3：事件消费
// =============================================================================

void frame_pipeline::stage_event_consume(engine& eng) {
 // 本阶段消费窗口事件位。
 //
 // 【硬约束：真正的脚本调用只发生在这里】
 // 窗口消息路径（platform::window::pump_events）只写位，不调脚本。
 // 理由：消息路径可以在任意时刻被系统调用；若它直接回调脚本，脚本就会
 // 在任意时刻重入引擎状态 —— 那是崩溃的经典来源，也是"引擎只用 13 个
 // 钩子回调脚本"这条封闭性被破坏的方式。
 //
 // 【休眠时必须继续】
 // 否则恢复事件收不到，会永远休眠。这是本阶段在休眠分支里被保留的理由。
    const auto ev = eng.window().consume_events();

 // 平台的关闭请求 → 请求退出。它不是直接退出：请求在帧末应用，
 // 与实际退出之间隔着关闭序列。
    if (has_bit(ev, platform::event_bits::close_requested)) {
        eng.request_exit();
    }

 // ---- 焦点 → 暂停源（2026-10-18 落地）----
 //
 // 【为什么读**状态**而不是读事件位】
 // `ev` 是"本帧发生过什么"，取走即清零；而失焦之后的每一帧都仍然失焦，
 // 但只有那一帧有 focus_lost 事件。
 // ⇒ 若用事件位推状态，失焦的下一帧就会"自动恢复"（暂停只闪一下）。
 // ⇒ 所以这里读 `window::has_focus()`（状态），事件位只用于别的用途。
 //
 // 【为什么是 hold/release 而不是 request_pause/resume】
 // 同一个引擎状态可能有多个源（脚本暂停 / 失焦 / 拖拽）。
 // 失焦只按自己那一位 ⇒ 玩家自己按的暂停不会被它解除，
 // 窗口切回来时也只松开自己那一位 ⇒ 仍处于玩家暂停。
 // 【只在**转移**时动它 + 记一条日志】
 // hold/release 本身是幂等的，但"每帧都调用"会让"焦点源到底有没有生效"
 // 变得不可观测。这条转移日志是排查"暂停了却不知道谁按的"时唯一的证据。
    const std::uint8_t focus_bit =
        static_cast<std::uint8_t>(pause_source::focus);
    if (!eng.focus_pause()) {
 // 【`--no-focus-pause`：启动期就关掉了这一源】
 // 防御性地把可能残留的位松开（正常情况下它从未被按过），
 // 并且**不读** `has_focus()` —— 读它也没有意义了。
        if ((eng.pause_sources() & focus_bit) != 0) {
            eng.release_pause(pause_source::focus);
            log::record("engine", "焦点源", "松开（--no-focus-pause 已禁用该源）");
        }
    } else if (eng.window().has_focus()) {
        if ((eng.pause_sources() & focus_bit) != 0) {
            eng.release_pause(pause_source::focus);
            log::record("engine", "焦点源", "松开（窗口重新获得焦点）");
        }
    } else {
        if ((eng.pause_sources() & focus_bit) == 0) {
            eng.hold_pause(pause_source::focus);
            log::record("engine", "焦点源", "按住（窗口失去焦点）");
        }
    }

 // ---- 焦点钩子接线（2026-11-16，）----
 //
 // 【为什么在这里】焦点变化就是这几种事件位里被消费的那一个；
 // 放到别处（比如帧末）会让"事件→脚本"的顺序变得不可解释。
 //
 // 【只在该位出现的那一帧调】`ev` 是"本帧发生过什么"，取走即清零。
 // ⇒ 失焦后持续失焦的每一帧**不会**重复调用 —— 与"暂停源读状态"是两件事：
 // 源读状态（否则会闪），钩子读事件（否则每帧都调）。
    if (has_bit(ev, platform::event_bits::focus_gained)) {
        script::call_hook(eng.script(), script::hook::on_focus_gain, fs);
    }
    if (has_bit(ev, platform::event_bits::focus_lost)) {
 // 【清键放在钩子之前】先让"按住"归零，再通知脚本 ——
 // 否则脚本在 on_focus_lose 里读到的还是"按住"（见）。
        eng.input_system().clear_down();
        script::call_hook(eng.script(), script::hook::on_focus_lose, fs);
    }

 // 【其余位的处置】
 // · resized ⇒ 什么都不做（本帧的变换已用旧尺寸算过；这是已知行为）
 // · close_requested ⇒ **不是直接退出**，而是走与 wag.engine.exit() 同一条
 // 通道：请求退出，在帧末生效
 // · 输入事件（键/鼠/轮）⇒ 由阶段 4 的批次交给 on_event（每帧一次，）
    (void)ev;
}

// =============================================================================
// 阶段 4：输入采样
// =============================================================================

void frame_pipeline::stage_input_sample(engine& eng) {
 // **无条件执行，与状态无关。**
 //
 // 【为什么暂停时也要采】因为暂停菜单需要读输入。若暂停时不采样，菜单就
 // 读不到按键与鼠标 —— 那是 Sub 那种"暂停时输入冻结"的形态，而这里是
 // 刻意避免它的。
 //
 // 【采样什么】把阶段 2 泵入的事件（由 window 的 sink 喂给 input）冻结成
 // 本帧快照：Down/Press/Release + 鼠标 DIP + 滚轮增量。
 // P 占帧的裁决也在这里做（I2）—— 因为它需要整帧的事件。
 //
 // 【为什么传 viewport 的变换】
 // 鼠标位置在事件里是**窗口坐标**；转成逻辑坐标（DIP）要用本帧的
 // transform。它在阶段 1 的 recompute 里定稿 ⇒ 阶段 4 读到的就是本帧值。
 // ⚠️ 最小化帧的 transform 可能退化（m00/m11 == 0）；是否换算由 input
 // 内部判断（退化时保留上次 DIP，不除零）。
    eng.input_system().sample(eng.viewport().get_transform());

 // ---- on_event：每帧一次批量（2026-11-16，；）----
 //
 // 【为什么在 sample 之后】本帧的事件批次就是在 sample 里冻结的；
 // 先采样再通知，脚本看到的"事件"与它查到的"状态"才是同一份真相。
 //
 // 【为什么每帧最多一次】帧内只应有一个快照；每事件一次会让脚本在帧中途
 // 被叫 N 次，而那时它读到的其它状态已经不属于"那一批事件"的时刻了。
 //
 // 【没有事件就不调】否则每帧都白跨界一次。
    fs.events = &eng.input_system().frame_events();
    if (!fs.events->empty()) {
        script::call_hook(eng.script(), script::hook::on_event, fs);
    }
 // 归属说明：`fs.events` 指向 input 内部的向量，**本帧内有效**；
 // 下一帧 sample 会把它换掉（见 frame_state::events 的注释）。
}

// =============================================================================
// 阶段 5：Ext 逻辑
// =============================================================================

void frame_pipeline::stage_ext_logic(engine& eng) {
 // Ext 是叠加层，它在逻辑上**先跑**。
 //
 // 【为什么先跑】因为 Ext 的结果可能要被主层读到（例如调试信息里的
 // "跳到某个节点"）。先跑 Ext 让主层能读到本帧的结果。
    fs.current_hook = script::hook_name(script::hook::on_frame_ext);
    script::call_hook(eng.script(), script::hook::on_frame_ext, fs);
    fs.current_hook = nullptr;
}

// =============================================================================
// 阶段 6：主逻辑
// =============================================================================

void frame_pipeline::stage_main_logic(engine& eng) {
 // 【判据是 fs.mode，不是 eng.get_state()】
 // 这一条是整个帧结构的核心修正：本帧该跑哪一支由**帧开始时的快照**
 // 决定。若用活状态，那么"按下暂停的那一帧"会跑 Pause 分支，而世界
 // 从未被画出来。
    if (fs.mode == frame_mode::normal) {
        fs.current_hook = script::hook_name(script::hook::on_frame_main);
        script::call_hook(eng.script(), script::hook::on_frame_main, fs);
    } else {
        fs.current_hook = script::hook_name(script::hook::on_frame_pause);
        script::call_hook(eng.script(), script::hook::on_frame_pause, fs);
    }
    fs.current_hook = nullptr;

 // //：碰撞在"逻辑之后、渲染之前"执行（阶段 6 的 Main 分支末尾）。
 // 【为什么在这里】本帧逻辑已经把位置/判定写最终；宽相与窄相看到的是最终状态。
 // 【Pause 分支】不检测，只丢弃本帧声明（unref 回调），否则请求会跨帧堆积。
    if (fs.mode == frame_mode::normal) {
        script::run_collisions(eng);
    } else {
        script::drop_collisions(eng);
    }
}

// =============================================================================
// 阶段 7：渲染开始
// =============================================================================

void frame_pipeline::stage_render_begin(engine& eng) {
 // 【把逻辑时间交给渲染器】着色器的 `u_time` 就是它。
 // 【为什么在这里】它是本帧常量，与 pixel_to_ndc_ 同一时机；
 // 而且 `frame_count` 只在正常帧推进 ⇒ 暂停时自动冻结（已定）。
 // 【步长】5.2 已把时间颗粒**锁死**为 1/60 秒；
 // 它不在 frame_state 里，而 frame_info::dtg 的默认值就是它。
    eng.renderer().set_logic_frame(fs.frame_count, 1.0 / 60.0);

 // 清屏，然后调 on_render_begin。
 //
 // 【这个钩子的语义（已定）】清屏之后、绘制任何容器之前。
 // 因此它是"绑定渲染目标 / 设置本帧渲染状态"的**唯一正当位置**。
 //
 // 【四个入参全部是"本帧常量"】它们都在阶段 1 定下，因此本阶段不需要
 // 再读一次窗口或视口 —— 那会让"本帧一个值"出现第二个来源。
 //
 // 【第三个参数是 get_viewport()，不是 get_render()】
 // 渲染层要用它设视口与 scissor（"内容画在屏幕的哪一块"）。
 // 那个矩形由策略决定：letterbox 时它是"等比装进窗口"的居中矩形,
 // stretch 时它等于整个窗口。
 // 若传 render_（世界的可见范围），渲染层只能自己推一遍 —— 而那条推导
 // 会混进两个方向不同的缩放，于是视口永远是"铺满"。
 //
 // 【第四个参数是"本帧几何整包"，来自阶段 1 的快照】
 // **不要**在这里再读一次窗口：那会让"本帧一个值"出现第二个来源，
 // 而且允许 DIP 与像素来自两个不同瞬间（实测事实 15 就是那样发生的）。
 // 也不能用 dip × dpi 算像素：那个乘积与真实像素有取整余量，
 // 而视口居中要按真实尺寸算，否则内容偏 (B−A)/2。
 //
 // ⚠️ 这里把 `fs.geom` 那几个扁平的数**装回**平台层的 `frame_geometry`。
 // 为什么快照是扁平的、而不是直接存那个类型，见 `frame_state::geom` 的说明
 // （内核的帧状态不该依赖平台实现）。**装配只在这一个地方做。**
 //
 // ⚠️ 第二个参数以前是 `fs.window_dip`，**本轮删掉了**：
 // 窗口尺寸现在就在 `geom.dip` 里 ⇒ **同一个量不留两个来源**
 // （两个来源会漂移，而那正是实测事实 15 的形态）。
    platform::window::frame_geometry geom{};
    geom.dip = platform::dip_size{fs.geom.dip_w, fs.geom.dip_h};
    geom.pixel = platform::dip_size{fs.geom.pixel_w, fs.geom.pixel_h};
    geom.scale = fs.geom.scale;
    eng.renderer().set_render_space(eng.viewport().get_render());
    eng.renderer().begin_frame(eng.viewport().get_transform(),
                               eng.viewport().get_viewport(), geom);

 // 【绘制相位门·开】从本阶段起允许收集绘制（场景阶段）；
 // 阶段 9 结束时关闸（见 stage_ext_render_and_finish）。
    eng.set_draw_phase(true, render_stage::scene);
    eng.renderer().set_collect_stage(draw::collect_stage_scene);

    fs.current_hook = script::hook_name(script::hook::on_render_begin);
    script::call_hook(eng.script(), script::hook::on_render_begin, fs);
    fs.current_hook = nullptr;
}

// =============================================================================
// 阶段 8：场景渲染
// =============================================================================

void frame_pipeline::stage_scene_render(engine& eng) {
    if (fs.mode == frame_mode::normal) {
        fs.current_hook = script::hook_name(script::hook::on_render_main);
        script::call_hook(eng.script(), script::hook::on_render_main, fs);
    } else {
 // 【B 方案（2026-10-22）：引擎不再预设暂停背景】
 // 旧形态是"先画冻结快照、再调 on_render_pause"；现在删掉那一笔，
 // 暂停画面完全由 on_render_pause 决定：
 // · 想要冻结世界 ⇒ 在钩子里 emit_containers（逻辑已停，位置不变）；
 // · 想要纯菜单/纯色 ⇒ 什么都不画，或画自己的背景。
 // ⇒ 引擎不再替作者决定"暂停时该看到什么"。
        fs.current_hook = script::hook_name(script::hook::on_render_pause);
        script::call_hook(eng.script(), script::hook::on_render_pause, fs);
    }
    fs.current_hook = nullptr;
}

// =============================================================================
// 阶段 9：Ext 渲染与收尾
// =============================================================================

void frame_pipeline::stage_ext_render_and_finish(engine& eng) {
 // 【顺序固定：先 Ext，后 end】
 // 因为 on_render_end 的语义是"所有容器绘制完毕"，而 Ext 的绘制也是
 // 容器（它画在最上面）。所以收尾必须在 Ext 之后。
 //
 // 【绘制相位门·切到 ext】Ext 画在所有内容之上 ⇒ 它的"阶段"排序键
 // 与场景不同（ext > scene）⇒ Ext 的绘制在排序后自然落在最上层。
    eng.set_draw_phase(true, render_stage::ext);
    eng.renderer().set_collect_stage(draw::collect_stage_ext);

    fs.current_hook = script::hook_name(script::hook::on_render_ext);
    script::call_hook(eng.script(), script::hook::on_render_ext, fs);
    fs.current_hook = nullptr;

    fs.current_hook = script::hook_name(script::hook::on_render_end);
    script::call_hook(eng.script(), script::hook::on_render_end, fs);
    fs.current_hook = nullptr;
 // 【这个钩子的正当用途】解绑渲染目标、后处理、截图。

 // 【绘制相位门·关】渲染钩子全部结束 ⇒ 收集结束。
 // 此后的 end_frame（排序 + 提交）与快照都是引擎自己的动作，
 // 不再接受任何脚本绘制 —— "引擎只在渲染相位收集"由这里结构性地保证。
    eng.set_draw_phase(false, render_stage::none);
}

// =============================================================================
// 阶段 10：帧快照
// =============================================================================

void frame_pipeline::stage_snapshot(engine& eng) {
 // 【B 方案（2026-10-22）：快照改为**显式请求**，不再每帧自动拍】
 // · 脚本调用 wag.draw.capture_snapshot() 置位；
 // · 阶段 10 消费一次（拍完清零）；
 // · 不请求 ⇒ 不拍 ⇒ 正常帧没有那 8.3 MB/帧的全屏 blit。
 //
 // 【为什么仍然在阶段 10】拍的是"本帧的完整渲染结果"（渲染之后）。
 // 【休眠帧不会走到这里】门在 run_one_frame 里；否则最小化会把快照清空。
    if (!eng.renderer().snapshot_requested()) {
        return;
    }
    eng.renderer().capture_staged_snapshot();
    eng.renderer().clear_snapshot_request();
    fs.staged_snapshot_valid = true;
}

// =============================================================================
// 阶段 11：状态切换与帧末结算
// =============================================================================

void frame_pipeline::stage_state_apply(engine& eng) {
 // 【它不做任何 GPU 操作】只做字段赋值。
    const bool changed = eng.apply_pending_state();

 // 【B 方案（2026-10-22）：不再取用冻结画面】
 // 暂停背景改由 on_render_pause 决定，引擎不再预设"暂停时显示最后一帧"。
 // frozen 那一套（adopt / draw_frozen_snapshot）保留待清理/待显式冻结 API；
 // 因此 changed 现在只在状态机内部有意义，这里显式忽略。
    (void)changed;

 // 【不确定代码 · 待定】取消退出
 // 已定的退出通道是 wag.engine.exit()，它在帧末生效。但"退出请求能否
 // 被撤回"这一条**没有决策过**。在收口之前，退出请求一旦置上就不再清除。
}

// =============================================================================
// 阶段 12：呈现
// =============================================================================

void frame_pipeline::stage_present(engine& eng) {
 // 【必须在最后】
 // 它是唯一把结果送出去的阶段。任何在它之后做的事都没机会被看到。
 //
 // 【它不等垂直同步】vsync 默认关闭（见1.4）。因此本调用不阻塞。
 // 【探针】效果竖带画在这里，而不是 end_frame 里 —— 这是被实测逼出来的：
 // 探针采样的是 staged 快照，而快照在阶段 10 拍摄。若探针在拍摄**之前**画，
 // 快照就会包含探针自己的输出 ⇒ 下一帧再采样它 ⇒ **正反馈**，几帧内烂成噪声。
 // ⇒ 放到快照之后：快照永远是"干净的场景"，探针只是最后覆盖上去的一层。
    eng.renderer().draw_shader_probe();
    eng.window().present();
}

} // namespace wag::time
