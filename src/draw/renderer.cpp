// =============================================================================
// wag/draw/renderer.cpp
//
// 渲染器实现：接收已经算好的变换，把绘制条目排序、分批提交到 GL。
//
// 【主要职责】
// · 取回 GL 3.3 入口并建立 GPU 资源；缺入口直接报错；
// · begin_frame：清屏、设视口/scissor、把窗口坐标盒换算成帧缓冲坐标；
// · 收集绘制项：draw_image / draw_vertices / draw_mesh / draw_glyphs；
// · 按 stage → layer → z → 来源 → 混合状态稳定排序后合批 flush；
// · 管理图片族（加载/卸载/帧表/锚点/快照来源）与自定义着色器；
// · 维护提交/flush/顶点/图元等统计，供诊断读取；
// · 快照：把 staging 画布拍成可再次加载的图片。
//
// 【批次为什么是“成员缓冲”】
// 一帧的绘制先收集到成员缓冲，再按批状态一次性上传/提交。
// 如果每次 draw_* 立即提交，状态切换和 GL 调用会淹没真正的内容，
// 而且无法稳定排序。
//
// 【坐标】
// 本文件消费 space 提供的 transform；不自己推导世界↔窗口映射。
// 顶点进入时是逻辑坐标，经变换变成窗口坐标，再由 framebuffer_box_for
// 翻成 GL 帧缓冲坐标。两次 y 翻转都必需。
// =============================================================================


#include "draw/renderer.hpp"

#include "draw/gl_api.hpp"
#include "error/error.hpp"
#include "log/log.hpp"

#if defined(WAG_HAVE_SDL3)
// 【这一支能不能编进来，取决于 CMake：wag_draw 必须链 wag_sdl3】
// WAG_HAVE_SDL3 由 wag_sdl3 这个 INTERFACE 目标携带（见 cmake/deps.cmake）。
// 若本模块漏链它，下面整段会被**预处理器删掉**，而症状是：
// · **没有编译警告**（#else 分支把参数 (void) 掉了）
// · 运行时**清屏静默失效** —— 窗口全黑，而阶段 7/12 的日志照打
// ⇒ 遇到"全黑"先查这里，不要往运行期查。
#include <SDL3/SDL_opengl.h>
// 【为什么还要这两个】`load_image_from_file` 用 SDL3 **核心自带**的图片加载器：
// `SDL_LoadPNG` / `SDL_LoadBMP`（SDL 3.4.0 起提供；本机 3.4.18）
// + `SDL_Surface` / `SDL_ConvertSurface` / `SDL_DestroySurface`（SDL_surface.h）
// + `SDL_GetError`（SDL_error.h）。
// ⇒ 零新依赖：这是 SDL3 核心，不是 SDL_image。
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_iostream.h> // SDL_LoadFile（外置 GLSL 探针用）
#include <SDL3/SDL_timer.h>   // SDL_GetTicks（效果探针的 u_time，仅探针）
#endif

#include <cstdio>

// 【为什么需要它】本文件里有两条**只调试构建生效**的断言（见 begin_frame 里
// "可断言的不变量"与"夹取之前先断言"两处）。它们的作用不是"检查参数合法"，
// 而是让**"内容偏一点"这类静默缺陷变成一次响亮的失败**。
#include <cassert>

#include <algorithm> // std::stable_sort（发射前的稳定排序）
#include <chrono>    // 资源加载耗时（只测量/记录，不改加载语义）

#include <cstddef>
#include <cstring>
#include <limits>   // std::numeric_limits（冻结画面的 layer 取最小值）
#include <utility>  // std::swap（staged → frozen 是一次交换）
#include <cmath>
#include <string>

