// =============================================================================
// wag/log/log.cpp
//
// 普通日志实现：隐私日志 + 完整日志，同一条事件流。
//
// 【设计依据】
// D1–D15：
// · <安装根>/engine.log 隐私日志
// · <安装根>/engine.full.log 完整日志
// · 各 8 MB，append，按行删除最旧
// · UTC 毫秒时间戳，L1–L4
// · 安装根不可写时回退到用户目录
//
// 【失败约束】
// 日志子系统自身失败（磁盘满 / 被占用 / 无权限）⇒ 降级到最后通道
// （标准错误），不再走报错机制 —— 否则会递归。
// =============================================================================

#include "log/log.hpp"
#include "platform/utf8_path.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace wag::log {

namespace {

// -----------------------------------------------------------------------------
// 常量
// -----------------------------------------------------------------------------

constexpr std::size_t kMaxLogBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxLineBytes = 1024u * 1024u;
constexpr unsigned kFlushEvery = 32;

// -----------------------------------------------------------------------------
// 两份文件
// -----------------------------------------------------------------------------

struct sink {
    FILE* file = nullptr;
    std::string path;
    bool fallback = false;
    bool trim_disabled = false;
};

sink g_privacy;
sink g_full;

bool g_no_log = false;
mode g_mode = mode::interactive;
dialog_handler g_dialog = nullptr;
bool g_initialized = false;
std::uint64_t g_frame = 0;
std::thread::id g_main_thread{};
std::string g_install_root;

// -----------------------------------------------------------------------------
// 最后通道
// -----------------------------------------------------------------------------

void last_resort_impl(std::string_view s) noexcept {
    if (!s.empty()) {
        std::fwrite(s.data(), 1, s.size(), stderr);
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }
}

std::string join_path(std::string_view root, const char* name) {
    if (root.empty()) {
        return {};
    }
    std::string out(root);
    while (!out.empty() &&
           (out.back() == '/' || out.back() == '\\')) {
        out.pop_back();
    }
    out += '/';
    out += name;
    return out;
}

FILE* open_append(const std::string& path) {
    if (path.empty()) {
        return nullptr;
    }
    return wag::paths::fopen_utf8(path, "ab");
}

std::string fallback_dir() {
#if defined(_WIN32)
 // 【为什么不用 getenv】LOCALAPPDATA 可能含中文用户名；getenv 返回的是
 // ACP 编码，不是 UTF-8。这里直接从宽字符环境取，再转回 UTF-8。
    wchar_t buffer[32768];
    const DWORD n = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) {
        return {};
    }
    const std::string local = wag::paths::narrow_utf8(
        std::wstring_view(buffer, static_cast<std::size_t>(n)));
    if (local.empty()) {
        return {};
    }
    return local + "/LuaWAG/logs";
#else
    const char* local = std::getenv("LOCALAPPDATA");
    if (local == nullptr || local[0] == '\0') {
        return {};
    }
    return std::string(local) + "/LuaWAG/logs";
#endif
}

// 【为什么不抛异常】日志打开失败是降级路径，不是错误路径。
void open_sink(sink& s, const std::string& primary,
               const std::string& fallback) {
    s.file = open_append(primary);
    if (s.file != nullptr) {
        s.path = primary;
        s.fallback = false;
        return;
    }

    if (!fallback.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(
            wag::paths::path_from_utf8(fallback).parent_path(), ec);
        s.file = open_append(fallback);
        if (s.file != nullptr) {
            s.path = fallback;
            s.fallback = true;
            return;
        }
    }

    s.file = nullptr;
    s.path.clear();
    s.fallback = false;
}

void close_sink(sink& s) noexcept {
    if (s.file != nullptr) {
        std::fflush(s.file);
        std::fclose(s.file);
        s.file = nullptr;
    }
}

// -----------------------------------------------------------------------------
// 8 MB 按行裁剪
// -----------------------------------------------------------------------------

// 【为什么读全文】上限只有 8 MB；行级保留比环形缓冲简单，也不会留下半行。
bool trim_sink(sink& s) {
    if (s.file == nullptr || s.trim_disabled) {
        return false;
    }

    std::fflush(s.file);
    std::fclose(s.file);
    s.file = nullptr;

    std::ifstream in(wag::paths::path_from_utf8(s.path), std::ios::binary);
    if (!in) {
        s.file = open_append(s.path);
        s.trim_disabled = true;
        return false;
    }

    std::vector<std::string> lines;
    std::size_t total = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        total += line.size() + 1;
        lines.push_back(std::move(line));
    }
    in.close();

    while (total > kMaxLogBytes && !lines.empty()) {
        total -= lines.front().size() + 1;
        lines.erase(lines.begin());
    }

    if (lines.empty()) {
 // 极端情况下连一行都放不下：保留最后一行并截断。
        s.file = open_append(s.path);
        s.trim_disabled = true;
        return false;
    }

    const std::string tmp = s.path + ".tmp";
    {
        std::ofstream out(wag::paths::path_from_utf8(tmp),
                          std::ios::binary | std::ios::trunc);
        if (!out) {
            s.file = open_append(s.path);
            s.trim_disabled = true;
            return false;
        }
        for (const auto& l : lines) {
            out.write(l.data(), static_cast<std::streamsize>(l.size()));
            out.put('\n');
        }
        out.flush();
        if (!out) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(wag::paths::path_from_utf8(tmp), ec);
            s.file = open_append(s.path);
            s.trim_disabled = true;
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::remove(wag::paths::path_from_utf8(s.path), ec);
    ec.clear();
    std::filesystem::rename(wag::paths::path_from_utf8(tmp),
                            wag::paths::path_from_utf8(s.path), ec);
    if (ec) {
        std::error_code ec2;
        std::filesystem::remove(wag::paths::path_from_utf8(tmp), ec2);
        s.file = open_append(s.path);
        s.trim_disabled = true;
        return false;
    }

    s.file = open_append(s.path);
    return s.file != nullptr;
}

