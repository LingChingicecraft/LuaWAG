// =============================================================================
//  wag/error/error.cpp
//
//  错误架构的实现。
//
//  【本文件只管"分类与校验"，不管"行为"】
//      行为（记录 / 通知 / 终止）由 wag/log/log.cpp 的决策点负责。
//      这里把"这是什么类别的错"与"这个值合法吗"回答清楚，然后交给决策点。
//
//  【为什么把校验放在这里而不是在交界处就地写 if】
//      因为已定的校验粒度是：
//
//          标量参数逐个查；
//          结构化 / 数组参数在交界处**整批查一次**。
//
//      把这套粒度写成一处的函数，才能保证每一处的粒度一致。
// =============================================================================

#include "error/error.hpp"

#include "diagnostics/diagnostics.hpp"
#include "platform/paths.hpp"
#include "log/log.hpp"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace wag {

namespace {

// -----------------------------------------------------------------------------
//  unreachable_terminate —— 让控制流显式终止
// -----------------------------------------------------------------------------
//
// 【为什么需要它 —— 这一条修的是一个真实的警告】
//   本文件里有两个函数声明为 `[[noreturn]]`：
//       fail_boundary(...)
//       fail_internal(...)
//   而它们的函数体最后一句是 `log::terminate(...)`。
//
//   问题：**编译器不会无条件信任另一个翻译单元里的 `[[noreturn]]`**。
//     它保守地假设"那个调用可能返回" ⇒ 于是走到 `fail_boundary` 的定义末尾
//     ⇒ 而那个函数声明了不返回 ⇒ GCC 报
//         warning: 'noreturn' function does return
//
//   原先的代码用 `return;` 来"让编译器高兴" —— 而那是错的：
//     ① 它自己就证明了"这个函数会返回"
//     ② 于是 `[[noreturn]]` 的声明与实现矛盾
//
//   ⇒ 正确做法是**给控制流一个真正的终止点**。
//     用标准库的 `std::abort()`：它保证不返回，且不需要平台 API。
//     （`std::abort()` 不展开栈、不调析构函数 —— 而 `[[noreturn]]` 的含义正是
//       "不要指望它之后还有代码"，所以这里它是对的。）
[[noreturn]] inline void unreachable_terminate() noexcept {
    std::abort();
}

} // namespace

// =============================================================================
//  名字表
// =============================================================================

const char* class_name(error_class c) noexcept {
    switch (c) {
    case error_class::boundary_validation:
        return "boundary_validation";
    case error_class::internal_invariant:
        return "internal_invariant";
    case error_class::resource_failure:
        return "resource_failure";
    case error_class::shutdown_error:
        return "shutdown_error";
    case error_class::script_error:
        return "script_error";
    case error_class::api_misuse:
        return "api_misuse";
    }
    return "?";
}

// =============================================================================
//  三个入口
// =============================================================================

// =============================================================================
//  error_value 的字段 → 两段文本
//
//  【为什么抽出来】`fail_boundary` 与 `fail_api_misuse` 报的是同一种结构
//    （error_value），只是级别不同。若各写一遍拼装，同一种错误值会被拼成
//    两种文本 —— 而"同一段知识写两处"正是我们已经吃过亏的形状。
// -----------------------------------------------------------------------------

namespace {

/// 主句：优先用调用方给的完整句子，否则由 where + 通用后缀拼出。
std::string subject_of(const error_value& e) {
    if (!e.sentence.empty()) {
        return std::string(e.sentence);
    }
    std::string s(e.where);
    if (!e.got.empty() || !e.want.empty()) {
        s += " 的值不合法";
    }
    return s;
}

/// 细节：got = … , want = …
std::string detail_of(const error_value& e) {
    std::string d;
    if (!e.got.empty()) {
        d += "got = ";
        d.append(e.got);
    }
    if (!e.want.empty()) {
        if (!d.empty()) {
            d += ", ";
        }
        d += "want = ";
        d.append(e.want);
    }
    return d;
}

} // namespace

// -----------------------------------------------------------------------------
//  交还通道的记账
//
//  【为什么放在 error 而不是 script】"有没有可交还的调用者"正是 fail_boundary
//    要问的问题；把计数放在问问题的一侧，script 只负责加/减。
//  【为什么不需要原子】所有加/减都发生在同一条线程（引擎循环+脚本都在主线程）。
// -----------------------------------------------------------------------------
namespace {

int g_hand_back_depth = 0;
boundary_raiser g_raiser = nullptr;

/// 暂存的、尚未被捕获的 raise（E11 ③b）。
///
/// 【为什么字段是 std::string 而不是 string_view】
///   `error_value` 的字段指向调用者的临时缓冲，而 `longjmp` 不会执行析构，
///   那些缓冲在捕获点已经不存在了 ⇒ 必须在抛之前复制下来。
struct pending_raise {
    bool active = false;
    error_class cls = error_class::boundary_validation;
    std::string where;
    std::string got;
    std::string want;
    std::string sentence;
    std::string file;
    int line = 0;
    /// 文件在安装根之外（只留 basename）⇒ errors.jsonl 里加 "external":true
    bool external = false;
};

pending_raise g_pending;

/// 终止前回调（由 main 注册；nullptr = 没有）。
void (*g_before_terminate)() = nullptr;

} // namespace

