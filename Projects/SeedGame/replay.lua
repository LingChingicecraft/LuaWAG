-- =============================================================================
--  Project/seedgame/replay.lua
--
--  单槽 rep：只保存最后一次。
--  记录的是每个逻辑帧的三个输入边沿/状态：left / right / jump。
--  不记录暂停（暂停期间不推进逻辑），不记录渲染帧率。
-- =============================================================================

local resolve = require("WagLib.resolve")

local GRAINS_PER_SECOND = 60

local R = {
    frames = {},
    recording = false,
    playing = false,
    index = 1,
    saved_path = nil,
    elapsed_grains = 0,
    total_grains = 0,
}

-- 单槽 rep 的文件路径。
local function path()
    -- 用户规定：rep 文件放在 exe 同目录；后缀随意。
    local root = wag.install_root or "."
    return root .. "/seedgame.rep"
end

-- 清空内存中的录像帧。
local function clear_frames()
    R.frames = {}
end

-- 开始记录一局。
function R.begin_recording()
    clear_frames()
    R.recording = true
    R.playing = false
    R.index = 1
    R.elapsed_grains = 0
    R.total_grains = 0
end

-- 记录一帧输入与时间颗粒。
function R.record(controls, dt)
    if not R.recording then
        return
    end
    local c = controls or {}
    R.frames[#R.frames + 1] = {
        left = c.left and true or false,
        right = c.right and true or false,
        jump = c.jump and true or false,
    }
    local step = tonumber(dt) or (1.0 / GRAINS_PER_SECOND)
    R.elapsed_grains = R.elapsed_grains + step * GRAINS_PER_SECOND
end

-- 把内存录像写成单槽 rep 文件。
function R.save()
    if not R.recording then
        return false, "not recording"
    end
    local p = path()
    local f, err = io.open(p, "wb")
    if f == nil then
        return false, tostring(err)
    end
    local grains = math.floor(R.elapsed_grains + 0.5)
    R.total_grains = grains
    f:write("seedgame-rep 2\n")
    f:write(tostring(#R.frames), "\n")
    f:write(tostring(grains), "\n")
    for i = 1, #R.frames do
        local c = R.frames[i]
        f:write(c.left and "1" or "0", " ",
                c.right and "1" or "0", " ",
                c.jump and "1" or "0", "\n")
    end
    f:close()
    R.saved_path = p
    wag.log.info("seedgame", string.format("rep saved frames=%d", #R.frames))
    return true
end

-- 读取单槽 rep 文件。
function R.load()
    local p = path()
    local f = io.open(p, "rb")
    if f == nil then
        return false, "no replay"
    end

    local magic = f:read("*l")
    local count = tonumber(f:read("*l"))
    if count == nil or count < 0 or
       (magic ~= "seedgame-rep 1" and magic ~= "seedgame-rep 2") then
        f:close()
        return false, "bad header"
    end

    local grains = count
    if magic == "seedgame-rep 2" then
        grains = tonumber(f:read("*l"))
        if grains == nil or grains < 0 then
            f:close()
            return false, "bad grains"
        end
    end

    local frames = {}
    for i = 1, count do
        local line = f:read("*l")
        if line == nil then
            f:close()
            return false, "truncated"
        end
        local a, b, c = line:match("^([01])%s+([01])%s+([01])%s*$")
        if a == nil then
            f:close()
            return false, "bad frame " .. tostring(i)
        end
        frames[i] = { left = a == "1", right = b == "1", jump = c == "1" }
    end
    f:close()

    R.frames = frames
    R.total_grains = grains
    R.elapsed_grains = 0
    R.saved_path = p
    wag.log.info("seedgame", string.format("rep loaded frames=%d grains=%d",
        #frames, grains))
    return true
end

-- 开始回放已加载的录像。
function R.begin_playback()
    if #R.frames == 0 then
        return false
    end
    R.recording = false
    R.playing = true
    R.index = 1
    return true
end

-- 取下一条回放输入；读完返回 nil。
function R.next_controls()
    if not R.playing then
        return nil
    end
    local c = R.frames[R.index]
    if c == nil then
        return nil
    end
    R.index = R.index + 1
    return c
end

-- 停止记录/回放并回到起点。
function R.stop()
    R.recording = false
    R.playing = false
    R.index = 1
end

-- 当前是否正在回放。
function R.is_playing()
    return R.playing
end

-- 按逻辑时间计算回放进度百分比。
function R.progress(seconds)
    if R.total_grains <= 0 then
        return 0.0
    end
    local grains = math.floor((tonumber(seconds) or 0.0) * GRAINS_PER_SECOND + 0.5)
    local pct = grains / R.total_grains * 100.0
    if pct < 0.0 then pct = 0.0 end
    if pct > 100.0 then pct = 100.0 end
    return pct
end

return R



