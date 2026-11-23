# =============================================================================
#  src/container/CMakeLists.txt
#
#  模块：container（容器的 64 B 结构体 + 池）
#  命名空间：wag::container
#
#  【它为什么是一个独立模块】
#    · 它是"个体的身份与回收"—— 不描述时间、空间、绘制；
#    · 池的规则（32767 / 多池 / 池满 L3 / 80% 预警）是**引擎不变量**，
#      必须有一个能被单独验证的落点；
#    · 依赖只有 log / error（报错方式），不依赖 draw / platform
#      ⇒ 它能在无图形环境下单独测（bench/pool_check.cpp 就是这么用的）。
# =============================================================================

add_library(wag_container STATIC
    container.hpp
    class_registry.hpp
    hit.cpp
    hit.hpp
    side_table.cpp
    side_table.hpp
    broad.cpp
    broad.hpp
    collide.cpp
    collide.hpp
    pool.cpp
    pool.hpp)

target_include_directories(wag_container PUBLIC ${CMAKE_SOURCE_DIR}/src)

# log：80% 预警（L1）；error：池满 / 句柄失效（L3）
target_link_libraries(wag_container
    PUBLIC  wag_build_config
    PRIVATE wag_log
    PRIVATE wag_error)

set_target_properties(wag_container PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
