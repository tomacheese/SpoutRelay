#requires -Version 7
# SpoutRelay state/health.json・state/metrics.json E2E テストスクリプト (pwsh 7+ 必須)
# 使い方: cd e2e-test; pwsh ./state-files-tests.ps1
#
# MetricsStore が出力する state/health.json と state/metrics.json の
# JSON 構造・値を実際のリレー実行を通じて直接検証する。

$ErrorActionPreference = "Continue"
$Root       = Split-Path -Parent $PSScriptRoot
$BuildDir   = "$Root\build"
$TestDir    = $PSScriptRoot
$CaptureDir = "$TestDir\capture"
$LogDir     = "$TestDir\logs"
$StateDir   = "$TestDir\state"
$RTSP       = "rtsp://127.0.0.1:8554/spout-e2e"
$RelayLog   = "$LogDir\spoutrelay-e2e-test.jsonl"
$HealthFile = "$StateDir\health.json"
$MetricsFile = "$StateDir\metrics.json"

$PassCount = 0; $FailCount = 0
function Pass($msg) { Write-Host "[PASS] $msg" -ForegroundColor Green;  $script:PassCount++ }
function Fail($msg) { Write-Host "[FAIL] $msg" -ForegroundColor Red;    $script:FailCount++ }
function Info($msg) { Write-Host "  >> $msg"   -ForegroundColor Cyan }
function Diag($msg) { Write-Host "     $msg"   -ForegroundColor DarkGray }

function Stop-AllProcs {
    @("mediamtx","spout-relay","spout_test_sender","spout_static_sender","ffmpeg","ffprobe") |
        ForEach-Object { Get-Process $_ -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue }
    Start-Sleep -Milliseconds 800
}

function Start-Mediamtx {
    $p = Start-Process mediamtx -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 1200
    return $p
}

function Clear-RelayLog {
    Remove-Item $RelayLog -Force -EA SilentlyContinue
}

function Clear-StateFiles {
    Remove-Item $HealthFile -Force -EA SilentlyContinue
    Remove-Item $MetricsFile -Force -EA SilentlyContinue
}

function Wait-LogEvent {
    param([string]$Pattern, [int]$TimeoutSec = 10)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $RelayLog) {
            $content = Get-Content $RelayLog -Raw -EA SilentlyContinue
            if ($content -match $Pattern) { return $true }
        }
        Start-Sleep -Milliseconds 300
    }
    return $false
}

# 指定したファイルが「存在し」かつ「有効な JSON としてパース可能」になるまで待つ
function Wait-JsonFile {
    param([string]$Path, [int]$TimeoutSec = 15)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $Path) {
            try {
                $content = Get-Content $Path -Raw -EA Stop
                if ($content.Trim().Length -gt 0) {
                    return ($content | ConvertFrom-Json -EA Stop)
                }
            } catch {
                # 書き込み中で不完全な場合があるためリトライ
            }
        }
        Start-Sleep -Milliseconds 300
    }
    return $null
}

# -----------------------------------------------------------------------
Write-Host "`n========================================" -ForegroundColor Yellow
Write-Host "  SpoutRelay State Files (health.json / metrics.json) Tests" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Yellow

Stop-AllProcs
Remove-Item "$CaptureDir\*" -Force -EA SilentlyContinue
New-Item -ItemType Directory -Force $CaptureDir | Out-Null
New-Item -ItemType Directory -Force $LogDir     | Out-Null
New-Item -ItemType Directory -Force $StateDir   | Out-Null

