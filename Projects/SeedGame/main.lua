-- =============================================================================
--  Project/seedgame/main.lua
--
--  入口：资源初始化、状态机接线、暂停快照、引擎钩子。
--  玩法在 game.lua，菜单/结点在 menu.lua，共享状态在 state.lua。
-- =============================================================================

local PREFIX = (...):match("^(.*)%.[^.]+$")
local GAME_DIR = "Luminosa/" .. PREFIX:gsub("%.", "/")

local input = require("WagLib.input")
local audio = require("WagLib.audio")
local resolve = require("WagLib.resolve")
local font = require("Lumi.Basic.font")
local shader = require("Lumi.Basic.shader")

local S = require(PREFIX .. ".state")
local game = require(PREFIX .. ".game")
local menu = require(PREFIX .. ".menu")
local replay = require(PREFIX .. ".replay")
local M = S.M

-- 从游玩进入普通暂停：切结点、拍快照、挂起引擎。
local function open_pause()
    if M.mode ~= "play" then
        return
    end
    M.mode = "pause"
    M.pause_index = 1
    menu.goto("seedgame:pause")
    wag.draw.capture_snapshot()
    M.pause_pending = true
    M.pause_ignore = true
    wag.engine.pause()
    wag.log.info("seedgame", "pause requested")
end

-- 从普通暂停恢复游玩。
local function resume_game()
    M.mode = "play"
    menu.goto("seedgame:play")
    M.pause_pending = false
    M.pause_ignore = false
    wag.engine.resume()
    wag.log.info("seedgame", "resume")
end

-- 清空当前游戏/回放并回到主菜单。
local function return_menu()
    local was_paused = wag.engine.get_state() == "paused"
    M.mode = "menu"
    M.menu_index = 1
    M.pause_pending = false
    M.pause_ignore = false
    M.pause_image = nil
    menu.goto("seedgame:menu")
    game.clear()
    if was_paused then
        wag.engine.resume()
    end
    wag.log.info("seedgame", "return menu")
end

-- 保存当前录像后回到主菜单。
local function save_and_menu()
    game.save_replay()
    return_menu()
end

-- 从回放进入回放暂停。
local function open_replay_pause()
    if M.mode ~= "replay" then
        return
    end
    M.mode = "replay_pause"
    M.pause_index = 1
    menu.goto("seedgame:replay_pause")
    wag.draw.capture_snapshot()
    M.pause_pending = true
    M.pause_ignore = true
    wag.engine.pause()
    wag.log.info("seedgame", "replay pause requested")
end

-- 从回放暂停继续播放。
local function resume_replay()
    M.mode = "replay"
    menu.goto("seedgame:replay")
    M.pause_pending = false
    M.pause_ignore = false
    wag.engine.resume()
    wag.log.info("seedgame", "replay resume")
end

-- 读取 rep 并开始回放。
local function start_replay()
    local ok = replay.load()
    if not ok then
        wag.log.warn("seedgame", "no replay")
        return
    end
    game.start(false)
    if not replay.begin_playback() then
        wag.log.warn("seedgame", "empty replay")
        return_menu()
        return
    end
    M.mode = "replay"
    M.fps = 60.0
    menu.goto("seedgame:replay")
    wag.log.info("seedgame", "replay started")
end

-- 回放播放到末尾后的收尾。
local function finish_replay()
    wag.log.info("seedgame", "replay finished")
    return_menu()
end

-- 读取本帧玩家输入，供游戏更新和录像使用。
local function read_controls()
    return {
        left = input.down("seed_left"),
        right = input.down("seed_right"),
        jump = input.press("seed_jump"),
    }
end

-- 执行控制台发来的结点跳转。
local function jump_to_node(id)
    if id == "seedgame:menu" then
        return_menu()
    elseif id == "seedgame:play" then
        if M.mode == "menu" then
            game.start()
            M.mode = "play"
            M.fps = 60.0
            menu.goto("seedgame:play")
        elseif M.mode == "pause" then
            resume_game()
        elseif M.mode == "replay" then
            return_menu()
        end
    elseif id == "seedgame:pause" then
        open_pause()
    elseif id == "seedgame:replay" then
        start_replay()
    elseif id == "seedgame:replay_pause" then
        open_replay_pause()
    else
        wag.log.warn("seedgame", "unknown control node: " .. tostring(id))
    end
