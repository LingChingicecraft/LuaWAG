#include <map>
// =============================================================================
// wag/draw/renderer.hpp
//
// 内核：渲染器。
//
// 【职责边界】
// 接受一个已经算好的变换，把顶点按层次/深度排序、分批提交到 GPU。
// 它不知道窗口，也不自己推导“两个矩形 → 映射”。
//
// 【为什么变换是参数，而不是内部状态】
// 如果渲染器自己从两个矩形算映射，窗口和渲染器就都知道了“矩形”这件事，
// 改映射要改两处。变换作为参数传入，天然支持“一批一个变换”。
//
// 【关于显示比例（display_scale）】
// 它来自 Windows 的 UI 缩放，仅作诊断；不参与任何几何换算。
// 窗口尺寸与绘制表面像素各自由平台报；本平台两者相等只是因为未启用
// 高像素密度，不是“DIP 等于像素”。
//
// 【坐标链】
// 逻辑坐标（double，项目逻辑单位，中心原点、+y 上）
// → 变换（由 space 提供）
// → 窗口坐标（窗口尺寸；左上原点、+y 下）
// → 转 float 写进顶点缓冲
// → framebuffer_box_for 做第二次 y 翻转，再交给 glViewport / glScissor
// 两次翻转净效果是不翻；删掉任意一次，画面都会上下颠倒。
//
// 【模块内容】
// 资源（图片/着色器/帧表）、批状态、绘制入口、统计、快照与热重载清理。
// =============================================================================

#pragma once

#include "draw/gl_api.hpp"
#include "light/light.hpp"
#include "light/occluder_pool.hpp"
#include "platform/window.hpp"
#include "space/rect.hpp"

// 【render_stage 的前置声明】定义在 engine/engine.hpp（带底层类型的枚举
// 前置声明合法）—— 绘制收集只把它当"阶段"排序键的值用，不需要引擎头。
namespace wag {
enum class render_stage : std::uint8_t;
}

#include <string>
#include <unordered_map>
#include <vector>
// 【为什么需要 viewport.hpp】因为它提供 `space::framebuffer_box` 与
// `space::framebuffer_box_for` —— 那是"窗口坐标矩形 → GL 盒子"的**唯一实现**，
// 而 `renderer.cpp` 要用它（而不是自己再写一次那个减法）。
// ⚠️ 这条 include 是本轮加接线时**漏掉的**：那个类型声明在 viewport.hpp，
// 而本文件当时只含了 rect.hpp ⇒ 于是 `space::framebuffer_box` 编不过。
#include "space/viewport.hpp"

#include <cstddef>
#include <cstdint>

namespace wag::draw {

/// 呈现策略：保持比例 / 拉满输出。
enum class present_mode : std::uint8_t {
    letterbox = 0,
    stretch = 1,
};

/// 呈现滤波。
enum class present_filter : std::uint8_t {
    nearest = 0,
    linear = 1,
};

// -----------------------------------------------------------------------------
// vertex —— 一个顶点
//
// 【字段的顺序与含义（已定）】
// x, y, z —— 位置
// u, v —— 纹理坐标
// color —— 该顶点的颜色
//
// 【三条容易记错的约定】
// ① **UV 的原点在图像左上角**：u 向右增大，v 向下增大
// ② **UV 的单位是像素，不是 0–1 的归一化值**
// ③ **颜色是逐顶点的** —— 因此可以做出渐变
//
// 【精度】位置与颜色用 float，因为它是最终送进顶点缓冲的东西。
// 而"逻辑坐标与变换过程中的计算使用双精度"这件事发生在**本结构体之前**:
// 写进这里的时候已经换算完了。
//
// 【已定：**AoS**（2026 定案，原为"待定"）】
// 一个顶点的字段挨在一起。定案理由两条：
// ① 顶点装配是**顺序读**（位置 + UV + 颜色 + 页一起要）⇒ 缓存友好；
// ② 渲染字段与几何字段**同放一个槽位**（同一判据）。
// ⇒ **SoA 那条路已否**：拆成并行数组会把装配变成多路随机访问。
// -----------------------------------------------------------------------------
/// 一个帧矩形（**纹理像素**；左上原点、+y 向下，与取图矩形同一套口径）。
///
/// 【为什么在 namespace 作用域而不是类内】脚本原语层要构造它（`define_frames`），
/// 类内私有类型拿不到；而它本来就是一个纯数据形状，没有封装价值。
struct frame_rect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
};

struct vertex {
    float x = 0.0f;   ///< 位置 x（物理像素）
    float y = 0.0f;   ///< 位置 y（物理像素）
    float z = 0.0f;   ///< 深度；+z 向前
    float u = 0.0f;   ///< 纹理坐标 u（**像素**，原点在图像左上角）
    float v = 0.0f;   ///< 纹理坐标 v（**像素**，向下增大）
    std::uint32_t color = 0xFFFFFFFFu; ///< 该顶点的颜色（ARGB）
};

/// 取图矩形：图上的哪一块（**纹理像素**，与 `vertex::u/v` 同单位）。
///
/// 【为什么单位是像素】UV 的归一化只发生在**顶点着色器**里（`u_tex_inv_size`）——
/// 引擎里"像素 UV"是唯一契约，脚本侧不需要知道每张图多大。
///
/// 【w/h 可以是负数 / 0】与 `draw_image` 的 w/h 同一判据：
/// 负 = 镜像（UV 反向），0 = 零面积 = 什么也不出。**只查有限值，不查正负。**
struct source_rect {
    double u = 0.0;   ///< 左上角 x（纹理像素）
    double v = 0.0;   ///< 左上角 y（纹理像素）
    double w = 0.0;   ///< 宽（纹理像素；负 = 镜像）
    double h = 0.0;   ///< 高（纹理像素；负 = 镜像）
};

// -----------------------------------------------------------------------------
// shape_vertex_limit / batch_buffer_capacity —— 批次的三个数（2026 定案）
//
// 【三个数管三层】
// ① **shape_vertex_limit = 32** 单笔**图元**的顶点上限（对外，语义边界）
// ② **batch_buffer_capacity = 8192** **批缓冲**容量（对内，实现参数）
// ③ 断言：② ≥ ①（否则一个合法单笔放不下 ⇒ 越界写）
//
// 【⚠️ 2026 更正：旧文"一次绘制调用最多 32 个顶点"是错的】
// 它把 ① 与 ② 混成了一个数。正确语义：**32 管"一个图元（一笔形状）"，
// 不管"一次调用"** —— 一次调用可以提交多个图元（draw_glyphs 就是
// n 图元一次调用）。旧文自证其错："64 顶点 ≈ 每 4 个四边形提交一次
// ⇒ 那等于把批处理废掉" ⇒ 若 32 是调用容量，批处理正是被它废掉的。
//
// 【① 为什么是 32】参数少 ⇒ 栈索引错不了；"20+ 顶点说明你在画轮廓或网格
// ⇒ 那不是这套机制的服务对象"。
// 【② 的性质】**实现参数，不是对外契约**：改它只改变每帧提交次数，
// 不改变画面、顺序与正确性。8192 = 2 的幂、约 196 KB（留在 L2）、
// 常见弹幕档位（约 2000 发）能一批送走。
// 【溢出与 flush 的分工】超 ① ⇒ 报错（整批不发，不自动拆分 —— 批次边界
// 由调用方决定）；② 满时不是溢出而是 **flush**（把攒的提交出去再继续攒）。
// flush 只有三处触发：①装不下下一笔 ②状态变化（混合/纹理页/渲染目标）
// ③帧末（end_frame）。**不得有第四种隐式触发。**
//
// 【事务边界 = 一次调用（不是一次 flush）】
// 任何一个顶点数据非法 ⇒ **整次调用一个图元都不发出** —— 即使第一次
// flush 本来已经能发。**容量决定 flush 次数（性能）；调用决定事务边界
// （正确性）。两者不许混。**
// =============================================================================
inline constexpr std::size_t shape_vertex_limit = 32;
inline constexpr std::size_t batch_buffer_capacity = 8192;

static_assert(batch_buffer_capacity >= shape_vertex_limit,
              "批缓冲容量必须 ≥ 单笔图元顶点上限（否则一个合法单笔放不下 ⇒ 越界写）");

// -----------------------------------------------------------------------------
// mesh_vertex_limit —— draw_mesh 的单次提交顶点上限

//
// 【为什么是 65535 而不是 65536】65535 = 2¹⁶ − 1 = u16 能表示的最大值 ⇒
// "顶点数"这个计数天然落在 u16 里，不会在边界上溢出；
// 取 65536 会让"刚好等于上限"变成一次溢出点。
// 【与 shape_vertex_limit 的关系】两者契约分开，互不相干：
// draw_vertices = 一笔图元（≤32）；draw_mesh = 三角形列表（≤65535）。
// 把上限塞进前者会把两个契约搅在一起。
// 【超限】报错，不静默截断（与"不静默降级"一致）。
// 【拓扑固定为三角形列表】不单开 strip / fan —— 扇形与条带由作者展开
// 成三角形（索引展开，代价可忽略）⇒ 只需要一个名字。
// =============================================================================
inline constexpr std::size_t mesh_vertex_limit = 65535;

// -----------------------------------------------------------------------------
// uv_overshoot_tolerance —— UV 越界的**容差**（单位：像素）
//
// 【为什么有容差】合法 UV 范围是 `[0, w] × [0, h]`（含边界）。而作者算 UV 时
// 可能出现 `100.0000001` 这种**浮点残差** ⇒ 严格 `<= w` 会误报。
// 【为什么容差这么小（1/256 像素）】"取整余量"与"真的写错"要分开：
// 真的写错（图集矩形串了、把归一化值当像素传）通常差几十像素或一个量级。
// 【容差内怎么处理】**夹到边界**（clamp），不报错 —— 它不是数据矛盾。
// 【超出容差】报错（L2）。因为"超过纹理尺寸"意味着这一笔的 UV **没有意义**。
// ⚠️ 注意它抓不到"把 0..1 当像素传"（0.5 落在合法范围内）—— 那只能靠契约。
// 【与采样器 clamp 的分工】采样器 `GL_CLAMP_TO_EDGE` 是**防线**（即使有路径绕过
// 校验，也绝不绕回图集另一处）；这里的检查是**诊断**（当场告诉作者写错了）。
// -----------------------------------------------------------------------------
inline constexpr double uv_overshoot_tolerance = 1.0 / 256.0;

// =============================================================================
// 混合状态（渲染与着色器-2026 权威表）
//
// 【它是什么】**一对 GL 混合因子 + 一个方程**：`out = src·sf + dst·df`
// （`sub` / `rev_sub` 换方程）。**不进着色器** —— 它是固定功能。
// ⇒ 所以 8 个混合状态是**零着色器变体**的：改的只是 `glBlendFuncSeparate` +
// `glBlendEquationSeparate` 的参数。这是它能先于顶点色模式落地的原因。
//
// 【名字纪律】名字**描述效果**，不描述因子；"名字 → 因子"这张表就是权威
// （`blend_state_info_()`，renderer.cpp）。默认项叫 `Normal`（**不叫 `Alpha`**，
// 以免与另一维"纹理 alpha 类型"混淆）。
//
// 【⚠️ alpha 通道怎么办】**RGB 按表，alpha 固定 `(ONE, ONE_MINUS_SRC_ALPHA)` +
// `FUNC_ADD`**（与落地前的默认行为一致）。理由：
// · 表里只定义了 RGB 因子对（`srcFactor`/`dstFactor`）
// · 让 alpha 也走 `Sub`/`RevSub` 方程会让帧缓冲 alpha 出现负值/清零，
// 而它对**画布/快照/将来的后处理**是有意义的量 ⇒ 不能顺手弄坏
// ⇒ 于是用 `blend_func_separate` + `blend_equation_separate` 分开设。
// =============================================================================
enum class blend_state : std::uint8_t {
    normal = 0,   ///< SRC_ALPHA / ONE_MINUS_SRC_ALPHA, ADD      （默认）
    add,          ///< SRC_ALPHA / ONE,                 ADD
    add_no_alpha, ///< ONE / ONE,                       ADD
    sub,          ///< SRC_ALPHA / ONE_MINUS_SRC_ALPHA, REVERSE_SUBTRACT
    sub_no_alpha, ///< ONE / ONE,                       REVERSE_SUBTRACT
    rev_sub,      ///< SRC_ALPHA / ONE_MINUS_SRC_ALPHA, SUBTRACT
    mul_alpha,    ///< ZERO / SRC_COLOR,                ADD
    mul_color,    ///< DST_COLOR / ZERO,                ADD
};

inline constexpr std::size_t blend_state_count = 8;

static_assert(static_cast<std::size_t>(blend_state::mul_color) + 1 ==
                  blend_state_count,
              "blend_state 的枚举值与 blend_state_count 不一致");

/// 混合状态的**协议名**（`Normal` / `Add` / …）。
const char* blend_state_name(blend_state s) noexcept;

/// 名字 → 混合状态。**大小写敏感**（与其它协议名同纪律）。
/// @return false = 不认识这个名字（调用方应当报错并列出 8 个合法名字）
[[nodiscard]] bool parse_blend_state(const char* name, blend_state& out) noexcept;

// =============================================================================
// 纹理 alpha 类型（渲染与着色器-2026）
//
// 【两个状态】`Normal`（默认，`a_out = a_tex · a_vtx`）/ `Replace`（`a_out = a_vtx`）
// 【它为什么进着色器】它改的是**片元输出的 alpha 通道**（不是固定功能）⇒
// 按 它是**着色器变体**的一维（16 = 4 × 2 × 2 里的那个 2）。
// 【它为什么不进排序键】 的判据：它**不是"前序内容的函数"**
// （`a_out` 只取决于本片元）⇒ **顺序无关** ⇒ 只影响 flush 点，不重排。
// ⚠️ 与混合状态的区别就是这一条：混合状态是"分区边界"，它在组内可合并。
// 【用在哪】`Replace` 让"半透明的源"在 `Normal`/`Add`/`Sub`/`RevSub` 下
// 变成**不透明**（那些状态的因子用的是 `SRC_ALPHA`）⇒ 最典型的用途是
// "把一张带 alpha 的图当成不透明贴纸贴上去"。
// =============================================================================
enum class tex_alpha : std::uint8_t {
    normal = 0, ///< a_out = a_tex · a_vtx（默认）
    replace,    ///< a_out = a_vtx
};

inline constexpr std::size_t tex_alpha_count = 2;

static_assert(static_cast<std::size_t>(tex_alpha::replace) + 1 == tex_alpha_count,
              "tex_alpha 的枚举值与 tex_alpha_count 不一致");

