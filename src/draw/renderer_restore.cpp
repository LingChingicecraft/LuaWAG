// =============================================================================
// draw/renderer_restore.cpp —— 灾后重建：图片族后半 + 锚点/帧表/字形
//
// 【为什么单独一个文件】
// 2026-11-18 一次工具误操作把 renderer.cpp 的换行全部抹掉；
// 这份文件把"后来加在 renderer.cpp 里的、与光照无关的成员函数"重新实现，
// 从而不必把整个 renderer.cpp 重写一遍。
// 【它定义什么】
// create_image_from_rgba / update_image_region / destroy_image_internal /
// define_frames / image_frame_count / image_frame_rect /
// set_image_anchor / image_anchor / draw_quad / draw_glyphs /
// is_reserved_image_name
// 【它不定义什么】renderer.cpp 里已有的核心（加载/卸载/槽位/绘制批）。
// =============================================================================

#include "draw/renderer.hpp"

#include "draw/gl_api.hpp"
#include "error/error.hpp"
#include "log/log.hpp"

#if defined(WAG_HAVE_SDL3)
#include <SDL3/SDL_opengl.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wag::draw {

bool renderer::is_reserved_image_name(const char* name) noexcept {
    return name != nullptr && name[0] == '_' && name[1] == '_';
}

bool renderer::is_reserved_image_handle(std::uint64_t handle) const noexcept {
    const std::uint32_t slot = static_cast<std::uint32_t>(handle & 0x1FFFFFu);
    const std::uint32_t gen = static_cast<std::uint32_t>(handle >> 21);
    if (slot >= images_.size()) {
        return false;
    }
    const image_slot& s = images_[slot];
    if (s.gen == 0 || s.gen != gen) {
        return false;
    }
    return is_reserved_image_name(s.name.c_str());
}

std::uint64_t renderer::create_image_from_rgba(const char* name, int w, int h,
                                               const unsigned char* rgba) {
    if (name == nullptr || *name == '\0' || w <= 0 || h <= 0 || rgba == nullptr) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "create_image_from_rgba 的参数";
        e.got = "name / w / h / rgba 有空值或非正尺寸";
        e.want = "非空名 + 正尺寸 + 非空像素";
        fail_boundary(e);
    }

 // ---- ① 槽位（同族同名 ⇒ 覆盖 = 同槽位 + 代数 +1）----
    std::uint32_t slot = 0;
    {
        const auto it = image_by_name_.find(name);
        if (it != image_by_name_.end()) {
            slot = it->second;
            unload_image_slot_(slot);
            image_free_.pop_back();
        } else if (!image_free_.empty()) {
            slot = image_free_.back();
            image_free_.pop_back();
        } else {
            if (images_.size() >= image_family_capacity) {
                error_value e;
                e.cls = error_class::resource_failure;
                e.where = "create_image_from_rgba 的图片槽位";
                e.got = "图片族的槽位已满";
                e.want = "先卸载一些图片（容量见 image_family_capacity）";
                fail_boundary(e);
            }
            slot = static_cast<std::uint32_t>(images_.size());
            images_.push_back(image_slot{});
        }
    }

 // ---- ② 纹理（RGBA8、linear、clamp）----
    while (gl_api::get_error() != gl_api::no_error) {
    }
    gl_api::uint_t tex = 0;
    gl_api::gen_textures(1, &tex);
    if (tex == 0) {
        error_value e;
        e.cls = error_class::resource_failure;
        e.where = "create_image_from_rgba 的纹理创建";
        e.got = "glGenTextures 返回 0";
        e.want = "一个可用的纹理名（显存/驱动正常）";
        fail_boundary(e);
    }
    gl_api::bind_texture(GL_TEXTURE_2D, tex);
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            static_cast<gl_api::int_t>(gl_api::clamp_to_edge));
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            static_cast<gl_api::int_t>(gl_api::clamp_to_edge));
    gl_api::tex_image_2d(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, rgba);
    gl_api::bind_texture(GL_TEXTURE_2D, 0);

    const gl_api::enum_t upload_err = gl_api::get_error();
    if (upload_err != gl_api::no_error) {
        gl_api::delete_textures(1, &tex);
        char got_buf[64];
        std::snprintf(got_buf, sizeof(got_buf), "GL error 0x%X",
                      static_cast<unsigned>(upload_err));
        error_value e;
        e.cls = error_class::resource_failure;
        e.where = "create_image_from_rgba 的纹理上传";
        e.got = got_buf;
        e.want = "glTexImage2D 成功（显存/驱动正常）";
        e.sentence = "图集页纹理上传失败（常见原因是显存不足）";
        fail_boundary(e);
    }

 // ---- ③ 落槽 ----
    image_slot& s = images_[slot];
 // 【 / 】复用槽位：先清掉上一张图的帧表与锚点。
    s.frames.clear();
    s.ax = 0.0f;
    s.ay = 0.0f;
    s.tex = tex;
    s.width = static_cast<std::uint32_t>(w);
    s.height = static_cast<std::uint32_t>(h);
    s.name = name;
    if (s.gen == 0) {
        s.gen = 1;
    }
    image_by_name_[name] = slot;
    warn_if_image_gen_too_high_(name, slot);
    update_image_watermark_();
    return (static_cast<std::uint64_t>(s.gen) << 21) |
           static_cast<std::uint64_t>(slot);
}

