#include "engine/reload_status.hpp"

#include "log/log.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wag::reload {
namespace {

namespace fs = std::filesystem;

void append_json_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (const unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                static constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[(c >> 4) & 0x0F]);
                out.push_back(hex[c & 0x0F]);
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

const char* phase_name(phase p) noexcept {
    switch (p) {
    case phase::preflight: return "preflight";
    case phase::rejected: return "rejected";
    case phase::ready: return "ready";
    case phase::committing: return "committing";
    case phase::committed: return "committed";
    case phase::runtime_failed: return "runtime_failed";
    }
    return "preflight";
}

status_writer::status_writer(std::string install_root, std::string session)
    : install_root_(std::move(install_root)),
      session_(std::move(session)),
      path_((fs::path(install_root_) / "debug" / "reload_status.json").string()) {}

void status_writer::begin(std::string scope) {
    ++reload_id_;
    active_ = true;
    scope_ = std::move(scope);
    phase_ = phase::preflight;
    t_ms_ = now_ms();
    message_.clear();
    errors_.clear();
    total_errors_ = 0;
    truncated_ = false;
    old_state_usable_ = true;
    checked_ = 0;
    total_ = 0;
}

void status_writer::set_phase(phase p, std::string message,
                              bool old_state_usable) {
    phase_ = p;
    t_ms_ = now_ms();
    message_ = std::move(message);
    old_state_usable_ = old_state_usable;
}

void status_writer::set_progress(std::size_t checked, std::size_t total) {
    checked_ = checked;
    total_ = total;
    t_ms_ = now_ms();
}

void status_writer::set_errors(std::vector<issue> errors,
                              std::size_t total_errors) {
    total_errors_ = std::max(total_errors, errors.size());
    if (errors.size() > kMaxErrors) {
        errors.resize(kMaxErrors);
    }
    errors_ = std::move(errors);
    truncated_ = total_errors_ > errors_.size();
    t_ms_ = now_ms();
}

bool status_writer::write() const {
    if (!active_) {
        return false;
    }

    std::string json;
    json.reserve(512 + errors_.size() * 128);
    json += "{";
    json += "\"reload_id\":" + std::to_string(reload_id_);
    json += ",\"scope\":";
    append_json_string(json, scope_);
    json += ",\"phase\":";
    append_json_string(json, phase_name(phase_));
    json += ",\"t\":" + std::to_string(t_ms_);
    json += ",\"message\":";
    append_json_string(json, message_);
    json += ",\"errors\":[";
    for (std::size_t i = 0; i < errors_.size(); ++i) {
        if (i != 0) {
            json.push_back(',');
        }
        json += "{\"path\":";
        append_json_string(json, errors_[i].path);
        json += ",\"message\":";
        append_json_string(json, errors_[i].message);
        json.push_back('}');
    }
    json += "],\"total_errors\":" + std::to_string(total_errors_);
    json += ",\"truncated\":";
    json += truncated_ ? "true" : "false";
    json += ",\"old_state_usable\":";
    json += old_state_usable_ ? "true" : "false";
    json += ",\"session\":";
    append_json_string(json, session_);
    if (phase_ == phase::preflight) {
        json += ",\"progress\":{\"checked\":" + std::to_string(checked_) +
                ",\"total\":" + std::to_string(total_) + "}";
    }
    json += "}";

    std::error_code ec;
    const fs::path path(path_);
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        log::warn("reload", log::format("reload_status 目录创建失败", ec.message()));
        return false;
    }

    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            log::warn("reload", "reload_status 临时文件打不开");
            return false;
        }
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        out.flush();
        if (!out) {
            log::warn("reload", "reload_status 临时文件写入失败");
            return false;
        }
    }

#if defined(_WIN32)
    if (MoveFileExW(tmp.wstring().c_str(), path.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        log::warn("reload", "reload_status 原子替换失败");
        return false;
    }
#else
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        log::warn("reload", log::format("reload_status 原子替换失败", ec.message()));
        return false;
    }
#endif
    return true;
}

} // namespace wag::reload
