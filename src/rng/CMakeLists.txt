# =============================================================================
#  src/rng/CMakeLists.txt
#
#  模块：rng（可序列化的随机流；splitmix64）
#  命名空间：wag::rng
#
#  【它为什么是一个独立模块，而不是塞进 time 或 space】
#      · 它不描述"时间"，也不描述"空间"——它是"可复现的随机"这一件事
#      · 本模块**不依赖任何其它模块**（连 log / error / SDL 都不依赖）
#        ⇒ 于是它能在无依赖构建里独立编译，也方便将来单独写测试
#      · 错误"怎么报"归调用方（Lua 层转 L3），本模块只返回 bool
#        ⇒ 这条边界让 rng 不会因为"日志格式变了"而被动修改
# =============================================================================

add_library(wag_rng STATIC
    rng.cpp
    rng.hpp)

target_include_directories(wag_rng PUBLIC ${CMAKE_SOURCE_DIR}/src)

# 只需要编译选项（告警等），不需要任何功能模块。
target_link_libraries(wag_rng PRIVATE wag_build_config)

set_target_properties(wag_rng PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
