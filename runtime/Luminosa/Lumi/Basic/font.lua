-- =============================================================================
--  Lumi/Basic/font.lua —— TTF 的 Lua 侧包装（2026-11-18）
--
--  【它补的是什么】引擎只回答"这个字长什么样、在图集哪一块"（wag.font.*）；
--    本文件负责**排版与策略**：
--      · 排版：对齐（left/center/right）、缩放、逐字推进、按页分组提交
--      · 策略：字形缓存、缺字 **L1 一条**（每种字体每个码点只记一次）
--
--  【设计依据】《文字与富文本排版决策》§十七（引擎侧）+ §7.5.6（缺字策略）。
--    用户词汇 `LoadTTF` / `RenderTTF` 留在 **Lua 层**（引擎 API 面不增加），
--    这是文档 4.5 已定的事。
--
--  【不做】换行/内联指令（那是 text.lua 的 label/rich 的职责）；回退链（第一版不做）。
-- =============================================================================

local M = {}

-- -----------------------------------------------------------------------------
--  内部：缓存与缺字去重
-- -----------------------------------------------------------------------------

--- 每个字体一份：{ glyphs = {cp → 拷贝}, metrics = {...}, name = 原名 }
local font_cache = {}

--- 缺字去重：missing_seen[font_name] = { [cp] = true }
local missing_seen = {}

--- **复用缓冲**：scratch[page] = { t = 扁平数组, len = 当前有效长度 }
--
-- 【为什么需要】`draw_glyphs` 每次都要一张扁平数组。若每帧新建，
--   1000 字/帧 = 每帧 ~9000 个 number 的新表 ⇒ GC 抖动（验收项 ③）。
--   这里按**页**各留一份，复用长度、用完把尾巴清掉（保证 `#t` 正确）。
local scratch = {}

-- -----------------------------------------------------------------------------
--  内部：UTF-8 → 码点（带缓存：同一段文本只解一次）
-- -----------------------------------------------------------------------------

local decode_cache = {}
local decode_cache_n = 0
local kDecodeCacheMax = 64

