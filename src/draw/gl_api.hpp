// =============================================================================
// wag/draw/gl_api.hpp
//
// GL 3.3 Core 入口表。
//
// 【为什么需要它 —— 实测过的事实，不是推测】
// Windows 的 opengl32.dll 只导出 **OpenGL 1.1** 的函数；
// `<GL/gl.h>` 里 3.3 的符号（glGenFramebuffers / glCreateShader …）
// 一个都没有。
// ⇒ 3.3 的一切入口都必须在运行时取回。
//
// 【已决：自写极小加载器，不引第三方 loader（glad / GLEW）】
// 三条理由：
// ① 本项目严格锁在 GL 3.3 Core ⇒ 需要的函数集合是一个**封闭清单**
// （见下），一张名字表就能穷举；
// ② 项目的依赖纪律是"只允许 SDL3 / LuaJIT"，
// 再引一个生成器或头文件库违背它 —— 而"实现便宜就引入"恰恰是
// 本项目反对的范畴；
// ③ SDL3 已提供解析器（SDL_GL_GetProcAddress，内部含 wglGetProcAddress
// 与对 opengl32.dll 导出的回退）⇒ 我们要补的只有"名字表 + 失败即报错"。
//
// 【取失败必须立刻报错，绝不静默 nullptr】
// 缺一个入口 ⇒ 后续所有 GL 代码都会在 nullptr 上炸，而且炸点远离原因。
// ⇒ load() 收集**全部**缺失的名字，一次报出（L3 终止）——
// 这比"碰到第一个缺的就停"多给一条诊断信息：驱动缺一片还是缺一个。
//
// 【为什么它是 struct 而不是一个子命名空间】
// 本项目的命名空间纪律是“最多两层”“不设后端子命名空间”。
// `wag::draw::gl` 会是第三层；于是入口表收成一个**类型**
// （wag::draw::gl_api，全静态成员），命名空间深度仍是两层。
// 它不是"抽象漏了后端名"——它是**唯一被授权写出 GL 名字的地方**
// （与 renderer.cpp"唯一与图形 API 打交道"同一条边界）。
//
// 【类型的对齐】
// 下面的别名按 GL 规范的 ABI 宽度取（GLenum/GLuint/GLbitfield = 32 位
// 无符号；GLint/GLsizei = 32 位有符号；GLfloat = 32 位浮点；
// GLboolean = 8 位；GLintptr/GLsizeiptr = 指针宽度）。
// 这些宽度由 GL 规范固定，在所有主流平台上与 <GL/gl.h> 的 typedef 一致。
// ⚠️ 本头文件**不包含** <GL/gl.h> —— 那个头只有 1.1 的声明，
// 混用它只会让"哪些符号要自己取"这件事变模糊。
//
// 【名单的边界】
// 只收"引擎接下来真的要调"的函数（批 / 着色器 / 纹理 / FBO / blit）。
// 没列进表的函数取不回来 —— 这是有意的：多一个名字就多一个
// "在缺它的驱动上起不来"的机会。
// =============================================================================

#pragma once

#include <cstdint>

namespace wag::draw {

struct gl_api {
 // -------------------------------------------------------------------------
 // GL ABI 类型别名（宽度由 GL 规范固定，见文件头的说明）
 // -------------------------------------------------------------------------
    using enum_t = std::uint32_t;       ///< GLenum / GLuint / GLbitfield
    using int_t = std::int32_t;         ///< GLint / GLsizei
    using uint_t = std::uint32_t;       ///< GLuint
    using float_t = float;              ///< GLfloat / GLclampf
    using boolean_t = std::uint8_t;     ///< GLboolean
    using ptrdiff_t_ = std::intptr_t;   ///< GLintptr
    using sizeiptr_t = std::ptrdiff_t;  ///< GLsizeiptr

 /// `GL_NO_ERROR`（规范固定为 0）。
    inline static constexpr enum_t no_error = 0;

