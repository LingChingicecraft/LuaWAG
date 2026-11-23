// =============================================================================
//  loop_logic_probe.cpp —— 直接测音频的**循环推进函数**（不是一个副本）
//
//  它 #include 的是引擎真正用的 `src/audio/loop_logic.hpp`。
//  编译：g++ -O2 -I <repo>/src -o loop_logic_probe.exe loop_logic_probe.cpp
// =============================================================================

#include <cstdio>
#include <cstdint>

#include "audio/loop_logic.hpp"

using wag::audio::wrap_position;

static void trace(const char* title, std::uint32_t loop_start, std::uint32_t loop_end,
                  bool loop, int steps) {
    std::uint64_t pos = 0;
    std::printf("%s\n  pos:", title);
    int shown = 0;
    bool alive = true;
    for (int i = 0; i < steps && alive; ++i) {
        if (shown < 12) { std::printf(" %llu", (unsigned long long)pos); ++shown; }
        alive = wrap_position(pos, loop_start, loop_end, loop);
        if (alive) { ++pos; }
    }
    std::printf(" …\n  最终: alive=%s pos=%llu（共走 %d 步）\n",
                alive ? "true" : "false", (unsigned long long)pos, steps);
}

int main() {
    // ① 整段循环（loop_start=0，loop_end=总帧数）
    trace("① 整段循环 frames=8 start=0 end=8 loop=true", 0, 8, true, 20);

    // ② 前奏 + 循环体：0..8 播一遍，之后在 [3,8) 循环
    trace("② 前奏+循环体 frames=8 start=3 end=8 loop=true", 3, 8, true, 24);

    // ③ 不循环：播到 loop_end 就结束
    trace("③ 不循环 frames=8 end=8 loop=false", 0, 8, false, 20);

    // ④ 极短循环体（1 帧）
    trace("④ 极短循环体 start=2 end=3 loop=true", 2, 3, true, 8);

    // ⑤ 边界：loop_start == loop_end 的非法区间（引擎在 play 里就 L3 拦掉，
    //    这里只证明"回绕函数不会卡死"）
    trace("⑤ 非法区间 start=4 end=4 loop=true（仅证明不死循环）", 4, 4, true, 4);

    return 0;
}
