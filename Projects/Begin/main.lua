-- =============================================================================
--  Project/main.lua
--
--  LuaWAG 启动画面（独立 Project）。
--  只做一件事：远山背景 + 四行居中文案。
-- =============================================================================

local font = require("Lumi.Basic.font")
local shader = require("Lumi.Basic.shader")
local resolve = require("WagLib.resolve")

local PREFIX = (...):match("^(.*)%.[^.]+$")
local ASSET_DIR = resolve.luminosa_dir() .. "/" .. PREFIX:gsub("%.", "/")

local M = {
    ready = false,
    bg = nil,
}

local TITLE_FONT = "splash_title"
local SUB_FONT = "splash_sub"
local TITLE_SIZE = 96
local SUB_SIZE = 64

local TITLE_TEXT = "LuaWAG"
local SUB_TEXTS = {
    "定义属于你的世界",
    "Define the World That Belongs to You",
    "Mundus tibi definendus est",
}

function GameInit()
    local root = resolve.luminosa_dir()
    M.bg = shader.load("splash_bg", ASSET_DIR .. "/shaders/bg.glsl")
    font.load(TITLE_FONT, ASSET_DIR .. "/SourceHanSansSC-Regular.otf", TITLE_SIZE)
    font.load(SUB_FONT, ASSET_DIR .. "/SourceHanSansSC-Regular.otf", SUB_SIZE)
    M.start_clock = os.clock()
    M.ready = true
end

local function draw_splash()
    if not M.ready then
        return
    end

    local fw, fh = wag.space.get_frame_size()
    local now = os.clock() - (M.start_clock or os.clock())
    shader.draw_fullscreen(M.bg, fw, fh, { now, 0.0, 0.0, 0.0 },
        shader.layer.background, 0.0)

    font.draw(TITLE_FONT, TITLE_TEXT, 0.0, 220.0, {
        color = 0xFFFFFFFF,
        align = "center",
        layer = 1000000,
    })

    local y = 90.0
    for i = 1, #SUB_TEXTS do
        local color = 0xFFB8C0D0
        font.draw(SUB_FONT, SUB_TEXTS[i], 0.0, y, {
            color = color,
            align = "center",
            layer = 1000000,
        })
        y = y - 86.0
    end
end

function MainRenderFunc()
    draw_splash()
end

-- 失焦时引擎会进入暂停帧。这里仍画启动画面，避免默认的哨兵色。
function PauseRenderFunc()
    draw_splash()
end

