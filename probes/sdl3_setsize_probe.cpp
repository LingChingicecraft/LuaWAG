// =============================================================================
//  sdl3_setsize_probe.cpp —— **独立的最小对照实验**，不属于引擎源码。
//
//  【它为什么存在】
//      引擎要新增一个原语 `wag.space.set_window_size(w, h)`。而实现它必须先回答
//      两件事，两件都**不能靠读文档确定**：
//
//        ① **单位**：`SDL_SetWindowSize` 收的是物理像素还是 DIP？
//           已知：`SDL_CreateWindow(1280,720)` 得到的客户区是 1280 单位，
//                 而 `SDL_GetWindowSize` 也报 1280 ⇒ 三者同一单位。
//           但"同一单位"是哪种单位**没有定**。而引擎现在窗口尺寸契约写的是 DIP。
//           ⇒ 猜错就是 1.5 倍误差，正是我们刚花几轮修掉的那个形状。
//
//        ② **同步性**：调用之后**立刻**读，`SDL_GetWindowSize` 是否已是新值？
//           ⇒ 若立刻 ⇒ "帧末应用"就无缝；若不立刻 ⇒ 会画一帧错位的画面。
//
//  【它是怎么定 ① 的】
//      与同一目录下的 `sdl3_size_probe.cpp` 对照：那个探针创建 1280×720 的窗口，
//      报 `DIP=1280x720 像素(报)=1280x720 比例=1.500`。
//      本探针做同一件事，并把结果与"1280 × 1.5 = 1920"比较：
//          · 若客户区仍是 1280 单位 ⇒ 窗口尺寸的单位**不随缩放变**（即"逻辑/像素"同值）
//          · 若客户区是 1920 单位     ⇒ 它是 DIP，物理像素才是 1920
//
//  【它刻意不做的事】不包含引擎的任何头、不链接引擎的任何目标文件。
//
//  【它是一次性的】结论拿到之后可以删掉。
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <cstdio>

namespace {

void report(SDL_Window* win, const char* tag) {
    int dip_w = 0;
    int dip_h = 0;
    int px_w = 0;
    int px_h = 0;
    SDL_GetWindowSize(win, &dip_w, &dip_h);              // 引擎契约里叫"逻辑尺寸"
    SDL_GetWindowSizeInPixels(win, &px_w, &px_h);        // 引擎契约里叫"帧缓冲尺寸"
    const float scale = SDL_GetWindowDisplayScale(win);

    std::printf("%-22s GetWindowSize=%dx%d  SizeInPixels=%dx%d  比例=%.3f\n", tag, dip_w,
                dip_h, px_w, px_h, static_cast<double>(scale));
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init 失败: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* win = SDL_CreateWindow("SetWindowSize 探针", 800, 600,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (win == nullptr) {
        std::printf("SDL_CreateWindow 失败: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (ctx == nullptr) {
        std::printf("SDL_GL_CreateContext 失败: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(win, ctx);

    std::printf("==============================================================\n");
    std::printf(" SetWindowSize 探针\n");
    std::printf(" 创建时请求 800x600，比例（缩放）见下。\n");
    std::printf(" 判据：\n");
    std::printf("   · 若『立刻』读到的值 == 请求值 ⇒ 调用是同步的\n");
    std::printf("   · 把请求值乘/不乘比例，看哪个与客户区单位一致 ⇒ 定出单位\n");
    std::printf("==============================================================\n");

    report(win, "创建后");

    // ---- 实验一：设一个明显不同的尺寸，立刻读 ----
    const int req_w = 1024;
    const int req_h = 768;
    std::printf("\n--- 实验一：SDL_SetWindowSize(%d, %d) 然后**立刻**读 ---\n", req_w, req_h);
    SDL_SetWindowSize(win, req_w, req_h);
    report(win, "设完立刻读");

    // ---- 实验二：跑几帧之后再看（判断"是否只是延迟"）----
    for (int i = 0; i < 30; ++i) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
        }
        SDL_GL_SwapWindow(win);
        SDL_Delay(16);
    }
    std::printf("\n--- 实验二：跑了约 0.5 秒之后 ---\n");
    report(win, "半秒后");

    // ---- 实验三：请求一个远大于屏幕的尺寸，看平台给什么 ----
    std::printf("\n--- 实验三：请求一个极大的尺寸 8000x6000 ---\n");
    SDL_SetWindowSize(win, 8000, 6000);
    report(win, "极大尺寸立刻读");
    for (int i = 0; i < 30; ++i) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
        }
        SDL_GL_SwapWindow(win);
        SDL_Delay(16);
    }
    report(win, "极大尺寸半秒后");

    std::printf("\n==============================================================\n");
    std::printf(" 判读：\n");
    std::printf("  实验一『立刻读』== 请求值 ⇒ 同步；否则不同步\n");
    std::printf("  实验三：若客户区 != 8000x6000 ⇒ 平台会限制 ⇒ 即『申请 != 结果』\n");
    std::printf("==============================================================\n");
    std::fflush(stdout);

    // 让用户有机会看清窗口，然后退出。
    for (int i = 0; i < 120; ++i) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                i = 1000;
                break;
            }
        }
        SDL_GL_SwapWindow(win);
        SDL_Delay(16);
    }

    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
