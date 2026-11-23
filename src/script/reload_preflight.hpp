#pragma once

// =============================================================================
// script/reload_preflight.hpp
//
// 热重载预检：只读文件、只编译、不执行、不开第二 Lua 状态。
//
// v1 scope 只有 scripts。预检结果包含错误清单与源码缓存；
// 缓存供后续提交事务使用（本轮只做预检，不接提交）。
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wag::script {

struct state;

enum class reload_scope : std::uint8_t { scripts };

/// 只接受 "scripts"；大小写敏感、不接受空白。
[[nodiscard]] bool parse_reload_scope(std::string_view text,
                                      reload_scope& out) noexcept;

struct reload_error {
    std::string path;     ///< 相对 Luminosa 的路径，或 "Luminosa"
    std::string message;  ///< 人类可读原因
};

struct reload_source {
    std::string path;     ///< 相对 Luminosa 的 .lua 路径
    std::string source;   ///< 预检时读入的字节（提交时用同一份）
};

struct preflight_result {
    bool ok = false;
    reload_scope scope = reload_scope::scripts;
    std::size_t checked = 0;
    std::vector<reload_error> errors;
    std::vector<reload_source> sources;
};

/// 预检 `<install_root>/Luminosa` 下的全部 .lua。
///
/// 规则：
/// · 递归扫描；跳过符号链接；
/// · 必须有 `WagLib/init.lua`；各层目录缺失是允许的；
/// · 每个文件用当前 Lua 状态 luaL_loadbuffer 编译，**不执行**；
/// · 源码缓存与错误清单一起返回；
/// · 不在第一个错误处停。
preflight_result preflight_scripts(state& script_state,
                                   const std::string& install_root);

/// 把预检缓存安装成新 Lua 状态的模块搜索器（提交用）。
///
/// 在 package.loaders / package.searchers 之前插入一个 C 搜索器：
/// · 模块名命中缓存 ⇒ 用缓存源码 luaL_loadbuffer 编译并返回；
/// · 未命中 ⇒ 返回 nil，继续走原有 package.path。
///
/// 必须在 initialize_script_runtime 之后、执行引导脚本之前调用。
void install_cached_modules(state& script_state,
                            const std::vector<reload_source>& sources);

} // namespace wag::script
