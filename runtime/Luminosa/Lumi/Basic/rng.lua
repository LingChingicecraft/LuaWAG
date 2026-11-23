-- =============================================================================
--  Luminosa/Lumi/Basic/rng.lua —— 作者面向的随机（九个"糖"）
--
--  【它在这一层的理由】
--    引擎侧只有 5 个**原语**（`wag.rng.*`），因为"状态是 u64、过不了 Lua"。
--    而作者想要的是 `ran:Int(0,360)` 这种写法 —— 那是**组合**，属于增强库。
--    ⇒ 引擎极薄、Lumi 厚，这条分界与本项目其它地方一致。
--
--  【九个糖，各自对应一种"写错了也不报错"的失误】
--      Int(lo,hi)  区间反向 / 取模细节        ⇒ 由引擎原语校验
--      Float()     [0,1) 的边界               ⇒ 引擎保证不会给出 1.0
--      Sign()      ±1 写反                    ⇒ 手写 `Int(0,1)*2-1` 容易糊涂
--      Bool()      50% 却写成 Bool(0.3)       ⇒ **不收参数，传了就 L3**
--      Chance(p)    `Float()<p` 与 `>p` 写反  ⇒ 名字把语义钉死
--      Pick(t)     下标起点 / 空表            ⇒ 一处检查，全项目一致
--      Shuffle(t)  范围不收缩 ⇒ 分布不均      ⇒ 唯一正确写法封装起来
--      Angle()     角度单位（度 vs 弧度）     ⇒ 这里只出**度**
--      Range(a,b)  浮点区间的 `a + r*(b-a)`   ⇒ 写反顺序就静默错
--
--  【错误等级：除 `Shuffle(空表)` 之外一律 L3】
--    与引擎侧一致（已定）。L3 走 `wag.fail{level = terminate}`，
--    它等价于 C++ 侧的 `fail_boundary`（弹窗 + 终止）。
--    `Shuffle(空表)` 返回空表且**不报警** —— 那是真正合法的空操作。
--
--  【消耗次数契约（回放兼容的抓手）】
--      Sign / Bool / Chance / Pick / Angle / Range  = **1 次**
--      Int / Float                                  = **1 次**
--      Shuffle(n 个元素)                            = **n-1 次**
--    ⇒ 若某天改了某个糖的消耗次数，**所有旧录像立刻失效**。
--      所以每个函数上面都写了"消耗"。
--
--  【命名空间】`lumi.rng`（本文件自注册，见文件末尾）。
--    自注册的理由：层自动装载还没落地（`resolve.list_modules()` 目前返回空），
--    现在靠 init.lua 里一行显式 require；等自动装载落地后，
--    加载器未必知道"这个模块该挂到哪个名字下" ⇒ 自注册两条路都通。
-- =============================================================================

local M = {}

-- 槽位：与引擎侧的 kStreamCount = 3 一一对应。
local SLOT = {
    main = 0,
    pause = 1,
    ext = 2,
}

-- -----------------------------------------------------------------------------
--  L3 报错（等价于 C++ 侧的 fail_boundary）
-- -----------------------------------------------------------------------------
local function fail3(where, got, want)
    wag.fail({
        level = wag.log.level.terminate,
        where = where,
        got = got,
        want = want,
    })
end

-- -----------------------------------------------------------------------------
--  流对象
-- -----------------------------------------------------------------------------
local Stream = {}
Stream.__index = Stream

--- 造一条流。`slot` 可以是 "main"/"pause"/"ext" 或 0/1/2。
function M.new(slot)
    local idx = SLOT[slot]
    if idx == nil then
        idx = slot
    end
    if type(idx) ~= "number" then
        fail3("lumi.rng.new 的 slot", tostring(slot), "main / pause / ext（或 0 / 1 / 2）")
    end
    -- 越界由引擎侧报 L3（消息里带 0/1/2 的说明）。
    local handle = wag.rng.new_stream(idx)
    return setmetatable({ h = handle, slot = idx }, Stream)
end

--- **消耗 1 次**。整数 [lo, hi]，**含两端**。
function Stream:Int(lo, hi)
    return wag.rng.int(self.h, lo, hi)
end

--- **消耗 1 次**。[0, 1)，53 位精度；**不会返回 1.0**。
--- 【为什么不收参数】区间请用 `Range(a,b)` —— 同一个需求只留一个入口。
function Stream:Float(...)
    if select("#", ...) > 0 then
        fail3("lumi.rng:Float 的参数", "传入了参数",
              "Float() 不收参数；要区间请用 Range(a, b)")
    end
    return wag.rng.float(self.h)
end

