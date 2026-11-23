-- =============================================================================
--  Luminosa/WagLib/vocab.lua
--
--  词汇映射层：把"用户词汇"装配到引擎的槽与引擎原语上。
--
--  【它解决什么问题】
--      引擎只认 13 个协议名（on_game_init / MainFrameFunc 之类的东西它不知道）。
--      而作者写的是人读得懂的名字。
--      这一层负责把后者接到前者上。
--
--  【它为什么住在 WagLib 里 —— 这是一条"特赦"，要说清】
--      按"删掉它引擎是否还能启动"这条判据，它**不该**在这里：
--      引擎照跑（wag.* 原语都在），只是所有用用户词汇写的脚本会炸。
--
--      而它命中另一条判据：**必须随引擎版本发布**。
--      理由：映射表把"用户词汇"对应到"引擎槽与引擎原语"，而引擎侧的槽名与
--      原语名**会随引擎版本变**。若它住在内容侧，就会出现"映射表版本"与
--      "引擎版本"两个版本 —— 而它们的对应关系没人管，错位就是一堆看不懂的
--      报错。
--
--      ⇒ 所以 WagLib 的成员有两类：
--          甲 = 引擎必需（删了引擎起不来 / 跑不下去 / 规格不成立）
--          乙 = 必须随引擎版本发布（本文件与它的映射表）
--
--  【它在一个已经不再被需要的世界里还存在吗】
--      存在，而且它是"照抄"的落点：新项目可以直接用官方词汇写，而
--      LuaSTG 迁移项目用映射把旧名字接上。
--      两条路共用同一套机制。
-- =============================================================================

local M = {}

--- 用户词汇表。
--
-- 【它是一张数据表，而不是一段代码】
--   理由有二：
--     ① 数据便于比对与生成（将来若有工具检查"映射表是否与引擎版本匹配"，
--        它读的是一张表而不是一段代码）
--     ② "装配"这个动作本身很短 —— 它就是把表里的条目写进 wag.hook 与
--        引擎原语的别名上。让它短，是为了让"表里有什么"成为唯一需要读的东西。
--
-- 【每一条的两端】
--   左边 = 作者写的名字（用户词汇）
--   右边 = 引擎侧的槽名（即 wag.hook 上的协议名）
--
-- 【为什么这里出现了协议名 —— 那不是"两份清单"吗】
--   不是。协议名的**权威**仍然是 C++ 侧那张表（script/hook.cpp 的
--   kHookNames）。本表是"用户词汇 → 协议名"的**映射**，它必然要提到协议名 ——
--   就像任何一个翻译表必然提到两种语言。
--   而 hooks.lua 的自检**不重列**协议名，它只查形状。两者职责不同。
M.hooks = {
    -- 生命周期
    GameInit             = "on_game_init",
    GameExit             = "on_game_exit",

    -- 逻辑相位
    ExtFrameFunc         = "on_frame_ext",
    MainFrameFunc        = "on_frame_main",
    PauseFrameFunc       = "on_frame_pause",

    -- 渲染相位
    OnRenderBegin        = "on_render_begin",
    MainRenderFunc       = "on_render_main",
    PauseRenderFunc      = "on_render_pause",
    ExtRenderFunc        = "on_render_ext",
    OnRenderEnd          = "on_render_end",

    -- 事件
    EventFunc            = "on_event",

    -- 焦点：**两个用户词映到两个引擎槽**
    --
    -- 【注意：合并发生在"使用侧"，不在这里】
    --   引擎侧是两个槽（gain / lose），这是为了让钩子的计数保持 13。
    --   若某个项目希望"只写一处分流"，它可以在自己的代码里写一个函数,
    --   同时赋给这两个用户词 —— 那是它的选择，不是引擎强加的形态。
    FocusGainFunc        = "on_focus_gain",
    FocusLoseFunc        = "on_focus_lose",
}

