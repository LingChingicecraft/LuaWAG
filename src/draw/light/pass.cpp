// =============================================================================
// draw/light/pass.cpp —— 光通道主文件（shader / 光图 / 合成）
//
// 【它负责什么】
// · 加载外置 light.vs / light.fs
// · 把本帧的光画进 RGBA16F 光图
// · 加色合成回场景（曝光在合成里）
// 【它不负责什么】
// · 渲染目标 → targets.cpp
// · 遮挡者池与遮蔽图 → occluders.cpp
// · band / commit / 带掩码 / 统计 → bands.cpp
// =============================================================================

#include "draw/renderer.hpp"

#include "draw/gl_api.hpp"
#include "draw/light/shader_util.hpp"
#include "error/error.hpp"
#include "log/log.hpp"

#if defined(WAG_HAVE_SDL3)
#include <SDL3/SDL_opengl.h>
#endif

#include <string>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace wag::draw {

// =============================================================================
// 光通道（灾后恢复版；目标/遮挡者/带已拆到同目录）
// =============================================================================

namespace {


struct light_gl_state {
    gl_api::uint_t prog = 0;
    gl_api::int_t u_mask = -1;
    gl_api::int_t u_band_mask = -1;
    gl_api::int_t u_mask_size = -1;
    gl_api::int_t u_pixel_to_ndc = -1;
    gl_api::uint_t vao = 0;
    gl_api::uint_t quad = 0;
    gl_api::uint_t inst = 0;
    gl_api::uint_t comp_prog = 0;
    gl_api::uint_t comp_vao = 0;
    gl_api::uint_t comp_vbo = 0;
    gl_api::int_t comp_u_light = -1;
    gl_api::int_t comp_u_box = -1;
    gl_api::int_t comp_u_exposure = -1;
    bool inst_ready = false;
    bool comp_ready = false;
};

light_gl_state g_light;

bool read_text_file(const std::string& path, std::string& out, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "读不到文件：" + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    if (out.empty()) {
        err = "文件为空：" + path;
        return false;
    }
    return true;
}

} // namespace

bool renderer::light_load_shader(const std::string& dir, std::string& err) {
    std::string vs_src;
    std::string fs_src;
    if (!read_text_file(dir + "/light.vs", vs_src, err) ||
        !read_text_file(dir + "/light.fs", fs_src, err)) {
        return false;
    }
    const gl_api::uint_t vs = light_compile_stage(gl_api::vertex_shader,
                                                  vs_src.c_str(), "光顶点");
    const gl_api::uint_t fs = light_compile_stage(gl_api::fragment_shader,
                                                  fs_src.c_str(), "光片元");
    const gl_api::uint_t prog = light_link_stage(vs, fs);
    gl_api::delete_shader(vs);
    gl_api::delete_shader(fs);

    if (g_light.prog != 0) {
        gl_api::delete_program(g_light.prog);
    }
    g_light.prog = prog;
    g_light.u_mask = gl_api::get_uniform_location(prog, "u_mask");
    g_light.u_band_mask = gl_api::get_uniform_location(prog, "u_band_mask");
    g_light.u_mask_size = gl_api::get_uniform_location(prog, "u_mask_size");
    g_light.u_pixel_to_ndc = gl_api::get_uniform_location(prog, "u_pixel_to_ndc");
    light_shader_dir_ = dir;
    light_prog_dirty_ = false;
    log::record("light", log::format("光 shader 已加载", dir));
    return true;
}

