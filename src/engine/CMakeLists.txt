# =============================================================================
#  src/engine/CMakeLists.txt
#
#  模块：engine（引擎实例、wag.engine 四项、启动与关闭序列）
#  命名空间：wag::（顶层）
#
#  【来源】原 src/core 的一部分（core 是收纳词，已拆分，见 log/log.hpp 文件头）。
#
#  本模块**不是**纯底座：engine::impl 里的三个值成员与 boot_sequence 里的调用
#  让它反向依赖 script / draw / platform。那个环由 src/CMakeLists.txt 的
#  LINK_GROUP 处理。
#
#  【依赖】
#      PUBLIC  wag_log   —— engine.hpp 包含 log/log.hpp
#      PUBLIC  wag_space —— engine.hpp 包含 space/viewport.hpp
#      PRIVATE wag_error —— 只有 engine.cpp 用到 fail_internal
#
#  【本文件是一件"模块清单"】
#      新增或删除源文件改这里。这是"哪个文件属于哪个模块"的唯一答案。
# =============================================================================

add_library(wag_engine STATIC
    engine.cpp
    engine.hpp
    control.cpp
    control.hpp
    reload_status.cpp
    reload_status.hpp)

target_include_directories(wag_engine PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(wag_engine
    PUBLIC  wag_log wag_space wag_rng wag_audio wag_text
    PRIVATE wag_error wag_build_config)

# -----------------------------------------------------------------------------
#  【2026-10-15 已修】WAG_HAVE_SDL3 必须传到 wag_engine —— 这是"坑一"的第三次复发
#
#  engine.cpp 里有一段包在 #if defined(WAG_HAVE_SDL3) 中的代码（SDL_Init /
#  SDL_Quit，以及 2026-10-15 起的窗口暴露观察者）。而该宏由 wag_sdl3 这个
#  INTERFACE 目标携带 —— 本模块此前不链它 ⇒ 那段代码在**带依赖构建里也被
#  预处理器删掉**，症状（本次运行实测）：
#      · 日志打出"窗口创建流程结束（未接 SDL3）"—— 在带依赖构建里
#      · SDL_Init 从未执行（SDL3 的隐式初始化把症状掩盖了）
#      · 暴露观察者静默缺失
#  处置与 wag_draw 同款：链 wag_sdl3（PRIVATE）。
#  教训原文见《WagLib接线梳理》5.3 坑一：任何编译期开关都要问
#  "哪些编译单元需要看到它"，答案看"哪个 .cpp 写了那个宏的条件分支"。
# -----------------------------------------------------------------------------
if(TARGET wag_sdl3)
    target_link_libraries(wag_engine PRIVATE wag_sdl3)
endif()

set_target_properties(wag_engine PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
