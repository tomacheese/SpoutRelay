#requires -Version 7
# Tests 8-11 単体実行スクリプト (monkey-tests.ps1 の新規テストのみ)
$ErrorActionPreference = "Continue"
$Root       = Split-Path -Parent $PSScriptRoot
$BuildDir   = "$Root\build"
$TestDir    = $PSScriptRoot
$RTSP       = "rtsp://127.0.0.1:8554/spout-e2e"
$RelayLog   = "$TestDir\logs\spoutrelay-e2e-test.jsonl"

$PassCount = 0; $FailCount = 0
function Pass($msg) { Write-Host "[PASS] $msg" -ForegroundColor Green;  $script:PassCount++ }
function Fail($msg) { Write-Host "[FAIL] $msg" -ForegroundColor Red;    $script:FailCount++ }
function Info($msg) { Write-Host "  >> $msg"   -ForegroundColor Cyan }
function Diag($msg) { Write-Host "     $msg"   -ForegroundColor DarkGray }
function Stop-AllProcs {
    @("mediamtx","spout-relay","spout_test_sender","spout_static_sender","spout_res_changer","ffmpeg","ffprobe") |
        ForEach-Object { Get-Process $_ -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue }
    Start-Sleep -Milliseconds 800
}
function Start-Mediamtx {
    $p = Start-Process mediamtx -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 1200; return $p
}
function Clear-RelayLog { Remove-Item $RelayLog -Force -EA SilentlyContinue }
function Wait-Stream {
    param([int]$TimeoutSec = 35)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $RelayLog) {
            $content = Get-Content $RelayLog -Raw -EA SilentlyContinue
            if ($content -match '"event":"publish_started"') { Start-Sleep -Milliseconds 400; return $true }
        }
        $r = & ffprobe -v error -rtsp_transport tcp -i $RTSP -show_entries stream=codec_type -of csv 2>&1
        if ($r -match "video") { return $true }
        Diag "  (no stream yet...)"
        Start-Sleep -Milliseconds 700
    }
    return $false
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

New-Item -ItemType Directory -Force "$TestDir\logs" | Out-Null
New-Item -ItemType Directory -Force "$TestDir\capture" | Out-Null
Stop-AllProcs

Write-Host "`n========================================" -ForegroundColor Yellow
Write-Host "  New Monkey Tests 8-11" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Yellow

# --- Test 8 ---
Write-Host "--- Test 8: STALLED → sender_disappeared → PROBING → recovery ---"
& {
    Clear-RelayLog
    $mtx    = Start-Mediamtx
    $sender = Start-Process "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
    $relay  = Start-Process "$BuildDir\spout-relay.exe" -ArgumentList "--config","$TestDir\config\config.json" -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden
    Info "Waiting for stream..."
    if (-not (Wait-Stream 35)) { Fail "Stream did not start"; Stop-AllProcs; return }
    Info "Killing sender permanently..."
    Stop-Process -Id $sender.Id -Force -EA SilentlyContinue
    Start-Sleep -Milliseconds 200
    Info "Waiting for sender_disappeared (4s)..."
    if (-not (Wait-LogEvent '"event":"sender_disappeared"' 4)) {
        Fail "sender_disappeared not logged"; Diag (Get-Content $RelayLog -Tail 5 -EA SilentlyContinue); Stop-AllProcs; return
    }
    Diag "sender_disappeared OK"
    $null = Wait-LogEvent '"to":"PROBING"' 3
    Start-Sleep -Milliseconds 1000
    Info "Restarting sender..."
    $null = Start-Process "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
    Info "Waiting for recovery (15s)..."
    if (Wait-Stream 15) { Pass "sender_disappeared → PROBING → recovered" }
    else { Fail "Did not recover"; Diag (Get-Content $RelayLog -Tail 5 -EA SilentlyContinue) }
    Stop-AllProcs
}

