# =============================================================================
#  LuaWAG-Vanilla / CMakeLists.txt （顶层）
#
#  【本文件只做四件事】
#      ① 声明项目与语言
#      ② 设定全局标准与输出位置
#      ③ 拉入四块配置（选项 / 依赖 / 编译选项 / 平台链接）
#      ④ 加入源码子树
#
#  【它刻意不做的事】
#      · 不列源文件清单（那在 src/CMakeLists.txt 与各子目录里）
#      · 不写编译选项的细节（那在 cmake/compiler_flags.cmake 里）
#      · 不写依赖的细节（那在 cmake/deps.cmake 里）
#  ⇒ 每一件独立的事各有一个文件。改哪一处是明确的。
# =============================================================================

# -----------------------------------------------------------------------------
#  cmake_minimum_required
# -----------------------------------------------------------------------------
#
# 【为什么写 3.28 而不是 4.4】
#   3.28 里我们需要的功能全部具备，而写低一些能让别的机器上也能配置。
#   注意 CMake 4.x 已经不再兼容 `cmake_minimum_required(VERSION <3.5)` 的写法,
#   所以**绝不要**把这一行往 3.5 以下写。
#
# 【本机环境】CMake 4.4.4（随 MSYS2 安装）⇒ 远高于此下限。
cmake_minimum_required(VERSION 3.28)

# -----------------------------------------------------------------------------
#  项目
# -----------------------------------------------------------------------------

project(LuaWAG
    VERSION 0.2.0
    DESCRIPTION "LuaWAG 引擎（内核阶段）"
    LANGUAGES CXX)          # 只要 C++：本项目没有 C 源文件

# -----------------------------------------------------------------------------
#  版本资源（Windows VERSIONINFO）—— §3.2.3.1，2026-11-14 定
# -----------------------------------------------------------------------------
#
# 【为什么在这里】模板里的版本号全部来自上面的 `project(VERSION ...)`，
#   与 `WAG_ENGINE_VERSION` 宏同源 ⇒ 满足"版本号只能有一个来源"。
#
# 【为什么开 RC 语言】MinGW 下 .rc 由 windres 编译，CMake 需要 RC 语言启用。
#   `if(WIN32)` 让它不在别的平台上变成配置期错误。
if(WIN32)
    enable_language(RC)
    configure_file(
        ${CMAKE_SOURCE_DIR}/cmake/version.rc.in
        ${CMAKE_BINARY_DIR}/version.rc
        @ONLY)
    configure_file(
        ${CMAKE_SOURCE_DIR}/cmake/icon.rc.in
        ${CMAKE_BINARY_DIR}/icon.rc
        @ONLY)
endif()

# -----------------------------------------------------------------------------
#  全局设置
# -----------------------------------------------------------------------------

# C++20。（已定）
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 不用编译器扩展（-std=c++20 而不是 -std=gnu++20）。
# 理由：我们锁的是标准，不是某个编译器的方言。这条也让"换编译器"少一类意外。
set(CMAKE_CXX_EXTENSIONS OFF)

# 导出编译数据库。
#
# 【它是 VSCode 认路的前提】IntelliSense 从 compile_commands.json 里读到
# "每个源文件是用什么命令、什么 include 路径编译的"，于是不再标红。
# 没有它，就要手工在 .vscode/c_cpp_properties.json 里维护 include 列表 ——
# 那会变成与构建系统平行的第二份真相。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 可执行文件输出到 build/<preset>/bin/<框架目录>/。
#
# 【目录形状（2026-11-16 定）】
#     bin/
#     ├─ LuaWAG-Luminosa-ver<版本>/     ← 运行框架 = 引擎 + 脚本库
#     │   ├─ LuaWAG.exe / *.dll
#     │   ├─ Luminosa/{WagLib,Lumi,Nosa,Project}
#     │   └─ debug/                     ← 诊断导出（在 exe 旁边）
#     └─ Tools/                         ← 给作者用的工具（与框架**并行**）
#
# 【为什么框架目录带版本号】
#   版本写在**目录名**上，不写进任何代码/配置文件 —— 目录名是"事实"，
#   版本文件是"声明"，后者可以被改，前者不能自相矛盾。
#   版本号由 `project(VERSION)` 拼出来 ⇒ 仍然是单源，不会手误。
#
# 【升级约定】只覆盖 exe/dll 即可；跨框架版本时连 Luminosa 下的框架层一起换，
#   但 **Luminosa/Project 永远保留**（那是作者的工程）。
#
# 【为什么 Tools 与框架并行、而不在 Luminosa 里】
#   工具不参与运行时，不属于任何脚本层（WagLib/Lumi/Nosa/Project）。
#   放进来会让"层"这个概念多出一个没有运行期含义的成员。
set(WAG_FRAMEWORK_DIR_NAME "LuaWAG-Luminosa-ver${PROJECT_VERSION}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY
    ${CMAKE_BINARY_DIR}/bin/${WAG_FRAMEWORK_DIR_NAME})

# 单配置生成器（Ninja）下，即使不指定 --config 也要有默认构建类型。
# 多配置生成器会忽略它。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# -----------------------------------------------------------------------------
#  四块配置
# -----------------------------------------------------------------------------
#
# 【顺序有讲究】
#   options       先定开关（后面几块要读它）
#   compiler_flags 定编译选项（含"要不要把某个 include 路径关掉"这类判断）
#   deps          依赖（它可能定义导入目标，供最后链接时用）
#   platform_link 平台相关的链接列表（它可能依赖 deps 定义的目标）

include(cmake/options.cmake)
include(cmake/compiler_flags.cmake)
include(cmake/deps.cmake)
include(cmake/platform_link.cmake)
include(cmake/deploy_runtime.cmake)

# -----------------------------------------------------------------------------
#  源码子树
# -----------------------------------------------------------------------------

add_subdirectory(src)
