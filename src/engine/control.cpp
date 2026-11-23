// =============================================================================
//  engine/control.cpp —— 控制通道的实现
//
//  【本文件只做三件事】
//     ① 按 tail 语义读出 `command.txt` 里新追加的完整行；
//     ② 把最多 8 条/帧交给脚本（`take`）；
//     ③ 把脚本 publish 的键值 1 Hz 写成 `debug/state.json`。
//
//  【它**不做**什么】不解释动词、不认结点、不等待控制台、不改任何帧内行为。
// =============================================================================

#include "engine/control.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "log/log.hpp"
#include "platform/paths.hpp"

namespace wag::control {

namespace {

namespace fs = std::filesystem;

/// 去掉行尾的 `\r`（控制台可能是 CRLF）与首尾空白。
std::string trim_ascii(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) {
        --e;
    }
    return s.substr(b, e - b);
}

/// JSON 字符串转义（只处理我们必须处理的那几类）。
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned>(c));
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

/// 判断一个值是不是"数字形态"（用于 state.json 不加引号）。
///
/// 【为什么不问脚本要类型】`publish(k, v)` 收的是 Lua number 或 string，
///   原语层已经把它转成字符串；这里只按形态判断，规则极小且明确：
///   能完整解析成有限数字的才算数字。
bool looks_numeric(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    return std::isfinite(v);
}

} // namespace

void channel::configure(std::string dir, std::string start_node,
                        std::string install_root) {
    start_node_ = std::move(start_node);
    dir_ = std::move(dir);
    enabled_ = !dir_.empty();
    if (!enabled_) {
        return;
    }

    const fs::path base = fs::path(dir_);
    command_path_ = (base / "command.txt").string();
    state_path_ = (fs::path(install_root) / "debug" / "state.json").string();

    // 【启动时把偏移推到文件末尾】**不吃历史命令**。
    //   控制台可能在引擎上一次运行时就写过命令；那些命令属于上一次会话。
    std::error_code ec;
    const auto sz = fs::file_size(command_path_, ec);
    offset_ = ec ? 0 : static_cast<std::uint64_t>(sz);

    log::record("control", log::format("控制通道已开启",
                            "命令文件 " + wag::paths::relativize(command_path_) + "（启动时跳过已有 " +
                                std::to_string(offset_) + " 字节）"));
}

void channel::poll() {
    if (!enabled_) {
        return;
    }
    taken_this_frame_ = 0;
    read_new_commands();
}