namespace wag::draw {

namespace {

// -----------------------------------------------------------------------------
// 两个清屏色
//
// 【为什么收成常量】信箱色现在有**两个**消费者（begin_frame 的第一段清屏、
// clear_framebuffer 的暴露重画）—— 两个消费者共享同一个值就必须有
// **同一个来源**，否则迟早漂移（那正是本项目反复消掉的"两个真相"）。
// -----------------------------------------------------------------------------
constexpr float kMailboxColor[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // 信箱区：黑色
constexpr float kSceneColor[4] = {0.0f, 0.0f, 1.0f, 1.0f};   // 场景底色：纯蓝（便于肉眼判断生效）

/// `u_tex_inv_size` 是否应当在该变体里活跃。
///
/// 【为什么需要】Alphablend + Replace 的片元既不用纹理 rgb，也不读纹理 a，
/// 于是 `texture()/v_uv` 整条链都可能被驱动删掉，顶点侧的
/// `u_tex_inv_size` 也随之消失。NVIDIA/Intel 会做这一步，AMD 可能不删。
/// ⇒ 这个 uniform 的“存在”是**变体语义**，不是所有变体都必须有。
[[nodiscard]] bool variant_needs_tex_inv_size(vertex_color_mode m,
                                              tex_alpha t) noexcept {
    return !(m == vertex_color_mode::alphablend && t == tex_alpha::replace);
}

// -----------------------------------------------------------------------------
// fail_z_not_finite —— `z` 不是有限实数 ⇒ **L3**（2026-10-18 已定）
//
// 【为什么必须拦，而不是"画不出来就算了"】z 是**绘序键**，它进 `std::stable_sort`
// 的比较函数：`if (a.z != b.z) return a.z < b.z;`
// NaN 时 `a.z != b.z` 为**真**，而两个方向的 `<` 都为**假**
// ⇒ 比较函数**不满足严格弱序** ⇒ `std::stable_sort` 的行为是**未定义**
// （可能乱序、也可能越界访问 —— 那是 UB，不是"画错")。
// ⇒ 所以它不是"看起来没用的输入"，是**会让引擎自身逻辑失效的数据矛盾**。
//
// 【为什么是 L3、且在渲染层查】与顶点校验同一条：
// · 渲染层是**唯一入口**（draw_vertices / draw_mesh 都过这里）⇒ 一份校验
// · L3 = 终止。理由：绘序键坏了，本帧的**整个绘制顺序**都不可信，
// 而"带错继续画"会把问题伪装成"图叠错了"（比终止难查得多）。
// ⚠️ 它抓的是**参数**；顶点里的 z 仍由 `find_vertex_defect` 查（两处都要，
// 因为它们进的是不同的东西：参数进排序键，顶点 z 现在只是透传）。
// -----------------------------------------------------------------------------
[[noreturn]] void fail_z_not_finite(const char* where, double z) {
    error_value e;
    e.cls = error_class::boundary_validation;
    e.where = where;
    e.got = std::isnan(z) ? "nan" : ((z > 0.0) ? "+inf" : "-inf");
    e.want = "有限实数（z 是绘序键：NaN / 无穷会让排序失去严格弱序 ⇒ 未定义行为）";
    fail_boundary(e);
}

// -----------------------------------------------------------------------------
// 资源加载耗时（· 只记录，不改语义）
//
// 【L1/L2】每次成功加载记 L1 耗时；单次 >= 33 ms（约两帧）升级为 L2。
// 不做分帧/异步加载 —— 那是架构级改动，另行立项。
// -----------------------------------------------------------------------------
constexpr double kLoadSlowMs = 33.0;

void log_load_time_ms(const char* kind, const std::string& name, double ms) {
    char ms_buf[64];
    std::snprintf(ms_buf, sizeof(ms_buf), "%.2f ms", ms);
    const std::string detail = name + " | " + ms_buf;
    if (ms >= kLoadSlowMs) {
        log::warn("draw", log::format(std::string(kind) + "加载耗时偏高（L2）",
                                detail));
    } else {
        log::record("draw", log::format(std::string(kind) + "加载耗时", detail));
    }
}

/// 快照路径的 GL 失败：L3（快照是暂停/冻结的必需件，不能静默黑屏）。
[[noreturn]] void fail_snapshot_gl(const char* where, gl_api::enum_t err) {
    char got_buf[64];
    std::snprintf(got_buf, sizeof(got_buf), "GL error 0x%X",
                  static_cast<unsigned>(err));
    error_value e;
    e.cls = error_class::resource_failure;
    e.where = where;
    e.got = got_buf;
    e.want = "GL 操作成功（上下文/显存正常）";
    e.sentence = "快照捕获失败";
    fail_boundary(e);
}

// -----------------------------------------------------------------------------
// clamp_uv_or_defect —— UV 越界的**唯一处理点**（draw_vertices 与 draw_mesh 共用）
//
// 【为什么容差内"夹"、超出容差"报错"】见 renderer.hpp 里 `uv_overshoot_tolerance`
// 那段：浮点残差不是数据矛盾（夹掉）；而"超过纹理尺寸"意味着这笔 UV 没意义
// （报错）。**采样器已经是 CLAMP_TO_EDGE（防线）**，所以即使漏检也不会
// 绕回图集别处 —— 这里的报错纯粹是**诊断**。
// 【为什么放在文件前部】draw_mesh 在 SDL3 段里、draw_vertices 在段外，
// 两者都要用它 ⇒ 它必须比两者都早。
// -----------------------------------------------------------------------------
bool clamp_uv_or_defect(vertex& vtx, double tex_w, double tex_h,
                        char& bad_field, double& bad_value) noexcept {
    const double values[2] = {static_cast<double>(vtx.u),
                              static_cast<double>(vtx.v)};
    const double limits[2] = {tex_w, tex_h};
    const char names[2] = {'u', 'v'};
    const double tol = uv_overshoot_tolerance;

    for (int k = 0; k < 2; ++k) {
        const double value = values[k];
        const double limit = limits[k];
        if (value < -tol || value > limit + tol) {
            bad_field = names[k];
            bad_value = value;
            return true;
        }
 // 容差内（含"恰好等于 limit"）⇒ 夹到 [0, limit]
        const double clamped =
            (value < 0.0) ? 0.0 : ((value > limit) ? limit : value);
        if (k == 0) {
            vtx.u = static_cast<float>(clamped);
        } else {
            vtx.v = static_cast<float>(clamped);
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// 第一档着色器（2026-10-15，子目标 1）
//
// 【第一档着色器：顶点色 Mul × 纹理 alpha × 雾】
// 变体总数 16 = 4 × 2 × 2；本档是文字将来永久占用的那一格。
// （13.2 ⑨ 建议把"文字 = Mul 顶点色 + Alpha
// 混合 + 无雾"写成硬规则，实测 aex THlib 字体状态调用全部走默认 MulAlpha）
// ⇒ 第一批先写它，文字落地时零新增变体。两层背书，不是"最常用"三个字。
//
// 【顶点在 GPU 侧收到的是什么域】
// 位置是**窗口坐标像素**（②域：左上原点、+y 向下）—— ①→② 已在 CPU
// 侧由 transform_ 完成（渲染决策二 2.1：转换点唯一，float 只在此后出现）。
// ② → NDC 由 uniform 的 3×3 矩阵完成（见 renderer.hpp 里
// pixel_to_ndc_ 的说明：矩阵由 box 推导，与 glViewport / glScissor 同源）。
//
// 【GLSL 源码刻意不写注释】驱动对非 ASCII 的源码注释没有保证，
// 所以关于它的一切说明都放在这外面的 C++ 注释里。
// -----------------------------------------------------------------------------
constexpr char kVertexShaderSource[] = R"GLSL(#version 330 core
in vec2 a_pos;
in vec2 a_uv;
in vec4 a_color;
in float a_z;
uniform mat3 u_pixel_to_ndc;
uniform vec2 u_tex_inv_size;
out vec2 v_uv;
out vec4 v_color;
out float v_dist;
void main() {
    vec3 p = u_pixel_to_ndc * vec3(a_pos, 1.0);
    gl_Position = vec4(p.xy, 0.0, 1.0);
    v_uv = a_uv * u_tex_inv_size;
    v_color = a_color;
    v_dist = a_z;
}
)GLSL";

// -----------------------------------------------------------------------------
// ⚠️【UV 的单位链 —— 只有一处转换，就在顶点着色器里】
//
// 顶点约定：`u, v` 是 **像素单位、原点左上**。
// GLSL：`texture()` 吃 **[0,1] 归一化**坐标。
// ⇒ 中间**必须**有一次除以纹理尺寸。它发生在哪里、为什么不放别处：
// · **不放脚本侧**：那会让作者必须知道每张图的尺寸，而"像素 UV"这条契约
// 存在的意义**正是**让作者只按图集想问题
// · **不放 CPU 侧**：每顶点一次运算不如 uniform 免费；而且会把顶点池里的
// **原始像素 UV 永久丢掉**（调试要看它，将来 texelFetch 也要它）
// · **放顶点着色器**（乘法）：除以常数是仿射 ⇒ "除后插值" 与 "插值后除"
// 结果相同，而顶点数远少于像素数
// ⇒ 于是 `u_tex_inv_size` 成了**批状态的一员**：纹理变了就必须 flush（触发②）。
//
// ⚠️【采样器必须是 CLAMP_TO_EDGE】`GL_TEXTURE_WRAP_S/T` 的缺省是 **GL_REPEAT**
// ⇒ "什么都不设"等于**取模**，而在**图集**上取模会绕回去采到别的精灵
// （静默错图）。所以四处纹理创建都显式设 clamp —— 它是防线，
// 而"UV 越界报错"（见 uv_overshoot_tolerance）是诊断。两者分工不同。
// -----------------------------------------------------------------------------

constexpr char kFragmentShaderHeader[] = R"GLSL(#version 330 core
in vec2 v_uv;
in vec4 v_color;
in float v_dist;
uniform sampler2D u_texture;
uniform vec3 u_fog_color;
uniform vec2 u_fog_range;
uniform float u_fog_strength;
out vec4 frag_color;
)GLSL";

// 【片元主体：顶点色模式 × 纹理 alpha × 雾 三维靠宏区分】
// 三个宏由 init_gl_resources_ 在编译前插入（：进着色器、启动时预编译）：
// · `VCOL_MODE` 0=Mul / 1=Add / 2=Sub / 3=Alphablend
// · `TEX_ALPHA_REPLACE` 0=Normal（a=a_tex·a_vtx）/ 1=Replace（a=a_vtx）
// · `FOG_ON` 0=无雾 / 1=线性雾（雾只贡献 1 位变体）
// ⚠️ 三维**互不干扰**：VCOL 只改 rgb、TEX_ALPHA 只改 a、雾只改 rgb（不动 a）
// ⇒ 4×2×2 的笛卡尔积成立。
// ⚠️ `Sub` 要 `max(0,·)` 夹紧：颜色是 [0,1]；让 GL 去夹是"依赖驱动行为"，
// 不是契约（RGBA8 帧缓冲一般会夹，但不该被依赖）。
// ⚠️【雾的次序】：①采样 → ②顶点色模式 → ③雾 → ④固定功能混合。
// 所以雾在 rgb 算完之后、写 frag_color 之前 —— 它作用于"已着色"的颜色。
// ⚠️【雾不改 alpha】①：改 alpha 会让"雾"变成"改可见性"，那是另一维的事。
// ⚠️【允许 near > far】②：分母可以为负 ⇒ 公式自然给出反向雾，**不特判**。
constexpr char kFragmentShaderBody[] = R"GLSL(
void main() {
    vec4 t = texture(u_texture, v_uv);
    vec3 vc = v_color.bgr;                  // 顶点色（已按字节序换回 R,G,B）
    float a = (TEX_ALPHA_REPLACE != 0) ? v_color.a : (t.a * v_color.a);
#if VCOL_MODE == 0
    vec3 rgb = t.rgb * vc;                  // Mul（默认）
#elif VCOL_MODE == 1
    vec3 rgb = t.rgb + vc;                  // Add
#elif VCOL_MODE == 2
    vec3 rgb = max(vec3(0.0), t.rgb - vc);  // Sub（夹紧 0）
#else
    vec3 rgb = vc;                          // Alphablend（纹理只出 alpha）
#endif
#if FOG_ON
 // 线性雾：f = clamp((dist − near) / (far − near), 0, 1) × strength
    float f = clamp((v_dist - u_fog_range.x) /
                        (u_fog_range.y - u_fog_range.x), 0.0, 1.0) *
              u_fog_strength;
    rgb = mix(rgb, u_fog_color, f);
#endif
    frag_color = vec4(rgb, a);
}
)GLSL";

// -----------------------------------------------------------------------------
// ⚠️【为什么片元着色器里是 v_color.bgr —— 颜色的字节序已定（子目标 2，2026-10-15）】
//
// vertex::color 是 **ARGB 的 u32**（0xAARRGGBB，照抄 LuaSTG/aex 的颜色惯例；
// Lua 侧以位运算构造：A<<24 | R<<16 | G<<8 | B）。
//
// 而颜色作为顶点属性喂给 GL 的路径是：
// u32 --(小端内存序)--> 字节序列 [BB, GG, RR, AA]
// --(GL_UNSIGNED_BYTE ×4, normalized)--> attribute 四个分量
// 分量0 = B，分量1 = G，分量2 = R，分量3 = A
//
// GLSL 的 swizzle 名字是**按位置**的（.r = 分量0），于是：
// v_color.r 实际是蓝；v_color.b 实际是红
// v_color.bgr = (分量2, 分量1, 分量0) = (R, G, B) ← 正确的 RGB 序
// v_color.a = (分量3) = A ← alpha 恰好不受影响
//
// ⇒ **决策：字节序在着色器侧修正（.bgr），不在 CPU 侧改写字节。**
// 理由：CPU 侧改写 = 每顶点一次字节重排（弹幕量级下是纯浪费），
// 而 swizzle 是寄存器重命名，零成本。
//
// ⚠️【两条边界，改代码前先读】
// ① 本推导依赖**小端**。大端平台上字节序会反过来 ⇒ 届时改这里
// （当前平台 x86/ARM64 Windows 全是小端；跨大端属于假想需求）。
// ② "Lua 侧颜色是 ARGB" 这个约定将来若有变化（例如增强库提供
// RGBA 构造），必须与这里的 swizzle 一起改 —— 它们是同一个契约的两半。
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// 着色器编译辅助
//
// 【失败即 L3 终止】编译/链接失败 = 这台环境跑不了本引擎 =
// 不可降级的资源失败。绝不静默（黑屏不报错是这类失败最坏的形态）。
// 【信息里必须带 info log】GL 的编译错误只有它有（带行号）。
// 【按档守卫】它们唯一的调用者是 init_gl_resources_（SDL3 档）——
// 无依赖档下没人用 ⇒ 不守卫会有 -Wunused-function 警告。
// -----------------------------------------------------------------------------
#if defined(WAG_HAVE_SDL3)

/// 编译单个着色器阶段。失败 ⇒ L3 终止（log::terminate 不返回）。
gl_api::uint_t compile_stage(const gl_api::enum_t type,
                             const char* source,
                             const char* stage_name) {
    const gl_api::uint_t sh = gl_api::create_shader(type);
 // length 传 nullptr = 按 NUL 结尾读（GL 规范明写的行为）。
    gl_api::shader_source(sh, 1, &source, nullptr);
    gl_api::compile_shader(sh);

    gl_api::int_t ok = 0;
    gl_api::get_shader_iv(sh, gl_api::compile_status, &ok);
    if (ok == 0) {
        char info[1024];
        gl_api::get_shader_info_log(sh, sizeof(info), nullptr, info);
        info[sizeof(info) - 1] = '\0';
        log::terminate("draw", log::format("着色器编译失败",
                                   std::string(stage_name) + " | " + info));
    }
    return sh;
}

/// 链接程序。失败 ⇒ L3 终止。
///
/// @param bind_standard_attribs
/// true = 链接前把 a_pos/a_uv/a_color/a_z 固定到 0/1/2/3。
/// 16 个管线变体必须传 true：它们共用一个 VAO（的批状态变体），
/// 而 GL 不保证不同 program 会把同名属性分到同一 location。
/// 自定义/探测着色器传 false，保留它们自己的接口。
gl_api::uint_t link_stage(const gl_api::uint_t vs, const gl_api::uint_t fs,
                          bool bind_standard_attribs = false) {
    const gl_api::uint_t prog = gl_api::create_program();
    gl_api::attach_shader(prog, vs);
    gl_api::attach_shader(prog, fs);
    if (bind_standard_attribs) {
        gl_api::bind_attrib_location(prog, 0, "a_pos");
        gl_api::bind_attrib_location(prog, 1, "a_uv");
        gl_api::bind_attrib_location(prog, 2, "a_color");
        gl_api::bind_attrib_location(prog, 3, "a_z");
    }
    gl_api::link_program(prog);

    gl_api::int_t ok = 0;
    gl_api::get_program_iv(prog, gl_api::link_status, &ok);
    if (ok == 0) {
        char info[1024];
        gl_api::get_program_info_log(prog, sizeof(info), nullptr, info);
        info[sizeof(info) - 1] = '\0';
        log::terminate("draw", log::format("着色器程序链接失败", info));
    }
    return prog;
}

#endif // WAG_HAVE_SDL3（着色器编译辅助）

} // namespace

// 【命名空间】原先这里有 `namespace core = wag::core;` 别名。`wag::core` 已拆除
// （见 log/log.hpp 文件头），`log::` 与 `wag::` 的成员在 wag::draw 内直接可见。

// =============================================================================
// 混合状态：名字表 + 因子表（渲染与着色器-2026 权威表）
//
// 【为什么名字与因子必须放在同一处】权威表就是这两列。
// 若名字表在 Lua 侧、因子表在 C++ 侧 ⇒ 两处迟早不同步，
// 而症状是"名字对了、因子错了"（画面偏色，最难查）。
// ⇒ 所以这里一张表同时给"协议名"和"三件 GL 参数"，另加一个下标检查。
// =============================================================================

namespace {

// 【为什么把"名字"与"因子"拆成两张表】名字表在**两档构建里都要有**
// （Lua 侧要能拿名字、也能把名字解析回来），而因子表用的 `GL_*` 常量
// 只在 SDL3 档有头文件 ⇒ 合成一张表会让无依赖档编不过。
// ⚠️ 两张表**下标必须同序**（都由 blend_state 的枚举值索引）——
// 这一点靠"同一个 blend_state_count"与下面的 static_assert 兜住。
constexpr const char* kBlendNames[blend_state_count] = {
    "Normal", "Add", "AddNoAlpha", "Sub",
    "SubNoAlpha", "RevSub", "MulAlpha", "MulColor",
};

#if defined(WAG_HAVE_SDL3)
struct blend_state_info_ {
    gl_api::enum_t src_rgb;    ///< RGB 的 srcFactor
    gl_api::enum_t dst_rgb;    ///< RGB 的 dstFactor
    gl_api::enum_t equation;   ///< RGB 的 blendEquation
};

// 【下标必须与 blend_state 的枚举值一一对应】靠上面的 static_assert 钉住个数，
// 靠 apply 用 `static_cast<size_t>` 直接索引 —— 不写 switch，
// 免得以后加状态时漏一个分支（漏了就是"静默用上一个状态"）。
constexpr blend_state_info_ kBlendStates[blend_state_count] = {
    {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, gl_api::func_add},             // Normal
    {GL_SRC_ALPHA, GL_ONE,                 gl_api::func_add},             // Add
    {GL_ONE,       GL_ONE,                 gl_api::func_add},             // AddNoAlpha
    {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, gl_api::func_reverse_subtract},// Sub
    {GL_ONE,       GL_ONE,                 gl_api::func_reverse_subtract},// SubNoAlpha
    {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, gl_api::func_subtract},        // RevSub
    {GL_ZERO,      GL_SRC_COLOR,           gl_api::func_add},             // MulAlpha
    {GL_DST_COLOR, GL_ZERO,                gl_api::func_add},             // MulColor
};
#endif // WAG_HAVE_SDL3

} // namespace

const char* blend_state_name(blend_state s) noexcept {
    const std::size_t i = static_cast<std::size_t>(s);
    if (i >= blend_state_count) {
        return "?";
    }
    return kBlendNames[i];
}

bool parse_blend_state(const char* name, blend_state& out) noexcept {
    if (name == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < blend_state_count; ++i) {
        if (std::strcmp(kBlendNames[i], name) == 0) {
            out = static_cast<blend_state>(i);
            return true;
        }
    }
    return false;
}

// =============================================================================
// 纹理 alpha 类型的名字（：`Normal` 默认 / `Replace`）
// =============================================================================
namespace {
constexpr const char* kTexAlphaNames[tex_alpha_count] = {
    "Normal",
    "Replace",
};
} // namespace

const char* tex_alpha_name(tex_alpha t) noexcept {
    const std::size_t i = static_cast<std::size_t>(t);
    if (i >= tex_alpha_count) {
        return "?";
    }
    return kTexAlphaNames[i];
}

bool parse_tex_alpha(const char* name, tex_alpha& out) noexcept {
    if (name == nullptr) {
        return false;
    }
 // （parse_tex_alpha 的查找循环）
    for (std::size_t i = 0; i < tex_alpha_count; ++i) {
        if (std::strcmp(kTexAlphaNames[i], name) == 0) {
            out = static_cast<tex_alpha>(i);
            return true;
        }
    }
    return false;
}

// =============================================================================
// 顶点色模式的名字（：Mul 默认 / Add / Sub / Alphablend）
//
// 【为什么名字表与 tex_alpha 分开】两维都会进 set_state，但它们的取值域不同、
// 报错时也要分别列出合法值 ⇒ 各一张表，不合并成一个"大枚举"
// （合并会让"这一维还有几档"变得不可读）。
// =============================================================================
namespace {
constexpr const char* kVertexColorNames[vertex_color_count] = {
    "Mul",
    "Add",
    "Sub",
    "Alphablend",
};
} // namespace

const char* vertex_color_name(vertex_color_mode m) noexcept {
    const std::size_t i = static_cast<std::size_t>(m);
    if (i >= vertex_color_count) {
        return "?";
    }
    return kVertexColorNames[i];
}

bool parse_vertex_color(const char* name, vertex_color_mode& out) noexcept {
    if (name == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < vertex_color_count; ++i) {
        if (std::strcmp(kVertexColorNames[i], name) == 0) {
            out = static_cast<vertex_color_mode>(i);
            return true;
        }
    }
    return false;
}

// =============================================================================
// 混合状态：真正设进 GL 的**唯一一处**
//
// 【为什么只有这一处】名字/因子表在文件上部（kBlendNames / kBlendStates），
// 而"把某一档送进 GL"只发生在这里 ⇒ 想加状态只需改表，不会漏改调用点。
// 【为什么 alpha 通道不跟着表走】表里只定义了 RGB 的因子对；
// 让 alpha 也走 Sub/RevSub 会把帧缓冲 alpha 弄成负值或清零，
// 而画布/快照/将来的后处理要用它 ⇒ alpha 固定 (ONE, ONE_MINUS_SRC_ALPHA) + ADD。
// 【为什么每次 flush 都重设】不做"变了才设"的缓存：GL 状态显式重设是几纳秒的事，
// 而"缓存失效时忘了重设"是静默错（整帧用错混合）。
// 与"每张纹理都显式设 clamp"是同一条理由。
// =============================================================================
#if defined(WAG_HAVE_SDL3)
namespace {

void apply_blend_state_(blend_state s) noexcept {
    const std::size_t i = static_cast<std::size_t>(s);
    if (i >= blend_state_count) {
        return; // 不可达（枚举值域由 static_assert 钉住）；防御性返回
    }
    const blend_state_info_& b = kBlendStates[i];
    gl_api::blend_func_separate(b.src_rgb, b.dst_rgb, GL_ONE,
                               GL_ONE_MINUS_SRC_ALPHA);
    gl_api::blend_equation_separate(b.equation, gl_api::func_add);
}

} // namespace
#endif // WAG_HAVE_SDL3

renderer::~renderer() {
 // 【析构不销毁上下文】
 // 上下文的所有权虽然在本对象，但它的销毁必须发生在**窗口销毁之前**，
 // 而那个顺序属于关闭序列（见 wag::shutdown_sequence 的第 5 步）。
 // 在析构里销毁它会让顺序依赖 C++ 的析构次序，而那是不保证的。
 //
 // 【挂起的纹理也不必在这里删】它们随 GL 上下文一起消失；
 // 而本 vector 随对象一起析构 ⇒ 不泄漏。
}

// =============================================================================
// 上下文的接收
// =============================================================================

bool renderer::adopt_context(platform::gl_context ctx) {
 // 【重复接收不是错误，但也不做第二次初始化】
 // GL 入口表与 GPU 资源都挂在同一个上下文上 ⇒ 再来一次没有意义。
    if (ctx_.is_valid()) {
        log::warn("draw", "渲染器已经持有上下文，重复的 adopt_context() 被忽略");
        return false;
    }
    if (!ctx.is_valid()) {
        log::warn("draw", "adopt_context() 收到一个空的上下文句柄");
        return false;
    }

    ctx_ = ctx;
#if defined(WAG_HAVE_SDL3)
 // 【为什么顺序不能反】入口表没取回之前，任何 gl* 调用都是空指针
 // ⇒ 必须先 load() 再 init_gl_resources_()。
    gl_api::load();
    init_gl_resources_();
#endif
    log::record("draw", "渲染器已接收图形上下文");
    return true;
}

// =============================================================================
// 帧开始：两段清屏 + 视口/scissor + ②→NDC 矩阵
// =============================================================================

void renderer::reset_for_reload() noexcept {
 //：旧 Lua 已经不能再发绘制；把上一代留下的收集队列与粘性状态丢掉。
 // 不做 GPU 操作；下一帧 begin_frame 会重新设 viewport/矩阵。
    draw_items_.clear();
    frame_vertices_.clear();
    batch_vertex_count_ = 0;
    batch_index_count_ = 0;
    state_ = draw_state_override{};
    light_clear();
}

void renderer::begin_frame(const space::transform& transform,
                           const space::rect& viewport,
                           const platform::window::frame_geometry& geom) {
 // 【本帧常量，只在这里写一次】顶点变换在收集时用，视口与矩阵在这里用。
    transform_ = transform;

 // 【每帧光照状态清零】带/计数/超限标记都是"本帧"的。
    bands_.clear();
    frame_commits_ = 0;
    frame_lights_max_ = 0;
    frame_fill_px_ = 0;
    new_over_lights_ = false;
    new_over_fill_ = false;
    new_over_commits_ = false;
    band_mask_active_ = false;

 // 【display_scale_ 只作诊断】渲染几何一次都不读它（见文件头部第 91 行那段）。
 // 它报的是 Windows 的 UI 缩放 —— 用它做任何换算都是错的。
    display_scale_ = geom.display_scale;
#if defined(WAG_HAVE_SDL3)
 // 【可断言的不变量】本平台上窗口尺寸与绘制表面像素必须相等
 // （尺寸 API 本来就给物理像素，实测过的）。若不等，说明有处自己用
 // window_size × display_scale 推导了 —— 那正是坐标系链条里最贵的一次事故。
    assert(geom.window.w == geom.pixel.w && geom.window.h == geom.pixel.h &&
           "窗口尺寸与绘制表面像素不等 —— 本平台不该发生；"
           "请检查是否有处自己用 window_size × display_scale 推导（见坐标系链条 6.2/清单第 5 条）");

    if (!ctx_.is_valid()) {
        return;
    }

 // 【帧缓冲尺寸问平台要，不自己推】见坐标系链条：像素是平台报的量。
    int fb_w = static_cast<int>(geom.pixel.w + 0.5);
    int fb_h = static_cast<int>(geom.pixel.h + 0.5);
    if (fb_w <= 0 || fb_h <= 0) {
 // 【退化帧】窗口被压到 0 像素（最小化/尺寸为 0）时，平台报的像素尺寸可能是 0。
 // 此时退回窗口尺寸只是"给一个能算的除数"，真正的防线是下面的 vw>0&&vh>0：
 // 矩阵不更新 ⇒ 沿用上一帧的好值，而不是写进一个除零产物。
        fb_w = static_cast<int>(geom.window.w + 0.5);
        fb_h = static_cast<int>(geom.window.h + 0.5);
    }

 // 【记住帧缓冲尺寸】快照画布要用它（阶段 10）。
    fb_w_ = fb_w;
    fb_h_ = fb_h;

 // 【内部场景目标】0.2.0 起永远先画到这里，再由 present_frame 送到输出。
 // 目标尺寸由 render_size 决定；默认 1920×1080。
    output_w_ = fb_w;
    output_h_ = fb_h;
    const int target_w = (scene_w_ > 0) ? scene_w_ : fb_w;
    const int target_h = (scene_h_ > 0) ? scene_h_ : fb_h;
    ensure_render_target_(scene_target_, target_w, target_h, false);
    bind_render_target_(scene_target_);
    fb_w_ = scene_target_.w;
    fb_h_ = scene_target_.h;

 // ---- 第一段清屏：整屏信箱色 ----
 // 【为什么先关 scissor】上一帧把它开着（限制场景绘制范围）⇒ 不关就清不到整屏。
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(kMailboxColor[0], kMailboxColor[1], kMailboxColor[2],
                 kMailboxColor[3]);
    glClear(GL_COLOR_BUFFER_BIT);

 // ---- 视口盒：逻辑 render 矩形 → 帧缓冲坐标（那一次 y 翻转就在这里）----
 // 【唯一来源】`space::framebuffer_box_for` —— 与 glViewport/glScissor/矩阵
 // 三个消费者共用同一个产物，所以没有第二个来源可漂移。
    const space::rect win_render = viewport;
    const space::framebuffer_box box =
        space::framebuffer_box_for(win_render, static_cast<double>(fb_w),
                                   static_cast<double>(fb_h));
    int vx = box.x;
    int vy = box.y;
    int vw = box.w;
    int vh = box.h;
    {
 // 【夹取之前先断言】"内容偏一点"这类静默缺陷要变成一次响亮的失败；
 // 而 slack = 2 像素是给取整留的余量（不是给算错的余量）。
 // Release 下 assert 被去掉，slack 也必须一起消失，否则会留下未使用变量警告。
#if !defined(NDEBUG)
        const int slack = 2;
        assert(vx >= -slack && vy >= -slack && vx + vw <= fb_w + slack &&
               vy + vh <= fb_h + slack &&
               "视口矩形越出帧缓冲过多 —— 像是被某个推导算错了"
               "（典型：拿 window_size × display_scale）。请查坐标系链条第 5 条。");
#endif
    }

 // 【夹到帧缓冲范围】letterbox 在极端窗口比例下可能让 box 略微越界；
 // 越界的 viewport 在 GL 里是未定义用法 ⇒ 先夹。
    if (vx < 0) { vw += vx; vx = 0; }
    if (vy < 0) { vh += vy; vy = 0; }
    if (vx + vw > fb_w) { vw = fb_w - vx; }
    if (vy + vh > fb_h) { vh = fb_h - vy; }
    if (vw < 0) { vw = 0; }
    if (vh < 0) { vh = 0; }

    glViewport(vx, vy, vw, vh);

 // 【整帧尺寸与渲染盒】给着色器的 `u_resolution` / `u_box` 用。
 // 它们是本帧常量：本帧只在这里写一次（与 pixel_to_ndc_ 同一时机）。
    frame_pixel_[0] = static_cast<float>(fb_w);
    frame_pixel_[1] = static_cast<float>(fb_h);
    frame_box_[0] = static_cast<float>(vx);
    frame_box_[1] = static_cast<float>(vy);
    frame_box_[2] = static_cast<float>(vw);
    frame_box_[3] = static_cast<float>(vh);

 // ---- 第二段清屏：只清视口盒（场景底色）----
 // 【清的是 scissor，不是 viewport】这一点是实测定的：
 // glClear 受 scissor 限制，viewport 只影响顶点映射 ⇒ 所以两者都要设。
    glEnable(GL_SCISSOR_TEST);
    glScissor(box.x, box.y, box.w, box.h);

    glClearColor(kSceneColor[0], kSceneColor[1], kSceneColor[2], kSceneColor[3]);
    glClear(GL_COLOR_BUFFER_BIT);

 // ---- ②→③ 的第二次翻转：像素 → NDC（以视口盒为参照）----
 // 【退化帧不写】vw/vh 为 0 时除零会产出 inf/NaN，而矩阵一旦被污染，
 // 之后所有顶点都不可信 ⇒ 宁可沿用上一帧的好值。
    if (vw > 0 && vh > 0) {
        const double a = 2.0 / static_cast<double>(vw);
        const double c = -2.0 * static_cast<double>(vx) / static_cast<double>(vw) - 1.0;
        const double d = -2.0 / static_cast<double>(vh);
        const double f =
            2.0 * (static_cast<double>(fb_h) - static_cast<double>(vy)) /
                static_cast<double>(vh) -
            1.0;
        pixel_to_ndc_[0] = static_cast<gl_api::float_t>(a);
        pixel_to_ndc_[4] = static_cast<gl_api::float_t>(d);
        pixel_to_ndc_[6] = static_cast<gl_api::float_t>(c);
        pixel_to_ndc_[7] = static_cast<gl_api::float_t>(f);
 // 其余分量恒为 0（列主序 3×3，只有 x/y 两个输出维度参与）
    }
#else
    (void)viewport;
    (void)geom;
#endif

#if defined(WAG_HAVE_SDL3)
    debug_rt_selftest_();
    debug_instance_selftest_();
    debug_mask_selftest_();
    debug_light_selftest_();
    debug_band_selftest_();
    debug_selfonly_selftest_();
    debug_budget_selftest_();
#endif
}

// =============================================================================
// 帧结束：排序 + 发射 + 帧末 flush（此后才呈现）
// =============================================================================

void renderer::end_frame() {
 // 【职责边界】①排序 + 发射（绘序统一管线）②帧末 flush（触发③）
 // ③帧末销毁挂起的纹理（"卸载在帧末生效"）
 // "全屏回画 + 效果"与"绑回屏幕"随屏幕与画布那批落地。
 //
 // 【它不呈现】呈现是阶段 12 的事，属于窗口（platform::window::present）。
 // 把换缓冲放在这里会让"谁负责呈现"有两个答案。
#if defined(WAG_HAVE_SDL3)
    sort_and_emit_draws_();
    flush_batch_();
    render_light_pass_(); // 未 commit 的光：帧末按默认单带走一次
 // 【探针的调用点已移到阶段 12（快照之后）】见 frame.cpp 的 stage_present
#endif
 // 【必须在 flush 之后】本帧已收集的绘制项此刻都已经绑过、画完了，删除才安全。
    flush_pending_destroy_tex_();
    flush_retired_shaders_();
}

#if defined(WAG_HAVE_SDL3)
namespace {
gl_api::uint_t compile_present_shader(gl_api::enum_t type, const char* src,
                                     const char* label) {
    const gl_api::uint_t sh = gl_api::create_shader(type);
    if (sh == 0) {
        fail_internal(std::string(label) + "：gl_api::create_shader 返回 0");
    }
    gl_api::shader_source(sh, 1, &src, nullptr);
    gl_api::compile_shader(sh);
    GLint ok = 0;
    gl_api::get_shader_iv(sh, GL_COMPILE_STATUS, &ok);
    if (ok == GL_FALSE) {
        GLint len = 0;
        gl_api::get_shader_iv(sh, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<std::size_t>(len > 1 ? len : 1), '\0');
        gl_api::get_shader_info_log(sh, len, nullptr, log.data());
        gl_api::delete_shader(sh);
        fail_internal(std::string(label) + " 编译失败：" + log);
    }
    return sh;
}
} // namespace
#endif

void renderer::ensure_present_resources_() {
#if defined(WAG_HAVE_SDL3)
    if (present_ready_) {
        return;
    }
    const char* vs_src = R"(#version 330 core
layout(location = 0) in vec2 a_corner;
out vec2 v_uv;
void main() {
    v_uv = a_corner;
    gl_Position = vec4(a_corner * 2.0 - 1.0, 0.0, 1.0);
}
)";
    const char* fs_src = R"(#version 330 core
in vec2 v_uv;
uniform sampler2D u_tex;
out vec4 frag;
void main() {
    frag = texture(u_tex, v_uv);
}
)";
    const gl_api::uint_t vs = compile_present_shader(
        gl_api::vertex_shader, vs_src, "present vertex");
    const gl_api::uint_t fs = compile_present_shader(
        gl_api::fragment_shader, fs_src, "present fragment");
    present_prog_ = gl_api::create_program();
    if (present_prog_ == 0) {
        gl_api::delete_shader(vs);
        gl_api::delete_shader(fs);
        fail_internal("present：gl_api::create_program 返回 0");
    }
    gl_api::attach_shader(present_prog_, vs);
    gl_api::attach_shader(present_prog_, fs);
    gl_api::link_program(present_prog_);
    gl_api::delete_shader(vs);
    gl_api::delete_shader(fs);
    GLint ok = 0;
    gl_api::get_program_iv(present_prog_, GL_LINK_STATUS, &ok);
    if (ok == GL_FALSE) {
        GLint len = 0;
        gl_api::get_program_iv(present_prog_, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<std::size_t>(len > 1 ? len : 1), '\0');
        gl_api::get_program_info_log(present_prog_, len, nullptr, log.data());
        gl_api::delete_program(present_prog_);
        present_prog_ = 0;
        fail_internal("present 链接失败：" + log);
    }
    present_u_tex_ = gl_api::get_uniform_location(present_prog_, "u_tex");
    gl_api::gen_vertex_arrays(1, &present_vao_);
    gl_api::gen_buffers(1, &present_vbo_);
    gl_api::bind_vertex_array(present_vao_);
    gl_api::bind_buffer(GL_ARRAY_BUFFER, present_vbo_);
    const float quad[8] = {0.0f, 0.0f, 1.0f, 0.0f,
                           0.0f, 1.0f, 1.0f, 1.0f};
    gl_api::buffer_data(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(quad)), quad,
                 GL_STATIC_DRAW);
    gl_api::vertex_attrib_pointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    gl_api::enable_vertex_attrib_array(0);
    gl_api::bind_vertex_array(0);
    gl_api::bind_buffer(GL_ARRAY_BUFFER, 0);
    present_ready_ = true;
#endif
}

void renderer::set_scene_size(int w, int h) noexcept {
    if (w > 0 && h > 0) {
        scene_w_ = w;
        scene_h_ = h;
    }
}

void renderer::set_present_mode(present_mode mode) noexcept {
    present_mode_ = mode;
}

void renderer::set_present_filter(present_filter filter) noexcept {
    present_filter_ = filter;
}

