# =============================================================================
#  src/platform/CMakeLists.txt
#
#  模块：platform（窗口）
#
#  【本模块的硬约束（三条，均在 platform/window.hpp 的文件头写明）】
#      ① 不得依赖图形 API
#      ② 不得依赖脚本层
#      ③ 不得依赖 space
#
#  【⚠ 一处必须说清的限制：构建系统**不能**强制上面那三条】
#      我们所有模块共用同一个 include 根（src/），于是
#      #include "draw/renderer.hpp" 在 platform/ 里**语法上是合法的**。
#      要强制它，就得给每个模块不同的 include 路径 —— 而那些路径是相对于
#      src/ 的（"log/log.hpp"），所以那样做**反而会把带路径的写法全禁掉**。
#
#      ⇒ 所以"依赖方向"在这里是**纪律 + 文档**，不是机制。
#        这与 platform 层"不得依赖图形 API"是同一条：靠写代码的人守。
#        它也是本模块的 CMakeLists.txt 值得被注释的原因 —— 让想越界的人先读到
#        这条说明。
# =============================================================================

add_library(wag_platform STATIC
    window.cpp
    window.hpp
    system_info.cpp
    system_info.hpp)

target_include_directories(wag_platform PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(wag_platform PRIVATE wag_build_config)
if(WIN32)
    target_link_libraries(wag_platform PRIVATE user32)
endif()

# SDL3 只被本模块用。依赖打开时 wag_sdl3 才存在（见 cmake/deps.cmake）。
if(TARGET wag_sdl3)
    target_link_libraries(wag_platform PRIVATE wag_sdl3)
endif()

# platform 是唯一与操作系统打交道的一层。
# 【将来若某些源文件只用于某个平台，选择写在这里】
#     if(WIN32)
#         target_sources(wag_platform PRIVATE win_xxx.cpp)
#     endif()
# 而"平台相关的链接项"在 cmake/platform_link.cmake 里，不在这里。

set_target_properties(wag_platform PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
