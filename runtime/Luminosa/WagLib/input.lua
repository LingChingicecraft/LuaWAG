-- =============================================================================
--  Luminosa/WagLib/input.lua
--
--  库层输入封装：**动作名 → 按键/鼠标按钮**，游戏代码只认动作，不认 scancode。
--
--  【它解决什么】
--    引擎（wag.input）只提供原始状态：down/press/release(scancode)、
--    鼠标位置/滚轮、gamepad 原始按钮/轴。游戏代码若直接写 wag.input.down(29)，
--    改键就要改代码。本模块把"动作"与"物理键"解耦：
--        input.bind("shot", { keys = { "Z" }, mouse = { 1 },
--                             gamepad = { {device=0, button=0} } })
--        if input.press("shot") then ... end
--    改键只改 bind 的参数（或调用方读配置后 bind），游戏逻辑不动。
--
--  【它不做什么】
--    · 不碰回放（源切换留了 set_source，但回放源由后续实现）；
--    · 不做连发/组合键（它们属于更高层）；
--    · 不读焦点（《开发公理》E.2）；
--    · 不解释 scancode 的语义（名字表在 WagLib/keycode.lua）。
--
--  【源（source）契约】
--    一个 source 是一张表，必须提供五个函数：
--        down(action)   -> boolean
--        press(action)  -> boolean
--        release(action)-> boolean
--        mouse_pos()    -> x, y   （逻辑坐标）
--        wheel()        -> dx, dy （本帧增量）
--    默认源是 "device"（用 wag.input + 本模块的绑定表）。
--    将来回放源实现同一张表即可替换，游戏代码无感。
--
--  【为什么 mouse_pos/wheel 也走 source】
--    回放时鼠标位置/滚轮同样是"记录的数据"，不能只换按键源 ——
--    否则回放画面里的鼠标会跟着真实设备跑。
-- =============================================================================

local keycode = require("WagLib.keycode")

local KEY = keycode.KEY

-- 引擎侧 Gamepad 原始索引（与 SDL 3.4 对齐；见 platform/input_event.hpp）。
local GAMEPAD_DEVICE_COUNT = 8
local GAMEPAD_BUTTON_COUNT = 26
local GAMEPAD_AXIS_COUNT = 6

local M = {}

-- action -> { keys = { scancode... }, mouse = { button... },
--             gamepad = { {device=, button=}... } }
local bindings = {}

-- 当前源；默认是 device（在下面定义后赋值）。
local source = nil

-- -----------------------------------------------------------------------------
--  绑定
-- -----------------------------------------------------------------------------

local function fail(fmt, ...)
    error(string.format(fmt, ...), 3)
end

local function resolve_key(k)
    if type(k) == "number" then
        -- 允许直接用 scancode（高级用法；越界交给引擎 L3）。
        return k
    end
    if type(k) == "string" then
        local sc = KEY[k]
        if sc == nil then
            fail("input.bind：未知按键名 %q（见 WagLib/keycode.lua）", k)
        end
        return sc
    end
    fail("input.bind：keys 的项必须是按键名或 scancode 数字")
end

