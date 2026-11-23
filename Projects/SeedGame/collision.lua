-- =============================================================================
--  Project/seedgame/collision.lua
--
--  子弹/BOSS 的碰撞代理：
--    · 真正的形状与位置在引擎侧 64B 容器结构体里
--    · 每帧 object.step 批量同步位置
--    · 碰撞请求由 wag.collide.check(子弹组, BOSS组, cb) 在 C++ 侧执行
--  绘制仍然走 seedgame 的 shader，不依赖容器可见性。
-- =============================================================================

local PREFIX = (...):match("^(.*)%.[^.]+$")

local object = require("Lumi.Basic.object")

local S = require(PREFIX .. ".state")
local M = S.M

local CLASS = 201         -- 只属于 seedgame 的代理类（0..65535）
local GROUP_BULLET = 1
local GROUP_BOSS = 3

local bullet_by_handle = {}
local boss_by_handle = {}
local C = { hits = 0 }

-- 无回调的类：只需要容器结构体 + hit；不参与 object.draw。
object.register(CLASS, {})

-- 创建一个不参与绘制的碰撞代理。
local function make_proxy(x, y, r, group)
    return object.new(CLASS, {
        img_id = 0,
        x = x, y = y,
        vx = 0.0, vy = 0.0,
        group = group,
        visible = false,
        hit = { kind = "ellipse", rx = r, ry = r },
    })
end

-- 为子弹挂接碰撞代理。
function C.attach_bullet(b)
    local proxy = make_proxy(b.x, b.y, b.r, GROUP_BULLET)
    b.proxy = proxy
    bullet_by_handle[proxy.handle] = b
end

-- 移除子弹的碰撞代理。
function C.detach_bullet(b)
    if b.proxy ~= nil then
        bullet_by_handle[b.proxy.handle] = nil
        object.kill(b.proxy.handle)
        b.proxy = nil
    end
end

-- 为 BOSS 挂接碰撞代理。
function C.attach_boss(b)
    local proxy = make_proxy(b.x, b.y, b.r, GROUP_BOSS)
    b.proxy = proxy
    boss_by_handle[proxy.handle] = b
end

-- 移除 BOSS 的碰撞代理。
function C.detach_boss(b)
    if b.proxy ~= nil then
        boss_by_handle[b.proxy.handle] = nil
        object.kill(b.proxy.handle)
        b.proxy = nil
    end
end

-- 清空所有子弹/BOSS 代理。
function C.clear()
    for i = 1, #M.bullets do
        C.detach_bullet(M.bullets[i])
    end
    for i = 1, #M.bosses do
        C.detach_boss(M.bosses[i])
    end
end

-- 把 Lua 计算出的最新位置写回容器并推进。
function C.step(dt)
    -- 位置由 Lua 玩法计算；这里只把最终位置批量写回 C++ 结构体。
    for i = 1, #M.bullets do
        local b = M.bullets[i]
        if b.proxy ~= nil then
            b.proxy.x = b.x
            b.proxy.y = b.y
        end
    end
    for i = 1, #M.bosses do
        local b = M.bosses[i]
        if b.proxy ~= nil then
            b.proxy.x = b.x
            b.proxy.y = b.y
        end
    end
    object.step(dt)
end

-- 声明本帧的子弹/BOSS 碰撞检查。
function C.check()
    -- 声明本帧要检的两组；引擎在阶段 6 末尾统一执行宽相/窄相。
    wag.collide.check(GROUP_BULLET, GROUP_BOSS, function(flat)
        for i = 1, #flat, 2 do
            local bullet = bullet_by_handle[flat[i]]
            local boss = boss_by_handle[flat[i + 1]]
            if bullet ~= nil and boss ~= nil and bullet.owner ~= boss and
               not bullet.dead then
                -- 子弹撞到非发射者的 BOSS：由 C++ 判定，Lua 只执行结果。
                bullet.dead = true
                C.hits = C.hits + 1
            end
        end
    end)
end

return C
