-- =============================================================================
--  Luminosa/Lumi/Basic/atlas.lua
--
--  切图增强库：把"一张规则的字符图集"切成引擎能用的**字体表**（2026-11-16）。
--
--  【为什么要有它】手写 `glyphs[65] = { u=…, v=…, w=…, h=… }` 九十多次是坐牢。
--    这一层只做一件事：**按行列数把图切成格子**，然后把格子写进字形表。
--    ⇒ 引擎侧仍然只认 `draw_glyphs`（不认字体），这里纯粹是 Lua 侧的便利。
--
--  【它不猜】列数、行数、起始字符都**必须显式给**。
--    图集里"哪个格子是哪个字符"是**素材的约定**，不是能算出来的东西。
--
--  用法：
--      local atlas = require("Lumi.Basic.atlas")
--
--      -- ① 直接给图片句柄 + 尺寸 + 行列
--      local font = atlas.grid {
--          image = handle, w = 512, h = 512,
--          cols = 12, rows = 8,
--          first = 32,                 -- 第一个格子是 ASCII 32（空格）
--      }
--
--      -- ② 一步到位：加载图片 + 切分
--      local font = atlas.load {
--          name = "score", path = ".../score.png",
--          w = 512, h = 512, cols = 12, rows = 8, first = 32,
--      }
--
--      wag.text.label(font, "Hello", 0, 0, 4.0, "#FFFFFF", "center", 20, 0)
--
--  【可选参数】
--      area     = { x=, y=, w=, h= }   -- 图集里**实际有效**的区域（默认整张图）
--                                        图下方留白、或只在左上角画了字符时用它
--      pad      = { left=, top=, right=, bottom= }  -- 每格再内缩（像素）
--                                        字符不顶满格子时，用它把空白切掉
--      advance  = <数>                 -- 步进（默认 = 格宽）
--      line_height = <数>              -- 行高（默认 = 格高）
--      missing  = "error" (默认)        -- 缺字策略：目前只支持 error
-- =============================================================================

local M = {}

-- -----------------------------------------------------------------------------
--  grid：纯切分（图已经加载好了）
-- -----------------------------------------------------------------------------
function M.grid(opt)
    if type(opt) ~= "table" then
        error("atlas.grid 需要一个表参数", 2)
    end

    local image = opt.image
    if type(image) ~= "number" then
        error("atlas.grid: image 必须是图片句柄", 2)
    end

    local cols = tonumber(opt.cols)
    local rows = tonumber(opt.rows)
    if cols == nil or cols <= 0 or rows == nil or rows <= 0 then
        error("atlas.grid: cols / rows 必须是正整数", 2)
    end

    -- 有效区域（默认整张图）
    local area = opt.area or {}
    local ax = tonumber(area.x) or 0.0
    local ay = tonumber(area.y) or 0.0
    local aw = tonumber(area.w) or tonumber(opt.w)
    local ah = tonumber(area.h) or tonumber(opt.h)
    if aw == nil or ah == nil or aw <= 0 or ah <= 0 then
        error("atlas.grid: 需要 w/h（图片尺寸）或 area 的四项", 2)
    end

    local cw = aw / cols          -- 格宽（纹理像素）
    local ch = ah / rows          -- 格高

    -- 内缩（字符不顶满格子时用）
    local pad = opt.pad or {}
    local pl = tonumber(pad.left) or 0.0
    local pt = tonumber(pad.top) or 0.0
    local pr = tonumber(pad.right) or 0.0
    local pb = tonumber(pad.bottom) or 0.0

    local first = tonumber(opt.first) or 32
    local advance = tonumber(opt.advance) or cw
    local line_height = tonumber(opt.line_height) or ch

    local glyphs = {}
    for i = 0, cols * rows - 1 do
        local col = i % cols
        local row = math.floor(i / cols)
        glyphs[first + i] = {
            u = ax + col * cw + pl,
            v = ay + row * ch + pt,
            w = cw - pl - pr,
            h = ch - pt - pb,
            advance = advance,
        }
    end

    return {
        image = image,
        line_height = line_height,
        glyphs = glyphs,
        -- 诊断用：切了什么、切了多少
        _meta = { cols = cols, rows = rows, cell_w = cw, cell_h = ch, first = first },
    }
