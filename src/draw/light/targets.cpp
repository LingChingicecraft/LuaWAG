// =============================================================================
//  draw/light/targets.cpp —— 离屏渲染目标（RGBA8 / RGBA16F）
//
//  【职责】只做“建 / 绑 / 解绑 / 毁”与一个自检。
//    它不碰光、不碰遮挡者、不碰带 —— 那些在 occluders.cpp / pass.cpp / bands.cpp。
//  【为什么单独一个文件】它是光照通道里唯一没有 shader 依赖的一块，
//    拆出来之后其余三块可以各自持有自己的 program/VAO，互不牵连。
// =============================================================================

#include "draw/renderer.hpp"

#include "draw/gl_api.hpp"
#include "error/error.hpp"
#include "log/log.hpp"

#if defined(WAG_HAVE_SDL3)
#include <SDL3/SDL_opengl.h>
#endif

#include <cmath>
#include <cstdlib>

namespace wag::draw {

namespace {
constexpr gl_api::enum_t kRgba16f = 0x881A;            // GL_RGBA16F
constexpr gl_api::enum_t kFramebufferBinding = 0x8CA6; // GL_FRAMEBUFFER_BINDING
} // namespace

void renderer::destroy_render_target_(render_target& t) {
    if (t.fbo != 0) {
        gl_api::delete_framebuffers(1, &t.fbo);
        t.fbo = 0;
    }
    if (t.tex != 0) {
        gl_api::delete_textures(1, &t.tex);
        t.tex = 0;
    }
    t.w = 0;
    t.h = 0;
    t.hdr = false;
}

void renderer::ensure_render_target_(render_target& t, int w, int h, bool hdr) {
    if (w <= 0 || h <= 0) {
        fail_internal("渲染目标的尺寸必须为正");
    }
    if (t.fbo != 0 && t.w == w && t.h == h && t.hdr == hdr) {
        return;
    }
    destroy_render_target_(t);
    while (gl_api::get_error() != gl_api::no_error) {
    }

    gl_api::gen_textures(1, &t.tex);
    if (t.tex == 0) {
        fail_internal("渲染目标：glGenTextures 返回 0");
    }
    gl_api::bind_texture(GL_TEXTURE_2D, t.tex);
    gl_api::tex_image_2d(GL_TEXTURE_2D, 0, hdr ? kRgba16f : GL_RGBA, w, h, 0,
                         GL_RGBA, hdr ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr);
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            static_cast<gl_api::int_t>(gl_api::clamp_to_edge));
    gl_api::tex_parameter_i(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            static_cast<gl_api::int_t>(gl_api::clamp_to_edge));

    gl_api::gen_framebuffers(1, &t.fbo);
    if (t.fbo == 0) {
        fail_internal("渲染目标：glGenFramebuffers 返回 0");
    }
    gl_api::bind_framebuffer(gl_api::framebuffer, t.fbo);
    gl_api::framebuffer_texture_2d(gl_api::framebuffer, gl_api::color_attachment0,
                                   GL_TEXTURE_2D, t.tex, 0);
    if (gl_api::check_framebuffer_status(gl_api::framebuffer) !=
        gl_api::framebuffer_complete) {
        gl_api::bind_framebuffer(gl_api::framebuffer, 0);
        fail_internal("渲染目标 FBO 不完整（尺寸或格式不被支持）");
    }
    gl_api::bind_framebuffer(gl_api::framebuffer, 0);

    if (gl_api::get_error() != gl_api::no_error) {
        fail_internal("渲染目标的纹理/FBO 创建失败");
    }
    t.w = w;
    t.h = h;
    t.hdr = hdr;
}

void renderer::bind_render_target_(render_target& t) {
    if (t.fbo == 0) {
        fail_internal("绑定渲染目标前未创建");
    }
    glGetIntegerv(GL_VIEWPORT, reinterpret_cast<GLint*>(saved_viewport_));
    glGetIntegerv(GL_SCISSOR_BOX, reinterpret_cast<GLint*>(saved_scissor_));
    saved_scissor_enabled_ = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    GLint fbo_now = 0;
    glGetIntegerv(static_cast<GLenum>(kFramebufferBinding), &fbo_now);
    saved_fbo_ = static_cast<gl_api::uint_t>(fbo_now);

    gl_api::bind_framebuffer(gl_api::framebuffer, t.fbo);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, t.w, t.h);
    target_bound_ = true;
}

void renderer::unbind_render_target_() {
    if (!target_bound_) {
        return;
    }
    gl_api::bind_framebuffer(gl_api::framebuffer, saved_fbo_);
    glViewport(saved_viewport_[0], saved_viewport_[1], saved_viewport_[2],
               saved_viewport_[3]);
    if (saved_scissor_enabled_) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(saved_scissor_[0], saved_scissor_[1], saved_scissor_[2],
                  saved_scissor_[3]);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    target_bound_ = false;
}

void renderer::debug_rt_selftest_() {
    static bool done = false;
    if (done) {
        return;
    }
    const char* env = std::getenv("WAG_RT_SELFTEST");
    if (env == nullptr || env[0] == '\0' || env[0] == '0') {
        return;
    }
    done = true;

    render_target rt8;
    ensure_render_target_(rt8, 64, 64, false);
    bind_render_target_(rt8);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char px[4] = {0, 0, 0, 0};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    unbind_render_target_();
    const bool ok8 = (px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255);
    destroy_render_target_(rt8);

    render_target rt16;
    ensure_render_target_(rt16, 32, 32, true);
    bind_render_target_(rt16);
    glClearColor(0.25f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    float fx[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, fx);
    unbind_render_target_();
    const bool ok16 = (std::fabs(fx[0] - 0.25f) < 0.01f);
    destroy_render_target_(rt16);

    log::record("rt", log::format("自检",
                            ok8 && ok16 ? "PASS：RGBA8 + RGBA16F"
                                        : "FAIL：见上"));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

} // namespace wag::draw
