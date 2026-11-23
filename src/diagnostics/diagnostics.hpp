// =============================================================================
//  diagnostics/diagnostics.hpp
//
//  模块：diagnostics（**游戏开发期的诊断导出**）
//  命名空间：wag::diagnostics
//
//  【它与 src/devtools/ 是两个目录、两个受众】
//      devtools    ：**引擎作者**用它验证引擎（时序打点、像素探针）
//      diagnostics ：**用引擎做游戏的人**用它看自己的游戏（性能、错误）
//
//  这个区分不是洁癖，是「契约权重」不同：
//      devtools 的输出不是契约（trace.csv 改格式无所谓）；
//      diagnostics 的输出**是契约**（字段名与版本号被外部控制台解析）。
//  ⇒ 所以前者不该编进发布版，后者**可以**（默认关）。
//
//  【准入判据：三条全中才准进本目录】
//      ① 只输出，不读取（不消费引擎状态去做判断）
//      ② 只写文件（不写屏幕、不弹窗、不走网络）
//      ③ 不改行为（帧序 / 校验 / 渲染 / RNG / 退出码都不许变）
//  判据 ③ 的可执行形式：**把本目录的代码全部删掉，发布版行为必须逐字节相同。**
//
//  【当前边界】
//    目前只有开关，不碰文件系统 —— 目录要到真有东西写的时候才建。
//    “不开 --diagnostics 就连空目录都不建”是发布前必须保持的行为。
//
//  【两条必须堵死的依赖边】（在 CMake 里用“不链接”实现，见本目录的 CMakeLists）
//      wag_diagnostics ──✗──► wag_devtools
//      wag_devtools    ──✗──► wag_diagnostics
//  目录分开而依赖图没分开，等于没分。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace wag::diagnostics {

/// 本次运行是否开启诊断导出（命令行 `--diagnostics`）。**默认关。**
///
/// 【为什么默认关，而不是"默认开、发布时关"】
///   因为"安全"这件事只能靠默认值兜：默认关 ⇒ 谁都不传 ⇒ 不写；
///   而"入口唯一"兜不住（一句手敲的参数就能破）。
[[nodiscard]] bool enabled() noexcept;

/// 由 main 在解析命令行之后设置**一次**。
///
/// 【为什么是"设置一次"而不是"可反复切换"】
///   它不是运行期配置，是**本次运行的性质**。允许中途改就会出现
///   "前半段写了、后半段没写"的半份导出 —— 那种数据比没有更误导。
void set_enabled(bool on) noexcept;

// -----------------------------------------------------------------------------
//  errors.jsonl
//
//  一行一个 JSON 对象；字段名与版本号由外部控制台解析，因此一旦发布就保持稳定。
//  只维护这一份错误记录，不再另写人类可读版；`sentence` 就是给人读的那句。
// -----------------------------------------------------------------------------

/// 一条错误事件的**视图**（不拥有任何字符串）。
///
/// 【生命周期】只在 `write_error_line` 调用期间有效 —— 它指向调用者的缓冲。
/// 【为什么不用 `error_value`】那会让本模块反过来依赖 `wag_error`，
///   而 `error.cpp` 将来要调本模块写记录 ⇒ 形成**模块级循环依赖**。
///   用视图隔开之后，依赖方向永远是 `error → diagnostics`，单向。
struct error_record {
    std::string_view level;    ///< record / warn / raise / terminate
    std::string_view cls;      ///< error_class 的名字；未知留空
    std::string_view where;
    std::string_view got;
    std::string_view want;
    std::string_view sentence; ///< 给人读的那一句（永远有）
    std::string_view file;     ///< **相对安装根**的文件名；空 ⇒ JSON `null`
    int line = 0;              ///< 脚本行号；<= 0 ⇒ JSON `null`
    /// 这个文件在**安装根之外**（只写 basename）⇒ JSON 里加 `"external":true`。
    bool external = false;
};

/// 设置输出根（**exe 所在目录**）。本模块自己拼 `debug/errors.jsonl`。
///
/// 【为什么由 main 给】锚点是 **exe 所在目录**，不是进程 CWD
///   （CWD 随启动方式变化，exe 位置是唯一稳定锚点）。
void set_output_dir(std::string dir);

/// 由帧管线每帧更新一次（记录里的 `frame` 字段）。
///
/// 【关掉时它也只是存一个整数】不建目录、不碰文件 ⇒ 不违反"删掉本模块行为不变"。
void set_frame(std::uint64_t frame) noexcept;

