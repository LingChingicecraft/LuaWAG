-- =============================================================================
--  Luminosa/Lumi/Basic/drag_pause.lua
--
--  默认策略：**用户拖拽 / 缩放窗口期间自动暂停**（2026-11-15）。
--
--  【为什么它存在】
--      拖拽期间 Windows 在模态循环里：键盘消息被过滤、鼠标被模态循环捕获
--      ⇒ 玩家**本来就没有输入通道**，让游戏继续跑只是在跑给没人看。
--      ⇒ 暂停更符合直觉，也让"拖拽期间最坏 1–2 帧的输入延迟"彻底失去意义。
--
--  【为什么是 Lumi（官方库）而不是引擎】
--      引擎**不**替你做这个决定（`pause_source::drag` 保留但不点亮）。
--      若让引擎持位：松手时释放就是"自动恢复"（撞《窗口交互与追帧文档》§6），
--      不释放则脚本松不掉（撞 P3a）⇒ 只能由脚本用自己的 `user` 位实现。
--
--  【用的是哪个事实】
--      `frame_info.dragging` —— 引擎在**阶段 1 定格**的本帧常量，
--      **拖动标题栏与拖拽边框都算**（Windows 对两者发同一条消息）。
--
--  【恢复语义：**不自动**】
--      本模块只负责"进入暂停"。恢复必须由玩家显式操作（游戏的恢复键/菜单）。
--      ⇒ 这是 §6 的要求，也是本策略必须配一个暂停界面的原因。
--
--  【安全阀：没有暂停线的工程不装】
--      若一个工程没有 `on_frame_pause`（仍是引擎步骤 0 预置的空函数），
--      那么装上去的后果是"拖一下窗口，游戏**再也回不来**"。
--      ⇒ 本模块在装载时（早于任何用户脚本）记下引擎预置的空函数，
--         `install()` 时比较一次：还是那个空函数就**跳过安装**并记一条 L1。
--
--  【显式约定：暂停线必须在"层装载期"设好】
--      停止线判定靠的是"`install()` 那一刻，`on_frame_pause` 变了没有"。
--      所以工程**必须在层装载期**（`Project` / `Nosa` / `Lumi` 中任一模块的**顶层**）
--      完成 `PauseFrameFunc = ...` 的赋值，也就是**早于** `install()`。
--
--      ⚠️ **不允许**在帧循环开始之后再补设 `on_frame_pause`。
--         那样做的后果是二选一：
--           ① 本策略在 install 时判定"没有暂停线" ⇒ **静默不装**（你以为装上了）；
--           ② 或者你手动调了 install() ⇒ 装上但没有恢复路径 ⇒ **拖一下锁死**。
--         两种都是"看起来能跑、实际上坏掉"，因此写成约定而不是留作巧合。
--
--      若确实需要后补设：自己调 `require("Lumi.Basic.drag_pause").install()` 控制时机，
--      并自己保证那时已经有一条可用的恢复路径。
--
--  【怎么关掉】
--      在工程的 `on_game_init` 里：
--          require("Lumi.Basic.drag_pause").enabled = false
--      （`require` 有缓存，所以先设 false、后 install 也生效。）
-- =============================================================================

local M = {}

-- 【必须在"装载任何用户脚本之前"抓下来，因此本模块要在 init.lua 顶部 require】
--   引擎在步骤 0 给 13 个槽预置了空函数；这里记下"暂停帧"那一个，
--   用来判断工程到底有没有自己写暂停线。
local ENGINE_PRESET_PAUSE = wag.hook.on_frame_pause

M.enabled = true
M.installed = false

local inner_main = nil
local paused_by_drag = false

-- 包装后的 on_frame_main：先看拖拽，再把本帧交给原来的钩体。
local function wrapped_on_main(info)
    if M.enabled and type(info) == "table" and info.dragging == true then
        if not paused_by_drag then
            paused_by_drag = true
            wag.engine.pause()
            wag.log.info("[Lumi] 拖拽窗口 ⇒ 暂停",
                         "松开后需显式恢复（本策略不自动恢复）")
        end
    else
        paused_by_drag = false
    end

    if inner_main ~= nil then
        return inner_main(info)
    end
end

-- -----------------------------------------------------------------------------
--  安装 / 卸载
-- -----------------------------------------------------------------------------

function M.install()
    if M.installed then
        return
    end
    M.installed = true -- 无论装没装成，都只评估一次

    -- 安全阀：没有暂停线 ⇒ 装了会锁死
    if wag.hook.on_frame_pause == ENGINE_PRESET_PAUSE then
        wag.log.info("[Lumi] 未安装拖拽暂停",
                     "本工程没有暂停线（on_frame_pause 仍是引擎预置）；装上去拖拽后将无法恢复")
        return
    end

    inner_main = wag.hook.on_frame_main
    wag.hook.on_frame_main = wrapped_on_main
    wag.log.info("[Lumi] 拖拽即暂停策略已安装",
                 "不要它：require('Lumi.Basic.drag_pause').enabled = false")
end

function M.uninstall()
    if not M.installed or inner_main == nil then
        return
    end
    wag.hook.on_frame_main = inner_main
    inner_main = nil
    M.installed = false
end

return M