void channel::read_new_commands() {
    std::error_code ec;
    const auto sz_raw = fs::file_size(command_path_, ec);
    if (ec) {
        // 文件不存在 ⇒ 正常（控制台还没写过任何命令），什么也不做。
        return;
    }
    const auto sz = static_cast<std::uint64_t>(sz_raw);

    if (sz == 0) {
        offset_ = 0;
        return;
    }
    if (sz > kMaxFileBytes) {
        if (!warned_file_too_big_) {
            warned_file_too_big_ = true;
            log::warn("control", log::format(
                "命令文件超过上限（整份忽略）",
                command_path_ + " 有 " + std::to_string(sz) + " 字节，上限 " +
                    std::to_string(kMaxFileBytes) +
                    "；疑似控制台写错了文件"));
        }
        return;
    }
    if (sz < offset_) {
        // 文件被清空 / 重写 ⇒ 从头读（“变小则归零”）。
        offset_ = 0;
    }
    if (sz == offset_) {
        return;
    }

    std::ifstream in(command_path_, std::ios::binary);
    if (!in) {
        return;
    }
    in.seekg(static_cast<std::streamoff>(offset_), std::ios::beg);
    std::string chunk(static_cast<std::size_t>(sz - offset_), '\0');
    in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    chunk.resize(static_cast<std::size_t>(in.gcount()));
    if (chunk.empty()) {
        return;
    }

    // 【只消费到最后一个换行】半行留给下一次。
    const std::size_t last_nl = chunk.rfind('\n');
    if (last_nl == std::string::npos) {
        return; // 还没有完整行
    }
    const std::size_t consumed = last_nl + 1;

    std::size_t pos = 0;
    while (pos < consumed) {
        const std::size_t nl = chunk.find('\n', pos);
        const std::size_t end = (nl == std::string::npos) ? consumed : nl;
        const std::string raw = chunk.substr(pos, end - pos);
        pos = end + 1;

        const std::string line = trim_ascii(raw);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.size() > kMaxLineBytes) {
            if (!warned_long_line_) {
                warned_long_line_ = true;
                log::warn("control", log::format(
                    "命令太长（该行跳过）",
                    "一行 " + std::to_string(line.size()) + " 字节，上限 " +
                        std::to_string(kMaxLineBytes) + ""));
            }
            continue;
        }
        if (queue_.size() >= kMaxQueue) {
            if (!warned_backlog_) {
                warned_backlog_ = true;
                log::warn("control", log::format("命令队列已满", "丢弃后续命令（上限 " +
                                                          std::to_string(kMaxQueue) +
                                                          " 条）"));
            }
            break;
        }

        command c;
        const std::size_t sp = line.find(' ');
        if (sp == std::string::npos) {
            c.verb = line;
        } else {
            c.verb = line.substr(0, sp);
            c.arg = trim_ascii(line.substr(sp + 1));
        }
        if (!c.verb.empty()) {
            queue_.push_back(std::move(c));
        }
    }

    offset_ += consumed;
}

bool channel::take(std::string& verb, std::string& arg) {
    if (!enabled_ || queue_.empty()) {
        return false;
    }
    if (taken_this_frame_ >= kMaxPerFrame) {
        return false;
    }
    verb = queue_.front().verb;
    arg = queue_.front().arg;
    queue_.pop_front();
    ++taken_this_frame_;
    return true;
}

bool channel::take_if(const std::function<bool(const command&)>& predicate,
                      command& out) {
    if (!enabled_ || queue_.empty() ||
        taken_this_frame_ >= kMaxPerFrame) {
        return false;
    }
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (predicate(*it)) {
            out = *it;
            queue_.erase(it);
            ++taken_this_frame_;
            return true;
        }
    }
    return false;
}

void channel::publish(const std::string& key, const std::string& value) {
    if (!enabled_ || key.empty()) {
        return;
    }
    for (auto& kv : published_) {
        if (kv.first == key) {
            kv.second = value;
            dirty_ = true;
            return;
        }
    }
    published_.emplace_back(key, value);
    dirty_ = true;
}

void channel::flush_state(double now_seconds) {
    if (!enabled_ || !dirty_) {
        return;
    }
    if (last_write_ >= 0.0 && (now_seconds - last_write_) < kStateIntervalSeconds) {
        return;
    }

    std::error_code ec;
    fs::path p{state_path_};
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
        if (ec) {
            log::warn("control", log::format("state.json 目录创建失败", ec.message()));
            return;
        }
    }
    std::ofstream out(state_path_, std::ios::binary | std::ios::trunc);
    if (!out) {
        // 只报一次：目录不可写时不该每帧刷屏。
        if (last_write_ < 0.0) {
            log::warn("control", log::format("state.json 写入失败", wag::paths::relativize(state_path_)));
        }
        last_write_ = now_seconds;
        dirty_ = false;
        return;
    }

    out << "{\n";
    for (std::size_t i = 0; i < published_.size(); ++i) {
        const auto& kv = published_[i];
        out << "  \"" << json_escape(kv.first) << "\": ";
        if (looks_numeric(kv.second)) {
            out << kv.second;
        } else {
            out << "\"" << json_escape(kv.second) << "\"";
        }
        out << (i + 1 == published_.size() ? "\n" : ",\n");
    }
    out << "}\n";

    last_write_ = now_seconds;
    dirty_ = false;
}

} // namespace wag::control
