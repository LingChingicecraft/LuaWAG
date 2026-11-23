// =============================================================================
//  draw/light/bands.cpp —— 带 / commit / 本带掩码 / 统计
//
//  【职责】
//    · light_add / light_clear（一次性光的入口）
//    · band 声明、commit（按 layer 过滤、发射本带几何）
//    · self_only 的本带掩码（commit 时、sort 之前画）
//    · take_light_stats（本帧计数 + 预算超限标记）
//    · 带 / self_only / 预算三个自检
//  【依赖】pass.cpp 的 render_light_pass_；shader_util.hpp 的编译 helper。
// =============================================================================

#include "draw/light/shader_util.hpp"
#include "draw/renderer.hpp"

#include "draw/gl_api.hpp"
#include "error/error.hpp"
#include "log/log.hpp"

#if defined(WAG_HAVE_SDL3)
#include <SDL3/SDL_opengl.h>
#endif

#include <string>
#include <utility>
#include <vector>

namespace wag::draw {
void renderer::light_add(const light::light& l) { lights_.push_back(l); }

void renderer::light_clear() noexcept { lights_.clear(); }

bool renderer::light_band_declare(const std::string& id, std::int64_t from,
                                  std::int64_t to, std::string& err) {
    if (id.empty()) {
        err = "band 的 id 不能为空";
        return false;
    }
    if (from > to) {
        err = "band「" + id + "」的 from > to（from=" + std::to_string(from) +
              " to=" + std::to_string(to) + "）";
        return false;
    }
    if (bands_.find(id) != bands_.end()) {
        err = "band「" + id + "」已经声明过（每帧每条带只声明一次）";
        return false;
    }
    for (const auto& kv : bands_) {
        const band_def& b = kv.second;
        if (from <= b.to && to >= b.from) {
            err = "band「" + id + "」与「" + b.id + "」的 layer 区间重叠";
            return false;
        }
    }
    band_def b;
    b.id = id;
    b.from = from;
    b.to = to;
    b.committed = false;
    bands_.emplace(id, b);
    return true;
}

bool renderer::light_commit(const std::string& id, std::string& err) {
    const auto it = bands_.find(id);
    if (it == bands_.end()) {
        err = "commit 的 band「" + id + "」没有声明";
        return false;
    }
    if (it->second.committed) {
        err = "band「" + id + "」本帧已经 commit 过";
        return false;
    }
    std::vector<light::light> selected;
    std::vector<light::light> rest;
    selected.reserve(lights_.size());
    for (const light::light& l : lights_) {
        if (l.layer >= it->second.from && l.layer <= it->second.to) {
            selected.push_back(l);
        } else {
            rest.push_back(l);
        }
    }
    it->second.committed = true;
    ++frame_commits_;
    if (frame_commits_ > 8 && !warned_commits_) {
        warned_commits_ = true;
        new_over_commits_ = true;
        log::warn("light", log::format("commit 次数超过 8",
                               "本帧 " + std::to_string(frame_commits_) + " 次"));
    }
    if (selected.empty()) {
        band_mask_active_ = false;
        return true;
    }
    bool has_self_only = false;
    for (const light::light& l : selected) {
        if (l.r == light::reach::self_only) {
            has_self_only = true;
            break;
        }
    }
    if (has_self_only) {
        render_band_mask_(it->second.from, it->second.to);
    } else {
        band_mask_active_ = false;
    }
    // 先把本带之前收集的场景几何发射出去（commit 是"带边界"）。
    sort_and_emit_draws_();
    const std::uint32_t n = static_cast<std::uint32_t>(selected.size());
    if (n > 256 && !warned_light_count_) {
        warned_light_count_ = true;
        new_over_lights_ = true;
        log::warn("light", log::format("光源数超过 256",
                               "本带 " + std::to_string(n) + " 盏（建议拆带）"));
    }
    if (n > frame_lights_max_) {
        frame_lights_max_ = n;
    }
    lights_.swap(selected);
    render_light_pass_(); // 内部会清空 lights_
    band_mask_active_ = false;
    lights_ = std::move(rest);
    return true;
}

bool renderer::light_layer_in_band(std::int64_t layer) const {
    if (bands_.empty()) {
        return true;
    }
    for (const auto& kv : bands_) {
        if (layer >= kv.second.from && layer <= kv.second.to) {
            return true;
        }
    }
    return false;
}



double renderer::light_fill_ratio_() const noexcept {
    const double mw = static_cast<double>(
        (light_map_.w > 0) ? light_map_.w : ((fb_w_ / 2 > 0) ? (fb_w_ / 2) : 1));
    const double mh = static_cast<double>(
        (light_map_.h > 0) ? light_map_.h : ((fb_h_ / 2 > 0) ? (fb_h_ / 2) : 1));
    return static_cast<double>(frame_fill_px_) / (mw * mh);
}

renderer::light_stats_snapshot renderer::take_light_stats() noexcept {
    light_stats_snapshot s;
    s.lights = frame_lights_max_;
    s.occluders = static_cast<std::uint32_t>(occluder_count());
    s.bands = static_cast<std::uint32_t>(bands_.size());
    s.commits = frame_commits_;
    s.fill_px = frame_fill_px_;
    s.fill_ratio = light_fill_ratio_();
    s.over_lights = new_over_lights_;
    s.over_fill = new_over_fill_;
    s.over_commits = new_over_commits_;
    new_over_lights_ = false;
    new_over_fill_ = false;
    new_over_commits_ = false;
    last_light_stats_ = s;
    return s;
}

// =============================================================================
//  本带对象掩码（self_only；2026-11-18 灾后恢复，第 7 步）
//
//  【为什么要它】self_only 的光只照"本带自己的对象"；
//    光 shader 采样一张 RGBA8 掩码，r=覆盖。
//  【什么时候画】commit 里、sort_and_emit_draws_ 之前（之后 items 就空了）。
//  【坐标】draw_items 的顶点是**窗口像素**；用 frame_box_ 反算到渲染盒 0..1。
// =============================================================================

void renderer::render_band_mask_(std::int64_t from, std::int64_t to) {
    const int bw = light::map_width;
    const int bh = light::map_height;
    ensure_render_target_(band_mask_, bw, bh, false);
    bind_render_target_(band_mask_);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    std::vector<float> tris;
    for (const draw_item& it : draw_items_) {
        if (it.layer < from || it.layer > to) {
            continue;
        }
        if (it.vertex_begin + it.vertex_count > frame_vertices_.size()) {
            continue;
        }
        const vertex* v = frame_vertices_.data() + it.vertex_begin;
        const auto push = [&tris](const vertex& a) {
            tris.push_back(a.x);
            tris.push_back(a.y);
        };
        if (it.topology == 0) {
            for (std::uint32_t k = 1; k + 1 < it.vertex_count; ++k) {
                push(v[0]);
                push(v[k]);
                push(v[k + 1]);
            }
        } else {
            for (std::uint32_t k = 0; k < it.vertex_count; ++k) {
                push(v[k]);
            }
        }
    }
    if (tris.empty()) {
        unbind_render_target_();
        band_mask_active_ = false;
        return;
    }

    struct resources {
        gl_api::uint_t prog = 0;
        gl_api::uint_t vao = 0;
        gl_api::uint_t vbo = 0;
        gl_api::int_t u_box = -1;
        bool ready = false;
    };
    static resources r;
    if (!r.ready) {
        const char* vs_src = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
uniform vec4 u_box;
void main() {
    vec2 ndc = vec2((a_pos.x - u_box.x) / max(u_box.z, 1.0),
                    (a_pos.y - u_box.y) / max(u_box.w, 1.0));
    gl_Position = vec4(ndc.x * 2.0 - 1.0, 1.0 - ndc.y * 2.0, 0.0, 1.0);
}
)";
        const char* fs_src = R"(#version 330 core
out vec4 frag;
void main() { frag = vec4(1.0, 0.0, 0.0, 1.0); }
)";
        const gl_api::uint_t vs = light_compile_stage(gl_api::vertex_shader, vs_src, "带掩码顶点");
        const gl_api::uint_t fs = light_compile_stage(gl_api::fragment_shader, fs_src, "带掩码片元");
        r.prog = light_link_stage(vs, fs);
        gl_api::delete_shader(vs);
        gl_api::delete_shader(fs);
        gl_api::gen_vertex_arrays(1, &r.vao);
        gl_api::gen_buffers(1, &r.vbo);
        gl_api::bind_vertex_array(r.vao);
        gl_api::bind_buffer(gl_api::array_buffer, r.vbo);
        gl_api::vertex_attrib_pointer(0, 2, GL_FLOAT, 0, 2 * sizeof(float), nullptr);
        gl_api::enable_vertex_attrib_array(0);
        gl_api::bind_vertex_array(0);
        r.u_box = gl_api::get_uniform_location(r.prog, "u_box");
        r.ready = true;
    }

    gl_api::bind_vertex_array(r.vao);
    gl_api::bind_buffer(gl_api::array_buffer, r.vbo);
    gl_api::buffer_data(gl_api::array_buffer,
                        static_cast<gl_api::sizeiptr_t>(tris.size() * sizeof(float)),
                        tris.data(), gl_api::stream_draw);
    gl_api::use_program(r.prog);
    if (r.u_box >= 0) {
        gl_api::uniform_4f(r.u_box, frame_box_[0], frame_box_[1],
                           frame_box_[2], frame_box_[3]);
    }
    gl_api::draw_arrays(GL_TRIANGLES, 0, static_cast<gl_api::int_t>(tris.size() / 2));
    gl_api::bind_vertex_array(0);
    unbind_render_target_();
    band_mask_active_ = true;
}