void renderer::present_frame() {
#if defined(WAG_HAVE_SDL3)
    if (!ctx_.is_valid() || scene_target_.fbo == 0 || output_w_ <= 0 ||
        output_h_ <= 0) {
        return;
    }
    if (target_bound_) {
        unbind_render_target_();
    }
    ensure_present_resources_();

    const space::fit_box fit = space::fit_into(
        static_cast<double>(scene_target_.w),
        static_cast<double>(scene_target_.h),
        static_cast<double>(output_w_),
        static_cast<double>(output_h_),
        present_mode_ == present_mode::stretch);
    const int dx = static_cast<int>(fit.x + 0.5);
    const int dy = static_cast<int>(fit.y + 0.5);
    const int dw = static_cast<int>(fit.w + 0.5);
    const int dh = static_cast<int>(fit.h + 0.5);

    gl_api::bind_framebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, output_w_, output_h_);
    glClearColor(kMailboxColor[0], kMailboxColor[1], kMailboxColor[2],
                 kMailboxColor[3]);
    glClear(GL_COLOR_BUFFER_BIT);

    glViewport(dx, dy, dw, dh);
    gl_api::use_program(present_prog_);
    gl_api::active_texture(0x84C0);
    const GLenum filter = (present_filter_ == present_filter::nearest)
                              ? GL_NEAREST
                              : GL_LINEAR;
    glBindTexture(GL_TEXTURE_2D, scene_target_.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    static_cast<GLint>(filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    static_cast<GLint>(filter));
    if (present_u_tex_ >= 0) {
        gl_api::uniform_1i(present_u_tex_, 0);
    }
    gl_api::bind_vertex_array(present_vao_);
    gl_api::draw_arrays(GL_TRIANGLE_STRIP, 0, 4);
    gl_api::bind_vertex_array(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    gl_api::use_program(0);
    glViewport(0, 0, output_w_, output_h_);
#endif
}


// =============================================================================
// 【探针 · 2026-10-18】外置 GLSL 视觉探针
//
// 【它验什么】"引擎能不能正常**动态**渲染一批完全不同的着色器"：
// · 5 个独立程序（各自编译链接，不共享 16 变体的宏体系）
// · 每个程序带 `u_time` ⇒ 画面**逐帧在变**
// · 采样"上一帧快照" ⇒ 效果作用在真实画面上，不是白底
// 【它不碰什么】16 个变体、批状态、排序键、绘制状态全都无关。
// `kShaderProbe = false` 时它对引擎零影响。
// =============================================================================
#if defined(WAG_HAVE_SDL3)
namespace {
constexpr bool kShaderProbe = true;
// 【只放文件名，不含目录】目录由 Lua 传入（：引擎不解析路径）。
// 【本次：只加载一个 GLSL】实验性关卡背景（雾之湖）——
// 全屏一张；kProbeFxCount 控制张数（= 1 时 strip 宽度等于整帧）。
// 想回到五宫格旧形态：把 kProbeFxCount 改回 5，并恢复下面五项文件名。
constexpr int kProbeFxCount = 1;
// 【自定义着色器的顶点侧 ABI —— 归引擎，作者不写】**
// 它必须与内建顶点着色器逐字一致（属性名 / 输出名都要对上），
// 否则作者写的片元接不上。改动它 = 改公开 ABI。
constexpr char kCustomShaderVertexSource[] = R"GLSL(#version 330 core
in vec2 a_pos;
in vec2 a_uv;
in vec4 a_color;
in float a_z;
uniform mat3 u_pixel_to_ndc;
uniform vec2 u_tex_inv_size;
out vec2 v_uv;
out vec4 v_color;
out float v_dist;
void main() {
    vec3 p = u_pixel_to_ndc * vec3(a_pos, 1.0);
    gl_Position = vec4(p.xy, 0.0, 1.0);
    v_uv = a_uv * u_tex_inv_size;
    v_color = a_color;
    v_dist = a_z;
}
)GLSL";

constexpr const char* kProbeFxFiles[5] = {
    "fx_lake.glsl",
};
} // namespace
#endif // WAG_HAVE_SDL3（效果探针的常量）

void renderer::set_shader_probe_dir(const std::string& dir) {
#if defined(WAG_HAVE_SDL3)
    if (dir == probe_dir_) {
        return;
    }
 // 【换目录 ⇒ 旧程序必须销毁】否则每换一次就漏一批着色器/程序。
    for (int i = 0; i < kProbeFxCount; ++i) {
        if (probe_fx_prog_[i] != 0) {
            gl_api::delete_program(probe_fx_prog_[i]);
            probe_fx_prog_[i] = 0;
            probe_fx_u_time_[i] = -1;
            probe_fx_u_tex_[i] = -1;
        }
    }
    probe_dir_ = dir;
    probe_built_ = false;
#else
    (void)dir;
#endif
}

void renderer::build_shader_probe_() {
#if defined(WAG_HAVE_SDL3)
    if (!kShaderProbe || probe_dir_.empty()) {
        return;
    }
 // 【路径 = 脚本给的目录 + 这里的文件名】
 // 引擎只做一次拼接，**不含**任何 `Luminosa/...` 字面量。
    auto join = [](const std::string& dir, const char* name) {
        std::string out = dir;
        if (!out.empty() && out.back() != '/' && out.back() != '\\') {
            out.push_back('/');
        }
        out += name;
        return out;
    };

 // ① 顶点着色器：**用内嵌的那一份**（作者只写片元，2026-10-19 已定）
    const gl_api::uint_t vs = compile_stage(
        gl_api::vertex_shader, kCustomShaderVertexSource, "内嵌顶点着色器");

 // ② 每个效果一份片元 → 一个独立程序
    for (int i = 0; i < kProbeFxCount; ++i) {
        const std::string fp = join(probe_dir_, kProbeFxFiles[i]);
        std::size_t fsz = 0;
        void* const fbytes = SDL_LoadFile(fp.c_str(), &fsz);
        if (fbytes == nullptr) {
            log::warn("draw", log::format("探针·效果片元读不到",
                                    fp + " | " + SDL_GetError()));
            continue;
        }
        const gl_api::uint_t fs = compile_stage(
            gl_api::fragment_shader, static_cast<const char*>(fbytes), fp.c_str());
        SDL_free(fbytes);
        probe_fx_prog_[i] = link_stage(vs, fs);
        gl_api::delete_shader(fs);
        probe_fx_u_time_[i] = gl_api::get_uniform_location(probe_fx_prog_[i], "u_time");
        probe_fx_u_tex_[i] = gl_api::get_uniform_location(probe_fx_prog_[i], "u_texture");
        if (probe_fx_u_time_[i] < 0) {
            log::warn("draw", log::format("探针·效果缺 u_time", fp + "（该条不会动）"));
        }
    }
    gl_api::delete_shader(vs);

 // ③ 接口连通性报告（原来在 init 期那段的检查，搬到这里）
 // 四个属性 + 两个 uniform：名字对不上就是 -1，而 -1 会让渲染静默错乱
    {
        std::string report;
        for (int i = 0; i < kProbeFxCount; ++i) {
            if (probe_fx_prog_[i] == 0) {
                continue;
            }
            const char* const attribs[] = {"a_pos", "a_uv", "a_color", "a_z"};
            const char* const uniforms[] = {"u_pixel_to_ndc", "u_tex_inv_size"};
            for (const char* a : attribs) {
                report += std::string(a) + "=" +
                          std::to_string(gl_api::get_attrib_location(probe_fx_prog_[i], a)) + " ";
            }
            for (const char* u : uniforms) {
                report += std::string(u) + "=" +
                          std::to_string(gl_api::get_uniform_location(probe_fx_prog_[i], u)) + " ";
            }
            break;  // 五个程序共用同一份顶点着色器，报一个就够
        }
        log::record("探针", "外置 GLSL 接口位置", report);
    }
    log::record("探针", "效果程序已建立", "1 个（目录由脚本给，各自带 u_time）");
#endif
}


void renderer::draw_shader_probe() {
#if defined(WAG_HAVE_SDL3)
    if (!kShaderProbe || fb_w_ <= 0 || fb_h_ <= 0) {
        return;
    }
 // 【延迟建立】程序要等脚本给出目录之后才建 —— 因为 GL 资源初始化时
 // 脚本还没跑，那时引擎拿不到任何路径（：路径由 Lua 拼）。
    if (!probe_built_) {
        if (probe_dir_.empty()) {
            return;
        }
        build_shader_probe_();
        probe_built_ = true;
    }
 // 没有可用快照（第一帧）时用白纹理兜底 —— 效果仍会显形（只是底图是白的）
    const gl_api::uint_t src_tex =
        (staged_valid_ && staged_color_tex_ != 0) ? staged_color_tex_ : white_texture_;
    const int src_w = (staged_valid_ && staged_w_ > 0) ? staged_w_ : 1;
    const int src_h = (staged_valid_ && staged_h_ > 0) ? staged_h_ : 1;

 // 【时间】探针专用：取墙钟秒。
 // ⚠️ 这不是引擎的确定性时间源（要求时间只能来自帧计数）。
 // 正式效果链要另立定案；这里只为"让它动起来"。
    const float t = static_cast<float>(SDL_GetTicks()) / 1000.0f;

 // 【为什么 5 条竖带而不是整屏】一屏同时看见 5 个效果，
 // 且每一格都采样同一张底图 ⇒ 差异只来自着色器，不来自内容。
    const int strip_w = fb_w_ / kProbeFxCount;

 // 顶点：整条竖带的像素矩形，UV 取"整张底图"（staged 是 framebuffer 朝向 ⇒ v 反）
    const float tw = static_cast<float>(src_w);
    const float th = static_cast<float>(src_h);
    vertex quad[4];
    quad[0] = vertex{0.0f, 0.0f, 0.0f, 0.0f, th, 0xFFFFFFFFu};
    quad[1] = vertex{static_cast<float>(strip_w), 0.0f, 0.0f, tw, th, 0xFFFFFFFFu};
    quad[2] = vertex{static_cast<float>(strip_w), static_cast<float>(fb_h_), 0.0f, tw, 0.0f,
                     0xFFFFFFFFu};
    quad[3] = vertex{0.0f, static_cast<float>(fb_h_), 0.0f, 0.0f, 0.0f, 0xFFFFFFFFu};
    const std::uint16_t idx[6] = {0, 1, 2, 0, 2, 3};

    gl_api::bind_vertex_array(vao_);
    gl_api::bind_texture(GL_TEXTURE_2D, src_tex);
    apply_blend_state_(blend_state::normal);
    glEnable(GL_SCISSOR_TEST);

    for (int i = 0; i < kProbeFxCount; ++i) {
        if (probe_fx_prog_[i] == 0) {
            continue;
        }
 // 视口与 scissor 都切到这一条竖带
        gl_api::bind_framebuffer(gl_api::framebuffer, 0);
        glViewport(i * strip_w, 0, strip_w, fb_h_);
        glScissor(i * strip_w, 0, strip_w, fb_h_);

        gl_api::use_program(probe_fx_prog_[i]);
 // 整条竖带的像素 → NDC（与主矩阵推导相同，只是宽度变成 strip_w）
        gl_api::float_t m[9] = {};
        m[0] = 2.0f / static_cast<gl_api::float_t>(strip_w);
        m[4] = -2.0f / static_cast<gl_api::float_t>(fb_h_);
        m[6] = -1.0f;
        m[7] = 1.0f;
        gl_api::uniform_matrix_3fv(
            gl_api::get_uniform_location(probe_fx_prog_[i], "u_pixel_to_ndc"), 1, 0, m);
        gl_api::uniform_2f(
            gl_api::get_uniform_location(probe_fx_prog_[i], "u_tex_inv_size"),
            1.0f / static_cast<gl_api::float_t>(src_w),
            1.0f / static_cast<gl_api::float_t>(src_h));

        if (probe_fx_u_time_[i] >= 0) {
            gl_api::uniform_1f(probe_fx_u_time_[i], t);
        }
        if (probe_fx_u_tex_[i] >= 0) {
 // u_texture 的缺省就是单元 0；显式设一遍，免得将来加采样器对象时崩
            gl_api::uniform_1f(probe_fx_u_tex_[i], 0.0f);
        }

        gl_api::bind_buffer(gl_api::array_buffer, vbo_);
        gl_api::buffer_data(gl_api::array_buffer,
                            static_cast<gl_api::sizeiptr_t>(sizeof(quad)), quad,
                            gl_api::stream_draw);
        gl_api::bind_buffer(gl_api::element_array_buffer, ibo_);
        gl_api::buffer_data(gl_api::element_array_buffer,
                            static_cast<gl_api::sizeiptr_t>(sizeof(idx)), idx,
                            gl_api::stream_draw);
        gl_api::draw_elements(GL_TRIANGLES, 6, gl_api::unsigned_short, nullptr);
    }
    glDisable(GL_SCISSOR_TEST);
#endif
}

// =============================================================================
// 自定义着色器（2026-10-19）
//
// 【模型】作者写**片元**；顶点侧用内嵌的 `kCustomShaderVertexSource`。
// 【它是一维批状态】程序变 ⇒ flush（与混合/纹理同类）。
// 【它同时改变坐标语义】开了它 ⇒ 顶点按整帧像素解释（不做世界变换），
// `v_uv` 归一化到该四边形。实现落在 draw_vertices 里（见那段注释）。
// 【失败 = L3】编译/链接失败会终止（与"资源加载失败即 L3"一致）。
// =============================================================================

const renderer::custom_shader* renderer::find_shader_(
    std::uint32_t handle) const noexcept {
 // 低 6 位 = 槽位 + 1；高 26 位 = 代数。0 = 没有。
    const std::uint32_t code = handle & 63u;
    if (code == 0) {
        return nullptr;
    }
    const std::uint32_t slot = code - 1;
    if (slot >= shaders_.size()) {
        return nullptr;
    }
    const custom_shader& s = shaders_[slot];
    if (s.program == 0 || s.gen != (handle >> 6)) {
        return nullptr;
    }
    return &s;
}

const renderer::custom_shader* renderer::find_shader_by_program_(
    gl_api::uint_t program) const noexcept {
    if (program == 0) {
        return nullptr;
    }
    for (const custom_shader& s : shaders_) {
        if (s.program == program) {
            return &s;
        }
    }
    for (const custom_shader& s : retired_shaders_) {
        if (s.program == program) {
            return &s;
        }
    }
    return nullptr;
}

bool renderer::valid_shader_handle(std::uint32_t handle) const noexcept {
    return find_shader_(handle) != nullptr;
}

std::size_t renderer::shader_count() const noexcept {
    std::size_t n = 0;
    for (const custom_shader& s : shaders_) {
        if (s.program != 0) {
            ++n;
        }
    }
    return n;
}

void renderer::update_shader_watermark_() {
    const std::size_t n = shader_count();
    if (n >= shader_watermark) {
        if (!shader_watermark_hit_) {
            shader_watermark_hit_ = true;
            log::record("资源", log::format("着色器达 80% 水位",
                std::to_string(n) + " / " +
                    std::to_string(shader_capacity)));
        }
    } else {
        shader_watermark_hit_ = false;
    }
}

std::uint32_t renderer::load_shader(const std::string& name, const char* path) {
#if defined(WAG_HAVE_SDL3)
    const auto load_t0 = std::chrono::steady_clock::now();
    if (path == nullptr) {
 // 绑定层已经拦了空路径；这里是直接 C++ 调用的最后一道边界。
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "load_shader 的路径";
        e.got = "nullptr";
        e.want = "非空的片元着色器路径";
        e.sentence = "加载着色器失败：路径为空";
        fail_boundary(e);
    }
 // 【同名覆盖】与图片一致：先卸掉旧的，否则表里会堆同名项。
 // overwrite 的“卸载→加载”是同一个操作：水位只在最终 load 后更新，
 // 否则每覆盖一次都会“掉下水位再升上来”，重复刷 80% 日志。
    shader_load_in_progress_ = true;
    unload_shader(name);

    if (shader_count() >= shader_capacity) {
 // 【2026-11-10 确认】槽满 ⇒ L3（与图片容量同一条错误等级）。
 // 不再返回 0：0 是"没有着色器"，不能同时兼作"加载失败"。
        char got_buf[64];
        std::snprintf(got_buf, sizeof(got_buf), "%zu 个活动槽（上限 %zu）",
                      shader_count(), shader_capacity);
        error_value e;
        e.cls = error_class::resource_failure;
        e.where = "load_shader 的着色器槽位";
        e.got = got_buf;
        e.want = "先 unload 一个不再使用的着色器，或把 32 当作设计上限";
        e.sentence = "着色器数量已达上限";
        fail_boundary(e);
    }

    std::size_t fsz = 0;
    void* const bytes = SDL_LoadFile(path, &fsz);
    if (bytes == nullptr) {
 // 【读不到 = L3】资源缺失不是"可降级"，与图片同一条：失败即终止。
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "load_shader 的路径";
        e.got = SDL_GetError();
        e.want = "一个存在的片元着色器文件（.glsl）";
        fail_boundary(e);
    }
    const gl_api::uint_t fs = compile_stage(
        gl_api::fragment_shader, static_cast<const char*>(bytes), path);
    SDL_free(bytes);

    const gl_api::uint_t vs = compile_stage(
        gl_api::vertex_shader, kCustomShaderVertexSource, "内嵌顶点着色器");
    const gl_api::uint_t prog = link_stage(vs, fs);
    gl_api::delete_shader(vs);
    gl_api::delete_shader(fs);

    custom_shader s;
    s.program = prog;
    s.name = name;
    s.u_pixel_to_ndc = gl_api::get_uniform_location(prog, "u_pixel_to_ndc");
    s.u_tex_inv_size = gl_api::get_uniform_location(prog, "u_tex_inv_size");
    s.u_time = gl_api::get_uniform_location(prog, "u_time");
    s.u_resolution = gl_api::get_uniform_location(prog, "u_resolution");
    s.u_params = gl_api::get_uniform_location(prog, "u_params");
    s.u_box = gl_api::get_uniform_location(prog, "u_box");
    s.u_tex_size = gl_api::get_uniform_location(prog, "u_tex_size");

 // 【复用空槽】槽位下标固定；句柄 = 代数与槽位编码。
    std::uint32_t slot = 0;
    bool reused = false;
    for (std::size_t i = 0; i < shaders_.size(); ++i) {
        if (shaders_[i].program == 0) {
            slot = static_cast<std::uint32_t>(i);
            reused = true;
            break;
        }
    }
    if (!reused) {
        slot = static_cast<std::uint32_t>(shaders_.size());
        shaders_.emplace_back();
    }

    custom_shader& dest = shaders_[slot];
    if (dest.gen == 0) {
        dest.gen = 1; // 新槽从 1 起；复用槽的 gen 已在卸载时 +1 过
    }
    s.gen = dest.gen;
    dest = s;

    const std::uint32_t handle = (dest.gen << 6) | (slot + 1);
    log::record("draw", "着色器已加载", name + "（句柄 " + std::to_string(handle) + "）");
    shader_load_in_progress_ = false;
    update_shader_watermark_();
    log_load_time_ms(
        "着色器",
        name,
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - load_t0)
            .count());
    return handle;
#else
    (void)name;
    (void)path;
    return 0;
#endif
}

void renderer::unload_shader(const std::string& name) {
#if defined(WAG_HAVE_SDL3)
    for (custom_shader& s : shaders_) {
        if (s.program != 0 && s.name == name) {
 // 【】本帧退役表上限：超过 128 个 ⇒ L3。
 // 一帧卸载 128 个着色器已经是异常 churn；不能让它无限占 GPU 内存。
            if (retired_shaders_.size() >= retired_shader_cap) {
                char got_buf[64];
                std::snprintf(got_buf, sizeof(got_buf), "%zu 个（上限 %zu）",
                              retired_shaders_.size(), retired_shader_cap);
                error_value e;
                e.cls = error_class::resource_failure;
                e.where = "unload_shader 的本帧退役队列";
                e.got = got_buf;
                e.want = "本帧少卸载一些着色器（它们要活到帧末）";
                e.sentence = "本帧退役的着色器过多";
                fail_boundary(e);
            }
            if (retired_shaders_.size() + 1 >= retired_shader_warn &&
                !retired_shader_warned_) {
                retired_shader_warned_ = true;
                log::record("资源", log::format("本帧退役着色器达 32（上限 128）",
                    std::to_string(retired_shaders_.size() + 1) + " 个 | " + name));
            }
 // 【句柄立刻失效；program 活到帧末】
 // 本帧已收集的绘制项持有的是 program，不是句柄 ⇒ 它们仍能画完。
 // 与图片纹理的 pending_destroy_tex_ 同形。
            retired_shaders_.push_back(s);
            const std::uint32_t next_gen = s.gen + 1;
            s = custom_shader{};
            s.gen = next_gen;
            log::record("draw", "着色器已卸载（program 帧末回收）", name);
        }
    }
 // overwrite 的“卸载→加载”由 load_shader 合起来更新水位；
 // 单独 unload 时在这里更新（否则永远不会掉下 80%）。
    if (!shader_load_in_progress_) {
        update_shader_watermark_();
    }
#else
    (void)name;
#endif
}

// =============================================================================
// 最小重画入口（暴露回调用：清整屏到信箱色）
// =============================================================================

