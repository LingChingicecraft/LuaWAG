# =============================================================================
#  src/devtools/CMakeLists.txt
#
#  模块：devtools（**开发期工具的接缝**，不是内核的一部分）
#  命名空间：wag::devtools
#
#  【它是什么】frame.cpp 与工具之间的那一层。它做三件事：
#      ① 把工具的头（trace.hpp / img_probe.hpp）挡在引擎之外；
#      ② 提供"编译期关闭即零成本"的两条接缝（阶段边界 / present 之前）；
#      ③ 在开启时把工具的静态库拉进来。
#
#  【它不是什么】不是资源、不是调试渲染、不参与任何帧语义。
#      两个 option 默认 OFF ⇒ 默认构建里本模块是一个空的静态库，
#      frame.cpp 里的接缝全部展开成 ((void)0)。
#
#  【为什么 option 放在这里而不是 cmake/options.cmake】
#      内核规范 §16 把构建系统整体划到内核之外；而这两个开关只服务开发期工具，
#      所以它们跟着模块走，不去污染那份"引擎级开关"的清单。
# =============================================================================

option(WAG_DEVTOOLS_TRACE "把时序打点接进帧管线（编译期；默认 OFF）" OFF)
option(WAG_DEVTOOLS_PROBE "把像素采样接进帧末（编译期；默认 OFF）" OFF)

add_library(wag_devtools STATIC seam.cpp seam.hpp)

# src/ 作为 include 根 ⇒ 下游写 #include "devtools/seam.hpp"
target_include_directories(wag_devtools PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(wag_devtools PUBLIC wag_build_config)

# -----------------------------------------------------------------------------
#  WAG_DEVTOOLS_TRACE —— 时序打点
# -----------------------------------------------------------------------------
if(WAG_DEVTOOLS_TRACE)
    # trace.cpp 在 devtools/trace_probe/，本仓库里它同时被独立工程使用。
    # 这里单独建一个库，是为了让"引擎构建"与"devtools 独立构建"共用同一份实现。
    add_library(wag_trace_probe STATIC
        ${CMAKE_SOURCE_DIR}/devtools/trace_probe/trace.cpp)

    target_include_directories(wag_trace_probe PUBLIC
        ${CMAKE_SOURCE_DIR}/devtools/trace_probe)
    target_compile_definitions(wag_trace_probe PUBLIC WAG_TRACE=1)
    target_link_libraries(wag_trace_probe PUBLIC wag_build_config)

    # PUBLIC：WAG_DEVTOOLS_TRACE 必须传到 wag_time（frame.cpp 要用），
    #   而 include 路径也要传过去（seam.hpp 里 include <trace.hpp>）。
    target_compile_definitions(wag_devtools PUBLIC WAG_DEVTOOLS_TRACE=1)
    target_link_libraries(wag_devtools PUBLIC wag_trace_probe)

    message(STATUS "devtools: 时序打点 = ON（每次运行动作：退出时写 trace.csv）")
endif()

# -----------------------------------------------------------------------------
#  WAG_DEVTOOLS_PROBE —— 像素采样
# -----------------------------------------------------------------------------
if(WAG_DEVTOOLS_PROBE)
    # 只取通用层（header-only）。driver 在引擎里由 seam.cpp 自己实现
    # （它用引擎已经在用的那个 GL 上下文，不另建窗口）。
    target_include_directories(wag_devtools PRIVATE
        ${CMAKE_SOURCE_DIR}/devtools/img_probe)
    target_compile_definitions(wag_devtools PUBLIC WAG_DEVTOOLS_PROBE=1)

    # GL 1.1 的函数要显式链 opengl32；SDL3 只提供上下文与取地址接口。
    # （与 src/draw/CMakeLists.txt 里那两条同因，见那里的注释。）
    if(TARGET wag_sdl3)
        target_link_libraries(wag_devtools PRIVATE wag_sdl3)
    endif()
    if(WIN32)
        target_link_libraries(wag_devtools PRIVATE opengl32)
    endif()

    message(STATUS "devtools: 像素采样 = ON（运行时按 WAG_PROBE_EVERY 采样）")
endif()

set_target_properties(wag_devtools PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
