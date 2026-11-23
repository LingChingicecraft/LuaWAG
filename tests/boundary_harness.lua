-- =============================================================================
--  boundary_harness.lua —— 确定性边界测试夹具
--
--  用法：由 tools/run-boundary-tests.ps1 复制到 Release 的 Project/ 下，
--  每个测试单独启动一次引擎；环境变量选择 case。
--
--  只在 WAG_BOUNDARY_CASE 非空时接管；正常游戏不加载这个文件。
-- =============================================================================

local CASE = os.getenv("WAG_BOUNDARY_CASE") or ""
if CASE == "" then
    return
end

local N = tonumber(os.getenv("WAG_BOUNDARY_N") or "") or 0
local RADIUS = tonumber(os.getenv("WAG_BOUNDARY_RADIUS") or "") or 5000.0

local M = {
    done = false,
    ext_frames = 0,
    render_frames = 0,
}

local original_main_render = MainRenderFunc

local function report(ok, detail)
    if M.done then
        return
    end
    M.done = true
    wag.log.info("boundary", string.format("%s %s %s",
        ok and "PASS" or "FAIL", CASE, detail or ""))
    wag.engine.exit()
end

local function is_draw_case()
    return CASE:sub(1, 5) == "draw_"
end

local function ensure_light_shader()
    local ok, err = pcall(wag.light.load_shader, "Luminosa/Project/shaders")
    if not ok then
        report(false, "light shader: " .. tostring(err))
        return false
    end
    return true
end

local function add_lights(n, radius)
    wag.light.band { id = "boundary", from = 0, to = 0 }
    local l = {
        kind = "point",
        x = 0.0,
        y = 0.0,
        radius = radius,
        color = 0xFFFFFFFF,
        intensity = 1.0,
        layer = 0,
        reach = "below",
        depth = 0,
    }
    for i = 1, n do
        l.x = ((i - 1) % 20) * 10.0 - 95.0
        l.y = (math.floor((i - 1) / 20) % 16) * 10.0 - 75.0
        wag.light.add(l)
    end
end

local function run_light_case()
    if not ensure_light_shader() then
        return
    end

    if CASE == "light_count" then
        add_lights(N, 50.0)
        wag.light.commit("boundary")
        local s = wag.light.stats()
        report(s.lights == N and s.commits == 1,
            string.format("N=%d lights=%d commits=%d", N, s.lights, s.commits))
        return
    end

    if CASE == "light_fill" then
        add_lights(1, RADIUS)
        wag.light.commit("boundary")
        local s = wag.light.stats()
        report(s.fill_ratio > 0.0,
            string.format("radius=%.1f fill=%.6f", RADIUS, s.fill_ratio))
        return
    end

    if CASE == "light_commits" then
        for i = 1, N do
            local id = "b" .. i
            wag.light.band { id = id, from = i, to = i }
            wag.light.add {
                kind = "point", x = 0.0, y = 0.0, radius = 10.0,
                color = 0xFFFFFFFF, intensity = 1.0, layer = i,
                reach = "below", depth = 0,
            }
            wag.light.commit(id)
        end
        local s = wag.light.stats()
        report(s.commits == N, string.format("N=%d commits=%d", N, s.commits))
        return
    end
end

