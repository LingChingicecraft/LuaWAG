-- =============================================================================
--  Luminosa/WagLib/screen_select.lua
--
--  选择与推送：把"清单里的一项"变成一次给引擎的请求。
--
--  【它与 screen_presets.lua 的分工 —— 这是本模块存在的原因】
--      screen_presets ：**数据**（有哪些项、格式对不对）—— 无状态、无副作用
--      screen_select  ：**选择**（选了哪个、什么时候推给引擎）—— 有状态、有副作用
--      两者的性质不同，所以不合并：混在一起会让"表写错了"与"选择时机错了"
--      被报成同一类失败。
--
--  【选择住在 Lua 侧，引擎只收"两个数" —— 这是已定的】
--      引擎侧的入口是 `wag.space.set_window_size(w, h)`：
--        · 它只收两个数，**不认识"名字"**
--        · 它是**命令式**的：请求一次、申请一次，**不吸附、不纠正、不记住**
--      ⇒ 所以"当前选中的是哪个名字"这个状态**只能住在这一侧**。
--        引擎那边只记得"最后一次申请过什么尺寸"（用于去重），那不是"选择"。
--
--  【为什么本模块不自己去调平台】
--      它只调 `wag.space.set_window_size`。于是"什么时候真的动窗口"
--      与"重复请求要不要动"这两件事都留在引擎一处 —— 天然一致，不会两处打架。
--
--  【生效在帧末 —— 因此 choose() 之后不要立刻期望窗口变了】
--      值在**本帧末尾**被应用（与 set_viewport / set_target_fps 同一处）。
--      下一帧的阶段 1 才会读到新尺寸。这是刻意的：它让"一帧一个窗口尺寸"成立。
--
--  【它不持久化】
--      选了哪个只活在内存里，重启回到默认。存档与配置是**游戏侧**的事 ——
--      本模块不碰文件、不记全局。
--
--  【它不做按显示器缩放的换算】
--      清单里的数是物理像素，直接交给引擎。若某个项目想按 DPI 适配，
--      那属于**游戏侧的二次映射**（它自己的表 → 本模块 → 引擎），
--      而那一层要乘的 dpi 由引擎暴露的事实提供。
-- =============================================================================

local M = {}

local presets = require("WagLib.screen_presets")

-- 当前选中的项名。nil 表示"从未选过"。
--
-- 【为什么用 nil 而不是一个默认名】
--   因为"没选过"与"选了清单里的某一项"是两件事：前者意味着窗口保持引擎
--   启动时的尺寸（引擎不收到任何请求）。若给一个默认名，就等于本模块替作者
--   表了态，而"启动时窗口多大"是 boot 层与项目的事。
local current_name = nil

--- 全部可选的项名（已排序）。
--
-- 【它为什么在这里而不是只放在 presets 里】
--   因为调用方（游戏侧的表）通常只需要"能选什么"，而它不该关心"清单住在
--   哪个模块里"。这里是一次转发。
function M.names()
    return presets.names()
end

--- 取某一项的尺寸。命中时返回 w, h；否则 nil。
function M.size_of(name)
    local e = presets.get(name)
    if e == nil then
        return nil
    end
    return e.w, e.h
end

--- 当前选中的项名；从未选过时返回 nil。
function M.current()
    return current_name
end

--- 选中一项，并**请引擎**把窗口改成它的尺寸。
--
-- @param name  清单里的键
-- @return      true 表示请求已提交（**不代表窗口已经是这个尺寸**）
--
-- 【名字不认识时怎么办 —— 抛错，不静默忽略】
--   因为"名字不认识"是**数据矛盾**（调用方引用了不存在的项），而不是
--   "这个资源暂时不可用"。静默忽略会让它变成"选了没反应"，那是最难查的一类。
--   ⇒ 与引擎侧那条"跨边界处校验、错了就报"一致：这里在脚本侧的边界上。
--   （这不与"单条无效只记 L2"矛盾：那条说的是**清单本身**有问题；
--     本条说的是**调用方**引用了不存在的项，性质不同。）
--
-- 【重复选同一项时仍然转发】
--   因为"引擎要不要动平台"由引擎的去重决定（它比较的是"上一次申请的值"）。
--   在这里再判一次会让同一条规则有两个实现。
function M.choose(name)
    local e = presets.get(name)

    if e == nil then
        -- 【为什么用 wag.fail 而不是全局 error()】
        --   全局 error() 是**脚本自己的**报错，引擎在 pcall 边界无法与"引擎侧的
        --   故障"区分。而这条错误问的是"引擎发的清单里有没有这一项"，属于引擎侧
        --   的事 ⇒ 用 wag.fail 之后两类能分开（也让"错误分级"那一批有个干净的边界）。
        wag.fail{
            where = "lumi.screen.choose 的 name",
            got = tostring(name),
            want = "清单里的一个名字（可用：" .. table.concat(M.names(), " / ") .. "）",
        }
    end

    current_name = name

    -- 【单位：清单里的数就是物理像素，直接交过去】
    --   中间不做任何换算 —— 引擎那条"与画面尺寸有关的路都不经过 dpi"必须
    --   一直连到这一行。
    wag.space.set_window_size(e.w, e.h)

    return true
end

--- 按"清单里的顺序"选下一项（用于快捷键循环切换）。
--
-- @return  切换后的项名；清单为空时返回 nil
--
-- 【为什么提供它】窗口本身可拖拽改尺寸，但还需要一条**不依赖拖拽**的
--   尺寸切换路径（快捷键循环、菜单选项、无边框/演示场合）。
--   循环切换是最短的那条。
function M.next()
    local all = M.names()
    if #all == 0 then
        return nil
    end

    if current_name == nil then
        M.choose(all[1])
        return all[1]
    end

    for i = 1, #all do
        if all[i] == current_name then
            local nxt = all[i % #all + 1]
            M.choose(nxt)
            return nxt
        end
    end

    -- current_name 不在清单里（清单被换过）⇒ 从第一项重新开始。
    M.choose(all[1])
    return all[1]
end

--- 清单自检的结果（转发），便于项目在启动时看一眼有没有写错的项。
--
-- @return ok(布尔), bad(表)
function M.verify()
    return presets.verify()
end

return M
