# =============================================================================
#  src/diagnostics/CMakeLists.txt
#
#  模块：diagnostics（**游戏开发期的诊断导出**；默认关，可随发布版编译）
#  命名空间：wag::diagnostics
#
#  【它是什么】用引擎做游戏的人的观测出口：性能、错误、节点快照。
#  【它不是什么】不是引擎作者的验证工具（那是 src/devtools），
#      不参与任何帧语义，也不读任何引擎状态去做判断。
#
#  【准入判据：三条全中才准进本目录】只输出 / 只写文件 / 不改行为。
#      可执行形式：**把本目录的代码全部删掉，发布版行为必须逐字节相同。**
#
#  【依赖边：两条都不许有】
#      wag_diagnostics ──✗──► wag_devtools
#      wag_devtools    ──✗──► wag_diagnostics
#    ⇒ 所以本文件**只有** wag_build_config 一条（它就是全局编译选项，不算模块依赖）。
#      目录分开而依赖图没分开，等于没分。
# =============================================================================

add_library(wag_diagnostics STATIC
    diagnostics.cpp
    diagnostics.hpp)

# src/ 作为 include 根：下游写 #include "diagnostics/diagnostics.hpp"。
target_include_directories(wag_diagnostics PUBLIC ${CMAKE_SOURCE_DIR}/src)

# 编译选项（C++20、UTF-8、告警）来自那一份唯一的配置。
target_link_libraries(wag_diagnostics PRIVATE wag_build_config)

# 静态库输出到统一目录（否则各模块的产物会散在各自的子目录里）。
set_target_properties(wag_diagnostics PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
