# =============================================================================
#  src/error/CMakeLists.txt
#
#  模块：error（错误架构：类别、类别→级别映射、三个入口、校验辅助）
#  命名空间：wag::（顶层 —— 每个模块都要报错，归哪个第二层都不对）
#
#  【来源】原 src/core 的一部分（core 是收纳词，已拆分，见 log/log.hpp 文件头）。
#
#  【依赖】只依赖 log：error.hpp 的公开签名里有 log::severity ⇒ PUBLIC。
#
#  【本文件是一件"模块清单"】
#      新增或删除源文件改这里。这是"哪个文件属于哪个模块"的唯一答案。
# =============================================================================

add_library(wag_error STATIC
    error.cpp
    error.hpp)

target_include_directories(wag_error PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(wag_error
    PUBLIC  wag_log
    # 【诊断导出】error.cpp 写 errors.jsonl 的记录（E11）。
    #   方向是单向的：error → diagnostics。反过来会成环（见 diagnostics.hpp 的说明）。
    #   PRIVATE：error.hpp 的公开签名里没有 diagnostics 的任何类型。
    PRIVATE wag_build_config wag_diagnostics)

set_target_properties(wag_error PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
