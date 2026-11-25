# =============================================================================
#  run-selftests.ps1 —— 逐进程运行引擎 selftest
#
#  【为什么每个自检单独一个进程】
#    这些 debug_*_selftest_ 共用 renderer 的 lights_ / bands_ /
#    frame_commits_ 等状态；同一进程连续跑会让后面的断言看到前面的残留。
#    ⇒ 一个自检 = 启动一次引擎 = 读日志 = 关闭，再跑下一个。
#
#  【用法】
#    powershell -ExecutionPolicy Bypass -File tools/run-selftests.ps1
#    powershell -ExecutionPolicy Bypass -File tools/run-selftests.ps1 -Config Release
# =============================================================================

# 【已过期，待更新】当前构建目录为 LuaWAG-Luminosa-ver0.2.0；本脚本仍保留旧版本路径，暂不纳入 0.2.0 发布流程。

param(
    [string[]]$Config = @("Release", "Debug-Release")
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$tests = @(
    [pscustomobject]@{ Name = "RT";        Env = "WAG_RT_SELFTEST";       Tag = "rt" },
    [pscustomobject]@{ Name = "INSTANCE";  Env = "WAG_INSTANCE_SELFTEST"; Tag = "inst" },
    [pscustomobject]@{ Name = "MASK";      Env = "WAG_MASK_SELFTEST";     Tag = "mask" },
    [pscustomobject]@{ Name = "LIGHT";     Env = "WAG_LIGHT_SELFTEST";    Tag = "light" },
    [pscustomobject]@{ Name = "BAND";      Env = "WAG_BAND_SELFTEST";     Tag = "band" },
    [pscustomobject]@{ Name = "SELFONLY";  Env = "WAG_SELFONLY_SELFTEST"; Tag = "selfonly" },
    [pscustomobject]@{ Name = "BUDGET";    Env = "WAG_BUDGET_SELFTEST";   Tag = "budget" }
)

$results = New-Object System.Collections.ArrayList

foreach ($cfg in $Config) {
    $exeDir = Join-Path $root "build/$cfg/bin/LuaWAG-Luminosa-ver0.1.0"
    $exe = Join-Path $exeDir "LuaWAG.exe"
    if (-not (Test-Path $exe)) {
        Write-Host "[SKIP] $cfg：找不到 $exe" -ForegroundColor Yellow
        continue
    }

    Write-Host ""
    Write-Host "===== $cfg =====" -ForegroundColor Cyan

    foreach ($t in $tests) {
        $log = Join-Path $exeDir "engine.log"
        $full = Join-Path $exeDir "engine.full.log"
        Remove-Item $log, $full -Force -ErrorAction SilentlyContinue

        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $exe
        $psi.Arguments = "--no-focus-pause"
        $psi.WorkingDirectory = $exeDir
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.EnvironmentVariables[$t.Env] = "1"
        $psi.EnvironmentVariables["WAG_EXIT_AFTER"] = "3"

        $p = [System.Diagnostics.Process]::Start($psi)
        if (-not $p.WaitForExit(15000)) {
            $p.Kill()
            $p.WaitForExit()
            $pass = $false
            $reason = "超时未退出"
        } else {
            Start-Sleep -Milliseconds 200
            $text = if (Test-Path $log) { Get-Content $log -Raw } else { "" }
            $hasPass = $text -match ("\[" + [regex]::Escape($t.Tag) + "\] 自检（PASS")
            $hasL4 = $text -match "\[L4\]"
            $pass = $hasPass -and -not $hasL4
            if ($pass) {
                $reason = "PASS"
            } elseif ($hasL4) {
                $reason = "L4"
            } else {
                $reason = "没有 PASS 行"
            }
        }

        $p.Dispose()
        $status = if ($pass) { "PASS" } else { "FAIL" }
        $color = if ($pass) { "Green" } else { "Red" }
        Write-Host ("  [{0,-9}] {1,-8} {2}" -f $t.Name, $status, $reason) -ForegroundColor $color
        [void]$results.Add([pscustomobject]@{
            Config = $cfg
            Test = $t.Name
            Status = $status
            Reason = $reason
        })
    }
}

Write-Host ""
Write-Host "===== 汇总 =====" -ForegroundColor Cyan
$results | Format-Table -AutoSize

$failed = @($results | Where-Object { $_.Status -ne "PASS" })
if ($failed.Count -gt 0) {
    exit 1
}
exit 0
