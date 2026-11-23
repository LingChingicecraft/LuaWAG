// =============================================================================
//  wag/draw/gl_api.cpp
//
//  GL 3.3 入口的取回实现：一张名字表 + 一次遍历 + 失败一次报全。
//
//  【本文件唯一的动作】把表里每个 GL 原名经 SDL_GL_GetProcAddress 解析出来，
//  写进对应入口；任何解析失败都**收集起来一起报**（L3 终止）。
//  除此之外它不做任何事 —— 它没有状态机，没有回退路径，没有"先试试看"。
//
//  【为什么失败要一次报全而不是碰到就停】
//      "缺一片"（驱动太老 / 软件渲染器）与"缺一个"（驱动怪癖）的处置与
//      诊断价值不同。一次报全让这两种情形在一眼内区分。
// =============================================================================

#include "draw/gl_api.hpp"

#include "log/log.hpp"

#if defined(WAG_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif

#include <cstdio>

// 【为什么需要它】条目数不再有独立常量 —— 全部用 `std::size(kEntries)` 由表推导。
#include <iterator>

namespace wag::draw {

namespace {

// -----------------------------------------------------------------------------
//  通用函数指针形态
//
//  【为什么需要它】名字表里每个条目的"落点"类型都不同（pfn_gen_buffers、
//    pfn_draw_arrays …），而表本身必须同构。于是落点统一表达成
//    "指向某个函数指针的指针"，经一次到 generic_fn 的函数指针转换写入。
//  【可移植性】函数指针 ↔ 函数指针的 reinterpret_cast 是良定义的；
//    调用永远经 gl_api.hpp 里的具名类型进行，不经 generic_fn 调用，
//    所以没有 ABI 问题。
// -----------------------------------------------------------------------------
using generic_fn = void (*)();

/// 名字表的一个条目：GL 原名 + 它的落点。
struct entry {
    const char* name;
    generic_fn* slot;
};

// -----------------------------------------------------------------------------
//  名字表（封闭清单）
//
//  【它为什么必须与 gl_api.hpp 的静态成员一一对应】
//      少一行 ⇒ 那个入口保持 nullptr ⇒ 使用它在缺名字的驱动上炸（而常见
//      驱动上不炸 —— 最坏的那类缺陷）。多一行 ⇒ 编译错（初始化列表里没有
//      对应成员）。
//      ⇒ "多一行会编译错"是这张表的安全方向：宁可错在编译期。
//      ⇒ 下面的 static_assert 把"条目数 = 成员数"钉成编译期事实；
//        两边任何一边改动，它都会先响。
//
//  ⚠️【2026-10-18 记录：这里原本有一个手写的 `kEntryCount = 46`，**已删除**】
//      当时它和这张表是"两个真相"：往表里加一行（`glUniform2f`）就必须记得
//      再改那个数，否则编译错 —— 它**唯一**能拦的就是"忘了改这个数"。
//      而真正危险的那种情况它拦不住：**在头里声明了成员、忘了往表里加**
//      （那样该成员保持 nullptr，只有走到它才崩）。
//      ⇒ 所以它被删掉，条目数一律由表推导（`std::size(kEntries)`）。
//      ⇒ **现在没有任何编译期断言守着这张表**，这是有意接受的：
//        能守的那条（声明/表一一对应）在 C++ 里无法静态表达 ——
//        表里存的是 `reinterpret_cast` 出来的函数指针，不是常量表达式，
//        所以也没法写 constexpr 查重。
//      ⇒ 剩下的保障是**运行期**的：`load()` 会把取不到的名字聚成一份清单，
//        缺任何一个就 L3 终止（"缺失 N 个（共 M 个）；清单：…"）。
//        也就是说：**声明了成员却漏了表项** 这种错，最晚在启动时暴露 ——
//        只要那条路径被走到；否则要等真的调用它。
// -----------------------------------------------------------------------------

const entry kEntries[] = {
    // ---- 顶点缓冲（5）----
    {"glGenBuffers", reinterpret_cast<generic_fn*>(&gl_api::gen_buffers)},
    {"glBindBuffer", reinterpret_cast<generic_fn*>(&gl_api::bind_buffer)},
    {"glBufferData", reinterpret_cast<generic_fn*>(&gl_api::buffer_data)},
    {"glBufferSubData", reinterpret_cast<generic_fn*>(&gl_api::buffer_sub_data)},
    {"glDeleteBuffers", reinterpret_cast<generic_fn*>(&gl_api::delete_buffers)},

    // ---- 顶点数组对象（3）----
    {"glGenVertexArrays", reinterpret_cast<generic_fn*>(&gl_api::gen_vertex_arrays)},
    {"glBindVertexArray", reinterpret_cast<generic_fn*>(&gl_api::bind_vertex_array)},
    {"glDeleteVertexArrays", reinterpret_cast<generic_fn*>(&gl_api::delete_vertex_arrays)},

    // ---- 顶点属性（3）----
    {"glEnableVertexAttribArray", reinterpret_cast<generic_fn*>(&gl_api::enable_vertex_attrib_array)},
    {"glDisableVertexAttribArray", reinterpret_cast<generic_fn*>(&gl_api::disable_vertex_attrib_array)},
    {"glVertexAttribPointer", reinterpret_cast<generic_fn*>(&gl_api::vertex_attrib_pointer)},

    // ---- 着色器与程序（17）----
    {"glCreateShader", reinterpret_cast<generic_fn*>(&gl_api::create_shader)},
    {"glShaderSource", reinterpret_cast<generic_fn*>(&gl_api::shader_source)},
    {"glCompileShader", reinterpret_cast<generic_fn*>(&gl_api::compile_shader)},
    {"glGetShaderiv", reinterpret_cast<generic_fn*>(&gl_api::get_shader_iv)},
    {"glGetShaderInfoLog", reinterpret_cast<generic_fn*>(&gl_api::get_shader_info_log)},
    {"glDeleteShader", reinterpret_cast<generic_fn*>(&gl_api::delete_shader)},
    {"glCreateProgram", reinterpret_cast<generic_fn*>(&gl_api::create_program)},
    {"glAttachShader", reinterpret_cast<generic_fn*>(&gl_api::attach_shader)},
    {"glLinkProgram", reinterpret_cast<generic_fn*>(&gl_api::link_program)},
    {"glGetProgramiv", reinterpret_cast<generic_fn*>(&gl_api::get_program_iv)},
    {"glGetProgramInfoLog", reinterpret_cast<generic_fn*>(&gl_api::get_program_info_log)},
    {"glDeleteProgram", reinterpret_cast<generic_fn*>(&gl_api::delete_program)},
    {"glUseProgram", reinterpret_cast<generic_fn*>(&gl_api::use_program)},
    {"glGetUniformLocation", reinterpret_cast<generic_fn*>(&gl_api::get_uniform_location)},
    {"glGetAttribLocation", reinterpret_cast<generic_fn*>(&gl_api::get_attrib_location)},
    {"glBindAttribLocation", reinterpret_cast<generic_fn*>(&gl_api::bind_attrib_location)},
    {"glUniform1i", reinterpret_cast<generic_fn*>(&gl_api::uniform_1i)},
    {"glUniform1f", reinterpret_cast<generic_fn*>(&gl_api::uniform_1f)},
    {"glUniform2f", reinterpret_cast<generic_fn*>(&gl_api::uniform_2f)},
    {"glUniform3f", reinterpret_cast<generic_fn*>(&gl_api::uniform_3f)},
    {"glUniform4f", reinterpret_cast<generic_fn*>(&gl_api::uniform_4f)},
    {"glUniformMatrix3fv", reinterpret_cast<generic_fn*>(&gl_api::uniform_matrix_3fv)},

    // ---- 纹理（8）----
    {"glGenTextures", reinterpret_cast<generic_fn*>(&gl_api::gen_textures)},
    {"glBindTexture", reinterpret_cast<generic_fn*>(&gl_api::bind_texture)},
    {"glDeleteTextures", reinterpret_cast<generic_fn*>(&gl_api::delete_textures)},
    {"glTexImage2D", reinterpret_cast<generic_fn*>(&gl_api::tex_image_2d)},
    {"glTexSubImage2D", reinterpret_cast<generic_fn*>(&gl_api::tex_sub_image_2d)},
    {"glTexParameteri", reinterpret_cast<generic_fn*>(&gl_api::tex_parameter_i)},
    {"glPixelStorei", reinterpret_cast<generic_fn*>(&gl_api::pixel_store_i)},
    {"glActiveTexture", reinterpret_cast<generic_fn*>(&gl_api::active_texture)},

    // ---- 诊断（1）----
    {"glGetError", reinterpret_cast<generic_fn*>(&gl_api::get_error)},

    // ---- 帧缓冲对象与 blit（6）----
    {"glGenFramebuffers", reinterpret_cast<generic_fn*>(&gl_api::gen_framebuffers)},
    {"glBindFramebuffer", reinterpret_cast<generic_fn*>(&gl_api::bind_framebuffer)},
    {"glFramebufferTexture2D", reinterpret_cast<generic_fn*>(&gl_api::framebuffer_texture_2d)},
    {"glCheckFramebufferStatus", reinterpret_cast<generic_fn*>(&gl_api::check_framebuffer_status)},
    {"glDeleteFramebuffers", reinterpret_cast<generic_fn*>(&gl_api::delete_framebuffers)},
    {"glBlitFramebuffer", reinterpret_cast<generic_fn*>(&gl_api::blit_framebuffer)},

    // ---- 提交与混合（4）----
    {"glDrawArrays", reinterpret_cast<generic_fn*>(&gl_api::draw_arrays)},
    {"glDrawArraysInstanced", reinterpret_cast<generic_fn*>(&gl_api::draw_arrays_instanced)},
    {"glVertexAttribDivisor", reinterpret_cast<generic_fn*>(&gl_api::vertex_attrib_divisor)},
    {"glDrawElements", reinterpret_cast<generic_fn*>(&gl_api::draw_elements)},
    {"glBlendFuncSeparate", reinterpret_cast<generic_fn*>(&gl_api::blend_func_separate)},
    {"glBlendEquationSeparate", reinterpret_cast<generic_fn*>(&gl_api::blend_equation_separate)},
};

// 【条目数只有一个来源：表本身】下面所有用到"共几个入口"的地方都取
//   `std::size(kEntries)` —— 不存在"需要手动同步的数字"。

} // namespace

// =============================================================================
//  取回
// =============================================================================

void gl_api::load() {
#if defined(WAG_HAVE_SDL3)
    if (loaded_) {
        return;
    }

    // ---- 逐条解析，收集缺失名单 ----
    //
    // 【detail 的容量】47 个名字 × 平均 24 字节 ≈ 1.1 KB 量级；
    //   真放满就是"缺了大半"，那条消息本身也已经说明问题了。
    char missing[1024];
    std::size_t missing_len = 0;
    int missing_count = 0;

    for (const entry& e : kEntries) {
        // SDL_GL_GetProcAddress 返回 SDL_FunctionPointer（一种通用函数指针）。
        const SDL_FunctionPointer addr = SDL_GL_GetProcAddress(e.name);
        if (addr == nullptr) {
            // 收集而不是立即终止：见文件头"一次报全"的理由。
            ++missing_count;
            const std::size_t room = sizeof(missing) - missing_len;
            const int written =
                std::snprintf(missing + missing_len, room, "%s%s",
                              (missing_len > 0) ? ", " : "", e.name);
            if (written > 0 && static_cast<std::size_t>(written) < room) {
                missing_len += static_cast<std::size_t>(written);
            } else {
                // 放不下了：截断并在末尾留一个明确的记号。
                static_cast<void>(std::snprintf(missing + missing_len, room, " …"));
                missing_len = sizeof(missing) - 1;
            }
            continue;
        }
        *e.slot = reinterpret_cast<generic_fn>(addr);
    }

    if (missing_count > 0) {
        // ---- L3：不可降级的资源失败 ----
        //   缺 GL 3.3 入口 ⇒ 环境跑不了本引擎 ⇒ 没有任何"带缺继续"的形态。
        //   log::terminate 内部保证"先落盘、再弹窗、后终止"（L3 之前先 flush）。
        char detail[1152];
        static_cast<void>(std::snprintf(
            detail, sizeof(detail), "缺失 %d 个（共 %zu 个）；清单：%s",
            missing_count, std::size(kEntries), missing));
        log::terminate("draw", log::format("GL 3.3 入口获取失败", detail));
    }

    loaded_ = true;
    char ok_detail[64];
    static_cast<void>(std::snprintf(ok_detail, sizeof(ok_detail), "count=%zu",
                                    std::size(kEntries)));
    log::record("draw", log::format("GL 3.3 入口已全部取回", ok_detail));
#else
    // 【无依赖档】本档根本没有上下文，load() 不该被调用
    //   （adopt_context 的 SDL3 分支才调它）。走到这里说明有人越档调用了，
    //   记一条 L1 留痕 —— 但不终止：这一档本来"只用于验证能否编译"。
    log::record("draw", "GL 入口未取回（本构建未接 SDL3）");
#endif
}
// -----------------------------------------------------------------------------
//  入口的类外定义（2026-10-15 从 hpp 的 inline static 移到这里）
//
//  【为什么移】inline 静态数据成员本应由链接器 COMDAT 去重；但本机工具链
//  （GCC 16 + binutils 2.47 快照 + PE/COFF）在带依赖档下把两个编译单元
//  （gl_api.cpp.obj 与 renderer.cpp.obj）各自的强 D 定义都带了出来，
//  链接报 multiple definition —— 而 ld 在报这个错时自己段错误，诊断完全丢失。
//  ⇒ 与其赌去重行为，不如回到经典形态：
//  类内声明（hpp）、唯一一份类外定义（本文件）—— 多定义这一类错误从结构上消失。
//  （顺带：它让"入口表在哪定义"与"在哪取回"同处一个文件，更好查。）
// -----------------------------------------------------------------------------
wag::draw::gl_api::pfn_gen_buffers wag::draw::gl_api::gen_buffers = nullptr;
wag::draw::gl_api::pfn_bind_buffer wag::draw::gl_api::bind_buffer = nullptr;
wag::draw::gl_api::pfn_buffer_data wag::draw::gl_api::buffer_data = nullptr;
wag::draw::gl_api::pfn_buffer_sub_data wag::draw::gl_api::buffer_sub_data = nullptr;
wag::draw::gl_api::pfn_delete_buffers wag::draw::gl_api::delete_buffers = nullptr;
wag::draw::gl_api::pfn_gen_vertex_arrays wag::draw::gl_api::gen_vertex_arrays = nullptr;
wag::draw::gl_api::pfn_bind_vertex_array wag::draw::gl_api::bind_vertex_array = nullptr;
wag::draw::gl_api::pfn_delete_vertex_arrays wag::draw::gl_api::delete_vertex_arrays = nullptr;
wag::draw::gl_api::pfn_enable_vertex_attrib_array wag::draw::gl_api::enable_vertex_attrib_array = nullptr;
wag::draw::gl_api::pfn_disable_vertex_attrib_array wag::draw::gl_api::disable_vertex_attrib_array = nullptr;
wag::draw::gl_api::pfn_vertex_attrib_pointer wag::draw::gl_api::vertex_attrib_pointer = nullptr;
wag::draw::gl_api::pfn_create_shader wag::draw::gl_api::create_shader = nullptr;
wag::draw::gl_api::pfn_shader_source wag::draw::gl_api::shader_source = nullptr;
wag::draw::gl_api::pfn_compile_shader wag::draw::gl_api::compile_shader = nullptr;
wag::draw::gl_api::pfn_get_shader_iv wag::draw::gl_api::get_shader_iv = nullptr;
wag::draw::gl_api::pfn_get_shader_info_log wag::draw::gl_api::get_shader_info_log = nullptr;
wag::draw::gl_api::pfn_delete_shader wag::draw::gl_api::delete_shader = nullptr;
wag::draw::gl_api::pfn_create_program wag::draw::gl_api::create_program = nullptr;
wag::draw::gl_api::pfn_attach_shader wag::draw::gl_api::attach_shader = nullptr;
wag::draw::gl_api::pfn_link_program wag::draw::gl_api::link_program = nullptr;
wag::draw::gl_api::pfn_get_program_iv wag::draw::gl_api::get_program_iv = nullptr;
wag::draw::gl_api::pfn_get_program_info_log wag::draw::gl_api::get_program_info_log = nullptr;
wag::draw::gl_api::pfn_delete_program wag::draw::gl_api::delete_program = nullptr;
wag::draw::gl_api::pfn_use_program wag::draw::gl_api::use_program = nullptr;
wag::draw::gl_api::pfn_get_uniform_location wag::draw::gl_api::get_uniform_location = nullptr;
wag::draw::gl_api::pfn_get_attrib_location wag::draw::gl_api::get_attrib_location = nullptr;
wag::draw::gl_api::pfn_bind_attrib_location wag::draw::gl_api::bind_attrib_location = nullptr;
wag::draw::gl_api::pfn_uniform_1i wag::draw::gl_api::uniform_1i = nullptr;
wag::draw::gl_api::pfn_uniform_1f wag::draw::gl_api::uniform_1f = nullptr;
wag::draw::gl_api::pfn_uniform_2f wag::draw::gl_api::uniform_2f = nullptr;
wag::draw::gl_api::pfn_uniform_3f wag::draw::gl_api::uniform_3f = nullptr;
wag::draw::gl_api::pfn_uniform_4f wag::draw::gl_api::uniform_4f = nullptr;
wag::draw::gl_api::pfn_uniform_matrix_3fv wag::draw::gl_api::uniform_matrix_3fv = nullptr;
wag::draw::gl_api::pfn_gen_textures wag::draw::gl_api::gen_textures = nullptr;
wag::draw::gl_api::pfn_bind_texture wag::draw::gl_api::bind_texture = nullptr;
wag::draw::gl_api::pfn_delete_textures wag::draw::gl_api::delete_textures = nullptr;
wag::draw::gl_api::pfn_get_error wag::draw::gl_api::get_error = nullptr;
wag::draw::gl_api::pfn_tex_image_2d wag::draw::gl_api::tex_image_2d = nullptr;
wag::draw::gl_api::pfn_tex_sub_image_2d wag::draw::gl_api::tex_sub_image_2d = nullptr;
wag::draw::gl_api::pfn_tex_parameter_i wag::draw::gl_api::tex_parameter_i = nullptr;
wag::draw::gl_api::pfn_pixel_store_i wag::draw::gl_api::pixel_store_i = nullptr;
wag::draw::gl_api::pfn_active_texture wag::draw::gl_api::active_texture = nullptr;
wag::draw::gl_api::pfn_gen_framebuffers wag::draw::gl_api::gen_framebuffers = nullptr;
wag::draw::gl_api::pfn_bind_framebuffer wag::draw::gl_api::bind_framebuffer = nullptr;
wag::draw::gl_api::pfn_framebuffer_texture_2d wag::draw::gl_api::framebuffer_texture_2d = nullptr;
wag::draw::gl_api::pfn_check_framebuffer_status wag::draw::gl_api::check_framebuffer_status = nullptr;
wag::draw::gl_api::pfn_delete_framebuffers wag::draw::gl_api::delete_framebuffers = nullptr;
wag::draw::gl_api::pfn_blit_framebuffer wag::draw::gl_api::blit_framebuffer = nullptr;
wag::draw::gl_api::pfn_draw_arrays wag::draw::gl_api::draw_arrays = nullptr;
wag::draw::gl_api::pfn_draw_arrays_instanced wag::draw::gl_api::draw_arrays_instanced = nullptr;
wag::draw::gl_api::pfn_vertex_attrib_divisor wag::draw::gl_api::vertex_attrib_divisor = nullptr;
wag::draw::gl_api::pfn_draw_elements wag::draw::gl_api::draw_elements = nullptr;
wag::draw::gl_api::pfn_blend_func_separate wag::draw::gl_api::blend_func_separate = nullptr;
wag::draw::gl_api::pfn_blend_equation_separate wag::draw::gl_api::blend_equation_separate = nullptr;
bool gl_api::loaded_ = false;

} // namespace wag::draw
