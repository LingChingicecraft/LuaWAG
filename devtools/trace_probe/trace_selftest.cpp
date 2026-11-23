// =============================================================================
//  trace_selftest.cpp —— 环与语义的自测（需要 WAG_TRACE=1 构建）
// =============================================================================
#include <cstdio>
#include <ostream>
#include <sstream>

#include "trace.hpp"

#if !WAG_TRACE
int main() {
    std::printf("SKIP  trace_selftest 需要 -DWAG_TRACE=1 构建\n");
    return 0;
}
#else

using namespace wag::trace;

namespace {
int g_fail = 0;

void expect_true(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        ++g_fail;
    }
}
} // namespace

int main() {
    std::printf("=== trace 自测（WAG_TRACE=%d, capacity=%zu）===\n", WAG_TRACE, capacity());

    // ---- 1. 基本顺序 ------------------------------------------------------
    reset(7);
    set_frame(7);
    for (std::uint32_t i = 0; i < 10; ++i) {
        mark(i);
    }
    expect_true(size() == 10, "写 10 条 ⇒ size == 10");
    expect_true(dropped() == 0, "没溢出 ⇒ dropped == 0");
    expect_true(at(0)->a == 0 && at(9)->a == 9, "at() 按时间顺序（0..9）");
    expect_true(at(0)->frame == 7, "帧号被写进记录");

    // ---- 2. 阶段语义：enter 之后的记录挂在那个阶段 ------------------------
    reset(0);
    enter(stage::main_logic);
    event(domain::container, 0xAA, 0xBB);
    exit(stage::main_logic);
    event(domain::container, 0xCC, 0xDD);
    expect_true(size() == 4, "enter+event+exit+event ⇒ 4 条");
    expect_true(at(1)->stage == static_cast<std::uint16_t>(stage::main_logic),
                "阶段内的事件挂在 main_logic");
    expect_true(at(1)->domain == static_cast<std::uint16_t>(domain::container), "域被记录");
    expect_true(at(3)->stage == 0, "退出阶段后的事件 stage == 0");

    // ---- 3. 溢出：丢最老的，保留最新的 -----------------------------------
    reset(0);
    const std::size_t cap = capacity();
    const std::uint32_t total = static_cast<std::uint32_t>(cap) + 100u;
    for (std::uint32_t i = 0; i < total; ++i) {
        mark(i);
    }
    expect_true(size() == cap, "溢出后 size == capacity");
    expect_true(dropped() == 100, "溢出后 dropped == 100");
    expect_true(at(0)->a == 100, "最老的一条是第 100 条（前 100 条被丢）");
    expect_true(at(cap - 1)->a == total - 1, "最新的一条是最后写入的");

    // ---- 4. 输出不为空（格式能人读、能被脚本解析）------------------------
    std::ostringstream text;
    dump_text(text, 3);
    expect_true(text.str().find("stage") != std::string::npos, "dump_text 有表头");

    std::ostringstream csv;
    dump_csv(csv, true);
    expect_true(csv.str().rfind("t_ns,frame,stage,kind,domain,a,b\n", 0) == 0, "dump_csv 有表头");
    expect_true(csv.str().find('\n', 30) != std::string::npos, "dump_csv 有数据行");

    std::printf("=== 结果：%s ===\n", g_fail == 0 ? "全部通过" : "有失败");
    return g_fail == 0 ? 0 : 1;
}
#endif