void enter_hand_back() noexcept {
    ++g_hand_back_depth;
    // 【新的一次"引擎→脚本"，上一轮没落盘的 pending 已经失效】
    //   为什么必须在这里清：脚本自己的 `pcall` 可以接住 raise，而那种接住
    //   我们**看不见**（它完全发生在 Lua 内部）⇒ 若不在这里清，
    //   一条被接住的 raise 会挂到很久以后的某次 pcall 失败上，写成一条假记录。
    //   ⇒ 清在"进入"这一侧，陈旧窗口就最多只有一次引擎→脚本调用。
    g_pending.active = false;
}

void leave_hand_back() noexcept {
    if (g_hand_back_depth > 0) {
        --g_hand_back_depth;
    }
}

void set_boundary_raiser(boundary_raiser r) noexcept { g_raiser = r; }

// -----------------------------------------------------------------------------
//  errors.jsonl 的记录点（E11）
//
//  【规则】E11 是「L2 只写日志；L3 **未捕获**才写；L4 写」。
//    ⇒ 所以 raise 的记录**不在抛出处写**（那时还拿不到脚本位置的时机已经过了），
//      而是在捕获处；其余级别在各自的 fail_* 入口写。
//  【为什么写记录的动作活在 fail_* 里】它们本来就拿着 error_value；
//    挂到日志通道上会让 27 处直接 `log::warn("…")` 也混进来，
//    而它们**没有 error_class** —— 硬填一个类别就是撒谎。
// -----------------------------------------------------------------------------
namespace {

/// 写一条记录（门禁关时 `write_error_line` 自己会立刻返回）。
void write_record(log::severity sev, error_class cls, std::string_view where,
                  std::string_view got, std::string_view want,
                  std::string_view sentence, std::string_view file, int line,
                  bool external = false) {
    diagnostics::error_record r;
    r.level = log::severity_name(sev);
    r.cls = class_name(cls);
    r.where = where;
    r.got = got;
    r.want = want;
    r.sentence = sentence;
    r.file = file;
    r.line = line;
    r.external = external;
    diagnostics::write_error_line(r);
}

/// `error_value` → 记录。`sentence` 用 `subject_of`（就是"给人读的那一句"）。
void write_record_of(log::severity sev, const error_value& e,
                     std::string_view file = {}, int line = 0,
                     bool external = false) {
    const std::string subject = subject_of(e);
    write_record(sev, e.cls, e.where, e.got, e.want, subject, file, line, external);
}

} // namespace

void stash_raise(const error_value& e) noexcept {
    try {
        g_pending.active = true;
        g_pending.cls = e.cls;
        g_pending.where.assign(e.where.data(), e.where.size());
        g_pending.got.assign(e.got.data(), e.got.size());
        g_pending.want.assign(e.want.data(), e.want.size());
        g_pending.sentence = subject_of(e);
        g_pending.file.clear();
        g_pending.external = false;
        g_pending.line = 0;
    } catch (...) {
        g_pending.active = false; // 存不下就当没发生（诊断导出不该影响抛错）
    }
}

void set_raise_position(const char* file, int line) noexcept {
    if (!g_pending.active || file == nullptr || line <= 0) {
        return;
    }
    try {
        g_pending.file = file;
        // Lua 的 source 形如 "@C:\path\script.lua" 或 "=stdin"：
        //   '@' 前缀表示"文件"，'=' 前缀表示"字面量块"。去掉前缀才是文件名。
        if (!g_pending.file.empty() &&
            (g_pending.file.front() == '@' || g_pending.file.front() == '=')) {
            g_pending.file.erase(g_pending.file.begin());
        }

        // ---------------------------------------------------------------------
        //  **兜底**：把 file 变成“相对安装根”
        //
        //  【为什么还要这一步】脚本装载已改成"相对 chunk 名"
        //    （`@Luminosa/Project/x.lua`），但还有别的通路会给绝对路径：
        //    `require` 走的是 `package.path`（绝对）⇒ 经由 require 装载的模块，
        //    Lua 的 chunk 名仍是绝对的。这里统一相对化。
        //  【根外】`is_external` 为真 ⇒ JSON 里加 `"external":true`。
        // ---------------------------------------------------------------------
        g_pending.external = wag::paths::is_external(g_pending.file);
        g_pending.file = wag::paths::relativize(g_pending.file);
        g_pending.line = line;
    } catch (...) {
        // 同上：存不下就保持没有位置（file/line 是可空字段）。
    }
}