# -----------------------------------------------------------------------
# Test 1 — PLACEHOLDER 配信中の health.json / metrics.json
# -----------------------------------------------------------------------
Write-Host "--- Test 1: health.json / metrics.json during PLACEHOLDER streaming ---"
& {
    Clear-RelayLog
    Clear-StateFiles
    $mtx = Start-Mediamtx
    # sender を起動せずに起動 → PLACEHOLDER (NO SIGNAL) へ遷移するはず
    $relay = Start-Process -FilePath "$BuildDir\spout-relay.exe" -ArgumentList "--config","$TestDir\config\config_placeholder.json" -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden

    Info "Waiting for placeholder_started event (up to 20s)..."
    if (-not (Wait-LogEvent -Pattern '"event":"placeholder_started"' -TimeoutSec 20)) {
        Fail "placeholder_started event not observed within 20s"
        Diag "Relay log tail: $(Get-Content $RelayLog -Tail 5 -EA SilentlyContinue)"
        Stop-Process -Id $relay.Id -Force -EA SilentlyContinue
        Stop-AllProcs
        return
    }

    # emit_health_interval_ms / emit_metrics_interval_ms (=500ms) より長く待ち、
    # 確実に health.json / metrics.json が書き出されるようにする
    Start-Sleep -Milliseconds 1500

    $health = Wait-JsonFile -Path $HealthFile -TimeoutSec 15
    if ($null -eq $health) {
        Fail "health.json was not created or is not valid JSON"
    } else {
        Pass "health.json is valid JSON"

        if ($health.PSObject.Properties.Name -contains "healthy" -and
            $health.PSObject.Properties.Name -contains "state" -and
            $health.PSObject.Properties.Name -contains "ts") {
            Pass "health.json has expected fields (healthy, state, ts)"
        } else {
            Fail "health.json is missing expected fields: $($health | ConvertTo-Json -Compress)"
        }

        if ($health.state -eq "PLACEHOLDER") {
            Pass "health.json state == 'PLACEHOLDER'"
        } else {
            Fail "health.json state == '$($health.state)' (expected 'PLACEHOLDER')"
        }

        if ($health.healthy -eq $true) {
            Pass "health.json healthy == true while in PLACEHOLDER"
        } else {
            Fail "health.json healthy == $($health.healthy) (expected true)"
        }

        try {
            [DateTimeOffset]::Parse($health.ts) | Out-Null
            Pass "health.json ts is a valid ISO 8601 timestamp ($($health.ts))"
        } catch {
            Fail "health.json ts is not a valid timestamp: $($health.ts)"
        }
    }

    $metrics = Wait-JsonFile -Path $MetricsFile -TimeoutSec 15
    if ($null -eq $metrics) {
        Fail "metrics.json was not created or is not valid JSON"
    } else {
        Pass "metrics.json is valid JSON"

        $expectedFields = @("state","sender_name","sender_width","sender_height","sender_fps",
                             "bitrate_kbps","current_fps","rtsp_url","encoder_codec","uptime_ms",
                             "frames_received","frames_encoded","frames_dropped","rtsp_errors",
                             "reconnect_attempts","ts")
        $missing = $expectedFields | Where-Object { $metrics.PSObject.Properties.Name -notcontains $_ }
        if ($missing.Count -eq 0) {
            Pass "metrics.json has all expected fields"
        } else {
            Fail "metrics.json is missing fields: $($missing -join ', ')"
        }

        if ($metrics.state -eq "PLACEHOLDER") {
            Pass "metrics.json state == 'PLACEHOLDER'"
        } else {
            Fail "metrics.json state == '$($metrics.state)' (expected 'PLACEHOLDER')"
        }

        if ($metrics.rtsp_url -eq $RTSP) {
            Pass "metrics.json rtsp_url == '$RTSP'"
        } else {
            Fail "metrics.json rtsp_url == '$($metrics.rtsp_url)' (expected '$RTSP')"
        }

        if ([string]::IsNullOrEmpty($metrics.encoder_codec)) {
            Fail "metrics.json encoder_codec is empty"
        } else {
            Pass "metrics.json encoder_codec == '$($metrics.encoder_codec)'"
        }

        if ($metrics.uptime_ms -gt 0) {
            Pass "metrics.json uptime_ms > 0 ($($metrics.uptime_ms))"
        } else {
            Fail "metrics.json uptime_ms == $($metrics.uptime_ms) (expected > 0)"
        }

        if ($metrics.frames_encoded -gt 0) {
            Pass "metrics.json frames_encoded > 0 ($($metrics.frames_encoded))"
        } else {
            Fail "metrics.json frames_encoded == $($metrics.frames_encoded) (expected > 0)"
        }
    }

    Stop-Process -Id $relay.Id -Force -EA SilentlyContinue
    Stop-AllProcs
}

# -----------------------------------------------------------------------
# Test 2 — FATAL 遷移後の health.json (healthy == false)
# -----------------------------------------------------------------------
Write-Host "`n--- Test 2: health.json reflects healthy=false after FATAL ---"
& {
    Clear-RelayLog
    Clear-StateFiles
    $mtx    = Start-Mediamtx
    $sender = Start-Process -FilePath "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
    $relay  = Start-Process -FilePath "$BuildDir\spout-relay.exe" -ArgumentList "--config","$TestDir\config\config_fatal.json" -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden

    Info "Waiting for RTSP stream (up to 30s)..."
    $streamLive = $false
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $RelayLog) {
            $content = Get-Content $RelayLog -Raw -EA SilentlyContinue
            if ($content -match '"event":"publish_started"') { $streamLive = $true; break }
        }
        Start-Sleep -Milliseconds 700
    }

    if (-not $streamLive) {
        Fail "Stream did not start — skipping Test 2"
        Stop-AllProcs
        return
    }

    Info "Stream live! Killing mediamtx permanently to force FATAL..."
    Stop-Process -Id $mtx.Id -Force -EA SilentlyContinue

    Info "Waiting for FATAL state (up to 15s)..."
    if (-not (Wait-LogEvent -Pattern '"event":"fatal_exit"|"to":"FATAL"' -TimeoutSec 15)) {
        Fail "FATAL state not reached within 15s"
        Diag "Relay log tail: $(Get-Content $RelayLog -Tail 5 -EA SilentlyContinue)"
        Stop-AllProcs
        return
    }
    Diag "FATAL state detected in log"

    $exited = $relay.WaitForExit(5000)
    if (-not $exited) {
        Fail "FATAL logged but relay process did not exit within 5s"
        Stop-Process -Id $relay.Id -Force -EA SilentlyContinue
        Stop-AllProcs
        return
    }
    Pass "relay reached FATAL and exited (code: $($relay.ExitCode))"

    $health = Wait-JsonFile -Path $HealthFile -TimeoutSec 5
    if ($null -eq $health) {
        Fail "health.json was not created or is not valid JSON after FATAL exit"
    } else {
        if ($health.state -eq "FATAL") {
            Pass "health.json state == 'FATAL' after fatal exit"
        } else {
            Fail "health.json state == '$($health.state)' (expected 'FATAL')"
        }

        if ($health.healthy -eq $false) {
            Pass "health.json healthy == false after FATAL"
        } else {
            Fail "health.json healthy == $($health.healthy) (expected false)"
        }
    }

    Stop-AllProcs
}

# -----------------------------------------------------------------------
Write-Host "`n========================================"  -ForegroundColor Yellow
Write-Host "  State Files Results: $PassCount PASS  /  $FailCount FAIL" -ForegroundColor $(if ($FailCount -eq 0) { "Green" } else { "Red" })
Write-Host "========================================`n" -ForegroundColor Yellow

if ($FailCount -gt 0) { exit 1 } else { exit 0 }