--- **消耗 1 次**。等概率返回 +1 / -1。
--- 【不收参数】`Sign(0.3)` 这种写法没有意义（要三成概率请用 `Chance`）。
function Stream:Sign(...)
    if select("#", ...) > 0 then
        fail3("lumi.rng:Sign 的参数", "传入了参数", "Sign() 不收参数（等概率 ±1）")
    end
    local v = wag.rng.int(self.h, 0, 1)
    return (v == 1) and 1 or -1
end

--- **消耗 1 次**。等概率 true / false（精确 50%）。
--- 【不收参数 —— 已定】`Bool(0.3)` 属于"两个入口做同一件事" ⇒ L3，并指出正确写法。
function Stream:Bool(...)
    if select("#", ...) > 0 then
        fail3("lumi.rng:Bool 的参数", "传入了参数",
              "Bool() 不收参数（50%）；要按概率请用 Chance(p)")
    end
    return wag.rng.float(self.h) < 0.5
end

--- **消耗 1 次**。以概率 `p`（∈ [0,1]）返回 true。
function Stream:Chance(p)
    if type(p) ~= "number" then
        fail3("lumi.rng:Chance 的 p", type(p), "一个 0~1 之间的数")
    end
    if p < 0 or p > 1 then
        fail3("lumi.rng:Chance 的 p", string.format("%g", p),
              "0~1 之间（是不是把 30% 写成了 30？）")
    end
    return wag.rng.float(self.h) < p
end

--- **消耗 1 次**。从数组里等概率取一个。
--- 【空表 ⇒ L3】"从空筐里摸一个"没有合法返回值。
function Stream:Pick(t)
    if type(t) ~= "table" then
        fail3("lumi.rng:Pick 的 t", type(t), "一个非空数组")
    end
    local n = #t
    if n < 1 then
        fail3("lumi.rng:Pick 的 t", "空表", "一个非空数组")
    end
    return t[wag.rng.int(self.h, 1, n)]
end

--- **消耗 n-1 次**（n = 元素个数）。原地等概率打乱，返回同一个表。
---
--- 【为什么从后往前 + 范围收缩】只有这样所有 n! 种排列才等概率。
---   写成 `j = Int(1, n)`（固定范围）或从前往后 ⇒ 分布不均，而且**不报错**。
--- 【空表 ⇒ 原样返回，不报警】那是真正合法的空操作（已定）。
function Stream:Shuffle(t)
    if type(t) ~= "table" then
        fail3("lumi.rng:Shuffle 的 t", type(t), "一个数组")
    end
    for i = #t, 2, -1 do
        local j = wag.rng.int(self.h, 1, i)
        t[i], t[j] = t[j], t[i]
    end
    return t
end

--- **消耗 1 次**。随机角度，`[0, 360)`，**单位是"度"**。
--- 【为什么是度】aex/lstg 的角度都是度；弧度不便调参（已定：脚本侧一律用度）。
function Stream:Angle(...)
    if select("#", ...) > 0 then
        fail3("lumi.rng:Angle 的参数", "传入了参数", "Angle() 不收参数（返回 [0,360) 度）")
    end
    return wag.rng.float(self.h) * 360.0
end

--- **消耗 1 次**。浮点区间 `[a, b)`。
--- 【a > b ⇒ L3】不自动交换（已定：引擎不替开发者做决定）。
function Stream:Range(a, b)
    if type(a) ~= "number" or type(b) ~= "number" then
        fail3("lumi.rng:Range 的区间", tostring(a) .. "," .. tostring(b),
              "两个数 a, b（a <= b）")
    end
    if a > b then
        fail3("lumi.rng:Range 的区间", string.format("a=%g b=%g", a, b), "a <= b")
    end
    return a + wag.rng.float(self.h) * (b - a)
end

--- 状态快照（存档用）。不消耗。
function Stream:State()
    return wag.rng.state(self.h)
end

--- 恢复状态（读档用）。不消耗。
function Stream:SetState(lo, hi)
    return wag.rng.set_state(self.h, lo, hi)
end

--- 主种子（lo, hi）。不消耗；同一进程内永远一样。
function M.master_seed()
    return wag.rng.master_seed()
end

--- 把三条流一次造出来：`local r = lumi.rng.all()` ⇒ r.main / r.pause / r.ext
function M.all()
    return {
        main = M.new("main"),
        pause = M.new("pause"),
        ext = M.new("ext"),
    }
end

-- -----------------------------------------------------------------------------
--  自注册（见文件头"命名空间"那一段）
-- -----------------------------------------------------------------------------
lumi = lumi or {}
lumi.rng = M

return M