--- 安装词汇映射层。
--
-- 【调用点】init.lua 的步骤 4，**在 boot.lua 之后**。
-- 【为什么必须在 boot 之后】
--   因为 boot（引导期配置，不是 on_game_init）运行在用户词汇装载之前 —— 它只能用引擎原语。
--   若顺序反了，那条约束就不成立，而"boot 里只能用原语"会变成一个过时的
--   说法而不是一条规则。
--
-- 【2026-10-15 收口的两条决策（原先都标着"不确定代码 · 待定"）】
--   决策一：**映射名放全局**（不放某张表下）。
--     理由：LuaSTG 那一批名字（MainFrameFunc / RenderTexture …）本来就是
--     全局的；放表下 = 迁移项目要改每一个调用点，而"让迁移项目不用改调用点"
--     恰恰是映射层存在的意义。全局命名空间的"污染"由前缀纪律兜住：
--     引擎只占 wag.，用户词汇由映射表收口、可被全文列出。
--   决策二：**装配方式 = 给 _G 装元表（__index/__newindex 双向映射）**。
--     即原先 wag.hook 上那个"候选 (甲)"推广到全局表。理由：
--       ① "裸全局赋值生效"不靠任何同步动作 —— 没有同步，就没有"忘了同步"；
--       ② 引擎按协议名读 wag.hook（物理键），元表只在"键不存在"时介入，
--         所以引擎侧、require、标准库全都不受影响；
--       ③ 热路径不受影响：性能敏感代码按 Lua 惯例用 local（而
--         "访问全局才走元表"本来就有一次哈希查找的成本，二者同量级）。
function M.install()
    -- -------------------------------------------------------------------------
    --  第一步：把用户词汇接到引擎槽上（wag.hook 的别名槽，保持原有行为）
    -- -------------------------------------------------------------------------
    --
    -- 【做法：给 wag.hook 增加"别名槽"】
    --   wag.hook 上现在有两类键：
    --     · 13 个协议名（引擎按名调用它们）
    --     · 一批用户词汇（作者按名写它们）
    --   两者是**同一个表上的两组键**，而它们指向同一批函数体。
    --
    --   为什么保留这一步（_G 元表已经能接住裸全局赋值）：
    --     因为 `wag.hook.MainFrameFunc = f` 这条**显式形式**也要继续成立 ——
    --     它是"我在给引擎槽挂函数"的自文档写法。两条路通向**同一个协议槽**，
    --     没有"两个真相"。
    local aliases = M.hooks

    local hook_table = wag.hook
    if type(hook_table) ~= "table" then
        -- 这属于引导层基础设施没到位，而不是用户写错了。
        error("[引擎内部] wag.hook 不存在：映射层无法装配。")
    end

    setmetatable(hook_table, {
        __newindex = function(t, key, value)
            local protocol = aliases[key]
            if protocol ~= nil then
                -- 【写用户词汇 ⇒ 同时写协议槽】
                --   用 rawset 避免再次触发 __newindex（那会无限递归）。
                rawset(t, protocol, value)
                rawset(t, key, value)
            else
                rawset(t, key, value)
            end
        end,

        __index = function(t, key)
            local protocol = aliases[key]
            if protocol ~= nil then
                return rawget(t, protocol)
            end
            return nil
        end,
    })


    -- -------------------------------------------------------------------------
    --  第二步：给 _G 装元表 —— 裸全局赋值从此生效（决策二）
    -- -------------------------------------------------------------------------
    --
    -- 【语义逐条】
    --   · 对**钩子词汇**赋值（如 `MainFrameFunc = f`）
    --       ⇒ rawset 进 wag.hook 的协议槽（"on_frame_main"），**不**落 _G。
    --         协议槽是唯一真相；全局名只是它的另一个读法（见 __index）。
    --         赋 nil 同理 = 清掉那个槽。
    --   · 对**原语词汇**赋值（如 `SetViewPort = ...`）
    --       ⇒ 照常落 _G（覆盖 / shadow）。读取从此拿不到映射函数，
    --         直到把它置回 nil。这是有意的：显式覆盖 = 作者的意图，不拦。
    --   · 对**任何其他名字**读写 ⇒ 与没有元表时完全一样。
    --
    -- 【一个必须知道的 Lua 细节】
    --   __newindex / __index 只在"键在表里物理不存在"时被调用。
    --   于是同一个名字的第二次赋值**仍然**会走 __newindex（因为我们从不把
    --   钩子词汇物理写进 _G），行为是幂等的；而所有物理存在的键
    --   （wag、print、require …）完全不经过元表。
    local hook_aliases = aliases

    -- 原语词汇在 install 时**解析成函数**（而不是每次读取都按路径走一遍）。
    -- 【为什么预算】原语在启动序列之后不再变，"读一次表"就够了；
    --   每次 __index 都拆路径是给热路径加一段纯浪费的字符串处理。
    local primitive_funcs = {}
    for name, path in pairs(M.primitives) do
        local node = _G
        for seg in string.gmatch(path, "[%w_]+") do
            node = node[seg]
            if node == nil then
                break
            end
        end
        -- 解析不到不报错：记一条 L2（映射表指向了不存在的原语 —— 这属于
        -- "必须随引擎版本发布"的那张表与引擎错位了，正是要暴露的事）。
        if node == nil then
            wag.log.warn("映射表指向的原语不存在",
                         string.format("%s = %s", name, path))
        else
            primitive_funcs[name] = node
        end
    end

    setmetatable(_G, {
        __newindex = function(t, key, value)
            if hook_aliases[key] ~= nil then
                -- 钩子词汇：落到协议槽（唯一真相），不落 _G。
                rawset(hook_table, hook_aliases[key], value)
                return
            end
            -- 其余（含对原语词汇的覆盖）：照常落表。
            rawset(t, key, value)
        end,

        __index = function(t, key)
            local protocol = hook_aliases[key]
            if protocol ~= nil then
                -- 返回协议槽当前值 —— 没挂就是 nil（真实的"还没挂"）。
                return rawget(hook_table, protocol)
            end
            local fn = primitive_funcs[key]
            if fn ~= nil then
                return fn
            end
            return rawget(t, key) -- 即 nil：与没有元表时一致
        end,
    })
