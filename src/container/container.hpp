// =============================================================================
//  container/container.hpp
//
//  容器：引擎侧唯一的个体结构体（64 B）+ 句柄编码。
//
//  【这个模块负责什么】
//    一个容器就是一个可渲染、可碰撞、可回收的个体。引擎只保存“每帧会读”的
//    固定字段；脚本侧的逻辑状态放在 Lua 表里，通过句柄与这里对应。
//
//  【为什么是 64 B】
//    64 B 是一条缓存线的宽度，也是本引擎固定的容器预算：
//      · 批量遍历、碰撞筛选、渲染发射都按顺序扫这个数组；
//      · 每个对象对齐到一条缓存线，对象之间不会共享同一条线；
//      · 字段刚好装进 64 B，没有多余的堆分配或间接跳转。
//    这不是“通用最优结构”，而是本引擎的固定契约：容量、对齐、字段顺序
//    都服务于顺序扫描和批量提交。
//
//  【为什么字段顺序不能动】
//    字段按 8 / 4 / 2 / 1 字节分组，先放宽字段，再用窄字段填满尾部，
//    避免编译器插入无谓的 padding。下面的 static_assert 把每个 offsetof
//    钉成编译期事实：谁改了字段或顺序，这里先编译错，而不是运行期采到
//    错位数据。
//
//  【句柄为什么是 53 位而不是 64 位】
//    句柄要经过 Lua；Lua 数字是 double，只能精确表示到 2^53。
//    所以句柄必须同时满足：
//      · 带池 id：容器分布在多个固定容量池里；
//      · 带代号：回收后旧句柄必须失效；
//      · 总位数不超过 53。
//    下面用 index:15 + pool:12 + gen:26 切分，并在编译期检查位宽。
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>

