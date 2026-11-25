-- =============================================================================
--  Project/seedgame/menu.lua
--
--  主菜单/暂停菜单与结点图。
--  结点图只描述“能去哪里”；按钮动作仍由本文件解释。
-- =============================================================================

local PREFIX = (...):match("^(.*)%.[^.]+$")

local input = require("WagLib.input")
local font = require("Lumi.Basic.font")
local shader = require("Lumi.Basic.shader")
local node_mod = require("Lumi.Basic.node")
local node = node_mod.node

local S = require(PREFIX .. ".state")
local M = S.M

local MENU_ITEMS = { "开始游戏", "精彩回放", "退出游戏" }
local PAUSE_ITEMS = {
    "继续游戏",
    "保存录像并回到主菜单",
    "直接回到主菜单",
}
local REPLAY_PAUSE_ITEMS = {
    "继续播放",
    "回到主菜单",
}

local MENU_NODE = node {
    id = "seedgame:menu", entry = true, kind = "menu",
    edges = { "seedgame:play", "seedgame:replay", "seedgame:quit" },
    items = MENU_ITEMS,
}
local PLAY_NODE = node {
    id = "seedgame:play", kind = "game",
    edges = { "seedgame:pause", "seedgame:menu" },
}
local PAUSE_NODE = node {
    id = "seedgame:pause", kind = "menu",
    edges = { "seedgame:play", "seedgame:menu" },
    items = PAUSE_ITEMS,
}
local REPLAY_PAUSE_NODE = node {
    id = "seedgame:replay_pause", kind = "menu",
    edges = { "seedgame:replay", "seedgame:menu" },
    items = REPLAY_PAUSE_ITEMS,
}
local REPLAY_NODE = node {
    id = "seedgame:replay", kind = "replay",
    edges = { "seedgame:replay_pause", "seedgame:menu" },
}
local QUIT_NODE = node {
    id = "seedgame:quit", kind = "quit", terminal = true,
}

M.current_node = "seedgame:menu"

local menu = {}

-- 按结点图的边校验并切换当前结点。
local function goto_node(id)
    if id == M.current_node then
        return true
    end
    local cur = node_mod.by_id[M.current_node]
    local allowed = false
    if cur ~= nil then
        for _, e in ipairs(node_mod.edges_of(cur)) do
            if e.to == id then
                allowed = true
                break
            end
        end
    end
    if not allowed then
        wag.log.warn("seedgame", string.format("node %s -> %s not in edges",
            tostring(M.current_node), tostring(id)))
    end
    M.current_node = id
    return allowed
end

-- 对外暴露的结点切换入口。
function menu.goto(id)
    return goto_node(id)
end

-- 菜单索引向下循环。
local function wrap_index(index, count)
    index = index + 1
    if index > count then index = 1 end
    return index
end

-- 菜单索引向上循环。
local function wrap_index_back(index, count)
    index = index - 1
    if index < 1 then index = count end
    return index
end

