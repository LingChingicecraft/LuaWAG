# =============================================================================
#  src/draw/CMakeLists.txt
#
#  模块：draw（渲染器）
#
#  【本模块的职责边界（一句话）】
#      **接受变换，消费它，把顶点写成物理像素。**
#      它**不知道窗口**，也**不知道任何矩形**。
#
#  【它为什么依赖 space 而 platform 只作为类型来源】
#      · space：它需要一个"变换"作为参数（那正是 space 的产出）
#      · platform：它只用一个类型 —— gl_context（上下文句柄）
#        ⇒ 它**不调用**窗口的任何函数
#      ⇒ 所以下面把 platform 列为 PUBLIC 依赖，是因为 gl_context 出现在
#        renderer.hpp 的函数签名里（renderer::adopt_context）
#
#  【GL 函数入口怎么取回 —— 2026-10-15 已决】
#      自写极小加载器（gl_api.hpp/.cpp：一张名字表 + SDL_GL_GetProcAddress
#      + 失败即 L3），不引第三方 loader。理由见 gl_api.hpp 文件头。
#      ⇒ 本模块因此多了那两个源文件；解析器由 SDL3 提供，不需要额外链接。
# =============================================================================

add_library(wag_draw STATIC
    gl_api.cpp
    gl_api.hpp
    renderer.cpp
    renderer_restore.cpp
    light/targets.cpp
    light/occluders.cpp
    light/bands.cpp
    light/pass.cpp
    renderer.hpp)

target_include_directories(wag_draw PUBLIC ${CMAKE_SOURCE_DIR}/src)

# -----------------------------------------------------------------------------
#  依赖
# -----------------------------------------------------------------------------
#
#   PUBLIC  wag_space    —— 变换出现在 renderer.hpp 的公开签名里
#   PUBLIC  wag_platform —— gl_context 出现在 renderer.hpp 的公开签名里
#   PRIVATE wag_error wag_log —— 只有 .cpp 用到报错与日志（原 wag_core，已拆分）
#
# 【为什么 space / platform 是 PUBLIC 而不是 PRIVATE】
#   因为 renderer.hpp 的**公开接口**里有它们的类型：
#       bool adopt_context(platform::gl_context ctx);
#       void begin_frame(const space::transform&, double);
#   ⇒ 任何 include renderer.hpp 的翻译单元都需要那两个头。
#     若写成 PRIVATE，下游就会"看得见函数但看不见参数类型"，那是编译错误。
#
# 【为什么 wag_error / wag_log 是 PRIVATE】
#   因为日志与报错只在 .cpp 里用，不出现在任何公开签名里。
#   ⇒ 依赖能写成 PRIVATE 的，就不要写成 PUBLIC：那会让"谁真的需要谁"变模糊。
target_link_libraries(wag_draw
    PUBLIC  wag_space wag_platform
    PRIVATE wag_error wag_log wag_build_config)

# 【这一行是必须的，而且它的缺失曾经造成一次很难查的静默失效】
#   renderer.cpp 的 GL 调用（清屏、视口、scissor）整段包在
#       #if defined(WAG_HAVE_SDL3)
#   里，而 WAG_HAVE_SDL3 由 wag_sdl3 这个 INTERFACE 目标携带。
#   ⇒ 若本模块不链它，那段实现会被**预处理器直接删掉**：
#         · 没有编译警告（#else 分支把参数 (void) 掉了）
#         · 运行时表现为**清屏静默失效**（窗口全黑，其他一切正常）
#   ⇒ 所以"某个源文件是否用 SDL3"取决于"它所在的目标有没有链 wag_sdl3"，
#     而这条约定必须写在这里，不能靠记忆。
if(TARGET wag_sdl3)
    target_link_libraries(wag_draw PRIVATE wag_sdl3)
endif()

# 【GL 1.1 的函数要显式链 opengl32】
#   Windows 的 opengl32.dll 导出 OpenGL **1.1** 的函数（glClear / glClearColor /
#   glViewport / glScissor / glEnable / glDisable …）。"头里有声明"与
#   "链接器找得到"是两件事 —— 不写这一项就会得到一串
#       undefined reference to `__imp_glClear'
#
# 【而链了 SDL3 不等于链了 GL】SDL3 只提供取函数地址的接口与上下文创建,
#   它**不导出** GL 函数。
if(WIN32)
    target_link_libraries(wag_draw PRIVATE opengl32)
endif()

set_target_properties(wag_draw PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
