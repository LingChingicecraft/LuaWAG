// =============================================================================
//  diagnostics/diagnostics.cpp
//
//  两件事：一个总开关，以及错误/性能/池记录的写出。
//
//  【它为什么不需要别的引擎模块】
//  本目录的准入判据是"只输出、不改行为"。零模块依赖是这条判据最省事的证明：
//  没有依赖 ⇒ 不可能反向改到任何东西。
//  `error_record` 用的是**视图**而不是 `wag::error_value`，正是为了保住这一点
//  （否则 `wag_error ↔ wag_diagnostics` 会成环，见头文件的说明）。
//
//  【"关掉时行为逐字节相同"怎么落实】
//  每一条写路径的第一句都是 `if (!g_enabled) return;` ——
//  目录、文件句柄、时钟一次都不碰。
// =============================================================================
#include "diagnostics/diagnostics.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
// 【为什么要 _fsopen + _SH_DENYNO】
//   引擎与控制台必须**共享读写**。
//   否则控制台一打开文件，引擎这边的下一次写就失败。
#include <process.h>
#include <share.h>
#define WAG_DIAG_FOPEN(path, mode) _fsopen((path), (mode), _SH_DENYNO)
#define WAG_DIAG_PID() static_cast<long long>(::_getpid())
#else
#include <unistd.h>
#define WAG_DIAG_FOPEN(path, mode) std::fopen((path), (mode))
#define WAG_DIAG_PID() static_cast<long long>(::getpid())
#endif

namespace wag::diagnostics {
namespace {

/// 【为什么是普通全局 bool，而不是 atomic / constinit】
///   写入只发生在启动期**一次**（main 解析完命令行），之后全程只读。
///   没有并发写入者 ⇒ 加原子只会假装存在一个并不存在的竞争。
bool g_enabled = false;

/// exe 所在目录（由 main 给）。空 ⇒ 没给 ⇒ 不写（**也不建目录**）。
std::string g_output_dir;

/// 记录里的 `frame` 字段。由帧管线每帧更新。
std::uint64_t g_frame = 0;

/// 本次会话 id（= `session.json` 的 `started_at`）。0 = 未开始。
std::int64_t g_session = 0;

/// 一个"惰性打开"的追加文件。
///
/// 【为什么每个文件各自一份状态】一个文件打不开不该让别的文件也停；
///   而"失败后不再重试"这条要对**每个文件**分别成立。
struct sink {
    std::FILE* file = nullptr;
    bool failed = false;
    const char* filename = "";
    const char* mode = "ab";
};

sink g_errors{nullptr, false, "errors.jsonl", "ab"};
sink g_pool{nullptr, false, "pool_log.jsonl", "ab"};
sink g_perf{nullptr, false, "perf_log.jsonl", "ab"};

/// perf_log 的未 flush 行数（每 60 行 flush 一次）。
unsigned g_perf_unflushed = 0;

/// perf_log 已经写出去的字节数（含打开时文件已有的内容）。
///
/// 【为什么要含已有内容】它是**追加**文件：上一次运行留下的体积也算在 64 MB 里。
std::size_t g_perf_bytes = 0;

/// 容量上限（**64 MB 按字节**，到顶停写）。
constexpr std::size_t kPerfCapBytes = 64u * 1024u * 1024u;

/// 是否已因容量上限停写。
bool g_perf_capped = false;

/// 把 double 按给定格式写进 JSON。
///
/// 【为什么要兜非有限值】JSON 没有 `NaN` / `Inf` 字面量 —— 写出去就是**非法 JSON**，
///   下游（控制台 / 校验器）会整行读失败。宁可写 0，也不写坏文件。
void append_double(std::string& out, double v, const char* fmt) {
    if (!std::isfinite(v)) {
        out += "0";
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), fmt, v);
    out += buf;
}

/// 当前时间（ms，Unix 纪元）。
long long now_ms() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
        .count();
}