local function run_container_case()
    local ok, err = pcall(function()
        wag.container.set_dispatcher(function() end)
        wag.container.register_class(1, {})
    end)
    if not ok then
        report(false, "dispatcher/class: " .. tostring(err))
        return
    end

    if CASE == "container_pools" then
        local made = 0
        local last_error = ""
        for i = 1, N do
            local okp, id = pcall(wag.container.new_pool, "p" .. i)
            if okp then
                made = made + 1
            else
                last_error = tostring(id)
            end
        end
        -- 4097 的期望：前 4096 个成功，最后一个由引擎拒绝。
        if N == 4097 then
            report(made == 4096 and last_error ~= "",
                string.format("N=%d made=%d rejected=%s", N, made, last_error))
        else
            report(made == N, string.format("N=%d made=%d", N, made))
        end
        return
    end

    if CASE == "container_handles" then
        local okp, pool = pcall(wag.container.new_pool, "handles")
        if not okp then
            report(false, "new_pool: " .. tostring(pool))
            return
        end
        local made = 0
        for i = 1, 32767 do
            local oks, h = pcall(wag.container.spawn, pool, { class = 1 })
            if oks then
                made = made + 1
            else
                report(false, "spawn at " .. i .. ": " .. tostring(h))
                return
            end
        end
        local live = wag.container.count_live_all()
        report(made == 32767 and live == 32767,
            string.format("made=%d live=%d", made, live))
        return
    end

    if CASE == "container_churn" then
        local okp, pool = pcall(wag.container.new_pool, "churn")
        if not okp then
            report(false, "new_pool: " .. tostring(pool))
            return
        end
        local handles = {}
        for i = 1, 10000 do
            local oks, h = pcall(wag.container.spawn, pool, { class = 1 })
            if not oks then
                report(false, "initial spawn at " .. i .. ": " .. tostring(h))
                return
            end
            handles[#handles + 1] = h
        end
        for i = 1, 5000 do
            local oks, e = pcall(wag.container.kill, handles[i])
            if not oks then
                report(false, "kill at " .. i .. ": " .. tostring(e))
                return
            end
        end
        local made = 0
        for i = 1, 5000 do
            local oks, h = pcall(wag.container.spawn, pool, { class = 1 })
            if oks then
                made = made + 1
            end
        end
        local live = wag.container.count_live_all()
        report(made == 5000 and live >= 10000,
            string.format("respawned=%d live=%d", made, live))
        return
    end
end

local function vertex_quad(x, y)
    return {
        { x = x, y = y, z = 0, u = 0, v = 0, color = 0xFFFFFFFF },
        { x = x + 4, y = y, z = 0, u = 1, v = 0, color = 0xFFFFFFFF },
        { x = x + 4, y = y - 4, z = 0, u = 1, v = 1, color = 0xFFFFFFFF },
        { x = x, y = y - 4, z = 0, u = 0, v = 1, color = 0xFFFFFFFF },
    }
end

local function draw_case(info)
    if M.done then
        return
    end
    M.render_frames = M.render_frames + 1

    if CASE == "draw_batch" then
        if M.render_frames == 1 then
            wag.draw.reset_state()
            for i = 1, N do
                local q = vertex_quad((i % 100) * 5 - 500, (i % 70) * 5 - 300)
                wag.draw.draw_vertices(0, 0, 0, q[1], q[2], q[3], q[4])
            end
            wag.log.info("boundary", string.format("draw_batch quads=%d", N))
        end
        return
    end

    if CASE == "draw_vertices" then
        if M.render_frames == 1 then
            local verts = {}
            for i = 1, N do
                local a = (i / N) * math.pi * 2.0
                verts[#verts + 1] = {
                    x = math.cos(a) * 200.0,
                    y = math.sin(a) * 200.0,
                    z = 0, u = 0, v = 0, color = 0xFFFFFFFF,
                }
            end
            local ok = pcall(function()
                wag.draw.draw_vertices(0, 0, 0, unpack(verts))
            end)
            -- 32 个是合法上限；33 个应当被拒绝（L2，帧继续）——两者都算预期。
            report(ok == (N <= 32), string.format("vertices=%d ok=%s", N, tostring(ok)))
        end
        return
    end

    if CASE == "draw_variants" then
        if M.render_frames == 1 then
            local alphas = { "Normal", "Replace" }
            local colors = { "Mul", "Add", "Sub", "Alphablend" }
            for c = 1, #colors do
                for a = 1, #alphas do
                    for f = 0, 1 do
                        wag.draw.set_state {
                            blend = "Normal",
                            texture_alpha = alphas[a],
                            vertex_color = colors[c],
                            fog = (f == 1) and {
                                color = 0x000000, near = 0.0, far = 1000.0,
                                strength = 1.0,
                            } or false,
                        }
                        local q = vertex_quad(0, 0)
                        wag.draw.draw_vertices(0, 0, 0, q[1], q[2], q[3], q[4])
                    end
                end
            end
            wag.draw.reset_state()
            report(true, "16 variants submitted")
        end
        return
    end

    if CASE == "draw_flush" then
        if M.render_frames == 1 then
            local names = { "Normal", "Add", "AddNoAlpha", "Sub",
                            "SubNoAlpha", "RevSub", "MulAlpha", "MulColor" }
            local ac = { "Normal", "Replace" }
            local vc = { "Mul", "Add", "Sub", "Alphablend" }
            local states = 0
            for i = 1, N do
                local k = i - 1
                local b = (k % #names) + 1
                k = math.floor(k / #names)
                local a = (k % #ac) + 1
                k = math.floor(k / #ac)
                local v = (k % #vc) + 1
                wag.draw.set_state {
                    blend = names[b],
                    texture_alpha = ac[a],
                    vertex_color = vc[v],
                }
                local q = vertex_quad((i % 100) * 5 - 500, (i % 70) * 5 - 300)
                wag.draw.draw_vertices(0, 0, 0, q[1], q[2], q[3], q[4])
                states = states + 1
            end
            wag.draw.reset_state()
            wag.log.info("boundary", string.format("draw_flush states=%d", states))
        end
        return
    end
end

function GameInit()
    if CASE:sub(1, 5) == "light" or CASE:sub(1, 3) == "fbo" then
        ensure_light_shader()
    end
end

function ExtFrameFunc(info)
    if M.done then
        return
    end
    M.ext_frames = M.ext_frames + 1
    if CASE:sub(1, 5) == "light" and M.ext_frames == 2 then
        run_light_case()
    elseif CASE:sub(1, 9) == "container" and M.ext_frames == 1 then
        run_container_case()
    end
end

function ExtRenderFunc(info)
    if CASE:sub(1, 3) ~= "fbo" then
        return
    end
    local ok = pcall(function()
        wag.light.band { id = "fbo", from = 0, to = 0 }
        wag.light.add {
            kind = "point", x = 0.0, y = 0.0, radius = 320.0,
            color = 0xFFFFFFFF, intensity = 1.0, layer = 0,
            reach = "below", depth = 0,
        }
        wag.light.commit("fbo")
    end)
    if ok then
        if (M.render_frames % 30) == 0 then
            wag.log.info("boundary", string.format("fbo frame=%d", M.render_frames + 1))
        end
    end
    M.render_frames = M.render_frames + 1
end

-- draw 用例要接管 main 渲染，避免农田背景污染批统计。
if is_draw_case() then
    function MainRenderFunc(info)
        draw_case(info)
    end
elseif original_main_render ~= nil then
    MainRenderFunc = original_main_render
end