const char* tex_alpha_name(tex_alpha t) noexcept;
/// @return false = 不认识这个名字
[[nodiscard]] bool parse_tex_alpha(const char* name, tex_alpha& out) noexcept;

// =============================================================================
// 顶点色模式（渲染与着色器-2026）
//
// 【四个状态】名字沿用既有（不新造）：
// · `Mul`（默认）：`rgb = rgb_tex · rgb_vtx`，`a = a_tex · a_vtx`（"文字 = Mul"那条）
// · `Add`：`rgb = rgb_tex + rgb_vtx`，`a = a_tex · a_vtx`
// · `Sub`：`rgb = rgb_tex − rgb_vtx`，`a = a_tex · a_vtx`（负值 `max(0,·)` 夹紧）
// · `Alphablend`：`rgb = rgb_vtx`（**纹理的 RGB 被丢弃**），`a = a_tex · a_vtx`
//
// 【它进着色器、且是变体】改的是**片元的 RGB 计算** ⇒ 的变体一维。
// 【它不进排序键】：不是"前序内容的函数" ⇒ 顺序无关 ⇒ 只改 flush 点。
// 【为什么 `Sub` 要夹紧】颜色通道是 `[0,1]` 的隐式范围；GL 对**颜色输出**是否
// 夹紧取决于帧缓冲格式（RGBA8 会夹），但"依赖驱动去夹"不是契约 ⇒ 自己 `max`。
// ⚠️ 而 `Alphablend` 的 `rgb = vtx` 意味着"**纹理只贡献 alpha**" ——
// 这是它和 `Mul` 拉开距离的地方（`Mul` 在 vtx=白时就是原样贴图）。
// =============================================================================
enum class vertex_color_mode : std::uint8_t {
    mul = 0,   ///< rgb_tex · rgb_vtx（默认）
    add,       ///< rgb_tex + rgb_vtx
    sub,       ///< rgb_tex − rgb_vtx（夹紧 0）
    alphablend ///< rgb_vtx（纹理只出 alpha）
};

inline constexpr std::size_t vertex_color_count = 4;

static_assert(
    static_cast<std::size_t>(vertex_color_mode::alphablend) + 1 == vertex_color_count,
    "vertex_color_mode 的枚举值与 vertex_color_count 不一致");

const char* vertex_color_name(vertex_color_mode m) noexcept;
/// @return false = 不认识这个名字
[[nodiscard]] bool parse_vertex_color(const char* name, vertex_color_mode& out) noexcept;

// =============================================================================
// 雾
//
// 【定义】`final.rgb = lerp(color.rgb, fog.rgb, f)`，
// `f = clamp((z − near) / (far − near), 0, 1) × strength`（线性雾）
// 【距离量 = 顶点的 `z`】：视空间距离在我们的坐标里就是 `z`——
// 用它的**数值**，不碰它作为排序键的作用（排序用的是**调用**的 z，不是顶点 z）。
// ⚠️ 硬件深度 `gl_FragCoord.z` 不可用：已定不做深度测试、没有深度缓冲。
// 【只改 RGB】①：用雾色的 alpha 会让"雾"变成"改可见性"，那是另一个状态的事。
// 【允许 near > far】②：反向雾是公式的自然结果，**不特判**。
// 【只贡献 1 位变体】：第一版只有线性雾一种公式 ⇒ 启用/关闭是变体位，
// 颜色/near/far/强度全部走 **uniform**。⇒ 变体账 = 4 × 2 × 2 = **16**。
// 【不进排序键】：不是"前序内容的函数" ⇒ 顺序无关，只影响 flush 点。
// =============================================================================
struct fog_state {
 /// 启用位（**唯一的变体维度**）。false = 该维度不参与着色器。
    bool enabled = false;
 /// 雾色（**只用 RGB**，①）；分量 0..1，与纹理值同口径（④ 不做 sRGB 转换）。
    float color[3] = {0.0f, 0.0f, 0.0f};
 /// 起止距离（顶点 z 的数值）；允许 near > far（反向雾）。
    double near_z = 0.0;
    double far_z = 1000.0;
 /// 强度（乘在 f 上；1 = 不额外削弱）。夹到 [0,1] 由调用方保证语义，
 /// 这里不夹 —— 与"允许 near>far、不特判"同一条纪律。
    double strength = 1.0;

 /// 逐字段比较：**批状态**要"完全相等"才能合批（的"组内可合并"）。
    [[nodiscard]] bool operator==(const fog_state& o) const noexcept {
        return enabled == o.enabled && color[0] == o.color[0] &&
               color[1] == o.color[1] && color[2] == o.color[2] &&
               near_z == o.near_z && far_z == o.far_z && strength == o.strength;
    }
    [[nodiscard]] bool operator!=(const fog_state& o) const noexcept {
        return !(*this == o);
    }
};

/// 雾只贡献 **1 位**变体⇒ 2 档。
inline constexpr std::size_t fog_count = 2;

// 【变体总数】三维全开：4 顶点色 × 2 纹理 alpha × 2 雾启用 = **16**（的账已满）。
inline constexpr std::size_t pipeline_variant_count =
    vertex_color_count * tex_alpha_count * fog_count;

/// 变体下标：`(顶点色 × tex_alpha_count + 纹理 alpha) × fog_count + 雾启用`。
/// 【为什么把顶点色放在最高位】任何顺序都行；这里选"变化慢的那一维在高位"，
/// 纯粹为了让日志/调试输出里的下标可读（0..7 是无雾，8..15 是有雾）。
constexpr std::size_t variant_index(vertex_color_mode m, tex_alpha t,
                                    bool fog_on) noexcept {
    return (static_cast<std::size_t>(m) * tex_alpha_count +
            static_cast<std::size_t>(t)) *
               fog_count +
           (fog_on ? 1u : 0u);
}

/// 三态当前状态：**未设置 / 已显式设置**。
/// · "生效值 = 显式值 若已设；否则用该维默认（混合维的默认 = `Normal`，
/// 等容器落地后默认变成"容器字段"）"
/// · **粘性**：改了就一直有效，直到再改或显式清除（**不**帧末重置 ——
/// 否则"在 `on_game_init` 里设一次"会失效，反直觉）
/// · 状态栈在 Lua 侧；引擎只保存这一个"当前值"
struct draw_state_override {
    bool blend_set = false;
    blend_state blend = blend_state::normal;
    bool tex_alpha_set = false;
    tex_alpha alpha = tex_alpha::normal;
    bool vcolor_set = false;
    vertex_color_mode vcolor = vertex_color_mode::mul;
 /// 雾这一维：`fog_set == false` ⇒ 用默认（= 关闭）；已设置 ⇒ 用 `fog` 整份
 /// （**四个字段一起生效** —— 雾不像 blend 那样只有一个名字，
 /// 所以"已设置"覆盖的是整组参数，不逐个字段三态化）。
    bool fog_set = false;
    fog_state fog;
 /// 自定义着色器（2026-10-19）。句柄 0 = 用内建变体。
 ///
 /// 【它是一维"批状态"】程序变了就必须 flush（与混合同一类）。
 /// 【它同时改变坐标语义】**开了它 ⇒ 顶点按整帧像素解释**（不做世界变换），
 /// `v_uv` 归一化到该四边形（而不是纹理）。这条是作者可见的规则。
    bool shader_set = false;
    std::uint32_t shader = 0;
 /// 每次绘制的 4 个浮点参数（`u_params`）。它同样是批状态（参数变就 flush）。
    bool params_set = false;
    float params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};


// 【收集阶段的值域（set_collect_stage 的参数）】与 wag::render_stage 的
// 阶段语义一一对应（none 不收集，由 engine 相位门把守）。
inline constexpr std::uint8_t collect_stage_scene = 0;
inline constexpr std::uint8_t collect_stage_ext   = 1;

// -----------------------------------------------------------------------------
// vertex_defect —— 顶点校验的失败定位
//
// 【它是什么】find_vertex_defect 的输出：哪个顶点的哪个分量不是有限值。
// 它**不携带"怎么办"** —— 报告策略（L3 终止 / 抛 Lua 错误）由调用方决定，
// 这正是错误架构"一个决策点 + 两个入口"在渲染层的落点：
// · C++ 侧内部路径（将来的容器管线）⇒ fail_boundary（L3，引擎 bug 才会到这）
// · 脚本侧路径（wag.draw.draw_vertices 的绑定）⇒ 抛 Lua 错误 ⇒
// lua_pcall 接住 ⇒ 按脚本错误处理（L2，整批不发，游戏继续）——
// 这正是“出错走 Lua 错误”与
// “一个错误的顶点最多让画面少一块”。
// ⇒ 于是**校验逻辑只有这一份**，而两个入口各取所需 —— 校验与报告分离。
// -----------------------------------------------------------------------------
struct vertex_defect {
 /// 第几个顶点（0 基；报错文案里 +1 成"第 n 个"）。
    std::size_t index = 0;

 /// 哪个分量：'x' / 'y' / 'z' / 'u' / 'v'（color 是无符号整数，天然有限）。
    char field = 0;

 /// 实际值（float 提升为 double；NaN / ±inf 原样带出）。
    double value = 0.0;

 /// 无缺陷时 field == 0。
    [[nodiscard]] bool ok() const noexcept { return field == 0; }
};

// -----------------------------------------------------------------------------
// renderer —— 渲染器
// -----------------------------------------------------------------------------

class renderer {
public:
    renderer() = default;
    ~renderer();

    renderer(const renderer&) = delete;
    renderer& operator=(const renderer&) = delete;
    renderer(renderer&&) = delete;
    renderer& operator=(renderer&&) = delete;

 // -------------------------------------------------------------------------
 // 上下文的接收（形态乙的另一半）
 // -------------------------------------------------------------------------

 /// 接收图形上下文并**独占**它。
 ///
 /// @param ctx 由窗口交出（platform::window::take_gl_context）
 /// @return 成功时 true
 ///
 /// 【为什么它必须与窗口的创建分两步】
 /// 因为平台层不得依赖图形 API：唯一能创建上下文的位置是窗口创建那一刻,
 /// 而"持有并使用"它的是渲染层。这两件事由交接分开。
 ///
 /// 【副作用（2026-10-15 起）：取回 GL 3.3 入口】
 /// 见 draw/gl_api.hpp。取失败 ⇒ L3 终止（不静默、不回退）。
    bool adopt_context(platform::gl_context ctx);

 /// 是否已经拿到上下文。
    [[nodiscard]] bool has_context() const noexcept { return ctx_.is_valid(); }

 /// 当前是否有一张**可用**的 staged 快照（`redraw_last_frame` 的唯一素材）。
 ///
 /// 【为什么调用方必须先问这一句（2026-11-14）】
 /// `redraw_last_frame` 的实现是"清成信箱黑 + 画快照"，快照为空时**只剩黑**。
 /// 而 B 方案（2026-10-22）把快照改成"按请求才拍"⇒ 正常游戏里它**永远是空的**。
 /// ⇒ 不先问就调用，等于把一块黑推上屏幕（拖拽闪黑的根因，实测已定位）。
 ///
 /// 【为什么不在 `redraw_last_frame` 内部挡】它只负责"画"，**呈现（Swap）在调用方**；
 /// 如果它在没快照时直接返回，调用方仍会 Swap 一块**没画过的后缓冲** —— 更糟。
 /// ⇒ 决定"画不画"必须在**同时掌握清屏与呈现**的那一层（`expose_watch`）。
    [[nodiscard]] bool has_staged_snapshot() const noexcept { return staged_valid_; }

 // -------------------------------------------------------------------------
 // 每帧的动作
 // -------------------------------------------------------------------------

    /// 开始一帧的绘制：清屏、绑定默认渲染目标、设置视口与裁剪。
    ///
    /// @param transform 本帧的变换（由 space 在阶段 1 算出）
    /// @param viewport 本帧屏幕上那个矩形（窗口坐标；左上原点、+y 下）
    ///                必须来自 viewport::get_viewport()
    /// @param geom 本帧几何（窗口尺寸 / 绘制表面像素 / 显示比例，一次读）
    ///
    /// 【窗口尺寸不在参数里单独出现】
    /// 它就在 `geom.window`；再传一个矩形会让同一个量有两个来源。
    ///
    /// 【为什么把三个量打包】
    /// 只传一个尺寸会让人想自己乘/除出另一个。打包后要混用必须先挑出分量，
    /// 那一步在审查里看得见；C++ 仍拦不住 `geom.window.w * geom.display_scale`。
    ///
    /// 【当前平台事实】
    /// 窗口尺寸与绘制表面像素相等（未启用高像素密度）；
    /// 这不是“DIP 等于像素”，也不要拿 display_scale 去换算。
    ///
    /// 【viewport 不是 render 矩形】
    /// render_ 是世界的可见范围；viewport 是按 letterbox/stretch 算好的
    /// 屏幕矩形。由变换反推进会混进两方向缩放，结果不可靠。
    void begin_frame(const space::transform& transform,
                     const space::rect& viewport,
                     const platform::window::frame_geometry& geom);

 // -------------------------------------------------------------------------
 // `set_clip` —— **已删除（2026-10-18）**
 //
 // 【为什么删】裁剪已被判给**脚本侧**：
 // 裁剪在脚本侧的唯一结论：
 // "**裁剪在脚本侧自己写** —— 引擎不提供裁剪的语义，脚本自行处理
 // （自己算被裁掉的部分、自己缩 UV/位置、或干脆不画）"
 // ⇒ 于是"斜的裁剪区""按形状裁剪"都不存在（Lua 想怎么裁就怎么裁）。
 //
 // 【本类不再持有裁剪原语】引擎侧的 `glScissor` 只服务**视口**
 // （`begin_frame` 里那一次，由 render 矩形推出）。内容裁剪不是引擎的事。
 // -------------------------------------------------------------------------

 /// 结束一帧的绘制：**帧末 flush**（flush 触发③）——此后呈现。
 ///
 /// 【结束一帧的职责】
 /// end_frame = ①帧末 flush ②全屏回画 + 运行效果（未实现，画布批落地）
 /// ③绑回屏幕。帧快照在 end_frame **之后**（它拍"玩家看到的最终画面"，
 /// 而效果会改画面 ⇒ 快照必须排在效果后）。
 /// ⇒ 当前批次管线只实现了 ①；②③ 随屏幕与画布那批落地。
 /// 【调用点】帧结构的阶段 9 之后、阶段 10（快照）之前；休眠帧不调用
 /// （休眠跳过整条渲染管线 ⇒ 缓冲本来就是空的）。
    void end_frame();

