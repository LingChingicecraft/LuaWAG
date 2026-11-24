// =============================================================================
//  draw/light/occluders.cpp —— 遮挡者池 + 遮蔽图
//
//  【职责】
//    · 池的增删改查（句柄规则见 D1）
//    · 每帧把存活的遮挡者画进半分辨率 RGBA8 遮蔽图（R=覆盖，B=透射）
//    · 一个自检：近遮挡者必须覆盖远遮挡者
//  【坐标】世界坐标 → 光图 NDC：由当前 light space 的 l/b/w/h 决定
//  （默认取启动后的 render；见 wag.light.set_space）。
// =============================================================================

#include "draw/light/shader_util.hpp"
#include "draw/renderer.hpp"

#include "draw/gl_api.hpp"
#include "error/error.hpp"
#include "log/log.hpp"

#if defined(WAG_HAVE_SDL3)
#include <SDL3/SDL_opengl.h>
#endif

#include <algorithm>
#include <string>
#include <vector>

namespace wag::draw {
bool renderer::light_occluders_new(const std::string& name) {
    if (occluder_pools_.find(name) != occluder_pools_.end()) {
        return false;
    }
    occluder_pools_.emplace(name, light::occluder_pool{});
    occluder_pool_ids_[name] = next_occluder_pool_id_++;
    return true;
}

std::uint64_t renderer::light_occluders_spawn(const std::string& name,
                                              const light::occluder& o) {
    auto it = occluder_pools_.find(name);
    if (it == occluder_pools_.end()) {
        return 0;
    }
    if (it->second.alive_count() >= light::occluder_pool::capacity) {
        return 0;
    }
    const std::uint64_t local = it->second.spawn(o);
    if (local == 0) {
        return 0;
    }
    const std::uint64_t gen = local >> 15;
    const std::uint64_t index = local & 0x7FFFu;
    const std::uint64_t pool = static_cast<std::uint64_t>(occluder_pool_ids_[name]);
    return (gen << 27) | (pool << 15) | index;
}

bool renderer::light_occluders_write(std::uint64_t h, const light::occluder& o) {
    const std::uint64_t index = h & 0x7FFFu;
    const std::uint64_t pool = (h >> 15) & 0xFFFu;
    const std::uint64_t gen = h >> 27;
    for (auto& kv : occluder_pools_) {
        const auto id_it = occluder_pool_ids_.find(kv.first);
        if (id_it == occluder_pool_ids_.end() ||
            static_cast<std::uint64_t>(id_it->second) != pool) {
            continue;
        }
        return kv.second.write((gen << 15) | index, o);
    }
    return false;
}

bool renderer::light_occluders_kill(std::uint64_t h) {
    const std::uint64_t index = h & 0x7FFFu;
    const std::uint64_t pool = (h >> 15) & 0xFFFu;
    const std::uint64_t gen = h >> 27;
    for (auto& kv : occluder_pools_) {
        const auto id_it = occluder_pool_ids_.find(kv.first);
        if (id_it == occluder_pool_ids_.end() ||
            static_cast<std::uint64_t>(id_it->second) != pool) {
            continue;
        }
        return kv.second.kill((gen << 15) | index);
    }
    return false;
}

bool renderer::light_occluders_alive(std::uint64_t h) const {
    const std::uint64_t index = h & 0x7FFFu;
    const std::uint64_t pool = (h >> 15) & 0xFFFu;
    const std::uint64_t gen = h >> 27;
    if (index >= light::occluder_pool::capacity) {
        return false;
    }
    for (const auto& kv : occluder_pools_) {
        const auto id_it = occluder_pool_ids_.find(kv.first);
        if (id_it == occluder_pool_ids_.end() ||
            static_cast<std::uint64_t>(id_it->second) != pool) {
            continue;
        }
        const light::occluder& o = kv.second.at(static_cast<std::uint32_t>(index));
        return o.alive && o.gen == gen;
    }
    return false;
}

std::size_t renderer::occluder_count() const noexcept {
    std::size_t n = 0;
    for (const auto& kv : occluder_pools_) {
        n += kv.second.alive_count();
    }
    return n;
}

// =============================================================================
//  遮挡者 → 遮蔽图（2026-11-18 灾后恢复，第 4 步）
//
//  遮蔽图约定（与 light.fs 对齐）：
//    R = 覆盖（1 = 挡住）
//    B = 透射（0 = 全挡，1 = 全透）
//    G = 0
//  绘制顺序：depth 大的先画、小的后画（近的覆盖远的）。
// =============================================================================

void renderer::render_occluder_mask_() {
    const int mw = light::map_width;
    const int mh = light::map_height;
    ensure_render_target_(occluder_mask_, mw, mh, false);
    bind_render_target_(occluder_mask_);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    struct inst {
        float x, y, w, h, rot, trans;
        std::uint8_t depth;
    };
    std::vector<inst> list;
    for (const auto& kv : occluder_pools_) {
        kv.second.for_each_live([&list](const light::occluder& o) {
            list.push_back(inst{o.x, o.y, o.w, o.h, o.rot,
                                static_cast<float>(o.transmission) / 255.0f,
                                o.depth});
        });
    }
    if (list.empty()) {
        unbind_render_target_();
        glEnable(GL_BLEND);
        return;
    }
    std::sort(list.begin(), list.end(),
              [](const inst& a, const inst& b) { return a.depth > b.depth; });

    struct resources {
        gl_api::uint_t vao = 0;
        gl_api::uint_t quad = 0;
        gl_api::uint_t inst = 0;
        gl_api::uint_t prog = 0;
        gl_api::int_t u_light_space = -1;
        bool ready = false;
    };
    static resources r;
    if (!r.ready) {
        const char* vs_src = R"(#version 330 core
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec4 a_rect;
layout(location = 2) in float a_rot;
layout(location = 3) in float a_trans;
uniform vec4 u_light_space;
out float v_trans;
void main() {
    vec2 c = a_rect.xy;
    vec2 hs = a_rect.zw * 0.5;
    vec2 local = (a_corner * 2.0 - 1.0) * hs;
    float rad = radians(a_rot);
    float cs = cos(rad);
    float sn = sin(rad);
    vec2 p = c + vec2(local.x * cs - local.y * sn,
                      local.x * sn + local.y * cs);
    vec2 ndc = vec2((p.x - u_light_space.x) / max(u_light_space.z, 0.0001),
                    (p.y - u_light_space.y) / max(u_light_space.w, 0.0001));
    gl_Position = vec4(ndc * 2.0 - 1.0, 0.0, 1.0);
    v_trans = a_trans;
}
)";
        const char* fs_src = R"(#version 330 core
in float v_trans;
out vec4 frag;
void main() { frag = vec4(1.0, 0.0, clamp(v_trans, 0.0, 1.0), 1.0); }
)";
        const gl_api::uint_t vs = light_compile_stage(gl_api::vertex_shader, vs_src, "遮蔽顶点");
        const gl_api::uint_t fs = light_compile_stage(gl_api::fragment_shader, fs_src, "遮蔽片元");
        r.prog = light_link_stage(vs, fs);
        gl_api::delete_shader(vs);
        gl_api::delete_shader(fs);