void renderer::debug_band_selftest_() {
    static bool done = false;
    if (done) {
        return;
    }
    const char* env = std::getenv("WAG_BAND_SELFTEST");
    if (env == nullptr || env[0] == '\0' || env[0] == '0') {
        return;
    }
    done = true;

    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, static_cast<int>(frame_pixel_[0]),
               static_cast<int>(frame_pixel_[1]));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    std::string err;
    if (!light_band_declare("bg", 0, 0, err) ||
        !light_band_declare("top", 1, 1, err)) {
        fail_internal("带自检：声明失败（" + err + "）");
    }
    light::light red;
    red.x = -300.0f;
    red.y = 0.0f;
    red.radius = 260.0f;
    red.color = 0xFFFF0000u;
    red.intensity = 2.0f;
    red.layer = 0;
    red.softness = 2.0f;
    light_add(red);
    light::light green = red;
    green.x = 300.0f;
    green.color = 0xFF00FF00u;
    green.layer = 1;
    light_add(green);

    if (!light_commit("bg", err) || !light_commit("top", err)) {
        fail_internal("带自检：commit 失败（" + err + "）");
    }

    const auto to_px = [this](float world_x) {
        return static_cast<int>(frame_box_[0] +
                                (world_x + 960.0f) / 1920.0f * frame_box_[2]);
    };
    const int py = static_cast<int>(frame_box_[1] + frame_box_[3] * 0.5f);
    unsigned char left[4] = {0, 0, 0, 0};
    unsigned char right[4] = {0, 0, 0, 0};
    glReadPixels(to_px(-300.0f), py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left);
    glReadPixels(to_px(300.0f), py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, right);
    const bool ok = (left[0] > 20 && left[1] < 5 &&
                     right[1] > 20 && right[0] < 5);
    log::record("band", log::format("自检",
                            ok ? "PASS：两条带各自着色、互不串色"
                               : "FAIL：带的 layer 过滤不对"));
    bands_.clear();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}