end

-- 每帧消费控制通道命令（最多 8 条）。
local function drain_control()
    if wag.control == nil then
        return
    end
    for _ = 1, 8 do
        local verb, arg = wag.control.take()
        if verb == nil then
            break
        end
        if verb == "jump" then
            jump_to_node(arg)
        else
            wag.log.warn("seedgame", "unhandled control: " .. tostring(verb) ..
                " " .. tostring(arg))
        end
    end
end

-- 把当前结点发布给控制台。
local function publish_node()
    if wag.control ~= nil then
        wag.control.publish("node", M.current_node)
    end
end
-- 引擎初始化回调：加载资源、绑定输入、进入主菜单。
function GameInit()
    M.bg = shader.load("seed_bg", GAME_DIR .. "/shaders/bg.glsl")
    M.map = shader.load("seed_map", GAME_DIR .. "/shaders/map.glsl")
    M.circle = shader.load("seed_circle", GAME_DIR .. "/shaders/circle.glsl")
    M.glow = shader.load("seed_glow", GAME_DIR .. "/shaders/glow.glsl")
    M.rect = shader.load("seed_rect", GAME_DIR .. "/shaders/ui_rect.glsl")

    audio.set_dir(GAME_DIR .. "/audio")
    local game_music = audio.load("game.ogg")
    local game_seconds = wag.audio.duration(game_music) or 0.0
    audio.play_music("game.ogg", {
        volume = 0.28,
        loop = true,
        loop_start = 0,
        loop_end = math.floor(game_seconds * audio.SAMPLE_RATE),
    })
    wag.time.set_target_fps(60)
    M.target_fps = 60

    local font_path = resolve.luminosa_dir() .. "/" .. PREFIX:gsub("%.", "/") .. "/江西拙楷2.0.ttf"
    font.load(S.UI_FONT, font_path, 30)
    font.load(S.MENU_FONT, font_path, 40)
    font.load(S.TITLE_FONT, font_path, 64)

    input.bind("seed_left", { keys = { "LEFT" } })
    input.bind("seed_right", { keys = { "RIGHT" } })
    input.bind("seed_jump", { keys = { "Z" } })
    input.bind("seed_confirm", { keys = { "Z" } })
    input.bind("seed_up", { keys = { "UP" } })
    input.bind("seed_down", { keys = { "DOWN" } })
    input.bind("seed_pause", { keys = { "ESCAPE" } })

    M.mode = "menu"
    M.menu_index = 1
    M.current_node = "seedgame:menu"
    M.ready = true
    wag.log.info("seedgame", "地图/玩家/BOSS/弹幕/菜单原型已装载")
end

-- 每逻辑帧回调：状态机与游戏推进。
function ExtFrameFunc(info)
    if not M.ready then
        return
    end

    drain_control()
    publish_node()

    -- 最小化/失焦回来时，引擎的 focus 暂停可能已经自动恢复。
    -- 这里把脚本态重新按回暂停，等待玩家在菜单里显式“继续”。
    if M.mode == "pause" or M.mode == "replay_pause" then
        if wag.engine.get_state() ~= "paused" then
            wag.engine.pause()
        end
        return
    end

    if M.mode == "menu" then
        local action = menu.update_main()
        if action == "start" then
            game.start()
            M.mode = "play"
            M.fps = 60.0
        elseif action == "replay" then
            start_replay()
        elseif action == "quit" then
            wag.engine.exit()
        end
        return
    end

    local dt = (type(info) == "table") and tonumber(info.dtg) or (1.0 / 60.0)

    if M.mode == "replay" then
        if input.press("seed_pause") then
            open_replay_pause()
            return
        end
        local controls = replay.next_controls()
        if controls == nil then
            finish_replay()
            return
        end
        game.update(dt, controls)
        return
    end

    if M.mode ~= "play" then
        return
    end

    if input.press("seed_pause") then
        open_pause()
        return
    end

    game.update(dt, read_controls())