--- 绑定一个动作。
--
-- @param action 动作名（非空字符串）
-- @param spec   {
--                 keys = {名字或数字...},
--                 mouse = {1..5...},
--                 gamepad = { {device=0..7, button=0..25}, ... }
--               }
--
-- 【语义】
--   · 同一个动作可以绑多个键/按钮，查询时按 **OR** 合并；
--   · 重复 bind 同一个动作 ⇒ **替换**（不是叠加）；
--   · 至少要有一个 keys / mouse / gamepad 项 —— 空绑定是拼写错误；
--   · 键名在 bind 时就解析成 scancode（拼错当场报，而不是查询时静默 false）。
function M.bind(action, spec)
    if type(action) ~= "string" or action == "" then
        fail("input.bind：action 必须是非空字符串")
    end
    if type(spec) ~= "table" then
        fail("input.bind：spec 必须是表（{ keys = {...}, mouse = {...}, gamepad = {...} }）")
    end

    local keys = {}
    local mouse = {}
    local gamepad = {}

    if spec.keys ~= nil then
        if type(spec.keys) ~= "table" then
            fail("input.bind：spec.keys 必须是数组")
        end
        for i = 1, #spec.keys do
            keys[#keys + 1] = resolve_key(spec.keys[i])
        end
    end

    if spec.mouse ~= nil then
        if type(spec.mouse) ~= "table" then
            fail("input.bind：spec.mouse 必须是数组")
        end
        for i = 1, #spec.mouse do
            local b = spec.mouse[i]
            if type(b) ~= "number" or b < 1 or b > 5 then
                fail("input.bind：mouse 的项必须是 1..5（LEFT=1 … X2=5）")
            end
            mouse[#mouse + 1] = b
        end
    end

    if spec.gamepad ~= nil then
        if type(spec.gamepad) ~= "table" then
            fail("input.bind：spec.gamepad 必须是数组")
        end
        for i = 1, #spec.gamepad do
            local g = spec.gamepad[i]
            if type(g) ~= "table" then
                fail("input.bind：gamepad 的项必须是 {device=, button=}")
            end
            local device, button = g.device, g.button
            if type(device) ~= "number" or device < 0 or device >= GAMEPAD_DEVICE_COUNT then
                fail("input.bind：gamepad.device 必须是 0..7")
            end
            if type(button) ~= "number" or button < 0 or button >= GAMEPAD_BUTTON_COUNT then
                fail("input.bind：gamepad.button 必须是 0..25")
            end
            gamepad[#gamepad + 1] = { device = device, button = button }
        end
    end

    if #keys == 0 and #mouse == 0 and #gamepad == 0 then
        fail("input.bind：动作 %q 没有任何绑定（keys/mouse/gamepad 都为空）", action)
    end

    bindings[action] = { keys = keys, mouse = mouse, gamepad = gamepad }
end

--- 解除一个动作的绑定（之后查询该动作会报错，而不是静默 false）。
function M.unbind(action)
    bindings[action] = nil
end

--- 清空全部绑定（用于重载配置/重新绑定）。
function M.clear_bindings()
    bindings = {}
end

-- -----------------------------------------------------------------------------
--  默认源：设备
-- -----------------------------------------------------------------------------

local function binding_of(action)
    local b = bindings[action]
    if b == nil then
        fail("input：动作 %q 未绑定（先调用 input.bind）", action)
    end
    return b
end

local device = {}

function device.down(action)
    local b = binding_of(action)
    for i = 1, #b.keys do
        if wag.input.down(b.keys[i]) then
            return true
        end
    end
    for i = 1, #b.mouse do
        if wag.input.mouse_down(b.mouse[i]) then
            return true
        end
    end
    for i = 1, #b.gamepad do
        local g = b.gamepad[i]
        if wag.input.gamepad_down(g.device, g.button) then
            return true
        end
    end
    return false
end

function device.press(action)
    local b = binding_of(action)
    for i = 1, #b.keys do
        if wag.input.press(b.keys[i]) then
            return true
        end
    end
    for i = 1, #b.mouse do
        if wag.input.mouse_press(b.mouse[i]) then
            return true
        end
    end
    for i = 1, #b.gamepad do
        local g = b.gamepad[i]
        if wag.input.gamepad_press(g.device, g.button) then
            return true
        end
    end
    return false
end

function device.release(action)
    local b = binding_of(action)
    for i = 1, #b.keys do
        if wag.input.release(b.keys[i]) then
            return true
        end
    end
    for i = 1, #b.mouse do
        if wag.input.mouse_release(b.mouse[i]) then
            return true
        end
    end
    for i = 1, #b.gamepad do
        local g = b.gamepad[i]
        if wag.input.gamepad_release(g.device, g.button) then
            return true
        end
    end
    return false
end

function device.mouse_pos()
    return wag.input.mouse_pos()
end

function device.wheel()
    return wag.input.wheel()
end

-- gamepad 原始查询（动作映射之外的底层入口）
function device.gamepad_devices()
    return wag.input.gamepad_devices()
end

function device.gamepad_connected(device_id)
    return wag.input.gamepad_connected(device_id)
end

function device.gamepad_down(device_id, button)
    return wag.input.gamepad_down(device_id, button)
end

function device.gamepad_press(device_id, button)
    return wag.input.gamepad_press(device_id, button)
end

function device.gamepad_release(device_id, button)
    return wag.input.gamepad_release(device_id, button)
end

function device.gamepad_axis(device_id, axis)
    return wag.input.gamepad_axis(device_id, axis)
end

source = device

-- -----------------------------------------------------------------------------
--  源切换（为回放留的接缝）
-- -----------------------------------------------------------------------------

local function check_source(s)
    if type(s) ~= "table" then
        fail("input.set_source：source 必须是表（或 nil 恢复设备源）")
    end
    local required = {
        "down", "press", "release", "mouse_pos", "wheel",
        "gamepad_devices", "gamepad_connected",
        "gamepad_down", "gamepad_press", "gamepad_release",
        "gamepad_axis",
    }
    for i = 1, #required do
        local name = required[i]
        if type(s[name]) ~= "function" then
            fail("input.set_source：source.%s 必须是函数", name)
        end
    end
end

--- 设置输入源。
--
-- @param s 符合“源契约”的表；**nil = 恢复设备源**
--
-- 【契约】11 个函数：down/press/release、mouse_pos/wheel、
--   gamepad_devices/connected/down/press/release/axis。
--   回放源必须把 gamepad 原始状态也一起回放。
function M.set_source(s)
    if s == nil then
        source = device
        return
    end
    check_source(s)
    source = s
end

--- 恢复设备源（等价于 set_source(nil)）。
function M.use_device()
    source = device
end

-- -----------------------------------------------------------------------------
--  查询（游戏代码只调这五个）
-- -----------------------------------------------------------------------------

--- 动作是否按住（本帧快照）。
function M.down(action)
    return source.down(action)
end

--- 动作本帧是否按下过（P 占帧；见《输入子系统设计》I1/I2）。
function M.press(action)
    return source.press(action)
end

--- 动作本帧是否松开（P 占帧时恒 false）。
function M.release(action)
    return source.release(action)
end

--- 鼠标位置：**逻辑坐标**。
function M.mouse_pos()
    return source.mouse_pos()
end

--- 滚轮本帧增量（+x 右 / +y 上）。
function M.wheel()
    return source.wheel()
end

-- ---------------------------------------------------------------------------
--  gamepad 原始查询（供动作映射/回放源使用；游戏逻辑优先用 action）
-- ---------------------------------------------------------------------------

function M.gamepad_devices()
    return source.gamepad_devices()
end

function M.gamepad_connected(device_id)
    return source.gamepad_connected(device_id)
end

function M.gamepad_down(device_id, button)
    return source.gamepad_down(device_id, button)
end

function M.gamepad_press(device_id, button)
    return source.gamepad_press(device_id, button)
end

function M.gamepad_release(device_id, button)
    return source.gamepad_release(device_id, button)
end

function M.gamepad_axis(device_id, axis)
    return source.gamepad_axis(device_id, axis)
end

return M