bool renderer::update_image_region(std::uint64_t handle, int x, int y, int w,
                                   int h, const unsigned char* rgba) {
    gl_api::uint_t tex = 0;
    double tw = 0.0;
    double th = 0.0;
    if (rgba == nullptr || w <= 0 || h <= 0) {
        return false;
    }
    if (!image_texture_(handle, tex, tw, th)) {
        return false;
    }
    if (x < 0 || y < 0 || static_cast<double>(x + w) > tw ||
        static_cast<double>(y + h) > th) {
        return false;
    }
    while (gl_api::get_error() != gl_api::no_error) {
    }
    gl_api::bind_texture(GL_TEXTURE_2D, tex);
    gl_api::tex_sub_image_2d(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA,
                             GL_UNSIGNED_BYTE, rgba);
    gl_api::bind_texture(GL_TEXTURE_2D, 0);
    return gl_api::get_error() == gl_api::no_error;
}

void renderer::destroy_image_internal(const char* name) noexcept {
    if (name == nullptr) {
        return;
    }
    const auto it = image_by_name_.find(name);
    if (it == image_by_name_.end()) {
        return;
    }
    unload_image_slot_(it->second);
    update_image_watermark_();
}

void renderer::define_frames(std::uint64_t handle,
                             const std::vector<frame_rect>& rects) {
    gl_api::uint_t tex = 0;
    double tex_w = 0.0;
    double tex_h = 0.0;
    if (!image_texture_(handle, tex, tex_w, tex_h)) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "define_frames 的图片句柄";
        e.got = std::to_string(handle);
        e.want = "一个仍有效的图片句柄（同名覆盖 / 卸载后旧句柄失效）";
        e.sentence = "给已失效的图片句柄挂帧表";
        fail_boundary(e);
    }
    if (rects.empty()) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "define_frames 的帧表";
        e.got = "空表";
        e.want = "至少一个 {x, y, w, h} 矩形；不想要帧表就不要调用本函数";
        fail_boundary(e);
    }
    if (rects.size() > 65535u) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "define_frames 的帧数";
        e.got = std::to_string(rects.size());
        e.want = "<= 65535（container.frame 是 u16）";
        fail_boundary(e);
    }
    for (std::size_t k = 0; k < rects.size(); ++k) {
        const frame_rect& f = rects[k];
        const bool bad = (f.w == 0 || f.h == 0 ||
                          static_cast<double>(f.x) + static_cast<double>(f.w) > tex_w ||
                          static_cast<double>(f.y) + static_cast<double>(f.h) > tex_h);
        if (!bad) {
            continue;
        }
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "define_frames 的第 " + std::to_string(k) + " 帧";
        char got_buf[128];
        std::snprintf(got_buf, sizeof(got_buf),
                      "x=%u y=%u w=%u h=%u（纹理 %ux%u）",
                      static_cast<unsigned>(f.x), static_cast<unsigned>(f.y),
                      static_cast<unsigned>(f.w), static_cast<unsigned>(f.h),
                      static_cast<unsigned>(tex_w),
                      static_cast<unsigned>(tex_h));
        e.got = got_buf;
        e.want = "w/h > 0 且整个矩形落在纹理像素范围内";
        e.sentence = "帧矩形越出纹理";
        fail_boundary(e);
    }
    const std::uint32_t slot = static_cast<std::uint32_t>(handle & 0x1FFFFFu);
    images_[slot].frames = rects;
}

