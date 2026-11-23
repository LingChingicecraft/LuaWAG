-- =============================================================================
--  Lumi/Basic/object.lua
--
--  容器 b 方案的库层：**一个逻辑对象 = 一张 Lua 表 + 一个引擎句柄**。
--
--  【回写路径】批量：每类把可变字段攒进一张**扁平数组**，每池一次
--    `wag.container.write_batch(pool, n, flat)`。
--
--  【为什么不用 FFI】WagLib/harden.lua 已把 ffi/jit 封死（确定性：
--    "ffi 允许脚本直接读写内存 ⇒ 同一项目在不同机器上行为可能不同"）。
--    所以批量回写是"C 读 Lua 数组"，不是"脚本写 C 内存"。
--
--  【flat 布局】每对象 17 个数，紧挨着：
--    handle, x, y, vx, vy, rot, sx, sy, z, rgba, frame, blend, motion, visible,
--    layer, vcolor, tex_alpha
--
--  【行为不变】回调还是 6 个；④ 表还是那张；写回的还是那 13 个字段。
-- =============================================================================

local M = {}

M.classes = {}      -- class_id -> type 表
local pools = {}    -- class_id -> 池 id（一池一类型）
local lists = {}    -- class_id -> 稠密数组（对象表）
local objs = {}     -- handle -> b（按句柄查找，kill 用）
local flats = {}    -- class_id -> 扁平数组（复用，不每帧新建）

-- flat 每行的字段数（与 primitives.cpp 的 write_batch 一致）
--   handle, x, y, vx, vy, rot, sx, sy, z, rgba, frame, blend, motion, visible,
--   layer, vcolor, tex_alpha
local STRIDE = 17

-- R24.1：分派器（引擎 → 库层的唯一入口）。
-- 引擎只持有一个 registry 引用；del 在帧末回收时由引擎调用。
local function dispatch(kind, handle)
    if kind == "del" then
        local b = objs[handle]
        if b == nil then
            return
        end
        local t = M.classes[b.class]
        if t and t.del then
            t.del(b)
        end
        objs[handle] = nil
    end
end
wag.container.set_dispatcher(dispatch)

local function pool_for(class)
    local p = pools[class]
    if p == nil then
        p = wag.container.new_pool("class:" .. tostring(class))
        pools[class] = p
        lists[class] = {}
        flats[class] = {}
    end
    return p
end

function M.register(class, type)
    if M.classes[class] ~= nil then
        wag.fail("class 重复注册：" .. tostring(class))
    end
    type = type or {}
    M.classes[class] = type
    -- R24/R25：引擎只登记"有没有回调"的存在位；函数留在本表里。
    wag.container.register_class(class, type)
end

