# 更新说明

本文件记录 LuaWAG 各版本的变化、已知问题和计划修复项。  
`0.x` 版本仍处于预发布阶段，API 和行为可能变化；请以对应版本的说明为准。

---

## 0.2.0 — 2026-11-25（待发布）

### 新增

- **全屏模式**

  ```lua
  wag.space.set_fullscreen("none" | "borderless" | "exclusive")
  wag.space.get_fullscreen()
  ```

  - `borderless`：覆盖当前显示器完整边界的无边框全屏
  - `exclusive`：使用当前显示器原生模式的独占全屏
  - 进入全屏前保存窗口位置、客户区尺寸、边框和最大化状态；退出时恢复
  - 切换期间按住 `pause_source::fullscreen`

- **内部渲染目标**

  ```lua
  wag.space.set_render_size(w, h)
  wag.space.get_render_size()
  wag.space.get_frame_size()    -- 内部渲染目标尺寸
  wag.space.get_output_size()   -- 输出 / 后缓冲尺寸
  ```

  默认内部渲染目标为 `1920×1080`，不再自动跟随窗口或全屏输出。
  所有场景先画到内部目标，再通过 present 输出。

- **呈现控制**

  ```lua
  wag.space.set_present_mode("letterbox" | "stretch")
  wag.space.set_present_filter("nearest" | "linear")
  wag.space.get_present_mode()
  wag.space.get_present_filter()
  ```

  默认 `letterbox` + `linear`。

- **显示器信息**

  ```lua
  wag.space.get_display_bounds()
  wag.space.get_display_work_area()
  ```

- **输入映射**

  鼠标坐标按：

  ```text
  输出 → 内部渲染目标 → 逻辑坐标
  ```

  反算；全屏和 `render_size` 改变后输入位置仍对应画面。

- **快照**

  `wag.draw.capture_snapshot()` 改为在 present 之后捕获最终输出，而不是内部画布。

### 兼容性变化

- `get_frame_size()` 的语义从“输出 / 窗口像素尺寸”改为“内部渲染目标尺寸”。
  需要输出尺寸时使用新增的 `get_output_size()`。
- `get_viewport()` 返回的 viewport 矩形改为内部渲染目标坐标，不再是输出窗口坐标。
- 游戏应继续用逻辑坐标（`render`）做布局；不要用 `get_frame_size()` 当窗口尺寸。
- 自定义光照 shader 如需 `self_only` band mask，必须使用新增的 `u_band_space` 采样；
  `u_light_space` 仍是光照/遮蔽图覆盖空间。

### 修复

- 修复全屏时视口仍按输出尺寸计算导致的整体漂移。
- 修复 present 关闭 `GL_BLEND` 后未恢复，导致字体和半透明内容变成白块。
- 修复快照改为最终输出后，`load_from_snapshot` 仍按内部目标尺寸取图的问题。
- 修复全屏中最小化再恢复后退出全屏尺寸错误。
- 修复重复进入全屏时仍使用第一次保存的窗口状态。
- 修复 band mask 在自定义光照空间下错误使用 light space 采样。

### 多显示器

**多显示器未在真实硬件上实测。**

按当前实现：

- 窗口不在显示器边界上时，预期行为与单显示器一致。
- 全屏请求失败会返回 `false`，引擎记录 L2 并回退窗口模式，不会因此主动终止进程。
- 跨显示器选择、显示器热插拔、显示器移除后恢复到可见位置、混合 DPI 均未验证。
- 显示器被移除或重新排列后，保存的位置可能落在不可见区域；当前不做窗口位置夹取，也不承诺崩溃后的显示状态恢复。

因此多显示器目前是“未实测”，不是“已验证支持”。

### 已知限制

- `exclusive` 只使用当前显示器原生模式，不做显示模式枚举或选择。
- 不提供 vsync API。
- 不做整数倍缩放。
- 不承诺崩溃后持久化显示状态恢复。
- 示例项目里原先使用的上下箭头字形在字体中缺失；示例文案已改用中文“上箭头/下箭头”，字体本身未补字。

### 发行包

仍只包含：

```text
Luminosa/Project   启动画面
Luminosa/SeedGame  弹幕 ACT 示例
Luminosa/Lumi
Luminosa/WagLib
```

---

## 0.1.2 — 2026-11-25

### 文档与内部收口

- **窗口尺寸契约去 DIP 化**  
  引擎内部的窗口尺寸标识统一为 `get_window_size` / `dimensions` 等；  
  公开文档明确：窗口尺寸不是 DIP，`display_scale` 只作诊断，不参与几何换算。  
  本次只改命名与契约，不改变窗口数值行为。

- **`__gc` 与确定性**  
  公开文档新增：`__gc` 不在三条逻辑帧线（`ExtFrameFunc` / `MainFrameFunc` / `PauseFrameFunc`）的协议内；  
  用 `__gc` 改游戏状态会主动放弃确定性，帧线架构救不回来。

### 已知限制

- **`__gc` 抛出的错误不会产生 L1**  
  LuaJIT 不会把 finalizer 错误抛回 `lua_cpcall`，而是打印或吞掉；  
  0.1.1 已确认该行为，0.1.2 及以后不再承诺 `__gc` 错误的 L1 回执。  
  业务收尾应放在 `on_game_exit`；不要把 `__gc` 当游戏生命周期。

