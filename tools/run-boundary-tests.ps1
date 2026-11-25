# =============================================================================
#  run-boundary-tests.ps1 —— 第二组：确定性边界测试
#
#  每个 case 单独启动一次引擎，等待退出，再启动下一个；不并行。
#  harness 由 tests/boundary_harness.lua 复制到 Release 的 Project/ 下。
#
#  用法：
#    powershell -ExecutionPolicy Bypass -File tools/run-boundary-tests.ps1
#    ... -Only light
# =============================================================================

# 【已过期，待更新】当前构建目录为 LuaWAG-Luminosa-ver0.2.0；本脚本仍保留旧版本路径，暂不纳入 0.2.0 发布流程。

param(
    [string]$Config = "Release",
    [string]$Only = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exeDir = Join-Path $root "build/$Config/bin/LuaWAG-Luminosa-ver0.1.0"
$exe = Join-Path $exeDir "LuaWAG.exe"
$harnessSrc = Join-Path $root "tests/boundary_harness.lua"
$harnessDst = Join-Path $exeDir "Luminosa/Project/zz_boundary_harness.lua"

if (-not (Test-Path $exe)) { throw "找不到 $exe" }
if (-not (Test-Path $harnessSrc)) { throw "找不到 $harnessSrc" }

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class BoundaryWin32 {
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int cmd);
}
"@

$cases = @(
    # ---- 灯光 ----
    [pscustomobject]@{ Name="light_count_255"; Case="light_count"; N=255; Radius=0; Exit=10; Timeout=30; Mode="marker"; ExpectWarn="none" }
    [pscustomobject]@{ Name="light_count_256"; Case="light_count"; N=256; Radius=0; Exit=10; Timeout=30; Mode="marker"; ExpectWarn="none" }
    [pscustomobject]@{ Name="light_count_257"; Case="light_count"; N=257; Radius=0; Exit=10; Timeout=30; Mode="marker"; ExpectWarn="count" }
    [pscustomobject]@{ Name="light_fill_32"; Case="light_fill"; N=1; Radius=4595; Exit=10; Timeout=30; Mode="fill"; RatioMin=31.9; RatioMax=32.0; ExpectWarn="none" }
    [pscustomobject]@{ Name="light_fill_33"; Case="light_fill"; N=1; Radius=4667; Exit=10; Timeout=30; Mode="fill"; RatioMin=32.9; RatioMax=33.1; ExpectWarn="fill" }
    [pscustomobject]@{ Name="light_commits_8"; Case="light_commits"; N=8; Radius=0; Exit=10; Timeout=30; Mode="marker"; ExpectWarn="none" }
    [pscustomobject]@{ Name="light_commits_9"; Case="light_commits"; N=9; Radius=0; Exit=10; Timeout=30; Mode="marker"; ExpectWarn="commits" }

    # ---- 容器 ----
    [pscustomobject]@{ Name="container_pools_4095"; Case="container_pools"; N=4095; Radius=0; Exit=10; Timeout=180; Mode="marker" }
    [pscustomobject]@{ Name="container_pools_4096"; Case="container_pools"; N=4096; Radius=0; Exit=10; Timeout=180; Mode="marker" }
    [pscustomobject]@{ Name="container_pools_4097"; Case="container_pools"; N=4097; Radius=0; Exit=10; Timeout=180; Mode="marker" }
    [pscustomobject]@{ Name="container_handles_32767"; Case="container_handles"; N=32767; Radius=0; Exit=10; Timeout=120; Mode="marker" }
    [pscustomobject]@{ Name="container_churn"; Case="container_churn"; N=10000; Radius=0; Exit=10; Timeout=60; Mode="marker" }

    # ---- 绘制 ----
    [pscustomobject]@{ Name="draw_batch_2048"; Case="draw_batch"; N=2048; Radius=0; Exit=120; Timeout=30; Mode="batch8192" }
    [pscustomobject]@{ Name="draw_batch_2049"; Case="draw_batch"; N=2049; Radius=0; Exit=120; Timeout=30; Mode="batch8193" }
    [pscustomobject]@{ Name="draw_vertices_32"; Case="draw_vertices"; N=32; Radius=0; Exit=10; Timeout=30; Mode="marker" }
    [pscustomobject]@{ Name="draw_vertices_33"; Case="draw_vertices"; N=33; Radius=0; Exit=10; Timeout=30; Mode="marker" }
    [pscustomobject]@{ Name="draw_variants"; Case="draw_variants"; N=16; Radius=0; Exit=10; Timeout=30; Mode="marker" }
    [pscustomobject]@{ Name="draw_flush_300"; Case="draw_flush"; N=300; Radius=0; Exit=120; Timeout=30; Mode="flush" }

    # ---- FBO / 渲染目标 ----
    [pscustomobject]@{ Name="fbo_normal"; Case="fbo"; N=0; Radius=0; Exit=180; Timeout=30; Mode="fbo"; Action="" }
    [pscustomobject]@{ Name="fbo_1x1"; Case="fbo"; N=0; Radius=0; Exit=180; Timeout=30; Mode="fbo"; Action="1x1" }
    [pscustomobject]@{ Name="fbo_extreme"; Case="fbo"; N=0; Radius=0; Exit=180; Timeout=30; Mode="fbo"; Action="extreme" }
    [pscustomobject]@{ Name="fbo_minimize"; Case="fbo"; N=0; Radius=0; Exit=240; Timeout=40; Mode="fbo"; Action="minimize" }
    [pscustomobject]@{ Name="fbo_resize_reload"; Case="fbo"; N=0; Radius=0; Exit=240; Timeout=40; Mode="fbo"; Action="resize_reload"; Control=$true }
)

