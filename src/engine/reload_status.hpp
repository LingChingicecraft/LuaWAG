#pragma once

// =============================================================================
// engine/reload_status.hpp
//
// 热重载状态文件协议（/ 设计说明）。
//
// 引擎写、控制台只读；只有收到 reload 才会创建/改写。
// v1 单文件只保留最近一次 reload；历史进日志。
//
// 本类只负责"把当前快照写成 JSON"，不负责触发重载。
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wag::reload {

enum class phase : std::uint8_t {
    preflight,
    rejected,
    ready,
    committing,
    committed,
    runtime_failed,
};

[[nodiscard]] const char* phase_name(phase p) noexcept;

struct issue {
    std::string path;
    std::string message;
};

/// 写 `<install_root>/debug/reload_status.json`。
class status_writer {
public:
    static constexpr std::size_t kMaxErrors = 64;

    status_writer(std::string install_root, std::string session);

 /// 新一次 reload：reload_id 自增，phase=preflight，清空错误/进度。
    void begin(std::string scope);

    void set_phase(phase p, std::string message, bool old_state_usable);
    void set_progress(std::size_t checked, std::size_t total);
    void set_errors(std::vector<issue> errors, std::size_t total_errors);

 /// 原子替换写盘。未 begin 或写失败 ⇒ false。
    [[nodiscard]] bool write() const;

    [[nodiscard]] std::uint64_t reload_id() const noexcept { return reload_id_; }
    [[nodiscard]] phase current_phase() const noexcept { return phase_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    std::string install_root_;
    std::string session_;
    std::string path_;
    std::uint64_t reload_id_ = 0;
    bool active_ = false;

    std::string scope_ = "scripts";
    phase phase_ = phase::preflight;
    std::int64_t t_ms_ = 0;
    std::string message_;
    std::vector<issue> errors_;
    std::size_t total_errors_ = 0;
    bool truncated_ = false;
    bool old_state_usable_ = true;
    std::size_t checked_ = 0;
    std::size_t total_ = 0;
};

} // namespace wag::reload