void flush_pending_raise() noexcept {
    if (!g_pending.active) {
        return;
    }
    try {
        write_record(log::severity::raise, g_pending.cls, g_pending.where,
                     g_pending.got, g_pending.want, g_pending.sentence,
                     g_pending.file, g_pending.line, g_pending.external);
    } catch (...) {
        // 诊断导出失败不是引擎该为之终止的理由。
    }
    g_pending.active = false;
}

void fail_boundary(const error_value& e) {
    // 【锚点与输出】错误文本由 subject/detail 拼出——与 fail_api_misuse 共用同一份
    //   拼装（见本文件上面那段的说明）。
    std::string text = log::format(subject_of(e), detail_of(e));

    // ---- E8：有落点就交还，没有才终止 ----
    //
    //  【为什么不再先记一条日志】文本会随 Lua 错误一起回到脚本：
    //    未捕获时由 capture_error 记（和以前一样）；被捕获时作者自己看着办。
    //    若这里先记一条，被捕获的 L3 会刷屏——而"能捕获"本来就是为了让它可控。
    //
    //  【长跳转的真实代价（必须记在明处）】`lua_error` 是 longjmp，
    //    **不执行 C++ 析构**。所以本函数与它的调用点都遵守一条纪律：
    //    **抛错点不持有需要析构的对象**（本函数只持有一个 text，而它在
    //    raiser 里被复制进 Lua 栈之后才跳，见 script 层的实现）。
    if (g_raiser != nullptr && g_hand_back_depth > 0) {
        // 【E11 ③b】先把内容**复制**到 pending（longjmp 之后原缓冲就没了），
        //   再由 script 层在抛之前补上 file:line，最后由捕获点落盘。
        stash_raise(e);
        g_raiser(text); // 有落点：longjmp，永不返回
        // 走到这里说明 raiser 违约（说好有落点却没抛）⇒ 落到终止，不放它过去。
    }

    // 【终止前最后一次脚本收尾】on_game_exit（幂等；由 main 注册的回调转调）。
    if (g_before_terminate != nullptr) {
        g_before_terminate();
    }

    // 没有落点（启动/关闭/宿主路径）⇒ L4：**遗言**，先落盘再终止
    log::flush(); // ① 文本日志先落盘
    write_record_of(log::severity::terminate, e); // ② 写 errors.jsonl 的 terminate 行
    log::terminate("error", text); // ③（弹窗在 log 内部，opt-in）→ ④ abort
}

void fail_host(const error_value& e) {
    // 【与 fail_boundary 的差别只有一处】这里**不看** g_hand_back_depth。
    //   调用方已经知道"这次不该活下去"（脚本显式声明致命，或宿主路径）。
    //   文本拼装故意与 fail_boundary 完全一致 —— 同一件事不该有两种说法。
    // 【终止前最后一次脚本收尾】（on_game_exit；幂等，由 main 注册）
    if (g_before_terminate != nullptr) {
        g_before_terminate();
    }

    log::flush();
    write_record_of(log::severity::terminate, e);
    log::terminate("error", log::format(subject_of(e), detail_of(e)));
}

std::string format_error(const error_value& e) {
    // 【唯一的一处拼装】subject_of / detail_of 就在上面，不另写一份。
    return log::format(subject_of(e), detail_of(e));
}

void fail_internal(std::string_view what) {
    // 【它与 fail_boundary 的区别不是级别，而是含义】
    //   boundary_validation 是"输入的错"，internal_invariant 是"引擎的错"。
    //   后者意味着引擎有 bug，因此错误信息里要明确说"这是引擎的问题"，
    //   否则读到它的人会先去怀疑脚本。
    std::string subject = "[引擎内部] ";
    subject.append(what.data(), what.size());

    // 【E11 乙：入口内部合成最小 error_value】（调用点一个都不改）
    error_value e;
    e.cls = error_class::internal_invariant;
    e.sentence = subject;
    // 【终止前最后一次脚本收尾】（on_game_exit；幂等，由 main 注册）
    if (g_before_terminate != nullptr) {
        g_before_terminate();
    }
    log::flush();
    write_record_of(log::severity::terminate, e);
    log::terminate("error", subject);
}

void set_before_terminate(void (*callback)()) noexcept {
    g_before_terminate = callback;
}