void maybe_trim(sink& s) {
    if (s.file == nullptr || s.trim_disabled) {
        return;
    }
    const long pos = std::ftell(s.file);
    if (pos >= 0 && static_cast<std::size_t>(pos) > kMaxLogBytes) {
        if (!trim_sink(s)) {
            last_resort_impl("[log] 日志文件裁剪失败，停止本次运行的裁剪");
        }
    }
}

// -----------------------------------------------------------------------------
// 时间与行
// -----------------------------------------------------------------------------

std::string utc_timestamp_ms() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);

    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif

    char base[32]{};
    std::strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tmv);
    char out[40]{};
    std::snprintf(out, sizeof(out), "%s.%03lldZ", base,
                  static_cast<long long>(ms.count()));
    return out;
}

const char* level_tag(severity sev) noexcept {
    switch (sev) {
    case severity::record:
        return "L1";
    case severity::warn:
        return "L2";
    case severity::raise:
        return "L3";
    case severity::terminate:
        return "L4";
    }
    return "L?";
}

// -----------------------------------------------------------------------------
// 写行
// -----------------------------------------------------------------------------

void write_to_sink(sink& s, std::string_view line) {
    if (s.file == nullptr) {
        return;
    }
    std::fwrite(line.data(), 1, line.size(), s.file);
    std::fputc('\n', s.file);
    if (std::ferror(s.file) != 0) {
 // 磁盘满/句柄失效：关闭这一份，另一份继续。
        close_sink(s);
        s.trim_disabled = true;
        return;
    }
    maybe_trim(s);
}

void write_line(std::string_view line) {
    if (line.empty()) {
        return;
    }

 // D9：日志里的安装根一律脱敏成 <root>，两份日志同一处理。
    std::string owned;
    if (!g_install_root.empty()) {
        owned.assign(line.data(), line.size());
        std::string root = g_install_root;
        while (!root.empty() && (root.back() == '/' || root.back() == '\\')) {
            root.pop_back();
        }
        auto replace_all = [&owned](const std::string& needle) {
            if (needle.empty()) {
                return;
            }
            std::size_t pos = 0;
            while ((pos = owned.find(needle, pos)) != std::string::npos) {
                owned.replace(pos, needle.size(), "<root>");
                pos += 6;
            }
        };
        replace_all(root);
        std::string slash = root;
        std::replace(slash.begin(), slash.end(), '\\', '/');
        replace_all(slash);
        line = owned;
    }

 // 单行过长：截断，避免一个错误消息把 8MB 预算一次吃光。
    std::string capped;
    if (line.size() > kMaxLineBytes) {
        capped.assign(line.data(), kMaxLineBytes);
        capped += "...（日志行过长，已截断）";
        line = capped;
    }

    if (g_privacy.file == nullptr && g_full.file == nullptr) {
        last_resort_impl(line);
        return;
    }
    write_to_sink(g_privacy, line);
    write_to_sink(g_full, line);
}

} // namespace

// =============================================================================
// 最后通道与刷新
// =============================================================================

void write_last_resort(std::string_view utf8) noexcept {
    last_resort_impl(utf8);
}

void write_raw(std::string_view utf8) {
    write_line(utf8);
}

void write_full_header(std::string_view text) {
    if (g_full.file == nullptr) {
        return;
    }

    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find('\n', begin);
        const std::size_t n =
            (end == std::string_view::npos) ? text.size() - begin : end - begin;
        std::string line;
        line.reserve(n + 48);
        line += "[L1][";
        line += utc_timestamp_ms();
        line += "][boot] ";
        line.append(text.data() + begin, n);
        write_to_sink(g_full, line);
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    std::fflush(g_full.file);
}

void flush() noexcept {
    if (g_privacy.file != nullptr) {
        std::fflush(g_privacy.file);
    }
    if (g_full.file != nullptr) {
        std::fflush(g_full.file);
    }
    std::fflush(stderr);
}

// =============================================================================
// 配置
// =============================================================================

