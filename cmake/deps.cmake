# =============================================================================
#  cmake/deps.cmake
#
#  外部依赖。
#
#  【本文件在本阶段的形态】
#      WAG_NO_EXTERNAL_DEPS 默认为 ON ⇒ **本文件什么都不做**。
#      而它把"将来怎么接"完整写在这里，包括三条候选路、以及它们的取舍。
#
#  【为什么"什么都不做"是对的】
#      内核目前的代码里，对外部符号的引用**全部在注释里**。
#      ⇒ 所以本轮的目标（"编译不报错"）不需要任何依赖。
#      ⇒ 而先接依赖再清语法问题，会让两件事的失败混在一起。
# =============================================================================

if(WAG_NO_EXTERNAL_DEPS)
    # -------------------------------------------------------------------------
    #  本阶段走这里。
    # -------------------------------------------------------------------------
    #
    # 不查找任何外部包，也不定义任何 "我们用了外部依赖" 的宏。
    #
    # 【它的一个有用副作用】
    #   若某个源文件不小心真的 #include 了 SDL3 或 lua 的头，
    #   那一处会**立刻编译失败**。而那是一个**该有的失败**：
    #   它说明"有人接线了，却没打开依赖开关"。
    message(STATUS "LuaWAG: WAG_NO_EXTERNAL_DEPS=ON —— 不引用任何外部依赖")