end

-- 每渲染帧回调：按当前状态绘制。
function MainRenderFunc(info)
    if not M.ready then
        return
    end
    if M.mode == "menu" then
        menu.draw_main()
    elseif M.mode == "play" or M.mode == "pause" or
           M.mode == "replay" or M.mode == "replay_pause" then
        -- pause / replay_pause 的请求帧仍要画游戏：阶段 10 的截图拍的是这一帧。
        game.draw()
    end
end

-- 引擎暂停帧回调：处理暂停菜单输入。
function PauseFrameFunc(info)
    if M.mode ~= "pause" and M.mode ~= "replay_pause" then
        return
    end
    drain_control()
    publish_node()
    if M.pause_ignore then
        if not input.down("seed_pause") then
            M.pause_ignore = false
        end
        return
    end

    if M.mode == "replay_pause" then
        local action = menu.update_replay_pause()
        if action == "replay_resume" then
            resume_replay()
        elseif action == "replay_menu" then
            return_menu()
        end
        return
    end

    local action = menu.update_pause()
    if action == "resume" then
        resume_game()
    elseif action == "save_menu" then
        save_and_menu()
    elseif action == "menu" then
        return_menu()
    end
end

-- 引擎暂停帧绘制：快照、背景与暂停菜单。
function PauseRenderFunc(info)
    if M.mode == "menu" then
        menu.draw_main()
        return
    end

    local fw, fh = S.frame_size()
    local vp = wag.space.get_viewport()
    local rw, rh = vp.render.w, vp.render.h
    if M.pause_pending then
        local ok, handle = pcall(wag.image.load_from_snapshot,
            "seed_pause", 0.0, 0.0, rw, rh)
        if ok then
            M.pause_image = handle
            M.pause_pending = false
            M.pause_retry = 0
            wag.log.info("seedgame", "pause snapshot loaded")
        else
            M.pause_retry = (M.pause_retry or 0) + 1
            wag.log.warn("seedgame", "pause snapshot load failed: " .. tostring(handle))
            if M.pause_retry >= 3 then
                M.pause_pending = false
                M.pause_retry = 0
            end
        end
    end

    -- 无论快照是否可用，先画背景。这样失败时不会露出引擎清屏色。
    if M.bg ~= nil then
        shader.draw_fullscreen(M.bg, fw, fh, nil, -10000, 0.0)
    end
    if M.pause_image ~= nil then
        local over = 4.0
        wag.draw.draw_image(M.pause_image, 900000, 0.0,
            -rw * 0.5 - over, rh * 0.5 + over, rw + over * 2.0, rh + over * 2.0)
    end
    if M.mode == "replay_pause" then
        menu.draw_replay_pause()
    else
        menu.draw_pause()
    end
end

-- 失焦回调：普通游玩进普通暂停，回放进回放暂停。
function FocusLoseFunc(info)
    if M.mode == "replay" then
        M.mode = "replay_pause"
        M.pause_index = 1
        menu.goto("seedgame:replay_pause")
        wag.draw.capture_snapshot()
        M.pause_pending = true
        M.pause_ignore = false
        return
    end
    if M.mode ~= "play" then
        return
    end
    M.mode = "pause"
    M.pause_index = 1
    menu.goto("seedgame:pause")
    wag.draw.capture_snapshot()
    M.pause_pending = true
    M.pause_ignore = false
end

-- 获得焦点回调：清理暂停请求并恢复菜单状态。
function FocusGainFunc(info)
    if M.mode ~= "pause" and M.mode ~= "replay_pause" then
        M.pause_pending = false
    end
    if M.mode == "menu" and wag.engine.get_state() == "paused" then
        wag.engine.resume()
    end
end