    /// 把内部场景目标呈现到默认输出。
    ///
    /// 【调用点】阶段 12：end_frame 与快照之后、window::present 之前。
    /// 【当前实现】letterbox + linear；渲染分辨率/比例 API 随后接入。
    void present_frame();

    /// 设置内部场景目标尺寸（在下一帧 begin_frame 生效/重建）。
    void set_scene_size(int w, int h) noexcept;

    /// 设置呈现策略与滤波（在下一帧 present 生效）。
    void set_present_mode(present_mode mode) noexcept;
    void set_present_filter(present_filter filter) noexcept;

    [[nodiscard]] int scene_width() const noexcept { return scene_w_; }
    [[nodiscard]] int scene_height() const noexcept { return scene_h_; }

 /// 设定当前收集阶段（绘制项的"阶段"排序键；帧结构在阶段 7/9 边界调用）。
 ///
 /// 【值域】0=scene 1=ext（与 wag::render_stage 的阶段语义一一对应；
 /// none 不收集 —— 由 engine 的相位门把守，不到达这里）。
 /// 【为什么是渲染器的状态】"阶段"是发射排序键的一员，而发射归渲染器 ⇒
 /// 键住在它惟一的消费者那里（帧结构在开闸/切阶段时同步过来）。
    void set_collect_stage(std::uint8_t stage) noexcept { collect_stage_ = stage; }

 /// 渲染侧统计（自上次取走以来的累计；取走即清零）。
 ///
 /// 【它为什么存在】“不设上限 ≠ 不提供证据”：
 /// 掉帧归因需要提交次数 / flush 次数 / 顶点数这三个数。
 /// 【实现纪律】只做整数累加；取走是拷贝 + 清零，无分配。
    struct draw_stats {
        std::uint64_t submissions = 0;  ///< glDrawElements 次数
        std::uint64_t flushes = 0;      ///< flush 次数（不含空 flush）
        std::uint64_t vertices = 0;     ///< 实际提交的缓冲顶点数
        std::uint64_t primitives = 0;   ///< 图元数（图元 = 一笔形状）
        std::uint64_t captures = 0;     ///< 捕获次数（LoadImageFromSnapshot）
        std::uint64_t capture_pixels = 0;  ///< 捕获像素量（宽×高的累计）
    };
    [[nodiscard]] draw_stats take_draw_stats() noexcept;

 // -------------------------------------------------------------------------
 // 当前状态（入口乙）—— 2026-10-18 起只开"混合状态"这一维
 //
 // 【为什么先只开这一维】8 个混合状态是**固定功能**（不进着色器）⇒ 零变体成本；
 // 而顶点色模式 / 纹理 alpha / 雾都要着色器变体（的 16 个），
 // 随那批一起做。⇒ 未实现的那三维在 Lua 侧**明确报错**，
 // 绝不静默忽略（"看起来生效了但没生效"是最坏的一类）。
 //
 // 【三态与粘性】见 `draw_state_override` 的注释。
 // -------------------------------------------------------------------------
    void set_blend_state(blend_state s) noexcept { state_.blend = s; state_.blend_set = true; }
 /// @return false = 该维**未显式设置**（调用方应当返回"未设置"，而不是 Normal）
    [[nodiscard]] bool get_blend_state(blend_state& out) const noexcept {
        out = state_.blend;
        return state_.blend_set;
    }
    void reset_blend_state() noexcept { state_.blend = blend_state::normal; state_.blend_set = false; }

    void set_tex_alpha(tex_alpha t) noexcept { state_.alpha = t; state_.tex_alpha_set = true; }
    [[nodiscard]] bool get_tex_alpha(tex_alpha& out) const noexcept {
        out = state_.alpha;
        return state_.tex_alpha_set;
    }
    void reset_tex_alpha() noexcept { state_.alpha = tex_alpha::normal; state_.tex_alpha_set = false; }

    void set_vertex_color(vertex_color_mode m) noexcept { state_.vcolor = m; state_.vcolor_set = true; }
    [[nodiscard]] bool get_vertex_color(vertex_color_mode& out) const noexcept {
        out = state_.vcolor;
        return state_.vcolor_set;
    }
    void reset_vertex_color() noexcept {
        state_.vcolor = vertex_color_mode::mul;
        state_.vcolor_set = false;
    }

 /// 设置雾的**整组**参数（`fog.enabled == false` 等价于"关掉雾"）。
    void set_fog(const fog_state& f) noexcept {
        state_.fog = f;
        state_.fog_set = true;
    }
    [[nodiscard]] bool get_fog(fog_state& out) const noexcept {
        out = state_.fog;
        return state_.fog_set;
    }
    void reset_fog() noexcept {
        state_.fog = fog_state{};
        state_.fog_set = false;
    }

 // -------------------------------------------------------------------------
 // 自定义着色器（2026-10-19，A 组裁决）
 //
 // 【它是一维批状态】与混合/顶点色/雾同类：变了就 flush。
 // 【它同时改变坐标语义】开了它 ⇒ 顶点按**整帧像素**解释（不做世界变换），
 // `v_uv` 归一化到该四边形。这是作者可见的规则。
 // -------------------------------------------------------------------------
    void set_shader(std::uint32_t handle) noexcept {
        state_.shader = handle;
        state_.shader_set = true;
    }
    [[nodiscard]] bool get_shader(std::uint32_t& out) const noexcept {
        out = state_.shader;
        return state_.shader_set;
    }
    void reset_shader() noexcept {
        state_.shader = 0;
        state_.shader_set = false;
        state_.params_set = false;
    }

 /// 热重载代际边界：丢弃已收集绘制、清批状态与粘性绘制状态。
 /// 只在重载闸门内调用；下一帧 begin_frame 会重新建立帧状态。
    void reset_for_reload() noexcept;
    void set_shader_params(const float (&p)[4]) noexcept {
        for (int k = 0; k < 4; ++k) {
            state_.params[k] = p[k];
        }
        state_.params_set = true;
    }
    [[nodiscard]] bool get_shader_params(float (&out)[4]) const noexcept {
        for (int k = 0; k < 4; ++k) {
            out[k] = state_.params[k];
        }
        return state_.params_set;
    }

 // -------------------------------------------------------------------------
 // 自定义着色器 —— 资源与时间（2026-10-19）
 // -------------------------------------------------------------------------

 /// 从文件加载一份片元着色器，与**内嵌顶点着色器**链接，返回有效句柄。
 /// 正常构建失败（读不到/编译/链接/槽满）⇒ L3；0 只来自未接 SDL 的构建。
 ///
 /// 【路径由 Lua 给】：引擎不解析路径。
 /// 【编译/链接失败 = L3】与"资源加载失败即 L3"一致（已定）。
    [[nodiscard]] std::uint32_t load_shader(const std::string& name,
                                           const char* path);

 /// 按名字卸载（句柄立刻失效；本帧已收集的绘制项仍能画完 —— 它们持有 program）。
    void unload_shader(const std::string& name);

 // ---------------------------------------------------------------------
 // 光照 API（2026-11-18，Part 6）—— 引擎原语的落地侧
 //
 // 【校验在哪】字段合法性/非有限值在**原语层**做（primitives.cpp），
 // 这里只收已校验的值；池满/池名重复一类的"跨帧资源"错误在这里判。
 // ---------------------------------------------------------------------

 /// 提交一盏光（本帧有效；光是一次性的，见 D5）。
    void light_add(const light::light& l);

 /// 清空本帧光列表（帧末渲染后调用）。
    void light_clear() noexcept;

 /// 声明带（D2/D6.3：显式 id、layer 升序、不重叠、每帧声明）。
    bool light_band_declare(const std::string& id, std::int64_t from,
                            std::int64_t to, std::string& err);

 /// 合成一条带（D3：每带最多一次；光恰合成一次）。
    bool light_commit(const std::string& id, std::string& err);

 /// 这个 layer 是否落在某个已声明的带内。
    [[nodiscard]] bool light_layer_in_band(std::int64_t layer) const;
    [[nodiscard]] bool light_bands_declared() const noexcept { return !bands_.empty(); }

 /// 曝光（D6.5：全局每帧一个值）。
    void light_set_exposure(float v) noexcept { exposure_ = v; }
    [[nodiscard]] float light_exposure() const noexcept { return exposure_; }

 /// 光照覆盖的世界矩形（与 set_viewport 分离；见 D-LIGHT-001）。
    void light_set_space(const space::rect& r) noexcept;
    [[nodiscard]] space::rect light_space() const noexcept { return light_space_; }

 /// 每帧由帧结构传入当前 render；只在作者未显式设置时提供默认值。
    void set_render_space(const space::rect& r) noexcept;

 /// 载入光 shader（目录；读 light.vs / light.fs）。失败 ⇒ 返回 false + err。
    [[nodiscard]] bool light_load_shader(const std::string& dir, std::string& err);

 /// 遮挡者池：建池（同名 ⇒ false）。
    [[nodiscard]] bool light_occluders_new(const std::string& name);
 /// 生成（池满 / 池不存在 ⇒ 0）。
    [[nodiscard]] std::uint64_t light_occluders_spawn(const std::string& name,
                                                      const light::occluder& o);
    bool light_occluders_write(std::uint64_t h, const light::occluder& o);
    bool light_occluders_kill(std::uint64_t h);
    [[nodiscard]] bool light_occluders_alive(std::uint64_t h) const;
    [[nodiscard]] std::size_t light_count() const noexcept { return lights_.size(); }
    [[nodiscard]] std::size_t occluder_count() const noexcept;

 /// 光照统计快照（；帧末取一次，取后清零帧内计数）。
    struct light_stats_snapshot {
        std::uint32_t lights = 0;      ///< 本帧最大提交数
        std::uint32_t occluders = 0;   ///< 池内存活
        std::uint32_t bands = 0;       ///< 本帧声明带数
        std::uint32_t commits = 0;     ///< 本帧合成次数
        std::uint64_t fill_px = 0;     ///< 本帧光图填充像素
        double fill_ratio = 0.0;       ///< fill_px / 光图像素
        bool over_lights = false;      ///< 本帧跨过 >256
        bool over_fill = false;        ///< 本帧跨过 >32×
        bool over_commits = false;     ///< 本帧跨过 >8
    };
    [[nodiscard]] light_stats_snapshot take_light_stats() noexcept;
    [[nodiscard]] const light_stats_snapshot& last_light_stats() const noexcept { return last_light_stats_; }
    [[nodiscard]] std::uint32_t light_band_count() const noexcept {
        return static_cast<std::uint32_t>(bands_.size());
    }
    [[nodiscard]] std::uint32_t light_frame_commits() const noexcept {
        return frame_commits_;
    }
    [[nodiscard]] std::uint32_t light_frame_lights() const noexcept {
        return frame_lights_max_;
    }
    [[nodiscard]] double light_fill_ratio_now() const noexcept {
        return light_fill_ratio_();
    }


 /// 句柄是否仍有效（活动、代数匹配、program 非 0）。
 ///
 /// 【给谁用】`set_state{shader=...}` 的跨边界校验：无效句柄必须在
 /// 进入绘制状态之前报 L3，而不是留到发射时静默回退（D3）。
    [[nodiscard]] bool valid_shader_handle(std::uint32_t handle) const noexcept;

 /// 本帧的逻辑帧号与步长（帧管线每帧调用一次）。
    void set_logic_frame(std::uint64_t frame_count, double dtg) noexcept {
        logic_frame_count_ = frame_count;
        logic_dtg_ = dtg;
    }

 /// 把"当前帧"重新定义为游戏第 n 帧（`wag.time.set`）。
    void set_time_frame(std::int64_t n) noexcept {
        time_frame_offset_ = static_cast<std::int64_t>(logic_frame_count_) - n;
    }

 /// 当前游戏帧号（`wag.time.get`）。
    [[nodiscard]] std::int64_t time_frame() const noexcept {
        return static_cast<std::int64_t>(logic_frame_count_) - time_frame_offset_;
    }

 /// 当前逻辑时间（秒）。着色器的 `u_time` 就是它。
    [[nodiscard]] double logic_time() const noexcept {
        return static_cast<double>(time_frame()) * logic_dtg_;
    }

 /// 已占用的着色器槽数（诊断）。
    [[nodiscard]] std::size_t shader_count() const noexcept;


 // -------------------------------------------------------------------------
 // 任意网格绘制（快照捕获与网格绘制-2026）
 // -------------------------------------------------------------------------

 /// 一次提交顶点列表，按**三角形列表**解释（每 3 顶点一个三角形）。
 ///
 /// @param verts 顶点数组；格式与 draw_vertices **完全相同**
 /// （逻辑坐标 + 像素 UV + 打包 u32 颜色；page 随纹理数组批加入）
 /// @param count 顶点个数；**必须 ≤ mesh_vertex_limit**，超限报错不截断。
 /// 不是 3 的倍数 ⇒ 末尾不足 3 个的顶点围不成三角形，
 /// **静默忽略**（与 draw_vertices 的退化输入同一条判据：
 /// "看起来没用"是自由，数据矛盾才是错误）
 ///
 /// 【它仍是纯几何】引擎只吃顶点，不认"网格结构"：不要求规则网格、
 /// 不要求连通、不要求顶点共享 ⇒ 引擎侧没有任何"网格语义"
 /// （与 draw_glyphs 不认字形语义同族；"网格"只是名字带给作者的说法）。
 /// 【批次与事务】照旧受批次三个数约束：顶点放不下 ⇒ flush（合法触发①），
 /// 一次调用可能跨多次 flush；**事务边界仍是一次调用** ——
 /// 校验段先完整走完（find_vertex_defect 扫全量），再开始分块入批，
 /// ⇒ "即使第一次 flush 本来已经能发，非法数据一笔都不发出"仍然成立。
 ///
 /// @param layer 本批三角形的**绘制顺序层**（64 位整数；更大者后画=在上）
 /// @param z 层内**顺序键**（浮点；更大者更靠前=后画=在上；无深度测试）
 /// @param image **图片句柄**（0 = 默认白纹理）。**必须有它**：
 /// UV 归一化要除以**纹理尺寸**，而尺寸只能从图片句柄查到
 /// （详见 `u_tex_inv_size` 的说明）。
    void draw_mesh(std::uint64_t image, std::int64_t layer, double z,
                   const vertex* verts, std::size_t count);