 // -------------------------------------------------------------------------
 // 函数指针类型
 //
 // 【签名逐字对齐 GL 3.3 规范】查证来源：GL 3.3 Core Profile Specification
 // 的命令摘要，或 Khronos 官方 gl.xml。改签名之前先对那两处。
 // -------------------------------------------------------------------------
    using pfn_gen_buffers = void (*)(int_t n, uint_t* buffers);
    using pfn_bind_buffer = void (*)(enum_t target, uint_t buffer);
    using pfn_buffer_data = void (*)(enum_t target, sizeiptr_t size,
                                     const void* data, enum_t usage);
    using pfn_buffer_sub_data = void (*)(enum_t target, ptrdiff_t_ offset,
                                         sizeiptr_t size, const void* data);
    using pfn_delete_buffers = void (*)(int_t n, const uint_t* buffers);

    using pfn_gen_vertex_arrays = void (*)(int_t n, uint_t* arrays);
    using pfn_bind_vertex_array = void (*)(uint_t array);
    using pfn_delete_vertex_arrays = void (*)(int_t n, const uint_t* arrays);

    using pfn_enable_vertex_attrib_array = void (*)(uint_t index);
    using pfn_disable_vertex_attrib_array = void (*)(uint_t index);
    using pfn_vertex_attrib_pointer = void (*)(uint_t index, int_t size,
                                               enum_t type, boolean_t normalized,
                                               int_t stride, const void* pointer);

    using pfn_create_shader = uint_t (*)(enum_t type);
    using pfn_shader_source = void (*)(uint_t shader, int_t count,
                                       const char* const* string,
                                       const int_t* length);
    using pfn_compile_shader = void (*)(uint_t shader);
    using pfn_get_shader_iv = void (*)(uint_t shader, enum_t pname, int_t* params);
    using pfn_get_shader_info_log = void (*)(uint_t shader, int_t buf_size,
                                             int_t* length, char* info_log);
    using pfn_delete_shader = void (*)(uint_t shader);

    using pfn_create_program = uint_t (*)();
    using pfn_attach_shader = void (*)(uint_t program, uint_t shader);
    using pfn_link_program = void (*)(uint_t program);
    using pfn_get_program_iv = void (*)(uint_t program, enum_t pname, int_t* params);
    using pfn_get_program_info_log = void (*)(uint_t program, int_t buf_size,
                                              int_t* length, char* info_log);
    using pfn_delete_program = void (*)(uint_t program);
    using pfn_use_program = void (*)(uint_t program);
    using pfn_get_uniform_location = int_t (*)(uint_t program, const char* name);
    using pfn_get_attrib_location = int_t (*)(uint_t program, const char* name);
    using pfn_bind_attrib_location = void (*)(uint_t program, uint_t index,
                                              const char* name);
    using pfn_uniform_1i = void (*)(int_t location, int_t v0);
    using pfn_uniform_1f = void (*)(int_t location, float_t v0);
    using pfn_uniform_2f = void (*)(int_t location, float_t v0, float_t v1);
    using pfn_uniform_3f = void (*)(int_t location, float_t v0, float_t v1,
                                    float_t v2);
    using pfn_uniform_4f = void (*)(int_t location, float_t v0, float_t v1,
                                    float_t v2, float_t v3);
    using pfn_uniform_matrix_3fv = void (*)(int_t location, int_t count,
                                            boolean_t transpose,
                                            const float_t* value);

    using pfn_gen_textures = void (*)(int_t n, uint_t* textures);
    using pfn_bind_texture = void (*)(enum_t target, uint_t texture);
    using pfn_delete_textures = void (*)(int_t n, const uint_t* textures);
    using pfn_get_error = enum_t (*)();
    using pfn_tex_image_2d = void (*)(enum_t target, int_t level,
                                      int_t internal_format, int_t width,
                                      int_t height, int_t border, enum_t format,
                                      enum_t type, const void* pixels);
    using pfn_tex_sub_image_2d = void (*)(enum_t target, int_t level,
                                          int_t x_offset, int_t y_offset,
                                          int_t width, int_t height,
                                          enum_t format, enum_t type,
                                          const void* pixels);
    using pfn_tex_parameter_i = void (*)(enum_t target, enum_t pname, int_t param);
    using pfn_pixel_store_i = void (*)(enum_t pname, int_t param);
    using pfn_active_texture = void (*)(enum_t texture);