end

--- 引擎原语的用户词汇别名。
--
-- 【与 M.hooks 的区别】
--   M.hooks 映射的是**钩子**（引擎调用脚本的位置）。
--   M.primitives 映射的是**引擎原语**（脚本调用引擎的位置）。
--   两者的方向相反，因此它们的装配方式也不同：
--     · 钩子：赋值转发（写 wag.hook 的协议槽）
--     · 原语：读取转发（读 _G 时拿到原语函数；见 install 的第二步）
--
-- 【2026-10-15 已决】映射名放全局（见 install 文件头那条决策一）。
--   ⚠️ 而"读取转发"有一个必须写明的边界：对原语词汇**赋值** = 覆盖
--     （shadow），此后读取拿到的就是赋的值，直到置回 nil。
--
-- 【2026-11-17 更新】**渲染原语的三个映射名已定**（P8c）。
--
--   纪律（见备忘 P8c）：**只做"名字 → 同一个函数对象"，零包装**。
--   一旦套 `function(...) return f(...) end` 做参数适配，就变成"第二个实现"。
--   ⇒ 所以 `Render4V` 直接指向 `wag.draw.draw_quad`（引擎原语本身），
--     而不是一层"帮忙算 4 个角 UV"的胶水。
--
--   ⚠️ 名字碰巧与源系统一致是**红利**，不是义务：参数以 LuaWAG 为准。
--      · `Render`   → 7 个参数（+ 可选取图矩形 4 个），不是源系统的 8+ 个
--      · `RenderTexture` → 顶点是**表**（{x,y,z,u,v,color}），不是扁平数字
--      · `Render4V` → 12 个数，与源系统同形（逐顶点 z 被接受但不参与渲染）
M.primitives = {
    -- 用户词汇名 → 引擎原语（以点号分隔的路径）
    SetViewPort   = "wag.space.set_viewport",
    Render        = "wag.draw.draw_image",
    RenderTexture = "wag.draw.draw_vertices",
    Render4V      = "wag.draw.draw_quad",
}

return M