/// 把一个字符串按 JSON 规则写进 out。
///
/// 【为什么不引 JSON 库】引擎只有 SDL3 + LuaJIT 两个依赖，必须保持；
///   而且这里**只写不读**，手写这一小段就够。
/// 【为什么 >= 0x80 直通】输入是 UTF-8；JSON 字符串允许非转义的 UTF-8 字节。
///   逐字节转义非 ASCII 会把中文变成一堆 \uXXXX，人就没法直接读了。
void append_json_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (const unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

/// 首次写入时建目录并打开文件。返回 nullptr 表示交不出去（不写）。
std::FILE* ensure_open(sink& s) noexcept {
    if (s.file != nullptr) {
        return s.file;
    }
    if (s.failed || g_output_dir.empty()) {
        return nullptr; // 没给目录 ⇒ 什么都不做（**不建目录**）
    }

    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::path(g_output_dir) / "debug";
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        s.failed = true; // 只关导出，不换目录
        return nullptr;
    }

    const std::filesystem::path path = dir / s.filename;
    const std::string native = path.string();
    s.file = WAG_DIAG_FOPEN(native.c_str(), s.mode);
    if (s.file == nullptr) {
        s.failed = true;
    }
    return s.file;
}

} // namespace

bool enabled() noexcept { return g_enabled; }

void set_enabled(bool on) noexcept { g_enabled = on; }

void set_output_dir(std::string dir) { g_output_dir = std::move(dir); }

void set_frame(std::uint64_t frame) noexcept { g_frame = frame; }

void write_error_line(const error_record& r) noexcept {
    if (!g_enabled) {
        return; // ← "关掉时连目录都不建"落在这一句
    }
    std::FILE* const f = ensure_open(g_errors);
    if (f == nullptr) {
        return;
    }

    try {
        std::string line;
        line.reserve(288);

        line += "{\"version\":1,\"session\":";
        line += std::to_string(g_session); // 每行都带会话 id
        line += ",\"t\":";
        line += std::to_string(now_ms());
        line += ",\"frame\":";
        line += std::to_string(g_frame);

        line += ",\"level\":";
        append_json_string(line, r.level);
        line += ",\"cls\":";
        append_json_string(line, r.cls);
        line += ",\"where\":";
        append_json_string(line, r.where);
        line += ",\"got\":";
        append_json_string(line, r.got);
        line += ",\"want\":";
        append_json_string(line, r.want);

        // file / line 是**唯一可空**的两个字段（控制台的 ErrorEvent 里它们是 string?/int?）。
        line += ",\"file\":";
        if (r.file.empty()) {
            line += "null";
        } else {
            append_json_string(line, r.file);
        }
        line += ",\"line\":";
        if (r.line <= 0) {
            line += "null";
        } else {
            line += std::to_string(r.line);
        }

        // 【只在这个文件位于安装根之外时才出现】可选字段，老读者忽略即可。
        if (r.external) {
            line += ",\"external\":true";
        }

        line += ",\"sentence\":";
        append_json_string(line, r.sentence);
        line += "}\n";

        std::fwrite(line.data(), 1, line.size(), f);
        // 行级 flush：控制台是"tail 读"，不 flush 就永远看不到；
        // 而且 L4 会 abort()（不跑 atexit），缓冲区会直接蒸发。
        std::fflush(f);
    } catch (...) {
        // 【为什么吞掉】它会被 fail_* 在"即将终止/抛错"的路径上调用，
        // 那里不允许异常穿出（longjmp 与 C++ 栈不兼容）。
        // 诊断导出失败不是引擎该为之终止的理由。
        g_errors.failed = true;
    }
}

void begin_session(const session_info& info) {
    if (!g_enabled || g_output_dir.empty()) {
        return; // 门禁关闭 ⇒ 连 started_at 都不记（没有读者）
    }
    g_session = now_ms();

    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::path(g_output_dir) / "debug";
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return; // 只关导出、不换目录
    }

    // 【为什么是 "wb" 而不是追加】它描述**这一次**运行。
    //   历史会话由 jsonl 每行的 `session` 自己带着，不靠这个文件保存。
    const std::filesystem::path path = dir / "session.json";
    const std::string native = path.string();
    std::FILE* const f = WAG_DIAG_FOPEN(native.c_str(), "wb");
    if (f == nullptr) {
        return;
    }

    try {
        std::string s;
        s.reserve(160);
        s += "{\"version\":1,\"pid\":";
        s += std::to_string(WAG_DIAG_PID());
        s += ",\"started_at\":";
        s += std::to_string(g_session);
        s += ",\"project\":";
        append_json_string(s, info.project);
        s += ",\"engine\":";
        append_json_string(s, info.engine_version);
        s += ",\"light\":{\"cap\":";
        s += std::to_string(info.light_cap);
        s += ",\"bands\":";
        s += std::to_string(info.band_cap);
        s += ",\"fill_k\":";
        s += std::to_string(info.fill_budget_k);
        s += "}}\n";
        std::fwrite(s.data(), 1, s.size(), f);
    } catch (...) {
        // 同上：诊断导出失败不影响引擎。
    }
    std::fflush(f);
    std::fclose(f);
}

