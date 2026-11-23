-- =============================================================================
--  Luminosa/Lumi/Basic/shader.lua —— 作者面向的自定义着色器
--
--  【它是什么】引擎侧只有原语（`wag.draw.load_shader` / `set_state{shader=…}`），
--    这里把"加载 + 画一张全屏四边形 + 清状态"包成一次调用。
--
--  【为什么必须成对设置/清除】`shader` 是**持久的绘制状态**（与混合同性质）：
--    设了不清 ⇒ 之后所有绘制都走这个着色器（症状：后面的东西全变成背景）。
--    ⇒ 本文件的每个绘制入口都在 `finally` 语义里清掉（用 pcall 保证异常也清）。
--
--  【坐标语义（已定）】开了 shader ⇒ 顶点按**整帧像素**解释，不做世界变换。
--    于是背景可以铺满整个窗口（含信箱黑边），而"只画在渲染盒内"要靠
--    调用方给出正确的四边形（见 `box_quad`）。
--
--  【片元可见的 uniform】
--      u_time        逻辑秒（暂停冻结）
--      u_resolution  整帧像素尺寸（vec2）
--      u_params      vec4（脚本每次绘制给，见 `draw` 的 params）
--      v_uv          0..1 覆盖这个四边形（不是纹理）
--      v_color       顶点色（本例给白）
-- =============================================================================

local M = {}

-- 层带约定（作者侧纪律，引擎不管）：
--   背景应当显著低于任何游戏对象。用一个很负的常数即可。
M.layer = {
    background = -10000,
    midground = -1000,
    occlusion = 1000,
    ui = 1000000,
}

-- -----------------------------------------------------------------------------
--  加载 / 卸载
-- -----------------------------------------------------------------------------

--- 加载一份片元着色器（顶点着色器由引擎内嵌）。
--- @param name string 逻辑名（卸载用；同名会覆盖）
--- @param path string 相对安装根的路径（§3.2：引擎不解析路径）
--- @return integer 有效句柄；失败即 L3（槽满/读不到/编译失败）。
---   0 只会出现在"未接 SDL"的构建里，正常构建不会用 0 表示失败。
function M.load(name, path)
    return wag.draw.load_shader(name, path)
end

--- 卸载（帧末生效：本帧已收集的绘制项仍能画完）。
function M.unload(name)
    wag.draw.unload_shader(name)
end

-- -----------------------------------------------------------------------------
--  四边形（顶点为**整帧像素**；u/v 也按像素给，着色器里归一化成 0..1）
-- -----------------------------------------------------------------------------

--- 覆盖整帧的四边形。
---
--- 【v 为什么反向】`v_uv` 的惯例是 **v = 1 在顶部、0 在底部**（GL / Shadertoy）；
---   而顶点 y 是「0 在顶部、向下增大」（像素坐标）⇒ 两者方向相反。
---   这里把 v 取成 (h - y)：上边缘给 v=h，下边缘给 v=0 ⇒ 着色器里 v_uv.y=1 在顶。
function M.fullscreen_quad(w, h)
    return {
        { x = 0.0, y = 0.0, z = 0.0, u = 0.0, v = h,   color = 0xFFFFFFFF },
        { x = w,   y = 0.0, z = 0.0, u = w,   v = h,   color = 0xFFFFFFFF },
        { x = w,   y = h,   z = 0.0, u = w,   v = 0.0, color = 0xFFFFFFFF },
        { x = 0.0, y = h,   z = 0.0, u = 0.0, v = 0.0, color = 0xFFFFFFFF },
    }
end

--- 只覆盖一个矩形的四边形（像素）。用于"只画在渲染盒内"这类需求。
function M.box_quad(x, y, w, h)
    return {
        { x = x,     y = y,     z = 0.0, u = 0.0, v = h,   color = 0xFFFFFFFF },
        { x = x + w, y = y,     z = 0.0, u = w,   v = h,   color = 0xFFFFFFFF },
        { x = x + w, y = y + h, z = 0.0, u = w,   v = 0.0, color = 0xFFFFFFFF },
        { x = x,     y = y + h, z = 0.0, u = 0.0, v = 0.0, color = 0xFFFFFFFF },
    }
end

-- -----------------------------------------------------------------------------
--  绘制
-- -----------------------------------------------------------------------------

-- 成对地设状态 → 画 → 清状态。异常也保证清（否则状态会粘住）。
local function draw_with(handle, quad, layer, z, params)
    local ok, err = pcall(function()
        if params ~= nil then
            wag.draw.set_state({ shader = handle, shader_params = params })
        else
            wag.draw.set_state({ shader = handle })
        end
        wag.draw.draw_vertices(0, layer or M.layer.background, z or 0.0,
                               quad[1], quad[2], quad[3], quad[4])
    end)
    wag.draw.set_state({ shader = 0 })          -- 一定清掉
    if not ok then
        error(err, 0)
    end
end

--- 画一张全屏着色器（常用：关卡背景）。
--- @param handle integer    `load` 的返回值
--- @param w,h    number     整帧像素尺寸（背景要铺满窗口就给窗口像素）
--- @param params table|nil  4 个数的数组 → `u_params`（每次绘制）
function M.draw_fullscreen(handle, w, h, params, layer, z)
    draw_with(handle, M.fullscreen_quad(w, h), layer, z, params)
end

--- 画一个矩形区域的着色器。
function M.draw_box(handle, x, y, w, h, params, layer, z)
    draw_with(handle, M.box_quad(x, y, w, h), layer, z, params)
end

-- -----------------------------------------------------------------------------
--  自注册（与 rng.lua 同一约定）
-- -----------------------------------------------------------------------------
lumi = lumi or {}
lumi.shader = M

return M
