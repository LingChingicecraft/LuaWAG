-- =============================================================================
--  Project/seedgame/game.lua
--
--  单屏平台/弹幕玩法：玩家、地图、BOSS、子弹、踩头、动态目标帧率。
--  主菜单、暂停菜单与结点切换在 menu.lua / main.lua。
-- =============================================================================

local PREFIX = (...):match("^(.*)%.[^.]+$")

local input = require("WagLib.input")
local audio = require("WagLib.audio")
local font = require("Lumi.Basic.font")
local shader = require("Lumi.Basic.shader")

local S = require(PREFIX .. ".state")
local collision = require(PREFIX .. ".collision")
local replay = require(PREFIX .. ".replay")
local M = S.M
local WORLD_W, WORLD_H = S.WORLD_W, S.WORLD_H
local GRAVITY, MOVE_SPEED, JUMP_SPEED = S.GRAVITY, S.MOVE_SPEED, S.JUMP_SPEED
local GROUND = S.GROUND
local BULLET_CAP = S.BULLET_CAP

-- 把世界坐标圆绘制成屏幕矩形。
local function draw_circle(handle, layer, x, y, r, color)
    local sx, sy, sw, sh = S.world_box(x, y, r * 2.0, r * 2.0)
    shader.draw_box(handle, sx, sy, sw, sh, color, layer, 0.0)
end

-- 绘制叠加混合的光晕。
local function draw_glow(x, y, r, color)
    local sx, sy, sw, sh = S.world_box(x, y, r * 2.0, r * 2.0)
    wag.draw.set_state { blend = "Add" }
    shader.draw_box(M.glow, sx, sy, sw, sh, color, -3500, 0.0)
    wag.draw.reset_state("blend")
end

-- 清空子弹及其碰撞代理。
local function clear_bullets()
    for i = 1, #M.bullets do
        collision.detach_bullet(M.bullets[i])
    end
    M.bullets = {}
end

-- 玩家重生：重置位置、无敌时间和弹幕。
local function respawn_player(reason)
    local p = M.player
    p.x, p.y = -880.0, 300.0
    p.vx, p.vy = 0.0, 0.0
    M.invuln = 1.2
    clear_bullets()
    M.deaths = M.deaths + 1
    wag.log.info("seedgame", string.format("respawn reason=%s deaths=%d", reason, M.deaths))
    if reason == "pit" or reason == "bullet" then
        audio.play_se("se_cancel00.wav", { volume = 0.5 })
    end
end

-- 创建一份 BOSS 数据。
local function make_boss(home_x, home_y, r, color, kind)
    return {
        x = home_x, y = home_y,
        home_x = home_x, home_y = home_y,
        r = r, color = color, kind = kind,
        active = true,
        respawn = 0.0,
        fire = 0.8,
        phase = 0.0,
    }
end

-- 创建两个 BOSS 并挂接碰撞代理。
local function setup_bosses()
    for i = 1, #M.bosses do
        collision.detach_boss(M.bosses[i])
    end
    M.bosses = {
        make_boss(380.0, 120.0, 66.0, { 1.0, 0.55, 0.28, 1.0 }, "radial"),
        make_boss(-420.0, 250.0, 54.0, { 0.70, 0.45, 1.0, 1.0 }, "aimed"),
    }
    for i = 1, #M.bosses do
        collision.attach_boss(M.bosses[i])
    end
end

-- 重置整局状态，开始新一局。
local function restart_game()
    M.player.x, M.player.y = -880.0, 300.0
    M.player.vx, M.player.vy = 0.0, 0.0
    M.score = 0
    M.deaths = 0
    M.time = 0.0
    M.score_timer = 0.0
    M.fps = 0.0
    M.target_fps = 60
    wag.time.set_target_fps(60)
    M.invuln = 0.0
    clear_bullets()
    setup_bosses()
    wag.log.info("seedgame", "restart")
end

-- 更新玩家输入、重力、落地与掉坑。
local function update_player(dt, controls)
    local p = M.player
    local c = controls or {}
    local dir = 0.0
    if c.left then dir = dir - 1.0 end
    if c.right then dir = dir + 1.0 end
    p.vx = dir * MOVE_SPEED

    if c.jump then
        p.vy = JUMP_SPEED
        audio.play_se("se_ch02.wav", { volume = 0.35 })
    end

    p.x = p.x + p.vx * dt
    p.vx = 0.0

    local min_x = -WORLD_W * 0.5 + p.r
    local max_x = WORLD_W * 0.5 - p.r
    if p.x < min_x then p.x = min_x end
    if p.x > max_x then p.x = max_x end

    local old_bottom = p.y - p.r
    p.vy = p.vy + GRAVITY * dt
    p.y = p.y + p.vy * dt

    local max_y = WORLD_H * 0.5 - p.r
    if p.y > max_y then
        p.y = max_y
        if p.vy > 0.0 then p.vy = 0.0 end
    end

    if p.vy <= 0.0 then
        for i = 1, #GROUND do
            local g = GROUND[i]
            if p.x >= g.x1 and p.x <= g.x2 then
                local new_bottom = p.y - p.r
                if old_bottom >= g.top and new_bottom <= g.top then
                    p.y = g.top + p.r
                    p.vy = 0.0
                    break
                end
            end
        end
    end

    if p.y < -700.0 then
        respawn_player("pit")
    end