void init(std::string_view install_root, bool no_log, mode m) {
    g_mode = m;
    g_no_log = no_log;
    g_initialized = true;
    g_main_thread = std::this_thread::get_id();
    g_install_root.assign(install_root.data(), install_root.size());

    if (g_no_log) {
        g_privacy.path.clear();
        g_full.path.clear();
        return;
    }

    const std::string primary_privacy =
        join_path(install_root, "engine.log");
    const std::string primary_full =
        join_path(install_root, "engine.full.log");
    const std::string fb = fallback_dir();

    open_sink(g_privacy, primary_privacy,
              fb.empty() ? std::string{} : join_path(fb, "engine.log"));
    open_sink(g_full, primary_full,
              fb.empty() ? std::string{} : join_path(fb, "engine.full.log"));

 // 日志系统启动这件事本身值得记一笔。
    record_message(severity::record, "log",
                   "日志已打开：隐私=engine.log" +
                       (g_privacy.file == nullptr
                            ? std::string("（未打开）")
                            : (g_privacy.fallback ? std::string("（用户目录）")
                                                  : std::string("（安装根）"))) +
                       "；完整=engine.full.log" +
                       (g_full.file == nullptr
                            ? std::string("（未打开）")
                            : (g_full.fallback ? std::string("（用户目录）")
                                               : std::string("（安装根）"))));
}

void set_dialog_handler(dialog_handler handler) noexcept {
    g_dialog = handler;
}

void shutdown() noexcept {
    close_sink(g_privacy);
    close_sink(g_full);
    g_initialized = false;
}

mode current_mode() noexcept {
    return g_mode;
}

const std::string& privacy_log_path() noexcept {
    return g_privacy.path;
}

const std::string& full_log_path() noexcept {
    return g_full.path;
}

bool privacy_log_fallback() noexcept {
    return g_privacy.fallback;
}

bool full_log_fallback() noexcept {
    return g_full.fallback;
}

void set_frame(std::uint64_t frame) noexcept {
    g_frame = frame;
}

// =============================================================================
// 输出
// =============================================================================

void record_message(severity sev, std::string_view category,
                    std::string_view canonical, std::string_view detail) {
 // D11：v1 只允许主线程。非主线程不写文件（避免数据竞争），只留一条
 // stderr；Debug 构建直接断言。
    if (g_initialized && std::this_thread::get_id() != g_main_thread) {
        assert(false && "wag::log 只允许主线程调用");
        last_resort_impl("[log] 非主线程日志调用被丢弃");
        return;
    }

    std::string line;
    line.reserve(category.size() + canonical.size() + detail.size() + 64);
    line += '[';
    line += level_tag(sev);
    line += "][";
    line += utc_timestamp_ms();
    line += "][";
    line.append(category.data(), category.size());
    line += "] ";

    line.append(canonical.data(), canonical.size());
    if (!detail.empty()) {
        line += "  | ";
        line.append(detail.data(), detail.size());
    }

    if (sev == severity::raise || sev == severity::terminate) {
        line += "  | frame=";
        line += std::to_string(g_frame);
    }

    write_line(line);

    static unsigned since_flush = 0;
    if (++since_flush >= kFlushEvery) {
        since_flush = 0;
        flush();
    }

    if (sev == severity::record) {
        return;
    }

    const bool terminal = (sev == severity::terminate);
    const bool raising = (sev == severity::raise);
    if (terminal || raising) {
        flush();
    }

    if (sev == severity::terminate) {
        if (g_dialog != nullptr && g_mode != mode::non_interactive) {
            const char* opt_in = std::getenv("WAG_SHOW_DIALOG");
            if (opt_in != nullptr && std::strcmp(opt_in, "1") == 0) {
                g_dialog("LuaWAG", canonical);
            }
        }
        std::abort();
    }
}

// =============================================================================
// 便捷包装
// =============================================================================

void record(std::string_view category, std::string_view canonical,
            std::string_view detail) {
    record_message(severity::record, category, canonical, detail);
}

const char* severity_name(severity s) noexcept {
    switch (s) {
    case severity::record:
        return "record";
    case severity::warn:
        return "warn";
    case severity::raise:
        return "raise";
    case severity::terminate:
        return "terminate";
    }
    return "unknown";
}

void warn(std::string_view category, std::string_view canonical,
          std::string_view detail) {
    record_message(severity::warn, category, canonical, detail);
}

void raise(std::string_view category, std::string_view canonical,
           std::string_view detail) {
    record_message(severity::raise, category, canonical, detail);
}

void terminate(std::string_view category, std::string_view canonical,
               std::string_view detail) {
    record_message(severity::terminate, category, canonical, detail);
    std::abort();
}

// =============================================================================
// 规范文本的构造
// =============================================================================

std::string format(std::string_view subject, std::string_view detail) {
    std::string out;
    out.reserve(subject.size() + detail.size() + 8);
    out.append(subject.data(), subject.size());
    if (!detail.empty()) {
        out += "（";
        out.append(detail.data(), detail.size());
        out += "）";
    }
    return out;
}

} // namespace wag::log