void renderer::render_light_pass_() {
    if (lights_.empty()) {
        return;
    }
    render_occluder_mask_();

    const int lw = light::map_width;
    const int lh = light::map_height;
    ensure_render_target_(light_map_, lw, lh, true);

    if (g_light.prog == 0) {
        std::string err;
        const std::string dir = light_shader_dir_.empty()
                                    ? std::string("Luminosa/Project/shaders")
                                    : light_shader_dir_;
        if (!light_load_shader(dir, err)) {
            fail_internal("光通道：未加载光 shader（" + err + "）");
        }
    }

    bind_render_target_(light_map_);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!g_light.inst_ready) {
        gl_api::gen_vertex_arrays(1, &g_light.vao);
        gl_api::gen_buffers(1, &g_light.quad);
        gl_api::gen_buffers(1, &g_light.inst);
    std::uint64_t fill = 0;
    for (const light::light& l : lights_) {
        const double r = (l.radius > 0.0f) ? (static_cast<double>(l.radius) * 0.5)
                                            : (static_cast<double>(l.width) * 0.5);
        fill += static_cast<std::uint64_t>(3.141592653589793 * r * r);
    }
    frame_fill_px_ += fill;
    if (lights_.size() > frame_lights_max_) {
        frame_lights_max_ = static_cast<std::uint32_t>(lights_.size());
    }
    {
        const double ratio = light_fill_ratio_();
        if (ratio > 32.0 && !warned_fill_) {
            warned_fill_ = true;
            new_over_fill_ = true;
            log::warn("light", log::format("光图填充超过 32×",
                                   "fill=" + std::to_string(ratio)));
        }
    }

        gl_api::bind_vertex_array(g_light.vao);
        const float quad[8] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
        gl_api::bind_buffer(gl_api::array_buffer, g_light.quad);
        gl_api::buffer_data(gl_api::array_buffer,
                            static_cast<gl_api::sizeiptr_t>(sizeof(quad)), quad,
                            gl_api::stream_draw);
        gl_api::vertex_attrib_pointer(0, 2, GL_FLOAT, 0, 2 * sizeof(float), nullptr);
        gl_api::enable_vertex_attrib_array(0);
        gl_api::bind_buffer(gl_api::array_buffer, g_light.inst);
        const auto base = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0));
        gl_api::vertex_attrib_pointer(1, 2, GL_FLOAT, 0, 14 * sizeof(float), base);
        gl_api::vertex_attrib_pointer(2, 2, GL_FLOAT, 0, 14 * sizeof(float),
                                      reinterpret_cast<const void*>(static_cast<std::uintptr_t>(2 * sizeof(float))));
        gl_api::vertex_attrib_pointer(3, 4, GL_FLOAT, 0, 14 * sizeof(float),
                                      reinterpret_cast<const void*>(static_cast<std::uintptr_t>(4 * sizeof(float))));
        gl_api::vertex_attrib_pointer(4, 1, GL_FLOAT, 0, 14 * sizeof(float),
                                      reinterpret_cast<const void*>(static_cast<std::uintptr_t>(8 * sizeof(float))));
        gl_api::vertex_attrib_pointer(5, 2, GL_FLOAT, 0, 14 * sizeof(float),
                                      reinterpret_cast<const void*>(static_cast<std::uintptr_t>(9 * sizeof(float))));
        gl_api::vertex_attrib_pointer(6, 2, GL_FLOAT, 0, 14 * sizeof(float),
                                      reinterpret_cast<const void*>(static_cast<std::uintptr_t>(11 * sizeof(float))));
        gl_api::vertex_attrib_pointer(7, 1, GL_FLOAT, 0, 14 * sizeof(float),
                                      reinterpret_cast<const void*>(static_cast<std::uintptr_t>(13 * sizeof(float))));
        for (gl_api::uint_t a = 1; a <= 7; ++a) {
            gl_api::enable_vertex_attrib_array(a);
            gl_api::vertex_attrib_divisor(a, 1);
        }
        gl_api::bind_vertex_array(0);
        g_light.inst_ready = true;
    }

    std::vector<float> data;
    data.reserve(lights_.size() * 14);
    for (const light::light& l : lights_) {
        const float r = (l.radius > 0.0f) ? l.radius
                                          : ((l.width > 0.0f) ? l.width : 1.0f);
        data.push_back(l.x);
        data.push_back(l.y);
        data.push_back(r * 2.0f);
        data.push_back(r * 2.0f);
        data.push_back(static_cast<float>((l.color >> 16) & 0xFF) / 255.0f);
        data.push_back(static_cast<float>((l.color >> 8) & 0xFF) / 255.0f);
        data.push_back(static_cast<float>(l.color & 0xFF) / 255.0f);
        data.push_back(static_cast<float>((l.color >> 24) & 0xFF) / 255.0f);
        data.push_back(l.intensity);
        data.push_back(l.angle);
        data.push_back(l.spread);
        data.push_back(l.softness);
        data.push_back(l.falloff);
        data.push_back((l.r == light::reach::self_only) ? 10.0f : 0.0f);
    }

    gl_api::bind_vertex_array(g_light.vao);
    gl_api::bind_buffer(gl_api::array_buffer, g_light.inst);
    gl_api::buffer_data(gl_api::array_buffer,
                        static_cast<gl_api::sizeiptr_t>(data.size() * sizeof(float)),
                        data.data(), gl_api::stream_draw);
    gl_api::use_program(g_light.prog);

    gl_api::active_texture(0x84C0);
    gl_api::bind_texture(GL_TEXTURE_2D, occluder_mask_.tex);
    gl_api::active_texture(0x84C1);
    gl_api::bind_texture(GL_TEXTURE_2D,
                         (band_mask_active_ && band_mask_.tex != 0)
                             ? band_mask_.tex
                             : occluder_mask_.tex);
    gl_api::active_texture(0x84C0);
    if (g_light.u_mask >= 0) {
        gl_api::uniform_1i(g_light.u_mask, 0);
    }
    if (g_light.u_band_mask >= 0) {
        gl_api::uniform_1i(g_light.u_band_mask, 1);
    }
    if (g_light.u_mask_size >= 0) {
        gl_api::uniform_2f(g_light.u_mask_size,
                           static_cast<float>(occluder_mask_.w),
                           static_cast<float>(occluder_mask_.h));
    }
    if (g_light.u_pixel_to_ndc >= 0) {
        const float sx = 2.0f / static_cast<float>(lw);
        const float sy = 2.0f / static_cast<float>(lh);
        const float mat[9] = {sx, 0.0f, 0.0f,
                              0.0f, sy, 0.0f,
                              -1.0f, -1.0f, 1.0f};
        gl_api::uniform_matrix_3fv(g_light.u_pixel_to_ndc, 1, 0, mat);
    }
    gl_api::draw_arrays_instanced(GL_TRIANGLE_STRIP, 0, 4,
                                  static_cast<gl_api::int_t>(lights_.size()));
    gl_api::bind_vertex_array(0);
    unbind_render_target_();

 // ---- 合成：光图加色叠回场景 ----
    if (!g_light.comp_ready) {
        const char* cvs = R"(#version 330 core
layout(location = 0) in vec2 a_corner;
out vec2 v_uv;
void main() {
    v_uv = a_corner;
    gl_Position = vec4(a_corner * 2.0 - 1.0, 0.0, 1.0);
}
)";
        const char* cfs = R"(#version 330 core