end

-- -----------------------------------------------------------------------------
--  load：加载图片 + 切分（最常见的一步到位）
-- -----------------------------------------------------------------------------
--
--  【name 用来干什么】它是图片的**逻辑名**（`wag.image.load_from_file` 的第二个参数），
--    后面 `wag.image.unload(name)` 卸得掉；同时这里也用它做"已加载"的判断。
function M.load(opt)
    if type(opt) ~= "table" then
        error("atlas.load 需要一个表参数", 2)
    end
    local name = opt.name
    if type(name) ~= "string" then
        error("atlas.load: 需要 name（图片逻辑名）", 2)
    end

    local image = wag.image.lookup(name)
    if image == nil then
        if type(opt.path) ~= "string" then
            error("atlas.load: 图片 '" .. name .. "' 还没加载，且没给 path", 2)
        end
        image = wag.image.load_from_file(name, opt.path)
    end

    opt.image = image

    return M.grid(opt)
end
    
-- -----------------------------------------------------------------------------
--  from_data：用"预切好的字形表"造字体（逐像素提取的那条路）
-- -----------------------------------------------------------------------------
--
--  规则图表用 `grid`；**比例排版的整段文本图**（每行字数不同）切不出格子，
--  那种图要用工具逐像素提取，产出一张 Lua 数据表，再走这里。
--
--  data 的形状（由生成工具产出）：
--      { line_height = 46, ascent = 30, space_advance = 20,
--        glyphs = { [33] = { u=, v=, w=, h=, advance=, dy= }, … } }
--
--  `dy` 是**相对基线**的竖直偏移（负数 = 基线上方）。有 `ascent` 时，
--  竖直位置按基线算（见 Lumi/Basic/text.lua）；没有则退化为"顶端对齐行盒顶"。
function M.from_data(image, data)
    if type(image) ~= "number" then
        error("atlas.from_data: image 必须是图片句柄", 2)
    end
    if type(data) ~= "table" or type(data.glyphs) ~= "table" then
        error("atlas.from_data: data 必须是生成工具产出的字形表", 2)
    end
    local glyphs = {}
    for cp, g in pairs(data.glyphs) do
        glyphs[cp] = {
            u = g.u, v = g.v, w = g.w, h = g.h,
            advance = g.advance, dy = g.dy,
        }
    end
    return {
        image = image,
        line_height = data.line_height,
        ascent = data.ascent,
        space_advance = data.space_advance,
        glyphs = glyphs,
    }
end

-- -----------------------------------------------------------------------------
--  load_data：加载图片 + 用字形表造字体
-- -----------------------------------------------------------------------------
function M.load_data(opt)
    local name = opt.name
    if type(name) ~= "string" then
        error("atlas.load_data: 需要 name", 2)
    end
    local image = wag.image.lookup(name)
    if image == nil then
        if type(opt.path) ~= "string" then
            error("atlas.load_data: 图片 '" .. name .. "' 还没加载，且没给 path", 2)
        end
        image = wag.image.load_from_file(name, opt.path)
    end
    return M.from_data(image, require(opt.data))
end
-- -----------------------------------------------------------------------------
--  自注册（与 rng.lua / shader.lua 同一约定）
-- -----------------------------------------------------------------------------
--
-- 【为什么自己注册而不是等引导层装】层序是 ②Lumi → ③Nosa → ④Project，
--   而 Project 一装载就可能要 `lumi.atlas`（在 on_game_init 里切字体）。
--   等别人来装 ⇒ 症状是"使用者先跑、lumi.atlas 还是 nil"。
lumi = lumi or {}
lumi.atlas = M

return M