void renderer::redraw_last_frame(int pixel_w, int pixel_h) {
 // 【先清】万一没有快照，至少是信箱色而不是上一轮残留的垃圾。
    clear_framebuffer(pixel_w, pixel_h);
#if defined(WAG_HAVE_SDL3)
    if (pixel_w <= 0 || pixel_h <= 0) {
        return;
    }
    if (!staged_valid_ || staged_color_tex_ == 0 || staged_w_ <= 0 ||
        staged_h_ <= 0) {
 // 【没有快照 ⇒ 退化为只清屏】与旧行为一致，不是错误。
        return;
    }

 // 【变体固定为"最朴素那一档"】顶点色 Mul + 纹理 alpha Normal + 无雾。
 // 拖拽重画是"原样照抄"，不该受脚本设过的任何绘制状态影响。
    const std::size_t vi = variant_index(vertex_color_mode::mul,
                                         tex_alpha::normal, false);
    gl_api::use_program(programs_[vi]);
    gl_api::bind_vertex_array(vao_);

 // 【整帧矩阵】像素 → NDC，不经 transform_、不留信箱边距：
 // x: x_ndc = 2/w · x − 1 y: y_ndc = −2/h · y + 1（y 向下 ⇒ 斜率为负）
    const gl_api::float_t w = static_cast<gl_api::float_t>(pixel_w);
    const gl_api::float_t h = static_cast<gl_api::float_t>(pixel_h);
    gl_api::float_t m[9] = {};
    m[0] = 2.0f / w;
    m[4] = -2.0f / h;
    m[6] = -1.0f;
    m[7] = 1.0f;
    gl_api::uniform_matrix_3fv(uniform_pixel_to_ndc_[vi], 1,
                               0 /* GL_FALSE：列主序，不转置 */, m);
    if (uniform_tex_inv_size_[vi] >= 0) {
        gl_api::uniform_2f(uniform_tex_inv_size_[vi],
                           1.0f / static_cast<gl_api::float_t>(staged_w_),
                           1.0f / static_cast<gl_api::float_t>(staged_h_));
    }

    gl_api::bind_texture(GL_TEXTURE_2D, staged_color_tex_);
    apply_blend_state_(blend_state::normal);

 // 【顶点 = 快照原尺寸，1:1，锚在左上角】
 // ⚠️ v 轴必须反向：staged 是 **framebuffer 朝向**（v=0 在底），
 // 而顶点 y=0 在屏幕顶部 ⇒ 左上顶点要取 v = 快照高度。
 // 写反的症状：拖拽期间画面上下颠倒（一眼可见）。
 // 【为什么不缩放】缩放会让拖拽期间的画面比例跳变；
 // 宁可在"窗口变大"时让新长出来的一圈保持信箱黑（松手后立刻正确）。
    const float tw = static_cast<float>(staged_w_);
    const float th = static_cast<float>(staged_h_);
    vertex quad[4];
    quad[0] = vertex{0.0f, 0.0f, 0.0f, 0.0f, th, 0xFFFFFFFFu};   // 左上
    quad[1] = vertex{tw, 0.0f, 0.0f, tw, th, 0xFFFFFFFFu};       // 右上
    quad[2] = vertex{tw, th, 0.0f, tw, 0.0f, 0xFFFFFFFFu};       // 右下
    quad[3] = vertex{0.0f, th, 0.0f, 0.0f, 0.0f, 0xFFFFFFFFu};   // 左下
    const std::uint16_t idx[6] = {0, 1, 2, 0, 2, 3};

 // 【用栈上的临时数组直接上传】不动 batch_buffer_ ——
 // 本函数可能在帧中途被调用（事件是在阶段 3 的泵里派发的），
 // 写批缓冲会把那一帧已攒的顶点弄脏。
    gl_api::bind_buffer(gl_api::array_buffer, vbo_);
    gl_api::buffer_data(gl_api::array_buffer,
                        static_cast<gl_api::sizeiptr_t>(sizeof(quad)), quad,
                        gl_api::stream_draw);
    gl_api::bind_buffer(gl_api::element_array_buffer, ibo_);
    gl_api::buffer_data(gl_api::element_array_buffer,
                        static_cast<gl_api::sizeiptr_t>(sizeof(idx)), idx,
                        gl_api::stream_draw);
    gl_api::draw_elements(GL_TRIANGLES, 6, gl_api::unsigned_short, nullptr);
#endif
}

void renderer::clear_framebuffer(int pixel_w, int pixel_h) {
 // 【它是什么】"第二个渲染入口"，只做一件事：关 scissor、清整屏到信箱色。
 // 主线程判断与呈现（Swap）都由调用方做 —— 那三条约束任何一条都不许省。
 // ⚠️ 它**不碰** transform_ / 帧绘制列表 / 批缓冲 ⇒ 下一帧照旧。
#if defined(WAG_HAVE_SDL3)
    if (pixel_w <= 0 || pixel_h <= 0 || !ctx_.is_valid()) {
        return;
    }
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, pixel_w, pixel_h);
    glClearColor(kMailboxColor[0], kMailboxColor[1], kMailboxColor[2],
                 kMailboxColor[3]);
    glClear(GL_COLOR_BUFFER_BIT);
#else
    (void)pixel_w;
    (void)pixel_h;
#endif
}

// =============================================================================
// GPU 资源建立（第一档着色器变体 / 白纹理 / VAO / VBO / 混合初值）
// =============================================================================

#if defined(WAG_HAVE_SDL3)
void renderer::init_gl_resources_() {
 // ---- 着色器变体：**启动时全部预编译**----
 // 【当前规模】三维全开：顶点色 ×4 × 纹理 alpha ×2 × 雾 ×2 ⇒ **16 个程序**。
 // 【为什么预编译而不做运行期分支】："第一次切到某效果时掉帧"
 // 是 STG 里最难查的一类抖动 ⇒ 宁可启动多花几十毫秒。
 // 【顶点着色器只编译一次】所有变体共用同一份顶点着色器（变体只在片元侧）。
    const gl_api::uint_t vs =
        compile_stage(gl_api::vertex_shader, kVertexShaderSource, "顶点着色器");

    for (std::size_t m = 0; m < vertex_color_count; ++m) {
        const vertex_color_mode vc = static_cast<vertex_color_mode>(m);
        for (std::size_t a = 0; a < tex_alpha_count; ++a) {
            const tex_alpha t = static_cast<tex_alpha>(a);
            for (std::size_t g = 0; g < fog_count; ++g) {
                const bool fog_on = (g == 1);

 // 片元源码 = 头 + 三维的宏 + 主体（宏决定 rgb / a / 雾各自怎么算）
                std::string fs_src = kFragmentShaderHeader;
                fs_src += "#define VCOL_MODE ";
                fs_src += std::to_string(static_cast<unsigned>(m));
                fs_src += "\n";
                fs_src += (t == tex_alpha::replace)
                              ? "#define TEX_ALPHA_REPLACE 1\n"
                              : "#define TEX_ALPHA_REPLACE 0\n";
                fs_src += fog_on ? "#define FOG_ON 1\n" : "#define FOG_ON 0\n";
                fs_src += kFragmentShaderBody;

                char label[160];
                static_cast<void>(std::snprintf(
                    label, sizeof(label),
                    "片元着色器（顶点色 = %s，纹理 alpha = %s，雾 = %s）",
                    vertex_color_name(vc), tex_alpha_name(t),
                    fog_on ? "开" : "关"));
                const gl_api::uint_t fs =
                    compile_stage(gl_api::fragment_shader, fs_src.c_str(), label);
                programs_[variant_index(vc, t, fog_on)] =
                    link_stage(vs, fs, true);
 // 链接完成后阶段对象不再有用（程序已持有它们的产物），删掉防泄漏。
                gl_api::delete_shader(fs);
            }
        }
    }
    gl_api::delete_shader(vs);

 // ---- 变体账写进日志（："启动时预编译 + 写进日志"）----
    {
        char detail[160];
        static_cast<void>(std::snprintf(
            detail, sizeof(detail),
            "已编译 %zu 个着色器变体（顶点色 ×%zu × 纹理 alpha ×%zu × 雾 ×%zu）",
            pipeline_variant_count, vertex_color_count, tex_alpha_count,
            fog_count));
        log::record("draw", log::format("着色器变体已预编译", detail));
    }

 // 【2026-10-18 移走了：外置 GLSL 的接口验证】
 // 它原来在这里硬编码 `Luminosa/shaders/...` 读文件 —— 那违反了
 // /（目录结构固定 + 引擎不解析路径）。
 // 现在改由 `set_shader_probe_dir()` 收下 **Lua 给的目录**，
 // 并在**首次绘制时**才建程序（那时脚本已经跑过）。

 // ---- 属性与 uniform 位置：链接后**查询**，不用 layout(location) ----
 // 【为什么查询】显式 location（GLSL 的 layout qualifiers）要到 GL 4.3
 // 才进核心（ARB_explicit_attrib_location），严格 3.3 上下文上不保证
 // 可用 ⇒ 查询是零依赖的写法。
 // 【名字的一致性】这里的字符串必须与上面 GLSL 源码里的 in/uniform 同名
 // —— 取不到（返回 -1）⇒ 引擎内部错误（两处源码名字对不上），
 // 绝不静默继续（缺属性的顶点数据是未定义行为）。
 // 【uniform 逐变体查】每个程序有自己的一套 location（不保证跨程序相同）。
 // ⚠️【雾的三个 uniform 只在有雾变体里存在】无雾变体里 GLSL 编译器会把
 // 它们整个移除 ⇒ 查询得到 **-1 是正常的**，不能当错误。
 // ⚠️【u_tex_inv_size 同理，但只在 Alphablend + Replace 变体里可缺失】
 // 那个组合既不采纹理 rgb，也不读纹理 a，texture()/v_uv 整条链会被
 // 驱动裁掉；AMD 可能保留、NVIDIA/Intel 通常删除。把 -1 一律当错误
 // 会让第三方机器在启动阶段 L4。
    for (std::size_t i = 0; i < pipeline_variant_count; ++i) {
        const std::size_t fog_index = i % fog_count;
        const std::size_t alpha_index =
            (i / fog_count) % tex_alpha_count;
        const std::size_t color_index =
            i / (fog_count * tex_alpha_count);
        const auto vc = static_cast<vertex_color_mode>(color_index);
        const auto t = static_cast<tex_alpha>(alpha_index);
        const bool needs_tex_inv_size = variant_needs_tex_inv_size(vc, t);

        char variant_label[160];
        static_cast<void>(std::snprintf(
            variant_label, sizeof(variant_label),
            "顶点色=%s，纹理 alpha=%s，雾=%s",
            vertex_color_name(vc), tex_alpha_name(t),
            fog_index == 1 ? "开" : "关"));

        uniform_pixel_to_ndc_[i] =
            gl_api::get_uniform_location(programs_[i], "u_pixel_to_ndc");
        uniform_tex_inv_size_[i] =
            gl_api::get_uniform_location(programs_[i], "u_tex_inv_size");
        if (uniform_pixel_to_ndc_[i] < 0 ||
            (needs_tex_inv_size && uniform_tex_inv_size_[i] < 0)) {
            std::string detail = std::string("着色器变体缺少预期的 uniform（") +
                                 variant_label + "；";
            if (uniform_pixel_to_ndc_[i] < 0) {
                detail += "u_pixel_to_ndc location=-1；";
            }
            if (needs_tex_inv_size && uniform_tex_inv_size_[i] < 0) {
                detail += "u_tex_inv_size location=-1；";
            }
            detail += "C++ 侧查询名与 GLSL 源码两处名字不一致）";
            fail_internal(detail);
        }
        uniform_fog_color_[i] = gl_api::get_uniform_location(programs_[i], "u_fog_color");
        uniform_fog_range_[i] = gl_api::get_uniform_location(programs_[i], "u_fog_range");
        uniform_fog_strength_[i] =
            gl_api::get_uniform_location(programs_[i], "u_fog_strength");
 // 有雾变体**必须**能取到；无雾变体**必须**取不到（取到说明宏没生效）。
        const bool fog_variant = (fog_index == 1);
        const bool all_present = uniform_fog_color_[i] >= 0 &&
                                 uniform_fog_range_[i] >= 0 &&
                                 uniform_fog_strength_[i] >= 0;
        if (fog_variant != all_present) {
            fail_internal(std::string("雾 uniform 与变体不符（") +
                          variant_label + "）：" +
                          (fog_variant
                               ? "有雾变体缺雾的 uniform —— 宏 FOG_ON 没进着色器源码"
                               : "无雾变体竟然有雾的 uniform —— 宏 FOG_ON 没有生效"));
        }
    }

 // 【属性位置是 VAO 的固定契约，不查驱动】
 // 16 个变体共用一个 VAO（的批状态变体）。GL 不保证不同 program
 // 会把同名属性分到同一 location；Intel/NVIDIA 的链接器会把无雾变体的
 // a_z 优化掉，也会给不同 program 分不同位置。若运行时查了再比，
 // 第三家驱动仍可能在这里炸。
 // ⇒ 链接前用 glBindAttribLocation 固定 0/1/2/3；VAO 直接按这个契约设置。
 // 驱动把某个属性优化掉没有关系：那个 location 只是没人读。
    attrib_pos_ = 0;
    attrib_uv_ = 1;
    attrib_color_ = 2;
    attrib_z_ = 3;

 // （变体账的日志已在编译循环后记录过一次；这里不再重复 —— "同一件事打两行日志"
 // 会让读日志的人以为编译了两轮。）

 // ---- 默认白纹理（1×1 不透明白）----
 // 【它为什么存在】"无图片的纯色绘制"需要一个合法的采样目标；
 // 全 1 纹理 × 顶点色 = 顶点色 ⇒ draw_vertices 不绑图也出正确的纯色。
 // 【它同时是纹理上传路径的最小验收】glTexImage2D 这条路由此打通。
    gl_api::gen_textures(1, &white_texture_);
    gl_api::bind_texture(GL_TEXTURE_2D, white_texture_);
    const std::uint8_t white_pixel[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    gl_api::tex_image_2d(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, white_pixel);
 // ⚠️【必须显式设 MIN_FILTER —— GL 的一个经典坑】默认值是
 // NEAREST_MIPMAP_LINEAR，而我们没有上传 mipmap ⇒ 纹理被判"不完整"
 // ⇒ 采样结果为黑。1×1 纹理上过滤方式本无数学意义，设 NEAREST 纯为
 // 关掉 mipmap 依赖。
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
 // ⚠️【WRAP 必须显式设成 clamp】缺省是 GL_REPEAT（= 取模）⇒ 图集上会绕回别处。
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            static_cast<gl_api::int_t>(gl_api::clamp_to_edge));
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            static_cast<gl_api::int_t>(gl_api::clamp_to_edge));

 // ---- 帧级批状态初值：白纹理 + 尺寸倒数 (1,1) ----
    pending_tex_ = white_texture_;
    pending_inv_size_[0] = 1.0f;
    pending_inv_size_[1] = 1.0f;

 // ---- VAO / VBO 与属性布局 ----
 // 【布局 = renderer::vertex 本体】stride = sizeof(vertex)；
 // 四个属性：位置(x,y) / 纹理(u,v) / 颜色(4 字节归一化) / 距离(z，雾用)。
    gl_api::gen_vertex_arrays(1, &vao_);
    gl_api::gen_buffers(1, &vbo_);
    gl_api::gen_buffers(1, &ibo_);
    gl_api::bind_vertex_array(vao_);
    gl_api::bind_buffer(gl_api::array_buffer, vbo_);
 // 索引缓冲也挂在 VAO 上（ELEMENT_ARRAY_BUFFER 的绑定属于 VAO 状态）；
 // 每次 flush 会显式再绑一次并整包上传（orphaning），这里的绑定只是初值。
    gl_api::bind_buffer(gl_api::element_array_buffer, ibo_);

 // 偏移量与 vertex 字段的对齐关系靠这一条编译期事实钉住：
 // 谁改了 vertex 的字段，这里先炸（而不是运行期采到错位数据）。
    static_assert(sizeof(vertex) == 24,
                  "vertex 的布局变了 —— 下面的属性偏移与 stride 必须同步改");

    gl_api::enable_vertex_attrib_array(static_cast<gl_api::uint_t>(attrib_pos_));
    gl_api::vertex_attrib_pointer(static_cast<gl_api::uint_t>(attrib_pos_), 2,
                                  GL_FLOAT, GL_FALSE, sizeof(vertex),
                                  reinterpret_cast<const void*>(offsetof(vertex, x)));
    gl_api::enable_vertex_attrib_array(static_cast<gl_api::uint_t>(attrib_uv_));
    gl_api::vertex_attrib_pointer(static_cast<gl_api::uint_t>(attrib_uv_), 2,
                                  GL_FLOAT, GL_FALSE, sizeof(vertex),
                                  reinterpret_cast<const void*>(offsetof(vertex, u)));
 // 颜色：4 字节无符号、归一化到 [0,1]。
 // ⚠️【ARGB 的字节序 —— **已定且已实现，不要在这里"再补一次换算"**】
 // vertex::color 是 ARGB 的 u32（0xAARRGGBB），小端内存序是 B,G,R,A
 // ⇒ attribute 实际读到 (b, g, r, a)。
 // **换算在片元着色器里做**：`vec4(v_color.bgr, v_color.a)`
 // （见本文件上方的 kFragmentShaderSource 与其后那段逐字节推导）。
 // ⇒ CPU 侧**不得**再做字节重排：那会变成**双重交换**，
 // 症状正是"红蓝互换"——一类极难归因的缺陷。
    gl_api::enable_vertex_attrib_array(static_cast<gl_api::uint_t>(attrib_color_));
    gl_api::vertex_attrib_pointer(static_cast<gl_api::uint_t>(attrib_color_), 4,
                                  GL_UNSIGNED_BYTE, GL_TRUE, sizeof(vertex),
                                  reinterpret_cast<const void*>(offsetof(vertex, color)));
 // z：**雾的距离量**（2026-10-18 起它不再是"留洞"）。
 // 【在此之前】z 不进属性、不参与计算（深度测试已关）。
 // 【现在】雾要**逐顶点**的距离并被插值（"逐像素算 f，用插值后的 z"）
 // ⇒ 它必须作为真正的顶点属性上传，否则片元里拿不到值。
 // ⚠️ 它**只用于雾**：排序用的是"调用的 z"，与这里无关（明确"不碰它作为排序键的作用"）。
    gl_api::enable_vertex_attrib_array(static_cast<gl_api::uint_t>(attrib_z_));
    gl_api::vertex_attrib_pointer(static_cast<gl_api::uint_t>(attrib_z_), 1,
                                  GL_FLOAT, GL_FALSE, sizeof(vertex),
                                  reinterpret_cast<const void*>(offsetof(vertex, z)));

 // ---- 混合：第一档的 alpha（那一格的另一半）----
 // 【现在：全局唯一的混合状态】批次**形态乙**（收集 → 排序 → 发射）已经落地，
 // 但批状态（flush 触发②）尚未接入 ⇒ 每帧仍然是**一种混合**。
 // 将来"每个绘制项各自指定混合"（混合状态那 8 对因子，随状态批落地）接进来时，
 // **改的是 flush 前的那段状态设置**，不是这里 —— 这里是帧级初值。
 // alpha 通道用 (ONE, ONE_MINUS_SRC_ALPHA)：与 RGB 通道的标准 alpha
 // 合成保持一致，避免帧缓冲 alpha 被重复累积（后处理/FBO 那一步会感激它）。
    glEnable(GL_BLEND);
    gl_api::blend_func_separate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                GL_ONE_MINUS_SRC_ALPHA);

 // ---- 采样器 uniform 不需要设置（写下来，防止后人"补"它）----
 // u_texture 的缺省值是 0（纹理单元 0），而我们从不调 active_texture
 // （默认活动单元就是 0）⇒ 恰好绑上。
 // ⚠️ 若将来引入第二个纹理单元，必须给入口表补 uniform_1i —— 到那时
 // 这里"不需要"的注释也一起改。
    log::record("draw", "GPU 资源已建立（第一档着色器 / 白纹理 / VAO / VBO）");
}

// =============================================================================
// 收集（渲染钩子期间）—— 顶点进帧顶点池，绘制项进帧绘制列表
// =============================================================================

void renderer::collect_primitive_(const vertex* gpu, std::size_t count,
                                  std::int64_t layer, double z,
                                  gl_api::uint_t tex, double inv_w, double inv_h,
                                  gl_api::uint_t shader_program) {
 // 顶点进帧顶点池（fan 语义原样保留，发射时才展开 —— 保住档位算术）
    const std::uint32_t begin = static_cast<std::uint32_t>(frame_vertices_.size());
    frame_vertices_.insert(frame_vertices_.end(), gpu, gpu + count);

    draw_item item;
    item.stage = collect_stage_;
    item.source = 1; // 脚本绘制（容器来源随容器批加入，值 0）
    item.topology = 0; // 扇形
    item.layer = layer;
    item.z = z;
    item.tex = tex;
    item.inv_size[0] = static_cast<gl_api::float_t>(inv_w);
    item.inv_size[1] = static_cast<gl_api::float_t>(inv_h);
    item.vertex_begin = begin;
    item.vertex_count = static_cast<std::uint32_t>(count);
    item.index_count = static_cast<std::uint32_t>(3 * (count - 2));
 // 【混合状态：入批时算生效值】 的统一规则 ——
 // 当前状态已显式设置 ⇒ 用那个；否则用该维默认。
    item.blend = state_.blend_set ? state_.blend : blend_state::normal;
    item.alpha = state_.tex_alpha_set ? state_.alpha : tex_alpha::normal;
    item.vcolor = state_.vcolor_set ? state_.vcolor : vertex_color_mode::mul;
    item.fog = state_.fog_set ? state_.fog : fog_state{};
 // 【自定义着色器：入批时已是 program】句柄在调用方解析并校验过；这里只存
 // 已解析的 program，使"卸载后旧绘制项仍能画完"成立。
    item.shader_program = shader_program;
    item.params_set = state_.params_set;
    for (int k = 0; k < 4; ++k) {
        item.params[k] = state_.params[k];
    }
    item.sequence = ++creation_counter_;
    draw_items_.push_back(item);
    ++stat_primitives_;
}

void renderer::collect_triangles_(const vertex* gpu, std::size_t count,
                                  std::int64_t layer, double z,
                                  gl_api::uint_t tex, double inv_w, double inv_h,
                                  gl_api::uint_t shader_program) {
    const std::uint32_t begin = static_cast<std::uint32_t>(frame_vertices_.size());
    frame_vertices_.insert(frame_vertices_.end(), gpu, gpu + count);

    draw_item item;
    item.stage = collect_stage_;
    item.source = 1;
    item.topology = 1; // 三角形列表
    item.layer = layer;
    item.z = z;
    item.tex = tex;
    item.inv_size[0] = static_cast<gl_api::float_t>(inv_w);
    item.inv_size[1] = static_cast<gl_api::float_t>(inv_h);
    item.vertex_begin = begin;
    item.vertex_count = static_cast<std::uint32_t>(count);
    item.index_count = static_cast<std::uint32_t>(count); // 顺序索引：每顶点一枚
 // 【混合状态：入批时算生效值】 的统一规则 ——
 // 当前状态已显式设置 ⇒ 用那个；否则用该维默认。
    item.blend = state_.blend_set ? state_.blend : blend_state::normal;
    item.alpha = state_.tex_alpha_set ? state_.alpha : tex_alpha::normal;
    item.vcolor = state_.vcolor_set ? state_.vcolor : vertex_color_mode::mul;
    item.fog = state_.fog_set ? state_.fog : fog_state{};
 // 【自定义着色器：入批时已是 program】句柄在调用方解析并校验过；这里只存
 // 已解析的 program，使"卸载后旧绘制项仍能画完"成立。
    item.shader_program = shader_program;
    item.params_set = state_.params_set;
    for (int k = 0; k < 4; ++k) {
        item.params[k] = state_.params[k];
    }
    item.sequence = ++creation_counter_;
    draw_items_.push_back(item);
    stat_primitives_ += count / 3; // 图元数按"笔"计：三角形列表 = count/3 笔
}