 // -------------------------------------------------------------------------
 // 从快照取图（快照捕获与网格绘制-2026）
 //
 // 【它是"单向桥"】画面 → 图片，永不反向：捕获产出的是**一张图片**，
 // 图片永远只读、不能被渲染进去 ⇒ "渲染目标"那扇门没有重新打开。
 // 【捕获不是渲染】实现是从快照画布**拷一块像素**（GPU 内部 blit），
 // 不绑渲染目标"画"一次；混合/顶点色/雾/纹理 alpha 四个状态一律不适用。
 // -------------------------------------------------------------------------

 /// 从快照（上一帧，阶段 10 生成）取一块，做成**图片族的标准图片资源**。
 ///
 /// @param name 图片名（**必需**，落进图片族 ⇒ 覆盖=卸载+加载、
 /// 可单张 Unload、可 FreeRes 整族清；匿名句柄与既有决定矛盾）
 /// @param x, y 矩形**中心**（逻辑坐标；与图片中心锚点同一惯例）
 /// @param w, h 矩形尺寸（逻辑单位；换算到快照像素，超界 ⇒ 报错不裁剪）
 /// @return 图片句柄（低 21 位槽位 / 高 43 位代数；取回/判活走资源入口）
 ///
 /// 【时机】读的是**上一帧的快照** ⇒ 任意阶段都可调用（与阶段无关）；
 /// 读纹理不引管线同步（若读当前 backbuffer 会多一次管线停顿 —— 不那样实现）。
 /// 【拷贝方向的关键（坑 1）】blit 的目标 Y 必须交换（dstY0 = h、dstY1 = 0）：
 /// 快照是 framebuffer 空间（原点左下），图片 UV 是左上原点 ⇒
 /// 不交换 = **照片上下颠倒**（这是本节最重要的一条）。
 /// 【产出】RGBA8 独立纹理 + 整块 UV = 标准图片；默认过滤 **linear**
 /// （照片常被画小，且它不在图集里 —— 与全局默认 nearest 不同的**明确例外**）。
 /// 【失败】快照未生成 / 矩形超界 / FBO 不完整 ⇒ fail_boundary（L3）。
    std::uint64_t load_image_from_snapshot(const char* name,
                                           double x, double y, double w, double h);

 /// 从**文件**加载一张图片（内部自建纹理；只返回图片句柄）。
 ///
 /// 【它是已定 API 里的 `LoadImageFromFile`】定位：**开发 / 小规模入口**
 /// （正式的项目应当用"纹理 + 图集子矩形"那条路 —— 它随纹理族那批落地）。
 /// 【解码器】用 **SDL3 核心自带的 `SDL_LoadPNG` / `SDL_LoadBMP`**
 /// —— 零新依赖（SDL 3.4.0 起提供，本机装的是 3.4.18）。
 /// ⚠️ SDL 的 PNG 加载器自己的说明写着"面向可信来源" ⇒ 第一版接受这个边界。
 /// 【产出】RGBA8 独立纹理 + 整块 UV（0,0,w,h）= 标准图片；过滤 **nearest**
 /// （全局默认；照片那种"被画小"的才用 linear —— 见 `load_image_from_snapshot`）。
 /// 【失败】读不到 / 解码失败 / 上传失败 ⇒ `fail_boundary`（L3，资源失败档）。
 /// @return 图片句柄（低 21 位槽位 / 高 43 位代数）
    std::uint64_t load_image_from_file(const char* name, const char* path);

 /// 按名卸载一张图片（代数 +1 ⇒ 旧句柄自证失效）。
 ///
 /// 【2026-10-18 已定：**纹理销毁延迟到帧末**】
 /// · **立刻发生的**：名字从名字表移除、代数 +1（旧句柄当场失效）、槽位回 free-list
 /// · **延迟到 `end_frame()` 的**：`glDeleteTextures`
 /// ⇒ 理由：绘制是"先收集、后发射"，脚本在渲染钩子里"画完就卸载"时，
 /// 那一笔的纹理名还没被绑过。立刻删 ⇒ 帧末 flush 绑到已删除的名字
 /// ⇒ **静默黑图/旧图**（而句柄校验发生在收集时，抓不到）。
 /// ⇒ 这与"暂停/退出/视口变更都在帧末生效"是**同一条纪律**。
 ///
 /// 【连带失效】当前没有"依赖图片的对象"（容器批未落地）⇒ 连带名单暂为空；
 /// 容器落地后这里报 L1 名单（只报不拦 —— 不引入引用计数）。
    void unload_image(const char* name);

 /// 按名取回句柄。不存在（或已卸载）⇒ 返回 false。
    [[nodiscard]] bool lookup_image_handle(const char* name,
                                           std::uint64_t& out_handle) const;

 // -------------------------------------------------------------------------
 // 字体图集的页（2026-11-18，TTF 第一版）
 // -------------------------------------------------------------------------
 //
 /// 从 **CPU 像素**建一张图片（RGBA8）。**字体图集的页**用它，不是脚本 API。
 ///
 /// 【为什么需要它】`load_image_from_file` 只能从文件来，而字形是**运行时生成**的
 /// 像素。这是"CPU 像素 → GPU 纹理"的唯一通路（**不再另开第二条**：
 /// 脚本侧没有 `image.from_rgba`，见）。
 ///
 /// 【过滤 = linear】字形是抗锯齿位图，1:1 时 linear 与 nearest 无差别，
 /// 被缩放时 linear 明显更好（与图片的全局默认 nearest 不同，是明确例外）。
 ///
 /// 【失败】槽位满 / 纹理创建失败 / 上传失败 ⇒ `fail_boundary`（L3）。
 /// @param name **保留名**（调用方必须用 `__` 前缀，见 `is_reserved_image_name`）
 /// @return 图片句柄（可喂 `draw_glyphs`）
    std::uint64_t create_image_from_rgba(const char* name, int w, int h,
                                         const unsigned char* rgba);

 /// 更新已有图片的**一个子矩形**（字体图集追加字形用它）。
 ///
 /// 【为什么是"更新"而不是"重建"】整页重传是 O(页) 的；字形只占页的几百字节。
 /// @return false = 句柄失效（调用方当作内部错误上报）
    bool update_image_region(std::uint64_t handle, int x, int y, int w, int h,
                             const unsigned char* rgba);

 /// 是不是**引擎保留的图片名**（`__` 前缀）。
 ///
 /// 【用途】`image.unload` 对保留名一律拒绝（L2）：图集页的生命周期只归
 /// `wag.font.unload` —— 否则脚本一卸载页，字体还在引用它（半死资源）。
    [[nodiscard]] static bool is_reserved_image_name(const char* name) noexcept;
 /// 句柄是否指向引擎保留图片（字体图集页）。
    [[nodiscard]] bool is_reserved_image_handle(std::uint64_t handle) const noexcept;

 /// **引擎内部**销毁一张图片（跳过保留名保护）。**不是脚本 API。**
 ///
 /// 【谁用】`wag.font.unload` 释放图集页；脚本侧那条 `image.unload` 会被
 /// `is_reserved_image_name` 挡掉，所以字体模块需要一条**内部**通路。
 /// 【语义】与 `unload_image` 相同（代数 +1、纹理延到帧末删）。
    void destroy_image_internal(const char* name) noexcept;

 /// 图片的**像素尺寸**（脚本侧 `GetImageSize` / 图集装箱工具要用）。
 ///
 /// @return false = 句柄失效（与内部判活同一套规则：槽位/代数/纹理）
 /// 【它是"查询"】失效时返回 false、**不报错** ——
 /// 判据与资源那条一致：**查询**返回 nil，**使用**才报错。
 /// （`draw_vertices` / `draw_mesh` 拿它当"使用"，所以那边报错。）
    [[nodiscard]] bool image_pixel_size(std::uint64_t handle, double& out_w,
                                        double& out_h) const;

 /// 给一张图片挂**帧表**（：图像组 = 带帧表的图片）。
 ///
 /// 【参数】`rects` 是**纹理像素**矩形（左上原点、+y 向下，与取图矩形同一套口径）。
 /// 【语义】同槽位原地替换（**不改代数**：句柄仍然有效，容器不必重建）。
 /// 【校验】空表 / 某个矩形 w 或 h 为 0 / 矩形越出纹理 ⇒ `fail_boundary`（L3）。
 /// 【与 的关系】帧表不是 `img` 本身 ⇒ 改它不违反"img 创建后不可变"。
 /// 【落地形态】只有"显式矩形"这一种；按行列切网格属于**排版概念**，
 /// 引擎不认识（用 `image_pixel_size` 在库层算）。
    void define_frames(std::uint64_t handle,
                       const std::vector<frame_rect>& rects);

 /// 这张图片的**帧数 n**（查询，不报错）。
 ///
 /// 【普通图片 = 1】整张图就是第 0 帧 ⇒ 于是 `frame` 的合法集合恒为 [0, n)，
 /// 不需要"两种图片两套规则"。
 /// 【失效句柄】返回 0（查询语义：失效 ⇒ 0，使用才报错）。
    [[nodiscard]] std::uint32_t image_frame_count(std::uint64_t handle) const;

 /// 取第 `frame` 帧的矩形（**发射路径专用**）。
 ///
 /// @return false = `frame` 越界（调用方必须**报错**，不许静默取第 0 帧）
 /// 【越界为什么是 L3 而不是静默】与 （图片没了但容器还在引用）同一条：
 /// 这是"脚本把帧表改小 / 写了越界帧号"造成的**结构性不一致**，
 /// 静默回退会画出错的画面而没人知道。
    [[nodiscard]] bool image_frame_rect(std::uint64_t handle,
                                        std::uint32_t frame,
                                        frame_rect& out) const;

 /// 设图片的**锚点**（归一化；定案）。
 ///
 /// 【语义】`draw_image(img, L, Z, x, y, w, h)` 的 (x, y) = 锚点落在哪。
 /// 默认 (0,0) = 左上 ⇒ 与加锚点之前**逐字节相同**。
 /// 一般式：left = x - ax·w，top = y + ay·h（+y 向上，矩形向下展开）。
 /// 【相对谁】相对**被绘制的矩形**（w×h）；取图矩形只决定 UV，不参与。
 /// 【镜像】w/h 为负时锚点**位置不变**，只有展开方向反了。
 /// 【范围】任意有限值（1.5 = 轴心在图外，合法、不钳制）；
 /// 非有限 ⇒ L3（与坐标同一个判据）。
 /// 【谁消费】只有 `draw_image`；容器恒以中心为锚（/）。
    void set_image_anchor(std::uint64_t handle, double ax, double ay);

 /// 查图片锚点（查询语义：失效句柄 ⇒ false，不报错）。
    [[nodiscard]] bool image_anchor(std::uint64_t handle, double& out_ax,
                                    double& out_ay) const;

 /// 把**整个帧缓冲**清成信箱色（不做任何别的事）。
 ///
 /// @param pixel_w 帧缓冲宽（物理像素）
 /// @param pixel_h 帧缓冲高（物理像素）
 ///
 /// 【它是"第二个渲染入口" —— 因此约束极严，这三条来自实测
 /// （6.2.4 /）】
 /// ① **主线程判断由调用方做**：它的设计调用点是窗口暴露（EXPOSED）
 /// 的事件观察者回调，而 SDL 明说回调可能不在主线程；
 /// 不是主线程 ⇒ 调用方必须什么都不做（本函数不再判断）。
 /// ② **只许"清一次"**：不碰场景、不碰状态机、不碰钩子、不提交任何
 /// 顶点 —— 它与主帧循环的"一次提交一套状态"必须没有任何交集。
 /// ③ **预算 ≤ 5 ms**：实测"清屏 + 呈现"约 5 ms 且几乎全在呈现上；
 /// 拖拽期间每秒可能有几十次暴露，回调里多做的事都会直接吃掉帧预算
 /// （呈现由调用方紧跟在本调用之后做，两者合起来才是那次"最小重画"）。
 ///
 /// 【它为什么清信箱色而不是场景色】
 /// 拖拽期间画面本来就会被下一帧盖掉；信箱色与两段清屏的第一段一致，
 /// 于是"重开一帧"的观感（探针实测：使用者判定清整屏更像"重开一帧"）。
    void clear_framebuffer(int pixel_w, int pixel_h);

 /// **立即**重画"上一张完整画面"（暴露回调用；2026-10-18 落地）
 ///
 /// 【它和 clear_framebuffer 的区别】
 /// clear_framebuffer 只清成信箱色 ⇒ 拖拽期间客户区**全黑**（实测 100% 黑像素）。
 /// 本函数在清屏之后，把 **staged 快照**（上一帧的完整结果）1:1 画上去
 /// ⇒ 拖拽期间看到的是"定格的画面"，而不是黑屏。
 ///
 /// 【为什么它必须"立即"发 GL，而不能提交绘制项】
 /// 它运行在窗口消息路径里（SDL 事件观察者），此时**没有帧管线**：
 /// 没有收集、没有排序、也不会有人调 end_frame ⇒ 提交了也不会被发射。
 /// ⇒ 它是"第二个渲染入口"，与 clear_framebuffer 同一类，只是多做一步绘制。
 ///
 /// 【它对帧状态是中性的】不碰 transform_ / 帧绘制列表 / 批缓冲 / 统计；
 /// 下一帧的 begin_frame 与 flush_batch_ 会重设所有批状态（viewport、
 /// program、uniform、混合、纹理绑定、scissor）⇒ 不需要"还原"。
 ///
 /// 【没有快照时】退化为只清屏（与旧行为一致）——不是错误：
 /// 启动后第一帧就拖拽时，还没有任何完整画面可画。
    void redraw_last_frame(int pixel_w, int pixel_h);

 /// 【临时探针入口 · 交付前删除】把**外置着色器的目录**交给引擎。
 ///
 /// 【为什么是脚本给】：目录结构固定，且**引擎不解析路径**
 /// —— 拼接在 Lua 侧（脚本用 `resolve.luminosa_dir() .. "/Project/shaders"`）。
 /// ⇒ 引擎里因此**不含**任何 `Luminosa/...` 字面量。
 /// 【生效时机】下一次绘制时才建程序（那时脚本已经跑过 —— GL 资源建立时脚本还没有）。
    void set_shader_probe_dir(const std::string& dir);

 /// 【探针 · 2026-10-18】画 5 条外置 GLSL 效果竖带（采样 staged 快照）。
 ///
 /// 【调用点】**阶段 12（present 之前）、阶段 10 快照之后**。
 /// ⚠️ 不能在快照之前画：探针采样的是 staged 快照，若它的输出进了快照，
 /// 下一帧再采样就会**正反馈**，几帧内烂成噪声（实测踩过）。
 /// 【开关】renderer.cpp 里的 `kShaderProbe`；false 时它是空函数。
    void draw_shader_probe();