namespace wag::container {

// -----------------------------------------------------------------------------
//  64 B 结构体
// -----------------------------------------------------------------------------
struct alignas(64) container {
    std::int64_t layer;       ///<   0  排序（层）
    float x;                  ///<   8  位置 x
    float y;                  ///<  12  位置 y
    float vx;                 ///<  16  速度 x
    float vy;                 ///<  20  速度 y
    float rot;                ///<  24  旋转（弧度）
    float sx;                 ///<  28  缩放 x（默认 1.0）
    float sy;                 ///<  32  缩放 y（默认 1.0）
    std::uint32_t rgba;       ///<  36  ARGB（小端字节序见渲染层）
    std::uint32_t img;        ///<  40  贴图 id（0 = 无贴图；出生后不换图）
    float z;                  ///<  44  深度（同组粒子需统一，否则断批）
    std::uint16_t frame;      ///<  48  图集帧号（u16）
    std::uint8_t blend;       ///<  50  混合档（8 档）
    std::uint8_t collides;    ///<  51  是否参与碰撞（出生时确定）
    std::uint8_t visible;     ///<  52  是否发射
    std::uint8_t motion;      ///<  53  运动模式（0 脚本 / 1 积分 / 2 加角速度）
    std::uint8_t group;       ///<  54  不透明分组标签
    std::uint8_t pad0;        ///<  55  对齐
    std::uint16_t class_id;   ///<  56  不透明 u16 类 id
    std::uint8_t vcolor;      ///<  58  顶点色模式（4 档）
    std::uint8_t tex_alpha;   ///<  59  纹理 alpha 档（2 档）
    std::uint32_t pad1;       ///<  60  对齐到 8
};

static_assert(sizeof(container) == 64, "容器结构体必须恰好 64 B");
static_assert(alignof(container) == 64, "容器结构体必须 64 字节对齐（一条缓存线）");

// 逐字段偏移：任何一处改动都在这里先编译错
static_assert(offsetof(container, layer) == 0);
static_assert(offsetof(container, x) == 8);
static_assert(offsetof(container, y) == 12);
static_assert(offsetof(container, vx) == 16);
static_assert(offsetof(container, vy) == 20);
static_assert(offsetof(container, rot) == 24);
static_assert(offsetof(container, sx) == 28);
static_assert(offsetof(container, sy) == 32);
static_assert(offsetof(container, rgba) == 36);
static_assert(offsetof(container, img) == 40);
static_assert(offsetof(container, z) == 44);
static_assert(offsetof(container, frame) == 48);
static_assert(offsetof(container, blend) == 50);
static_assert(offsetof(container, collides) == 51);
static_assert(offsetof(container, visible) == 52);
static_assert(offsetof(container, motion) == 53);
static_assert(offsetof(container, group) == 54);
static_assert(offsetof(container, class_id) == 56);
static_assert(offsetof(container, vcolor) == 58);
static_assert(offsetof(container, tex_alpha) == 59);

// -----------------------------------------------------------------------------
//  句柄： (gen:u26 | pool:u12 | index:u15)   —— 共 **53 位**
//
//  【为什么是 53 位，而不是 64】
//    句柄要**经过 Lua**（`lua_pushinteger` 压的是 double），而 double 只能精确
//    表示到 2^53。旧布局是 `gen28|pool12|index24`（64 位），代号从第 36 位起
//    ⇒ **代号 ≥ 2^17（131,072）就顶到 2^53**，之后：
//        · `handle_from_number` 拒收（L3"句柄不是精确整数"）⇒ 整个类的
//          `write_batch` 每帧抛错 ⇒ 该类冻结、容器杀不掉、池慢慢被填满；
//        · 更隐蔽的：库层用句柄当表键，两个句柄可能**舍入成同一个 double**。
//    实测最坏工况（一帧寿命的拖尾每帧回收）在 240 Hz 下 **9 分钟**就撞上。
//
//  【为什么能把 index 压到 15 位】
//    每池容量 32767 = 2^15 - 1 ⇒ 本来只用 15 位，旧布局白扔 9 位。
//    容量固定、不扩容，所以这个压缩不新增风险。
//    下面有一条 static_assert 把它钉住：谁改容量，编译就会错。
//
//  【换来的余量】gen 有效值 2^17 → **2^26 = 67,108,864（512×）**：
//    同一最坏工况 240 Hz 下 9 分钟 → **3.24 天**；960 Hz 下 2.3 分钟 → 19.4 小时。
//
//  【为什么是 53 位（53 = 12 + 15 + 26），不是 52 或 50】
//    这是"头寸给满、又不越界"的唯一切法：index 15 与 pool 12 都是硬需求，
//    剩下的全部给代号。
//
//  【三条纪律（改这里之前先读）】
//    ① **句柄不得被序列化**（存档 / 回放）—— 它是**派生量**（代号由回收次数决定），
//       不是数据。回放靠"同样的历史算出同样的句柄"，不需要存。
//    ② **脚本侧不得对句柄做算术**：只能传递、比较、当表键。
//       （库层现在只把它当表键，是对的。）
//    ③ **ABA 不是问题，不要为它做设计**：旧句柄要"复活"，该槽必须恰好又回收了
//       2^26 的整数倍次，**并且**有人跨那么多天还持着它。而暂停 / 菜单不产生回收
//       ⇒ 那 3.24 天是"纯玩耍时间"。
//
//  【invalid_handle = 0】
//    每个槽位的初始代号是 **1**，回收时 +1 ⇒ 0 永远不是有效句柄
//    （0 也正好是"未初始化"的自然值）。
// -----------------------------------------------------------------------------

using handle = std::uint64_t;

inline constexpr unsigned index_bits = 15;
inline constexpr unsigned pool_bits = 12;
inline constexpr unsigned gen_bits = 26;

/// 句柄总位数。**必须 ≤ 53**：句柄要经过 Lua 的 double。
inline constexpr unsigned handle_bits = index_bits + pool_bits + gen_bits;

inline constexpr std::uint32_t max_pools = 1u << pool_bits;      ///< 4096
inline constexpr std::uint32_t pool_capacity = 32767;            ///< 固定容量，不扩容
inline constexpr std::uint32_t max_index = (1u << index_bits) - 1;

// 【把"池容量"与"句柄位宽"的耦合钉死】
//   index 只有 15 位 ⇒ 容量一旦超过 32768，句柄就装不下所有槽位。
//  容量固定是设计约束；这条断言让它成为**编译期**事实，而不是注释里的约定。
static_assert(pool_capacity <= (1u << index_bits),
              "池容量装不进 index 位宽：改容量就要重切句柄（见上面那段说明）");
static_assert(handle_bits <= 53,
              "句柄超过 53 位：Lua 的 double 无法精确表示，见上面那段说明");

inline constexpr std::uint64_t index_mask = (std::uint64_t{1} << index_bits) - 1;
inline constexpr std::uint64_t pool_mask = ((std::uint64_t{1} << pool_bits) - 1) << index_bits;
inline constexpr std::uint64_t gen_mask =
    ((std::uint64_t{1} << gen_bits) - 1) << (index_bits + pool_bits);

inline constexpr handle invalid_handle = 0;

[[nodiscard]] constexpr handle make_handle(std::uint32_t pool, std::uint32_t index,
                                           std::uint32_t gen) noexcept {
    return (static_cast<std::uint64_t>(gen & ((1u << gen_bits) - 1))
            << (index_bits + pool_bits)) |
           (static_cast<std::uint64_t>(pool & ((1u << pool_bits) - 1)) << index_bits) |
           static_cast<std::uint64_t>(index & max_index);
}

[[nodiscard]] constexpr std::uint32_t handle_pool(handle h) noexcept {
    return static_cast<std::uint32_t>((h & pool_mask) >> index_bits);
}

[[nodiscard]] constexpr std::uint32_t handle_index(handle h) noexcept {
    return static_cast<std::uint32_t>(h & index_mask);
}

[[nodiscard]] constexpr std::uint32_t handle_gen(handle h) noexcept {
    return static_cast<std::uint32_t>((h & gen_mask) >> (index_bits + pool_bits));
}

/// 回收时推进代号；**跳过 0**（0 保留给 invalid_handle）。
[[nodiscard]] constexpr std::uint32_t next_gen(std::uint32_t gen) noexcept {
    std::uint32_t g = (gen + 1) & ((1u << gen_bits) - 1);
    if (g == 0) {
        g = 1;
    }
    return g;
}

} // namespace wag::container