in vec2 v_uv;
uniform sampler2D u_light;
uniform vec4 u_box;
uniform float u_exposure;
out vec4 frag;
void main() {
    vec2 uv = (gl_FragCoord.xy - u_box.xy) / max(u_box.zw, vec2(1.0));
    frag = vec4(texture(u_light, uv).rgb * u_exposure, 1.0);
}
)";
        const gl_api::uint_t vs = light_compile_stage(gl_api::vertex_shader, cvs, "合成顶点");
        const gl_api::uint_t fs = light_compile_stage(gl_api::fragment_shader, cfs, "合成片元");
        g_light.comp_prog = light_link_stage(vs, fs);
        gl_api::delete_shader(vs);
        gl_api::delete_shader(fs);
        gl_api::gen_vertex_arrays(1, &g_light.comp_vao);
        gl_api::gen_buffers(1, &g_light.comp_vbo);
        gl_api::bind_vertex_array(g_light.comp_vao);
        gl_api::bind_buffer(gl_api::array_buffer, g_light.comp_vbo);
        const float quad[8] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
        gl_api::buffer_data(gl_api::array_buffer,
                            static_cast<gl_api::sizeiptr_t>(sizeof(quad)), quad,
                            gl_api::stream_draw);
        gl_api::vertex_attrib_pointer(0, 2, GL_FLOAT, 0, 2 * sizeof(float), nullptr);
        gl_api::enable_vertex_attrib_array(0);
        gl_api::bind_vertex_array(0);
        g_light.comp_u_light = gl_api::get_uniform_location(g_light.comp_prog, "u_light");
        g_light.comp_u_box = gl_api::get_uniform_location(g_light.comp_prog, "u_box");
        g_light.comp_u_exposure = gl_api::get_uniform_location(g_light.comp_prog, "u_exposure");
        g_light.comp_ready = true;
    }

    glViewport(0, 0, static_cast<int>(frame_pixel_[0]),
               static_cast<int>(frame_pixel_[1]));
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    gl_api::use_program(g_light.comp_prog);
    gl_api::bind_vertex_array(g_light.comp_vao);
    gl_api::active_texture(0x84C0);
    gl_api::bind_texture(GL_TEXTURE_2D, light_map_.tex);
    if (g_light.comp_u_light >= 0) {
        gl_api::uniform_1i(g_light.comp_u_light, 0);
    }
    if (g_light.comp_u_box >= 0) {
        gl_api::uniform_4f(g_light.comp_u_box, frame_box_[0], frame_box_[1],
                           frame_box_[2], frame_box_[3]);
    }
    if (g_light.comp_u_exposure >= 0) {
        gl_api::uniform_1f(g_light.comp_u_exposure, exposure_);
    }
    gl_api::draw_arrays(GL_TRIANGLE_STRIP, 0, 4);
    gl_api::bind_vertex_array(0);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(static_cast<int>(frame_box_[0]), static_cast<int>(frame_box_[1]),
               static_cast<int>(frame_box_[2]), static_cast<int>(frame_box_[3]));
    glEnable(GL_SCISSOR_TEST);
    lights_.clear();
}