else()
    message(STATUS "LuaWAG: 查找外部依赖")

    # =========================================================================
    #  SDL3
    # =========================================================================
    find_package(SDL3 CONFIG REQUIRED)
    message(STATUS "LuaWAG: SDL3 ${SDL3_VERSION}")

    # 一个 INTERFACE 目标，携带两件事：
    #   · 链接 SDL3
    #   · 定义 WAG_HAVE_SDL3 —— window.cpp 据此决定编真实调用还是空实现
    add_library(wag_sdl3 INTERFACE)
    target_link_libraries(wag_sdl3 INTERFACE SDL3::SDL3)
    target_compile_definitions(wag_sdl3 INTERFACE WAG_HAVE_SDL3=1)

    # =========================================================================
    #  LuaJIT
    # =========================================================================
    #
    # MSYS2 的 luajit 包不提供 CMake config，所以手工找头与库。
    #
    # 【关键：有两份 lua.h】
    #   include/lua.h            是 **Lua 5.5** 的（lua 包提供）
    #   include/luajit-2.1/lua.h 才是 **LuaJIT** 的
    #   ⇒ 源码里写的是 #include <luajit-2.1/lua.h>，用版本目录钉死身份。
    #     下面那条 include 路径是为了让 luaconf.h / lualib.h 等同目录的头
    #     也能被找到。
    find_path(WAG_LUAJIT_INCLUDE_DIR
        NAMES luajit-2.1/lua.h
        PATHS ${CMAKE_PREFIX_PATH})
    find_library(WAG_LUAJIT_LIBRARY
        NAMES luajit-5.1 libluajit-5.1)

    if(NOT WAG_LUAJIT_INCLUDE_DIR OR NOT WAG_LUAJIT_LIBRARY)
        message(FATAL_ERROR
            "找不到 LuaJIT。装它：pacman -S mingw-w64-ucrt-x86_64-luajit")
    endif()
    message(STATUS "LuaWAG: LuaJIT 头 ${WAG_LUAJIT_INCLUDE_DIR}")
    message(STATUS "LuaWAG: LuaJIT 库 ${WAG_LUAJIT_LIBRARY}")

    add_library(wag_luajit INTERFACE)
    target_include_directories(wag_luajit INTERFACE
        ${WAG_LUAJIT_INCLUDE_DIR}/luajit-2.1)
    target_link_libraries(wag_luajit INTERFACE ${WAG_LUAJIT_LIBRARY})
    target_compile_definitions(wag_luajit INTERFACE WAG_HAVE_LUAJIT=1)

    # =========================================================================
    #  一、依赖清单（**这就是内核对外部世界的全部依赖面**）
    # =========================================================================
    #
    #   ① SDL3
    #        谁需要：platform/window.cpp（窗口、事件、GL 上下文）
    #        包：   ucrt64/mingw-w64-ucrt-x86_64-sdl3   3.4.18-1（已确认存在）
    #
    #   ② LuaJIT 的 C API 头（lua.h / lauxlib.h / lualib.h）
    #        谁需要：script/state.cpp、script/hook.cpp、script/primitives.cpp
    #        包：   ucrt64/mingw-w64-ucrt-x86_64-luajit 2.1.1788460057-2（已确认存在）
    #
    #   ⇒ 安装：
    #        pacman -S mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-luajit
    #
    #   ⇒ **不要混子系统**：ucrt64 的库要与 ucrt64 的 g++ 配。不要把
    #     /mingw64/lib（MSVCRT 那一套）放进链接路径，也不要把 clang64 的头
    #     放进 include 路径。混了会在链接期以"符号找不到"、或运行期以更隐晦的
    #     方式炸。
    #
    #   【其余不在内核里】
    #      FreeType、音频后端、图像解码（sdl3-image）等都等到对应子系统落地
    #      时再加。它们的存在说明**将来不需要自建** —— 同仓库里都有包。

    # =========================================================================
    #  二、SDL3
    # =========================================================================
    #
    # 【形态】MSYS2 的包带 CMake config 包 ⇒ 用 find_package
    #
    #   find_package(SDL3 CONFIG REQUIRED)
    #
    # 【静态链还是动态链 —— 待定，但倾向静态】
    #   · 静态：产物少一个 DLL；代价是要自己链 SDL3 的**传递系统库**
    #     （那一段写进 cmake/platform_link.cmake，让平台相关的链接集中一处）
    #   · 动态：由 SDL3.dll 自己解决传递依赖；代价是多一个部署文件
    #
    # 【[?] 需核实】MSYS2 的 SDL3 config 暴露的**目标名**（静态与动态各一个）
    #   在名字确认之前，下面这段保持注释状态。
    #
    #   target_link_libraries(LuaWAG PRIVATE SDL3::SDL3)          # 或 -static 那个
    #   target_compile_definitions(LuaWAG PRIVATE WAG_HAVE_SDL3=1)

    # =========================================================================
    #  三、LuaJIT
    # =========================================================================
    #
    # 【形态】取决于 MSYS2 的 luajit 包是否提供 CMake config 包
    #   · 有 ⇒ find_package(LuaJIT CONFIG REQUIRED)
    #   · 没有 ⇒ 自己找头与库并建一个导入目标（几行）
    #
    # 【[?] 需实测】
    #   · 包里是否有 libluajit.a（静态库）
    #     查法：pacman -Ql mingw-w64-ucrt-x86_64-luajit | grep '\.a$'
    #   · 包里 `ffi` 注册在 `package.preload` 还是别处
    #     ⇒ 这一条决定"运行期封堵五路"里的第 ① 路怎么写
    #
    # 【关于 FFI —— 一条已经定下来的结论，写在这里以免将来重走】
    #   MSYS2 的 luajit 包**带着 FFI**（上游默认编进去，发行版不会为人关掉它）。
    #   而我们**不做编译期关 FFI**，理由：
    #     · **编译期关掉与运行期封堵，在"能不能绕过"上是等价的**
    #     · 而编译期方案在两处更差：
    #         ① 报错信息不由我们控制 —— `require("ffi")` 会得到"模块不存在",
    #            那是一个零信息量的报错；而运行期封堵可以告诉使用者"为什么禁用、
    #            该往哪走"
    #         ② 要维护一份 LuaJIT 的补丁或自定义构建，收益为零
    #   ⇒ 所以：**走运行期五路封堵（见 runtime/Luminosa/WagLib/harden.lua）,
    #     用 MSYS2 的 luajit 包。**
    #
    #   target_link_libraries(LuaWAG PRIVATE <lua 目标>)
    #   target_compile_definitions(LuaWAG PRIVATE WAG_HAVE_LUAJIT=1)

    # =========================================================================
    #  四、一条与 CMake 版本有关的坑（现在不需要，但要知道）
    # =========================================================================
    #
    # CMake 4.x 移除了对 `cmake_minimum_required(VERSION <3.5)` 的兼容。
    # ⇒ 若将来用 FetchContent 拉进某个声明了低版本的项目，配置期会直接失败。
    # ⇒ 应对方式有二：
    #     ① 我们的顶层写 3.28（已做）
    #     ② 若某个依赖声明了低版本，用 CMAKE_POLICY_VERSION_MINIMUM 兜住
    #        【[?] 这一条我确定它存在，但不确定在当前 CMake 版本下的精确行为,
    #            需要实测。】
    #
    # 【为什么现在不会撞上】因为我们不需要拉任何第三方 CMake 项目 ——
    # 依赖全部由 pacman 提供（已确认存在）。
endif()
