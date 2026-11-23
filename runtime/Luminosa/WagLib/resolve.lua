-- =============================================================================
--  Luminosa/WagLib/resolve.lua
--
--  目录解析：找到安装根与 Luminosa，把模块名映射到路径。
--
--  【它为什么与 load.lua 分开 —— 这是已定的】
--      resolve 是**纯路径解析**：易单测、无副作用。
--      load    会**执行代码**：有副作用、会失败。
--      两者的性质不同，混在一起会让错误归因变难 —— 一个"目录找不到"与一个
--      "模块里报错"会被报成同一类失败。
-- =============================================================================

local M = {}

-- =============================================================================
--  一、安装根与 Luminosa
-- =============================================================================
--
-- 【已定的四条规则】
--   ① 安装根：**先按"exe 所在目录"**，并允许被命令行覆盖
--   ② Luminosa：**固定路径** <安装根>/Luminosa/
--   ③ 模块名 → 路径：**扫描目录 + 按相对路径推模块名**（不用清单文件）
--   ④ 缺失某一层（Lumi / Nosa / Project）⇒ **引擎仍要能起**，记 L1 并继续
--
-- 【为什么 ③ 是"扫描 + 推"而不是"清单文件"】
--   清单文件多一个需要维护的东西，而它解决的问题（顺序与开关）目前还不存在。
--   扫描 + 按相对路径推名的映射是：Lumi/foo/bar.lua ⇒ 模块名 foo.bar。
--
--   【"显式声明"那条路留作将来】若 Nosa / Project 将来需要顺序或开关，
--   再加清单。而那时是**增加**一种能力，不是替换现有规则。
-- =============================================================================

-- -----------------------------------------------------------------------------
--  层的定义
--
-- 【已定：四层，顺序固定】
--   ① wag      = 引擎本身 + WagLib（本目录）
--   ② Lumi     = **官方库**（官方通用基础库 + 官方特定游戏类型的增强库）
--   ③ Nosa     = **第三方库**（其他作者与游戏作者自己的库）
--   ④ Project  = 游戏本身
--
-- 【依赖规则】
--   每一层可以使用它**下面**的各层以及它自己。
--   ② 不得使用 ③ 或 ④。
--   ③ 不得使用 ④。
--   ① 不得依赖任何脚本侧内容。
--
-- 【模块名带层前缀】
--   模块名形如 Lumi.Basic.rng / Project.main；不同层不会因同名互相覆盖。
--   需要替换某个模块时，请显式 require 或替换文件，不要依赖“后装者胜”。
-- -----------------------------------------------------------------------------
M.layers = {
    { name = "Lumi",    dir = "Lumi"    },
    { name = "Nosa",    dir = "Nosa"    },
    { name = "Project", dir = "Project" },
}

--- 返回安装根。
--
-- @return 安装根的路径（不带末尾斜杠）
--
-- 【已定】默认是 exe 所在目录，可被命令行覆盖（`--root`）。
--
-- 【2026-10-18 落地：走候选 (甲)】引擎在启动时把**只读**的安装根发布成
--   `wag.install_root`（见 `main.cpp` 的 `publish_install_root` 调用点与
--   `script/state.cpp` 的实现）。于是本函数只是**读一个字符串**。
--
--   为什么不选 (乙)（从 package.path / debug 推）：那依赖装载方式，不可靠 ——
--   而"当前工作目录与 exe 所在目录可以不同"，从别处启动游戏时按工作目录找
--   Luminosa 会失败。
--
-- 【路径拼接在 Lua 侧】已定："引擎不解析路径" ⇒ 相对安装根的拼接都在脚本侧。
function M.install_root()
    local root = wag and wag.install_root
    if type(root) ~= "string" or root == "" then
        -- 这不是"可以降级"的情形：没有安装根 ⇒ 所有资源路径都无法拼。
        -- 报错而不是返回 "."（返回 "." 会把"猜"伪装成"知道"）。
        error("wag.install_root 未发布 —— 引擎启动序列没有把安装根交给脚本",
              2)
    end
    return root
end

--- 返回 Luminosa 目录。
--
-- 【已定】固定路径 <安装根>/Luminosa/。
function M.luminosa_dir()
    return M.install_root() .. "/Luminosa"
end

--- 返回某一层的目录路径。
--
-- @param layer  M.layers 里的一项
function M.layer_dir(layer)
    return M.luminosa_dir() .. "/" .. layer.dir
end

-- =============================================================================
--  二、模块名 → 路径
-- =============================================================================

--- 把一个模块名转成相对 Luminosa 的路径。
--
-- @param module_name  模块名，以点分隔（例：foo.bar）
-- @return             相对路径（例：foo/bar.lua）
--
-- 【已定：按相对路径推模块名】于是这里是那条规则的逆运算。
--
-- 【为什么它也在这里而不在 load.lua】
--   因为它是"名字 → 路径"的规则，而那是解析的一部分。load 只管"按什么次序
--   装载"，不管"装载谁"是怎么算出来的。
function M.module_to_relative_path(module_name)
    -- 把点换成斜杠。注意 LuaJIT 的目标平台里也包含 Windows，
    -- 而 Lua 的 require 在 Windows 上也接受正斜杠。
    return (module_name:gsub("%.", "/")) .. ".lua"
end

--- 列举某一层里可被装载的模块。
--
-- @param layer  M.layers 里的一项
-- @return 模块名数组；目录不存在时返回空数组
--
-- 【已定：按目录扫描发现文件】
--   不需要清单文件。而"目录不存在"必须返回空数组而不是报错 ——
--   因为缺失某一层时引擎仍要能起（规则 ④）。
--
-- 【它为什么返回"模块名"而不是"文件路径"】
--   因为装载（load.lua）是按模块名做的：它要让 Lua 的 require 机制参与,
--   于是模块名才是那个正确的钥匙。
--
-- 【实现】2026-11-19 起走引擎原语 wag.fs.list_files：
--   它返回相对路径（统一 `/`、字典序），不跟随符号链接。
--   模块名采用**层前缀**：Lumi/Basic/rng.lua → Lumi.Basic.rng。
--   这样 package.path 只需指向 <安装根>/Luminosa，且不同层不会互相覆盖。
function M.list_modules(layer)
    local dir = M.layer_dir(layer)
    local files = {}

    if type(wag) ~= "table" or type(wag.fs) ~= "table"
        or type(wag.fs.list_files) ~= "function" then
        wag.log.warn("wag.fs.list_files 不存在，层扫描跳过",
                     string.format("层=%s", tostring(layer.name)))
        return {}
    end

    local ok, listed = pcall(wag.fs.list_files, dir, true)
    if not ok then
        wag.log.warn("层目录扫描失败，按空层处理",
                     string.format("层=%s 路径=%s 原因=%s",
                                   tostring(layer.name), dir, tostring(listed)))
        return {}
    end
    files = listed

    local out = {}
    for i = 1, #files do
        local rel = files[i]
        if rel:sub(-4) == ".lua" then
            local no_ext = rel:sub(1, -5):gsub("/", ".")
            out[#out + 1] = layer.name .. "." .. no_ext
        end
    end

    table.sort(out)
    return out
end

return M