std::int64_t session_id() noexcept { return g_session; }

void write_pool_line(const pool_row* rows, std::size_t count) noexcept {
    if (!g_enabled) {
        return;
    }
    std::FILE* const f = ensure_open(g_pool);
    if (f == nullptr) {
        return;
    }

    try {
        std::string line;
        line.reserve(160 + 64 * count);
        line += "{\"version\":1,\"session\":";
        line += std::to_string(g_session);
        line += ",\"t\":";
        line += std::to_string(now_ms());
        line += ",\"frame\":";
        line += std::to_string(g_frame);
        line += ",\"pools\":[";
        for (std::size_t i = 0; i < count; ++i) {
            if (i != 0) {
                line.push_back(',');
            }
            line += "{\"name\":";
            append_json_string(line, rows[i].name);
            line += ",\"capacity\":";
            line += std::to_string(rows[i].capacity);
            line += ",\"active\":";
            line += std::to_string(rows[i].active);
            line += '}';
        }
        line += "]}\n";

        std::fwrite(line.data(), 1, line.size(), f);
        std::fflush(f); // 低频文件也逐行 flush：量级太小，没必要攒
    } catch (...) {
        g_pool.failed = true;
    }
}

void write_perf_line(const perf_sample& s) noexcept {
    if (!g_enabled || g_perf_capped) {
        return;
    }
    std::FILE* const f = ensure_open(g_perf);
    if (f == nullptr) {
        return;
    }

    try {
        // 【容量上限】首次写入时把文件**已有体积**算进来（它是追加文件）。
        if (g_perf_bytes == 0) {
            std::fseek(f, 0, SEEK_END);
            const long pos = std::ftell(f);
            g_perf_bytes = pos > 0 ? static_cast<std::size_t>(pos) : 0;
        }
        if (g_perf_bytes >= kPerfCapBytes) {
            g_perf_capped = true; // 到顶就停写；日志由帧管线用 perf_capped() 打一条
            return;
        }

        std::string line;
        line.reserve(160);
        line += "{\"version\":1,\"session\":";
        line += std::to_string(g_session);
        line += ",\"t\":";
        line += std::to_string(now_ms());
        line += ",\"frame\":";
        line += std::to_string(g_frame);
        line += ",\"target_fps\":";
        append_double(line, s.target_fps, "%.2f");
        line += ",\"containers\":";
        line += std::to_string(s.containers);
        line += ",\"draw_calls\":";
        line += std::to_string(s.draw_calls);
        line += ",\"frame_ms\":";
        append_double(line, s.frame_ms, "%.3f");
        line += ",\"light_lights\":";
        line += std::to_string(s.light_lights);
        line += ",\"light_occluders\":";
        line += std::to_string(s.light_occluders);
        line += ",\"light_commits\":";
        line += std::to_string(s.light_commits);
        line += ",\"light_fill_ratio\":";
        append_double(line, s.light_fill_ratio, "%.3f");
        line += "}\n";

        std::fwrite(line.data(), 1, line.size(), f);
        g_perf_bytes += line.size();

        // 【flush 粒度】每 60 行（约 1 秒 @60 Hz）flush 一次。
        //   逐帧 flush 是 60 次/秒的无谓 I/O；攒 1 秒的代价是崩溃最多丢 1 秒数据。
        if (++g_perf_unflushed >= 60) {
            g_perf_unflushed = 0;
            std::fflush(f);
        }
    } catch (...) {
        g_perf.failed = true;
    }
}

bool perf_capped() noexcept { return g_perf_capped; }

} // namespace wag::diagnostics
