# =============================================================================
#  src/log/CMakeLists.txt
#
#  模块：log（日志与诊断；错误架构的决策点）
#  命名空间：wag::log
#
#  【来源】原 src/core 的一部分（core 是收纳词，已拆分，见 log/log.hpp 文件头）。
#
#  【它是最底层】不依赖任何其它引擎模块 —— 启动最早期（任何部件建立之前）
#      也必须能记日志。
#
#  【本文件是一件"模块清单"】
#      新增或删除源文件改这里。这是"哪个文件属于哪个模块"的唯一答案。
# =============================================================================

add_library(wag_log STATIC
    log.cpp
    log.hpp)

# src/ 作为 include 根：下游写 #include "log/log.hpp"。
target_include_directories(wag_log PUBLIC ${CMAKE_SOURCE_DIR}/src)

# 编译选项（C++20、UTF-8、告警）来自那一份唯一的配置。
target_link_libraries(wag_log PRIVATE wag_build_config)

# 静态库输出到统一的目录（否则各模块的产物会散在各自的子目录里）。
set_target_properties(wag_log PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