 // -------------------------------------------------------------------------
 // 绘制入口（**三个**）
 //
 // 【统一视角】把"顶点"换成"图元" ⇒ 三个入口是同一规则的两个取值加一个特例：
 // · `draw_image` **1 个图元** · 扁平数字（整张图，不用 UV）
 // · `draw_vertices` **1 个图元** · ≤ shape_vertex_limit 顶点 · C++ 顶点数组
 // · `draw_glyphs` **n 个图元** · 每图元固定 4 顶点 · 扁平图元描述数组
 // （每图元 9 个数：矩形 4 + UV 4 + 颜色 1）
 // ⇒ 所以 **32 的位置不再含糊**：它管"一个图元多大"，不管"一次调用多大"
 // （一次调用的量由批缓冲容量决定，装不下自动 flush）。
 //
 // ⚠️【draw_glyphs 的名字纪律】名字允许带使用者（一眼看懂用途），
 // 但**引擎内部的行为不得带字形语义** —— 不排版、不换行、不查字体。
 // 判据："这个原语的行为会不会因为『这是文字』而不同？"
 // 不会 ⇒ 合规；会 ⇒ 越界。名字不参与这个判据。
 //
 // ⚠️【共同的规则】三个入口共享：
 // · 同一个 `shape_vertex_limit`（单笔图元的顶点上限）
 // · 同一套校验（**校验在读出之前**、**整批校验**、有限值）
 // · 同一个事务边界（**一次调用**：一个坏图元 ⇒ 整次调用不发出）
 // · 同一套状态约束（顶点色模式/混合状态/雾/纹理 alpha —— 随变体批落地）
 // ⇒ 各处注释必须互相引用，否则将来会各自漂移。
 // -------------------------------------------------------------------------

 /// 纯位置版：把整张图按位置与大小画出来。
 ///
 /// @param image 图片句柄（**0 = 白纹理**：纯色矩形）
 /// @param layer 绘序层（64 位整数；更大者后画=在上）
 /// @param z 层内顺序键（浮点；更大者更靠前=后画=在上）
 /// @param x, y 目标位置（逻辑坐标，DIP）
 /// @param w, h 目标尺寸（逻辑单位，DIP）
 ///
 /// 【参数全是扁平的数字】不使用 UV，也不使用顶点结构。
 /// 【它的源是整张图】UV 由图片自己的尺寸给出：(0,0) → (图宽, 图高)，
 /// 于是"页与 UV 同源"那条在这里自动成立（不需要调用方给 UV）。
 ///
 /// 【锚点 = 图片**左上角**，落在逻辑 (x, y)】
 /// 逻辑坐标 +y 向上 ⇒ 矩形占 `x .. x+w` 与 `y-h .. y`。
 /// 将来 `SetImageCenter` 落地时，锚点改成"图片中心落在 (x, y)"：
 /// 左上 = (x − center.x, y + center.y)
 /// ⇒ `center` 当前恒为 (0,0)，所以上面那条式子**今天就是左上角**
 /// ⇒ 那时不需要改调用方语义（向后兼容）。
 ///
 /// 【它是一笔形状、4 顶点】⇒ 实现上**直接构 4 个顶点交给 `draw_vertices`**：
 /// 校验（有限值 / UV 越界）、变换（逻辑→窗口）、收集（排序键）全部复用同一份，
 /// 不新开第二条链 —— 否则"校验只有一份"立刻变成两份。
 ///
 /// ⚠️【2026-10-18 补 `layer`】原签名没有 layer，而绘序法要求"**所有绘制都带
 /// layer/z**"。漏了它 = 给这个入口一个不许排序的特权层 ⇒ 与定案冲突。
 /// 参数次序与 `draw_vertices(image, layer, z, …)` 对齐。
 ///
 /// 【2026-11-13 补取图矩形】`src == nullptr` ⇒ 画整张图（与旧行为
 /// **逐字节相同**）。给 `src` ⇒ 只画图上的那一块，坐标是**纹理像素**，
 /// 与 `draw_vertices` 的 UV 同一个单位（归一化在顶点着色器里做，只有一处）。
 ///
 /// 【为什么用指针而不是 4 个带哨兵值的默认参数】
 /// 哨兵值（如 -1）会造出一个"合法但不该出现"的数；而"有没有给矩形"
 /// 本身是个**有/无**的事实 ⇒ 指针直接表达它，编译期也没法误传半个矩形。
    void draw_image(std::uint64_t image, std::int64_t layer, double z,
                    double x, double y, double w, double h,
                    const source_rect* src = nullptr);

 /// 四任意点的四边形：**源固定为整张图**（纯位置版，无关，见）。
 ///
 /// @param points 12 个数字：x1,y1,z1, x2,y2,z2, x3,y3,z3, x4,y4,z4（按周边顺序）
 ///
 /// 【它是什么】`draw_vertices` 的**扁平糖**：不新增任何顶点管线 ——
 /// 内部只做"参数 → 4 个顶点"，然后与 `draw_image` 共用 `collect_quad`。
 /// 若各自拼顶点，校验/UV/变换一改就漏一份（那才是真正的"第二条路径"）。
 ///
 /// 【逐顶点的 z：接受但**不参与渲染**】深度测试关闭；顶点里的 z 一律写 0，
 /// 排序键是参数 `layer` / `z`。保留这三个数是为了让源系统的 `Render4V`
 /// 调用**原样搬得过来**（迁移零改动）。
 ///
 /// 【与 `draw_image` 的分工】轴对齐就用 `draw_image`（更快表达意图）；
 /// 需要四个任意点才用它。要逐顶点 UV / 逐顶点颜色 ⇒ 用 `draw_vertices`。
    void draw_quad(std::uint64_t image, std::int64_t layer, double z,
                   const double* points);


 /// 容器发射专用：按**图片自身的像素尺寸 × sx/sy** 画，并带顶点色。
 ///
 /// 【为什么不是给 draw_image 加参数】
 /// 那个入口是脚本面（w/h 显式给、颜色固定白）；容器发射的 w/h 应当来自
 /// 图片尺寸与结构体的 sx/sy —— 语义不同，不该挤进同一个签名。
 /// 【校验口径】数值来自结构体（spawn 已校验）；非有限值由 draw_vertices
 /// 的统一校验兜住（它本来就会报），这里不重复一遍热路径检查。
    void draw_image_scaled(std::uint64_t image, std::int64_t layer, double z,
                           double x, double y, double sx, double sy,
                           std::uint32_t rgba);

 /// 容器发射专用：每容器自带 blend / vcolor / tex_alpha，支持 rot 与负缩放。
 ///
 /// 【与 draw_image_scaled 的区别】
 /// · 状态从**容器字段**来，不读全局 `state_`（：三态是逐容器的）；
 /// · `rot` 绕四边形**中心**旋转（rot=0 时与 draw_image_scaled 完全一致）；
 /// · 负 sx/sy = 镜像：翻转 UV，不改变位置；
 /// · 直接算 4 个顶点、直接入帧顶点池，**不走** draw_vertices 的通用形状校验
 /// （容器字段在 spawn/write 已校验；这里只兜有限值）。
 ///
 /// 【排序】仍按 layer/z/来源/混合排序，vcolor/tex_alpha 只影响 flush 点。
 /// 【雾】容器不吃雾（：fog 固定关闭）⇒ 本项 fog 恒为默认。
    void draw_container_quad(std::uint64_t image, std::uint32_t frame,
                             std::int64_t layer, double z, double x, double y,
                             double sx, double sy, double rot, std::uint32_t rgba,
                             blend_state blend, vertex_color_mode vcolor,
                             tex_alpha alpha);
 /// 带 UV 版：提交一组顶点。
 ///
 /// @param image 图片句柄
 /// @param layer 本笔图元的**绘制顺序层**（64 位整数；更大者后画=在上）
 /// @param z 层内**顺序键**（浮点；更大者更靠前=后画=在上；无深度测试）
 /// @param verts 顶点数组（逻辑坐标下的位置 + 像素单位的 UV + 颜色）
 /// @param count 顶点个数；**必须 <= shape_vertex_limit**（单笔图元的顶点上限），否则报错
 ///
 /// 【溢出会报错，不自动拆分】见 shape_vertex_limit 的注释。
 /// 【校验在读出之前】已定："检查要在读表之前"。也就是说 count 的合法性
 /// 在读取任何顶点之前判定 —— 否则一个越界的 count 会先读到不属于这批
 /// 数据的内存。
 /// 【整批校验】位置与 UV 全部必须是有限值；任何一个不是，整批都不发出。
 ///
 /// 【校验顺序（四步，次序不可换 —— 全部来自已定规则）】
 /// ① count 上限（读任何数据之前）
 /// ② 退化输入：count 0/1/2 围不成面 ⇒ **静默返回**，不报错
 /// （1.6："引擎应当拒绝的不是开发者做的事看起来
 /// 没用，而是数据互相矛盾"）
 /// ③ 空指针（数据矛盾 ⇒ 报错）
 /// ④ 整批有限值校验（见 find_vertex_defect）
 /// ⇒ ④ 全过之后**不再有任何失败路径**：变换与提交不可能失败，
 /// 这就是"一次绘制调用是一个事务"的全部内容。
 ///
 /// 【失败走哪个入口 —— 一条已写明的缝（2026-10-15）】
 /// 本函数是 C++ 侧的内部契约（将来的容器管线直接调它），
 /// 校验失败 ⇒ fail_boundary（L3，到达即引擎 bug）。
 /// 而**脚本侧**的 wag.draw.draw_vertices 绑定必须**先用
 /// find_vertex_defect 预检、把失败抛成 Lua 错误**（pcall 接住 ⇒ L2，
 /// 整批不发、游戏继续）—— 那是2.6
 /// "出错走 Lua 错误"的落点。⇒ 脚本路径上 fail_boundary 不可达。
 /// ⚠️ 这条缝（"脚本侧边界校验走 L2 而决策表写 L3"）要在错误架构
 /// 收口那一批正式裁决；两处注释必须一起改。
    void draw_vertices(std::uint64_t image, std::int64_t layer, double z,
                       const vertex* verts, std::size_t count);

 /// 把 n 个**字形四边形**一次提交（2026-11-16，阶段1）。
 ///
 /// @param image 图集页句柄（**0 = 白纹理**，与 draw_image 同义）
 /// @param layer 绘序层
 /// @param z 层内顺序键
 /// @param flat **扁平数值数组**，每图元 **9 个数**：
 /// `x, y, w, h, u, v, uw, uh, color`
 /// （矩形左上 + 尺寸；UV 左上 + 尺寸，**纹理像素**；颜色 0xAARRGGBB）
 /// @param count `flat` 里数字的个数，**必须是 9 的倍数**
 ///
 /// 【它不认字形语义】不排版、不换行、不查字体。
 /// 名字里的"glyph"只说明**典型用途**，行为与"画一堆带 UV 的矩形"完全一致。
 ///
 /// 【一个坏图元 ⇒ 整批不发出】校验在**任何提交之前**整批做完
 /// （与 draw_vertices 同一事务边界）。
 ///
 /// 【提交形态】**一个字形 = 一次 `draw_vertices`（4 顶点）**。
 /// ⚠️ 不能把多个四边形凑成一次调用 —— `draw_vertices` 是**扇形语义**，
 /// 多个四边形会被连成一片（实测：第一个字形正常、后面全部拉丝）。
 /// 批缓冲会把这些图元合成一次 flush，所以"多次调用"不额外付 flush 代价。
    void draw_glyphs(std::uint64_t image, std::int64_t layer, double z,
                     const double* flat, std::size_t count);

 /// 整批校验一批顶点：所有位置与 UV 分量是否有限。
 ///
 /// @param out 发现缺陷时填入定位（索引 / 分量 / 实际值）
 /// @return true = 有缺陷（整批都不可发出）；false = 全部合格
 ///
 /// 【它只查不报】报告策略归调用方（见 draw_vertices 的"缝"说明）。
 /// 【粒度】这是"结构化/数组参数整批查一次"的那条实现（error.hpp 的
 /// require_all_finite 是它的 double 版兄弟；这里字段是 float，
 /// 提升为 double 后用同一个 isfinite 判定）。
 /// 【颜色不查】color 是无符号整数，天然有限（见 renderer.hpp vertex 注释）。
    [[nodiscard]] bool find_vertex_defect(const vertex* verts, std::size_t count,
                                          vertex_defect& out) const noexcept;

 // -------------------------------------------------------------------------
 // 渲染目标与快照
 // -------------------------------------------------------------------------

 /// 绑定自定义渲染目标。
 ///
 /// 【正当调用点】on_render_begin —— 它在清屏之后、任何绘制之前。
    void bind_render_target(std::uint64_t fbo);

 /// 解绑，回到默认渲染目标。
 ///
 /// 【正当调用点】on_render_end —— 它在所有绘制之后、呈现之前。
    void unbind_render_target();

 /// 把当前帧缓冲拷进暂存快照。
 ///
 /// 【调用点】阶段 10，且**只在显式请求时**（B 方案，2026-10-22）：
 /// 脚本调用 `wag.draw.capture_snapshot()` 置位，阶段 10 消费一次后清零。
 /// 不请求 ⇒ 不拍；正常帧不再有每帧 8.3 MB 的全屏 blit。
 /// 【休眠时不得调用】否则最小化会把快照清空。
    void capture_staged_snapshot();

 // -------------------------------------------------------------------------
 // 快照的显式请求（B 方案）
 // -------------------------------------------------------------------------

 /// 请求"本帧阶段 10 拍一张快照"。
    void request_staged_snapshot() noexcept { snapshot_requested_ = true; }
    [[nodiscard]] bool snapshot_requested() const noexcept {
        return snapshot_requested_;
    }
    void clear_snapshot_request() noexcept { snapshot_requested_ = false; }

 /// 把暂存的快照"取用"为冻结画面（进入暂停时用）。
 ///
 /// 【B 方案后没有调用者】暂停背景改由 `on_render_pause` 决定；
 /// 本函数与 `draw_frozen_snapshot` 保留待清理/待显式冻结 API。
    void adopt_staged_snapshot_as_frozen();

