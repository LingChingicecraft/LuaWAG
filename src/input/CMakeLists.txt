# =============================================================================
#  src/input/CMakeLists.txt
#
#  模块：input（Down/Press/Release 状态机 + 鼠标 DIP 快照）
#  命名空间：wag::input
#
#  【它为什么是一个独立模块】
#    · 它是纯状态机：不认识 SDL、不认识窗口、不认识 viewport；
#    · 依赖只有 platform 的 POD 头（header-only）与 space 的 transform 头；
#    · 报错走 wag_error（scancode/button 越界 ⇒ L3）；
#    ⇒ 可以在无 SDL、无图形环境下单独测（bench/input_check.cpp）。
#
#  【不依赖 wag_platform 的库】只用到 platform/input_event.hpp 里的常量与结构体，
#    它们是纯头文件；真正产生事件的是 window.cpp，而那是 platform 模块的事。
# =============================================================================

add_library(wag_input STATIC
    input.cpp
    input.hpp)

target_include_directories(wag_input PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(wag_input
    PUBLIC  wag_build_config
    PRIVATE wag_error)

set_target_properties(wag_input PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
