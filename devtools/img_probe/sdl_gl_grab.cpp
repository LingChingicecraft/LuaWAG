#include "sdl_gl_grab.hpp"

#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

namespace imgprobe::sdl_gl {
namespace {

SDL_Window* g_window = nullptr;
SDL_GLContext g_ctx = nullptr;
int g_w = 0;
int g_h = 0;
unsigned g_last_error = 0;

void remember_gl_error() noexcept {
    const GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        g_last_error = static_cast<unsigned>(e);
    }
}

} // namespace

bool create(const config& cfg, std::string& err) {
    if (g_ctx != nullptr) {
        return true;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        err = std::string("SDL_Init(VIDEO) 失败: ") + SDL_GetError();
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    if (cfg.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }

    g_window = SDL_CreateWindow(cfg.title, cfg.w, cfg.h, flags);
    if (g_window == nullptr) {
        err = std::string("SDL_CreateWindow 失败: ") + SDL_GetError();
        SDL_Quit();
        return false;
    }

    g_ctx = SDL_GL_CreateContext(g_window);
    if (g_ctx == nullptr) {
        err = std::string("SDL_GL_CreateContext 失败: ") + SDL_GetError();
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
        SDL_Quit();
        return false;
    }

    if (!SDL_GL_MakeCurrent(g_window, g_ctx)) {
        err = std::string("SDL_GL_MakeCurrent 失败: ") + SDL_GetError();
    }

    // 与引擎一致：vsync 关。抓帧要的是"立刻可读"，不是"等垂直同步"。
    SDL_GL_SetSwapInterval(0);

    int fw = 0;
    int fh = 0;
    SDL_GetWindowSizeInPixels(g_window, &fw, &fh);
    g_w = fw;
    g_h = fh;

    reset_state();
    remember_gl_error();
    return true;
}

void destroy() {
    if (g_ctx != nullptr) {
        SDL_GL_DestroyContext(g_ctx);
        g_ctx = nullptr;
    }
    if (g_window != nullptr) {
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
    }
    SDL_Quit();
    g_w = 0;
    g_h = 0;
}

bool alive() noexcept { return g_ctx != nullptr; }
int fb_w() noexcept { return g_w; }
int fb_h() noexcept { return g_h; }

void reset_state() {
    if (g_ctx == nullptr) {
        return;
    }
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, g_w, g_h);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
}

void clear_all(std::uint32_t argb) {
    if (g_ctx == nullptr) {
        return;
    }
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, g_w, g_h);
    glClearColor(static_cast<GLfloat>(red_of(argb)) / 255.0f,
                 static_cast<GLfloat>(green_of(argb)) / 255.0f,
                 static_cast<GLfloat>(blue_of(argb)) / 255.0f,
                 static_cast<GLfloat>(alpha_of(argb)) / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void clear_rect(imgprobe::rect r, std::uint32_t argb) {
    if (g_ctx == nullptr || !r.valid()) {
        return;
    }
    // 左上原点 → GL 左下原点：y 只在这里翻一次。
    const int y_gl = g_h - (r.y + r.h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(r.x, y_gl, r.w, r.h);
    glClearColor(static_cast<GLfloat>(red_of(argb)) / 255.0f,
                 static_cast<GLfloat>(green_of(argb)) / 255.0f,
                 static_cast<GLfloat>(blue_of(argb)) / 255.0f,
                 static_cast<GLfloat>(alpha_of(argb)) / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

imgprobe::image grab(std::string& err) {
    imgprobe::image img;
    if (g_ctx == nullptr) {
        err = "grab: 没有 GL 上下文";
        return img;
    }
    if (g_w <= 0 || g_h <= 0) {
        err = "grab: 帧缓冲尺寸为 0（窗口最小化？）";
        return img;
    }

    img.w = g_w;
    img.h = g_h;
    img.rgba.assign(static_cast<std::size_t>(g_w) * static_cast<std::size_t>(g_h) * 4u, 0);

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    // 默认帧缓冲 + 后缓冲 = draw framebuffer，就是刚画完的那一帧。
    glReadPixels(0, 0, g_w, g_h, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
    remember_gl_error();

    // GL 原点在左下 ⇒ 翻成左上，只在这里翻一次。
    const std::size_t stride = static_cast<std::size_t>(g_w) * 4u;
    std::vector<std::uint8_t> row(stride);
    for (int y = 0; y < g_h / 2; ++y) {
        std::uint8_t* top = img.rgba.data() + static_cast<std::size_t>(y) * stride;
        std::uint8_t* bottom = img.rgba.data() + static_cast<std::size_t>(g_h - 1 - y) * stride;
        std::copy(top, top + stride, row.begin());
        std::copy(bottom, bottom + stride, top);
        std::copy(row.begin(), row.end(), bottom);
    }
    return img;
}

void present() {
    if (g_window != nullptr) {
        SDL_GL_SwapWindow(g_window);
    }
}

unsigned last_gl_error() noexcept { return g_last_error; }

} // namespace imgprobe::sdl_gl
