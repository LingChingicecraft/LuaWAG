# =============================================================================
#  cmake/deploy_runtime.cmake
#
#  把脚本侧的资源复制到构建目录。
#
#  【为什么需要它】
#      引擎要在 **exe 同级** 找 Luminosa（见 platform/window.hpp 与
#      runtime/Luminosa/WagLib/init.lua）。而源码树里的 runtime/ 与构建目录
#      是两个地方 ⇒ 必须有一次复制。
#
#  【为什么开发期不直接指向源码树】
#      因为那样"exe 旁边的目录形状"就与发布期不同，而"脚本找不到"这类问题
#      只会在打包之后才暴露。让开发期就**长得像发布期**，那类问题在第一天
#      就暴露。
#
#  【两处复制（2026-11-16 定）】
#      runtime/Luminosa/  →  <框架目录>/Luminosa/      （跟着 exe 走）
#      runtime/Tools/     →  bin/Tools/                （与框架**并行**）
#    ⇒ 框架目录是 `LuaWAG-Luminosa-ver<版本>`，由 CMake 的
#      `CMAKE_RUNTIME_OUTPUT_DIRECTORY` 决定（见顶层 CMakeLists 的注释）。
# =============================================================================

if(NOT WAG_DEPLOY_RUNTIME)
    return()
endif()

set(WAG_RUNTIME_SRC ${CMAKE_SOURCE_DIR}/runtime)
set(WAG_FRAMEWORK_DST ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
set(WAG_TOOLS_DST ${CMAKE_BINARY_DIR}/bin/Tools)

# -----------------------------------------------------------------------------
#  为什么要检查存在性
# -----------------------------------------------------------------------------
#
# runtime/ 目前在仓库里（它有 10 个 .lua 文件）。但这一层检查仍然要有：
# 因为若它被清空或挪走，下面那条自定义命令会在**构建期**静默地什么都不做,
# 而"脚本没被复制"要到运行游戏时才发现。
#
# ⇒ 所以这里在**配置期**给出提示。
if(EXISTS ${WAG_RUNTIME_SRC}/Luminosa)
    add_custom_target(wag_deploy_runtime ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${WAG_RUNTIME_SRC}/Luminosa ${WAG_FRAMEWORK_DST}/Luminosa
        COMMENT "复制脚本到框架目录（${WAG_FRAMEWORK_DST}/Luminosa）"
        VERBATIM)
else()
    message(STATUS "LuaWAG: runtime/Luminosa 不存在 —— 跳过脚本复制")
endif()

# 工具：没有就跳过（作者可以不装任何工具）
if(EXISTS ${WAG_RUNTIME_SRC}/Tools)
    add_custom_target(wag_deploy_tools ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${WAG_RUNTIME_SRC}/Tools ${WAG_TOOLS_DST}
        COMMENT "复制工具到 ${WAG_TOOLS_DST}"
        VERBATIM)
endif()

# -----------------------------------------------------------------------------
#  发布期：install 规则
# -----------------------------------------------------------------------------
#
# 【为什么两份都要留】
#   开发期用 copy_directory（改脚本立刻生效，不需要真的 install）
#   发布期用 install（产物按安装前缀摆放，交给打包工具）
#
# 【[?] 安装布局尚未定】
#   已知的约定是：
#       <安装根>/LuaWAG.exe
#       <安装根>/Luminosa/WagLib/*.lua
#   ⇒ 而"<安装根>"与 CMAKE_INSTALL_PREFIX 的关系、以及
#     "<引擎工具>"（与 exe 同级、独立目录）怎么摆，都没有决策过。
#   ⇒ 所以下面这段保持注释，等布局定下来再写。
#
# install(TARGETS LuaWAG RUNTIME DESTINATION .)
# install(DIRECTORY ${WAG_RUNTIME_SRC}/Luminosa DESTINATION .)