void renderer::debug_selfonly_selftest_() {
    static bool done = false;
    if (done) {
        return;
    }
    const char* env = std::getenv("WAG_SELFONLY_SELFTEST");
    if (env == nullptr || env[0] == '\0' || env[0] == '0') {
        return;
    }
    done = true;

    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, static_cast<int>(frame_pixel_[0]),
               static_cast<int>(frame_pixel_[1]));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    const auto quad = [this](std::int64_t layer, float x, float y, float w,
                             float h, std::uint32_t color) {
        vertex v[4];
        v[0] = vertex{x, y, 0.0f, 0.0f, 0.0f, color};
        v[1] = vertex{x + w, y, 0.0f, 1.0f, 0.0f, color};
        v[2] = vertex{x + w, y - h, 0.0f, 1.0f, 1.0f, color};
        v[3] = vertex{x, y - h, 0.0f, 0.0f, 1.0f, color};
        draw_vertices(0, layer, 0.0, v, 4);
    };
    quad(0, -900.0f, 200.0f, 800.0f, 400.0f, 0xFF303030u);
    quad(2, 100.0f, 200.0f, 800.0f, 400.0f, 0xFF303030u);

    std::string err;
    if (!light_band_declare("ui", 2, 2, err)) {
        fail_internal("self_only 自检：声明带失败（" + err + "）");
    }
    light::light l;
    l.x = 0.0f;
    l.y = 0.0f;
    l.radius = 1200.0f;
    l.color = 0xFFFFFFFFu;
    l.intensity = 3.0f;
    l.layer = 2;
    l.r = light::reach::self_only;
    l.softness = 2.0f;
    light_add(l);
    if (!light_commit("ui", err)) {
        fail_internal("self_only 自检：commit 失败（" + err + "）");
    }

    const auto to_px = [this](float world_x) {
        return static_cast<int>(frame_box_[0] +
                                (world_x + 960.0f) / 1920.0f * frame_box_[2]);
    };
    const int py = static_cast<int>(frame_box_[1] + frame_box_[3] * 0.5f);
    unsigned char left[4] = {0, 0, 0, 0};
    unsigned char right[4] = {0, 0, 0, 0};
    glReadPixels(to_px(-500.0f), py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left);
    glReadPixels(to_px(500.0f), py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, right);
    const bool ok = (left[0] < 80 && right[0] > left[0] + 40);
    log::record("selfonly", log::format("自检",
                            ok ? "PASS：光只落在本带对象上"
                               : "FAIL：光漏到带外对象"));
    band_mask_active_ = false;
    bands_.clear();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void renderer::debug_budget_selftest_() {
    static bool done = false;
    if (done) {
        return;
    }
    const char* env = std::getenv("WAG_BUDGET_SELFTEST");
    if (env == nullptr || env[0] == '\0' || env[0] == '0') {
        return;
    }
    done = true;

    std::string err;
    for (int i = 0; i < 9; ++i) {
        const std::string id = "b" + std::to_string(i);
        if (!light_band_declare(id, i, i, err)) {
            fail_internal("预算自检：声明带失败（" + err + "）");
        }
    }
    light::light tiny;
    tiny.radius = 1.0f;
    tiny.intensity = 1.0f;
    tiny.layer = 0;
    for (int i = 0; i < 300; ++i) {
        tiny.x = static_cast<float>(i % 20) * 10.0f - 100.0f;
        light_add(tiny);
    }
    light::light huge = tiny;
    huge.radius = 5000.0f;
    light_add(huge);
    for (int i = 1; i < 9; ++i) {
        light::light one = tiny;
        one.x = 0.0f;
        one.layer = i;
        light_add(one);
    }
    for (int i = 0; i < 9; ++i) {
        if (!light_commit("b" + std::to_string(i), err)) {
            fail_internal("预算自检：commit 失败（" + err + "）");
        }
    }
    const light_stats_snapshot s = take_light_stats();
    const bool ok = s.over_lights && s.over_fill && s.over_commits &&
                    s.lights >= 300 && s.commits == 9;
    log::record("budget", log::format("自检",
                            ok ? "PASS：256 / 32× / 8 次 三条线都触发"
                               : "FAIL：预算标记没有全部触发"));
    bands_.clear();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}
} // namespace wag::draw