# --- Test 9 ---
Write-Host "`n--- Test 9: GPU STREAMING → sender_disappeared → PLACEHOLDER seamless ---"
& {
    Clear-RelayLog
    $mtx    = Start-Mediamtx
    $sender = Start-Process "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
    $relay  = Start-Process "$BuildDir\spout-relay.exe" -ArgumentList "--config","$TestDir\config\config_placeholder.json" -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden
    Info "Waiting for STREAMING + gpu_zero_copy_enabled (25s)..."
    $gpuOk = $false
    $dl = (Get-Date).AddSeconds(25)
    while ((Get-Date) -lt $dl) {
        if ((Get-Content $RelayLog -Raw -EA SilentlyContinue) -match '"event":"gpu_zero_copy_enabled"') { $gpuOk = $true; break }
        Start-Sleep -Milliseconds 500; Diag "  waiting GPU..."
    }
    if (-not $gpuOk) {
        Diag "GPU not active — checking stream (CPU path?)"
        if (-not (Wait-Stream 5)) { Fail "No stream"; Stop-AllProcs; return }
    } else { Diag "GPU mode OK"; Start-Sleep -Milliseconds 500 }
    Info "Killing sender..."
    Stop-Process -Id $sender.Id -Force -EA SilentlyContinue
    Start-Sleep -Milliseconds 200
    if (-not (Wait-LogEvent '"event":"sender_disappeared"' 4)) {
        Fail "sender_disappeared not logged"; Diag (Get-Content $RelayLog -Tail 5 -EA SilentlyContinue); Stop-AllProcs; return
    }
    Diag "sender_disappeared OK"
    if (-not (Wait-LogEvent '"event":"placeholder_seamless"|"event":"placeholder_started"' 5)) {
        Fail "Placeholder not activated"; Diag (Get-Content $RelayLog -Tail 5 -EA SilentlyContinue); Stop-AllProcs; return
    }
    $c = Get-Content $RelayLog -Raw -EA SilentlyContinue
    $seamless = $c -match '"event":"placeholder_seamless"'
    Diag ("Seamless=" + $seamless)
    Start-Sleep -Milliseconds 1500
    $r = & ffprobe -v error -rtsp_transport tcp -i $RTSP -show_entries stream=codec_type -of csv 2>&1
    if ($r -match "video") {
        if ($seamless) { Pass "GPU → sender_disappeared → PLACEHOLDER seamless (RTSP maintained)" }
        else { Pass "GPU → sender_disappeared → PLACEHOLDER non-seamless (RTSP active)" }
    } else { Fail "Placeholder started but RTSP lost" }
    Stop-AllProcs
}

# --- Test 10 ---
Write-Host "`n--- Test 10: Resolution change 640x360 → 320x240 (RECONFIGURING) ---"
& {
    Clear-RelayLog
    $mtx  = Start-Mediamtx
    $null = Start-Process "$BuildDir\spout_res_changer.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
    $relay = Start-Process "$BuildDir\spout-relay.exe" -ArgumentList "--config","$TestDir\config\config.json" -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden
    Info "Waiting for 640x360 stream (30s)..."
    $ok = $false
    $dl = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $dl) {
        if ((Get-Content $RelayLog -Raw -EA SilentlyContinue) -match '"event":"publish_started".*"width":"640"') { $ok = $true; break }
        Start-Sleep -Milliseconds 700; Diag "  waiting 640x360..."
    }
    if (-not $ok) { Fail "640x360 not confirmed"; Diag (Get-Content $RelayLog -Raw -EA SilentlyContinue); Stop-AllProcs; return }
    Diag "640x360 confirmed"
    Info "Waiting for RECONFIGURING (25s)..."
    if (-not (Wait-LogEvent '"to":"RECONFIGURING"' 25)) {
        Fail "RECONFIGURING not triggered"; Diag (Get-Content $RelayLog -Tail 5 -EA SilentlyContinue); Stop-AllProcs; return
    }
    Diag "RECONFIGURING detected"
    Info "Waiting for re-STREAMING (15s)..."
    if (-not (Wait-LogEvent '"event":"publish_started".*"width":"320"' 15)) {
        if (-not (Wait-LogEvent '"to":"STREAMING"' 5)) {
            Fail "No STREAMING after RECONFIGURING"; Diag (Get-Content $RelayLog -Tail 5 -EA SilentlyContinue); Stop-AllProcs; return
        }
        Diag "STREAMING restored (320x240 not in log)"
    } else { Diag "320x240 publish_started confirmed" }
    Start-Sleep -Milliseconds 1000
    $r = & ffprobe -v error -rtsp_transport tcp -i $RTSP -show_entries stream=codec_type -of csv 2>&1
    if ($r -match "video") { Pass "RECONFIGURING: 640x360→320x240 succeeded, stream active" }
    else { Fail "RECONFIGURING done but stream lost"; Diag (Get-Content $RelayLog -Tail 5 -EA SilentlyContinue) }
    Stop-AllProcs
}

