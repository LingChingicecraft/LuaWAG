# =============================================================================
#  src/audio/CMakeLists.txt
#
#  模块：audio（音频子系统；《LuaWAG 音频设计文档》）
#  命名空间：wag::audio
#
#  【依赖】
#      PUBLIC  wag_log     —— 日志
#      PRIVATE wag_error   —— fail_boundary（加载失败 / 句柄失效都是 L3）
#      PRIVATE wag_sdl3    —— SDL3 音频流（与 wag_draw 同一条：开关必须传进来）
#
#  【为什么它是独立模块】它有自己的线程边界（SDL 音频线程）与自己的
#    数据模型（PCM / voice / 两条队列），塞进 engine 或 draw 都会把
#    "谁在跨线程"这件事搅乱。
# =============================================================================

#  【为什么要把 stb_vorbis.c 按 C++ 编译】
#     本项目 `project(... LANGUAGES CXX)` **只启用 C++**（曾经没有 C 源文件）。
#     直接塞一个 .c 进来时 CMake **不会报错，也不会编译它** ⇒ 链接期才炸
#     （undefined reference to `stb_vorbis_open_filename`）。
#     两条路：① 把 C 语言也启用；② 只让这一个文件按 C++ 编译。
#     选 ②：不动全局语言设置；而 stb_vorbis 的声明本身包在 `extern "C"` 里，
#     所以编译成 C++ 之后**链接名仍然是 C 符号**，与 audio.cpp 里的调用一致。
set_source_files_properties(${CMAKE_SOURCE_DIR}/third_party/stb_vorbis.c
    PROPERTIES LANGUAGE CXX)

add_library(wag_audio STATIC
    audio.cpp
    audio.hpp
    ${CMAKE_SOURCE_DIR}/third_party/stb_vorbis.c)

target_include_directories(wag_audio
    PUBLIC  ${CMAKE_SOURCE_DIR}/src
    PRIVATE ${CMAKE_SOURCE_DIR}/third_party)

target_link_libraries(wag_audio
    PUBLIC  wag_log
    PRIVATE wag_error wag_build_config)

if(TARGET wag_sdl3)
    target_link_libraries(wag_audio PRIVATE wag_sdl3)
endif()

set_target_properties(wag_audio PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)