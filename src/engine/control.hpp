// =============================================================================
//  engine/control.hpp
//
//  控制通道：命令入口 + state.json 出口。
//
//  【它是什么】一条**只搬运字符串**的入口 + 一个**键值出口**：
//     · 入口：控制台在**它自己的目录**里追加 `command.txt`；引擎按 tail 语义
//             读出新的一行行命令，交给脚本（`wag.control.take()`）。
//     · 出口：脚本 `publish` 键值，引擎 1 Hz 写 `<安装根>/debug/state.json`。
//
//  【引擎在里面认得什么】**只有"一行里第一个空格"**。
//    动词与参数都是不透明字符串 —— 引擎不知道 `jump` 是什么意思，
//    更不知道"结点"是什么。
//
//  【它为什么不在 diagnostics】那边是"输出"，这边是"输入"。
//    混在一起会让"关掉 --diagnostics 就什么都不写"这条判据失效。
//
//  【边界】禁止的是**隐式依赖**（控制台存在即
//    改变引擎行为），用户显式下发的命令不属于此类。可执行判据：
//    **`--control` 没给 ⇒ 本类一个字节也不读写**（日志除外，且只记一次"已开启"）。
// =============================================================================

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace wag::control {

/// 一条已到达的命令：动词 + 参数（都是**原样**字符串，不解释）。
struct command {
    std::string verb;
    std::string arg;
};

/// 控制通道。默认关闭；`configure` 时给了目录才启用。
class channel {
public:
    /// 协议额度；改动这些值时要同时检查读写两侧。
    static constexpr std::uint64_t kMaxFileBytes = 4ull * 1024ull;   ///< 整份上限
    static constexpr std::size_t kMaxLineBytes = 512;                ///< 单行上限
    static constexpr std::uint32_t kMaxPerFrame = 8;                 ///< 每帧最多取走
    static constexpr std::size_t kMaxQueue = 512;                    ///< 队列上限（防御）
    static constexpr double kStateIntervalSeconds = 1.0;             ///< state.json 节拍

    /// 配置通道。
    ///
    /// @param dir          **控制台**目录（命令文件在它下面）；空 ⇒ 关闭
    /// @param start_node   `--start-node` 的原样值（`""` = 没给）
    /// @param install_root 安装根（`state.json` 写到 `<install_root>/debug/`）
    void configure(std::string dir, std::string start_node,
                   std::string install_root);

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    /// `--start-node` 的值（没给 ⇒ 空串）。**引擎不解释它。**
    [[nodiscard]] const std::string& start_node() const noexcept {
        return start_node_;
    }

    /// 每帧一次（阶段 1）：收新命令、重置"本帧已取"计数。
    void poll();

    /// 取一条命令。本帧已取满 `kMaxPerFrame` 或队列空 ⇒ false。
    bool take(std::string& verb, std::string& arg);

    /// 从队列中取第一条满足 predicate 的命令；不满足的保持原顺序。
    /// 用于引擎自己消费 reload 命令，同时把其它命令留给脚本。
    bool take_if(const std::function<bool(const command&)>& predicate,
                 command& out);

    /// 标记一个键值（同键覆盖，保持首次出现的顺序）。引擎不解释键名。
    void publish(const std::string& key, const std::string& value);

    /// 每帧调用一次：有未写内容且距上次 ≥ `kStateIntervalSeconds` ⇒ 写文件。
    /// @param now_seconds 单调时钟秒（调用方给，便于测试与复用）
    void flush_state(double now_seconds);

private:
    /// 按 tail 语义把新追加的完整行收进队列。
    void read_new_commands();

    bool enabled_ = false;
    std::string dir_;            ///< 控制台目录（命令文件所在）
    std::string command_path_;   ///< `<dir>/command.txt`
    std::string state_path_;     ///< `<install_root>/debug/state.json`
    std::string start_node_;

    std::uint64_t offset_ = 0;   ///< 已消费到的字节位置（只增；文件变小则归零）
    std::deque<command> queue_;
    std::uint32_t taken_this_frame_ = 0;

    std::vector<std::pair<std::string, std::string>> published_;
    bool dirty_ = false;
    double last_write_ = -1.0;

    // 每个"异常"只报一次，避免每帧刷屏
    bool warned_file_too_big_ = false;
    bool warned_long_line_ = false;
    bool warned_backlog_ = false;
};

} // namespace wag::control
