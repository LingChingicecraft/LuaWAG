// =============================================================================
//  bench/pool_check.cpp
//
//  容器池的验证程序（不依赖图形/脚本；只链接 wag_container + wag_error + wag_log）。
//
//  【它验证什么】
//    ① 64 B 结构体与句柄布局（编译期 static_assert 已在 container.hpp 里）
//    ② 每池恰好 32767：第 32767 个成功，第 32768 个失败（try_allocate 返回空）
//    ③ 标记 dead 的槽**不算空位**（R21.2）：kill 之后、回收之前，try_allocate 仍返回空
//    ④ 回收后代号 +1：旧句柄失效，新句柄可用
//    ⑤ 80% 预警：进入区间记一次，回落再升上来再记一次（R22）
//    ⑥ 多池：池 id 单调、句柄按池路由
//    ⑦ L3 路径（池满 / 旧句柄）在**子进程**里验证 —— 它们会 abort，不能在本进程里跑
//
//  【怎么跑】
//    先构建（release-with-deps），再手工编译：
//      g++ -O2 -std=c++20 -I src bench/pool_check.cpp -o bench/pool_check.exe \
//          build/release-with-deps/lib/libwag_container.a \
//          build/release-with-deps/lib/libwag_error.a \
//          build/release-with-deps/lib/libwag_log.a
//    然后 `bench/pool_check.exe`。
// =============================================================================

#include "container/pool.hpp"
#include "log/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "[ok]  " : "[FAIL]", what);
    if (!ok) {
        ++failures;
    }
}

std::size_t count_substring_in_file(const std::string& path,
                                    const std::string& needle) {
    std::ifstream in(path, std::ios::binary);
    std::size_t n = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find(needle) != std::string::npos) {
            ++n;
        }
    }
    return n;
}

// 子进程入口：故意触发 L3（abort）。
// 【为什么必须子进程】L3 会 std::abort；在本进程里跑会把测试自己杀掉，
//   于是"L3 是否真的发生"就没法被断言。
int run_child(const char* mode) {
    wag::log::init(".", false, wag::log::mode::non_interactive);
    wag::container::container_pool_set set;
    const std::uint32_t pid = set.create_pool("child");

    if (std::strcmp(mode, "--expect-full") == 0) {
        for (int i = 0; i < 32768; ++i) {
            wag::container::handle h;
            (void)set.allocate(pid, h);
        }
        return 0; // 不应该到达：第 32768 次必须 abort
    }

    if (std::strcmp(mode, "--expect-stale") == 0) {
        wag::container::handle h;
        (void)set.allocate(pid, h);
        set.kill(h);
        set.recycle_dead();
        (void)set.get(h); // 旧句柄 ⇒ L3
        return 0;         // 不应该到达
    }

    return 2;
}

} // namespace