if ($Only -ne "") {
    $cases = @($cases | Where-Object { $_.Name -match $Only -or $_.Case -match $Only })
}

Copy-Item $harnessSrc $harnessDst -Force
$results = New-Object System.Collections.ArrayList

try {
    foreach ($c in $cases) {
        $log = Join-Path $exeDir "engine.log"
        $full = Join-Path $exeDir "engine.full.log"
        Remove-Item $log, $full -Force -ErrorAction SilentlyContinue

        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $exe
        $psi.Arguments = "--no-focus-pause"
        $psi.WorkingDirectory = $exeDir
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.EnvironmentVariables["WAG_BOUNDARY_CASE"] = $c.Case
        $psi.EnvironmentVariables["WAG_BOUNDARY_N"] = [string]$c.N
        $psi.EnvironmentVariables["WAG_BOUNDARY_RADIUS"] = [string]$c.Radius
        $psi.EnvironmentVariables["WAG_EXIT_AFTER"] = [string]$c.Exit

        $debugDir = Join-Path $exeDir "debug"
        $cmdFile = Join-Path $debugDir "command.txt"
        $statusFile = Join-Path $debugDir "reload_status.json"
        if ($c.Control) {
            New-Item -ItemType Directory -Path $debugDir -Force | Out-Null
            Remove-Item $cmdFile, $statusFile -Force -ErrorAction SilentlyContinue
            $psi.Arguments += " --control=$debugDir"
        }

        $p = [System.Diagnostics.Process]::Start($psi)
        $actionError = $null

        if ($c.Action -ne "") {
            $hwnd = [IntPtr]::Zero
            for ($i = 0; $i -lt 40; ++$i) {
                Start-Sleep -Milliseconds 100
                $p.Refresh()
                $hwnd = $p.MainWindowHandle
                if ($hwnd -ne [IntPtr]::Zero) { break }
            }
            if ($hwnd -eq [IntPtr]::Zero) {
                $actionError = "没有窗口句柄"
            } else {
                Start-Sleep -Milliseconds 400
                switch ($c.Action) {
                    "1x1" {
                        [void][BoundaryWin32]::SetWindowPos($hwnd, [IntPtr]::Zero,
                            0, 0, 1, 1, 0x0002 -bor 0x0004 -bor 0x0010)
                    }
                    "extreme" {
                        [void][BoundaryWin32]::SetWindowPos($hwnd, [IntPtr]::Zero,
                            0, 0, 3840, 200, 0x0002 -bor 0x0004 -bor 0x0010)
                    }
                    "minimize" {
                        [void][BoundaryWin32]::ShowWindow($hwnd, 6)
                        Start-Sleep -Seconds 1
                        [void][BoundaryWin32]::ShowWindow($hwnd, 9)
                    }
                    "resize_reload" {
                        [void][BoundaryWin32]::SetWindowPos($hwnd, [IntPtr]::Zero,
                            0, 0, 1600, 900, 0x0002 -bor 0x0004 -bor 0x0010)
                        Start-Sleep -Milliseconds 300
                        $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
                        [System.IO.File]::AppendAllText($cmdFile, "reload scripts`n", $utf8NoBom)
                        Start-Sleep -Milliseconds 1500
                    }
                }
            }
        }

        if (-not $p.WaitForExit($c.Timeout * 1000)) {
            $p.Kill()
            $p.WaitForExit()
            $status = "FAIL"
            $detail = "超时"
        } else {
            Start-Sleep -Milliseconds 250
            $text = if (Test-Path $log) { Get-Content $log -Raw } else { "" }
            $hasL4 = $text -match "\[L4\]"
            $status = "PASS"
            $detail = ""
            if ($null -ne $actionError) {
                $status = "FAIL"; $detail = $actionError
            }

            if ($hasL4) {
                $status = "FAIL"; $detail = "L4"
            } else {
                $warnings = @{
                    count = "光源数超过 256"
                    fill = "光图填充超过 32×"
                    commits = "commit 次数超过 8"
                }
                if ($c.ExpectWarn -eq "none") {
                    foreach ($k in $warnings.Keys) {
                        if ($text -match $warnings[$k]) {
                            $status = "FAIL"; $detail = "意外警告：" + $warnings[$k]
                        }
                    }
                } elseif ($c.ExpectWarn) {
                    if ($text -notmatch $warnings[$c.ExpectWarn]) {
                        $status = "FAIL"; $detail = "缺少警告：" + $warnings[$c.ExpectWarn]
                    }
                }
            }

            if ($status -eq "PASS" -and $c.Mode -eq "marker") {
                if ($text -notmatch ("boundary（PASS " + [regex]::Escape($c.Case))) {
                    $status = "FAIL"; $detail = "没有 boundary PASS"
                }
            } elseif ($status -eq "PASS" -and $c.Mode -eq "fbo") {
                if ($text -notmatch "boundary（fbo frame=") {
                    $status = "FAIL"; $detail = "没有 fbo 帧日志"
                } elseif ($c.Control) {
                    if (-not (Test-Path $statusFile)) {
                        $status = "FAIL"; $detail = "没有 reload_status"
                    } else {
                        $st = Get-Content $statusFile -Raw
                        if ($st -notmatch '"phase":"committed"') {
                            $status = "FAIL"; $detail = "reload 未 committed"
                        }
                    }
                }
            } elseif ($status -eq "PASS" -and $c.Mode -eq "fill") {
                $m = [regex]::Match($text, "boundary（.*fill=([0-9.]+)")
                if (-not $m.Success) { $status = "FAIL"; $detail = "没有 fill 值" }
                else {
                    $ratio = [double]$m.Groups[1].Value
                    $detail = "fill=" + $m.Groups[1].Value
                    if ($ratio -lt $c.RatioMin -or $ratio -gt $c.RatioMax) {
                        $status = "FAIL"; $detail += " 超出范围"
                    }
                    $wm = [regex]::Match($text, "光图填充超过 32×（fill=([0-9.]+)）")
                    if ($wm.Success) {
                        $warnRatio = [double]$wm.Groups[1].Value
                        $detail += " warn=" + $wm.Groups[1].Value
                        if ([math]::Abs($warnRatio - $ratio) -gt 0.05) {
                            $status = "FAIL"; $detail += " 与 stats 不一致"
                        }
                    }
                }
            } elseif ($status -eq "PASS" -and ($c.Mode -eq "batch8192" -or $c.Mode -eq "batch8193")) {
                $m = [regex]::Match($text, "渲染统计（提交=(\d+)\s+flush=(\d+)\s+顶点=(\d+)")
                if (-not $m.Success) { $status = "FAIL"; $detail = "没有渲染统计" }
                else {
                    $verts = [int]$m.Groups[3].Value
                    $wantVerts = if ($c.Mode -eq "batch8192") { 8192 } else { 8196 }
                    if ($verts -ne $wantVerts) { $status = "FAIL"; $detail = "顶点=$verts" }
                    else { $detail = "顶点=$verts flush=" + $m.Groups[2].Value }
                }
            } elseif ($status -eq "PASS" -and $c.Mode -eq "flush") {
                $m = [regex]::Match($text, "渲染统计（提交=(\d+)\s+flush=(\d+)\s+顶点=(\d+)")
                if (-not $m.Success) { $status = "FAIL"; $detail = "没有渲染统计" }
                else {
                    $flush = [int]$m.Groups[2].Value
                    if ($flush -lt 32) { $status = "FAIL"; $detail = "flush=$flush" }
                    else { $detail = "flush=$flush" }
                }
            }
        }

        $p.Dispose()
        $color = if ($status -eq "PASS") { "Green" } else { "Red" }
        Write-Host ("[{0,-22}] {1,-4} {2}" -f $c.Name, $status, $detail) -ForegroundColor $color
        [void]$results.Add([pscustomobject]@{
            Name = $c.Name; Status = $status; Detail = $detail
        })
    }
}
finally {
    Remove-Item $harnessDst -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "===== 边界测试汇总 ====="
$results | Format-Table -AutoSize
$failed = @($results | Where-Object { $_.Status -ne "PASS" })
if ($failed.Count -gt 0) { exit 1 }
exit 0