std::uint32_t renderer::image_frame_count(std::uint64_t handle) const {
    const std::uint32_t slot = static_cast<std::uint32_t>(handle & 0x1FFFFFu);
    const std::uint32_t gen = static_cast<std::uint32_t>(handle >> 21);
    if (slot >= images_.size()) {
        return 0;
    }
    const image_slot& s = images_[slot];
    if (s.gen == 0 || s.gen != gen) {
        return 0;
    }
    return s.frames.empty() ? 1u : static_cast<std::uint32_t>(s.frames.size());
}

bool renderer::image_frame_rect(std::uint64_t handle, std::uint32_t frame,
                                frame_rect& out) const {
    const std::uint32_t slot = static_cast<std::uint32_t>(handle & 0x1FFFFFu);
    const std::uint32_t gen = static_cast<std::uint32_t>(handle >> 21);
    if (slot >= images_.size()) {
        return false;
    }
    const image_slot& s = images_[slot];
    if (s.gen == 0 || s.gen != gen) {
        return false;
    }
    if (s.frames.empty()) {
        if (frame != 0u) {
            return false;
        }
        out = frame_rect{0u, 0u, s.width, s.height};
        return true;
    }
    if (frame >= s.frames.size()) {
        return false;
    }
    out = s.frames[frame];
    return true;
}

void renderer::set_image_anchor(std::uint64_t handle, double ax, double ay) {
    gl_api::uint_t tex = 0;
    double tex_w = 0.0;
    double tex_h = 0.0;
    if (!image_texture_(handle, tex, tex_w, tex_h)) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "anchor 的图片句柄";
        e.got = image_desc_(handle);
        e.want = "一个仍有效的图片句柄（同名覆盖 / 卸载后旧句柄失效）";
        fail_boundary(e);
    }
    if (!std::isfinite(ax) || !std::isfinite(ay)) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "anchor 的 ax / ay";
        char got_buf[96];
        std::snprintf(got_buf, sizeof(got_buf), "ax=%g ay=%g", ax, ay);
        e.got = got_buf;
        e.want = "有限实数（归一化；默认 0,0 = 左上；允许 0..1 之外）";
        fail_boundary(e);
    }
    const std::uint32_t slot = static_cast<std::uint32_t>(handle & 0x1FFFFFu);
    images_[slot].ax = static_cast<float>(ax);
    images_[slot].ay = static_cast<float>(ay);
}

bool renderer::image_anchor(std::uint64_t handle, double& out_ax,
                            double& out_ay) const {
    const std::uint32_t slot = static_cast<std::uint32_t>(handle & 0x1FFFFFu);
    const std::uint32_t gen = static_cast<std::uint32_t>(handle >> 21);
    if (slot >= images_.size()) {
        return false;
    }
    const image_slot& s = images_[slot];
    if (s.gen == 0 || s.gen != gen) {
        return false;
    }
    out_ax = static_cast<double>(s.ax);
    out_ay = static_cast<double>(s.ay);
    return true;
}

