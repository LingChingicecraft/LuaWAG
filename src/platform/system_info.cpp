#include "platform/system_info.hpp"

#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace wag::platform {

namespace {

#if defined(_WIN32)
std::string to_utf8(const wchar_t* text) {
    if (text == nullptr || text[0] == L'\0') {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}
#endif

} // namespace

system_summary get_system_summary() {
    system_summary out{"未知", "未知", "未知", "未知"};

#if defined(_WIN32)
    // ---- OS ----
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (GetVersionExW(&vi) != 0) {
        const char* family =
            (vi.dwBuildNumber >= 22000) ? "Windows 11" : "Windows 10";
        char buf[128]{};
        std::snprintf(buf, sizeof(buf), "%s (%lu.%lu.%lu)", family,
                      static_cast<unsigned long>(vi.dwMajorVersion),
                      static_cast<unsigned long>(vi.dwMinorVersion),
                      static_cast<unsigned long>(vi.dwBuildNumber));
        out.os = buf;
    }

    // ---- CPU ----
    wchar_t cpu[256]{};
    const DWORD cpu_n = GetEnvironmentVariableW(
        L"PROCESSOR_IDENTIFIER", cpu, static_cast<DWORD>(std::size(cpu)));
    if (cpu_n > 0) {
        out.cpu = to_utf8(cpu);
    }

    // ---- GPU（只取适配器名称；不取 LUID / 序列号）----
    std::vector<std::string> gpus;
    for (DWORD i = 0; i < 16; ++i) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        if (EnumDisplayDevicesW(nullptr, i, &dd, 0) == 0) {
            break;
        }
        const std::string name = to_utf8(dd.DeviceString);
        if (name.empty()) {
            continue;
        }
        bool seen = false;
        for (const auto& g : gpus) {
            if (g == name) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            gpus.push_back(name);
        }
    }
    if (!gpus.empty()) {
        out.gpu.clear();
        for (std::size_t i = 0; i < gpus.size(); ++i) {
            if (i != 0) {
                out.gpu += " / ";
            }
            out.gpu += gpus[i];
        }
    }

    // ---- 内存 ----
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms) != 0) {
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "%.2f GiB",
                      static_cast<double>(ms.ullTotalPhys) /
                          (1024.0 * 1024.0 * 1024.0));
        out.memory = buf;
    }
#endif

    return out;
}

} // namespace wag::platform
