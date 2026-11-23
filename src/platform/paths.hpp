// =============================================================================
// platform/paths.hpp —— **安装根**与"相对化 / 消毒"的唯一定义
//
// 【为什么要一个全局】两处需要同一个根，而它们不在同一个模块：
// · `script/state.cpp` —— 给 Lua chunk 名（`@Luminosa/Project/main.lua`）
// · `error/error.cpp` —— 给 `errors.jsonl` 的 `file` 字段
// 它只是一个**目录字符串**，没有生命周期问题；用 inline 的静态存放最省事，
// 也避免让 `wag_error` 去依赖 `wag_platform`（见 error/CMakeLists.txt）。
//
// 【锚点】= **exe 所在目录**。**只认目录，不认目录名**——
// 绝不按 `LuaWAG-Luminosa-ver…` 这种名字匹配（名字随时会变）。
// =============================================================================

#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace wag::paths {

/// 安装根的存储（inline 函数的静态局部 ⇒ 全程序唯一一份）。
inline std::string& root_storage() {
    static std::string s;
    return s;
}

/// 设定安装根（启动时一次）。
inline void set_install_root(std::string_view root) {
    std::string& s = root_storage();
    s.assign(root.data(), root.size());
 // 去掉尾部斜杠，比较时少一种情况。
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) {
        s.pop_back();
    }
}

[[nodiscard]] inline std::string_view install_root() {
    return root_storage();
}

namespace detail {

inline char lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/// 把 `s` 里的 `\` 统一成 `/`。
inline std::string slashes(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        if (c == '\\') { c = '/'; }
    }
    return out;
}

/// 取 basename（最后一个分隔符之后）。
inline std::string basename_of(std::string_view s) {
    const auto pos = s.find_last_of("/\\");
    return (pos == std::string_view::npos) ? std::string(s) : std::string(s.substr(pos + 1));
}

/// `text` 在 `pos` 处是否**不区分大小写**地等于 `pat`。
inline bool matches_at(std::string_view text, std::size_t pos, std::string_view pat) {
    if (pos + pat.size() > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i < pat.size(); ++i) {
        if (lower(text[pos + i]) != lower(pat[i])) {
            return false;
        }
    }
    return true;
}

/// 是不是"绝对路径"（`C:\`、`C:/`、`\\server`、`/x`）。
inline bool is_absolute(std::string_view p) {
    if (p.size() >= 3 && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) {
        return true;
    }
    return !p.empty() && (p.front() == '\\' || p.front() == '/');
}

} // namespace detail

/// 绝对路径 ⇒ **相对安装根**（分隔符 `/`、不带 `./`）。
///
/// 【根外怎么办】只返回 basename —— 调用方据此打 `external` 标记；
/// 本函数**永不返回绝对路径**。
[[nodiscard]] inline std::string relativize(std::string_view path) {
    const std::string norm = detail::slashes(path);
    const std::string root = detail::slashes(install_root());
    if (root.empty()) {
        return detail::basename_of(norm);
    }
    if (detail::matches_at(norm, 0, root)) {
        if (norm.size() == root.size()) {
            return ".";
        }
 // 必须紧跟分隔符才算"在根内"（防 `C:/a/bc` 误判成 `C:/a/b` 的子路径）
        if (norm[root.size()] == '/') {
            return norm.substr(root.size() + 1);
        }
    }
    return detail::basename_of(norm);
}

/// 该路径是否**在安装根之外**。
[[nodiscard]] inline bool is_external(std::string_view path) {
    const std::string norm = detail::slashes(path);
    const std::string root = detail::slashes(install_root());
    if (root.empty()) {
        return false;
    }
    if (!detail::matches_at(norm, 0, root)) {
        return detail::is_absolute(norm);
    }
    return norm.size() > root.size() && norm[root.size()] != '/';
}

/// 把一段文本里出现的"安装根前缀"换成 **`<根目录名>/`**（保留锚点，见 17.13.4）。
///
/// 【为什么需要】Lua 的错误消息、`debug.traceback`、`require` 的"找不到模块"
/// 都会把路径写进字符串；那些字符串不经过我们的 `file` 字段，只能就地洗掉。
inline std::string scrub(std::string_view text) {
    const std::string root_f = detail::slashes(install_root());
    if (root_f.empty()) {
        return std::string(text);
    }
 // 两种分隔符都要认：脚本可能拿 `install_root()` 的原始形态（反斜杠）拼路径，
 // 而 `slashes()` 只把根转成正斜杠 ⇒ 只比对一种会漏掉"整段绝对路径"那一类。
    std::string root_b = root_f;
    for (char& c : root_b) {
        if (c == '/') {
            c = '\\';
        }
    }
    std::string out(text);
    const std::string base = detail::basename_of(root_f);

 // ① 完整安装根前缀（"根 + 一个分隔符"）→ **`<根目录名>/`**（保留锚点，不是删掉）
 //
 // 【为什么保留目录名】只留相对路径时，一旦消息被 Lua 截断就成了
 // `...uminosa-ver0.1.0/...` —— 半个目录名，读的人分不清"这是哪个目录"。
 // 保留完整目录名之后，一眼能看出"这是安装根下的路径"。
    const std::string* roots[2] = {&root_f, &root_b};
    for (const std::string* rp : roots) {
        const std::string& root = *rp;
        std::size_t from = 0;
        while (!base.empty()) {
            bool found = false;
            for (std::size_t i = from; i + root.size() < out.size(); ++i) {
                if (!detail::matches_at(out, i, root)) {
                    continue;
                }
                const char next = out[i + root.size()];
                if (next != '/' && next != '\\') {
                    continue;
                }
                out.replace(i, root.size() + 1, base + "/");
                from = i + base.size() + 1;
                found = true;
                break;
            }
            if (!found) {
                break;
            }
        }
    }

 // ② 被 Lua 截断过的形态：`...<尾巴>/`
 //
 // Lua 的 `luaO_chunkid` 在源名超过 60 字符时保留尾部并加 `...`
 // ⇒ 完整前缀不存在，只剩"… + 目录名的一段尾巴 + 分隔符"。
 // 尾巴可能是**完整目录名**，也可能只是它的**后缀**（`...uminosa-ver0.1.0/`）。
 // ⇒ 判据：`...` 到下一个分隔符之间的那段，是 base 的后缀（含"相等"与"空"）
 // ⇒ 整段替换成 `<base>/`。长度门槛 6 字符，避免把正文里的 `...` 误伤。
    if (!base.empty()) {
        std::size_t pos = 0;
        while ((pos = out.find("...", pos)) != std::string::npos) {
            const std::size_t seg_begin = pos + 3;
            std::size_t seg_end = seg_begin;
            while (seg_end < out.size() && out[seg_end] != '/' && out[seg_end] != '\\') {
                ++seg_end;
            }
            if (seg_end >= out.size()) {
                ++pos;
                continue;
            }
            const std::size_t seg_len = seg_end - seg_begin;
            bool match = (seg_len == 0);
            if (!match && seg_len >= 6 && seg_len <= base.size()) {
 // seg 必须是 base 的后缀（大小写不敏感）
                match = detail::matches_at(base, base.size() - seg_len, std::string_view(out).substr(seg_begin, seg_len));
            }
            if (!match) {
                ++pos;
                continue;
            }
            out.replace(pos, seg_end - pos + 1, base + "/");
            pos = pos + base.size() + 1;
        }
    }
    return out;
}

} // namespace wag::paths