// =============================================================================
// 排序 + 发射（end_frame）—— 批缓冲此时退为"发射目标"
// =============================================================================

void renderer::sort_and_emit_draws_() {
 // ---- 稳定排序（排序键从高到低）----
 // 画布/目标（常量 screen）→ 阶段 → layer → z → 来源 → 混合状态（未落地）
 // → 创建序。创建序由稳定排序保证（相等键保持收集序 = 后收集的在后
 // = 在上），comparator 里的 sequence 是给未来"跨池创建序"留的锚。
 // 【z 用精确相等比较】z 表达顺序与批处理此消彼长。
    std::stable_sort(draw_items_.begin(), draw_items_.end(),
                     [](const draw_item& a, const draw_item& b) {
                         if (a.stage != b.stage) {
                             return a.stage < b.stage;
                         }
                         if (a.layer != b.layer) {
                             return a.layer < b.layer;
                         }
                         if (a.z != b.z) {
                             return a.z < b.z;
                         }
                         if (a.source != b.source) {
                             return a.source < b.source;
                         }
 // 【排序键的一维：混合状态】键序：
 // 画布/目标 → 阶段 → layer → z → 来源 → **混合状态** → 创建序
 // ⇒ 它在创建序**之前**（同 blend 才能连成一批），
 // 在来源**之后**（"同层同 z 内池先脚本后"那条不能被它破坏）。
 // ⚠️ 后果：同一个 (layer, z) 内混用不同混合 ⇒ 引擎按 blend **分组重排**，
 // 不是按调用序 —— 这正是 写的"作者义务"。
                         if (a.blend != b.blend) {
                             return static_cast<std::uint8_t>(a.blend) <
                                    static_cast<std::uint8_t>(b.blend);
                         }
                         return a.sequence < b.sequence;
                     });

 // ---- 逐项发射进批缓冲 ----
 // 【触发① 装不下】项的顶点/索引放不下 ⇒ 先 flush（保持提交顺序）。
 // 【触发② 纹理变化】**已落地**：纹理（以及它的尺寸倒数）是采样器状态 ⇒
 // 换纹理必须 flush。
 // 【触发② 混合状态变化】**2026-10-18 落地**：混合因子/方程同样是批状态 ⇒
 // 换混合必须 flush。它与纹理那一条**并列**：任一项变了都要先送走已攒的。
 // 【扇形展开在发射时做】顶点按原始 fan 语义入批（不展开），
 // 索引按 (v0, vk, vk+1) 生成 —— "8192 顶点 = 2048 四边形"成立。
 //
 // ⚠️【帧初的批状态】`pending_tex_` / `pending_blend_` 在 begin_frame 里未必是
 // 初值（是上一帧的末值）⇒ 这里先复位一次，保证"第一项若用初值"也不会漏 flush。
    pending_tex_ = white_texture_;
    pending_inv_size_[0] = 1.0f;
    pending_inv_size_[1] = 1.0f;
    pending_blend_ = blend_state::normal;
    pending_alpha_ = tex_alpha::normal;
    pending_vcolor_ = vertex_color_mode::mul;
    pending_fog_ = fog_state{};
    pending_shader_ = 0;
    pending_params_set_ = false;
    for (int k = 0; k < 4; ++k) {
        pending_params_[k] = 0.0f;
    }

    for (const draw_item& item : draw_items_) {
 // ---- 触发②：纹理或混合状态变了 ⇒ 先把攒的送走，再换状态 ----
 // 【为什么必须在 append 之前】批缓冲里所有顶点共用一套采样器状态 + 混合状态。
        const gl_api::uint_t item_tex =
            (item.tex != 0) ? item.tex : white_texture_;
        const bool tex_changed = (item_tex != pending_tex_);
 // 【UV 倒数也要独立判定】它原本只随纹理变化更新，但**自定义着色器**
 // 的纹理恒为白纹理（tex 不变），而变化的是 inv_size（四边形包围盒）⇒
 // 实测症状：`u_tex_inv_size` 一直是 1.0，`v_uv` 恒等于 (1,1)（整屏纯白）。
        const bool inv_changed =
            (item.inv_size[0] != pending_inv_size_[0] ||
             item.inv_size[1] != pending_inv_size_[1]);
        const bool blend_changed = (item.blend != pending_blend_);
        const bool alpha_changed = (item.alpha != pending_alpha_);
        const bool vcolor_changed = (item.vcolor != pending_vcolor_);
 // 【雾整组比较】启用位变 ⇒ 换程序；颜色/near/far/强度变 ⇒ 换 uniform。
 // 两者都必须 flush（否则新状态会改到已攒的图元）。
        const bool fog_changed = (item.fog != pending_fog_);
 // 【自定义着色器与 u_params 也是批状态】程序变了就要换程序；
 // 参数变了要重传 uniform —— 两者都必须先 flush 已攒的。
        const bool shader_changed = (item.shader_program != pending_shader_);
        bool params_changed = (item.params_set != pending_params_set_);
        if (!params_changed && item.params_set) {
            for (int k = 0; k < 4; ++k) {
                if (item.params[k] != pending_params_[k]) {
                    params_changed = true;
                    break;
                }
            }
        }

        if ((tex_changed || blend_changed || alpha_changed || vcolor_changed ||
             fog_changed ||
             shader_changed || params_changed || inv_changed) &&
            batch_index_count_ != 0) {
            flush_batch_();
        }
 // 【为什么状态更新在 if 外面】缓冲区为空时不必 flush 一个空批，
 // 但**状态仍然必须更新** —— 否则下一项会带着旧状态入批（那是最坏的一类静默错）。
        if (tex_changed) {
            pending_tex_ = item_tex;
 // （inv_size 的更新移到下面：它与纹理无关）
 // （inv_size 的更新移到下面：它与纹理无关）
        }
        if (inv_changed) {
            pending_inv_size_[0] = item.inv_size[0];
            pending_inv_size_[1] = item.inv_size[1];
        }
        if (blend_changed) {
            pending_blend_ = item.blend;
        }
        if (alpha_changed) {
            pending_alpha_ = item.alpha;
        }
        if (vcolor_changed) {
            pending_vcolor_ = item.vcolor;
        }
        if (fog_changed) {
            pending_fog_ = item.fog;
        }
        if (shader_changed) {
            pending_shader_ = item.shader_program;
        }
        if (params_changed) {
            pending_params_set_ = item.params_set;
            for (int k = 0; k < 4; ++k) {
                pending_params_[k] = item.params[k];
            }
        }

 // ---- 触发①：装不下 ----
        if (batch_vertex_count_ + item.vertex_count > batch_buffer_capacity ||
            batch_index_count_ + item.index_count > batch_buffer_capacity * 3) {
            flush_batch_();
        }

        const std::uint16_t base = static_cast<std::uint16_t>(batch_vertex_count_);
        const vertex* src = frame_vertices_.data() + item.vertex_begin;

        if (item.topology == 0) {
 // 扇形：(v0, vk, vk+1)
            const std::size_t n = item.vertex_count;
            for (std::size_t k = 0; k + 2 < n; ++k) {
                batch_indices_[batch_index_count_ + 0] = base;
                batch_indices_[batch_index_count_ + 1] =
                    base + static_cast<std::uint16_t>(k + 1);
                batch_indices_[batch_index_count_ + 2] =
                    base + static_cast<std::uint16_t>(k + 2);
                batch_index_count_ += 3;
            }
        } else {
 // 三角形列表：顺序展开
            for (std::size_t k = 0; k < item.vertex_count; k += 3) {
                batch_indices_[batch_index_count_ + 0] = base + static_cast<std::uint16_t>(k);
                batch_indices_[batch_index_count_ + 1] = base + static_cast<std::uint16_t>(k + 1);
                batch_indices_[batch_index_count_ + 2] = base + static_cast<std::uint16_t>(k + 2);
                batch_index_count_ += 3;
            }
        }

        std::memcpy(batch_buffer_ + batch_vertex_count_, src,
                    item.vertex_count * sizeof(vertex));
        batch_vertex_count_ += item.vertex_count;
    }

    flush_batch_(); // 触发③：帧末（把最后一批送走）

    draw_items_.clear();
    frame_vertices_.clear();
}

void renderer::flush_batch_() {
 // 【空缓冲不是 flush】没有发生提交 ⇒ 不计数（否则统计在说谎）。
    if (batch_index_count_ == 0) {
        return;
    }

 // ---- 一次 flush = 一整套状态（8.1 的最小实例）----
 // flush 触发②（状态变化）落地后，这里的状态在入批时已经比较过；
 // flush 本身只负责"把当前攒的一套状态 + 它的顶点送走"。
 // 【程序 = 变体】顶点色模式 × 纹理 alpha ⇒ 8 个程序之一（2026-10-18）。
 // ⚠️ 换程序要重新设 uniform（每个程序有自己的一套 location）。
 // ⚠️【钳位必须用 pipeline_variant_count】
 // 不能用 `variant < tex_alpha_count`：后者是“只有纹理 alpha 一维”时的写法。
 // 加了顶点色后 `Add` 的下标是 2，会被那个钳位打回 0（Mul）。
 // 这是"数组扩容了但校验没跟着扩"的典型形状。
    const std::size_t variant =
        variant_index(pending_vcolor_, pending_alpha_, pending_fog_.enabled);
    const std::size_t vi = (variant < pipeline_variant_count) ? variant : 0;

 // 【自定义着色器 vs 内建变体】pending_shader_ != 0 ⇒ 用作者的片元程序；
 // 否则走 16 变体之一。两者互斥：程序变了在发射循环里已经 flush 过。
 // 【找不到 ⇒ L3】不退回内建变体：那会让"已卸载的着色器"静默画成另一个效果。
    const custom_shader* const cs =
        (pending_shader_ != 0) ? find_shader_by_program_(pending_shader_) : nullptr;
    if (pending_shader_ != 0 && cs == nullptr) {
        fail_internal("批发射：着色器 program 已失效（program=" +
                      std::to_string(static_cast<unsigned long long>(
                          pending_shader_)) +
                      "）—— 不应发生：program 应活到帧末");
    }
    if (cs != nullptr) {
        gl_api::use_program(cs->program);
    } else {
        gl_api::use_program(programs_[vi]);
    }
    gl_api::bind_vertex_array(vao_);
 // 【纹理 = 批状态的一员】发射时绑的是 `pending_tex_`（白纹理是它的兜底初值）。
 // 触发②（纹理变化就 flush）在 `sort_and_emit_draws_` 里判；
 // 这里只负责"把当前这套状态送走"。
    gl_api::bind_texture(GL_TEXTURE_2D,
                         (pending_tex_ != 0) ? pending_tex_ : white_texture_);
    if (cs != nullptr) {
 // ---- 自定义着色器：一套自己的 uniform ----
        gl_api::uniform_matrix_3fv(cs->u_pixel_to_ndc, 1, 0, pixel_to_ndc_);
 // `v_uv` 归一化：**到这个四边形**（已定），不是到纹理。
        gl_api::uniform_2f(cs->u_tex_inv_size, pending_inv_size_[0],
                           pending_inv_size_[1]);
        if (cs->u_time >= 0) {
            gl_api::uniform_1f(cs->u_time,
                               static_cast<gl_api::float_t>(logic_time()));
        }
        if (cs->u_resolution >= 0) {
            gl_api::uniform_2f(cs->u_resolution, frame_pixel_[0], frame_pixel_[1]);
        }
        if (cs->u_box >= 0) {
            gl_api::uniform_4f(cs->u_box, frame_box_[0], frame_box_[1],
                               frame_box_[2], frame_box_[3]);
        }
        if (cs->u_params >= 0) {
 // 【为什么用 4 个 float 的数组 uniform】`u_params[4]`。
 // 【ABI 是 vec4 而不是 float[4]】GL 的标量数组 uniform 也能喂，
 // 但 `vec4` 让作者写 `u_params.x` 比 `u_params[0]` 更顺手，且少一个入口。
            gl_api::uniform_4f(cs->u_params, pending_params_[0], pending_params_[1],
                               pending_params_[2], pending_params_[3]);
        }
        if (cs->u_tex_size >= 0) {
 // 纹理像素尺寸（由 inv_size 反推；没有纹理时是 1×1）
            gl_api::uniform_2f(
                cs->u_tex_size,
                (pending_inv_size_[0] > 0.0f) ? (1.0f / pending_inv_size_[0]) : 1.0f,
                (pending_inv_size_[1] > 0.0f) ? (1.0f / pending_inv_size_[1]) : 1.0f);
        }
    } else {
        gl_api::uniform_matrix_3fv(uniform_pixel_to_ndc_[vi], 1,
                                   0 /* GL_FALSE：矩阵已是列主序，不转置 */,
                                   pixel_to_ndc_);
 // 【UV 归一化的那一半】顶点的 `u,v` 是像素 ⇒ 这里把"尺寸的倒数"送上去，
 // 顶点着色器做乘法。详见文件上方"UV 的单位链"那段。
        if (uniform_tex_inv_size_[vi] >= 0) {
            gl_api::uniform_2f(uniform_tex_inv_size_[vi], pending_inv_size_[0],
                               pending_inv_size_[1]);
        }
    }
 // 【混合状态 = 批状态的一员（2026-10-18 落地）】
 // 触发②（混合变化就 flush）在 `sort_and_emit_draws_` 里判；
 // 这里只负责"把当前这套状态送走"。
 // ⚠️ 每次 flush 都重设一遍（不做"变了才设"的缓存）：GL 状态的显式重设是
 // 几纳秒的事，而"缓存失效时忘了重设"是静默错（整帧用错混合）。
 // ⇒ 这条选择与"采样器 clamp 每张纹理都显式设"同一条理由。
    apply_blend_state_(pending_blend_);
 // 【雾 = 批状态的一员（2026-10-18 落地）】
 // 启用位决定用哪个程序（已在上面 vi 里体现）；其余四个参数走 uniform。
 // ⚠️ **只在有雾变体里设**：无雾变体没有这三个 uniform（location = -1），
 // 对 -1 调 glUniform* 是 GL 的 "no-op 但产生 INVALID_OPERATION"
 // —— 不致命但会污染错误状态，且掩盖真正的错误。
    if (cs == nullptr && pending_fog_.enabled) {
        gl_api::uniform_3f(uniform_fog_color_[vi], pending_fog_.color[0],
                           pending_fog_.color[1], pending_fog_.color[2]);
        gl_api::uniform_2f(uniform_fog_range_[vi],
                           static_cast<gl_api::float_t>(pending_fog_.near_z),
                           static_cast<gl_api::float_t>(pending_fog_.far_z));
        gl_api::uniform_1f(uniform_fog_strength_[vi],
                           static_cast<gl_api::float_t>(pending_fog_.strength));
    }

 // 顶点与索引整包上传（GL_STREAM_DRAW 的语义即"每次写入、draw 几次就丢"）。
 // 【纹理已落地（2026-10-18）】每次 flush 绑的是 `pending_tex_`（白纹理兜底）；
 // "页"这一维随**纹理数组**那批接入（那时 `u_texture` 会变成数组采样器）。
    gl_api::bind_buffer(gl_api::array_buffer, vbo_);
    gl_api::buffer_data(gl_api::array_buffer,
                        static_cast<gl_api::sizeiptr_t>(batch_vertex_count_ *
                                                        sizeof(vertex)),
                        batch_buffer_, gl_api::stream_draw);
    gl_api::bind_buffer(gl_api::element_array_buffer, ibo_);
    gl_api::buffer_data(gl_api::element_array_buffer,
                        static_cast<gl_api::sizeiptr_t>(batch_index_count_ *
                                                        sizeof(std::uint16_t)),
                        batch_indices_, gl_api::stream_draw);
 // 【自定义着色器：这一批可能覆盖整帧（含信箱边）】
 // ⚠️ 只关 scissor 不够：**glViewport** 也被 begin_frame 设成了渲染盒，
 // 视口之外的片元直接被裁掉 ⇒ 实测症状是"只有盒子中间那块是红的，四角全黑"。
 // ⇒ 视口与 scissor 两者都必须临时改成整帧，画完立刻恢复，
 // 以保证后续的内建变体仍被裁剪到盒内。
    const bool full_frame_batch = (cs != nullptr);
    if (full_frame_batch) {
        glViewport(0, 0, static_cast<int>(frame_pixel_[0]),
                   static_cast<int>(frame_pixel_[1]));
        glDisable(GL_SCISSOR_TEST);
    }
    gl_api::draw_elements(GL_TRIANGLES, static_cast<gl_api::int_t>(batch_index_count_),
                          gl_api::unsigned_short, nullptr);
    if (full_frame_batch) {
        glViewport(static_cast<int>(frame_box_[0]), static_cast<int>(frame_box_[1]),
                   static_cast<int>(frame_box_[2]), static_cast<int>(frame_box_[3]));
        glEnable(GL_SCISSOR_TEST);
    }

    ++stat_submissions_;
    ++stat_flushes_;
    stat_vertices_ += batch_vertex_count_;

    batch_vertex_count_ = 0;
    batch_index_count_ = 0;
}

renderer::draw_stats renderer::take_draw_stats() noexcept {
    draw_stats out;
    out.submissions = stat_submissions_;
    out.flushes = stat_flushes_;
    out.vertices = stat_vertices_;
    out.primitives = stat_primitives_;
    out.captures = stat_captures_;
    out.capture_pixels = stat_capture_pixels_;
    stat_submissions_ = 0;
    stat_flushes_ = 0;
    stat_vertices_ = 0;
    stat_primitives_ = 0;
    stat_captures_ = 0;
    stat_capture_pixels_ = 0;
    return out;
}

// =============================================================================
// 快照画布（staged）—— 阶段 10 的真身（快照捕获与网格绘制）
// =============================================================================

void renderer::ensure_staged_snapshot_(int fb_w, int fb_h) {
    if (staged_fbo_ != 0 && staged_w_ == fb_w && staged_h_ == fb_h) {
        return;
    }

 // ---- 尺寸变化（或首次）⇒ 重建：旧纹理/FBO 销毁，快照内容视为清空 ----
    if (staged_fbo_ != 0) {
        gl_api::delete_framebuffers(1, &staged_fbo_);
        staged_fbo_ = 0;
    }
    if (staged_color_tex_ != 0) {
        gl_api::delete_textures(1, &staged_color_tex_);
        staged_color_tex_ = 0;
    }

 // 【】快照画布的 GL 失败要报 L3，不能留下“staged_valid_ 为真但内容是黑”的状态。
 // 先清掉之前遗留的错误，避免把别的调用的错误算到这次建纹理/FBO 头上。
    while (gl_api::get_error() != gl_api::no_error) {
    }

 // ① 颜色附件纹理（RGBA8；可采样的是它，不是 FBO）
    gl_api::gen_textures(1, &staged_color_tex_);
    if (staged_color_tex_ == 0) {
        fail_internal("staged 快照：glGenTextures 返回 0");
    }
    gl_api::bind_texture(GL_TEXTURE_2D, staged_color_tex_);
    gl_api::tex_image_2d(GL_TEXTURE_2D, 0, GL_RGBA, fb_w, fb_h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, nullptr);
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
 // 快照纹理也要 clamp：它会被"画冻结画面"采样，而缺省 REPEAT 会在边缘绕回。
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            static_cast<gl_api::int_t>(gl_api::clamp_to_edge));
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            static_cast<gl_api::int_t>(gl_api::clamp_to_edge));

 // ② FBO 挂上并检查完整性 —— 不顺 ⇒ 内部不变量被破坏（引擎/环境问题）
    gl_api::gen_framebuffers(1, &staged_fbo_);
    if (staged_fbo_ == 0) {
        fail_internal("staged 快照：glGenFramebuffers 返回 0");
    }
    gl_api::bind_framebuffer(gl_api::framebuffer, staged_fbo_);
    gl_api::framebuffer_texture_2d(gl_api::framebuffer, gl_api::color_attachment0,
                                   GL_TEXTURE_2D, staged_color_tex_, 0);
    if (gl_api::check_framebuffer_status(gl_api::framebuffer) !=
        gl_api::framebuffer_complete) {
        gl_api::bind_framebuffer(gl_api::framebuffer, 0);
        fail_internal("staged 快照 FBO 不完整（尺寸 = fb 像素尺寸）");
    }
    gl_api::bind_framebuffer(gl_api::framebuffer, 0);

    const gl_api::enum_t build_err = gl_api::get_error();
    if (build_err != gl_api::no_error) {
        fail_snapshot_gl("staged 快照的纹理/FBO 创建", build_err);
    }

    staged_w_ = fb_w;
    staged_h_ = fb_h;
    staged_valid_ = false; // 重建即清空 ⇒ 恢复后第一帧重拍
}

