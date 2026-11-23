-- =============================================================================
--  Luminosa/Lumi/Basic/text.lua
--
--  文字：库层入口（2026-11-16，P7 阶段1）。
--
--  【它做什么】把"一串字符 + 一张字表"翻译成一串**已排好的字形四边形**，
--    然后交给引擎的 `wag.draw.draw_glyphs`。引擎**不排版、不换行、不查字体**
--    （《文字与富文本排版决策》§4.7）—— 排版全部在这一层。
--
--  【阶段1 的范围】（§十四）
--    · 只有 **ASCII**（`>= 0x80` 的字节直接报错，中文留给 FreeType 那一批）
--    · **单行**（不换行、不折行）
--    · 只做 `label`（一次性绘制）；`rich`（长驻、有排版）属阶段3
--
--  【字体是什么】一个**普通的 Lua 表**（引擎侧没有字体对象，§14.2 D2）：
--      font = {
--          image       = <图片句柄>,          -- 图集页（必填）
--          line_height = <数>,                -- 行盒高度（逻辑单位；必填）
--          glyphs      = {                    -- 码点 → 字形
--              [65] = { u=…, v=…, w=…, h=…, advance=… },   -- u/v/w/h 是**纹理像素**
--          },
--      }
--
--  【缺字 ⇒ 报错】点阵字体的字符集是封闭的，查得出来（§7.5.2）⇒
--    与 §6.2.9"未知指令一律显式报错"同一风格：不画豆腐、不跳过。
-- =============================================================================

local M = {}

-- -----------------------------------------------------------------------------
--  统一的报错入口
-- -----------------------------------------------------------------------------
--
--  【为什么不用 `error()`】`wag.fail` 是引擎的报错通道：它把 where/got/want
--    写进日志与 errors.jsonl。
--
--  【⚠️ 必须显式给 `level = raise`】`wag.fail` 的 level **默认是 warn ——
--    只记录、不抛**。缺字/非法字符要"这次调用不生效"，那必须是 raise（L3）：
--    有落点（钩子里的 pcall）⇒ 抛成 Lua 错误 ⇒ 被接住 ⇒ L2 语义（记录、帧继续）。
local function fail(where, got, want)
    wag.fail {
        level = wag.log.level.raise,
        where = where,
        got = got,
        want = want,
    }
end

-- -----------------------------------------------------------------------------
-- -----------------------------------------------------------------------------
--  颜色名（**库层**提供，不进引擎）
-- -----------------------------------------------------------------------------
--
--  《文字与富文本排版决策》§6.2.6：引擎里**不存任何颜色名**，它们由 Lua 侧的
--  增强库展开成 §6.2.5 的四种规范形态之一；且**只在参数位置生效**
--  （不做全局裸词替换 —— 那会误伤 `\size{small}` 这类参数）。
--
--  ⚠️【D1 的直接影响】`\color{red}{ABC}` 是定案的例子 ⇒ 名字**必须**能用。
local COLOR_NAMES = {
    white   = "#FFFFFF",
    black   = "#000000",
    red     = "#FF0000",
    green   = "#00FF00",
    blue    = "#0000FF",
    yellow  = "#FFFF00",
    cyan    = "#00FFFF",
    magenta = "#FF00FF",
    gray    = "#808080",
    grey    = "#808080",
    orange  = "#FFA500",
}

