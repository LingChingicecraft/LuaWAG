// =============================================================================
//  waglib_syntax_probe.cpp —— **独立的最小对照实验**，不属于引擎源码。
//
//  【它为什么存在】
//      引擎的引导脚本 `runtime/Luminosa/WagLib/*.lua` **从来没有被加载过**
//      （启动序列的步骤 6~8 仍是"待接线"）。而这带来一个具体的风险：
//      那几个 .lua 里若有语法错或 require 路径错，**要到接线那天才会爆**，
//      而接线本身是一件独立的事 —— 两件事同时出问题时归因会很难。
//
//      本探针只做一件事：
//          **把 WagLib 按它的真实加载方式跑一遍，看有没有语法错/加载错。**
//      它不驱动帧、不建窗口、不画任何东西。
//
//  【它为什么用 LuaJIT 而不是 luajit/lua 命令行】
//      因为这台机器的 PATH 里不保证有解释器，而我们**确定**有 libluajit-5.1。
//      用同一套头与库也顺便证明了"引擎用的那个 Lua 能跑这些脚本"。
//
//  【它怎么模拟 wag.* 原语】
//      WagLib 里用到的引擎原语只有 `wag.space.set_viewport` /
//      `wag.space.set_window_size` / `wag.boot.run` 三个（本探针里 stub 掉）。
//      ⇒ 于是"脚本层本身"可以被单独验证，与 C++ 侧是否接线无关。
//
//  【它是一次性的】结论拿到之后可以删掉。
// =============================================================================

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <cstdio>

namespace {

/// 记录被调用的原语，便于确认"确实走到了那一句"。
void print_and_pop(lua_State* L, const char* who) {
    const int n = lua_gettop(L);
    std::printf("      [stub] %s 被调用（%d 个参数）\n", who, n);
    lua_settop(L, 0);
}

// ---- stub：wag.space.set_viewport(t) ----
int stub_set_viewport(lua_State* L) {
    // 顺手校验一下它收到的表结构（这正是 WagLib 的真实用法）
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "render");
        std::printf("      [stub] render 表？%s\n", lua_istable(L, -1) ? "是" : "否");
        lua_pop(L, 1);
        lua_getfield(L, 1, "strategy");
        std::printf("      [stub] strategy = %s\n",
                    lua_isstring(L, -1) ? lua_tostring(L, -1) : "(非字符串)");
        lua_pop(L, 1);
    }
    lua_settop(L, 0);
    return 0;
}

int stub_set_window_size(lua_State* L) {
    const double w = luaL_checknumber(L, 1);
    const double h = luaL_checknumber(L, 2);
    std::printf("      [stub] set_window_size(%.0f, %.0f)\n", w, h);
    lua_settop(L, 0);
    return 0;
}

int stub_boot_run(lua_State* L) {
    std::printf("      [stub] wag.boot.run() —— 本探针不驱动帧，直接返回\n");
    lua_settop(L, 0);
    return 0;
}

/// 建一个最小的 wag 表（只含 WagLib 真正用到的东西）。
void build_wag_stub(lua_State* L) {
    lua_newtable(L);                       // wag

    lua_newtable(L);                       // wag.space
    lua_pushcfunction(L, stub_set_viewport);
    lua_setfield(L, -2, "set_viewport");
    lua_pushcfunction(L, stub_set_window_size);
    lua_setfield(L, -2, "set_window_size");
    lua_setfield(L, -2, "space");

    lua_newtable(L);                       // wag.boot
    lua_pushcfunction(L, stub_boot_run);
    lua_setfield(L, -2, "run");
    lua_setfield(L, -2, "boot");

    lua_setglobal(L, "wag");
}