        r.u_light_space = gl_api::get_uniform_location(r.prog, "u_light_space");

        gl_api::gen_vertex_arrays(1, &r.vao);
        gl_api::gen_buffers(1, &r.quad);
        gl_api::gen_buffers(1, &r.inst);
        gl_api::bind_vertex_array(r.vao);
        const float quad[8] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
        gl_api::bind_buffer(gl_api::array_buffer, r.quad);
        gl_api::buffer_data(gl_api::array_buffer,
                            static_cast<gl_api::sizeiptr_t>(sizeof(quad)), quad,
                            gl_api::stream_draw);
        gl_api::vertex_attrib_pointer(0, 2, GL_FLOAT, 0, 2 * sizeof(float), nullptr);
        gl_api::enable_vertex_attrib_array(0);
        gl_api::bind_buffer(gl_api::array_buffer, r.inst);
        gl_api::vertex_attrib_pointer(1, 4, GL_FLOAT, 0, sizeof(inst),
                                      reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0)));
        gl_api::enable_vertex_attrib_array(1);
        gl_api::vertex_attrib_pointer(2, 1, GL_FLOAT, 0, sizeof(inst),
                                      reinterpret_cast<const void*>(static_cast<std::uintptr_t>(4 * sizeof(float))));
        gl_api::enable_vertex_attrib_array(2);
        gl_api::vertex_attrib_pointer(3, 1, GL_FLOAT, 0, sizeof(inst),
                                      reinterpret_cast<const void*>(static_cast<std::uintptr_t>(5 * sizeof(float))));
        gl_api::enable_vertex_attrib_array(3);
        gl_api::vertex_attrib_divisor(1, 1);
        gl_api::vertex_attrib_divisor(2, 1);
        gl_api::vertex_attrib_divisor(3, 1);
        gl_api::bind_vertex_array(0);
        r.ready = true;
    }

    gl_api::bind_vertex_array(r.vao);
    gl_api::bind_buffer(gl_api::array_buffer, r.inst);
    gl_api::buffer_data(gl_api::array_buffer,
                        static_cast<gl_api::sizeiptr_t>(list.size() * sizeof(inst)),
                        list.data(), gl_api::stream_draw);
    gl_api::use_program(r.prog);
    if (r.u_light_space >= 0) {
        gl_api::uniform_4f(r.u_light_space,
                           static_cast<float>(light_space_.l),
                           static_cast<float>(light_space_.b),
                           static_cast<float>(light_space_.w),
                           static_cast<float>(light_space_.h));
    }
    gl_api::draw_arrays_instanced(GL_TRIANGLE_STRIP, 0, 4,
                                  static_cast<gl_api::int_t>(list.size()));
    gl_api::bind_vertex_array(0);
    unbind_render_target_();
    glEnable(GL_BLEND);
}