    using pfn_gen_framebuffers = void (*)(int_t n, uint_t* framebuffers);
    using pfn_bind_framebuffer = void (*)(enum_t target, uint_t framebuffer);
    using pfn_framebuffer_texture_2d = void (*)(enum_t target,
                                                enum_t attachment,
                                                enum_t textarget, uint_t texture,
                                                int_t level);
    using pfn_check_framebuffer_status = enum_t (*)(enum_t target);
    using pfn_delete_framebuffers = void (*)(int_t n, const uint_t* framebuffers);
    using pfn_blit_framebuffer = void (*)(int_t src_x0, int_t src_y0,
                                          int_t src_x1, int_t src_y1,
                                          int_t dst_x0, int_t dst_y0,
                                          int_t dst_x1, int_t dst_y1,
                                          enum_t mask, enum_t filter);

    using pfn_draw_arrays = void (*)(enum_t mode, int_t first, int_t count);
    using pfn_draw_arrays_instanced = void (*)(enum_t mode, int_t first,
                                               int_t count, int_t instance_count);
    using pfn_vertex_attrib_divisor = void (*)(uint_t index, uint_t divisor);
    using pfn_draw_elements = void (*)(enum_t mode, int_t count, enum_t type,
                                       const void* indices);
    using pfn_blend_func_separate = void (*)(enum_t src_rgb, enum_t dst_rgb,
                                             enum_t src_alpha, enum_t dst_alpha);
    using pfn_blend_equation_separate = void (*)(enum_t mode_rgb,
                                                 enum_t mode_alpha);

 // -------------------------------------------------------------------------
 // GL 3.3 常量（<GL/gl.h> 只到 1.1，缺的在这里按 GL 规范值补齐）
 //
 // 【为什么收进本表】与"入口表"同一条边界：本类型是唯一被授权写出 GL
 // 名字的地方。1.1 的常量（GL_TRIANGLES / GL_RGBA / GL_BLEND …）直接用
 // <GL/gl.h> 里的宏（renderer.cpp 经 SDL_opengl.h 拿到），**不**在这里重复
 // —— 同一个值两处定义就是两个来源。
 // 【命名】去 GL_ 前缀、snake_case，与入口的命名纪律一致。
 // 【取值】逐项对过 GL 3.3 规范（gl.xml 同值）；改之前先对规范。
 // -------------------------------------------------------------------------
    static constexpr enum_t vertex_shader   = 0x8B31;  ///< GL_VERTEX_SHADER
    static constexpr enum_t fragment_shader = 0x8B30;  ///< GL_FRAGMENT_SHADER
    static constexpr enum_t compile_status  = 0x8B81;  ///< GL_COMPILE_STATUS
    static constexpr enum_t link_status     = 0x8B82;  ///< GL_LINK_STATUS
    static constexpr enum_t array_buffer    = 0x8892;  ///< GL_ARRAY_BUFFER
    static constexpr enum_t element_array_buffer = 0x8893; ///< GL_ELEMENT_ARRAY_BUFFER
    static constexpr enum_t unsigned_short = 0x1403;  ///< GL_UNSIGNED_SHORT（draw_elements 的索引类型）
    static constexpr enum_t framebuffer          = 0x8D40;  ///< GL_FRAMEBUFFER（读写双向）
    static constexpr enum_t read_framebuffer     = 0x8CA8;  ///< GL_READ_FRAMEBUFFER
    static constexpr enum_t draw_framebuffer     = 0x8CA9;  ///< GL_DRAW_FRAMEBUFFER
    static constexpr enum_t framebuffer_complete = 0x8CD5;  ///< GL_FRAMEBUFFER_COMPLETE
    static constexpr enum_t color_attachment0    = 0x8CE0;  ///< GL_COLOR_ATTACHMENT0
    static constexpr enum_t stream_draw     = 0x88E0;  ///< GL_STREAM_DRAW

