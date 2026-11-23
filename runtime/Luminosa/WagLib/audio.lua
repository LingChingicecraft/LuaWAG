-- =============================================================================
--  Luminosa/WagLib/audio.lua
--
--  音频的**库层糖**：名字 ⇄ 句柄，音乐与音效两组。
--
--  【它解决什么】引擎原语 `wag.audio.*` 只认**句柄**与 **voice_id**（《LuaWAG
--    音频设计文档》D6/D11/D12）。作者不该每处都写一遍"拼路径 + load + 记句柄"。
--    这一层把它压成 `play_music("spellcard")` / `play_se("cancel")`。
--
--  【它不解决什么】
--    · 不做**播放策略**（要不要循环、音量多少、暂停时是否静音）—— 那是游戏的事
--    · 不做**流式/预读**（引擎第一版整段解码）
--    · 不猜格式：只认 .ogg / .wav（与引擎一致）
--
--  【资源目录】默认 `<Luminosa>/Project/audio/`（与图片"落在项目层"同一条规则）。
--    可用 set_dir 覆盖；相对路径按**安装根**解析（与引擎"路径在脚本侧拼好"一致）。
-- =============================================================================

local resolve = require("WagLib.resolve")

local M = {}

--- 引擎第一版只接受 44100 Hz（文档 §四）；秒 ⇄ 帧的换算就靠它。
M.SAMPLE_RATE = 44100

local handles = {}          -- name -> 句柄
local dir_override = nil    -- nil = 用默认目录

-- -----------------------------------------------------------------------------
--  目录
-- -----------------------------------------------------------------------------

--- 设定音频目录。
-- @param dir  绝对路径，或相对**安装根**的路径；nil 恢复默认
function M.set_dir(dir)
    -- 先真正卸掉旧目录下已缓存的资源，再换目录；
    -- 只清 Lua 表会把名字/采样留在引擎里（见 API 文档 §1.6.2）。
    for name in pairs(handles) do
        wag.audio.unload(name)
    end
    handles = {}
    dir_override = dir
end

local function base_dir()
    if dir_override == nil then
        return resolve.luminosa_dir() .. "/Project/audio"
    end
    -- 绝对路径：`/…`（POSIX）或 `C:/…` / `C:\…`（Windows）
    if dir_override:sub(1, 1) == "/"
        or dir_override:sub(1, 1) == "\\"
        or dir_override:match("^%a:[/\\]") then
        return dir_override
    end
    return resolve.install_root() .. "/" .. dir_override
end

-- -----------------------------------------------------------------------------
--  名字 ⇄ 句柄
-- -----------------------------------------------------------------------------

local function has_extension(name)
    return name:match("%.ogg$") ~= nil or name:match("%.wav$") ~= nil
end

--- 取句柄（第一次用到时加载）。
--
-- 【为什么用 pcall 试候选而不是先问"文件在不在"】
--   引擎不给"文件存在"的查询，而 `io` 在各层脚本里不是必须可用的。
--   试两个候选（.ogg 再 .wav）的成本只有一次失败的加载尝试，换来零依赖。
local function handle_of(name)
    local h = handles[name]
    if h ~= nil then
        return h
    end

    local base = base_dir()
    local candidates
    if has_extension(name) then
        candidates = { base .. "/" .. name }
    else
        candidates = { base .. "/" .. name .. ".ogg", base .. "/" .. name .. ".wav" }
    end

    local last_error = nil
    for _, path in ipairs(candidates) do
        local ok, result = pcall(wag.audio.load, name, path)
        if ok then
            handles[name] = result
            return result
        end
        last_error = result
    end

    wag.fail{
        level = wag.log.level.raise,
        where = "WagLib.audio 的名字",
        got = tostring(name),
        want = "一个能找到的音频名（目录：" .. base .. "；试过：" ..
               table.concat(candidates, "、") .. "）",
        sentence = last_error and tostring(last_error) or "音频加载失败",
    }
end

--- 预加载（可选）。返回句柄。
function M.load(name)
    return handle_of(name)
end

--- 卸载一个已由本模块加载的音频（幂等）。
--
-- 【不碰 voice】正在播的采样由引擎按 refs 保留到播完；
--   要立刻静音，作者应显式 stop_music / stop_all（API 文档 §1.6.2）。
function M.unload(name)
    if type(name) ~= "string" then
        return
    end
    if handles[name] == nil then
        return
    end
    handles[name] = nil
    wag.audio.unload(name)
end

-- -----------------------------------------------------------------------------
--  音乐（BGM 槽）
-- -----------------------------------------------------------------------------