/// 写一行 `errors.jsonl`。**门禁关闭时什么都不做**（不建目录、不开文件）。
///
/// 【三条纪律】
///   ① **行级 flush**：不 flush，控制台永远看不到；而且 `abort()` 不跑 `atexit`，
///      缓冲区会直接蒸发。
///   ② **永不抛**：它会被 `fail_*` 在"即将终止/抛错"的路径上调用，
///      那里不允许异常穿过。分配失败 ⇒ 直接终止（对诊断导出是可接受的）。
///   ③ **首次写才建目录**：目录不可写时只关导出、不换目录；
///      失败后不再重试，也不会去别的地方写。
void write_error_line(const error_record& r) noexcept;

// -----------------------------------------------------------------------------
//  会话标识（session.json）、低频导出（pool_log.jsonl）、逐帧导出（perf_log.jsonl）
//
//  session.json 记录本次运行的标识；两个 jsonl 的每一行都带同一个 session id。
// -----------------------------------------------------------------------------

/// 一次会话的标识（启动期给一次）。
struct session_info {
    std::string_view project;        ///< 工程目录名（今天恒为 `Luminosa`）
    std::string_view engine_version; ///< 软件版本；必须来自**构建配置的同一个变量**
    // 光照配置
    std::uint32_t light_cap = 4096;   ///< 光源硬上限
    std::uint32_t band_cap = 64;      ///< 带数量上限
    std::uint32_t fill_budget_k = 32; ///< 填充预算倍数
};

/// 启动期调用一次：记下 `pid` / `started_at`，并写 `debug/session.json`。
///
/// 【`started_at` 兼作会话 id】三个 `.jsonl` 每行的 `session` 就取它。
/// 【为什么是“覆盖”不是“追加”】它描述这一次运行；历史会话靠 jsonl 每行的
///   `session` 自己带着，不靠这个文件保存。
void begin_session(const session_info& info);

/// 本次会话 id（= `started_at`，Unix 毫秒）。**未开始时为 0**。
[[nodiscard]] std::int64_t session_id() noexcept;

/// 池子占用的一行（`pool_log.jsonl` 用）。
struct pool_row {
    std::string_view name;      ///< 池名（池必须有名字）
    std::uint32_t capacity = 0; ///< 容量（固定 32767，不是可配置值）
    std::uint32_t active = 0;   ///< 当前存活数（含“已标 dead 未回收”的槽）
};

/// 写一行 `pool_log.jsonl`（建议 **1–2 Hz**，不要逐帧）。
void write_pool_line(const pool_row* rows, std::size_t count) noexcept;

/// 一帧的性能样本（`perf_log.jsonl`）。
///
/// 【谁负责测量】调用方（帧管线）。本模块只负责**格式化与写出** ——
///   它不该知道“睡眠之后”这种时序事实。
struct perf_sample {
    double target_fps = 0.0;      ///< **声明值**（`engine::get_target_fps()`），不是实测
    std::uint32_t containers = 0; ///< 帧末存活对象数（各池之和）
    std::uint32_t draw_calls = 0; ///< **批次数**（`glDrawElements` 次数），不是对象数
    double frame_ms = 0.0;        ///< 这一帧的**工作耗时**（不含限速睡眠）
    // 光照
    std::uint32_t light_lights = 0;   ///< 本帧最大光源数
    std::uint32_t light_occluders = 0;///< 遮挡者存活
    std::uint32_t light_commits = 0;  ///< 本帧合成次数
    double light_fill_ratio = 0.0;    ///< 光图填充 / 光图像素
};

/// 写一行 `perf_log.jsonl`。**门禁关闭时什么都不做**。
///
/// 【flush 粒度】**每 60 行 flush 一次**（约 1 秒 @60 Hz），
///   换取“崩溃最多丢 1 秒”。逐行 flush 是 60 次/秒的无谓 I/O。
/// 【容量上限】**64 MB 按字节，到顶就停写**；
///   调用方用 `perf_capped()` 查一次并记日志（本模块不依赖日志）。
void write_perf_line(const perf_sample& s) noexcept;

/// `perf_log.jsonl` 是否已经**因容量上限而停写**。
///
/// 【为什么由调用方记日志】本目录的准入判据是"只输出、零模块依赖"；
///   直接依赖 `wag_log` 会让这条判据失效。⇒ 这里只暴露状态，日志由帧管线打。
[[nodiscard]] bool perf_capped() noexcept;


} // namespace wag::diagnostics