void renderer::capture_staged_snapshot() {
#if defined(WAG_HAVE_SDL3)
    if (output_w_ <= 0 || output_h_ <= 0 || !ctx_.is_valid()) {
        return;
    }
    ensure_staged_snapshot_(output_w_, output_h_);

 // ---- 默认帧缓冲（最终输出）→ staged 快照：1:1 blit ----
 // 【为什么是输出而不是内部目标】快照语义是“玩家最终看到的画面”，
 // 包含 present 的 letterbox / 缩放结果。
    while (gl_api::get_error() != gl_api::no_error) {
    }
    glDisable(GL_SCISSOR_TEST);
    gl_api::bind_framebuffer(gl_api::read_framebuffer, 0);
    gl_api::bind_framebuffer(gl_api::draw_framebuffer, staged_fbo_);
    gl_api::blit_framebuffer(0, 0, output_w_, output_h_, 0, 0, output_w_,
                             output_h_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl_api::bind_framebuffer(gl_api::read_framebuffer, 0);
    gl_api::bind_framebuffer(gl_api::draw_framebuffer, 0);
    glEnable(GL_SCISSOR_TEST);

    const gl_api::enum_t blit_err = gl_api::get_error();
    if (blit_err != gl_api::no_error) {
        staged_valid_ = false;
        fail_snapshot_gl("capture_staged_snapshot 的 blit", blit_err);
    }

    staged_valid_ = true;
#endif
}

// =============================================================================
// 任意网格绘制（快照捕获与网格绘制）
// =============================================================================

void renderer::draw_mesh(std::uint64_t image, std::int64_t layer, double z,
                         const vertex* verts, std::size_t count) {
 // 【拒绝自定义着色器】draw_mesh 逐三角形走世界变换与 UV 钳制，
 // 与着色器的"整帧像素"语义不一致 ⇒ 直接报错，不静默画错（D4）。
    reject_shader_state_("draw_mesh");

 // ---- ⓪ z 必须是有限实数（L3；标量参数，先查它最便宜）----
    if (!std::isfinite(z)) {
        fail_z_not_finite("draw_mesh 的 z", z);
    }

 // ---- ① 图片句柄 → (纹理, 像素尺寸) ----
 // 【为什么 draw_mesh 也必须有 image】UV 归一化要除以纹理尺寸；
 // 没有 image 就无法归一化 ⇒ 网格贴不了图（而"给图片做非仿射变形"
 // 正是这个入口的主要用途）。`image == 0` = 白纹理 + 1×1。
    gl_api::uint_t draw_tex = 0;
    double tex_w = 1.0;
    double tex_h = 1.0;
    if (image != 0 && !image_texture_(image, draw_tex, tex_w, tex_h)) {
        const std::string desc = image_desc_(image);
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "draw_mesh 的 image";
        e.got = desc;
        e.want = "一个**有效**的图片句柄（0 = 白纹理）；"
                 "失效通常是它被 Unload 或被同名覆盖过";
        fail_boundary(e);
    }

 // ---- ① 上限：必须在读任何数据之前（超限报错，不静默截断）----
    if (count > mesh_vertex_limit) {
        char got_buf[32];
        std::snprintf(got_buf, sizeof(got_buf), "%zu", count);
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "draw_mesh 的顶点个数";
        e.got = got_buf;
        e.want = "<= 65535（mesh_vertex_limit = 2^16 - 1，顶点计数落在 u16 内）";
        fail_boundary(e);
    }

 // ---- ② 退化输入：不足 3 顶点围不成三角形 ⇒ 静默（与 draw_vertices 同判据）
    if (count < 3) {
        return;
    }

 // ---- ③ 空指针：数据矛盾 ----
    if (verts == nullptr) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "draw_mesh 的顶点数组";
        e.got = "nullptr";
        e.want = "非空（顶点个数 >= 3 时必须指向可读的顶点数据）";
        fail_boundary(e);
    }

 // ---- ④ 整批有限值校验（复用同一份实现 —— 校验逻辑只有一份）----
    vertex_defect d;
    if (find_vertex_defect(verts, count, d)) {
        char where_buf[96];
        std::snprintf(where_buf, sizeof(where_buf), "第 %zu 个顶点的 %c",
                      d.index + 1, d.field);
        char got_buf[64];
        std::snprintf(got_buf, sizeof(got_buf), "%g", d.value);
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = where_buf;
        e.got = got_buf;
        e.want = "有限实数（NaN 与无穷不是引擎能解释的坐标）";
        fail_boundary(e);
    }

 // ---- ⑤ 收集：按三角形列表变换后进帧顶点池（排序与发射在 end_frame）----
 // 【事务边界 = 一次调用】校验段已扫全量 ⇒ 收集不可能失败；
 // 一次调用跨多次 flush 的行为在发射侧成立（校验在前、收集在后）。
 // 【末尾不足 3 个】静默忽略（退化输入同判据）。
 //
 // 【⚠️ 顶点是逻辑坐标 —— 必须过与 draw_vertices 同一段变换】
 // 首版实现直接把顶点 memcpy 进批缓冲（漏了变换），逻辑坐标被当成
 // 窗口像素 ⇒ 三角形画到屏幕外（实测症状：统计里顶点在、屏幕上无）。
 // 修正 = 收集时逐顶点变换（double 域算完才转 float，精度纪律同前）。
    const space::transform& tf = transform_;
    const std::size_t triangles = count / 3;
    for (std::size_t ti = 0; ti < triangles; ++ti) {
        vertex tri[3];
        for (std::size_t k = 0; k < 3; ++k) {
            const vertex& src = verts[ti * 3 + k];
            tri[k].x = static_cast<float>(tf.apply_x(src.x, src.y));
            tri[k].y = static_cast<float>(tf.apply_y(src.x, src.y));
            tri[k].z = src.z;   // z 透传：深度测试已关
            tri[k].u = src.u;   // UV 是**纹理像素单位**，透传（归一化在顶点着色器）
            tri[k].v = src.v;
            tri[k].color = src.color;

 // ---- UV 越界：容差内夹取，超出容差 ⇒ 报错 ----
            char bad_field = 0;
            double bad_value = 0.0;
            if (clamp_uv_or_defect(tri[k], tex_w, tex_h, bad_field, bad_value)) {
                char where_buf[112];
                std::snprintf(where_buf, sizeof(where_buf),
                              "draw_mesh 第 %zu 个顶点的 %c", ti * 3 + k + 1,
                              bad_field);
                char got_buf[128];
                std::snprintf(got_buf, sizeof(got_buf), "%g（纹理 %gx%g）",
                              bad_value, tex_w, tex_h);
                error_value e;
                e.cls = error_class::boundary_validation;
                e.where = where_buf;
                e.got = got_buf;
                e.want = "落在 [0, 纹理宽/高] 内的**像素**坐标";
                fail_boundary(e);
            }
        }
        collect_triangles_(tri, 3, layer, z, draw_tex,
                           (tex_w > 0.0) ? (1.0 / tex_w) : 1.0,
                           (tex_h > 0.0) ? (1.0 / tex_h) : 1.0,
                           0); // draw_mesh 不接受自定义着色器（见函数入口的拒绝）
    }
}

// =============================================================================
// 从快照取图（LoadImageFromSnapshot）
// =============================================================================

void renderer::warn_if_image_gen_too_high_(const char* name,
                                           std::uint32_t slot) const {
    if (slot >= images_.size()) {
        return;
    }
    const image_slot& s = images_[slot];
    if (s.gen < image_gen_container_limit) {
        return;
    }
    log::warn("draw", log::format(
        "图片槽位代数已达容器 u32 上限（container.img 无法引用）",
        std::string(name != nullptr ? name : s.name.c_str()) +
            " | slot=" + std::to_string(slot) +
            " gen=" + std::to_string(s.gen) +
            "；重启编辑器/引擎可归零；draw_image / draw_vertices 不受影响"));
}

void renderer::unload_image_slot_(std::uint32_t slot) {
    image_slot& s = images_[slot];
 // 【2026-10-18 已定：纹理**不在这一刻销毁**，只摘下来放进挂起队列】
 // 原因见 renderer.hpp 的 `pending_destroy_tex_`：
 // 本帧已收集的 draw_item 还持着这个纹理名，而 glBindTexture 要到帧末才发生。
 // 立刻删 ⇒ 帧末绑到已删除的名字 ⇒ 静默黑图（且没有任何报错）。
 // ⚠️ 槽位照旧**立刻**回 free-list —— 槽位复用与纹理存活是两件事，
 // 把槽位也拖到帧末会让"覆盖 = 同槽位 + 代数 +1"这条失效。
    if (s.tex != 0) {
        pending_destroy_tex_.push_back(s.tex);
        s.tex = 0;
    }
    image_by_name_.erase(s.name);
 // 【名字不在这里清空】 的错误在下一次发射才发生，那时必须还能报出图名。
 // 清空会让 "unload 后容器发射" 只能报 handle。
 // ⚠️ 槽位若被新图复用，s.name 会变成新名字；image_desc_ 会注明“槽位可能已复用”。
    s.gen += 1; // 唯一的失效规则：代数不匹配 ⇒ 旧句柄自动失效
    s.width = 0;
    s.height = 0;
 // 【 / 】帧表与锚点是"这张图"的属性；槽位复用必须清零，
 // 否则新图片会继承旧图片的帧数与锚点（image_frame_count / image_anchor 说谎）。
    s.frames.clear();
    s.ax = 0.0f;
    s.ay = 0.0f;
    image_free_.push_back(slot);
}



void renderer::update_image_watermark_() {
    const std::size_t n = image_by_name_.size();
    if (n >= image_watermark) {
        if (!image_watermark_hit_) {
            image_watermark_hit_ = true;
            log::record("资源", log::format("图片族达 80% 水位",
                std::to_string(n) + " / " +
                    std::to_string(image_family_capacity)));
        }
    } else {
        image_watermark_hit_ = false;
    }
}

void renderer::unload_image(const char* name) {
#if defined(WAG_HAVE_SDL3)
    if (name == nullptr) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "unload_image 的 name";
        e.got = "nullptr";
        e.want = "图片名";
        fail_boundary(e);
    }
    const auto it = image_by_name_.find(name);
    if (it == image_by_name_.end()) {
 // 查询类语义：不存在不算错误（"卸载一个没有的东西"是幂等操作）
        return;
    }
    unload_image_slot_(it->second);
    update_image_watermark_();
 // 【】这里**不扫描容器**：按，旧容器在“下一次发射”时才会暴露——
 // image_texture_ 发现代数不匹配 ⇒ L3。错误信息带图名（image_desc_）；
 // “引用容器数”诊断暂未实现。
#else
    (void)name;
#endif
}

bool renderer::lookup_image_handle(const char* name,
                                   std::uint64_t& out_handle) const {
    if (name == nullptr) {
        return false;
    }
    const auto it = image_by_name_.find(name);
    if (it == image_by_name_.end()) {
        return false;
    }
    const image_slot& s = images_[it->second];
    out_handle = (static_cast<std::uint64_t>(s.gen) << 21) |
                 static_cast<std::uint64_t>(it->second);
    return true;
}

bool renderer::image_texture_(std::uint64_t handle, gl_api::uint_t& tex,
                              double& pixel_w, double& pixel_h) const {
 // 句柄布局与 lookup_image_handle 同源：低 21 位槽位、高 43 位代数。
    const std::uint32_t slot = static_cast<std::uint32_t>(handle & 0x1FFFFFu);
    const std::uint32_t gen = static_cast<std::uint32_t>(handle >> 21);
    if (slot >= images_.size()) {
        return false;
    }
    const image_slot& s = images_[slot];
 // 【三件事一起判】空槽（gen==0）/ 代数不匹配（被覆盖或卸载过）/ 纹理已销毁。
 // 任何一个成立 ⇒ 这是"使用失效资源句柄" ⇒ 调用方必须**报错**，不许静默不画。
    if (s.gen == 0 || s.gen != gen || s.tex == 0) {
        return false;
    }
    tex = s.tex;
    pixel_w = static_cast<double>(s.width);
    pixel_h = static_cast<double>(s.height);
    return true;
}

std::uint64_t renderer::load_image_from_file(const char* name, const char* path) {
#if defined(WAG_HAVE_SDL3)
    const auto load_t0 = std::chrono::steady_clock::now();
 // ---- ① 参数 ----
    if (name == nullptr || *name == '\0' || path == nullptr || *path == '\0') {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "load_image_from_file 的 name / path";
        e.got = (name == nullptr || *name == '\0') ? "name 为空" : "path 为空";
        e.want = "非空图片名 + 非空路径（路径由脚本侧按安装根拼好）";
        fail_boundary(e);
    }

 // ---- ② 解码：用 **SDL3 核心自带的 SDL_LoadPNG**（零新依赖）----
 // 先按扩展名选：.png ⇒ SDL_LoadPNG，.bmp ⇒ SDL_LoadBMP。
 // ⚠️ 不做"猜格式"（SDL 的两个加载器各自只认一种），也不静默回退另一个
 // —— 猜错会把"文件损坏"伪装成"格式不支持"，那一类最难查。
    SDL_Surface* raw = nullptr;
    {
        const char* dot = std::strrchr(path, '.');
        const bool is_png = (dot != nullptr) &&
                            (std::strcmp(dot, ".png") == 0 || std::strcmp(dot, ".PNG") == 0);
        const bool is_bmp = (dot != nullptr) &&
                            (std::strcmp(dot, ".bmp") == 0 || std::strcmp(dot, ".BMP") == 0);
        if (is_png) {
            raw = SDL_LoadPNG(path);
        } else if (is_bmp) {
            raw = SDL_LoadBMP(path);
        } else {
            error_value e;
            e.cls = error_class::resource_failure;
            e.where = "load_image_from_file 的格式";
            e.got = (dot != nullptr) ? dot : "（无扩展名）";
            e.want = "第一版只解 .png / .bmp（SDL3 核心自带的那两个加载器）";
            fail_boundary(e);
        }
    }
    if (raw == nullptr) {
        error_value e;
        e.cls = error_class::resource_failure;
        e.where = "load_image_from_file 的解码";
        e.got = SDL_GetError();
        e.want = "一个可读的 PNG / BMP 文件";
        e.sentence = "图片解码失败（引擎必需件缺失？）";
        fail_boundary(e);
    }

 // ---- ③ 统一成 RGBA8（内存序 R,G,B,A —— 正是 GL_RGBA/GL_UNSIGNED_BYTE）----
    SDL_Surface* rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    if (rgba == nullptr) {
        error_value e;
        e.cls = error_class::resource_failure;
        e.where = "load_image_from_file 的格式转换";
        e.got = SDL_GetError();
        e.want = "能转成 RGBA32 的表面";
        fail_boundary(e);
    }

    const int px_w = rgba->w;
    const int px_h = rgba->h;
    const int pitch = rgba->pitch;
    if (px_w <= 0 || px_h <= 0 || rgba->pixels == nullptr) {
        SDL_DestroySurface(rgba);
        error_value e;
        e.cls = error_class::resource_failure;
        e.where = "load_image_from_file 的尺寸";
        e.got = "宽或高为 0";
        e.want = "> 0";
        fail_boundary(e);
    }

 // ---- ④ 槽位（同族同名 ⇒ 覆盖 = 卸载 + 加载）----
    std::uint32_t slot = 0;
    {
        const auto it = image_by_name_.find(name);
        if (it != image_by_name_.end()) {
            slot = it->second;
            unload_image_slot_(slot);
 // unload 已把槽位推回 free-list ⇒ 立刻取回同一槽位：
 // "覆盖"的语义是**同槽位 + 代数 +1**，不是换槽位。
            image_free_.pop_back();
        } else if (!image_free_.empty()) {
            slot = image_free_.back();
            image_free_.pop_back();
        } else {
            if (images_.size() >= image_family_capacity) {
                SDL_DestroySurface(rgba);
                error_value e;
                e.cls = error_class::resource_failure;
                e.where = "load_image_from_file 的图片槽位";
                e.got = "图片族的槽位已满";
                e.want = "先 Unload 一些图片（容量见 image_family_capacity）";
                fail_boundary(e);
            }
            slot = static_cast<std::uint32_t>(images_.size());
            images_.push_back(image_slot{});
        }
    }

 // ---- ⑤ 建纹理并上传（独立纹理 ⇒ 逐图片过滤天然成立）----
 // 【2026-11-10 】纹理创建/上传失败 ⇒ 加载时 L3。
 // 不允许"名字已登记、句柄已返回，但纹理是 0/不完整"的半死资源。
 // 先清掉之前遗留的 GL 错误，避免把别的调用的错误算到这次上传头上。
    while (gl_api::get_error() != gl_api::no_error) {
    }
    gl_api::uint_t tex = 0;
    gl_api::gen_textures(1, &tex);
    if (tex == 0) {
        SDL_DestroySurface(rgba);
        error_value e;
        e.cls = error_class::resource_failure;
        e.where = "load_image_from_file 的纹理创建";
        e.got = "glGenTextures 返回 0";
        e.want = "一个可用的纹理名（显存/驱动正常）";
        e.sentence = "图片纹理创建失败（显卡/驱动无法分配纹理）";
        fail_boundary(e);
    }
    gl_api::bind_texture(GL_TEXTURE_2D, tex);
 // 【过滤 = nearest】全局默认是最近邻（像素风/锐利）；照片那种才用 linear。
 // 【MIN_FILTER 必须显式设】否则默认 NEAREST_MIPMAP_LINEAR + 没有 mipmap
 // ⇒ 纹理"不完整" ⇒ 采样为黑。
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
 // 【WRAP 必须显式 clamp】缺省 REPEAT 会在图集上绕回别处（静默错图）。
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            static_cast<gl_api::int_t>(gl_api::clamp_to_edge));
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            static_cast<gl_api::int_t>(gl_api::clamp_to_edge));

 // 【行距 ≠ w*4 时靠 UNPACK_ROW_LENGTH 处理】不重排像素（那要一次整块拷贝）。
    const int row_pixels = (pitch > 0) ? (pitch / 4) : px_w;
    if (row_pixels != px_w) {
        gl_api::pixel_store_i(GL_UNPACK_ROW_LENGTH, row_pixels);
    }
    gl_api::tex_image_2d(GL_TEXTURE_2D, 0, GL_RGBA8, px_w, px_h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, rgba->pixels);
    if (row_pixels != px_w) {
        gl_api::pixel_store_i(GL_UNPACK_ROW_LENGTH, 0);
    }
    gl_api::bind_texture(GL_TEXTURE_2D, 0);

 // 【上传后的错误检查】glTexImage2D 失败（常见是 GL_OUT_OF_MEMORY）
 // 不会改变 tex 名，但纹理内容不完整 ⇒ 必须在这里终止，不能登记名字。
    const gl_api::enum_t upload_err = gl_api::get_error();
    if (upload_err != gl_api::no_error) {
        gl_api::delete_textures(1, &tex);
        SDL_DestroySurface(rgba);
        char got_buf[64];
        std::snprintf(got_buf, sizeof(got_buf), "GL error 0x%X",
                      static_cast<unsigned>(upload_err));
        error_value e;
        e.cls = error_class::resource_failure;
        e.where = "load_image_from_file 的纹理上传";
        e.got = got_buf;
        e.want = "glTexImage2D 成功（显存/驱动正常）";
        e.sentence = "图片纹理上传失败（常见原因是显存不足；见 GL 错误码）";
        fail_boundary(e);
    }

    SDL_DestroySurface(rgba);

 // ---- ⑥ 落槽 + 名字表 ----
    image_slot& s = images_[slot];
 // 【 / 】复用槽位：先清掉上一张图的帧表与锚点。
    s.frames.clear();
    s.ax = 0.0f;
    s.ay = 0.0f;
    s.tex = tex;
    s.width = static_cast<std::uint32_t>(px_w);
    s.height = static_cast<std::uint32_t>(px_h);
    s.name = name;
 // 【代数：只在"从未用过"时置 1】回收槽位的 gen 已经在 unload 里 +1 过了。
 // ⚠️ 千万不要写成"if (没复用) gen = 1"：从 free-list 取到的槽位也是
 // "没复用"的，那样会把 gen 重置回 1 ⇒ **半死的旧句柄被复活**
 // （旧句柄正是 gen==1 那一代）。判据只认"槽位从未用过"：gen == 0。
    if (s.gen == 0) {
        s.gen = 1;
    }
    image_by_name_[name] = slot;
    warn_if_image_gen_too_high_(name, slot);
    update_image_watermark_();
    log_load_time_ms(
        "图片",
        name,
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - load_t0)
            .count());

    return (static_cast<std::uint64_t>(s.gen) << 21) |
           static_cast<std::uint64_t>(slot);
#else
    (void)name;
    (void)path;
    return 0;
#endif
}

