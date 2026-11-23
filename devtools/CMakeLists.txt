# =============================================================================
#  devtools/CMakeLists.txt —— 独立编译的开发期工具（不链引擎、不进发布物）
#
#  【为什么独立】开发决策 §2.7：引擎自己的开发期工具放引擎源码目录；
#    而这两个工具都要能脱离引擎单独用（尤其 img_probe 的判据层）。
#
#  【两个工具的共同纪律】采集与判据分离：
#    采集（抓帧 / 打点）平台相关、有副作用；
#    判据（阈值 / 事件名）纯数据、可脱机单测。
# =============================================================================
cmake_minimum_required(VERSION 3.28)

project(LuaWAG_DevTools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(WAG_DEVTOOLS_SDL3 "构建需要 SDL3 的 driver（pixel_probe 的 gl-demo）" ON)

set(WAG_DEVTOOLS_HAVE_SDL3 OFF)
if (WAG_DEVTOOLS_SDL3)
    # MSYS2 ucrt64 的默认前缀；其他环境可用 -DCMAKE_PREFIX_PATH 覆盖。
    list(APPEND CMAKE_PREFIX_PATH "C:/msys64/ucrt64")
    find_package(SDL3 CONFIG QUIET)
    if (TARGET SDL3::SDL3)
        set(WAG_DEVTOOLS_HAVE_SDL3 ON)
    else()
        message(STATUS "devtools: 没找到 SDL3 ⇒ pixel_probe 只构建 selftest / engine-check")
    endif()
endif()

# 本项目自己的代码才加告警（不污染 SDL3 的头）。
add_library(wag_devtools_flags INTERFACE)
target_compile_options(wag_devtools_flags INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)

# -----------------------------------------------------------------------------
#  img_probe（判据层：header-only，无任何依赖）
# -----------------------------------------------------------------------------
add_library(wag_img_probe INTERFACE)
target_include_directories(wag_img_probe INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/img_probe")
target_link_libraries(wag_img_probe INTERFACE wag_devtools_flags)

add_executable(img_probe_selftest img_probe/img_probe_selftest.cpp)
target_link_libraries(img_probe_selftest PRIVATE wag_img_probe)

# -----------------------------------------------------------------------------
#  pixel_probe（driver + 命令行 + engine-check）
# -----------------------------------------------------------------------------
# driver 只有找到 SDL3 才能编（它无条件 include SDL 头）。
set(PIXEL_PROBE_SOURCES
    img_probe/main.cpp
    img_probe/engine_check.cpp)
if (WAG_DEVTOOLS_HAVE_SDL3)
    list(APPEND PIXEL_PROBE_SOURCES img_probe/sdl_gl_grab.cpp)
endif()

add_executable(pixel_probe ${PIXEL_PROBE_SOURCES})
target_link_libraries(pixel_probe PRIVATE wag_img_probe)
if (WAG_DEVTOOLS_HAVE_SDL3)
    target_compile_definitions(pixel_probe PRIVATE WAG_DEVTOOLS_HAVE_SDL3=1)
    target_link_libraries(pixel_probe PRIVATE SDL3::SDL3)
    if (WIN32)
        target_link_libraries(pixel_probe PRIVATE opengl32)
        # 运行期要 SDL3.dll：拷到 exe 旁边，免得到处配 PATH。
        add_custom_command(TARGET pixel_probe POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:pixel_probe>"
            VERBATIM)
    endif()
endif()

# -----------------------------------------------------------------------------
#  trace_probe（编译期开关的两个构建，用于实测"观测扰动"）
# -----------------------------------------------------------------------------
add_library(wag_trace_off STATIC trace_probe/trace.cpp)
target_include_directories(wag_trace_off PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/trace_probe")
target_compile_definitions(wag_trace_off PUBLIC WAG_TRACE=0)
target_link_libraries(wag_trace_off PUBLIC wag_devtools_flags)

add_library(wag_trace_on STATIC trace_probe/trace.cpp)
target_include_directories(wag_trace_on PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/trace_probe")
target_compile_definitions(wag_trace_on PUBLIC WAG_TRACE=1)
target_link_libraries(wag_trace_on PUBLIC wag_devtools_flags)

add_executable(trace_demo_off trace_probe/main.cpp)
target_link_libraries(trace_demo_off PRIVATE wag_trace_off)

add_executable(trace_demo_on trace_probe/main.cpp)
target_link_libraries(trace_demo_on PRIVATE wag_trace_on)

add_executable(trace_selftest trace_probe/trace_selftest.cpp)
target_link_libraries(trace_selftest PRIVATE wag_trace_on)

# -----------------------------------------------------------------------------
#  一键跑（ctest 的最小形态：四个可执行、四条断言）
# -----------------------------------------------------------------------------
enable_testing()
add_test(NAME img_probe_selftest COMMAND img_probe_selftest)
add_test(NAME trace_selftest COMMAND trace_selftest)
add_test(NAME trace_demo_off COMMAND trace_demo_off --frames 60)
add_test(NAME trace_demo_on COMMAND trace_demo_on --frames 60)

message(STATUS "devtools: SDL3 = ${WAG_DEVTOOLS_HAVE_SDL3}")
