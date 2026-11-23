-- =============================================================================
--  Luminosa/Lumi/Basic/node.lua
--
--  结点登记表（2026-11-17）。**这是"谁是结点"的唯一真相。**
--
--  【它是什么】一个**纯有向图**的登记表：结点有 `id` 与出边 `edges`，
--    外加三个标记（入口 / 终点 / 显示分组）。**它不认识任何玩法。**
--
--  【它不认识什么 —— 这一条比"它是什么"更重要】
--    · 不认识文字：没有 `text`（文字冒险的内容由它自己的库里放字段）
--    · 不认识选项：没有 `choices`（同上）
--    · 不认识"自动推进还是玩家点的"：**没有 auto**
--        ⇒ 关卡推进、符卡练习、ED 画面，都只是"有边"，起因不归图管
--
--  【为什么要有它】叙事/关卡在文档里被叫作"结点图"，但引擎**不认识结点**
--    （不遍历、不解析、不持图 —— 控制台文档 V3 §12.1）。于是"图"只能来自脚本侧。
--    脚本侧的两个坏办法：
--      ① 让工具**猜形状**（遍历所有表找 `id`）：别的配置表也可能有 `id` ⇒ 误报；
--      ② 让项目**手写一份结点名单**：多一处维护，漏一个就查不到。
--    ⇒ 第三路：**构造即登记**。项目只是把裸表换成 `node{...}`，零额外工作；
--      检查器只读 `registry`，零猜测。
--
--  【三条规矩 —— 违反任何一条，检查器给出的图就是**假的**】
--    ① **基础字段名保留**：`id` / `edges` / `entry` / `terminal` / `kind` / `cluster` / `tags`
--       不得被库挪作他用（比如把 `edges` 当"渲染多边形"）。
--    ② **只有一条边通道**：任何会"跳"的机制都必须写进 `edges`。
--       另造 `next2` / `links` 之类 ⇒ 运行时跳得过去、图上没有这条线 ⇒ 图是假的。
--    ③ **字段必须可枚举**：额外字段是**真实存在的数据**，不许用元表 `__index` 算出来
--       （算出来的字段 `pairs` 看不见，静态图与运行时会分叉）。
--    ⇒ 三条之外**随便加**：检查器只读上面七个名字，其余一律不读。
--
--  【与容器 `group` 的区别】**容器有个 `group`（u8，碰撞分桶，R13）；这里没有。**
--    结点用来分组显示的字段叫 `cluster`（字符串）—— 两个概念、两个名字，
--    免得同一个项目里"group"指两件事。
--
--  用法：
--      local node = require("Lumi.Basic.node").node
--
--      -- 纯图论：只有 id 与出边
--      node{ id = "stage_1@Normal", edges = { "stage_1_boss@Normal" }, kind = "stage", cluster = "Normal" }
--
--      -- 终点：允许没有出边
--      node{ id = "ed_1", kind = "ed", terminal = true }
--
--      -- 入口：练习/符卡/主菜单都是"从图外面进来"的，不是从别的结点走过来
--      node{ id = "practice:stage_1", entry = true, edges = { "stage_1@Normal" } }
--
--      -- 动态指向：edges 的元素是函数 ⇒ 图上只算"动态"，不解析目标
--      node{ id = "gate", edges = { function(state) return "room_locked" end } }
--
--      -- 特化结点（上层库）：额外字段随便加，检查器不读
--      node{
--          id = "spell_07", kind = "spell", edges = {},
--          stg = { hp = 1000, time = 60 },              -- 库自己的子表
--          on_frame = function(self, dt) ... end,        -- 行为字段
--      }
-- =============================================================================

local M = {}

-- -----------------------------------------------------------------------------
--  登记表
-- -----------------------------------------------------------------------------
--
--  【为什么是数组】检查器要报"重名"；map 会把后一个悄悄覆盖。
M.registry = {}

-- 【id → 结点】运行时用（跳转要按 id 找结点）；重名时保留**第一个**。
M.by_id = {}

-- 【重名记录】`{ id = "...", first = <表>, again = <表> }`；正常项目应为空。
M.duplicates = {}

-- 【入口与终点】给检查器与游戏直接用的两个派生列表（登记时顺手维护）。
M.entries = {}
M.terminals = {}

-- 【按表身份去重】同一个表被 require 两次（不同路径解析到同一文件）时不重复登记。
local registered = setmetatable({}, { __mode = "k" })

-- -----------------------------------------------------------------------------
--  统一的报错入口（与 Lumi/Basic/text.lua 同一条）
-- -----------------------------------------------------------------------------
local function fail(where, got, want)
    wag.fail {
        level = wag.log.level.raise,
        where = where,
        got = got,
        want = want,
    }
end