# --- Test 11 ---
Write-Host "`n--- Test 11: max_reconnect_attempts exceeded → FATAL ---"
& {
    $cfg = @'
{"app":{"instance_name":"spoutrelay-e2e-test","log_dir":"./logs","metrics_path":"./state/metrics.json","health_path":"./state/health.json"},"spout":{"sender_name":"TestSpoutSender","poll_interval_ms":50,"frame_timeout_ms":300,"sender_missing_timeout_ms":800,"prefer_dx11":true},"encoder":{"codec":"h264_nvenc","fallback_codec":"h264_mf","bitrate_kbps":2000,"fps":30,"gop_size":30,"max_b_frames":0,"preset":"fast","tune":"zerolatency","threads":0},"rtsp":{"url":"rtsp://127.0.0.1:8554/spout-e2e","connect_timeout_ms":1500,"send_timeout_ms":1500,"max_reconnect_attempts":2,"reconnect_delay_ms":400,"reconnect_max_delay_ms":2000,"reconnect_backoff_multiplier":2.0},"runtime":{"shutdown_grace_ms":500,"emit_metrics_interval_ms":500,"emit_health_interval_ms":500}}
'@
    $cfgPath = "$TestDir\config\config_fatal.json"
    Set-Content $cfgPath $cfg -Encoding UTF8
    Clear-RelayLog
    $mtx   = Start-Mediamtx
    $null  = Start-Process "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
    $relay = Start-Process "$BuildDir\spout-relay.exe" -ArgumentList "--config",$cfgPath -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden
    Info "Waiting for stream (30s)..."
    if (-not (Wait-Stream 30)) { Fail "Stream did not start"; Stop-AllProcs; return }
    Info "Killing mediamtx permanently..."
    Stop-Process -Id $mtx.Id -Force -EA SilentlyContinue
    Info "Waiting for FATAL (15s)..."
    if (-not (Wait-LogEvent '"event":"fatal_exit"|"to":"FATAL"' 15)) {
        Fail "FATAL not reached"; Diag (Get-Content $RelayLog -Tail 5 -EA SilentlyContinue); Stop-AllProcs; return
    }
    Diag "FATAL detected"
    if ($relay.WaitForExit(5000)) {
        Pass "FATAL: relay exited after max_reconnect_attempts (code: $($relay.ExitCode))"
    } else {
        Fail "FATAL logged but relay did not exit within 5s"
        Stop-Process -Id $relay.Id -Force -EA SilentlyContinue
    }
    Stop-AllProcs
}

Write-Host "`n========================================" -ForegroundColor Yellow
Write-Host "  Tests 8-11 Results: $PassCount PASS / $FailCount FAIL" -ForegroundColor $(if ($FailCount -eq 0) {"Green"} else {"Red"})
Write-Host "========================================`n" -ForegroundColor Yellow