std::uint64_t renderer::load_image_from_snapshot(const char* name,
                                                 double x, double y,
                                                 double w, double h) {
#if defined(WAG_HAVE_SDL3)
    const auto load_t0 = std::chrono::steady_clock::now();
 // ---- ① 参数与状态前置 ----
    if (name == nullptr || *name == '\0') {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "load_image_from_snapshot 的 name";
        e.got = (name == nullptr) ? "nullptr" : "空字符串";
        e.want = "非空图片名（落进图片族：覆盖=卸载+加载、可 Unload/FreeRes）";
        fail_boundary(e);
    }
    if (!staged_valid_) {
        error_value e;
        e.cls = error_class::resource_failure;
        e.where = "load_image_from_snapshot 的快照来源";
        e.got = "快照尚未生成";
        e.want = "至少跑过一帧的阶段 10（捕获读的是上一帧的快照）";
        fail_boundary(e);
    }
 // 【2026-11-10 】x/y/w/h 必须是有限实数；w/h > 0。
 // 非有限值不允许进入 llround（NaN/Inf 是域错误，结果不可判定）。
    if (!std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(w) || !std::isfinite(h) ||
        !(w > 0.0) || !(h > 0.0)) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "load_image_from_snapshot 的 x/y/w/h";
        e.got = "非有限值或宽高非正";
        e.want = "有限实数，且 w/h > 0";
        e.sentence = "快照取图矩形不是有限实数";
        fail_boundary(e);
    }

 // ---- ② 逻辑矩形 → 快照像素（用本帧变换；坑 1 的 y 翻转在 blit 里做）----
 // (x, y) = 矩形中心（与图片中心锚点同一惯例）；边缘按"最近整数像素"取整。
    const double cx = transform_.apply_x(x, y);
    const double cy = transform_.apply_y(x, y);
    const double hw = std::fabs(transform_.m00) * w * 0.5;
    const double hh = std::fabs(transform_.m11) * h * 0.5;

 // 【先在 double 域判界，再 llround】
 // 有限输入乘上变换仍可能溢出成 Inf；而 llround(Inf/NaN) 是域错误。
 // 先确认四条边都是有限值、且完全落在快照内，取整才是安全的。
    const double left_d = cx - hw;
    const double right_d = cx + hw;
    const double top_d = cy - hh;   // 窗口"距顶"小的一端
    const double bot_d = cy + hh;   // 窗口"距顶"大的一端
    if (!std::isfinite(left_d) || !std::isfinite(right_d) ||
        !std::isfinite(top_d) || !std::isfinite(bot_d) ||
        left_d < 0.0 || top_d < 0.0 ||
        right_d > static_cast<double>(staged_w_) ||
        bot_d > static_cast<double>(staged_h_) ||
        right_d <= left_d || bot_d <= top_d) {
        char detail[192];
        std::snprintf(detail, sizeof(detail),
                      "逻辑矩形(%g,%g,%g,%g) → 边缘[%g..%g]x[%g..%g]，"
                      "快照 %dx%d",
                      x, y, w, h, left_d, right_d, top_d, bot_d,
                      staged_w_, staged_h_);
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "load_image_from_snapshot 的取图矩形";
        e.got = detail;
        e.want = "完全落在快照范围内（不静默裁剪）";
        fail_boundary(e);
    }

    const long long x0 = std::llround(left_d);
    const long long x1 = std::llround(right_d);
    const long long wy_top = std::llround(top_d);
    const long long wy_bot = std::llround(bot_d);
    const long long src_y0 = static_cast<long long>(fb_h_) - wy_bot; // 帧缓冲距底
    const long long src_y1 = static_cast<long long>(fb_h_) - wy_top;

 // 【取整后仍要防"零宽/零高"】亚像素矩形可能取整到同一条边。
    if (x1 <= x0 || src_y1 <= src_y0) {
        char detail[160];
        std::snprintf(detail, sizeof(detail),
                      "逻辑矩形(%g,%g,%g,%g) → 像素[%lld..%lld]x[%lld..%lld]",
                      x, y, w, h, x0, x1, src_y0, src_y1);
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "load_image_from_snapshot 的取图矩形";
        e.got = detail;
        e.want = "取整后仍 > 0 像素（亚像素矩形无法取图）";
        fail_boundary(e);
    }
    const int px_w = static_cast<int>(x1 - x0);
    const int px_h = static_cast<int>(src_y1 - src_y0);

 // ---- ③ 覆盖 = 卸载 + 加载（同族同名 ⇒ 旧句柄自动失效）----
    const auto existing = image_by_name_.find(name);
    if (existing != image_by_name_.end()) {
        unload_image_slot_(existing->second);
    }

 // ---- ④ 分槽（free-list 优先，否则顺次）----
    std::uint32_t slot;
    if (!image_free_.empty()) {
        slot = image_free_.back();
        image_free_.pop_back();
    } else {
        if (images_.size() >= image_family_capacity) {
            error_value e;
            e.cls = error_class::resource_failure;
            e.where = "图片族容量";
            e.got = "已满";
            e.want = "有空闲槽位（卸载或 FreeRes 后再试）";
            fail_boundary(e);
        }
        slot = static_cast<std::uint32_t>(images_.size());
        images_.emplace_back();
        images_[slot].gen = 1;
    }
    image_slot& s = images_[slot];
 // 【 / 】复用槽位：先清掉上一张图的帧表与锚点。
    s.frames.clear();
    s.ax = 0.0f;
    s.ay = 0.0f;
    if (s.gen == 0) {
        s.gen = 1; // 复用槽的代数已在卸载时 +1；新槽从 1 起
    }

 // ---- ⑤ 目标纹理（RGBA8；照片默认 linear —— 陆-④ 的明确例外）----
    gl_api::gen_textures(1, &s.tex);
    if (s.tex == 0) {
 // 与文件图片一致：纹理创建失败 ⇒ L3，不留下"名字已登记但没有纹理"的半死资源。
        unload_image_slot_(slot);
        error_value e;
        e.cls = error_class::resource_failure;
        e.where = "load_image_from_snapshot 的纹理创建";
        e.got = "glGenTextures 返回 0";
        e.want = "一个可用的纹理名（显存/驱动正常）";
        e.sentence = "快照图片纹理创建失败（显卡/驱动无法分配纹理）";
        fail_boundary(e);
    }
    gl_api::bind_texture(GL_TEXTURE_2D, s.tex);
    gl_api::tex_image_2d(GL_TEXTURE_2D, 0, GL_RGBA, px_w, px_h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, nullptr);
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
 // 【WRAP 必须显式 clamp】照片是独立纹理，缺省 REPEAT 会在边缘绕回。
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            static_cast<gl_api::int_t>(gl_api::clamp_to_edge));
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            static_cast<gl_api::int_t>(gl_api::clamp_to_edge));

 // ---- ⑥ 临时 FBO + 拷贝（捕获不是渲染 —— 不绑目标"画"，只 blit）----
    gl_api::uint_t temp_fbo = 0;
    gl_api::gen_framebuffers(1, &temp_fbo);
    gl_api::bind_framebuffer(gl_api::framebuffer, temp_fbo);
    gl_api::framebuffer_texture_2d(gl_api::framebuffer, gl_api::color_attachment0,
                                   GL_TEXTURE_2D, s.tex, 0);
    if (gl_api::check_framebuffer_status(gl_api::framebuffer) !=
        gl_api::framebuffer_complete) {
        gl_api::bind_framebuffer(gl_api::framebuffer, 0);
        gl_api::delete_framebuffers(1, &temp_fbo);
 // 槽位回退走卸载路径（销毁纹理、代数 +1、归还 free-list、移出名字表）
 // —— 不手写第二份清理（两份清理迟早分歧）。
        unload_image_slot_(slot);
        fail_internal("捕获照片的临时 FBO 不完整");
    }

 // 【坑 1 · y 翻转必须在 blit 里做】目标 Y 交换（dstY0 = h、dstY1 = 0）：
 // 快照是 framebuffer 空间（原点左下），图片 UV 是左上原点 ⇒
 // 不交换 = 照片上下颠倒。这是本节最重要的一条。
 // 【坑 2 · 原样拷贝】不做混合、不受四个绘制状态影响（捕获不是绘制）。
 // 【关 scissor】blit 受 scissor 影响（同 capture_staged_snapshot 的说明）。
    glDisable(GL_SCISSOR_TEST);
    gl_api::bind_framebuffer(gl_api::read_framebuffer, staged_fbo_);
    gl_api::bind_framebuffer(gl_api::draw_framebuffer, temp_fbo);
    gl_api::blit_framebuffer(x0, static_cast<int>(src_y0), x0 + px_w,
                             static_cast<int>(src_y1), 0, px_h, px_w, 0,
                             GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl_api::bind_framebuffer(gl_api::read_framebuffer, 0);
    gl_api::bind_framebuffer(gl_api::draw_framebuffer, 0);
    glEnable(GL_SCISSOR_TEST);
    gl_api::delete_framebuffers(1, &temp_fbo); // 纹理留着 —— 它才是图片

 // ---- ⑦ 登记成图片资源（name 必需）----
    s.width = static_cast<std::uint32_t>(px_w);
    s.height = static_cast<std::uint32_t>(px_h);
    s.name = name;
    image_by_name_.emplace(name, slot);
    warn_if_image_gen_too_high_(name, slot);
    update_image_watermark_();
    log_load_time_ms(
        "快照图片",
        name,
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - load_t0)
            .count());

    ++stat_captures_;
    stat_capture_pixels_ += static_cast<std::uint64_t>(px_w) *
                            static_cast<std::uint64_t>(px_h);

    return (static_cast<std::uint64_t>(s.gen) << 21) |
           static_cast<std::uint64_t>(slot);
#else
    (void)name;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    return 0;
#endif
}

#else // !WAG_HAVE_SDL3

// 【无依赖档】空实现。理由见本段顶部的说明；形参显式忽略
// （两档零警告的纪律，与 begin_frame 的 #else 分支同一条）。
// ⚠️ flush_batch_ / capture_staged_snapshot / collect_* / sort_and_emit_
// 必须有这一支：draw_vertices / draw_mesh **不在**档位守卫里（核心 API，
// 逻辑上与 GL 无关），帧结构的 stage_snapshot 与相位门也无条件调用 ——
// 若漏了这支，无依赖档会在链接期缺符号。

void renderer::collect_primitive_(const vertex* gpu, std::size_t count,
                                  std::int64_t layer, double z,
                                  gl_api::uint_t tex, double inv_w, double inv_h,
                                  gl_api::uint_t shader_program) {
    (void)gpu;
    (void)count;
    (void)layer;
    (void)z;
    (void)tex;
    (void)inv_w;
    (void)inv_h;
    (void)shader_program;
}

void renderer::collect_triangles_(const vertex* gpu, std::size_t count,
                                  std::int64_t layer, double z,
                                  gl_api::uint_t tex, double inv_w, double inv_h,
                                  gl_api::uint_t shader_program) {
    (void)gpu;
    (void)count;
    (void)layer;
    (void)z;
    (void)tex;
    (void)inv_w;
    (void)inv_h;
    (void)shader_program;
}

void renderer::sort_and_emit_draws_() {}

// 【无依赖档的 image_texture_】draw_vertices / draw_mesh **不在档位守卫里**，
// 它们会调这个助手 ⇒ 必须有这一支，否则链接期缺符号。
// 这一档没有图片族（load_image_from_file / from_snapshot 都返回 0）
// ⇒ 任何非 0 句柄都"查不到"；调用方据此报错（本档本来不渲染，走不到）。
bool renderer::image_texture_(std::uint64_t handle, gl_api::uint_t& tex,
                              double& pixel_w, double& pixel_h) const {
    (void)handle;
    (void)tex;
    (void)pixel_w;
    (void)pixel_h;
    return false;
}

void renderer::flush_batch_() {}

void renderer::capture_staged_snapshot() {}

renderer::draw_stats renderer::take_draw_stats() noexcept {
    return {};
}

#endif // WAG_HAVE_SDL3

void renderer::flush_pending_destroy_tex_() {
 // 【它为什么在档位守卫之外】`end_frame()` 无条件调用它（帧结构不知道档位），
 // 而无依赖档也要把挂起列表清空 ⇒ 两档都要有定义。
 // 真正要 GL 的那一段在下面的 #if 里。
#if defined(WAG_HAVE_SDL3)
    if (pending_destroy_tex_.empty()) {
        return;
    }
    const std::size_t n = pending_destroy_tex_.size();
    for (gl_api::uint_t tex : pending_destroy_tex_) {
        gl_api::delete_textures(1, &tex);
    }
    pending_destroy_tex_.clear();
 // 【为什么打日志】"卸载在帧末生效"这条纪律若被谁改回"立刻删"，
 // 症状是静默黑图；而这条记录至少证明销毁**确实发生过**、发生了几张。
    char detail[64];
    static_cast<void>(std::snprintf(detail, sizeof(detail),
                                    "帧末销毁 %zu 张纹理（卸载延迟生效）", n));
    log::record("draw", log::format("图片销毁", detail));
#else
    pending_destroy_tex_.clear();
#endif
}

void renderer::flush_retired_shaders_() {
 // 【调用点】end_frame() 在 flush_batch_() 之后 —— 本帧已收集的绘制项
 // 都已经发射完，退役 program 才能安全删除。
#if defined(WAG_HAVE_SDL3)
    if (retired_shaders_.empty()) {
        return;
    }
    const std::size_t n = retired_shaders_.size();
    for (custom_shader& s : retired_shaders_) {
        if (s.program != 0) {
            gl_api::delete_program(s.program);
            s.program = 0;
        }
    }
    retired_shaders_.clear();
    retired_shader_warned_ = false;
    char detail[64];
    static_cast<void>(std::snprintf(detail, sizeof(detail),
                                    "帧末销毁 %zu 个着色器程序（卸载延迟生效）", n));
    log::record("draw", log::format("着色器销毁", detail));
#else
    retired_shaders_.clear();
#endif
}

std::string renderer::image_desc_(std::uint64_t handle) const {
    if (handle == 0) {
        return "handle=0（白纹理/无图）";
    }
    const std::uint32_t slot = static_cast<std::uint32_t>(handle & 0x1FFFFFu);
    const std::uint32_t gen = static_cast<std::uint32_t>(handle >> 21);
    std::string out = "handle=" + std::to_string(handle);
    if (slot >= images_.size()) {
        out += " 图名=<槽位越界>";
        return out;
    }
    const image_slot& s = images_[slot];
    if (s.gen == gen && !s.name.empty()) {
        out += " 图名=" + s.name;
    } else if (!s.name.empty()) {
        out += " 图名=" + s.name + "（槽位已失效/可能已被新图复用）";
    } else {
        out += " 图名=<该句柄已失效或名字已摘除>";
    }
    return out;
}

bool renderer::image_pixel_size(std::uint64_t handle, double& out_w,
                                double& out_h) const {
 // 【为什么这么短】判活只有一处实现（`image_texture_`）；这里只是"只要尺寸"。
 // 两档构建都能编：无依赖档的 `image_texture_` 是返回 false 的存根。
    gl_api::uint_t tex = 0;
    return image_texture_(handle, tex, out_w, out_h);
}

gl_api::uint_t renderer::resolve_draw_shader_() {
    if (!state_.shader_set || state_.shader == 0) {
        return 0;
    }
    const custom_shader* cs = find_shader_(state_.shader);
    if (cs == nullptr) {
        char got_buf[64];
        std::snprintf(got_buf, sizeof(got_buf), "handle=%u",
                      static_cast<unsigned>(state_.shader));
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "绘制状态里的自定义着色器句柄";
        e.got = got_buf;
        e.want = "一个仍有效的 load_shader 句柄（0 = 内建）；"
                 "卸载或同名覆盖后旧句柄失效";
        e.sentence = "使用失效的着色器句柄";
        fail_boundary(e);
    }
    return cs->program;
}

void renderer::reject_shader_state_(const char* what) const {
    if (!state_.shader_set || state_.shader == 0) {
        return;
    }
    char got_buf[64];
    std::snprintf(got_buf, sizeof(got_buf), "shader=%u",
                  static_cast<unsigned>(state_.shader));
    error_value e;
    e.cls = error_class::boundary_validation;
    e.where = what;
    e.got = got_buf;
    e.want = "先 set_state{shader=0}，或用 draw_vertices（像素坐标）";
    e.sentence = "该绘制入口不接受自定义着色器状态";
    fail_boundary(e);
}

void renderer::draw_image(std::uint64_t image, std::int64_t layer, double z,
                          double x, double y, double w, double h,
                          const source_rect* src) {
 // =====================================================================
 // 纯位置版：源是整张图，不使用 UV（UV 从图片尺寸推出来）
 //
 // 【为什么它是"薄壳"】四个入口里它是唯一能把全部校验交给别人的：
 // 它只是把 5 个数字拼成 4 个顶点，然后**原样调用 draw_vertices**
 // ⇒ 有限值校验、UV 越界校验、逻辑→窗口变换、收集、排序键，全部同一份实现。
 // 若在这里另写一遍，立刻就有两份校验要同步 —— 那正是本项目反复消掉的东西。
 // =====================================================================

 // 【拒绝逻辑坐标 + 自定义着色器的组合】。
    reject_shader_state_("draw_image");

 // ---- ① z：绘序键必须是有限实数（L3；理由见 fail_z_not_finite）----
    if (!std::isfinite(z)) {
        fail_z_not_finite("draw_image 的 z", z);
    }

 // ---- ② 位置与尺寸：标量参数逐个查（不是数组，所以不走 find_vertex_defect）----
 // ⚠️ 只查"有限"，**不查正负**：
 // · w/h 为负 = 镜像（没有面剔除，反向绕序照样画）
 // · w/h 为 0 = 零面积 = 什么也不出
 // 两者都属"看起来没用"，不是数据矛盾（引擎拒绝的是后者）。
    {
        struct { const char* name; double value; } fields[] = {
            {"x", x}, {"y", y}, {"w", w}, {"h", h},
        };
        for (const auto& f : fields) {
            if (!std::isfinite(f.value)) {
                error_value e;
                e.cls = error_class::boundary_validation;
                char where_buf[64];
                std::snprintf(where_buf, sizeof(where_buf),
                              "draw_image 的 %s", f.name);
                e.where = where_buf;
                e.got = std::isnan(f.value) ? "nan" : ((f.value > 0.0) ? "+inf" : "-inf");
                e.want = "有限实数（位置与尺寸；NaN / 无穷不是引擎能解释的坐标）";
                fail_boundary(e);
            }
        }

 // 【取图矩形同样只查有限】w/h 允许负数（镜像）、允许 0（零面积），
 // 与上面 w/h 的判据完全一致 —— 不在这里另立一套"必须为正"的规矩。
        if (src != nullptr) {
            struct { const char* name; double value; } rect_fields[] = {
                {"u", src->u}, {"v", src->v}, {"w", src->w}, {"h", src->h},
            };
            for (const auto& f : rect_fields) {
                if (!std::isfinite(f.value)) {
                    error_value e;
                    e.cls = error_class::boundary_validation;
                    char where_buf[80];
                    std::snprintf(where_buf, sizeof(where_buf),
                                  "draw_image 的取图矩形 %s", f.name);
                    e.where = where_buf;
                    e.got = std::isnan(f.value) ? "nan"
                                                : ((f.value > 0.0) ? "+inf" : "-inf");
                    e.want = "有限实数（纹理像素；NaN / 无穷不是一块图）";
                    fail_boundary(e);
                }
            }
        }
    }

 // ---- ③ 图片句柄 → 纹理尺寸（UV 的来源）----
 // 0 = 白纹理（1×1）：UV 给 (0,0)-(1,1)，采到的就是那一个白 texel。
 // 失效句柄 ⇒ 报错（"使用资源"；这里自己先查一次，是为了拿到 UV 用的尺寸）。
    gl_api::uint_t draw_tex = 0;
    double tex_w = 1.0;
    double tex_h = 1.0;
    if (image != 0 && !image_texture_(image, draw_tex, tex_w, tex_h)) {
        const std::string desc = image_desc_(image);
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "draw_image 的 image";
        e.got = desc;
        e.want = "一个**有效**的图片句柄（0 = 白纹理）；"
                 "失效通常是它被 Unload 或被同名覆盖过";
        fail_boundary(e);
    }

 // ---- ④ 锚点 → 左上角（：只有 draw_image 消费锚点）----
 // 【公式】left = x - ax·w，top = y + ay·h（+y 向上，矩形向下展开）。
 // ax=ay=0 ⇒ 与加锚点之前逐字节相同（默认值就是左上角）。
 // w/h 为负（镜像）时锚点位置不变，只改展开方向 —— 与 同一条判据。
    double ax = 0.0;
    double ay = 0.0;
    if (image != 0 && !image_anchor(image, ax, ay)) {
 // 句柄在上面的 image_texture_ 已判过活；这里失败只可能是 0 号白纹理。
        ax = 0.0;
        ay = 0.0;
    }
    const double left = x - ax * w;
    const double top = y + ay * h;

 // ---- ⑤ 拼 4 个顶点（扇形语义：按周边顺序 TL → TR → BR → BL）----
 // 【UV】默认整张图：左上 = (0,0)，右下 = (tex_w, tex_h)。
 // 给了取图矩形就用矩形那四边，**仍是像素单位**。
 // 越界不在这里判 —— 范围处理只有一处（`clamp_uv_or_defect`，
 // 与 draw_vertices / draw_mesh 共用）；在这里再查一遍就是两份。
 // 【颜色】白 = 不染色。逐对象染色走"顶点色模式"那条（尚未落地），
 // 而本入口没有颜色参数（扁平数字版）。
 // 【顶点里的 z】透传 0：深度测试已关，它不参与任何判定；
 // 真正决定顺序的是参数 `z`（排序键）。
    double u0 = 0.0;
    double v0 = 0.0;
    double u1 = tex_w;
    double v1 = tex_h;
    if (src != nullptr) {
        u0 = src->u;
        v0 = src->v;
        u1 = src->u + src->w;
        v1 = src->v + src->h;
    }
    const vertex quad[4] = {
        {static_cast<float>(left),     static_cast<float>(top),     0.0f,
         static_cast<float>(u0),       static_cast<float>(v0),      0xFFFFFFFFu},
        {static_cast<float>(left + w), static_cast<float>(top),     0.0f,
         static_cast<float>(u1),       static_cast<float>(v0),      0xFFFFFFFFu},
        {static_cast<float>(left + w), static_cast<float>(top - h), 0.0f,
         static_cast<float>(u1),       static_cast<float>(v1),      0xFFFFFFFFu},
        {static_cast<float>(left),     static_cast<float>(top - h), 0.0f,
         static_cast<float>(u0),       static_cast<float>(v1),      0xFFFFFFFFu},
    };

 // ---- ⑤ 交给 draw_vertices（校验 / 变换 / 收集全复用）----
    draw_vertices(image, layer, z, quad, 4);
}