 /// 绘制冻结画面。
 ///
 /// 【B 方案后没有调用者】引擎不再预设暂停背景；
 /// 若未来要做"显式冻结"，由脚本先 capture、再自行绘制。
    void draw_frozen_snapshot();

private:
 // -------------------------------------------------------------------------
 // 四顶点提交的**唯一实现**（draw_image / draw_quad 共用）
 // -------------------------------------------------------------------------
 //
 // 【为什么要有它】两个入口都只做"参数 → 4 个顶点"的转换。若各自拼顶点、
 // 各自调 draw_vertices，就是把同一件事写两遍 —— 有限值校验 / UV 越界 /
 // 逻辑→窗口变换 / 收集 / 排序键，任何一处一改就漏一份。
 // ⇒ 这里只有一份：4 个顶点 + 一个源矩形（纹理像素）。
 //
 // 【顶点顺序】按周边顺序 TL → TR → BR → BL（扇形语义，与 draw_vertices 同）。
 // 【顶点里的 z 恒为 0】深度测试关闭；排序键是参数 `layer` / `z`。
 //
 // @param xy 8 个数：x1,y1, x2,y2, x3,y3, x4,y4（逻辑坐标）
 // @param 源矩形 u0,v0,u1,v1（**纹理像素**；归一化在顶点着色器里做，只有一处）
    void collect_quad(std::uint64_t image, std::int64_t layer, double z,
                      const double xy[8], double u0, double v0, double u1,
                      double v1);
 // -------------------------------------------------------------------------
 // GPU 侧资源（2026-10-15，子目标 1 建立）
 //
 // 【它们的生命周期】在 adopt_context 里创建；**没有显式销毁** ——
 // 它们随上下文一起消亡，而上下文的销毁属于关闭序列（第 5 步）。
 // "显式的资源释放"属于资源系统那一批（纹理池/程序池），到那时再收口。
 //
 // 【句柄类型】gl_api::uint_t（= GLuint 的别名）。0 表示"未创建"。
 // -------------------------------------------------------------------------

 /// 图形上下文。所有权在这里（由窗口交出）。
    platform::gl_context ctx_{};

 /// 本帧的变换。由 begin_frame 写入，本帧内只读。
    space::transform transform_ = space::transform::identity();

 /// 本帧的 Windows UI 显示比例。由 begin_frame 写入，仅作诊断。
 ///
 /// 【为什么它在这里而不是在 space 那边】
 /// 它不参与任何几何换算；变换与帧缓冲尺寸都不读它。
    double display_scale_ = 1.0;

 /// 着色器程序（**每变体一个**）。
 ///
 /// 【为什么是数组而不是"一个程序 + uniform 开关"】 已定：顶点色模式 /
 /// 纹理 alpha / 雾这三维**进着色器**，且**启动时全部预编译**
 /// （理由："第一次切到某效果时掉帧"是 STG 里最难查的一类抖动）。
 /// ⇒ 用变体，不用运行期分支。
 /// 【当前规模】只开了"纹理 alpha"这一维 ⇒ **2 个**（16 个是全部实现后的总数：
 /// 4 顶点色模式 × 2 纹理 alpha × 2 雾启用）。
 /// 【下标】= `tex_alpha` 的枚举值。
    gl_api::uint_t programs_[pipeline_variant_count] = {};

 /// 默认白纹理（1×1，不透明）。
 ///
 /// 【它为什么存在】"无纹理的纯色绘制"需要一个合法的采样目标。
 /// 用一张全 1 纹理，则 texture × 顶点色 = 顶点色 ⇒ draw_vertices
 /// 在没有绑定任何图片时也能出正确的纯色。它是"纹理上传"这条路的
 /// 最小实例（glTexImage2D 路径打通的最小验收）。
    gl_api::uint_t white_texture_ = 0;

 /// 顶点数组对象 / 顶点缓冲 / 索引缓冲（各一个；批形态乙下全体绘制共用）。
    gl_api::uint_t vao_ = 0;
    gl_api::uint_t vbo_ = 0;
    gl_api::uint_t ibo_ = 0;

 // -------------------------------------------------------------------------
 // 批缓冲 —— 【形态乙】
 //
 // draw_* 调用把校验合格的顶点**追加**进这里（不直接碰 GL）；
 // flush 只有三处触发：①装不下下一笔 ②状态变化（混合/纹理页/渲染目标，
 // 随资源与状态两批落地）③帧末（end_frame）。**不得有第四种隐式触发**
 // （否则破坏"引擎不替你决定批次边界"—— 那条 2026-10-15 的形态甲注记
 // 所担心的"引擎决定批次边界"问题，已由 2026 定案正面回答：
 // **容量决定 flush 次数（性能）；调用决定事务边界（正确性）**，两者分立）。
 //
 // 【为什么是成员而不是栈上数组】8192 × 24 B ≈ 196 KB ⇒ 放栈上必爆。
 // renderer 经 engine::impl 在堆上构造 ⇒ 一次性占位、长期复用、零逐帧分配。
 //
 // 【索引化】图元以扇形语义提交（n 顶点），展开为三角形需要索引。
 // 用索引缓冲（u16；顶点数 ≤ 8192 < 65536）而非展开顶点：
 // 8192 顶点 = 2048 个四边形，与定案给的档位算术（"约 2000 发一批"）一致。
 // 索引容量取 3 × 8192（最坏情形：全部单顶点退化图元；实际远小于此）。
 //
 // 【flush 触发 ② 的现状】混合状态 / 纹理页 / 渲染目标三个维度随
 // 资源系统（page）与混合状态表两批落地；当前只有单一状态，
 // 实际生效的触发是 ① 与 ③ —— 结构已按三触发留好。
 // -------------------------------------------------------------------------
    vertex batch_buffer_[batch_buffer_capacity];
    std::uint16_t batch_indices_[batch_buffer_capacity * 3];
    std::size_t batch_vertex_count_ = 0;
    std::size_t batch_index_count_ = 0;

 /// 渲染侧统计（“不设上限 ≠ 不提供证据”）。
 /// 自上次 take 起的累计；只做整数累加，统计本身不许成为开销。
    std::uint64_t stat_submissions_ = 0;
    std::uint64_t stat_flushes_ = 0;
    std::uint64_t stat_vertices_ = 0;
    std::uint64_t stat_primitives_ = 0;
    std::uint64_t stat_captures_ = 0;
    std::uint64_t stat_capture_pixels_ = 0;

 // -------------------------------------------------------------------------
 // 快照画布（staged）—— 帧快照的真身与"从快照取图"的来源
 //
 // 【真身是 FBO，不是纹理】（快照捕获与网格绘制）
 // FBO 本身不可采样；可采样的是它挂的**颜色附件纹理**。
 // 阶段 10 每帧把默认帧缓冲 blit 进来（capture_staged_snapshot）；
 // LoadImageFromSnapshot 从这里再拷一块成为图片。
 // 【尺寸】跟随帧缓冲像素尺寸（begin_frame 写入）；变化时重建
 // （重建即"快照清空"⇒ staged_valid_ 复位，恢复后第一帧自动重拍）。
 // 【frozen 那一半】staged → frozen 的取用、冻结画面绘制 ⇒ 屏幕与画布批。
 // -------------------------------------------------------------------------
    gl_api::uint_t staged_fbo_ = 0;
    gl_api::uint_t staged_color_tex_ = 0;
    int staged_w_ = 0;
    int staged_h_ = 0;
    bool staged_valid_ = false;

 /// B 方案（2026-10-22）：本帧是否请求过拍快照。阶段 10 消费后清零。
    bool snapshot_requested_ = false;

 // -------------------------------------------------------------------------
 // 冻结快照（frozen）—— **B 方案后没有调用者**（保留待清理）
 //
 // 【它与 staged 的关系】
 // staged 是"每帧都刷新"的暂存（阶段 10 捕获，供 LoadImageFromSnapshot 取图）；
 // frozen 是"只在切换到 Paused 那一刻"从 staged 取用的**静止画面**。
 // 取用方式是**交换**（adopt_staged_snapshot_as_frozen），不是拷贝 ——
 // 所以它不会失败，于是"冻结画面与状态切换是同一个原子事件"成立。
 // 【为什么交换之后 staged 变成"空"】
 // 交换后 staged 拿到的是上一轮的 frozen（首次为 0）⇒ staged_valid_ = false
 // ⇒ 下一帧的 capture_staged_snapshot 会重新 blit 进来。
 // 这顺带形成双缓冲：连续暂停/恢复不会每帧重建纹理。
 // 【尺寸】跟随帧缓冲（与 staged 同尺寸，因为它是从 staged 换过来的）。
 // -------------------------------------------------------------------------
    gl_api::uint_t frozen_fbo_ = 0;
    gl_api::uint_t frozen_color_tex_ = 0;
    int frozen_w_ = 0;
    int frozen_h_ = 0;
    bool frozen_valid_ = false;

 // -------------------------------------------------------------------------
 // 渲染目标（离屏）—— 光通道的第一块地基（2026-11-18）
 //
 // 【它是什么】一张可采样的颜色纹理 + 一个挂它的 FBO。
 // 与 staged 快照同构，但**用途相反**：快照是从屏幕拷进来，
 // 渲染目标是"往它上面画"，画完再把纹理拿去采样。
 // 【格式】RGBA8（遮蔽图/掩码）或 RGBA16F（光图；D1/D8 已定）。
 // 【尺寸】光通道固定半分辨率（962×405 量级，见 D7），不跟窗口。
 // -------------------------------------------------------------------------
    struct render_target {
        gl_api::uint_t fbo = 0;
        gl_api::uint_t tex = 0;
        int w = 0;
        int h = 0;
        bool hdr = false;
    };


 /// 遮蔽图（D1：RGBA8、半分辨率；遮挡者池每帧画进来）
    render_target occluder_mask_;

 /// 光图（RGBA16F、半分辨率；光通道画进来，合成时上采样）
    render_target light_map_;

 /// 本带对象掩码（self_only 用；RGBA8、半分辨率）
    render_target band_mask_;
    bool band_mask_active_ = false;

 /// 曝光（D6.5：全局每帧一个值；只作用于光，不作用于场景）
    float exposure_ = 1.0f;

 /// 光照覆盖的世界矩形；与 viewport 的 render 分离。
    space::rect light_space_{-960.0, -540.0, 1920.0, 1080.0};
    bool light_space_explicit_ = false;
    bool light_space_initialized_ = false;

    /// 当前 render 空间；band mask 采样必须用它，而不是 light_space。
    space::rect render_space_{-960.0, -540.0, 1920.0, 1080.0};

 /// 本帧的光（一次性；帧末渲染后清空）
    std::vector<light::light> lights_;

 /// 遮挡者池（按名字；句柄 = gen26|pool12|index15）
    std::map<std::string, light::occluder_pool> occluder_pools_;
    std::map<std::string, int> occluder_pool_ids_;
    int next_occluder_pool_id_ = 1;

 /// 光 shader 目录（空 = 用内嵌测试 shader）
    std::string light_shader_dir_;
    bool light_prog_dirty_ = true;

 // 帧内光照计数（；begin_frame 清零，take 取走）
    std::uint32_t frame_commits_ = 0;
    std::uint32_t frame_lights_max_ = 0;
    std::uint64_t frame_fill_px_ = 0;
    bool warned_light_count_ = false;
    bool warned_fill_ = false;
    bool warned_commits_ = false;
    bool new_over_lights_ = false;
    bool new_over_fill_ = false;
    bool new_over_commits_ = false;
    light_stats_snapshot last_light_stats_{};

 /// 带（每帧声明；帧末清空）
    struct band_def {
        std::string id;
        std::int64_t from = 0;
        std::int64_t to = 0;
        bool committed = false;
    };
    std::map<std::string, band_def> bands_;

 /// 绑定/解绑要保存与恢复的 GL 状态（viewport / scissor / 之前的 FBO）
    gl_api::int_t saved_viewport_[4] = {0, 0, 0, 0};
    gl_api::int_t saved_scissor_[4] = {0, 0, 0, 0};
    bool saved_scissor_enabled_ = false;
    gl_api::uint_t saved_fbo_ = 0;
    bool target_bound_ = false;

 /// 本帧的帧缓冲像素尺寸（begin_frame 写入；快照的尺寸来源）。
 ///
 /// 【它为什么回来了】子目标 4 清理时曾以"没有消费者"删除；
 /// 现在有了真实消费者（快照的尺寸与捕获矩形的越界检查）⇒ 恢复。
 /// 与 geom.pixel 同值同源（begin_frame 一次算出），不是第二个来源。
    int fb_w_ = 0;
    int fb_h_ = 0;

    // 内部场景目标（永远 RTT；默认 1920×1080）。
    render_target scene_target_{};
    int scene_w_ = 1920;
    int scene_h_ = 1080;
    present_mode present_mode_ = present_mode::letterbox;
    present_filter present_filter_ = present_filter::linear;

    /// 最近一次 present 的目标矩形（输出像素，左上原点）。
    space::rect present_box_{};

    // 当前输出/后缓冲尺寸（来自窗口 pixel size）。
    int output_w_ = 0;
    int output_h_ = 0;

    // present pass 资源。
    gl_api::uint_t present_prog_ = 0;
    gl_api::uint_t present_vao_ = 0;
    gl_api::uint_t present_vbo_ = 0;
    gl_api::int_t present_u_tex_ = -1;
    bool present_ready_ = false;

 // -------------------------------------------------------------------------
 // 图片族（资源系统的种子 —— 2026 先以最小形态住在渲染器，
 // 资源批落地时整体迁往资源模块；句柄/代数/覆盖规则从第一天就按定案）
 //
 // 【句柄】= (代数 << 21) | 槽位（低 21 位槽位 / 高 43 位代数，
 // 全值 < 2^53 ⇒ Lua double 精确表示 —— 与对象/资源句柄同形）。
 // 【失效】唯一规则：代数不匹配 ⇒ 无效；卸载 = gen + 1。
 // 【覆盖】同族同名 ⇒ 卸载 + 加载（旧句柄自动失效），不报错。
 // 【默认过滤】照片 = linear（与全局默认 nearest 不同的**明确例外**，
 // 快照捕获的过滤规则）。
 // -------------------------------------------------------------------------
    static constexpr std::size_t image_family_capacity = 1024;

 /// 80% 水位（的同一判据；ceil(0.8 × 1024) = 820）。
    static constexpr std::size_t image_watermark =
        (image_family_capacity * 4 + 4) / 5;

 /// 图片代数达到此值后，句柄超出 `container.img` 的 u32 范围（仅容器路径受影响）。
 ///
 /// 【为什么是 2048】handle = gen << 21；gen == 2048 ⇒ handle >= 2^32。
 /// 容器结构体的 img 是 u32；draw_image / draw_vertices 走 u64，不受影响。
    static constexpr std::uint32_t image_gen_container_limit = 2048;


