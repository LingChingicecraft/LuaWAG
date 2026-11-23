// =============================================================================
//  luajit_probe.cpp —— **独立的最小对照实验**，不属于引擎源码。
//
//  【它为什么存在】
//      引擎里出现了 `lua_gettop` **返回负数**、并且**每帧 -6** 的现象（每帧
//      恰好 6 个钩子 ⇒ 每次 call_hook 让栈基抬高一格）。
//
//      那不是"少弹一个值"这种一次性错误能产生的稳定漂移 —— 它更像
//      **调用方与 LuaJIT 库对栈帧的理解不一致**（头是 luajit-2.1 的，库却
//      是动态导入的 libluajit-5.1.dll.a）。
//
//      本文件用**同一套头、同一个库、同一批编译选项**，只做最基本的几件事，
//      从而把"我们的代码错了"与"这套头/库配不上"分开。
//
//  【它刻意不做的事】
//      不包含引擎的任何头、不链接引擎的任何目标文件。它只有 LuaJIT。
//
//  【它是一次性的】
//      结论拿到之后可以删掉；它不属于交付物。
// =============================================================================

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void check(const char* what, long got, long want) {
    const bool ok = (got == want);
    if (!ok) {
        ++g_failures;
    }
    std::printf("  %-46s got=%-6ld want=%-4ld %s\n", what, got, want,
                ok ? "OK" : "**MISMATCH**");
}

/// 一个最小的 C 函数：接受任何实参，不做任何事。
int noop(lua_State*) {
    return 0;
}

/// 一个会报错的 C 函数：用来验证 pcall 的错误路径。
int boom(lua_State* L) {
    return luaL_error(L, "boom");
}

} // namespace

