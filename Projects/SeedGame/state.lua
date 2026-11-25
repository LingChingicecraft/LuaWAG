-- =============================================================================
--  Project/seedgame/state.lua
--
--  共享状态、尺寸、地面与绘制坐标换算。
--  本文件不拥有游戏逻辑，也不注册结点。
-- =============================================================================

local M = {
    ready = false,
    player = {
        x = -880.0, y = 300.0,
        vx = 0.0, vy = 0.0,
        r = 28.0,
    },
    score = 0,
    deaths = 0,
    time = 0.0,
    score_timer = 0.0,
    fps = 0.0,
    target_fps = 60,
    bullets = {},
    bosses = {},
    invuln = 0.0,

    mode = "menu",
    current_node = "seedgame:menu",
    menu_index = 1,
    pause_index = 1,
    last_replay_saved = false,

    pause_pending = false,
    pause_image = nil,
    pause_ignore = false,
}

local S = {
    M = M,
    WORLD_W = 1920.0,
    WORLD_H = 1080.0,
    GRAVITY = -2200.0,
    MOVE_SPEED = 430.0,
    JUMP_SPEED = 860.0,
    BULLET_CAP = 320,

    UI_FONT = "seed_ui",
    MENU_FONT = "seed_menu",
    TITLE_FONT = "seed_title",
    UI_LAYER = 1000000,
    BUTTON_LAYER = 950000,
}

S.GROUND = {
    -- 只保留左侧平台；右侧 2/3 是虚空。
    { x1 = -960.0, x2 = -300.0, top = -420.0 },
}

-- 内部渲染目标像素尺寸；不是输出窗口尺寸，也不是 DIP。
function S.frame_size()
    local w, h = wag.space.get_frame_size()
    return w, h
end

-- 世界中心 → 整帧像素左上角
function S.world_box(x, y, w, h)
    local fw, fh = S.frame_size()
    local sx = (x - w * 0.5 + S.WORLD_W * 0.5) / S.WORLD_W * fw
    local sy = (S.WORLD_H * 0.5 - (y + h * 0.5)) / S.WORLD_H * fh
    return sx, sy, w / S.WORLD_W * fw, h / S.WORLD_H * fh
end

return S



