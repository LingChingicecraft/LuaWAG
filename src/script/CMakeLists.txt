# =============================================================================
#  src/script/CMakeLists.txt
#
#  模块：script（脚本状态、钩子协议、引擎原语）
#
#  【本模块的硬约束】
#      **对 Lua C API 的使用限制在本目录内。**
#      别的模块只看得到 script/state.hpp 里的不透明句柄（struct lua_State 的
#      前向声明），看不到 lua.h / lauxlib.h / lualib.h 里的任何东西。
#      ⇒ 这是"内核的其余部分不需要知道 Lua 的具体类型"这条在编译期的落点。
#
#  【本模块依赖 engine / error / log 而不是反过来 —— 但有一处例外要说清】
#      time/frame.cpp 与 engine/engine.cpp 都会**调用** script 的函数
#      （call_hook / install_hook_noops / …）。而 time 与 engine 都依赖 script,
#      所以那个方向是一致的：script 不依赖 time。
#      （原 core 已拆成 log / error / engine 三个模块。）
#
#      例外是 script/hook.hpp 里有一个前向声明：
#          namespace wag::time { struct frame_state; }
#      ⇒ 那只是"我需要知道有这么个类型"，不是链接依赖。
# =============================================================================

add_library(wag_script STATIC
    state.cpp
    state.hpp
    hook.cpp
    hook.hpp
    collide_hook.cpp
    collide_hook.hpp
    reload_preflight.cpp
    reload_preflight.hpp
    primitives.cpp)

target_include_directories(wag_script PUBLIC ${CMAKE_SOURCE_DIR}/src)

# -----------------------------------------------------------------------------
#  依赖
# -----------------------------------------------------------------------------
#
#   PUBLIC  wag_engine wag_error wag_log —— 原 `PUBLIC wag_core` 的一对一替换
#                      （本轮只拆目录，不重新评估 PUBLIC/PRIVATE）
#   PRIVATE wag_*    —— 其余只在 .cpp 里用
#
# 【为什么 script 不依赖 wag_space】
#   它调用 wag.space.set_viewport，但那一次调用的落地是**通过 Lua 的 C API**
#   转到 primitives.cpp 里的一个 C 函数，而那个 C 函数再调 space。
#   ⇒ 也就是说：依赖是**运行期经过 Lua** 的，不是编译期的。
#   ⇒ 而 primitives.cpp 确实需要 space::viewport ⇒ 所以下面把 space 列为
#     PRIVATE 依赖（它只在 .cpp 里用）。
#
# 【wag_draw 同理（2026-10-15，子目标 3）】
#   primitives.cpp 的 wag.draw.draw_vertices 绑定需要 draw::vertex /
#   draw::batch_capacity / find_vertex_defect（校验逻辑只有一份）⇒
#   同一条"运行期经 Lua、编译期在此"的理由 ⇒ PRIVATE。
target_link_libraries(wag_script
    PUBLIC  wag_engine wag_error wag_log
    PRIVATE wag_space wag_draw wag_rng wag_build_config)

# LuaJIT 只被本模块用。依赖打开时 wag_luajit 才存在（见 cmake/deps.cmake）。
if(TARGET wag_luajit)
    target_link_libraries(wag_script PRIVATE wag_luajit)
endif()

# -----------------------------------------------------------------------------
#  外部依赖：LuaJIT
# -----------------------------------------------------------------------------
#
# 【本阶段】WAG_NO_EXTERNAL_DEPS 为 ON ⇒ 不链接。
#   ⇒ 此刻 state.cpp / hook.cpp / primitives.cpp 里对 Lua C API 的引用**全部在
#     注释里** ⇒ 于是它们编出来是空实现 ⇒ **能编过**。
#   ⇒ 而这正是本阶段的目标：先把语法与类型问题清掉。
#
# 【将来接依赖时】这里会有类似下面的一段（形态待定，见 cmake/deps.cmake）：
#     if(NOT WAG_NO_EXTERNAL_DEPS)
#         target_link_libraries(wag_script PRIVATE <lua 目标>)
#         target_compile_definitions(wag_script PRIVATE WAG_HAVE_LUAJIT=1)
#     endif()
#
# 【而"封堵 FFI"这件事与链接无关】它是**运行期**做的（五路封堵），
#   写在 runtime/Luminosa/WagLib/harden.lua 里，不需要 LuaJIT 的任何构建选项。
#   理由见 cmake/deps.cmake 里那段关于 FFI 的注释。

set_target_properties(wag_script PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
