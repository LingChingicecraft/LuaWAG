#pragma once

#include <string>

namespace wag::platform {

/// 供普通日志“完整日志”启动段使用的系统摘要。
///
/// 【不包含】用户名、序列号、MAC、LUID、完整安装路径。
/// 【失败行为】取不到的项写“未知”，不抛异常。
struct system_summary {
    std::string os;
    std::string cpu;
    std::string gpu;
    std::string memory;
};

[[nodiscard]] system_summary get_system_summary();

} // namespace wag::platform