-- -----------------------------------------------------------------------------
--  登记一个结点
-- -----------------------------------------------------------------------------
--
--  @param spec table  必填 `id`（非空字符串）；其余字段见下面的字段表。
--  @return 同一个 spec（便于 `local n = node{...}`）
--
--  字段表（**检查器只读这七个**）：
--      id        string   **必有**，非空；图上的名字（重名由检查器报）
--      edges     string | 数组   出边：元素为字符串（静态）或函数（动态）；省略 = 无出边
--      entry     boolean  可选；true = 从图外部进入的入口（练习/符卡/主菜单）
--      terminal  boolean  可选；true = 到这里结束（允许出度为 0）
--      kind      string   可选；显示类别（"stage" / "spell" / "ed" …），不参与判定
--      cluster   string   可选；显示分组（字符串）。**注意不是容器的 group**
--      tags      数组     可选；自由标签，不参与判定
--
--  其余字段：**随便加**（叙事库的 `text`/`choices`、STG 库的 `stg`/`on_frame` …），
--  检查器不读；但必须守上面三条规矩。
function M.node(spec)
    if type(spec) ~= "table" then
        fail("node 的参数", type(spec), "一个带 id 的表")
    end

    local id = spec.id
    if type(id) ~= "string" or id == "" then
        fail("node 的 id", tostring(id), "非空字符串（结点在图上的名字）")
    end

    -- ---- 基础字段的类型校验（只查"形状"，不查玩法）----
    --
    -- 【为什么 edges 允许字符串】单出边（关卡推进）写成 `edges = "stage_2"` 最省；
    --   多条才用数组。`edges_of` 会把它归一化成数组，登记时不改写 spec。
    local e = spec.edges
    if e ~= nil and type(e) ~= "string" and type(e) ~= "table" then
        fail("node 的 edges", type(e), "字符串（单条出边）或数组；省略 = 没有出边")
    end
    if type(e) == "table" then
        for i = 1, #e do
            local t = type(e[i])
            if t ~= "string" and t ~= "function" then
                fail("node 的第 " .. i .. " 条出边", t,
                     "字符串（目标 id）或函数（动态指向）")
            end
        end
    end
    -- 【cluster 必须是字符串】专门挡"把容器的 group（u8 桶号）抄到结点上"这种错：
    --   那种错不会当场炸，只会在图上显示成一个叫 "3" 的分组。
    if spec.cluster ~= nil and type(spec.cluster) ~= "string" then
        fail("node 的 cluster", type(spec.cluster),
             "字符串（显示分组名）；容器的碰撞分组是另一件事（u8，容器字段 group）")
    end
    if spec.kind ~= nil and type(spec.kind) ~= "string" then
        fail("node 的 kind", type(spec.kind), "字符串（显示类别）")
    end
    if spec.tags ~= nil and type(spec.tags) ~= "table" then
        fail("node 的 tags", type(spec.tags), "数组（自由标签）")
    end

    if registered[spec] then
        return spec          -- 同一个表重复登记：忽略（幂等）
    end
    registered[spec] = true

    M.registry[#M.registry + 1] = spec

    if M.by_id[id] == nil then
        M.by_id[id] = spec
    else
        M.duplicates[#M.duplicates + 1] = {
            id = id, first = M.by_id[id], again = spec,
        }
        -- 【为什么不报错】重名是**静态可查**的错误 ⇒ 交给检查器统一报 error；
        --   运行时在这里抛会让游戏"加载到一半挂掉"，比多一个登记项更难查。
        wag.log.warn("结点 id 重名（检查器会报 error）", id)
    end

    if spec.entry then
        M.entries[#M.entries + 1] = spec
    end
    if spec.terminal then
        M.terminals[#M.terminals + 1] = spec
    end

    return spec
end

-- -----------------------------------------------------------------------------
--  取一个结点的出边（**形状的唯一出处**）
-- -----------------------------------------------------------------------------
--
--  【为什么放在库里】`edges` 是**库的约定**，不是工具的猜想。
--    检查器与游戏都调它 ⇒ 改形状只改这一处（工具与游戏同时跟上）。
--
--  @return 数组，每项 `{ to = "目标 id" | nil, dynamic = true? }`
--          · `to` 有值  ⇒ 静态边
--          · `dynamic`  ⇒ 元素是函数，图上不解析目标（只计数）
--
--  【它不排序、不去重】`edges` 里的顺序就是作者写的顺序；重复边由检查器报。
function M.edges_of(spec)
    local out = {}
    local e = spec and spec.edges
    if e == nil then
        return out
    end
    if type(e) == "string" then
        out[1] = { to = e }
        return out
    end
    for i = 1, #e do
        local v = e[i]
        if type(v) == "string" then
            out[#out + 1] = { to = v }
        elseif type(v) == "function" then
            out[#out + 1] = { dynamic = true }
        end
    end
    return out
end

-- -----------------------------------------------------------------------------
--  三条规矩（检查器按这些读；写在这里是为了"唯一出处"）
-- -----------------------------------------------------------------------------
--
--  ① 基础字段名保留：id / edges / entry / terminal / kind / cluster / tags
--  ② 只有一条边通道：会"跳"的机制一律写进 edges
--  ③ 字段必须可枚举：额外字段是真实数据，不许用 __index 算出来
--
--  ⚠️ 检查器的判定**只看** id / edges / entry / terminal；
--     kind / cluster / tags 只用于**显示**，不参与任何对错判断。

lumi = lumi or {}
lumi.node = M

return M