int main(int argc, char** argv) {
    // 【为什么要关掉 stdout 缓冲】验证程序会故意触发 L3（abort），
    // 缓冲里的输出会跟着丢失 —— 关掉缓冲才能看到"死在哪一条"。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc >= 2) {
        return run_child(argv[1]);
    }

    wag::log::init(".", false, wag::log::mode::non_interactive);

    using namespace wag::container;

    container_pool_set set;
    const std::uint32_t pid = set.create_pool("bullets");
    check(pid == 0, "第一个池的 id 是 0");
    check(set.pool_count() == 1, "池数为 1");

    container_pool& pool = set.pool(pid);
    check(pool.capacity() == 32767, "容量是 32767");
    check(pool.alive_count() == 0, "初始存活数为 0");

    // ---- ② 填满 32767 ----
    std::vector<handle> hs;
    hs.reserve(pool_capacity);
    bool all_ok = true;
    bool order_ok = true;
    for (std::uint32_t i = 0; i < pool_capacity; ++i) {
        handle h = invalid_handle;
        container* c = set.allocate(pid, h);
        if (c == nullptr || h == invalid_handle) {
            all_ok = false;
            break;
        }
        if (handle_pool(h) != pid || handle_index(h) != i || handle_gen(h) != 1) {
            order_ok = false;
        }
        hs.push_back(h);
    }
    check(all_ok, "32767 次分配全部成功");
    check(order_ok, "下标按 0..32766 递增、代号从 1 起、池 id 正确");
    check(pool.alive_count() == 32767, "存活数达到 32767");
    check(pool.warn_entries() == 1, "80% 预警进入了一次");

    handle extra = invalid_handle;
    check(pool.try_allocate(extra) == nullptr, "第 32768 个分配失败（池满）");
    check(extra == invalid_handle, "池满时输出句柄被复位为 invalid");

    // ---- ③ 标记 dead 不算空位 ----
    const handle h0 = hs[0];
    set.kill(h0);
    check(pool.alive_count() == 32767, "kill 之后存活数仍是 32767（dead 仍占用）");
    extra = invalid_handle;
    check(pool.try_allocate(extra) == nullptr,
          "回收之前不能复用 dead 槽（R21.2）");

    // ---- ④ 回收后代号 +1 ----
    set.recycle_dead();
    check(pool.alive_count() == 32766, "回收后存活数 32766");
    check(pool.try_get(h0) == nullptr, "旧句柄失效");

    handle h_new = invalid_handle;
    container* c_new = set.allocate(pid, h_new);
    check(c_new != nullptr, "回收后可以再分配");
    check(handle_index(h_new) == 0, "复用的是下标 0");
    check(handle_gen(h_new) == 2, "复用的槽代号 +1");
    check(h_new != h0, "新旧句柄不同");
    check(pool.alive_count() == 32767, "存活数回到 32767");

    // ---- ⑤ 80% 预警：回落再升上来再记一次 ----
    const std::uint32_t drop = 6554; // 32767 - 6554 = 26213 < 26214
    for (std::uint32_t i = 1; i <= drop; ++i) {
        set.kill(hs[i]);
    }
    set.recycle_dead();
    check(pool.alive_count() == 32767 - drop, "批量回收后降到 80% 以下");

    for (std::uint32_t i = 0; i < drop; ++i) {
        handle h = invalid_handle;
        (void)set.allocate(pid, h);
    }
    check(pool.warn_entries() == 2, "重新升到 80% 后再记一次");

    // ---- ⑥ 多池 ----
    const std::uint32_t pid2 = set.create_pool("particles");
    check(pid2 == 1, "第二个池 id 是 1（单调、不复用）");

    handle h2 = invalid_handle;
    check(set.allocate(pid2, h2) != nullptr, "第二个池可以独立分配");
    check(handle_pool(h2) == 1, "句柄里带的是第二个池的 id");
    check(set.get(h2) != nullptr, "按句柄路由到正确的池");
    // 注意：hs[1..6554] 在 80% 测试里已经被 kill+回收，这里必须挑一个**没被回收**的
    check(set.get(hs[7000]) != nullptr, "第一个池里未被回收的句柄仍然可用");

    // 第二个池的代号递增
    for (std::uint32_t round = 2; round <= 4; ++round) {
        set.kill(h2);
        set.recycle_dead();
        handle hh = invalid_handle;
        (void)set.allocate(pid2, hh);
        h2 = hh;
        if (handle_gen(h2) != round) {
            check(false, "第二个池的代号没有按 2/3/4 递增");
            break;
        }
    }
    check(handle_gen(h2) == 4, "第二个池回收三次后代号为 4");

    // ---- ⑦ L3 路径（子进程）----
    const std::string self = argv[0];
    const int full_rc =
        std::system(("\"" + self + "\" --expect-full").c_str());
    check(full_rc != 0, "池满在子进程里触发 L3（非 0 退出）");

    const int stale_rc =
        std::system(("\"" + self + "\" --expect-stale").c_str());
    check(stale_rc != 0, "旧句柄在子进程里触发 L3（非 0 退出）");

    // ---- ⑧ 日志里应当恰好有两条 80% 预警 ----
    wag::log::shutdown();
    const std::size_t warns = count_substring_in_file("engine.log", "达 80%");
    check(warns == 2, "日志里恰好两条 80% 预警");

    std::printf("\n%s（失败 %d 项）\n", failures == 0 ? "全部通过" : "有失败",
                failures);
    return failures == 0 ? 0 : 1;
}