    struct image_slot {
        std::uint32_t tex = 0;        ///< 颜色纹理（图片的真身）
        std::uint32_t gen = 0;        ///< 代数；0 = 空槽
        std::uint32_t width = 0;      ///< 像素宽
        std::uint32_t height = 0;     ///< 像素高
 /// **帧表**（：图像组 = 带帧表的图片）。
 ///
 /// 【空 ⇒ 普通图片】此时"帧数 n = 1"，`frame` 只能是 0。
 /// 【非空 ⇒ 图像组】n = size()；`container.frame` 的合法集合是 [0, n)。
 /// 【为什么挂在图片上而不是独立对象】 把容器钉死 64 B，加不了 group 字段；
 /// 而"组名 = 图片名"让 `image_by_name_` 直接复用。
        std::vector<frame_rect> frames;
 /// **锚点**（，2026-11-17）：归一化 0..1，默认 (0,0) = 左上。
 ///
 /// 【整张图一个】不逐帧 —— 逐帧动画还变锚点会让"轴心"变成运动的一部分。
 /// 【任意有限值】1.5 = 轴心在矩形外，是合法表达（不钳制）。
 /// 【存放位置】它是**资源属性**，不是容器字段（与它无关）。
        float ax = 0.0f;
        float ay = 0.0f;
        std::string name;             ///< 反查用（报错可解释"图片 X 的纹理没了"）
    };
    std::vector<image_slot> images_;
    std::vector<std::uint32_t> image_free_;       ///< 空闲槽位栈
    std::unordered_map<std::string, std::uint32_t> image_by_name_;  ///< 名字 → 槽位
    bool image_watermark_hit_ = false; ///< 已报过 80%；回落到水位下再清

 /// **帧末才销毁的纹理**（"卸载在帧末生效"的实现）。
 ///
 /// 【为什么不能立刻删】绘制是**先收集、后发射**的：
 /// 脚本在渲染钩子里 `draw_XXXX(...)` 之后立刻 `wag.image.unload(name)` 时，
 /// 那一笔的 `draw_item.tex` 已经被记下、但 `glBindTexture` 还没发生。
 /// 若此时就 `glDeleteTextures`，帧末 flush 会绑到一个**已删除的名字**
 /// ⇒ GL 不报错、**静默黑图或旧图**。
 /// ⚠️ 而"使用失效资源句柄 ⇒ 立即报错"那条**抓不到它**：
 /// 句柄校验在**收集时**做（那时它还活着）。
 ///
 /// 【为什么用"挂起队列"而不是"延迟释放槽位"】
 /// 槽位必须能立刻复用 —— 否则"覆盖 = 同槽位 + 代数 +1"这条就不成立了。
 /// 而纹理名可以脱离槽位活下去 ⇒ 所以这里存的是**被摘下来的 GL 纹理名**，
 /// 槽位照旧立刻回 free-list。
 ///
 /// 【什么时候清】`end_frame()` 末尾（那时本帧所有 flush 都已完成）。
 /// 若中途没有帧（休眠帧不调 end_frame），它就留到下一个 end_frame —— 无害。
 /// 【关闭时】不清也**不漏**：GL 纹理随上下文一起消失（`shutdown_sequence`
 /// 在销毁窗口前销毁上下文），而本 vector 随对象一起析构。
 /// ⇒ 所以析构里**不调 glDeleteTextures**（那时上下文可能已经没了）。
    std::vector<gl_api::uint_t> pending_destroy_tex_;
 /// 三个顶点属性在程序里的位置（链接后查询得到，见 renderer.cpp 的说明）。
    gl_api::int_t attrib_pos_ = -1;
    gl_api::int_t attrib_uv_ = -1;
    gl_api::int_t attrib_color_ = -1;

 /// `a_z` 的位置（**雾的距离量**，2026-10-18 新增）。
 /// 【为什么现在才要它】在此之前 `vertex::z` 是"布局里留的洞"：不进属性、
 /// 不参与任何计算（深度测试已关）。雾需要**逐顶点**的距离并被插值
 /// ⇒ 它必须先成为一个真正的顶点属性。
    gl_api::int_t attrib_z_ = -1;

 /// u_pixel_to_ndc 的 uniform 位置（**每变体一个**）。
    gl_api::int_t uniform_pixel_to_ndc_[pipeline_variant_count] = {};

 /// `u_tex_inv_size` 的 uniform 位置（**UV 归一化用的倒数**）。
 ///
 /// 【为什么需要它】契约里顶点 UV 是**像素单位**（原点左上），
 /// 而 GLSL 的 `texture()` 吃 **[0,1] 归一化**坐标 ⇒ 必须除纹理尺寸。
 /// 【为什么放 GPU 侧】与"颜色字节序在片元着色器里用 swizzle 修正、不在 CPU 侧
 /// 重排字节"是**同一条判据**：CPU 侧归一化是每顶点一次运算，而 uniform 是免费的；
 /// 而且**顶点池里保留原始像素 UV**（调试看得见图集坐标，将来 texelFetch 也要它）。
 /// 【在哪个阶段做】**顶点着色器**（乘法）—— 除以常数是仿射，所以
 /// "除后再插值" 与 "插值后再除" 结果相同，而顶点数远少于像素数。
 /// 【纹理数组之后仍成立】数组各层共享 W/H ⇒ 它仍是 `vec2`，layer 只进第三分量。
    gl_api::int_t uniform_tex_inv_size_[pipeline_variant_count] = {};

 /// 雾的四个 uniform（**每变体一个**；只有启用雾的变体里才存在）。
 /// 【为什么是数组】每个程序有自己的一套 location（与上面两个同理）。
 /// 【无雾变体里这些位置是 -1】GLSL 编译器会把未被使用的 uniform 移除
 /// ⇒ "取不到"在无雾变体里是**正常**的，不能当错误。
    gl_api::int_t uniform_fog_color_[pipeline_variant_count] = {};
    gl_api::int_t uniform_fog_range_[pipeline_variant_count] = {};   ///< (near, far)
    gl_api::int_t uniform_fog_strength_[pipeline_variant_count] = {};

 /// 发射时当前绑定的纹理与其尺寸倒数（flush 触发②的状态）。
 /// 【帧初值】白纹理 + (1,1) —— 1×1 白纹理对任意 UV 都采到同一个白 texel。
    gl_api::uint_t pending_tex_ = 0;
    gl_api::float_t pending_inv_size_[2] = {1.0f, 1.0f};

 /// 发射时当前生效的混合状态（flush 触发②的另一半，2026-10-18）。
 /// 【帧初值】`Normal` —— 与"批缓冲里没有东西"时的心智模型一致。
    blend_state pending_blend_ = blend_state::normal;

 /// 发射时当前生效的纹理 alpha 类型（flush 触发② 的第三半）。
 /// 【为什么它也要 pending】换它要换**程序** ⇒ 必须先把已攒的送走
 /// （同一条理由：批里的顶点共用一套 GL 状态）。
 /// ⚠️ 但它**不进排序键**（：它不是"前序内容的函数" ⇒ 顺序无关）
 /// ⇒ 只改 flush 点，不重排。**这正是它与混合状态的区别。**
    tex_alpha pending_alpha_ = tex_alpha::normal;

 /// 发射时当前生效的顶点色模式（flush 触发② 的第四半）。
 /// 【同上】换它要换程序 ⇒ 必须 flush；但**不重排**。
    vertex_color_mode pending_vcolor_ = vertex_color_mode::mul;

 /// 发射时当前生效的雾参数整组（flush 触发② 的第五半）。
 /// 【为什么整组比较】启用位决定"用哪个程序"，而颜色/near/far/强度是
 /// **uniform** —— 它们变了也必须 flush（否则新 uniform 会改到已攒的图元）。
 /// 【同上】不进排序键，只改 flush 点。
    fog_state pending_fog_;

 /// **当前状态**（三态里的"未设置 / 已显式设置"）。
 /// 生效值 = 已设 ⇒ 用它；未设 ⇒ 用该维默认（混合维默认 = `Normal`）。
 /// 粘性：不帧末重置。
    draw_state_override state_;
 /// 本帧的"窗口像素 → NDC（以视口盒为参照）"矩阵，3×3 列主序。
 ///
 /// 【已定（2026-10-15，子目标 1）：像素 → NDC 的换算归属】
 /// 顶点在 CPU 侧走完 ①→②（逻辑 → 窗口像素）之后以 float 进 GPU；
 /// 本矩阵在 GPU 侧完成 ② → NDC，而它**由 `begin_frame` 里那个 box
 /// 推导** —— 就是喂给 glViewport / glScissor 的同一个 box。
 /// ⇒ 视口设置的三个消费者（viewport / scissor / 顶点矩阵）共用
 /// **②→③ 那一次翻转的唯一产物**，没有第二个来源可漂移。
 ///
 /// 【它含一次 y 方向反转（斜率为负）—— 这不是"第三次链上翻转"】
 /// NDC 的 +1 是"上"，而窗口坐标的 +y 是"下"，矩阵的 d 分量因此为负。
 /// 它与 `framebuffer_box_for` 的减法是**同一次 ②→③ 翻转的两个表达面**：
 /// box 管"视口矩形在帧缓冲里的位置"，矩阵管"顶点在 NDC 里的位置"，
 /// 两者从同一个 box 推出，净效果一致（删掉任何一个，画面立刻上下颠倒
 /// —— 与坐标链上那两个负号完全同构）。
 ///
 /// 【为什么列主序的 9 个 float】glUniformMatrix3fv 按列主序读，
 /// 且 GL 的矩阵元素是 float —— 存成 float 意味着每帧零转换。
    gl_api::float_t pixel_to_ndc_[9] = {1.0f, 0.0f, 0.0f,
                                        0.0f, 1.0f, 0.0f,
                                        0.0f, 0.0f, 1.0f};

 // ---------------------------------------------------------------------
 // 【探针 · 2026-10-18】外置 GLSL 视觉探针（5 个独立程序）
 //
 // 【它为什么不进变体账】探针不是渲染能力的一部分：它是"拿来试接口"的
 // 一次性装置，用完全独立的程序画几条竖带，**不碰** 16 个变体。
 // ⇒ 关掉它（kShaderProbe = false）对引擎行为零影响。
 // ---------------------------------------------------------------------
    gl_api::uint_t probe_fx_prog_[5] = {};
    gl_api::int_t probe_fx_u_time_[5] = {};
    gl_api::int_t probe_fx_u_tex_[5] = {};

 /// 【目录由脚本给】：引擎不解析路径 ⇒ 引擎只持有这个字符串。
 /// 空 = 脚本没调过 set_shader_probe_dir ⇒ 探针不工作（这不是错误）。
    std::string probe_dir_;
    bool probe_built_ = false;

 /// 建立探测程序（读脚本给的外置目录，与 probe_vertex.glsl 链接）。
 /// 【调用点】draw_shader_probe 的首次绘制（延迟到脚本跑过之后）。
    void build_shader_probe_();



 /// 创建 GPU 侧资源（着色器 / 白纹理 / VAO / VBO）。
 ///
 /// 【失败即 L3 终止，因此没有返回值】着色器编译失败一类的失败属于
 /// "不可降级的资源失败"—— 引擎没有"没有着色器也能跑"的形态。
 /// 【调用点】adopt_context 的末尾（GL 入口已取回、上下文已当前）。
    void init_gl_resources_();

 /// 确保 staged 快照的 FBO 与颜色附件纹理存在且尺寸正确（帧几何变化时重建）。
    void ensure_staged_snapshot_(int fb_w, int fb_h);

 /// 渲染目标：确保存在（尺寸/格式变了就重建）
    void ensure_render_target_(render_target& t, int w, int h, bool hdr);
    /// present pass：建 shader / VAO / VBO（一次）。
    void ensure_present_resources_();
 /// 灯光填充率：**唯一来源**（警告、快照、Lua stats 共用同一分母）
    [[nodiscard]] double light_fill_ratio_() const noexcept;
 /// 渲染目标：销毁（幂等）
    void destroy_render_target_(render_target& t);
 /// 渲染目标：绑定为当前绘制目标（保存并改写 viewport/scissor）
    void bind_render_target_(render_target& t);
 /// 渲染目标：解绑，恢复之前保存的 viewport/scissor/FBO
    void unbind_render_target_();
 /// 临时自检（WAG_RT_SELFTEST=1），验收后删除
    void debug_rt_selftest_();

 /// 临时自检（WAG_INSTANCE_SELFTEST=1）：实例化绘制，验收后删除
    void debug_instance_selftest_();
 /// 把遮挡者池画进半分辨率遮蔽图（RGBA8：R=覆盖，B=透射）。
    void render_occluder_mask_();
 /// 遮蔽图自检：远/近两个重叠遮挡者，近的必须赢。
    void debug_mask_selftest_();
 /// 光通道自检：一盏点光 → 光图 → 加色合成。
    void debug_light_selftest_();
 /// 带自检：两条带各自一盏光，互不串色。
    void debug_band_selftest_();
 /// self_only 自检：光只照本带对象。
    void debug_selfonly_selftest_();
 /// 预算自检：光源数/填充/commit 三个阈值同时触发。
    void debug_budget_selftest_();

 /// 光通道：遮蔽图 → 光图 → 加法合成（每帧，end_frame 调用）
    void render_light_pass_();

 /// 按带的范围，把本带对象（draw_items_ 里 layer 落入范围内的）画进掩码。
    void render_band_mask_(std::int64_t from, std::int64_t to);

 /// 图片族的内部卸载（供覆盖与显式 Unload 共用）：代数 +1、移出名字表、
 /// **纹理名推进挂起队列**（帧末才真删，见 `pending_destroy_tex_`）。
    void unload_image_slot_(std::uint32_t slot);

 /// 图片代数到 `image_gen_container_limit` 时记一条 L2：
 /// 句柄已超出 container.img（u32），容器将无法引用；draw_image 不受影响。
    void warn_if_image_gen_too_high_(const char* name, std::uint32_t slot) const;

 /// 更新图片族 80% 水位：达到时记一次 L1；回落到水位下清标记。
    void update_image_watermark_();

 /// 销毁挂起的纹理（`end_frame()` 末尾调用；析构与关闭序列也会调）。
    void flush_pending_destroy_tex_();

 /// 删除退役的着色器 program（`end_frame()` 在 flush 之后调用）。
    void flush_retired_shaders_();