void renderer::debug_light_selftest_() {
    static bool done = false;
    if (done) {
        return;
    }
    const char* env = std::getenv("WAG_LIGHT_SELFTEST");
    if (env == nullptr || env[0] == '\0' || env[0] == '0') {
        return;
    }
    done = true;

    std::string err;
    if (!light_load_shader("Luminosa/Project/shaders", err)) {
        fail_internal("光自检：加载 shader 失败（" + err + "）");
    }
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, static_cast<int>(frame_pixel_[0]),
               static_cast<int>(frame_pixel_[1]));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    light::light l;
    l.k = light::kind::point;
    l.x = 0.0f;
    l.y = 0.0f;
    l.radius = 320.0f;
    l.color = 0xFFFFFFFFu;
    l.intensity = 2.0f;
    l.softness = 2.0f;
    l.falloff = 1.0f;
    light_add(l);
    render_light_pass_();

    const int cx = static_cast<int>(frame_box_[0] + frame_box_[2] * 0.5f);
    const int cy = static_cast<int>(frame_box_[1] + frame_box_[3] * 0.5f);
    unsigned char center[4] = {0, 0, 0, 0};
    unsigned char edge[4] = {0, 0, 0, 0};
    glReadPixels(cx, cy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
    glReadPixels(static_cast<int>(frame_box_[0]) + 4,
                 static_cast<int>(frame_box_[1]) + 4, 1, 1, GL_RGBA,
                 GL_UNSIGNED_BYTE, edge);
    const bool ok = (center[0] > 20 && edge[0] < 5);
    log::record("light", log::format("自检",
                            ok ? "PASS：中心亮、边缘暗"
                               : "FAIL：光没有落到光图/合成"));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}




// =============================================================================
// 实例化自检（2026-11-18 灾后恢复，第 3 步）
// =============================================================================