void renderer::collect_quad(std::uint64_t image, std::int64_t layer, double z,
                            const double xy[8], double u0, double v0,
                            double u1, double v1) {
    const vertex quad[4] = {
        {static_cast<float>(xy[0]), static_cast<float>(xy[1]), 0.0f,
         static_cast<float>(u0), static_cast<float>(v0), 0xFFFFFFFFu},
        {static_cast<float>(xy[2]), static_cast<float>(xy[3]), 0.0f,
         static_cast<float>(u1), static_cast<float>(v0), 0xFFFFFFFFu},
        {static_cast<float>(xy[4]), static_cast<float>(xy[5]), 0.0f,
         static_cast<float>(u1), static_cast<float>(v1), 0xFFFFFFFFu},
        {static_cast<float>(xy[6]), static_cast<float>(xy[7]), 0.0f,
         static_cast<float>(u0), static_cast<float>(v1), 0xFFFFFFFFu},
    };
    draw_vertices(image, layer, z, quad, 4);
}

void renderer::draw_quad(std::uint64_t image, std::int64_t layer, double z,
                         const double* points) {
    if (points == nullptr) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "draw_quad 的点数组";
        e.got = "nullptr";
        e.want = "12 个数（4 个角 × x,y,z）";
        fail_boundary(e);
    }
    if (!std::isfinite(z)) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "draw_quad 的 z";
        e.got = std::isnan(z) ? "nan" : "inf";
        e.want = "有限实数";
        fail_boundary(e);
    }
    double tex_w = 1.0;
    double tex_h = 1.0;
    if (image != 0) {
        gl_api::uint_t tex = 0;
        if (!image_texture_(image, tex, tex_w, tex_h)) {
            error_value e;
            e.cls = error_class::boundary_validation;
            e.where = "draw_quad 的 image";
            e.got = image_desc_(image);
            e.want = "一个仍有效的图片句柄（0 = 白纹理）";
            fail_boundary(e);
        }
    }
    const double xy[8] = {points[0], points[1], points[3], points[4],
                          points[6], points[7], points[9], points[10]};
    collect_quad(image, layer, z, xy, 0.0, 0.0, tex_w, tex_h);
}

void renderer::draw_glyphs(std::uint64_t image, std::int64_t layer, double z,
                           const double* flat, std::size_t count) {
    constexpr std::size_t kPerGlyph = 9;
    if (count == 0) {
        return;
    }
    if (flat == nullptr || count % kPerGlyph != 0) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "draw_glyphs 的数组";
        e.got = "空指针或长度不是 9 的倍数";
        e.want = "每图元 9 个数（x,y,w,h,u,v,uw,uh,color）";
        fail_boundary(e);
    }
    const std::size_t glyphs = count / kPerGlyph;
    for (std::size_t k = 0; k < glyphs; ++k) {
        const double* g = flat + k * kPerGlyph;
        const double x = g[0];
        const double y = g[1];
        const double w = g[2];
        const double h = g[3];
        const double u = g[4];
        const double v = g[5];
        const double uw = g[6];
        const double vh = g[7];
        const auto color = static_cast<std::uint32_t>(g[8]);
        vertex q[4];
        q[0] = vertex{static_cast<float>(x), static_cast<float>(y), 0.0f,
                      static_cast<float>(u), static_cast<float>(v), color};
        q[1] = vertex{static_cast<float>(x + w), static_cast<float>(y), 0.0f,
                      static_cast<float>(u + uw), static_cast<float>(v), color};
        q[2] = vertex{static_cast<float>(x + w),
                      static_cast<float>(y - h), 0.0f,
                      static_cast<float>(u + uw),
                      static_cast<float>(v + vh), color};
        q[3] = vertex{static_cast<float>(x),
                      static_cast<float>(y - h), 0.0f,
                      static_cast<float>(u),
                      static_cast<float>(v + vh), color};
        draw_vertices(image, layer, z, q, 4);
    }
}

} // namespace wag::draw
