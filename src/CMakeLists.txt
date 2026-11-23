# =============================================================================
#  src/CMakeLists.txt
#
#  源码子树：把八个模块编成静态库，再汇成一个可执行文件。
#
#  【目录名与命名空间名不必相同】（《开发决策》1.2："第二层按职责划分，不按目录划分"）
#      例：src/engine 与 src/error 都在 `wag::`；src/time/frame.* 在 `wag::time`。
#      本轮拆掉的 src/core 是收纳词，与命名空间里的 `wag::core` 同理不能留。
#
#  【为什么先编静态库再汇合，而不是把所有 .cpp 一把列给可执行文件】
#      ① 每个模块**自带源文件清单** ⇒ 新增文件只改本地 CMakeLists.txt,
#         不必去动顶层那一份长清单
#      ② **模块边界在构建系统里留下痕迹** ⇒ 它不再只存在于目录名与文档里
#      ③ 依赖方向可以写成 target_link_libraries ⇒ 于是"谁依赖谁"能被检查
#      ④ 将来若要给某个模块单独写测试，它已经是一个独立目标
#
#  【本文件不做的事】
#      不列任何 .cpp —— 那些在各子目录的 CMakeLists.txt 里。
#      这样"哪个文件属于哪个模块"这个问题只有一个答案。
# =============================================================================

# -----------------------------------------------------------------------------
#  子目录（顺序只影响配置期的处理次序，不构成依赖）
# -----------------------------------------------------------------------------

add_subdirectory(log)
add_subdirectory(error)
add_subdirectory(engine)
add_subdirectory(platform)
add_subdirectory(input)
add_subdirectory(space)
add_subdirectory(draw)
add_subdirectory(text)
add_subdirectory(script)
add_subdirectory(time)
add_subdirectory(rng)
add_subdirectory(audio)
add_subdirectory(container)

# 【开发期工具的接缝】默认是一个空静态库（两个 option 都 OFF）；
# 详见 src/devtools/CMakeLists.txt 的模块头注释。
add_subdirectory(devtools)

# 【游戏开发期的诊断导出】与上面那条是**两个受众、两个目录**：
#   devtools 是"引擎作者的验证工具"，diagnostics 是"游戏作者的观测出口"。
#   两者的输出**契约权重不同**：devtools 的不是契约，diagnostics 的是。
#   ⇒ 两条依赖边都不许有（见 src/diagnostics/CMakeLists.txt）。
add_subdirectory(diagnostics)

# -----------------------------------------------------------------------------
#  可执行文件
# -----------------------------------------------------------------------------

add_executable(LuaWAG main.cpp)