void renderer::debug_mask_selftest_() {
    static bool done = false;
    if (done) {
        return;
    }
    const char* env = std::getenv("WAG_MASK_SELFTEST");
    if (env == nullptr || env[0] == '\0' || env[0] == '0') {
        return;
    }
    done = true;

    const std::string pool = "__mask_selftest";
    if (!light_occluders_new(pool)) {
        log::record("mask", "自检：池已存在，跳过");
        return;
    }
    light::occluder far_o;
    far_o.x = 0.0f;
    far_o.y = 0.0f;
    far_o.w = 300.0f;
    far_o.h = 300.0f;
    far_o.depth = 200;
    far_o.transmission = 128;
    const auto far_h = light_occluders_spawn(pool, far_o);
    light::occluder near_o = far_o;
    near_o.w = 120.0f;
    near_o.h = 120.0f;
    near_o.depth = 10;
    near_o.transmission = 0;
    const auto near_h = light_occluders_spawn(pool, near_o);
    if (far_h == 0 || near_h == 0) {
        log::record("mask", "自检：遮挡者生成失败，跳过");
        occluder_pools_.erase(pool);
        occluder_pool_ids_.erase(pool);
        return;
    }

    render_occluder_mask_();
    bind_render_target_(occluder_mask_);
    unsigned char px[4] = {0, 0, 0, 0};
    glReadPixels(occluder_mask_.w / 2, occluder_mask_.h / 2, 1, 1, GL_RGBA,
                 GL_UNSIGNED_BYTE, px);
    unbind_render_target_();

    log::record("mask", log::format("px",
        std::string("r=") + std::to_string(static_cast<int>(px[0])) +
        " g=" + std::to_string(static_cast<int>(px[1])) +
        " b=" + std::to_string(static_cast<int>(px[2])) +
        " a=" + std::to_string(static_cast<int>(px[3])) +
        " size=" + std::to_string(occluder_mask_.w) + "x" + std::to_string(occluder_mask_.h)));
    const bool ok = (px[0] > 250 && px[2] < 5);
    log::record("mask", log::format("自检",
                            ok ? "PASS：近遮挡者覆盖远遮挡者"
                               : "FAIL：近遮挡者没有赢"));
    occluder_pools_.erase(pool);
    occluder_pool_ids_.erase(pool);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}
} // namespace wag::draw