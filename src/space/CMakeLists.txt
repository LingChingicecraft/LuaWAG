# =============================================================================
#  src/space/CMakeLists.txt
#
#  模块：space（矩形、映射、视口）
#
#  【本模块的特殊地位】
#      它是全项目**唯一纯逻辑**的一块：
#        · 不依赖窗口
#        · 不依赖 GPU
#        · 不依赖脚本
#        · 不依赖任何平台 API
#      输入是几个数，输出是几个数。
#
#      ⇒ 因此它是**最划算的一次隔离**：
#        "世界↔屏幕"这件事错了，整个画面就错了 —— 而它错了不会崩，只会画歪。
#        把它隔成纯函数，才可能被单独测试。
#
#  【rect.hpp 是 header-only】
#      它全是内联的小函数，没有状态、没有分配、没有平台依赖。
#      做成 .cpp 只会增加一次链接期依赖，换不到任何东西。
#      而 header-only 让它可以被单测直接 include（不需要链接整个模块）。
# =============================================================================

add_library(wag_space STATIC
    viewport.cpp
    viewport.hpp)

# 【rect.hpp 不在源文件清单里 —— 这不是遗漏】
#   它是 header-only 且只有头文件，因此它不产生任何编译单元。
#   把它列进 add_library 只会让 CMake 把它当成"未分类的源文件"并在
#   Visual Studio 生成器下多出一个筛选器节点，别处没有任何作用。
#   它仍然会被编译器正常工作 —— 因为 include 路径（src/）已经在了。

target_include_directories(wag_space PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(wag_space PRIVATE wag_build_config)

# 【本模块不依赖任何其它模块 —— 这一条是它作为"纯逻辑"的定义】
# 若将来有人在这里 target_link_libraries 了 wag_platform 或 wag_script,
# 那说明"两个矩形 → 变换"这件事被挪到了别处，而本规则要求它只在这里。

set_target_properties(wag_space PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