function M.new(class, params)
    local t = M.classes[class]
    if t == nil then
        wag.fail("未注册的 class：" .. tostring(class))
    end

    local b = params or {}
    b.class = class

    if t.init then
        t.init(b)
    end

    local p = {
        x = b.x or 0, y = b.y or 0,
        vx = b.vx or 0, vy = b.vy or 0,
        rot = b.rot or 0, sx = b.sx or 1, sy = b.sy or 1,
        rgba = b.rgba or 0xFFFFFFFF,
        layer = b.layer or 0, z = b.z or 0,
        frame = b.frame or 0, blend = b.blend or 0, motion = b.motion or 0,
        vcolor = b.vcolor or 0, tex_alpha = b.tex_alpha or 0,
        group = b.group or 0,
        visible = (b.visible ~= false),
        class = class,
        img = b.img_id or 0,
        collides = (b.hit ~= nil),
    }
    if b.hit ~= nil then
        p.hit = b.hit
    end

    local h = wag.container.spawn(pool_for(class), p)
    b.handle = h
    objs[h] = b

    local list = lists[class]
    b.__idx = #list + 1
    list[#list + 1] = b
    return b
end

-- R3：换图（引擎先建后毁；旧容器由引擎 kill，旧句柄立即失效）
-- params 里可出现任意可写字段 + img；class/group/collides 不可变。
function M.replace(b, params)
    local p = params or {}
    local h = b.handle
    local t = M.classes[b.class]
    local new_h = wag.container.replace(h, p)

    -- ④ 浅拷贝旧表，再覆盖参数
    local nb = {}
    for k, v in pairs(b) do
        nb[k] = v
    end
    for k, v in pairs(p) do
        nb[k] = v
    end
    nb.handle = new_h

    -- 列表里原地替换（遍历顺序不变）
    local list = lists[b.class]
    local i = b.__idx
    if i ~= nil and list[i] == b then
        list[i] = nb
        nb.__idx = i
    end
    b.__idx = nil

    -- 旧表：声明了 del 的留到帧末 del 派发；否则当场释放
    if t and t.del then
        b.__replaced = true
    else
        objs[h] = nil
    end
    objs[new_h] = nb
    return nb
end

-- R6/R18：改判定的**唯一**路径（写时同步到引擎侧表）
function M.set_hit(b, hit)
    wag.container.set_hit(b.handle, hit)
    b.hit = hit
end

-- 批量路径：一个类一次 write_batch
local function step_class(class, list, dt)
    local t = M.classes[class]
    local frame = t.frame
    local flat = flats[class]
    local n = #list
    local k = 0
    local dead = nil

    for i = 1, n do
        local b = list[i]
        if frame then
            frame(b, dt)
        end
        if b.dead then
            dead = dead or {}
            dead[#dead + 1] = b.handle
        else
            local o = k * STRIDE
            flat[o + 1] = b.handle
            flat[o + 2] = b.x or 0
            flat[o + 3] = b.y or 0
            flat[o + 4] = b.vx or 0
            flat[o + 5] = b.vy or 0
            flat[o + 6] = b.rot or 0
            flat[o + 7] = b.sx or 1
            flat[o + 8] = b.sy or 1
            flat[o + 9] = b.z or 0
            flat[o + 10] = b.rgba or 0xFFFFFFFF
            flat[o + 11] = b.frame or 0
            flat[o + 12] = b.blend or 0
            flat[o + 13] = b.motion or 0
            flat[o + 14] = (b.visible ~= false) and 1 or 0
            flat[o + 15] = b.layer or 0
            flat[o + 16] = b.vcolor or 0
            flat[o + 17] = b.tex_alpha or 0
            k = k + 1
        end
    end

    if k > 0 then
        wag.container.write_batch(pools[class], k, flat)
    end
    if dead then
        for i = 1, #dead do
            M.kill(dead[i])
        end
    end
end

function M.step(dt)
    for class, list in pairs(lists) do
        step_class(class, list, dt)
    end
end

function M.draw()
    -- 默认渲染 = 引擎从 64 B 结构体一次发射（b 方案的默认路径，零 Lua 回调）。
    -- 【自定义 render 暂不由这里调用】它需要"按类筛出对象"的机制，
    --   本轮只接默认发射；自定义形状等那条路落地再补。
    wag.draw.emit_containers()
end

function M.kill(h)
    local b = objs[h]
    if b == nil or b.__dying then
        return
    end
    b.__dying = true
    local t = M.classes[b.class]
    if t and t.kill then
        t.kill(b)
    end
    -- R24.3：类声明了 del 的，把表留到帧末 del 派发；否则当场释放。
    if not (t and t.del) then
        objs[h] = nil
    end

    local list = lists[b.class]
    local i = b.__idx
    if i ~= nil and i <= #list then
        local last = list[#list]
        list[i] = last
        if last ~= nil then
            last.__idx = i
        end
        list[#list] = nil
    end
    b.__idx = nil

    wag.container.kill(h)
end

function M.count()
    local n = 0
    for _ in pairs(objs) do
        n = n + 1
    end
    return n
end

function M.mode()
    return "批量(write_batch)"
end

return M