 /// GL_CLAMP_TO_EDGE（**GL 1.2 的常量** —— `<GL/gl.h>` 只到 1.1，头里没有它）
 ///
 /// 【为什么必须显式设它】`GL_TEXTURE_WRAP_S/T` 的**缺省值是 `GL_REPEAT`**（规范定死）
 /// ⇒ "什么都不做"等于**取模**。而我们的主线是**图集**：UV 越界取模会绕回去
 /// **采到图集里别的精灵** ⇒ 静默错图（本项目最贵的一类缺陷）。
 /// ⇒ 所以每张纹理都必须显式 clamp（见 renderer 里四处纹理创建）。
 /// 【为什么不用 GL_CLAMP】它是 GL 1.0 的、已被 core profile 移除，且行为不同。
    static constexpr enum_t clamp_to_edge    = 0x812F; ///< GL_CLAMP_TO_EDGE

 /// 混合方程（**GL 1.4 的常量** —— `<GL/gl.h>` 只到 1.1，头里**三个都没有**）
 ///
 /// 【为什么现在才需要它们】GL 的**默认方程就是 `FUNC_ADD`**
 /// ⇒ 只做"加"的时候一个常量都不用写、"什么都不设"就能编过；
 /// ⇒ 而 `Sub` / `SubNoAlpha` / `RevSub` 三个混合状态必须显式切换方程
 /// ⇒ 必须把这三个常量自己补齐（与 `clamp_to_edge` 同一类坑：
 /// **"不设"能编过、"设"反而要先补常量**）。
 ///
 /// 【方程语义（GL 规范，`R` 是结果）】
 /// · `FUNC_ADD` `R = src·sf + dst·df`
 /// · `FUNC_SUBTRACT` `R = src·sf − dst·df`
 /// · `FUNC_REVERSE_SUBTRACT` `R = dst·df − src·sf`
 /// ⚠️ 后两个**顺序相反**，写反了 `Sub` 与 `RevSub` 会正好互换 —— 而两者都能出画面，
 /// 所以这是个只会被"逐像素对照"抓住的错，肉眼很难判断。
    static constexpr enum_t func_add             = 0x8006; ///< GL_FUNC_ADD
    static constexpr enum_t func_subtract        = 0x800A; ///< GL_FUNC_SUBTRACT
    static constexpr enum_t func_reverse_subtract = 0x800B; ///< GL_FUNC_REVERSE_SUBTRACT

 // -------------------------------------------------------------------------
 // 生命周期
 // -------------------------------------------------------------------------

 /// 取回全部入口。**取失败立即 L3 终止**（收集全部缺失名字后一次报出）。
 ///
 /// 【调用点】renderer::adopt_context —— 拿到上下文之后、任何 GL 3.3 调用之前。
 ///
 /// 【前置条件：上下文必须已经在当前线程上成为当前上下文】
 /// 解析器用 SDL_GL_GetProcAddress，它在 Windows 上经 wglGetProcAddress，
 /// 而后者只对"当前上下文"有效。本引擎里这个前提由启动序列保证：
 /// SDL_GL_CreateContext 在创建那一刻就把上下文设为**创建线程的当前上下文**
 /// （SDL 说明明写），而 adopt_context 在同一线程上紧随其后。
 /// ⇒ 若将来出现"多个上下文 / 别的线程取入口"的形态，必须先 MakeCurrent。
 ///
 /// 【重复调用】幂等但无必要：入口在进程生存期内不会失效。
    static void load();

 /// 是否已经取回。
    [[nodiscard]] static bool loaded() noexcept { return loaded_; }

