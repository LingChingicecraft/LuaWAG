# =============================================================================
#  src/time/CMakeLists.txt
#
#  模块：time（12 阶段、帧级可变状态、休眠、状态切换）
#  命名空间：wag::time
#
#  【来源】原 src/frame（目标名 wag_frame）。目录按命名空间所属的职责改名；
#      文件名 frame.* 不变 —— 它描述的是"帧"，是 time 这个职责里的一件东西。
#
#  【本模块为什么是"最上面"的那一个】
#      它同时需要：engine（引擎实例）、error（报错）、script（钩子调用）、
#      space（视口与变换）、platform（窗口尺寸与 DPI、换缓冲）、draw（绘制）。
#      ⇒ 它是装配点的**下一层**：main 把所有人装起来，而 time 在运行期把
#        所有人按次序串起来。
#
#  【它也是"顺序"这件事的载体】
#      12 个阶段的次序、休眠时跳哪些、阶段 11 只做状态切换 —— 全在这里。
#      ⇒ 所以它值得被读，而它的 CMakeLists.txt 值得注明这一点。
# =============================================================================

add_library(wag_time STATIC
    frame.cpp
    frame.hpp)

target_include_directories(wag_time PUBLIC ${CMAKE_SOURCE_DIR}/src)

# -----------------------------------------------------------------------------
#  依赖（本模块把其余模块几乎全部拉进来了，这是它作为"装配的下一层"的定义）
# -----------------------------------------------------------------------------
#
#   PUBLIC  wag_engine wag_error wag_log —— 原 `PUBLIC wag_core` 的一对一替换
#                           （frame.hpp 的签名里有 engine& 与 frame_state）
#   PRIVATE 其余四个     —— 只在 .cpp 里用
#
# 【为什么 engine 是 PUBLIC】
#   frame_pipeline 是 struct，它的成员函数签名里有 wag::engine& 与
#   wag::time::frame_state。任何 include frame.hpp 的翻译单元都需要 engine 的头。
#
# 【为什么 script 是 PRIVATE】
#   frame.cpp 调 script::call_hook，而 frame.hpp 里没有出现任何 script 的类型。
#   ⇒ 下一次有人想在 frame.hpp 的签名里用 script 的东西时，这一行会提醒他:
#     "那会让 time 的公开接口依赖脚本层" —— 而那多半不是他想要的。
target_link_libraries(wag_time
    PUBLIC  wag_engine wag_error wag_log
    PRIVATE wag_script wag_space wag_platform wag_draw wag_build_config
            # 【开发期工具接缝】frame.cpp include "devtools/seam.hpp"。
            #   写着 PRIVATE 是刻意的：**frame.hpp 的公开签名里没有 devtools 的任何类型**
            #   ⇒ 谁想在公开接口里用工具的东西，这一行会立刻变成编译错误。
            #   两个工具开关默认 OFF ⇒ 这条链接不引入任何符号，也不改编译结果。
            wag_devtools
            # 【诊断导出】frame.cpp 只调一次 `diagnostics::set_frame(frame_count)`，
            #   为的是让 errors.jsonl 的记录带得上帧号。
            #   同样 PRIVATE：**frame.hpp 的公开签名里没有 diagnostics 的任何类型**。
            #   默认关 ⇒ 它只是一个整数赋值，不改行为。
            wag_diagnostics)

set_target_properties(wag_time PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
