// =============================================================================
//  sdl3_dpi_probe —— 确证"进程的 DPI 感知模式"，并用 Win32 交叉验证尺寸
//
//  【为什么还要问 OS，而不能只看 SDL 报的数】
//      `sdl3_density_probe` 的结果：
//          · SDL_WINDOW_HIGH_PIXEL_DENSITY **完全无效**（A 与 B 逐项相同）
//          · pixel_density = 1.000000，而 display_scale = 1.500000（两个量不同）
//          · 窗口坐标 = 像素 = GL 帧缓冲 = 1920×1080，而显示器物理 = 2560×1600
//      而人眼判据给出："窗口约占屏幕宽的 3/4" ⇒ 1920/2560 = 0.75
//          ⇒ 说明那 1920 **就是物理像素**（若被系统放大 1.5 倍，窗口会占满甚至超出）
//      本探针把"进程是 DPI 感知"这一条**从推断变成确证**，并给 display_scale
//      一个权威语义（它是 Windows 的 UI 缩放，而不是"物理/逻辑比例"）。
//
//  【本探针测四件事】
//      ① 进程 / 线程的 DPI 感知模式（两路查询，互相印证）
//      ② GDI 的系统 DPI（LOGPIXELSX/Y，96 = 100%、144 = 150%）
//      ③ 走 Win32（**不经过 SDL**）量同一个窗口：GetClientRect / GetWindowRect /
//         GetDpiForWindow，并与 SDL 报的三个尺寸对照
//      ④ 最后用"窗口物理尺寸 / 桌面物理尺寸"给出可判读的结论
//
//  【它不进引擎源码、不进 CMake】与 build/ 下其它探针同性质，用完可删。
//  编译与运行见本文件末尾。
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
// ⚠️ SDL_opengl.h（在 SDL.h 之后被包含）**已经定义过** WIN32_LEAN_AND_MEAN，
//    所以这里必须带 #ifndef —— 否则得到一个"重定义"警告。
//    （与项目里 NOMINMAX 那次是同一个形状。）
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
// GetProcessDpiAwareness 在 shellscalingapi.h 里，而它被 NTDDI_VERSION 的门挡着。
// 为了让本探针在任何 SDK 配置下都能编，**动态取**它（而不是链接期依赖）。
typedef HRESULT (WINAPI *PFN_GetProcessDpiAwareness)(HANDLE, int*);
#endif

namespace {

#if defined(_WIN32)
const char* process_awareness_name(int a) {
    switch (a) {
    case 0: return "PROCESS_DPI_UNAWARE（不感知）";
    case 1: return "PROCESS_SYSTEM_DPI_AWARE（系统级感知）";
    case 2: return "PROCESS_PER_MONITOR_DPI_AWARE（每显示器感知）";
    default: return "(未知)";
    }
}

const char* context_awareness_name(DPI_AWARENESS a) {
    switch (a) {
    case DPI_AWARENESS_INVALID:            return "DPI_AWARENESS_INVALID";
    case DPI_AWARENESS_UNAWARE:            return "DPI_AWARENESS_UNAWARE（不感知）";
    case DPI_AWARENESS_SYSTEM_AWARE:       return "DPI_AWARENESS_SYSTEM_AWARE（系统级感知）";
    case DPI_AWARENESS_PER_MONITOR_AWARE:  return "DPI_AWARENESS_PER_MONITOR_AWARE（每显示器感知）";
    default: return "(未知)";
    }
}
#endif

} // namespace

