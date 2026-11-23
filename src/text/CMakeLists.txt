# =============================================================================
#  src/text/CMakeLists.txt
#
#  模块：text（TTF 字体与字形图集；2026-11-18）
#
#  【依赖】
#      PUBLIC  wag_draw  —— font.cpp 用 renderer 的 create_image_from_rgba /
#                           update_image_region / destroy_image_internal
#      PRIVATE wag_error / wag_log / wag_build_config（失败路径与日志）
#
#  【为什么它不是 draw 的一部分】按职责：draw 只管"把顶点发出去"，
#    字体是"字形供给"（栅格化 + 图集 + 缓存）—— 与 draw 的方向刚好相反
#    （draw 不知道字形，text 知道 draw 的图片句柄）。
#
#  【本文件是一件"模块清单"】新增或删除源文件改这里。
# =============================================================================

add_library(wag_text STATIC
    font.cpp
    font.hpp)

target_include_directories(wag_text PUBLIC ${CMAKE_SOURCE_DIR}/src)
# stb_truetype.h 在 third_party/ 下（与 stb_vorbis.c 同目录）
target_include_directories(wag_text PRIVATE ${CMAKE_SOURCE_DIR}/third_party)

target_link_libraries(wag_text
    PUBLIC  wag_draw
    PRIVATE wag_error wag_log wag_build_config)

set_target_properties(wag_text PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
