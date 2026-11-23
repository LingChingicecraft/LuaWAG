# =============================================================================
#  cmake/compiler_flags.cmake
#
#  编译选项。集中一处，因为"警告级别"与"字符集"这类东西一旦散在多个目标里
#  就会出现"某个目标松一点"的情形，而那种不一致只在特定目标出错时才暴露。
#
#  【本文件建立的是一个 INTERFACE 目标】
#      wag_build_config —— 它不带源文件，只携带"怎么编译"。
#      所有编译单元（各模块的对象库 + 最终可执行）都链接它。
#
#  【为什么用 INTERFACE 目标，而不是给每个目标各写一遍 option】
#      因为"每个目标各写一遍"就是同一份配置的多个副本。
#      将来改一次警告级别，就要改六处 —— 而漏掉的那一处不会报错，只会静默地
#      少一套选项。INTERFACE 目标让配置只有一份。
# =============================================================================

add_library(wag_build_config INTERFACE)

# -----------------------------------------------------------------------------
#  在本项目内，永远用带 src/ 前缀的 include 写法
# -----------------------------------------------------------------------------
#
# 【这就是为什么 src/ 要作为 include 根】
#   源码里的写法统一是：
#       #include "log/log.hpp"
#       #include "space/viewport.hpp"
#       #include "time/frame.hpp"
#   ⇒ 所以只要把 src/ 加进 include 路径，全部 include 立刻可用,
#     而**一个源文件都不用改**。
#
# 【为什么是 PRIVATE 的语义（不给外部）】
#   本项目不是库，没有外部消费者 ⇒ 头文件不需要装到系统里,
#   也不需要做成 include/ 布局。那套布局是"给别人用"的信号。
target_include_directories(wag_build_config INTERFACE
    ${CMAKE_SOURCE_DIR}/src)

# -----------------------------------------------------------------------------
#  工程版本号：**唯一来源** = 顶层 `project(... VERSION ...)`
# -----------------------------------------------------------------------------
#
# 【为什么在这里定义】《控制台文档》§3.2.3 要求"版本号只能有一个来源"：
#   exe 的 VERSIONINFO 与 `session.json` 的 `engine` 必须来自同一个变量。
#   本目标被所有编译单元链接 ⇒ 它注入的宏天然只有一份。
#
# 【为什么用宏而不是运行时读文件】读文件会引入"文件丢了就报不出版本"的失败模式，
#   而且它就不是"编译期常量"了。
#
# 【它没有 R 编号】这条是控制台文档级的纪律（§3.2.3），不是容器模型的规则。
#   写在这里是为了让"引不出编号"这件事本身可见，而不是假装它有一条 R。
target_compile_definitions(wag_build_config INTERFACE
    WAG_ENGINE_VERSION="${PROJECT_VERSION}")

# -----------------------------------------------------------------------------
#  C++20
# -----------------------------------------------------------------------------
#
# 顶层已经设了 CMAKE_CXX_STANDARD 20。这里再显式写一遍，是为了让本目标
# **带动**（INTERFACE）这个要求：若将来某个子目标自己改了标准，它会与本目标
# 冲突并在配置期报错，而不是静默地各编各的。
target_compile_features(wag_build_config INTERFACE cxx_std_20)

# -----------------------------------------------------------------------------
#  UTF-8
# -----------------------------------------------------------------------------
#
# 【为什么必须显式设】
#   本项目的注释与字符串里全是中文。GCC 默认按 UTF-8 读源码，但**执行字符集**
#   在 Windows 上取决于环境 —— 而如果它被当成别的代码页，中文字符串字面量
#   会在运行时变成乱码（编译期通常不报错，所以这是**静默**的）。
#
#   ⇒ 两个都显式钉在 UTF-8：
#       -finput-charset=UTF-8   源码怎么读
#       -fexec-charset=UTF-8    字符串字面量怎么存
#
# 【为什么只对 GCC/Clang 设】
#   MSVC 用 /utf-8，而它不在本项目的目标工具链里（我们用 MSYS2 + GCC）。
#   但这个文件仍然写上了 MSVC 分支 —— 因为"默认可移植"是已定的，
#   而一份"只对当前编译器有效"的构建配置会把可移植性悄悄丢掉。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(wag_build_config INTERFACE
        -finput-charset=UTF-8
        -fexec-charset=UTF-8)
elseif(MSVC)
    target_compile_options(wag_build_config INTERFACE /utf-8)
endif()

# -----------------------------------------------------------------------------
#  告警
# -----------------------------------------------------------------------------
#
# 【已定：开高，但不当作错误】
#   理由见 cmake/options.cmake 里 WAG_WARNINGS_AS_ERRORS 的注释。
#
# 【为什么 -Wextra 而不是更激进的集合】
#   -Wall -Wextra 是"各家编译器都认、且噪音可控"的那一档。
#   更激进的（-Wconversion / -Wsign-conversion 等）在本项目的代码风格下会产生
#   大量噪音（大量 double 与 float 的互转、size_t 与 int 的互转）,
#   而那些噪音会把真正的问题淹掉。
#   ⇒ 若将来想开，应当逐个文件开，而不是全局开。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(wag_build_config INTERFACE -Wall -Wextra)
elseif(MSVC)
    target_compile_options(wag_build_config INTERFACE /W4)
endif()

if(WAG_WARNINGS_AS_ERRORS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(wag_build_config INTERFACE -Werror)
    elseif(MSVC)
        target_compile_options(wag_build_config INTERFACE /WX)
    endif()
endif()

# -----------------------------------------------------------------------------
#  输出目录（可执行与库分开）
# -----------------------------------------------------------------------------
#
# 【注意：这里**没有**把静态运行库的选项写进来】
#   它属于"发布形态"，因此写在 preset 里（见 CMakePresets.json 的 release
#   预设与 cmake/deps.cmake 的注释）。理由：Debug 构建不应该带
#   -static-libstdc++，否则调试时会看不到标准库的符号。
