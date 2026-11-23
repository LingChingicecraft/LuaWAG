// =============================================================================
//  platform/utf8_path.hpp —— UTF-8 脚本路径与 OS 原生路径的唯一转换边界
//
//  【为什么要这一层】
//    Lua / JSON / 日志里的路径统一是 UTF-8 字节串；Windows 的文件 API
//    走的是宽字符。直接把 `const char*` 交给 std::ifstream / fopen，
//    会按系统 ACP 解释，中文、日文、非 ASCII 用户名目录都可能读不到。
//
//  【约定】
//    · 本文件之外的路径字符串：UTF-8
//    · Windows 文件 API：先 widen_utf8 / path_from_utf8
//    · POSIX：路径本来就是字节串，直接透传
//
//  【失败】
//    UTF-8 非法或转换失败时返回空 path / 空 wstring / nullptr。
//    调用方已有的 L2 资源失败路径负责报告，不在这里静默回退到 ACP。
// =============================================================================

#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wag::paths {

#if defined(_WIN32)

/// UTF-8 → UTF-16。非法 UTF-8 或空串返回空。
[[nodiscard]] inline std::wstring widen_utf8(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int need = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (need <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), out.data(), need);
    if (written != need) {
        return {};
    }
    return out;
}

/// UTF-16 → UTF-8。非法序列返回空。
[[nodiscard]] inline std::string narrow_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int need = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(need), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), out.data(), need, nullptr, nullptr);
    if (written != need) {
        return {};
    }
    return out;
}

#endif

/// UTF-8 路径 → 原生 filesystem::path。
[[nodiscard]] inline std::filesystem::path path_from_utf8(std::string_view text) {
#if defined(_WIN32)
    const std::wstring wide = widen_utf8(text);
    if (wide.empty()) {
        return {};
    }
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(std::string(text));
#endif
}

/// 原生 filesystem::path → UTF-8。
[[nodiscard]] inline std::string utf8_from_path(const std::filesystem::path& path) {
#if defined(_WIN32)
    return narrow_utf8(path.wstring());
#else
    return path.string();
#endif
}

/// UTF-8 路径的 fopen。Windows 走 _wfopen，POSIX 直接 fopen。
[[nodiscard]] inline std::FILE* fopen_utf8(std::string_view path, const char* mode) {
#if defined(_WIN32)
    const std::wstring wide_path = widen_utf8(path);
    const std::wstring wide_mode = widen_utf8(mode == nullptr ? "" : mode);
    if (wide_path.empty() || wide_mode.empty()) {
        return nullptr;
    }
    return _wfopen(wide_path.c_str(), wide_mode.c_str());
#else
    return std::fopen(std::string(path).c_str(), mode);
#endif
}

} // namespace wag::paths