end

-- 生成一颗子弹并挂接碰撞代理。
local function spawn_bullet(x, y, vx, vy, r, color, owner)
    if #M.bullets >= BULLET_CAP then
        return
    end
    local b = {
        x = x, y = y, vx = vx, vy = vy,
        r = r, color = color, owner = owner,
    }
    collision.attach_bullet(b)
    M.bullets[#M.bullets + 1] = b
end

-- 按 BOSS 类型发射弹幕。
local function fire_boss(b)
    local p = M.player
    if b.kind == "radial" then
        for i = 0, 11 do
            local a = b.phase + i * (math.pi * 2.0 / 12.0)
            spawn_bullet(b.x, b.y, math.cos(a) * 260.0, math.sin(a) * 260.0,
                10.0, { 1.0, 0.55, 0.25, 1.0 }, b)
        end
    else
        local base = math.atan2(p.y - b.y, p.x - b.x)
        for i = -1, 1 do
            local a = base + i * 0.18
            spawn_bullet(b.x, b.y, math.cos(a) * 300.0, math.sin(a) * 300.0,
                9.0, { 0.75, 0.55, 1.0, 1.0 }, b)
        end
    end
end

-- 更新 BOSS 移动、开火、踩头与重生。
local function update_bosses(dt)
    local p = M.player
    for i = 1, #M.bosses do
        local b = M.bosses[i]
        if not b.active then
            b.respawn = b.respawn - dt
            if b.respawn <= 0.0 then
                b.active = true
                b.x, b.y = b.home_x, b.home_y
                b.phase = 0.0
                b.fire = 0.7
                collision.attach_boss(b)
                wag.log.info("seedgame", "boss respawn")
            end
        else
            b.phase = b.phase + dt
            if b.kind == "radial" then
                b.y = b.home_y + math.sin(b.phase * 1.6) * 26.0
            else
                b.x = b.home_x + math.sin(b.phase * 0.9) * 150.0
                b.y = b.home_y + math.cos(b.phase * 1.3) * 20.0
            end

            b.fire = b.fire - dt
            if b.fire <= 0.0 then
                fire_boss(b)
                b.fire = (b.kind == "radial") and 1.15 or 0.9
            end

            local dx = p.x - b.x
            local dy = p.y - b.y
            local rr = p.r + b.r
            if dx * dx + dy * dy < rr * rr then
                if p.vy < 0.0 and p.y > b.y + b.r * 0.15 then
                    collision.detach_boss(b)
                    b.active = false
                    b.respawn = 7.0--BOSS重生间隔
                    M.score = M.score + 100
                    p.vy = 760.0
                    audio.play_se("se_bonus.wav", { volume = 0.55 })
                    wag.log.info("seedgame", "boss stomped")
                end
            end
        end
    end
end

-- 更新子弹位置并处理玩家中弹。
local function update_bullets(dt)
    local p = M.player
    for i = #M.bullets, 1, -1 do
        local b = M.bullets[i]
        b.x = b.x + b.vx * dt
        b.y = b.y + b.vy * dt
        if b.x < -1200.0 or b.x > 1200.0 or b.y < -800.0 or b.y > 800.0 or b.dead then
            collision.detach_bullet(b)
            table.remove(M.bullets, i)
        else
            local dx = p.x - b.x
            local dy = p.y - b.y
            local rr = p.r + b.r
            if M.invuln <= 0.0 and dx * dx + dy * dy < rr * rr then
                respawn_player("bullet")
                return
            end
        end
    end
end

-- 绘制所有 BOSS 与子弹。
local function draw_bosses_and_bullets()
    for i = 1, #M.bosses do
        local b = M.bosses[i]
        if b.active then
            draw_glow(b.x, b.y, b.r * 2.2, { b.color[1], b.color[2], b.color[3], 0.22 })
            draw_circle(M.circle, 150, b.x, b.y, b.r, b.color)
        end
    end
    for i = 1, #M.bullets do
        local b = M.bullets[i]
        draw_circle(M.circle, 100, b.x, b.y, b.r, b.color)
    end
end

-- 按最近 BOSS 的距离计算目标帧率。
local function desired_target_fps()
    local p = M.player
    local nearest = 1e30
    for i = 1, #M.bosses do
        local b = M.bosses[i]
        if b.active then
            local dx = p.x - b.x
            local dy = p.y - b.y
            local d = math.sqrt(dx * dx + dy * dy)
            if d < nearest then nearest = d end
        end
    end
    if nearest >= 450.0 then
        return 60
    end
    local step = math.floor(nearest / 75.0)
    if step > 5 then step = 5 end
    return 30 + step * 5
end

-- 需要时应用新的目标帧率。
local function update_target_fps()
    local want = desired_target_fps()
    if want ~= M.target_fps then
        M.target_fps = want
        wag.time.set_target_fps(want)
        wag.log.info("seedgame", string.format("target fps -> %d", want))
    end
end

local game = {}

-- 开始一局；record=false 时进入回放。
function game.start(record)
    restart_game()
    if record == false then
        replay.stop()
    else
        replay.begin_recording()
    end
end

-- 清空游戏对象并停止回放。
function game.clear()
    replay.stop()
    collision.clear()
    M.bullets = {}
    M.bosses = {}
    M.pause_pending = false
    M.pause_image = nil
    M.fps = 60.0
    M.target_fps = 60
    wag.time.set_target_fps(60)
end

-- 推进一帧玩法。
function game.update(dt, controls)
    replay.record(controls, dt)
    M.time = M.time + dt
    M.score_timer = M.score_timer + dt
    while M.score_timer >= 1.0 do
        M.score = M.score + 10
        M.score_timer = M.score_timer - 1.0
    end
    if M.invuln > 0.0 then
        M.invuln = M.invuln - dt
    end
    update_player(dt, controls)
    update_bosses(dt)
    update_bullets(dt)
    collision.step(dt)
    collision.check()
    update_target_fps()
    M.fps = M.target_fps
end

-- 绘制一帧游戏画面与 HUD。
function game.draw()
    local fw, fh = S.frame_size()
    if not M.logged_frame then
        M.logged_frame = true
        wag.log.info("seedgame", string.format("frame=%dx%d player=(%.1f,%.1f)",
            fw, fh, M.player.x, M.player.y))
    end

    shader.draw_fullscreen(M.bg, fw, fh, nil, -10000, 0.0)
    for i = 1, #GROUND do
        local g = GROUND[i]
        local cx = (g.x1 + g.x2) * 0.5
        local cy = (g.top + (-620.0)) * 0.5
        local sx, sy, sw, sh = S.world_box(cx, cy, g.x2 - g.x1, g.top - (-620.0))
        shader.draw_box(M.map, sx, sy, sw, sh, nil, -5000, 0.0)
    end

    draw_bosses_and_bullets()

    local p = M.player
    if M.invuln <= 0.0 or (math.floor(M.time * 16.0) % 2 == 0) then
        draw_glow(p.x, p.y, 150.0, { 0.25, 0.65, 1.0, 0.30 })
        draw_circle(M.circle, 200, p.x, p.y, p.r, { 0.55, 0.92, 1.0, 1.0 })
    end

    local hud = string.format("分数 %06d    死亡 %d", M.score, M.deaths)
    font.draw(S.UI_FONT, hud, -900.0, 500.0,
        { color = 0xFFFFFFFF, layer = S.UI_LAYER })
    if M.mode == "replay" or M.mode == "replay_pause" then
        local hud_w = font.measure(S.UI_FONT, hud)
        font.draw(S.UI_FONT, string.format("当前播放进度：%.1f%%", replay.progress(M.time)),
            -900.0 + hud_w + 24.0, 500.0,
            { color = 0xFFFFD080, layer = S.UI_LAYER })
    end
    if M.target_fps < 60 then
        font.draw(S.UI_FONT, "BOSS正在减速时间",
            740.0, -500.0, { color = 0xFFFFB870, align = "right", layer = S.UI_LAYER })
    end
    font.draw(S.UI_FONT, string.format("逻辑帧率 %.1f", M.fps),
        900.0, -500.0, { color = 0xFF9FE0A0, align = "right", layer = S.UI_LAYER })
    font.draw(S.UI_FONT, "←/→ 移动   Z 跳跃   Esc 暂停",
        -900.0, -500.0, { color = 0xFFB8C8D8, layer = S.UI_LAYER })
end

-- 保存当前这局的单槽 rep。
function game.save_replay()
    local ok, err = replay.save()
    if ok then
        M.last_replay_saved = true
        return true
    end
    M.last_replay_saved = false
    wag.log.warn("seedgame", "save replay failed: " .. tostring(err))
    return false
end

return game