void renderer::draw_image_scaled(std::uint64_t image, std::int64_t layer, double z,
                                 double x, double y, double sx, double sy,
                                 std::uint32_t rgba) {
 // 【拒绝逻辑坐标 + 自定义着色器的组合】。
    reject_shader_state_("draw_image_scaled");

 // ---- ① 图片尺寸（决定 UV 与默认大小）----
    gl_api::uint_t draw_tex = 0;
    double tex_w = 1.0;
    double tex_h = 1.0;
    if (image != 0 && !image_texture_(image, draw_tex, tex_w, tex_h)) {
        fail_internal("容器发射：图片不可用（" + image_desc_(image) +
                      "）。它已被卸载/覆盖，但容器还在引用它；"
                      "请先清掉这些容器，再卸载/覆盖图片");
    }


 // ---- ② 尺寸 = 图片像素 × sx/sy（与 draw_image 的 w/h 语义对齐）----
    const double w = tex_w * sx;
    const double h = tex_h * sy;

 // ---- ③ 四个顶点：锚点左上、+y 向上、UV 像素单位、颜色取结构体的 rgba ----
    const vertex quad[4] = {
        {static_cast<float>(x), static_cast<float>(y), 0.0f, 0.0f, 0.0f, rgba},
        {static_cast<float>(x + w), static_cast<float>(y), 0.0f,
         static_cast<float>(tex_w), 0.0f, rgba},
        {static_cast<float>(x + w), static_cast<float>(y - h), 0.0f,
         static_cast<float>(tex_w), static_cast<float>(tex_h), rgba},
        {static_cast<float>(x), static_cast<float>(y - h), 0.0f, 0.0f,
         static_cast<float>(tex_h), rgba},
    };

 // ---- ④ 交给 draw_vertices：校验/变换/收集/排序全部复用同一份实现 ----
    draw_vertices(image, layer, z, quad, 4);
}

void renderer::draw_container_quad(std::uint64_t image, std::uint32_t frame,
                                   std::int64_t layer, double z, double x,
                                   double y, double sx, double sy, double rot,
                                   std::uint32_t rgba, blend_state blend,
                                   vertex_color_mode vcolor, tex_alpha alpha) {
#if defined(WAG_HAVE_SDL3)
 // ---- ⓪ 有限值兜底（容器字段在 spawn/write 已校验；这里防越权写入）----
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(sx) ||
        !std::isfinite(sy) || !std::isfinite(rot) || !std::isfinite(z)) {
        fail_internal("容器发射：坐标/缩放/旋转/z 必须是有限实数");
    }

 // ---- ① 图片尺寸（决定 UV 与默认大小；img==0 由调用方跳过）----
    gl_api::uint_t draw_tex = 0;
    double tex_w = 1.0;
    double tex_h = 1.0;
    if (image == 0 || !image_texture_(image, draw_tex, tex_w, tex_h)) {
        fail_internal("容器发射：图片不可用（" + image_desc_(image) +
                      "）。它已被卸载/覆盖，但容器还在引用它；"
                      "请先清掉这些容器，再卸载/覆盖图片");
    }

 // ---- ⓪ 帧号 → 帧矩形（：frame 的合法集合是 [0, n)；越界 L3）----
 // 【n 从哪来】图片槽位的 frames 表；普通图片 n=1、frame 只能是 0。
 // 【谁消费】只有容器发射（draw_image 画整图，不查帧表）。
    frame_rect fr;
    const std::uint32_t frame_count = image_frame_count(image);
    if (!image_frame_rect(image, frame, fr)) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "draw_container_quad 的 frame";
        e.got = std::to_string(frame) + "（该图帧数 " +
                std::to_string(frame_count) + "）";
        e.want = "0 <= frame < " + std::to_string(frame_count) +
                 "（普通图片只有 0；带帧表的图片是 [0, n)）";
        e.sentence = "容器引用了不存在的帧";
        fail_boundary(e);
    }

 // ---- ② 尺寸与中心（：负 sx/sy = 镜像，只翻 UV，不改位置）----
 // 【尺寸取帧矩形，不取整张纹理】：容器画的是"帧"。
    const double w = static_cast<double>(fr.w) * std::fabs(sx);
    const double h = static_cast<double>(fr.h) * std::fabs(sy);
    const double cx = x + w * 0.5;
    const double cy = y - h * 0.5;
    const double cr = std::cos(rot);
    const double sr = std::sin(rot);

 // ---- ③ 四角（世界坐标，绕中心旋转；rot=0 时与 draw_image_scaled 一致）----
    const double ox[4] = {-w * 0.5, w * 0.5, w * 0.5, -w * 0.5};
    const double oy[4] = {h * 0.5, h * 0.5, -h * 0.5, -h * 0.5};

 // 【UV 取帧矩形，仍是纹理像素单位】镜像只交换两对边。
    double u0 = static_cast<double>(fr.x);
    double u1 = static_cast<double>(fr.x + fr.w);
    double v0 = static_cast<double>(fr.y);
    double v1 = static_cast<double>(fr.y + fr.h);
    if (sx < 0.0) {
        std::swap(u0, u1);
    }
    if (sy < 0.0) {
        std::swap(v0, v1);
    }
    const double uu[4] = {u0, u1, u1, u0};
    const double vv[4] = {v0, v0, v1, v1};

    vertex quad[4];
    for (int i = 0; i < 4; ++i) {
        const double wx = cx + ox[i] * cr - oy[i] * sr;
        const double wy = cy + ox[i] * sr + oy[i] * cr;
        quad[i].x = static_cast<float>(transform_.apply_x(wx, wy));
        quad[i].y = static_cast<float>(transform_.apply_y(wx, wy));
        quad[i].z = 0.0f;
        quad[i].u = static_cast<float>(uu[i]);
        quad[i].v = static_cast<float>(vv[i]);
        quad[i].color = rgba;
    }

 // ---- ④ 入帧顶点池 + 绘制项（source=0：容器来源）----
    const std::uint32_t begin =
        static_cast<std::uint32_t>(frame_vertices_.size());
    frame_vertices_.insert(frame_vertices_.end(), quad, quad + 4);

    draw_item item;
    item.stage = collect_stage_;
    item.source = 0; // 容器（脚本绘制是 1）
    item.topology = 0;
    item.layer = layer;
    item.z = z;
    item.tex = draw_tex;
    item.inv_size[0] = static_cast<gl_api::float_t>(1.0 / tex_w);
    item.inv_size[1] = static_cast<gl_api::float_t>(1.0 / tex_h);
    item.vertex_begin = begin;
    item.vertex_count = 4;
    item.index_count = 6;
    item.blend = blend;
    item.alpha = alpha;
    item.vcolor = vcolor;
    item.fog = fog_state{}; // 容器不吃雾
 // 容器发射走内建变体：不接受 shader 状态。
    item.shader_program = 0;
    item.sequence = ++creation_counter_;
    draw_items_.push_back(item);
    ++stat_primitives_;
#endif
}

void renderer::draw_vertices(std::uint64_t image, std::int64_t layer, double z,
                             const vertex* verts, std::size_t count) {
 // ---- ⓪ z 必须是有限实数（L3；标量参数，先查它最便宜）----
    if (!std::isfinite(z)) {
        fail_z_not_finite("draw_vertices 的 z", z);
    }

 // ---- ⓪b 自定义着色器句柄：解析并校验（失效 ⇒ L3，不静默回退）----
    const gl_api::uint_t shader_program = resolve_draw_shader_();

 // ---- 图片句柄 → (纹理, 像素尺寸) ----
 // 【必须有它】UV 归一化要除以纹理尺寸（`u_tex_inv_size`），
 // 而尺寸只能从图片句柄查到。`image == 0` 表示"用默认白纹理"：1×1 白纹理
 // 对任意 UV 都采到同一个白 texel，所以纯顶点色绘制不需要图片。
 // 【失效句柄 ⇒ 报错】这是"使用资源"，不是"查询资源"
 // 使用失效资源句柄必须报错。
    gl_api::uint_t draw_tex = 0;
    double tex_w = 1.0;
    double tex_h = 1.0;
    if (image != 0 && !image_texture_(image, draw_tex, tex_w, tex_h)) {
        const std::string desc = image_desc_(image);
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "draw_vertices 的 image";
        e.got = desc;
        e.want = "一个**有效**的图片句柄（0 = 白纹理）；"
                 "失效通常是它被 Unload 或被同名覆盖过";
        fail_boundary(e);
    }

 // =====================================================================
 // 校验段（四步，次序不可换 —— 每一步的理由见 renderer.hpp 的声明）。
 // 校验段里任何一步失败 ⇒ 整批不发；而它发生在任何副作用之前，
 // 所以没有需要回滚的东西 —— 这就是"一次调用是一个事务"的实现全文。
 // =====================================================================

 // ---- ① 上限：必须在读任何数据之前（已定）----
 // 否则一个越界的 count 会先把百万个元素读完才发现放不下，
 // 那时时间与内存已经花掉；而且它同时是"越界写的下界"。
    if (count > shape_vertex_limit) {
        char got_buf[32];
        std::snprintf(got_buf, sizeof(got_buf), "%zu", count);
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "绘制调用的顶点个数";
        e.got = got_buf;
        e.want = "<= 32（溢出报错，不自动拆分 —— 批边界由调用方决定）";
        fail_boundary(e);
    }

 // ---- ② 退化输入：count 0/1/2 围不成面 ⇒ 静默返回（已定，不报错）----
 // 一个片元都不会产生，无副作用；"看起来没用"是开发者的自由
 // （1.6：引擎拒绝的是数据矛盾，不是"看起来没用"）。
    if (count < 3) {
        return;
    }

 // ---- ③ 空指针：数据矛盾（count >= 3 却没有数据可读）----
    if (verts == nullptr) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "绘制调用的顶点数组";
        e.got = "nullptr";
        e.want = "非空（顶点个数 >= 3 时必须指向可读的顶点数据）";
        fail_boundary(e);
    }

 // ---- ④ 整批有限值校验：一个不合格，整批不发 ----
    vertex_defect d;
    if (find_vertex_defect(verts, count, d)) {
        char where_buf[96];
        std::snprintf(where_buf, sizeof(where_buf), "第 %zu 个顶点的 %c",
                      d.index + 1, d.field);
        char got_buf[64];
 // %g 会把 NaN 打成 "nan"、无穷打成 "inf" —— 正好是可读的形态。
        std::snprintf(got_buf, sizeof(got_buf), "%g", d.value);
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = where_buf;
        e.got = got_buf;
        e.want = "有限实数（NaN 与无穷不是引擎能解释的坐标）";
        fail_boundary(e);
    }

 // =====================================================================
 // 变换段：逻辑坐标 → 窗口像素（double），然后才转 float。
 //
 // 【精度纪律（渲染决策二 2.1）】映射在 double 域完成，float 只出现在
 // 写缓冲那一刻。顶点结构体里的 x/y 虽然已是 float（照抄 lstg 的结构，
 // 而"顶点精度有界"是写进5.3 的已接受事实），但**从它们
 // 出发到窗口像素的整段映射仍是 double 算术** —— letterbox 的缩放与
 // dst.b 的平移不会被 float 截断放大。
 //
 // 【本变换就是全链的 ①→②】from_rects 的 m11 负号（第一次 y 翻转）
 // 在这里面；GPU 侧的 pixel_to_ndc_ 是第二次。两次都必需，谁也别删。
 // =====================================================================
 // 【自定义着色器 ⇒ 顶点按整帧像素解释，不做世界变换】
 // 已定（2026-10-19）：开了 shader 状态，顶点就是**帧内像素**，
 // 于是它不受视口/letterbox 影响 —— 背景才能铺满整帧。
 // `v_uv` 归一化到**这个四边形**（取 u/v 的包围盒），不是到纹理。
    const bool shader_mode = (shader_program != 0);
    float uv_min_u = 0.0f;
    float uv_min_v = 0.0f;
    float uv_max_u = 0.0f;
    float uv_max_v = 0.0f;
    if (shader_mode) {
        uv_min_u = uv_min_v = 1e30f;
        uv_max_u = uv_max_v = -1e30f;
        for (std::size_t i = 0; i < count; ++i) {
            if (verts[i].u < uv_min_u) { uv_min_u = verts[i].u; }
            if (verts[i].v < uv_min_v) { uv_min_v = verts[i].v; }
            if (verts[i].u > uv_max_u) { uv_max_u = verts[i].u; }
            if (verts[i].v > uv_max_v) { uv_max_v = verts[i].v; }
        }
    }

    const space::transform& t = transform_;
    vertex gpu[shape_vertex_limit];
    for (std::size_t i = 0; i < count; ++i) {
        const vertex& src = verts[i];
 // 【shader_mode：像素直通】否则走世界→窗口像素的映射（全链 ①→②）
        gpu[i].x = shader_mode ? src.x : static_cast<float>(t.apply_x(src.x, src.y));
        gpu[i].y = shader_mode ? src.y : static_cast<float>(t.apply_y(src.x, src.y));
        gpu[i].u = src.u;   // UV 是**纹理像素单位**，与窗口映射无关，原样透传
        gpu[i].v = src.v;   //   （归一化在顶点着色器里做，见 u_tex_inv_size）
        gpu[i].color = src.color;

 // ---- UV 越界：容差内夹取，超出容差 ⇒ 整批不发 ----
        char bad_field = 0;
        double bad_value = 0.0;
 // 【shader_mode 跳过 UV 钳制】那组 u/v 是四边形局部像素，与纹理无关。
        if (!shader_mode && clamp_uv_or_defect(gpu[i], tex_w, tex_h, bad_field, bad_value)) {
            char where_buf[96];
            std::snprintf(where_buf, sizeof(where_buf),
                          "draw_vertices 第 %zu 个顶点的 %c", i + 1, bad_field);
            char got_buf[128];
            std::snprintf(got_buf, sizeof(got_buf), "%g（纹理 %gx%g）", bad_value,
                          tex_w, tex_h);
            error_value e;
            e.cls = error_class::boundary_validation;
            e.where = where_buf;
            e.got = got_buf;
            e.want = "落在 [0, 纹理宽/高] 内的**像素**坐标"
                     "（越界通常是图集矩形写错，或误传了 0..1 归一化值）";
            fail_boundary(e);
        }
    }

 // =====================================================================
 // 收集段（2026 绘序管线：变换后进帧顶点池 + 带排序键的绘制项）。
 // 到这里仍然没有任何 GL 调用 —— 排序与提交统一在 end_frame（阶段 9 后）。
 // 【事务边界的落点】校验段四步全过之后才收集 ⇒ 收集不可能失败；
 // "一次调用一个事务"由"校验在前、收集在后"这一顺序结构性地保证。
 // 【排序与发射】在 end_frame 统一做（sort_and_emit_draws_）：
 // 容量决定发射时的 flush 次数，不改变本调用的正确性与顺序。
 // 【纹理进批状态】同一个纹理的所有绘制项会连成一批；换纹理 ⇒ flush（触发②）。
 // =====================================================================
    if (shader_mode) {
        const float du = uv_max_u - uv_min_u;
        const float dv = uv_max_v - uv_min_v;
        collect_primitive_(gpu, count, layer, z, draw_tex,
                           (du > 0.0f) ? (1.0 / static_cast<double>(du)) : 1.0,
                           (dv > 0.0f) ? (1.0 / static_cast<double>(dv)) : 1.0,
                           shader_program);
    } else {
        collect_primitive_(gpu, count, layer, z, draw_tex,
                           (tex_w > 0.0) ? (1.0 / tex_w) : 1.0,
                           (tex_h > 0.0) ? (1.0 / tex_h) : 1.0,
                           0);
    }
}

bool renderer::find_vertex_defect(const vertex* verts, std::size_t count,
                                  vertex_defect& out) const noexcept {
 // 【查什么】x/y/z/u/v 五个 float 分量；颜色是无符号整数，天然有限。
 // 【怎么查】float 提升为 double 后用与 error.hpp 同一个 isfinite 判据
 // —— "NaN 与无穷对坐标而言不是少见的值，是没有意义的值"。
    static constexpr char kFieldNames[5] = {'x', 'y', 'z', 'u', 'v'};
    for (std::size_t i = 0; i < count; ++i) {
        const float fields[5] = {verts[i].x, verts[i].y, verts[i].z,
                                 verts[i].u, verts[i].v};
        for (int k = 0; k < 5; ++k) {
            if (!std::isfinite(static_cast<double>(fields[k]))) {
                out.index = i;
                out.field = kFieldNames[k];
                out.value = static_cast<double>(fields[k]);
                return true;
            }
        }
    }
    return false;
}

// =============================================================================
// 渲染目标与快照
// =============================================================================

void renderer::bind_render_target(std::uint64_t fbo) {
 // 【正当调用点】on_render_begin —— 它在清屏之后、任何绘制之前。
 //
 // 【为什么要限制在那一刻】
 // 因为帧结构把一个"渲染状态可以随意改"的窗口压缩成了两个点：
 // on_render_begin（开始）与 on_render_end（收尾）。在这两点之间，
 // 渲染状态是被脚本的绘制意图所依赖的。
 //
 // ---- 待补 · 接线 ----
 // gl_api::bind_framebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
 // // 并重设 glViewport 为该渲染目标的尺寸
    (void)fbo;
}

void renderer::unbind_render_target() {
 // 【正当调用点】on_render_end —— 它在所有绘制之后、呈现之前。
 //
 // ---- 待补 · 接线 ----
 // gl_api::bind_framebuffer(GL_FRAMEBUFFER, 0);
}

void renderer::adopt_staged_snapshot_as_frozen() {
 // 【它是一次指针交换，不是 GPU 操作】
 //
 // ⇒ 因此它**不会失败**。这一点让"冻结画面与状态切换是同一个原子事件"
 // 成立 —— 早期设计里这一步是 GPU 拷贝，于是它可能失败，于是那句话
 // 就不成立了（而"快照可能失败、出错时记录日志、不中断状态切换"
 // 那句也可以整句删掉）。
 //
 // 【为什么连 FBO 一起换，而不只换纹理】
 // 冻结这一半**永远不再被渲染进去**，它只是纹理的来源；
 // 但 staged 那一半下一帧要继续 blit ⇒ 两个 FBO 都得跟着各自的纹理走，
 // 否则 FBO 会指向"已经换了主人的纹理"（症状：下一帧 blit 到冻结画面里）。
    std::swap(frozen_fbo_, staged_fbo_);
    std::swap(frozen_color_tex_, staged_color_tex_);
    std::swap(frozen_w_, staged_w_);
    std::swap(frozen_h_, staged_h_);
    frozen_valid_ = staged_valid_;
 // 【交换后 staged 是"空的"】它拿到的是上一轮的 frozen（首次为 0）。
 // ⇒ 显式复位，让下一帧的 capture 重新 blit；也让"恢复后第一帧"有画面。
    staged_valid_ = false;
}

void renderer::draw_frozen_snapshot() {
 // 【调用点】阶段 8 的 Pause 分支，**在 on_render_pause 之前**。
 //
 // ⇒ 于是"暂停背景"由引擎提供，而菜单由脚本提供；
 // 脚本可以直接覆盖它（它只是 layer 最小的一笔，不是"背景层"特权）。
 //
 // ---- 做法：提交一笔"覆盖整个帧缓冲"的四边形 ----
 // 【为什么不直接 gl_api::draw_arrays】帧结构是"收集 → 排序 → 发射"，
 // 在这里直接发 GL 会绕过排序，于是它可能盖在已经被收集的绘制之上。
 // 提交成一笔绘制项 ⇒ 它自然走同一条管线（批状态、混合、发射时机）。
 //
 // 【为什么顶点用"整个帧缓冲的像素坐标"，而不是逻辑坐标】
 // 冻结画面**已经是屏幕像素**了：它是从帧缓冲 1:1 blit 出来的。
 // 若把它当作世界内容（再走一次 transform_），它会被缩放进渲染盒，
 // 于是"信箱边距"会被画进盒子里 —— 画面缩一圈。
 // ⇒ 用帧缓冲像素坐标 + 现有矩阵，矩阵把它映射到 NDC、viewport 再裁到
 // 渲染盒 ⇒ 可见部分正好是"渲染盒那一块的快照"，1:1 无缩放。
 // ⚠️ 所以**不要**在这里用 transform_。
 //
 // 【v 轴为什么要反过来】纹理 v=0 在帧缓冲**底部**（GL 约定），
 // 而顶点 y=0 在屏幕**顶部**（窗口像素、+y 向下）⇒ 两者反向。
 // 写反的症状是"暂停画面上下颠倒"，一眼可见。
 //
 // 【layer 取最小值】它必须压在所有脚本绘制之下：暂停菜单由
 // on_render_pause 在本函数之后提交，而排序键的第一维是 layer。
 // z 保持 0（z 只参与排序；顶点的 z 是雾的距离量，这里雾是关的）。
    if (!frozen_valid_ || frozen_color_tex_ == 0 || frozen_w_ <= 0 ||
        frozen_h_ <= 0) {
 // 【没有快照时静默不画】这是合法状态：例如启动后第一时间就暂停，
 // 那时还从未拍过任何一帧。它不是错误，不需要日志。
        return;
    }

    const float w = static_cast<float>(frozen_w_);
    const float h = static_cast<float>(frozen_h_);

    vertex quad[4];
    quad[0] = vertex{0.0f, 0.0f, 0.0f, 0.0f, h, 0xFFFFFFFFu};      // 左上
    quad[1] = vertex{w, 0.0f, 0.0f, w, h, 0xFFFFFFFFu};           // 右上
    quad[2] = vertex{w, h, 0.0f, w, 0.0f, 0xFFFFFFFFu};            // 右下
    quad[3] = vertex{0.0f, h, 0.0f, 0.0f, 0.0f, 0xFFFFFFFFu};      // 左下

 // 【强制默认状态】冻结画面必须是"原样照抄"：
 // 若继承脚本设过的雾/混合/顶点色，暂停背景就会被二次着色。
 // ⇒ 临时把状态覆盖归零，收集完立刻还原（脚本的状态栈不受影响）。
    const draw_state_override saved = state_;
    state_ = draw_state_override{};
    collect_primitive_(quad, 4, std::numeric_limits<std::int64_t>::min(), 0.0,
                       frozen_color_tex_, 1.0 / static_cast<double>(frozen_w_),
                       1.0 / static_cast<double>(frozen_h_), 0);
    state_ = saved;
}

} // namespace wag::draw