void renderer::debug_instance_selftest_() {
    static bool done = false;
    if (done) {
        return;
    }
    const char* env = std::getenv("WAG_INSTANCE_SELFTEST");
    if (env == nullptr || env[0] == '\0' || env[0] == '0') {
        return;
    }
    done = true;

    render_target rt;
    ensure_render_target_(rt, 64, 64, false);
    bind_render_target_(rt);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    const float quad[8] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    const float instances[4 * 6] = {
        4.0f,  4.0f,  1.0f, 0.0f, 0.0f, 1.0f,
        20.0f, 4.0f,  0.0f, 1.0f, 0.0f, 1.0f,
        36.0f, 4.0f,  0.0f, 0.0f, 1.0f, 1.0f,
        52.0f, 4.0f,  1.0f, 1.0f, 1.0f, 1.0f,
    };

    gl_api::uint_t vao = 0;
    gl_api::uint_t quad_vbo = 0;
    gl_api::uint_t inst_vbo = 0;
    gl_api::gen_vertex_arrays(1, &vao);
    gl_api::gen_buffers(1, &quad_vbo);
    gl_api::gen_buffers(1, &inst_vbo);
    gl_api::bind_vertex_array(vao);

    gl_api::bind_buffer(gl_api::array_buffer, quad_vbo);
    gl_api::buffer_data(gl_api::array_buffer,
                        static_cast<gl_api::sizeiptr_t>(sizeof(quad)), quad,
                        gl_api::stream_draw);
    gl_api::vertex_attrib_pointer(0, 2, GL_FLOAT, 0, 2 * sizeof(float), nullptr);
    gl_api::enable_vertex_attrib_array(0);

    gl_api::bind_buffer(gl_api::array_buffer, inst_vbo);
    gl_api::buffer_data(gl_api::array_buffer,
                        static_cast<gl_api::sizeiptr_t>(sizeof(instances)),
                        instances, gl_api::stream_draw);
    gl_api::vertex_attrib_pointer(1, 2, GL_FLOAT, 0, 6 * sizeof(float),
                                  reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0)));
    gl_api::enable_vertex_attrib_array(1);
    gl_api::vertex_attrib_pointer(2, 4, GL_FLOAT, 0, 6 * sizeof(float),
                                  reinterpret_cast<const void*>(static_cast<std::uintptr_t>(2 * sizeof(float))));
    gl_api::enable_vertex_attrib_array(2);
    gl_api::vertex_attrib_divisor(1, 1);
    gl_api::vertex_attrib_divisor(2, 1);

    const char* vs_src = R"(#version 330 core
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec2 a_offset;
layout(location = 2) in vec4 a_color;
out vec4 v_color;
void main() {
    vec2 p = a_offset + a_corner * 8.0;
    gl_Position = vec4(p.x / 32.0 - 1.0, p.y / 32.0 - 1.0, 0.0, 1.0);
    v_color = a_color;
}
)";
    const char* fs_src = R"(#version 330 core
in vec4 v_color;
out vec4 frag;
void main() { frag = v_color; }
)";
    const gl_api::uint_t vs = light_compile_stage(gl_api::vertex_shader, vs_src, "实例顶点");
    const gl_api::uint_t fs = light_compile_stage(gl_api::fragment_shader, fs_src, "实例片元");
    const gl_api::uint_t prog = light_link_stage(vs, fs);
    gl_api::delete_shader(vs);
    gl_api::delete_shader(fs);

    gl_api::use_program(prog);
    gl_api::draw_arrays_instanced(GL_TRIANGLE_STRIP, 0, 4, 4);

    const int cx[4] = {8, 24, 40, 56};
    const unsigned char expect[4][3] = {{255, 0, 0}, {0, 255, 0},
                                        {0, 0, 255}, {255, 255, 255}};
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
        unsigned char px[4] = {0, 0, 0, 0};
        glReadPixels(cx[i], 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        const int dr = std::abs(static_cast<int>(px[0]) - expect[i][0]);
        const int dg = std::abs(static_cast<int>(px[1]) - expect[i][1]);
        const int db = std::abs(static_cast<int>(px[2]) - expect[i][2]);
        if (dr > 2 || dg > 2 || db > 2) {
            ok = false;
        }
    }

    gl_api::bind_vertex_array(0);
    gl_api::delete_buffers(1, &quad_vbo);
    gl_api::delete_buffers(1, &inst_vbo);
    gl_api::delete_vertex_arrays(1, &vao);
    gl_api::delete_program(prog);
    unbind_render_target_();
    destroy_render_target_(rt);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    log::record("inst", log::format("自检",
                            ok ? "PASS：一次 draw 画 4 个实例"
                               : "FAIL：实例颜色或位置不对"));
}





} // namespace wag::draw