 // -------------------------------------------------------------------------
 // GL 3.3 Core 入口（封闭清单，按用途分组）
 //
 // 【命名】去 gl 前缀、snake_case —— 与本项目"snake_case"的纪律一致；
 // GL 原名写在 .cpp 那张名字表里，查 GL 说明时对照它。
 // -------------------------------------------------------------------------

 // ---- 顶点缓冲 ----
    static pfn_gen_buffers gen_buffers;
    static pfn_bind_buffer bind_buffer;
    static pfn_buffer_data buffer_data;
    static pfn_buffer_sub_data buffer_sub_data;
    static pfn_delete_buffers delete_buffers;

 // ---- 顶点数组对象 ----
    static pfn_gen_vertex_arrays gen_vertex_arrays;
    static pfn_bind_vertex_array bind_vertex_array;
    static pfn_delete_vertex_arrays delete_vertex_arrays;

 // ---- 顶点属性 ----
    static pfn_enable_vertex_attrib_array enable_vertex_attrib_array;
    static pfn_disable_vertex_attrib_array disable_vertex_attrib_array;
    static pfn_vertex_attrib_pointer vertex_attrib_pointer;

 // ---- 着色器与程序 ----
    static pfn_create_shader create_shader;
    static pfn_shader_source shader_source;
    static pfn_compile_shader compile_shader;
    static pfn_get_shader_iv get_shader_iv;
    static pfn_get_shader_info_log get_shader_info_log;
    static pfn_delete_shader delete_shader;
    static pfn_create_program create_program;
    static pfn_attach_shader attach_shader;
    static pfn_link_program link_program;
    static pfn_get_program_iv get_program_iv;
    static pfn_get_program_info_log get_program_info_log;
    static pfn_delete_program delete_program;
    static pfn_use_program use_program;
    static pfn_get_uniform_location get_uniform_location;
    static pfn_get_attrib_location get_attrib_location;
    static pfn_bind_attrib_location bind_attrib_location;
    static pfn_uniform_1i uniform_1i;
    static pfn_uniform_1f uniform_1f;
    static pfn_uniform_2f uniform_2f;
    static pfn_uniform_3f uniform_3f;
    static pfn_uniform_4f uniform_4f;
    static pfn_uniform_matrix_3fv uniform_matrix_3fv;

 // ---- 纹理 ----
    static pfn_gen_textures gen_textures;
    static pfn_bind_texture bind_texture;
    static pfn_delete_textures delete_textures;
    static pfn_get_error get_error;
    static pfn_tex_image_2d tex_image_2d;
    static pfn_tex_sub_image_2d tex_sub_image_2d;
    static pfn_tex_parameter_i tex_parameter_i;
    static pfn_pixel_store_i pixel_store_i;
    static pfn_active_texture active_texture;

 // ---- 帧缓冲对象与 blit ----
    static pfn_gen_framebuffers gen_framebuffers;
    static pfn_bind_framebuffer bind_framebuffer;
    static pfn_framebuffer_texture_2d framebuffer_texture_2d;
    static pfn_check_framebuffer_status check_framebuffer_status;
    static pfn_delete_framebuffers delete_framebuffers;
    static pfn_blit_framebuffer blit_framebuffer;

 // ---- 提交与混合 ----
    static pfn_draw_arrays draw_arrays;
    static pfn_draw_arrays_instanced draw_arrays_instanced;
    static pfn_vertex_attrib_divisor vertex_attrib_divisor;
    static pfn_draw_elements draw_elements;
    static pfn_blend_func_separate blend_func_separate;
    static pfn_blend_equation_separate blend_equation_separate;

private:
 /// 是否已取回。唯一写入点是 load()。
 ///
 /// 【为什么不是 atomic】引擎自己零并发，
 /// 而 load 只发生在启动序列（主线程）。
    static bool loaded_;   // 定义在 gl_api.cpp（与入口表同一批：类内声明、类外定义）
};

} // namespace wag::draw