int main(int argc, char** argv) {
    const int req_w = (argc >= 2) ? std::atoi(argv[1]) : 1920;
    const int req_h = (argc >= 3) ? std::atoi(argv[2]) : 1080;

    std::printf("=============================================================\n");
    std::printf(" DPI 感知与尺寸交叉验证探针\n");
    std::printf(" 请求尺寸 = %d x %d\n", req_w, req_h);
    std::printf("=============================================================\n\n");

#if defined(_WIN32)
    // ---------------------------------------------------------------- ① 感知模式
    std::printf("【一】DPI 感知模式（进程 / 线程级）\n");

    std::printf("    GetThreadDpiAwarenessContext()      = %s\n",
                context_awareness_name(GetAwarenessFromDpiAwarenessContext(
                    GetThreadDpiAwarenessContext())));

    // 进程级的那个：动态取，避开 SDK 版本门
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        auto p = reinterpret_cast<PFN_GetProcessDpiAwareness>(
            reinterpret_cast<void*>(GetProcAddress(shcore, "GetProcessDpiAwareness")));
        if (p != nullptr) {
            int aw = -1;
            const HRESULT hr = p(nullptr, &aw);
            std::printf("    GetProcessDpiAwareness(NULL)        = hr=0x%08lX  %s\n",
                        static_cast<unsigned long>(hr), process_awareness_name(aw));
        } else {
            std::printf("    GetProcessDpiAwareness: 取地址失败\n");
        }
    } else {
        std::printf("    shcore.dll 未加载\n");
    }

    // ---------------------------------------------------------------- ② 系统 DPI
    std::printf("\n【二】GDI 报的 DPI 与桌面尺寸\n");
    if (HDC screen = GetDC(nullptr)) {
        std::printf("    GetDeviceCaps(LOGPIXELSX/Y) = %d / %d   （96=100%%，120=125%%，144=150%%）\n",
                    GetDeviceCaps(screen, LOGPIXELSX), GetDeviceCaps(screen, LOGPIXELSY));
        ReleaseDC(nullptr, screen);
    }
    std::printf("    GetSystemMetrics(SM_CXSCREEN/CYSCREEN) = %d x %d   （物理像素）\n",
                GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    std::printf("\n");
#endif

    // ---------------------------------------------------------------- SDL 侧
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow("dpi probe",
                                       req_w, req_h,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (win == nullptr) {
        std::printf("SDL_CreateWindow 失败：%s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    SDL_ShowWindow(win);
    SDL_PumpEvents();
    SDL_Delay(400);

    int coord_w = 0, coord_h = 0, pix_w = 0, pix_h = 0;
    SDL_GetWindowSize(win, &coord_w, &coord_h);
    SDL_GetWindowSizeInPixels(win, &pix_w, &pix_h);
    const float display_scale = SDL_GetWindowDisplayScale(win);
    const float pixel_density = SDL_GetWindowPixelDensity(win);

    int gl_vp[4] = {-1, -1, -1, -1};
    if (gl != nullptr) {
        SDL_GL_MakeCurrent(win, gl);
        glGetIntegerv(GL_VIEWPORT, gl_vp);
    }

    std::printf("【三】SDL 报的数\n");
    std::printf("    窗口坐标 = %d x %d      (SDL_GetWindowSize)\n", coord_w, coord_h);
    std::printf("    像素     = %d x %d      (SDL_GetWindowSizeInPixels)\n", pix_w, pix_h);
    std::printf("    GL 帧缓冲= %d x %d      (glGetIntegerv(GL_VIEWPORT))\n", gl_vp[2], gl_vp[3]);
    std::printf("    display_scale = %.4f    pixel_density = %.4f\n",
                static_cast<double>(display_scale), static_cast<double>(pixel_density));
    std::printf("    （注意：这两个量**不相等**，它们不是同一件事）\n\n");

#if defined(_WIN32)
    // ---------------------------------------------------------------- ③ Win32 交叉
    std::printf("【四】走 Win32 量同一个窗口（**不经过 SDL**）\n");
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    std::printf("    HWND = %p（取自 SDL 的窗口属性）\n", static_cast<void*>(hwnd));

    // ★ **定案用的那一路**：查**我们窗口自己**的感知上下文。
    //   上一版探针只查了"桌面窗口"的，那不是我们要的答案。
    if (hwnd != nullptr) {
        std::printf("    GetWindowDpiAwarenessContext(本窗口) = %s   ★ 这一路才是定案\n",
                    context_awareness_name(GetAwarenessFromDpiAwarenessContext(
                        GetWindowDpiAwarenessContext(hwnd))));
    }

    long client_w = -1, client_h = -1;
    if (hwnd != nullptr) {
        RECT cr{};
        RECT wr{};
        if (GetClientRect(hwnd, &cr)) {
            client_w = cr.right - cr.left;
            client_h = cr.bottom - cr.top;
            std::printf("    GetClientRect  = %ld x %ld（客户区）\n", client_w, client_h);
        }
        if (GetWindowRect(hwnd, &wr)) {
            std::printf("    GetWindowRect  = x=%ld y=%ld w=%ld h=%ld（含边框/标题栏）\n",
                        static_cast<long>(wr.left), static_cast<long>(wr.top),
                        static_cast<long>(wr.right - wr.left),
                        static_cast<long>(wr.bottom - wr.top));
        }
        const UINT d = GetDpiForWindow(hwnd);
        std::printf("    GetDpiForWindow= %u   ⇒ 比例 = %.4f\n",
                    d, static_cast<double>(d) / 96.0);
    }

    // ---------------------------------------------------------------- 判据
    std::printf("\n【五】判据\n");

    // 把 SDL 报的**物理**桌面尺寸取来（SDL 走显示模式，不经 DPI 虚拟化）
    int desk_phys_w = 0, desk_phys_h = 0;
    if (SDL_DisplayID disp = SDL_GetPrimaryDisplay()) {
        if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(disp)) {
            desk_phys_w = m->w;
            desk_phys_h = m->h;
        }
    }
    const int desk_virt_w = GetSystemMetrics(SM_CXSCREEN);
    const int desk_virt_h = GetSystemMetrics(SM_CYSCREEN);

    std::printf("    桌面（SDL 显示模式，**不经虚拟化**） = %d x %d\n", desk_phys_w, desk_phys_h);
    std::printf("    桌面（GetSystemMetrics，**经虚拟化**） = %d x %d\n", desk_virt_w, desk_virt_h);
    if (desk_virt_w > 0) {
        std::printf("    ⇒ 虚拟化比例 = %d / %d = %.4f   （=%.0f%% 缩放的签名）\n",
                    desk_phys_w, desk_virt_w,
                    static_cast<double>(desk_phys_w) / static_cast<double>(desk_virt_w),
                    100.0 * static_cast<double>(desk_phys_w) / static_cast<double>(desk_virt_w) - 100.0);
    }
    std::printf("\n");

    std::printf("    SDL 像素宽 = %d\n", pix_w);
    std::printf("    Win32 客户区宽 = %ld\n", client_w);
    if (hwnd != nullptr) {
        if (client_w == pix_w) {
            std::printf("    ⇒ 两者**相等** ⇒ SDL 报的\"像素\"就是 Win32 的客户区\n");
        } else {
            std::printf("    ⇒ 两者**不等** ⇒ 对\"像素\"的定义不同 ⇒ 需要细查\n");
        }
        if (gl_vp[2] == pix_w) {
            std::printf("    ⇒ 而 GL 帧缓冲宽 = %d，与它**也相等** ⇒ 三方一致\n", gl_vp[2]);
        }
    }
    std::printf("\n");

    // ★ 不依赖任何 API 的算术判据
    if (hwnd != nullptr && desk_phys_w > 0) {
        RECT wr{};
        if (GetWindowRect(hwnd, &wr)) {
            const long ww = wr.right - wr.left;             // 含边框
            const long cw = (client_w > 0) ? client_w : pix_w;
            std::printf("    ★ 算术判据（不依赖任何 API 的语义，只用\"屏幕上占多大\"）\n");
            std::printf("       窗口**客户区**宽 %ld / 桌面物理宽 %d = %.4f\n",
                        cw, desk_phys_w, static_cast<double>(cw) / static_cast<double>(desk_phys_w));
            std::printf("       窗口**含边框**宽 %ld / 桌面物理宽 %d = %.4f\n",
                        ww, desk_phys_w, static_cast<double>(ww) / static_cast<double>(desk_phys_w));
            std::printf("       ⇒ 若客户区比例 ≈ %.2f 且**不超过 1.0** ⇒ 客户区那个数就是**物理像素**\n",
                        static_cast<double>(cw) / static_cast<double>(desk_phys_w));
            std::printf("          （若它曾被系统放大 1.5 倍，比例会是 %.2f ⇒ 超过 1.0 ⇒ 不可能）\n",
                        1.5 * static_cast<double>(cw) / static_cast<double>(desk_phys_w));
        }
    }
#endif

    std::printf("\n按回车退出（或等 15 秒）。\n");
    {
        const Uint64 deadline = SDL_GetTicks() + 15000;
        bool quit = false;
        while (!quit && SDL_GetTicks() < deadline) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_KEY_DOWN ||
                    e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    quit = true;
                }
            }
            SDL_Delay(50);
        }
    }

    if (gl != nullptr) { SDL_GL_DestroyContext(gl); }
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

// =============================================================================
//  编译与运行（MSYS2 UCRT64）
//
//  cd /c/Users/vqwjq/Documents/LuaWAG/LuaWAG-Vanilla
//  g++ -std=c++20 -O1 -g -IC:/msys64/ucrt64/include sdl3_dpi_probe.cpp \
//      -o bin/sdl3_dpi_probe.exe -LC:/msys64/ucrt64/lib -lSDL3 -lopengl32 -luser32 -lgdi32
//
//  ./bin/sdl3_dpi_probe.exe > dpi.txt 2>&1; cat dpi.txt
// =============================================================================