- **没有真正的 DPI-aware UI**  
  当前窗口尺寸与绘制表面像素相等，是因为未启用高像素密度；  
  150% 缩放下不会自动乘 1.5。窗口尺寸不是 DIP。

- **自定义光照空间下的 band mask 限制**  
  仍与 0.1.1 相同；band mask 按 render 空间生成，未映射到 light space。

### 发行包

仍只包含：

```text
Luminosa/Project   启动画面
Luminosa/SeedGame  弹幕 ACT 示例
Luminosa/Lumi
Luminosa/WagLib
```


---

## 0.1.1 — 2026-11-24

### 修复

- **进程退出时的 Lua 生命周期顺序**  
  关闭序列现在在 `on_game_exit` 之后、字体 / 音频 / SDL / 日志关闭之前调用 `script().destroy()`。  
  这样 `lua_close` 触发的 `__gc` 仍能访问仍然存在的引擎资源；`~engine` 保留幂等兜底。

- **光照链路固定 1920×1080 设计空间**  
  新增独立的光照空间：

  ```lua
  wag.light.set_space { l = -960, b = -540, w = 1920, h = 1080 }
  wag.light.get_space()
  ```

  未显式设置时，首次使用取启动后的 render；显式设置后以作者设置为准。  
  遮挡图顶点映射已改用当前 light space，不再固定按 `p / 960`、`p / 540`。

- **Release 构建的未使用变量警告**  
  `renderer.cpp` 中 `slack` 只由 `assert` 使用；`NDEBUG` 下变量与断言一起消失，Release 构建不再产生未使用变量警告。

- **公开的临时探针入口**  
  已移除公开的 `wag.draw.set_probe_shaders`。  
  `WAG_RT_SELFTEST`、`WAG_INSTANCE_SELFTEST` 等内部自检环境变量保留，用于引擎内部验收。

### 变更与兼容性

- 自定义 `light.vs` / `light.fs` 需要接收新的 `u_light_space`（`l, b, w, h`）。  
  旧 shader 仍能加载，但会继续使用自己的固定空间常量；要支持自定义 light space，必须同步更新。
- 光图 / 遮蔽图 / 带掩码的分辨率仍固定为 `960×540`；改变的是它们覆盖的世界矩形。

### 已知限制

- **自定义光照空间下的 `self_only` 带掩码**  
  `wag.light.set_space` 已经支持独立光照空间；但 band mask 仍按 render 空间生成。  
  如果项目同时使用 `self_only` 带，必须保持 light space 与 render 相同；否则带掩码会与光照采样空间不一致。  
  解除这条限制需要把 band mask 也映射到 light space，计划留给后续版本。

- **`__gc` 抛出的错误没有 L1 回执**  
  `__gc` 正常执行时仍能写日志；但如果 `__gc` 自身抛错，LuaJIT 会吞掉错误，当前不会产生 L1。  
  0.1.2 已复核：公开 LuaJIT API 无法捕获该错误；不再把“受保护 finalizer 批处理”列为后续修复。  
  业务收尾仍应放在 `on_game_exit`，不要依赖 `__gc` 的错误回执。

### 计划在 0.2.0 处理

- `end_frame` 的全屏回画 / 运行效果；
- 渲染目标绑定的完整实现；
- 引用容器数诊断；
- 其它尚未落地的渲染维度；
- 更完整的跨平台支持；
- 自定义光照空间下的 band mask 映射。

---

## 0.1.0 — 2026-11-23

### 概述

- 基于 C++20、SDL3、OpenGL 3.3 与 LuaJIT 的轻量游戏引擎；
- 固定运行时层：`WagLib`、`Lumi`、`Nosa`、`Project`；
- 游戏逻辑由 Lua 编写；C++ 侧保留窗口、输入、音频、渲染、脚本边界与帧结构；
- `Projects/` 提供 `Begin` 启动画面与 `SeedGame` 示例卡带；
- Windows 优先，当前实际验证平台为 Windows 11 x86_64。

### 已知问题

- **非默认 render 尺寸下的光照**  
  光照的灯、遮挡图和带掩码按固定 `1920×1080` 设计空间工作；默认尺寸正常，自定义 render 尺寸暂不保证。

- **退出阶段的 Lua finalizer**  
  当前仓库的示例脚本没有使用 `__gc`；带 userdata finalizer 的工程在退出阶段可能访问已经关闭的引擎资源。  
  规避方式：把需要可靠执行的收尾逻辑放在 `on_game_exit`，不要在 `__gc` 中依赖引擎服务。

- **部分渲染入口尚未实现**  
  渲染目标绑定、`end_frame` 全屏效果、引用容器数诊断等入口仍处于未完成状态。  
  不要把“调用没有报错”当作“功能已经生效”；依赖这些功能前请先确认当前实现。

- **平台限制**  
  当前只验证 Windows；非 Windows 的路径、窗口与平台层行为不承诺可用。

- **预发布 API**  
  `0.1.x` 的 Lua API、模块路径和目录约定仍可能调整；升级前请先看对应版本的更新说明。

### 建议

- 需要保存进度时，在 `on_game_exit` 中完成；
- 需要自定义世界范围时，暂时避免依赖光照链路；
- 使用 `Projects/` 中的示例卡带时，按 `Projects/README.md` 的说明切换；
- 遇到问题时附上 `engine.full.log`、是否有 `__gc`、render 尺寸和是否使用光照。