void fail_script(std::string_view what) {
    // 【L2：记录 + 通知，然后继续】
    //
    // 【"继续"的精确含义】见 error.hpp 与 log.hpp 的注释：
    //   不是继续那个出错的调用，而是**继续推进帧结构**。
    //
    // 【它不终止】因为这是"调用方写错了"，而引擎可以当作"这一次调用没有发生"
    //   继续跑下去。
    // 【E11 乙：入口内部合成最小 error_value】句子即诊断
    error_value e;
    e.cls = error_class::script_error;
    e.sentence = what;
    write_record_of(log::severity::warn, e);

    log::warn("error", log::format("脚本错误", what));
}

void fail_api_misuse(const error_value& e) {
    // 【L2：记录 + 通知，然后继续】
    //
    // 【它为什么不能终止】"调用方传了一个不认识的名字"不破坏任何不变量：
    //   引擎可以当作"这一次调用没有发生"继续跑。而终止会把一个参数的笔误
    //   升级成"游戏起不来"。
    //
    // 【级别从哪来】由 e.cls 查表得出（class_severity）。本函数**不判断级别**
    //   —— 那是决策表的事。见 error.hpp 那段"级别是类别的函数"。
    write_record_of(log::severity::warn, e);
    log::warn("error", log::format(subject_of(e), detail_of(e)));
}

void fail_shutdown(std::string_view what) {
    // 【L1：只记录】
    //
    // 【为什么不弹窗】因为关闭序列里没有"可带错继续"这一档 —— 那时回不到
    //   脚本，弹窗也没有地方可以回到。
    //
    // 【它的存在理由】让关闭过程中的失败留下痕迹，而不是被静默吞掉。
    log::record("关闭序列", log::format("", what));
}

// =============================================================================
//  校验辅助
// =============================================================================

bool is_finite(double v) noexcept {
    // 【为什么 NaN 与无穷都要拒绝】
    //   对"坐标"这个语义而言，它们不是"少见的值"，而是**没有意义的值**：
    //     · NaN 不是一个数
    //     · 无穷没有实现语义（"坐标是无穷"这件事画不出来）
    //   ⇒ 所以它们属于"数据互相矛盾"，属于必须拒绝的那一类。
    //
    // 【为什么用 std::isfinite 而不是自己比较】
    //   自己比较（v == v 检 NaN、与 DBL_MAX 比较检无穷）在开启快速数学优化时
    //   可能被优化掉。std::isfinite 是唯一可靠的写法。
    return std::isfinite(v);
}

void require_all_finite(const double* values, std::size_t count,
                        std::string_view where) {
    if (values == nullptr) {
        // 【空指针也是一种数据矛盾】但它是"这一批数据不存在"，而不是"某个值
        //   不合法"，所以单独报。
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = where;
        e.got = "null";
        e.want = "一个非空的数据指针";
        fail_boundary(e);
        // 【为什么这里是"不可达终止"而不是 return】
        //   原先写的是 `return;`，注释说"只为满足编译器" —— 而那是错的：
        //   它自己就证明了"这个函数会返回"，于是 `[[noreturn]]` 的声明与
        //   实现互相矛盾（GCC 会报 'noreturn' function does return）。
        //   ⇒ 正确做法是给控制流一个真正的终止点。见 error.cpp 顶部
        //     unreachable_terminate 的注释。
        unreachable_terminate();
    }

    for (std::size_t i = 0; i < count; ++i) {
        if (!is_finite(values[i])) {
            // 【整批拒绝，但报出**第一个**出问题的那一项】
            //
            //   报第一个而不是收集全部，理由是：
            //     · 一个元素的错通常意味着同一处的写法错了，后面往往是同一原因
            //     · 报全部会让错误信息变长而可读性变差
            //   而"整批不生效"这条保证与报几个无关 —— 它由调用方在动手之前
            //   调用本函数来落实。
            error_value e;
            e.cls = error_class::boundary_validation;
            e.where = where;
            // 【为什么把序号也报出来】因为"第 3 个顶点的 x"能直接定位到代码，
            //   而"某个顶点的 x"不能。
            e.got = "不是有限值";
            e.want = "有限实数";
            e.sentence = where; // 具体的位置描述由调用方给出（含序号）
            fail_boundary(e);
            // 【这里刻意没有 return】
            //   `fail_boundary` 是 `[[noreturn]]`，它不会返回 ⇒ 写 return 会让
            //   编译器报"不可达代码"。而本函数**上面那一处**（空指针那条）写的是
            //   `unreachable_terminate()` —— 两处处理同一个函数的两条出口,
            //   写法必须一致，否则就是"同一件事两种写法"。
            unreachable_terminate();
        }
    }
    // 全部合法 ⇒ 正常返回。调用方此时才可以动手。
}

} // namespace wag