local function decode(s)
    local out = {}
    local i = 1
    local n = #s
    while i <= n do
        local c = s:byte(i)
        local cp, extra
        if c < 0x80 then cp, extra = c, 0
        elseif c < 0xE0 then cp, extra = c % 0x20, 1
        elseif c < 0xF0 then cp, extra = c % 0x10, 2
        elseif c < 0xF8 then cp, extra = c % 0x08, 3
        else cp, extra = c % 0x04, 4 end
        local ok = true
        for k = 1, extra do
            local b = s:byte(i + k)
            if b == nil then ok = false; break end
            cp = cp * 64 + b % 64
        end
        if ok then
            out[#out + 1] = cp
        end
        i = i + extra + 1
    end
    return out
end

--- 带缓存：同一段文本只解一次（验收项 ③ 的"无每帧分配"靠它）
local function codepoints(s)
    local hit = decode_cache[s]
    if hit ~= nil then
        return hit
    end
    local out = decode(s)
    if decode_cache_n >= kDecodeCacheMax then
        decode_cache = {}
        decode_cache_n = 0
    end
    decode_cache[s] = out
    decode_cache_n = decode_cache_n + 1
    return out
end

-- -----------------------------------------------------------------------------
--  载入 / 卸载
-- -----------------------------------------------------------------------------

--- 载入一份 TTF。同名再载 ⇒ 旧字体卸载（与引擎同一条规则）。
--
-- @param name       字体名（用户侧用它引用）
-- @param path       文件路径（.ttf / .otf；通常由工程按安装根拼好）
-- @param pixel_size 像素高度（> 0）
-- @return           true / false（失败原因已经由引擎记成 L3）
function M.load(name, path, pixel_size)
    if type(name) ~= "string" or type(path) ~= "string" then
        return false
    end
    local ok, handle = pcall(wag.font.load_ttf, name, path, pixel_size)
    if not ok then
        wag.log.warn("[font] 载入失败", tostring(handle))
        return false
    end
    local m = wag.font.metrics(handle)
    font_cache[name] = {
        handle = handle,
        name = name,
        pixel_size = m.pixel_size,
        ascent = m.ascent,
        descent = m.descent,
        line_gap = m.line_gap,
        line_height = m.line_height,
        num_glyphs = m.num_glyphs,
        glyphs = {},
    }
    missing_seen[name] = nil
    wag.log.info("[font] 已载入",
                 string.format("%s  %.0fpx  line=%.1f  glyphs=%d",
                               name, m.pixel_size, m.line_height, m.num_glyphs))
    return true
end

--- LuaSTG 的名字（同一函数对象，零包装）
M.LoadTTF = M.load

--- 卸载（幂等）：清掉 Lua 侧缓存与引擎侧字体。
function M.unload(name)
    if font_cache[name] ~= nil then
        font_cache[name] = nil
        missing_seen[name] = nil
        wag.font.unload(name)
    end
end

--- 查询是否已载入（名字存在且引擎句柄仍有效）。
function M.exists(name)
    return font_cache[name] ~= nil
end

--- 取字体句柄（查询语义；未载入 / 缓存失效 ⇒ nil）。
-- 不改 load 的 true/false 语义；给需要直接调 draw_glyphs 的高级用法。
function M.handle(name)
    local f = font_cache[name]
    if f == nil then
        return nil
    end
    return f.handle
end

--- 字体度量（拷贝一份给调用方；不会随引擎复用表变化）。
function M.metrics(name)
    local f = font_cache[name]
    if f == nil then
        return nil
    end
    return {
        pixel_size = f.pixel_size, ascent = f.ascent, descent = f.descent,
        line_gap = f.line_gap, line_height = f.line_height,
        num_glyphs = f.num_glyphs,
    }
end

-- -----------------------------------------------------------------------------
--  字形取用（带缓存与缺字 L1）
-- -----------------------------------------------------------------------------

--- 取一个字形（Lua 侧缓存；首次才回引擎）。
-- @return 拷贝出来的表；字体不存在 ⇒ nil
local function glyph_of(f, cp)
    local g = f.glyphs[cp]
    if g ~= nil then
        return g
    end
    local raw = wag.font.glyph(f.handle, cp)
    if raw == nil then
        -- 句柄失效：这只可能发生在"字体被同名覆盖/卸载"之后
        return nil
    end
    -- ⚠️ 引擎给的是**复用表**：必须当场拷走，否则下一次调用会覆盖它
    g = {
        image = raw.image, u = raw.u, v = raw.v, uw = raw.uw, uh = raw.uh,
        advance = raw.advance, bearing_x = raw.bearing_x,
        bearing_y = raw.bearing_y, w = raw.w, h = raw.h, missing = raw.missing,
    }
    f.glyphs[cp] = g

    if g.missing then
        local seen = missing_seen[f.name]
        if seen == nil then
            seen = {}
            missing_seen[f.name] = seen
        end
        if not seen[cp] then
            seen[cp] = true
            wag.log.info("[font] 缺字",
                         string.format("%s 没有 U+%04X（画 .notdef）", f.name, cp))
        end
    end
    return g
end

--- 量出一段文字的宽度（像素，未乘 scale）。
function M.measure(name, text, scale)
    local f = font_cache[name]
    if f == nil then
        return 0
    end
    scale = scale or 1.0
    local w = 0
    for _, cp in ipairs(codepoints(text)) do
        local g = glyph_of(f, cp)
        if g == nil then
            break
        end
        w = w + g.advance
    end
    return w * scale
end

--- 画一段文字（**只能在渲染相位**调用）。
--
-- @param name     字体名（`M.load` 时给的）
-- @param text     UTF-8 文本
-- @param x        **左端**（按 align 调整前的锚点 x）
-- @param baseline 基线 y（世界坐标，y 向上）
-- @param opts     { color=0xFFFFFFFF, scale=1, align="left"|"center"|"right",
--                   layer=0, z=0 }
-- @return         实际绘制宽度（像素，已乘 scale）；字体不存在 ⇒ nil
-- 本帧用到的页（模块级复用，避免每次 draw 都建表 → 验收项 ③ 的"无每帧分配"）
local active = {}
local nactive = 0

--- 取（或建）某页的复用缓冲。
local function buffer_for(image)
    local buf = scratch[image]
    if buf == nil then
        buf = { t = {}, len = 0, kept = 0, image = image }
        scratch[image] = buf
    end
    return buf
end

function M.draw(name, text, x, baseline, opts)
    local f = font_cache[name]
    if f == nil then
        return nil
    end
    opts = opts or {}
    local color = opts.color or 0xFFFFFFFF
    local scale = opts.scale or 1.0
    local align = opts.align or "left"
    local layer = opts.layer or 0
    local z = opts.z or 0

    local cps = codepoints(text)
    local total = M.measure(name, text, scale)
    local pen = x
    if align == "center" then
        pen = x - total * 0.5
    elseif align == "right" then
        pen = x - total
    end

    nactive = 0
    local any = false
    for _, cp in ipairs(cps) do
        local g = glyph_of(f, cp)
        if g == nil then
            break
        end
        if g.w > 0 and g.h > 0 then
            local buf = buffer_for(g.image)
            if buf.len == 0 then
                nactive = nactive + 1
                active[nactive] = buf
            end
            local arr = buf.t
            local i = buf.len
            arr[i + 1] = pen + g.bearing_x * scale       -- x（左上）
            arr[i + 2] = baseline - g.bearing_y * scale  -- y（上边；世界 y 向上）
            arr[i + 3] = g.w * scale
            arr[i + 4] = g.h * scale
            arr[i + 5] = g.u
            arr[i + 6] = g.v
            arr[i + 7] = g.uw
            arr[i + 8] = g.uh
            arr[i + 9] = color
            buf.len = i + 9
            any = true
        end
        pen = pen + g.advance * scale
    end
    if any then
        for k = 1, nactive do
            local buf = active[k]
            local n = buf.len
            -- 先清尾巴（否则 `#buf.t` 会是**上一次**更长时的长度，多画旧字形）
            if n < buf.kept then
                for i = buf.kept, n + 1, -1 do
                    buf.t[i] = nil
                end
            end
            buf.kept = n
            buf.len = 0
            wag.draw.draw_glyphs(buf.image, layer, z, buf.t)
        end
        for k = 1, nactive do
            active[k] = nil
        end
        nactive = 0
    end
    return total
end

--- LuaSTG 风格的便捷名（固定参数顺序）。
--   `RenderTTF(name, text, x, baseline, color, align, scale, layer, z)`
function M.RenderTTF(name, text, x, baseline, color, align, scale, layer, z)
    return M.draw(name, text, x, baseline, {
        color = color, align = align, scale = scale, layer = layer, z = z,
    })
end

--- 查询某字体有没有这个字（走引擎；用于工具/作者自检）。
function M.has_glyph(name, codepoint)
    local f = font_cache[name]
    if f == nil then
        return false
    end
    return wag.font.has_glyph(f.handle, codepoint)
end

return M