-- 主菜单输入与动作。
function menu.update_main()
    local items = MENU_NODE.items
    if input.press("seed_up") then
        M.menu_index = wrap_index_back(M.menu_index, #items)
    elseif input.press("seed_down") then
        M.menu_index = wrap_index(M.menu_index, #items)
    end

    if input.press("seed_confirm") then
        if M.menu_index == 1 then
            goto_node("seedgame:play")
            return "start"
        elseif M.menu_index == 2 then
            return "replay"
        elseif M.menu_index == 3 then
            goto_node("seedgame:quit")
            return "quit"
        end
    end
    return nil
end

-- 回放暂停菜单输入与动作。
function menu.update_replay_pause()
    if input.press("seed_pause") then
        goto_node("seedgame:replay")
        return "replay_resume"
    end

    local items = REPLAY_PAUSE_NODE.items
    if input.press("seed_up") then
        M.pause_index = wrap_index_back(M.pause_index, #items)
    elseif input.press("seed_down") then
        M.pause_index = wrap_index(M.pause_index, #items)
    end

    if input.press("seed_confirm") then
        if M.pause_index == 1 then
            goto_node("seedgame:replay")
            return "replay_resume"
        elseif M.pause_index == 2 then
            goto_node("seedgame:menu")
            return "replay_menu"
        end
    end
    return nil
end
-- 普通暂停菜单输入与动作。
function menu.update_pause()
    if input.press("seed_pause") then
        goto_node("seedgame:play")
        return "resume"
    end

    local items = PAUSE_NODE.items
    if input.press("seed_up") then
        M.pause_index = wrap_index_back(M.pause_index, #items)
    elseif input.press("seed_down") then
        M.pause_index = wrap_index(M.pause_index, #items)
    end

    if input.press("seed_confirm") then
        if M.pause_index == 1 then
            goto_node("seedgame:play")
            return "resume"
        elseif M.pause_index == 2 then
            goto_node("seedgame:menu")
            return "save_menu"
        elseif M.pause_index == 3 then
            goto_node("seedgame:menu")
            return "menu"
        end
    end
    return nil
end

-- 把世界坐标矩形绘制到屏幕。
local function draw_rect(cx, cy, w, h, color)
    local sx, sy, sw, sh = S.world_box(cx, cy, w, h)
    shader.draw_box(M.rect, sx, sy, sw, sh, color, S.BUTTON_LAYER, 0.0)
end

-- 绘制一个菜单按钮。
local function draw_button(x, y, w, h, text, selected)
    if M.rect == nil then
        return
    end
    local bg
    if selected then
        bg = { 0.40, 0.40, 0.46, 0.96 }
    else
        bg = { 0.24, 0.24, 0.29, 0.92 }
    end
    draw_rect(x, y, w, h, bg)
    font.draw(S.MENU_FONT, text, x, y - 14.0, {
        color = selected and 0xFFFFFFFF or 0xFFD0D0D0,
        align = "center",
        layer = S.UI_LAYER,
    })
end

-- 绘制一组纵向菜单按钮。
local function draw_menu_buttons(items, index, y0, step, w, h)
    for i, text in ipairs(items) do
        draw_button(0.0, y0 - (i - 1) * step, w, h, text, i == index)
    end
end

-- 绘制主菜单。
function menu.draw_main()
    local fw, fh = S.frame_size()
    shader.draw_fullscreen(M.bg, fw, fh, nil, -10000, 0.0)

    font.draw(S.TITLE_FONT, "弹幕ACT 无尽BOSS RUSH", 0.0, 360.0, {
        color = 0xFFFFFFFF, align = "center", layer = S.UI_LAYER,
    })
    font.draw(S.UI_FONT, "踩到BOSS即为击杀。被子弹击中或掉下虚空则重生。跳跃次数没有限制。",
        0.0, 270.0, { color = 0xFFD8D8E8, align = "center", layer = S.UI_LAYER })
    draw_menu_buttons(MENU_ITEMS, M.menu_index, 190.0, 120.0, 520.0, 92.0)
    font.draw(S.UI_FONT, "上箭头/下箭头 选择    Z 确认", 0.0, -320.0, {
        color = 0xFFB8C8D8, align = "center", layer = S.UI_LAYER,
    })
end

-- 绘制回放暂停菜单。
function menu.draw_replay_pause()
    draw_rect(0.0, 0.0, S.WORLD_W, S.WORLD_H, { 0.02, 0.02, 0.04, 0.52 })

    font.draw(S.TITLE_FONT, "回放暂停", 0.0, 360.0, {
        color = 0xFFFFFFFF, align = "center", layer = S.UI_LAYER,
    })
    draw_menu_buttons(REPLAY_PAUSE_ITEMS, M.pause_index, 190.0, 120.0, 620.0, 86.0)
    font.draw(S.UI_FONT, "上/下 选择    Z 确认    Esc 继续", 0.0, -320.0, {
        color = 0xFFB8C8D8, align = "center", layer = S.UI_LAYER,
    })
end

-- 绘制普通暂停菜单。
function menu.draw_pause()
    draw_rect(0.0, 0.0, S.WORLD_W, S.WORLD_H, { 0.02, 0.02, 0.04, 0.52 })

    font.draw(S.TITLE_FONT, "暂停", 0.0, 360.0, {
        color = 0xFFFFFFFF, align = "center", layer = S.UI_LAYER,
    })
    draw_menu_buttons(PAUSE_ITEMS, M.pause_index, 190.0, 120.0, 780.0, 86.0)
    font.draw(S.UI_FONT, "上/下 选择    Z 确认    Esc 继续", 0.0, -320.0, {
        color = 0xFFB8C8D8, align = "center", layer = S.UI_LAYER,
    })
end

return menu