--  颜色：四种形态（§6.2.5）
-- -----------------------------------------------------------------------------
--
--  "#RRGGBB" / "#RRGGBBAA" / "r,g,b" / "r,g,b,a"；省略 alpha = 255。
--  数字原样接受（视为 0xAARRGGBB，与容器 rgba 同一个约定）。
local function parse_color(c)
    if type(c) == "number" then
        return math.floor(c)
    end
    if type(c) ~= "string" then
        fail("颜色参数", type(c), "#RRGGBB / #RRGGBBAA / r,g,b / r,g,b,a / 颜色名")
    end

    -- 颜色名 → 规范形态（库层展开，见文件头的 COLOR_NAMES）
    local named = COLOR_NAMES[c]
    if named ~= nil then
        c = named
    end

    -- "#RRGGBB" / "#RRGGBBAA"
    if string.sub(c, 1, 1) == "#" then
        local hex = string.sub(c, 2)
        if #hex ~= 6 and #hex ~= 8 then
            fail("颜色参数", "#" .. hex, "6 或 8 位十六进制")
        end
        local function byte_at(i)
            local v = tonumber(string.sub(hex, i, i + 1), 16)
            if v == nil then
                fail("颜色参数", "#" .. hex, "只含 0-9 a-f A-F")
            end
            return v
        end
        local r, g, b = byte_at(1), byte_at(3), byte_at(5)
        local a = (#hex == 8) and byte_at(7) or 255
        return a * 16777216 + r * 65536 + g * 256 + b
    end

    -- "r,g,b" / "r,g,b,a"
    local parts = {}
    for piece in string.gmatch(c, "[^,]+") do
        parts[#parts + 1] = tonumber(piece)
    end
    if #parts ~= 3 and #parts ~= 4 then
        fail("颜色参数", c, "3 或 4 个分量")
    end
    for i = 1, #parts do
        if parts[i] == nil then
            fail("颜色参数", c, "分量都必须是数字")
        end
    end
    local a = parts[4] or 255
    return a * 16777216 + parts[1] * 65536 + parts[2] * 256 + parts[3]
end

-- -----------------------------------------------------------------------------
--  label：一次性、单行
-- -----------------------------------------------------------------------------
--
--  wag.text.label(font, text, x, y [, size [, color [, align [, layer [, z]]]]])
--
--  【align】`"center"`（默认）/`"left"`/`"right"`：**文本的哪一个点对准 (x, y)**。
--    纵向固定：行盒中线对准 y（§4.6.2 的"没有上下对齐"）。
--  【layer / z】§4.6.2 的七参数表里没有它们；这里作为**可选扩展**加在后面 ——
--    否则文字只能画在 0 层，与其他东西没法排序。默认 (0, 0)。
function M.label(font, text, x, y, size, color, align, layer, z, opts)
    -- =========================================================================
    --  基础校验（阶段1 原样）
    -- =========================================================================
    if type(font) ~= "table" then
        fail("label 的 font", type(font), "字体表（引擎侧没有字体对象）")
    end
    if type(font.glyphs) ~= "table" then
        fail("font.glyphs", type(font.glyphs), "一张字形表（码点 → 字形）")
    end
    if type(font.image) ~= "number" then
        fail("font.image", type(font.image), "一个图集页句柄")
    end
    if type(text) ~= "string" then
        fail("label 的 text", type(text), "字符串")
    end

    size = size or 1.0
    color = parse_color(color or "#FFFFFF")
    align = align or "center"
    layer = layer or 0
    z = z or 0

    if align ~= "center" and align ~= "left" and align ~= "right" then
        fail("label 的 align", tostring(align), "center / left / right")
    end

    local line_h = tonumber(font.line_height)
    if line_h == nil or line_h <= 0 then
        fail("font.line_height", tostring(font.line_height), "大于 0 的数")
    end

    -- ---- 可选参数（opts）----
    local opt = {}
    if type(opts) == "number" then
        opt.width = opts
    elseif type(opts) == "table" then
        opt = opts
    elseif opts ~= nil then
        fail("label 的 opts", type(opts), "表（或一个数 = width）")
    end

    local wrap_w   = tonumber(opt.width)
    local leading  = tonumber(opt.leading) or 0.0
    local tracking = tonumber(opt.tracking) or 0.0
    local para_gap = tonumber(opt.paragraph_gap) or 0.0

    if wrap_w ~= nil and wrap_w <= 0 then
        fail("opts.width", tostring(opt.width), "大于 0 的数（省略 = 不换行）")
    end

    -- D3：em 先用 line_height（将来由生成工具产出 font.em 再切换）
    local em = tonumber(font.em) or line_h

    -- =========================================================================
    --  ① 词法 + 语法 → 扁平的 item 流
    -- =========================================================================
    --
    --  item 的种类：
    --    glyph  {cp, g, adv, scale, color}   一个字形
    --    space  {scale, em?}                 空格 / \quad / \qquad
    --    br     {}                           段内硬换行（\\）
    --    par    {}                           段界（空行 / \par）
    --    align  {value}                      段对齐（随后被折叠进段属性）
    local items = {}

    local function is_alpha(ch)
        return ch ~= "" and string.find(ch, "^[A-Za-z]$") ~= nil
    end

    -- D6：报错带**字符位置 + 该处片段**
    local function fail_at(pos, msg)
        local snip = string.sub(text, pos, math.min(#text, pos + 11))
        fail("第 " .. pos .. " 个字符处（\"" .. snip .. "…\"）", msg,
             "见《文字与富文本排版决策》§十六")
    end

    -- 读一个 {…} 参数，返回 内容, 下一个位置
    local function read_brace(i)
        if string.sub(text, i, i) ~= "{" then
            fail_at(i, "缺花括号参数（指令后面必须紧跟 {…}）")
        end
        local j = string.find(text, "}", i + 1, true)
        if j == nil then
            fail_at(i, "花括号未闭合")
        end
        return string.sub(text, i + 1, j - 1), j + 1
    end

    local parse_into

    parse_into = function(i, out, cur_scale, cur_color, stop_at_close)
        local newline_run = 0
        local pending_space = false
        local n = #text

        local function flush_ws()
            if newline_run >= 2 then
                out[#out + 1] = { kind = "par" }
                pending_space = false
            elseif newline_run == 1 then
                pending_space = true
            end
            newline_run = 0
        end

        local function emit_space(sc)
            if pending_space then
                out[#out + 1] = { kind = "space", scale = sc }
                pending_space = false
            end
        end

        while i <= n do
            local c = string.sub(text, i, i)

            if c == "\n" then
                newline_run = newline_run + 1
                i = i + 1

            elseif c == " " or c == "\t" then
                flush_ws()
                pending_space = true
                i = i + 1

            elseif c == "}" then
                if not stop_at_close then
                    fail_at(i, "多余的 }")
                end
                return i + 1

            elseif c == "{" then
                fail_at(i, "孤立的花括号（括号只能跟在指令后）")

            elseif c == "\\" then
                flush_ws()
                local nxt = string.sub(text, i + 1, i + 1)

                if nxt == "\\" then
                    out[#out + 1] = { kind = "br" }
                    pending_space = false
                    i = i + 2
                else
                    local j = i + 1
                    while j <= n and is_alpha(string.sub(text, j, j)) do j = j + 1 end
                    local name = string.sub(text, i + 1, j - 1)
                    if name == "" then
                        fail_at(i, "裸反斜杠后必须跟指令名（要硬换行请写 \\\\）")
                    end

                    if name == "quad" or name == "qquad" then
                        emit_space(cur_scale)
                        out[#out + 1] = {
                            kind = "space", scale = cur_scale,
                            em = (name == "quad") and 1 or 2,
                        }
                        i = j

                    elseif name == "par" then
                        out[#out + 1] = { kind = "par" }
                        pending_space = false
                        i = j

                    elseif name == "align" then
                        local arg, nxt_i = read_brace(j)
                        if arg ~= "left" and arg ~= "center" and arg ~= "right" then
                            fail_at(j, "\\align 的参数只能是 left / center / right，got " .. arg)
                        end
                        out[#out + 1] = { kind = "align", value = arg }
                        pending_space = false
                        i = nxt_i

                    elseif name == "color" or name == "size" then
                        local arg, nxt_i = read_brace(j)
                        if string.sub(text, nxt_i, nxt_i) ~= "{" then
                            fail_at(nxt_i, "\\" .. name .. " 的参数后必须紧跟 {…} 作用域块")
                        end
                        -- 【空格不能被吞掉】`Plain \color{red}{RED}` 里的那个空格
                        --   必须在**进入作用域之前**吐出来 —— 递归用的是新的一份
                        --   pending_space，外层这个不会自己生效。
                        emit_space(cur_scale)

                        local ns, nc = cur_scale, cur_color
                        if name == "color" then
                            nc = parse_color(arg)
                        else
                            local v = tonumber(arg)
                            if v == nil or v <= 0 then
                                fail_at(j, "\\size 的参数必须是大于 0 的数，got " .. arg)
                            end
                            ns = cur_scale * v
                        end
                        local ni = parse_into(nxt_i + 1, out, ns, nc, true)
                        pending_space = false
                        i = ni

                    else
                        fail_at(i, "未知指令 \\" .. name)
                    end
                end

            else
                flush_ws()
                local cp = string.byte(c)
                if cp >= 128 then
                    fail_at(i, "非 ASCII 字节 " .. cp .. "（阶段3 仍只支持 ASCII）")
                end
                emit_space(cur_scale)
                local g = font.glyphs[cp]
                if g == nil and cp ~= 32 then
                    fail_at(i, "缺字：font.glyphs[" .. cp .. "] 不存在（字符 " .. c .. "）")
                end
                out[#out + 1] = {
                    kind = "glyph", cp = cp, g = g,
                    adv = g and g.advance or (font.space_advance or line_h * 0.5),
                    scale = cur_scale, color = cur_color,
                }
                i = i + 1
            end
        end

        if stop_at_close then
            fail_at(n, "花括号未闭合")
        end
        return i
    end

    parse_into(1, items, size, color, false)

    -- =========================================================================
    --  ② 折叠成段：par 分段，align 变段属性（D5：后者生效）
    -- =========================================================================
    local segments = {}
    local cur_seg = { items = {}, align = align }
    for k = 1, #items do
        local it = items[k]
        if it.kind == "par" then
            segments[#segments + 1] = cur_seg
            cur_seg = { items = {}, align = align }
        elseif it.kind == "align" then
            cur_seg.align = it.value
        else
            cur_seg.items[#cur_seg.items + 1] = it
        end
    end
    segments[#segments + 1] = cur_seg

    -- 末尾的空段不要（"a\par" 不该多出一个空行）
    while #segments > 1 and #segments[#segments].items == 0 do
        segments[#segments] = nil
    end

    -- =========================================================================
    --  ③ 每段的 item 推进量
    -- =========================================================================
    local function item_adv(it)
        if it.kind == "space" then
            if it.em ~= nil then
                return (em * it.em + tracking) * it.scale
            end
            return ((font.space_advance or line_h * 0.5) + tracking) * it.scale
        end
        return (it.adv + tracking) * it.scale
    end

    local function line_width(line)
        if #line == 0 then return 0.0 end
        local w = 0.0
        for k = 1, #line do w = w + item_adv(line[k]) end
        return w - tracking * line[#line].scale
    end

    -- =========================================================================
    --  ④ 换行：按 br 切"硬行组"，组内再按宽度软换行
    -- =========================================================================
    local function wrap(items_in)
        local out = {}
        if wrap_w == nil then
            out[1] = items_in
            return out
        end

        local i = 1
        local n = #items_in
        while i <= n do
            -- 行首空格（含 quad 之前那种）丢弃
            while i <= n and items_in[i].kind == "space" do i = i + 1 end
            if i > n then break end

            local line = {}
            local last_space = nil
            while i <= n do
                local it = items_in[i]
                local cand = line_width(line) + item_adv(it)
                if #line > 0 and cand > wrap_w then break end
                line[#line + 1] = it
                if it.kind == "space" then last_space = #line end
                i = i + 1
                if #line == 1 and line_width(line) > wrap_w then break end
            end

            if i <= n and last_space ~= nil and last_space < #line then
                local back = #line - last_space
                i = i - back
                for k = #line, last_space + 1, -1 do line[k] = nil end
            end
            while #line > 0 and line[#line].kind == "space" do line[#line] = nil end

            out[#out + 1] = line
        end

        if #out == 0 then out[1] = {} end
        return out
    end

    -- =========================================================================
    --  ⑤ 逐行定位 + 组扁平平元数组
    -- =========================================================================
    local flat = {}
    local n_out = 0
    local y_line = y
    local lead_step = leading * size       -- D4：leading 始终乘**基准 size**

    for si = 1, #segments do
        local seg = segments[si]
        if si > 1 then y_line = y_line - para_gap * size end

        -- 按 br 切成硬行组
        local chunks = {}
        local chunk = {}
        for k = 1, #seg.items do
            local it = seg.items[k]
            if it.kind == "br" then
                chunks[#chunks + 1] = chunk
                chunk = {}
            else
                chunk[#chunk + 1] = it
            end
        end
        chunks[#chunks + 1] = chunk

        for ci = 1, #chunks do
            local lines = wrap(chunks[ci])
            for li = 1, #lines do
                local line = lines[li]

                -- 行高 = line_height × 该行最大 scale（D4）
                local max_scale = size
                for k = 1, #line do
                    if line[k].scale > max_scale then max_scale = line[k].scale end
                end
                local lh = line_h * max_scale

                -- 行内对齐（段属性）
                local w = line_width(line)
                local cx = x
                if seg.align == "center" then
                    cx = x - w * 0.5
                elseif seg.align == "right" then
                    cx = x - w
                end

                local line_top = y_line + lh * 0.5
                local baseline = nil
                if font.ascent ~= nil then
                    baseline = line_top - font.ascent * max_scale
                end

                for k = 1, #line do
                    local it = line[k]
                    local g = it.g
                    if it.kind == "glyph" and g ~= nil then
                        local gy
                        if baseline ~= nil and g.dy ~= nil then
                            gy = baseline - g.dy * it.scale
                        else
                            gy = line_top
                        end
                        flat[n_out + 1] = cx
                        flat[n_out + 2] = gy
                        flat[n_out + 3] = g.w * it.scale
                        flat[n_out + 4] = g.h * it.scale
                        flat[n_out + 5] = g.u
                        flat[n_out + 6] = g.v
                        flat[n_out + 7] = g.w
                        flat[n_out + 8] = g.h
                        flat[n_out + 9] = it.color
                        n_out = n_out + 9
                    end
                    cx = cx + item_adv(it)
                end

                y_line = y_line - (lh + lead_step)
            end
        end
    end

    if n_out > 0 then
        wag.draw.draw_glyphs(font.image, layer, z, flat)
    end
end

return M