int main() {
    std::printf("=== LuaJIT 最小对照实验 ===\n");
    std::printf("LUA_VERSION_NUM = %d\n", LUA_VERSION_NUM);
    // 【注意这一处签名 —— 它不能靠"5.1 还是 5.2"推断】
    //   LuaJIT 2.1 的 lua.h 里写的是：
    //       LUA_API const lua_Number *lua_version (lua_State *L);   ← 返回**指针**
    //   虽然它自报 "Lua 5.1"、语义也是 5.1，但**这一处跟着 5.2+ 的签名走**。
    //   而正式 Lua 5.5 的 lua.h 反而返回 lua_Number（数值）。
    //   ⇒ 结论：这一处的形态必须看那份头，不能靠版本号推。
    //
    //   这一项有诊断价值：它让**运行期的库自己报版本**。
    const lua_Number* ver = lua_version(nullptr);
    std::printf("lua_version()   = %d\n",
                ver != nullptr ? static_cast<int>(*ver) : -1);

    lua_State* L = luaL_newstate();
    if (L == nullptr) {
        std::printf("luaL_newstate 失败\n");
        return 2;
    }

    // ---- A：装入 base 之后 ----
    std::printf("\n[A] 装入 base\n");
    lua_pushcfunction(L, luaopen_base);
    check("压入 luaopen_base 后 gettop", lua_gettop(L), 1);
    lua_call(L, 0, 1);
    check("lua_call 之后 gettop（0 参数 1 结果）", lua_gettop(L), 1);
    lua_pop(L, 1);
    check("弹出结果之后 gettop", lua_gettop(L), 0);

    // ---- B：C 函数 + 实参表（正确形态：函数先压、实参后压）----
    std::printf("\n[B] pcall：函数在 -2、实参在 -1\n");
    lua_pushcfunction(L, noop);
    lua_newtable(L);
    check("压入函数与表之后 gettop", lua_gettop(L), 2);
    std::printf("  -1 类型 = %s\n", lua_typename(L, lua_type(L, -1)));
    std::printf("  -2 类型 = %s\n", lua_typename(L, lua_type(L, -2)));

    const int rc = lua_pcall(L, 1, 0, 0);
    check("pcall 返回码", rc, 0);
    check("pcall 之后 gettop（应为 0）", lua_gettop(L), 0);

    // ---- C：报错的 C 函数（验证错误路径也归位）----
    std::printf("\n[C] pcall：被调函数会报错\n");
    lua_pushcfunction(L, boom);
    lua_newtable(L);
    const int rc2 = lua_pcall(L, 1, 0, 0);
    check("pcall 返回码（应非 0）", (rc2 != 0) ? 1 : 0, 1);
    check("错误路径之后 gettop（错误对象在栈上）", lua_gettop(L), 1);
    lua_pop(L, 1);
    check("弹掉错误对象之后 gettop", lua_gettop(L), 0);

    // ---- D：用 dostring 定义一个 Lua 函数并调用它 ----
    std::printf("\n[D] Lua 函数（经 dostring 定义）\n");
    lua_getglobal(L, "print");
    check("print 是函数（推入后 gettop）", lua_gettop(L), 1);
    lua_pop(L, 1);

    lua_getglobal(L, "dofile"); // 5.1 里 base 有这个
    check("dofile 存在（推入后 gettop）", lua_gettop(L), 1);
    lua_pop(L, 1);

    // ---- E：连续 1000 次调用，看栈是否漂移 ----
    std::printf("\n[E] 连续 1000 次 pcall（漂移检测）\n");
    const int before = lua_gettop(L);
    for (int i = 0; i < 1000; ++i) {
        lua_pushcfunction(L, noop);
        lua_newtable(L);
        const int r = lua_pcall(L, 1, 0, 0);
        if (r != 0) {
            std::printf("  第 %d 次 pcall 报错：%s\n", i,
                        lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1)
                                                       : "(非字符串)");
            lua_pop(L, 1);
            break;
        }
    }
    const int after = lua_gettop(L);
    check("1000 次调用之后 gettop", after, before);

    // =========================================================================
    // [F] 照抄引擎的 call_hook 序列 —— 这是本轮的关键一节
    //
    //  引擎的序列是（每一步都已核过）：
    //      lua_getglobal "wag"                     +1  → 1
    //      lua_getfield  "hook"                    +1  → 2
    //      lua_getfield  <槽名>                     +1  → 3
    //      lua_createtable（实参）                  +1  → 4
    //      lua_pcall(1, 0)                          -2  → 2   （吃掉函数与实参）
    //      lua_pop(3)                                   → -1  ← **弹多了？**
    //
    //  ⇒ 这一节就把这个序列原样走两遍：一遍按 pop(2)、一遍按 pop(3)，
    //    比较两种做法之后 gettop 的**符号**。
    //       pop(2) → 0     ⇒ 正确
    //       pop(3) → 负数  ⇒ "多弹一格会让栈基抬高"，即引擎那个漂移
    // =========================================================================
    std::printf("\n[F] 照抄引擎 call_hook 的序列（分别试 pop(2) 与 pop(3)）\n");

    // 先建 wag.hook.slot = noop，让序列能真的走通。
    lua_newtable(L);                 // wag
    lua_newtable(L);                 // wag.hook
    lua_pushcfunction(L, noop);
    lua_setfield(L, -2, "slot");
    lua_setfield(L, -2, "hook");
    lua_setglobal(L, "wag");
    check("建完 wag.hook 之后 gettop", lua_gettop(L), 0);

    // ---- 试法一：pop(2) ----
    for (int i = 0; i < 100; ++i) {
        lua_getglobal(L, "wag");
        lua_getfield(L, -1, "hook");
        lua_getfield(L, -1, "slot");
        lua_createtable(L, 0, 1); // 实参
        const int r = lua_pcall(L, 1, 0, 0);
        if (r != 0) {
            std::printf("  试法一第 %d 次 pcall 报错\n", i);
            lua_pop(L, 1);
            break;
        }
        lua_pop(L, 2); // ← 修法候选
    }
    const int gt_pop2 = lua_gettop(L);
    check("试法一（pop 2）× 100 次之后 gettop", gt_pop2, 0);

    // ---- 试法二：pop(3)（引擎当前的写法）----
    for (int i = 0; i < 100; ++i) {
        lua_getglobal(L, "wag");
        lua_getfield(L, -1, "hook");
        lua_getfield(L, -1, "slot");
        lua_createtable(L, 0, 1); // 实参
        const int r = lua_pcall(L, 1, 0, 0);
        if (r != 0) {
            std::printf("  试法二第 %d 次 pcall 报错\n", i);
            lua_pop(L, 1);
            break;
        }
        lua_pop(L, 3); // ← 引擎当前的写法
    }
    const int gt_pop3 = lua_gettop(L);
    std::printf("  试法二（pop 3）× 100 次之后 gettop = %d  %s\n", gt_pop3,
                (gt_pop3 >= 0) ? "（未变负）" : "**变负 ⇒ 与引擎那个漂移同源**");
    std::printf("  【判读】正确写法应让 gettop 回到 0。\n");

    lua_close(L);

    std::printf("\n=== 结论 ===\n");
    if (g_failures == 0) {
        std::printf("全部通过 ⇒ 头与库是**配得上的**，而且这套写法是对的。\n");
        std::printf("           ⇒ 引擎里的栈漂移出在**引擎自己的代码**里。\n");
    } else {
        std::printf("有 %d 项不符 ⇒ **这套头/库的组合本身就有问题**（ABI/链接方式）。\n",
                    g_failures);
    }
    return (g_failures == 0) ? 0 : 1;
}