 /// 把一笔校验合格的图元**收集**进帧绘制列表（排序在 end_frame 统一做）。
 ///
 /// 【前置条件】数据已整批校验合格、已变换到窗口像素；n ≤ shape_vertex_limit。
 /// 【它做什么】①顶点追进帧顶点池 ⇒ ②记一个带排序键的绘制项
 /// （拓扑 = 扇形；创建序 = 全局单调计数器）。没有任何 GL 调用。
 ///
 /// @param tex 本次绘制用的纹理（0 = 白纹理）
 /// @param inv_w/h 该纹理尺寸的倒数（发射时上传为 `u_tex_inv_size`）
 /// @param shader_program 已解析的自定义着色器 program（0 = 内建变体）
    void collect_primitive_(const vertex* gpu, std::size_t count,
                            std::int64_t layer, double z,
                            gl_api::uint_t tex, double inv_w, double inv_h,
                            gl_api::uint_t shader_program);

 /// 把一次收集（三角形列表语义）追进帧绘制列表。拓扑 = 三角形列表。
    void collect_triangles_(const vertex* gpu, std::size_t count,
                            std::int64_t layer, double z,
                            gl_api::uint_t tex, double inv_w, double inv_h,
                            gl_api::uint_t shader_program);

 /// 把当前 shader 状态解析成 program；0 = 内建。
 ///
 /// 【失效 ⇒ L3】这里不返回 nullptr 让调用方猜：当前状态非 0 但句柄
 /// 已失效（被卸载/覆盖）⇒ 直接 fail_boundary。
    [[nodiscard]] gl_api::uint_t resolve_draw_shader_();

 /// 拒绝"逻辑坐标绘制 + 自定义着色器"的组合（L3）。
 ///
 /// 【为什么拒绝】draw_image / draw_mesh 的坐标是逻辑坐标，而 shader 模式
 /// 把顶点当整帧像素；两者同时出现会静默画错，而不是报错。
    void reject_shader_state_(const char* what) const;

 /// 图片句柄 → (纹理, 像素宽, 像素高)。
 ///
 /// @return false = 句柄失效（代数不匹配 / 槽位越界 / 未加载）
 /// 【它做的是"使用"而不是"查询"】调用方拿到 false 应当**报错**，
 /// 而不是静默不画（使用失效资源句柄必须报错）。
    [[nodiscard]] bool image_texture_(std::uint64_t handle, gl_api::uint_t& tex,
                                      double& pixel_w, double& pixel_h) const;

 /// 给错误信息用的图片描述：`handle=... 图名=...`（的诊断要求带图名）。
 /// 句柄已失效 / 名字已摘除时，图名部分会明确说"不可用"。
    [[nodiscard]] std::string image_desc_(std::uint64_t handle) const;

 /// **排序 + 发射**：帧绘制列表按排序键稳定排序，逐项写入批缓冲
 /// （装不下 ⇒ flush 触发①；批键变化 ⇒ flush 触发②——随状态批落地；
 /// 全部发完 ⇒ flush 触发③）——然后清空帧绘制列表。
 ///
 /// 【排序键（从高到低）】
 /// 画布/目标（当前常量 screen）→ 阶段 → layer → z → 来源（池/脚本）
 /// → 混合状态（随混合状态批落地）→ 创建序。
 /// 【稳定排序】同一容器被画多次时两次的排序键完全相同 ⇒
 /// 必须稳定排序（先收集者先画）—— 它是"同一对象一帧多次绘制"的
 /// 唯一但必需的前提。
    void sort_and_emit_draws_();

 /// **flush**：把批缓冲里攒的图元一次性提交（设状态 → 上传 VBO/IBO → draw）。
 ///
 /// 【它就是"提交尾"】draw_vertices / draw_image / draw_glyphs 的终点，
 /// 也是 flush 三触发（装不下 / 状态变化 / 帧末）的共同落点。
 /// 【前置条件】缓冲内容全部校验合格（入批前已保证 ⇒ 无失败路径）。
 /// 【空缓冲】直接返回（不计 flush 次数 —— 没有发生提交）。
    void flush_batch_();

 // -------------------------------------------------------------------------
 // 批重的持有方式 —— 【2026 定案：形态乙（成员批缓冲）】
 //
 // 2026-10-15 的“形态甲（每次调用立即提交）”**已被
 // 定案取代**：批缓冲容量 8192、
 // flush 只有三处触发、事务边界 = 一次调用。
 // 当时形态甲留下的两个挂点（“draw_* 尾部替换成累积”与
 // “flush 时机归谁”）均已由该定案回答：尾部 = append_primitive_；
 // 时机 = 装不下 / 状态变化 / 帧末，三者之外无隐式触发。
 //
 // 【2026-10-16 二次演进：绘序统一管线（子目标 2）】
 // draw_* 调用改为**收集**（变换后的顶点进帧顶点池 + 带排序键的绘制项），
 // end_frame 统一"稳定排序 → 逐项写入批缓冲 → flush"。
 // 排序键与稳定性见 sort_and_emit_draws_ 的说明；
 // 批缓冲（下面的数组）退为**发射目标**，职责更纯了。
 // -------------------------------------------------------------------------

 // -------------------------------------------------------------------------
 // 自定义着色器（2026-10-19）
 //
 // 【它是什么】作者写一份**片元** GLSL，引擎用**内嵌的顶点着色器**与它链接。
 // ⇒ 顶点侧 ABI（属性名 a_pos/a_uv/a_color/a_z，输出 v_uv/v_color/v_dist）
 // 由引擎保证，作者只关心片元。
 // 【为什么 VS 内嵌】否则作者要写两份文件，而且 ABI 容易对不上。
 // 【句柄】= (代数 << 6) | (槽位 + 1)；0 表示"没有"。
 // 与图片句柄同一形态：**带代数，卸载即失效**（：失效句柄使用 ⇒ L3）。
 // 低 6 位放"槽位 + 1"（容量 32 ⇒ 1..32），高 26 位放代数。
 // -------------------------------------------------------------------------
    struct custom_shader {
        gl_api::uint_t program = 0;
        std::uint32_t gen = 0; ///< 槽位代数；0 = 从未使用过
        gl_api::int_t u_pixel_to_ndc = -1;
        gl_api::int_t u_tex_inv_size = -1;
        gl_api::int_t u_time = -1;
        gl_api::int_t u_resolution = -1;
        gl_api::int_t u_params = -1;
        gl_api::int_t u_tex_size = -1;
 /// 渲染盒（像素矩形；y 以帧缓冲左下为原点）—— 2026-11-16 新增（C3）。
 ///
 /// 【为什么给着色器】`v_uv` 覆盖的是**那个四边形**，而 `u_box` 覆盖的是
 /// **可见区域**。两者口径不同：窗口一变，四边形的映射变了而 uv 没变
 /// ⇒ 只靠 `v_uv` 的背景会变形/露底。作者应当用
 /// `uv = (gl_FragCoord.xy - u_box.xy) / u_box.zw` 算 uv。
 ///
 /// 【为什么它只给着色器、不给脚本】它要在渲染阶段才算出来（依赖帧尺寸
 /// 与信箱）；脚本在 `on_frame_*` 读到的是**上一帧**的盒。见1.5。
        gl_api::int_t u_box = -1;
        std::string name;
    };

 /// 活动着色器槽：下标 i 对应槽位 i；`program == 0` 表示空槽。
    std::vector<custom_shader> shaders_;

 /// 本帧已卸载、但 program 必须活到帧末的着色器。
 ///
 /// 【为什么需要】绘制是"先收集、后发射"的；本帧已收集的绘制项持有
 /// 已解析的 program（不是句柄）。若卸载时立刻 glDeleteProgram，
 /// 帧末发射就会用到已删除的程序。⇒ 与纹理的 `pending_destroy_tex_`
 /// 同形：句柄立刻失效，GPU 对象帧末才删。
    std::vector<custom_shader> retired_shaders_;

 /// 本帧退役 program 的告警/硬上限（只影响“一帧内卸载次数”，不影响活动槽）。
    static constexpr std::size_t retired_shader_warn = 32;
    static constexpr std::size_t retired_shader_cap = 128;
    bool retired_shader_warned_ = false;
    bool shader_watermark_hit_ = false;
    bool shader_load_in_progress_ = false;

 /// 取句柄对应的**活动**着色器（越界/代数不匹配/已释放 ⇒ nullptr）。
    [[nodiscard]] const custom_shader* find_shader_(std::uint32_t handle) const noexcept;

 /// 按 program 找活动或退役的着色器（发射时取 uniform 位置用）。
    [[nodiscard]] const custom_shader* find_shader_by_program_(
        gl_api::uint_t program) const noexcept;

 /// 更新活动着色器 80% 水位：达到时记一次 L1；回落到水位下清标记。
    void update_shader_watermark_();

 /// 活动槽上限：满 ⇒ L3（与图片容量同一错误等级）。数值是"一个关卡够用"的量级。
    static constexpr std::size_t shader_capacity = 32;
 /// 80% 水位（ceil(0.8 × 32) = 26）。
    static constexpr std::size_t shader_watermark =
        (shader_capacity * 4 + 4) / 5;
 // 句柄低 6 位放"槽位 + 1" ⇒ 容量不得超过 63（当前 32）。
    static_assert(shader_capacity <= 63,
                  "着色器容量超过 6 位槽位编码：句柄布局需要同步修改");

 /// 本帧的自定义着色器状态（发射时用；由 sorted 列表逐项带入）。
 /// 存的是**已解析的 GL program**（0 = 内建变体），不是句柄。
    gl_api::uint_t pending_shader_ = 0;
    bool pending_params_set_ = false;
    float pending_params_[4] = {0.0f, 0.0f, 0.0f, 0.0f};

 /// 逻辑时间（秒）：`(frame_count - time_frame_offset) * dtg`。
 /// 【为什么是"帧号 × 步长"而不是累加】累加会漂移，而帧号是整数量。
 /// 【暂停】`frame_count` 只在正常帧推进 ⇒ 时间自动冻结（已定）。
    std::uint64_t logic_frame_count_ = 0;
    std::int64_t time_frame_offset_ = 0;
    double logic_dtg_ = 0.0;
 /// 渲染盒（整帧像素，x, y, w, h）与整帧尺寸（像素）——给着色器的 `u_box` /
 /// `u_resolution` 用。它们是**本帧常量**（begin_frame 时定下）。
    float frame_box_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float frame_pixel_[2] = {0.0f, 0.0f};

 // -------------------------------------------------------------------------
 // 帧绘制列表（收集侧）
 //
 // 【draw_item】= 一次绘制调用的推迟表示：排序键 + 顶点在帧顶点池里的
 // 位置 + 拓扑。发射时按拓扑生成索引（扇形 / 三角形顺序）。
 // 【不展开扇形】保住"8192 顶点 = 2048 个四边形"的档位算术
 // （展开会把四边形撑成 6 顶点，档位缩水到 1365）。
 // 【stage/来源】画布常量 screen、混合状态随各自批落地 ——
 // 比较器里先留位（注释标明），当前是常量。
 // -------------------------------------------------------------------------
    struct draw_item {
        std::uint8_t stage = 0;         ///< 渲染阶段（0=scene 1=ext；值域 = wag::render_stage）
        std::uint8_t source = 0;        ///< 来源（0=池/容器 1=脚本；当前全是脚本）
        std::uint8_t topology = 0;      ///< 0=扇形（draw_vertices）1=三角形列表（draw_mesh）
        std::int64_t layer = 0;         ///< 绘制顺序层
        double z = 0.0;                 ///< 层内顺序键（精确相等才可合并/同键）
 /// 本次绘制用的纹理（0 = 未定，发射时按白纹理）。
 /// 【为什么在 item 上】UV 归一化要除以纹理尺寸，而**纹理变了就必须 flush**
 /// （flush 触发②）。⇒ 它是批状态的一员，必须逐项记录。
        gl_api::uint_t tex = 0;
 /// 纹理尺寸的倒数（`1/w`, `1/h`）—— 发射时作为 `u_tex_inv_size` 上传。
        gl_api::float_t inv_size[2] = {1.0f, 1.0f};
        std::uint32_t vertex_begin = 0; ///< 帧顶点池中的起点
        std::uint32_t vertex_count = 0; ///< 原始顶点数（fan 未展开）
        std::uint32_t index_count = 0;  ///< 发射时的索引数
        std::uint64_t sequence = 0;     ///< 创建序（全局单调；稳定排序的锚）
 /// 本次绘制**生效的**混合状态（2026-10-18）。
 /// 【为什么在 item 上】它是**批状态**的一员：变了就必须 flush（触发②），
 /// 而且它是排序键里的一维（见 sort_and_emit_draws_ 的键序）。
 /// 【它从哪来】`draw_state_override` 已显式设置 ⇒ 用那个；
 /// 否则用该维默认（当前默认 = `Normal`；容器落地后默认变成容器字段）。
 /// ⇒ 这个"入批时计算生效值"就是 那条统一规则的第一处实现。
        blend_state blend = blend_state::normal;
 /// 本次绘制生效的**纹理 alpha 类型**（2026-10-18）。
 /// 【为什么不进排序键】：它不是"前序内容的函数" ⇒ 顺序无关
 /// ⇒ 只影响 flush 点（换它就要换程序），**不重排**。
        tex_alpha alpha = tex_alpha::normal;
 /// 本次绘制生效的**顶点色模式**（2026-10-18）。
 /// 【为什么不进排序键】：不是"前序内容的函数" ⇒ 顺序无关
 /// ⇒ 只影响 flush 点（换它就要换程序），**不重排**。
        vertex_color_mode vcolor = vertex_color_mode::mul;
 /// 本次绘制**已解析**的自定义着色器 program（0 = 用内建变体）。
 /// 【为什么存 program 而不是句柄】卸载会让句柄立刻失效，但本帧已收集的
 /// 绘制项必须仍能画完 ⇒ 句柄在收集时解析、program 活到帧末才删。
        gl_api::uint_t shader_program = 0;
 /// `u_params` 是否被显式设置过，以及它的 4 个值。
        bool params_set = false;
        float params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
 /// 本次绘制生效的**雾参数整组**（2026-10-18）。
 /// 【为什么整组放在 item 上，而不只放"启用位"】
 /// 雾的参数也是**批状态**：同一批里 near/far/颜色必须一致，
 /// 否则后设的 uniform 会把先收集的图元也改掉 ⇒ 必须能逐项比较、变化就 flush。
 /// （代价是每个 item 多 ~32 字节；容器落地后同批共享同一份，实际浪费很小。）
        fog_state fog;
    };
    std::vector<vertex> frame_vertices_;   ///< 帧顶点池（每帧清空，容量跨帧复用）
    std::vector<draw_item> draw_items_;    ///< 帧绘制列表（每帧清空）
    std::uint64_t creation_counter_ = 0;   ///< 全局单调创建序（跨帧不复位）
    std::uint8_t collect_stage_ = 0;       ///< 当前收集阶段（set_collect_stage 写入）
};

} // namespace wag::draw