--- 播放音乐（默认**循环**，因为 BGM 基本都是循环的）。
-- @param name  音频名（可带 .ogg / .wav）
-- @param opts  { volume=, pan=, loop=, loop_start=, loop_end=, start_frame= }（loop 默认 true）
--   【pan = 平衡】∈ [-1,1]：-1 纯左 / 0 居中（−3 dB）/ +1 纯右；不折叠立体声。
--   播放中改它没有斜坡 ⇒ 别逐帧扫（《LuaWAG 音频设计文档》§4.2）。
--   【start_frame = 起点】采样帧；它不是 seek（引擎不做中途跳转，见文档 §六）。
-- @return voice_id
function M.play_music(name, opts)
    opts = opts or {}
    local h = handle_of(name)
    -- 【记住这份参数】seek_music 要重播同一首，音量/平衡/循环点都得原样带过去
    --   （新 voice 不会继承旧 voice 的任何参数）。
    M.music_state = {
        volume = opts.volume or 1.0,
        pan = opts.pan or 0.0,
        loop = (opts.loop == nil) and true or opts.loop,
        loop_start = opts.loop_start,
        loop_end = opts.loop_end,
    }
    local id = wag.audio.play(h, {
        volume = M.music_state.volume,
        pan = M.music_state.pan,
        loop = M.music_state.loop,
        music = true,
        -- 循环区间（采样帧）：首次从 start_frame 播到 loop_end，之后在 [loop_start, loop_end) 循环
        loop_start = M.music_state.loop_start,
        loop_end = M.music_state.loop_end,
        start_frame = opts.start_frame,
    })
    M.music_voice = id
    M.music_handle = h
    M.music_name = name
    return id
end

function M.stop_music()
    if M.music_voice then
        wag.audio.stop(M.music_voice)
        M.music_voice = nil
        M.music_handle = nil
        M.music_name = nil
    end
end

--- **暂停**音乐（位置保留，可 `resume_music` 继续）—— 暂停菜单用。
function M.pause_music()
    if M.music_voice then
        wag.audio.pause(M.music_voice)
    end
end

--- **继续**音乐（从暂停处接着播）。
function M.resume_music()
    if M.music_voice then
        wag.audio.resume(M.music_voice)
    end
end

--- 音乐是否正在播（**暂停不算**）。
function M.music_active()
    return M.music_voice ~= nil and wag.audio.is_active(M.music_voice)
        and not wag.audio.is_paused(M.music_voice)
end

--- 音乐是否暂停。
function M.music_paused()
    return M.music_voice ~= nil and wag.audio.is_paused(M.music_voice)
end

--- 跳到某个时刻继续播（单位：**秒**）。
--
-- 【它怎么实现】引擎**不做 seek**（文档 §六）：这里是"**停掉旧 voice + 用
--   start_frame 重播**"，BGM 槽直接替换。代价：切换处有一次约 38 ms 的空隙。
--
-- 【为什么它仍然值得存在】它替作者记住"音量/平衡/循环点"，并处理 voice_id 变化；
--   没有它，每个作者都要自己踩这些坑。
--
-- 【不支持什么】**拖拽进度条时连续出声** —— 那需要引擎侧改 pos，是有意不做的。
function M.seek_music(seconds)
    if not M.music_name then
        return nil
    end
    local state = M.music_state or {}
    local frame = math.floor(math.max(0.0, seconds) * M.SAMPLE_RATE)
    return M.play_music(M.music_name, {
        volume = state.volume,
        pan = state.pan,
        loop = state.loop,
        loop_start = state.loop_start,
        loop_end = state.loop_end,
        start_frame = frame,
    })
end

-- -----------------------------------------------------------------------------
--  音效（SE 池）
-- -----------------------------------------------------------------------------

--- 播放音效（默认**不循环**）。
-- @param name  音频名
-- @param opts  { volume=, pan=, loop=, loop_start=, loop_end= }（pan = 平衡，语义同 play_music）
-- @return voice_id（池满时引擎会抢最早的，见设计文档 §七）
function M.play_se(name, opts)
    opts = opts or {}
    local h = handle_of(name)
    return wag.audio.play(h, {
        volume = opts.volume or 1.0,
        pan = opts.pan or 0.0,
        loop = opts.loop or false,
        music = false,
        loop_start = opts.loop_start,
        loop_end = opts.loop_end,
    })
end

-- -----------------------------------------------------------------------------
--  音量（转发；三组线性 0..1）
-- -----------------------------------------------------------------------------

M.set_master_volume = wag.audio.set_master_volume
M.set_music_volume = wag.audio.set_music_volume
M.set_se_volume = wag.audio.set_se_volume
M.get_master_volume = wag.audio.get_master_volume
M.get_music_volume = wag.audio.get_music_volume
M.get_se_volume = wag.audio.get_se_volume

M.stop_all = wag.audio.stop_all

-- H17 / §9.3：热重载后新 Lua 状态自动接回仍在播的 BGM。
--   只在本模块加载时执行一次；没有 BGM ⇒ 保持 nil。
--   不恢复播放、不重建 voice、不知道旧曲名/旧参数（v1 限制）。
do
    local voice, handle = wag.audio.current_music()
    if voice ~= nil then
        M.music_voice = voice
        M.music_handle = handle
        M.music_name = nil
        M.music_state = nil
    end
end

return M