/// 从一个 .lua 文件按字节读入并执行。返回 true 表示无错。
bool run_file(lua_State* L, const char* path) {
    std::printf("  -- %s\n", path);
    if (luaL_loadfile(L, path) != 0) {
        std::printf("     **加载失败**: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != 0) {
        std::printf("     **执行失败**: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

/// 打印一个 PascalCase 名字的表（顺带验证 lumi.screen 的接口存在）。
void dump_screen_interface(lua_State* L) {
    lua_getglobal(L, "lumi");
    if (!lua_istable(L, -1)) {
        std::printf("  !! lumi 不是表（步骤 4.5 没生效）\n");
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "screen");
    if (!lua_istable(L, -1)) {
        std::printf("  !! lumi.screen 不是表\n");
        lua_pop(L, 2);
        return;
    }
    std::printf("  lumi.screen 的成员：");
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        std::printf(" %s", lua_tostring(L, -2));
        lua_pop(L, 1);
    }
    std::printf("\n");
    lua_pop(L, 2);
}

} // namespace

int main(int argc, char** argv) {
    // 用法：waglib_syntax_probe <runtime 目录>
    const char* runtime_dir = (argc > 1) ? argv[1] : "runtime";

    lua_State* L = luaL_newstate();
    if (L == nullptr) {
        std::printf("luaL_newstate 失败\n");
        return 1;
    }
    luaL_openlibs(L);

    // ---- 把 <runtime>/?.lua 与 <runtime>/Luminosa/?.lua 加进搜索路径 ----
    //
    // 【这一行本身就是一条结论】
    //   WagLib 里的模块互相 require 用的是 `WagLib.xxx` 形式，
    //   而它们在磁盘上是 `Luminosa/WagLib/xxx.lua`。
    //   ⇒ 所以 `package.path` 里**必须**有一条能把 `WagLib.` 前缀映射到
    //     `Luminosa/WagLib/` 的模式。本探针先按最自然的那种试。
    char path[1024];
    std::snprintf(path, sizeof(path),
                  "%s/?.lua;%s/Luminosa/?.lua;%s/Luminosa/?/init.lua;%s/?.lua",
                  runtime_dir, runtime_dir, runtime_dir, runtime_dir);
    lua_getglobal(L, "package");
    lua_pushstring(L, path);
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);

    std::printf("==============================================================\n");
    std::printf(" WagLib 加载探针（不驱动帧、不建窗口）\n");
    std::printf(" runtime = %s\n", runtime_dir);
    std::printf(" package.path = %s\n", path);
    std::printf("==============================================================\n");

    build_wag_stub(L);

    // ---- 步骤 4.5 等价：装载 lumi.screen（它 require 两个新模块）----
    std::printf("\n[1] 装载 lumi.screen（含 screen_presets / screen_select）\n");
    {
        const char* code =
            "lumi = lumi or {}\n"
            "local ok, err = pcall(function()\n"
            "    lumi.screen = require('WagLib.screen_select')\n"
            "end)\n"
            "if not ok then error(err) end\n";
        if (luaL_dostring(L, code) != 0) {
            std::printf("     **失败**: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else {
            std::printf("     OK\n");
        }
    }
    dump_screen_interface(L);

    // ---- 清单自检 ----
    std::printf("\n[2] 清单自检（格式/互质/比例一致性）\n");
    {
        const char* code =
            "local ok, bad = lumi.screen.verify()\n"
            "print('     load_ok = ' .. tostring(ok) .. '   坏项数 = ' .. #bad)\n"
            "for i = 1, #bad do\n"
            "    print('       ! ' .. bad[i][1] .. ' : ' .. bad[i][2])\n"
            "end\n"
            "local names = lumi.screen.names()\n"
            "print('     可用项（已排序）: ' .. table.concat(names, ', '))\n";
        if (luaL_dostring(L, code) != 0) {
            std::printf("     **失败**: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }

    // ---- 选择一项，看它是否推给引擎 ----
    std::printf("\n[3] choose('720p') —— 应当看到 stub 收到 1280, 720\n");
    {
        const char* code =
            "local ok, err = pcall(function() lumi.screen.choose('720p') end)\n"
            "print('     choose 结果: ' .. tostring(ok) .. (ok and '' or (' ' .. tostring(err))))\n"
            "print('     current = ' .. tostring(lumi.screen.current()))\n";
        if (luaL_dostring(L, code) != 0) {
            std::printf("     **失败**: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }

    // ---- 选一个不存在的名字，看它是否报错 ----
    std::printf("\n[4] choose('不存在的名字') —— 应当报错（不是静默）\n");
    {
        const char* code =
            "local ok, err = pcall(function() lumi.screen.choose('nope') end)\n"
            "print('     ok = ' .. tostring(ok))\n"
            "print('     错误信息 = ' .. tostring(err))\n";
        if (luaL_dostring(L, code) != 0) {
            std::printf("     **失败**: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }

    // ---- next() 循环 ----
    std::printf("\n[5] next() 连按三次（循环切换）\n");
    {
        const char* code =
            "for i = 1, 3 do\n"
            "    local n = lumi.screen.next()\n"
            "    print('     第 ' .. i .. ' 次 next -> ' .. tostring(n))\n"
            "end\n";
        if (luaL_dostring(L, code) != 0) {
            std::printf("     **失败**: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }

    std::printf("\n==============================================================\n");
    std::printf(" 判读：\n");
    std::printf("  · 任何一步报『**失败**』⇒ 那个 .lua 有问题（语法或 require 路径）\n");
    std::printf("  · [2] 的『坏项数』应为 0\n");
    std::printf("  · [3] 的 stub 应当收到 1280, 720 ⇒ 说明 choose 真的推给了引擎\n");
    std::printf("  · [4] 应当 ok=false（不存在的名字必须报错，不能静默）\n");
    std::printf("==============================================================\n");

    lua_close(L);
    return 0;
}