# 【Debug 保留终端，Release 用 GUI 子系统】2026-11-19 定。
#   Debug：CUI，开发者能看 stderr / printf。
#   Release：GUI，双击不创建控制台；日志走 engine.log。
#   MinGW 下 WIN32_EXECUTABLE 对应 -mwindows，只改 PE 子系统，
#   入口仍是 mainCRTStartup，因此 int main(argc, argv) 保持可用。
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_target_properties(LuaWAG PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# 【Debug 独立可运行】Debug 用动态 MinGW 运行库；把三件 DLL 放到 exe 旁边，
#   这样从资源管理器双击也能启动，而不是依赖 MSYS2 的 PATH。
if(WIN32)
    get_filename_component(_wag_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
endif()
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    foreach(_wag_dll
            libgcc_s_seh-1.dll
            libstdc++-6.dll
            libwinpthread-1.dll)
        if(EXISTS "${_wag_mingw_bin}/${_wag_dll}")
            add_custom_command(TARGET LuaWAG POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_wag_mingw_bin}/${_wag_dll}"
                        $<TARGET_FILE_DIR:LuaWAG>)
        endif()
    endforeach()
endif()

# 【版本资源】由顶层 CMakeLists.txt 在 configure 期生成（`${CMAKE_BINARY_DIR}/version.rc`）。
#   加进源列表 ⇒ MinGW 下 CMake 会自动用 windres 编译它。
#   版本号来自 `project(VERSION ...)`，不在这里重复写。
if(WIN32)
    target_sources(LuaWAG PRIVATE ${CMAKE_BINARY_DIR}/version.rc)
    target_sources(LuaWAG PRIVATE ${CMAKE_BINARY_DIR}/icon.rc)
endif()

# -----------------------------------------------------------------------------
#  运行时依赖（DLL）随 exe 落到框架目录 —— 2026-11-16
# -----------------------------------------------------------------------------
#
# 【为什么需要】exe 的导入表里有 `SDL3.dll` / `lua51.dll`。
#   开发期它们能从 MSYS2 的 PATH 里找到，但**发布期不会** ——
#   ⇒ 框架目录必须自带这两件，否则"exe 旁边的目录形状"到用户那里就不完整。
#
# 【为什么用 TARGET_RUNTIME_DLLS】它由 CMake 从链接目标自己算，
#   不需要在这里手写 dll 名字/路径（那是第二份真相，加一个依赖就会漏）。
#
# 【为什么还要单独补 lua51.dll】
#   LuaJIT 在 deps.cmake 里是 `find_library` 找到的**裸路径**，不是导入目标
#   ⇒ `TARGET_RUNTIME_DLLS` 看不见它的 DLL。而 exe 的导入表里确实有它。
#   ⇒ 从库文件所在的 ucrt64 根推 `bin/lua51.dll`；找不到就跳过（不构建失败）。
set(WAG_EXTRA_RUNTIME_DLLS "")
if(WAG_LUAJIT_LIBRARY)
    get_filename_component(_wag_luajit_libdir "${WAG_LUAJIT_LIBRARY}" DIRECTORY)
    get_filename_component(_wag_ucrt_root "${_wag_luajit_libdir}" DIRECTORY)
    if(EXISTS "${_wag_ucrt_root}/bin/lua51.dll")
        list(APPEND WAG_EXTRA_RUNTIME_DLLS "${_wag_ucrt_root}/bin/lua51.dll")
    endif()
endif()

add_custom_command(TARGET LuaWAG POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:LuaWAG> ${WAG_EXTRA_RUNTIME_DLLS}
            $<TARGET_FILE_DIR:LuaWAG>
    COMMAND_EXPAND_LISTS
    COMMENT "复制运行时依赖到框架目录")

# 【传递依赖】TARGET_RUNTIME_DLLS 只给直接链接目标；SDL3.dll 自己的
#   libiconv-2.dll 不在其中。用 exe 的真实导入表递归求闭包，避免漏 DLL。
if(WIN32)
    add_custom_command(TARGET LuaWAG POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DWAG_EXE=$<TARGET_FILE:LuaWAG>
                -DWAG_DEST_DIR=$<TARGET_FILE_DIR:LuaWAG>
                -DWAG_APP_DIR=$<TARGET_FILE_DIR:LuaWAG>
                -DWAG_MINGW_BIN=${_wag_mingw_bin}
                -P ${CMAKE_SOURCE_DIR}/cmake/copy_runtime_closure.cmake
        COMMENT "补齐运行时依赖闭包（含 SDL3 的传递依赖）"
        VERBATIM)
endif()

# -----------------------------------------------------------------------------
#  静态库之间的真实依赖环
# -----------------------------------------------------------------------------
#
# engine::impl 里是三个**值成员**（window / renderer / state），而它们的析构函数
# 在各自的 .cpp 里；同时 boot_sequence 直接调 script 的三个函数。
# ⇒ 于是 engine **反向依赖** script / draw / platform，与它们对 engine 的依赖成环。
#
# GNU 链接器单遍左到右解析静态库、走过不再回头，所以环会丢符号。
# RESCAN 让链接器对这一组重扫，直到不再产生新的未解决符号。
#
# 【为什么把全部八个都列进来，而不是只列"根"】
#   因为环的成员是**一组**，而不是"一个根加几个叶子"。列出整组之后，
#   "谁在环里"这件事在构建文件里是可见的 —— 将来若有人打破了这个环，
#   删掉多余的成员会是一次**显式**动作，而不是一处静默的遗留。
# 【devtools 接缝也在这一组里】
#   WAG_DEVTOOLS_TRACE=ON 时，wag_time 里的 frame.cpp 会调用 wag::trace::*
#   （定义在 wag_trace_probe），而 wag_devtools 把 wag_trace_probe 以 PUBLIC 拉进来
#   ⇒ 这两个目标必须一起进重扫组，否则"跨库互相引用"的符号会因单遍解析而丢。
#
# 【diagnostics 也在这一组里】
#   它现在是**零依赖**（只有开关），进组只是为了让 main.cpp 能直接调它。
#   注意：进这个组**不等于**它与 devtools 有边 —— 两条互链由各自的 CMakeLists
#   的 target_link_libraries 保证不存在（组只影响链接顺序，不产生依赖语义）。
set(WAG_LINK_GROUP
    wag_time wag_script wag_draw wag_text wag_space wag_platform wag_engine
    wag_container wag_error wag_log wag_rng wag_audio wag_input wag_devtools
    wag_diagnostics)
if (TARGET wag_trace_probe)
    list(APPEND WAG_LINK_GROUP wag_trace_probe)
endif()
string(REPLACE ";" "," WAG_LINK_GROUP_STR "${WAG_LINK_GROUP}")

target_link_libraries(LuaWAG PRIVATE
    "$<LINK_GROUP:RESCAN,${WAG_LINK_GROUP_STR}>")

# 【为什么要再加一次 build_config】
#   main.cpp 自己也是一个编译单元，它同样需要 include 路径与告警选项。
#   而 wag_build_config 是 INTERFACE 目标 ⇒ 不加这一行，main.cpp 就会用
#   一份"默认的"编译选项，于是它与其余模块**编得不一样**。
target_link_libraries(LuaWAG PRIVATE wag_build_config)
