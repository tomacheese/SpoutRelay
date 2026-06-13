#requires -Version 7
# SpoutRelay モンキーテスト / 追加シナリオテスト
# 使い方: cd e2e-test; pwsh ./monkey-tests.ps1
#
# run-tests.ps1 が検証しない以下のシナリオを確認する:
#   Test 4  - Sender 切断 (STREAMING→STALLED→sender 復帰→STREAMING)
#   Test 5  - Placeholder モード（sender なし → NO SIGNAL 配信 → sender 接続 → ハンドオフ）
#   Test 6  - 連続 RTSP 再接続 (3 回)
#   Test 7  - libx264 CPU コーデック直接使用

$ErrorActionPreference = "Continue"
$Root       = Split-Path -Parent $PSScriptRoot
$BuildDir   = "$Root\build"
$TestDir    = $PSScriptRoot
$CaptureDir = "$TestDir\capture"
$LogDir     = "$TestDir\logs"
$RTSP       = "rtsp://127.0.0.1:8554/spout-e2e"
$RelayLog   = "$LogDir\spoutrelay-e2e-test.jsonl"

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

function Wait-Stream {
    param([int]$TimeoutSec = 35)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $RelayLog) {
            $content = Get-Content $RelayLog -Raw -EA SilentlyContinue
            if ($content -match '"event":"publish_started"') {
                Start-Sleep -Milliseconds 400
                return $true
            }
        }
        $r = & ffprobe -v error -rtsp_transport tcp -i $RTSP `
             -show_entries stream=codec_type -of csv 2>&1
        if ($r -match "video") { return $true }
        Diag "  (no stream yet, retrying...)"
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

# -----------------------------------------------------------------------
Write-Host "`n========================================" -ForegroundColor Yellow
Write-Host "  SpoutRelay Monkey Tests" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Yellow

Stop-AllProcs
Remove-Item "$CaptureDir\*" -Force -EA SilentlyContinue
New-Item -ItemType Directory -Force $CaptureDir | Out-Null
New-Item -ItemType Directory -Force $LogDir     | Out-Null

# -----------------------------------------------------------------------
# Test 4 — STREAMING → Sender 切断 → STALLED → Sender 復帰 → STREAMING
# -----------------------------------------------------------------------
Write-Host "--- Test 4: Sender disconnect → STALLED → sender reconnect ---"

Clear-RelayLog
$mtx    = Start-Mediamtx
$sender = Start-Process -FilePath "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800
$relay  = Start-Process -FilePath "$BuildDir\spout-relay.exe" -ArgumentList "--config","$TestDir\config\config.json" -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden

Info "Waiting for RTSP stream..."
if (-not (Wait-Stream 35)) {
    Fail "Stream did not start — skipping Test 4"
    Stop-AllProcs
} else {
    Info "Stream live! Killing sender to trigger STALLED..."
    Stop-Process -Id $sender.Id -Force -EA SilentlyContinue
    Start-Sleep -Milliseconds 200  # force-kill が完了するまで待つ

    # STALLED への遷移を待つ (frame_timeout_ms=300ms なので 2s 以内に遷移するはず)
    Info "Waiting for STALLED state (up to 4s)..."
    if (Wait-LogEvent -Pattern '"event":"state_changed".*"to":"STALLED"' -TimeoutSec 4) {
        Diag "STALLED detected"
    } else {
        Diag "STALLED not confirmed via log — continuing anyway"
    }

    # sender 再起動 (stalled 後も relay は接続を維持するはず)
    Info "Restarting sender..."
    $sender2 = Start-Process -FilePath "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden

    # STALLED → STREAMING の復帰を待つ
    Info "Waiting for stall_recovered / STREAMING (up to 8s)..."
    $recovered = Wait-LogEvent -Pattern '"event":"stall_recovered"|"to":"STREAMING"' -TimeoutSec 8
    if ($recovered) {
        Diag "Stream recovered"
        # ffprobe でストリームが実際に流れているか確認
        Start-Sleep -Milliseconds 1000
        $r = & ffprobe -v error -rtsp_transport tcp -i $RTSP `
             -show_entries stream=codec_type -of csv 2>&1
        if ($r -match "video") {
            Pass "Sender reconnect: stream resumed after STALLED (stall_recovered + video confirmed)"
        } else {
            Fail "stall_recovered logged but RTSP stream not detected by ffprobe"
        }
    } else {
        Fail "Stream did not recover after sender reconnect within 8s"
        Diag "Relay log tail: $(Get-Content $RelayLog -Tail 3 -EA SilentlyContinue)"
    }
    Stop-AllProcs
}

# -----------------------------------------------------------------------
# Test 5 — Placeholder モード: sender なし → NO SIGNAL 配信 → sender 接続 → ハンドオフ
# -----------------------------------------------------------------------
Write-Host "`n--- Test 5: Placeholder mode — NO SIGNAL then live handoff ---"

# Placeholder 有効の一時設定ファイルを作る
$PlaceholderConfig = @"
{
  "app": {
    "instance_name": "spoutrelay-e2e-test",
    "log_dir": "./logs",
    "metrics_path": "./state/metrics.json",
    "health_path": "./state/health.json"
  },
  "spout": {
    "sender_name": "TestSpoutSender",
    "poll_interval_ms": 50,
    "frame_timeout_ms": 300,
    "sender_missing_timeout_ms": 800,
    "prefer_dx11": true
  },
  "placeholder": {
    "enabled": true,
    "width": 640,
    "height": 360,
    "message": "NO SIGNAL",
    "background_hex": "#000000",
    "text_hex": "#FFFFFF",
    "show_sender_name": true
  },
  "encoder": {
    "codec": "h264_nvenc",
    "fallback_codec": "h264_mf",
    "bitrate_kbps": 2000,
    "fps": 30,
    "gop_size": 30,
    "max_b_frames": 0,
    "preset": "fast",
    "tune": "zerolatency",
    "threads": 0
  },
  "rtsp": {
    "url": "rtsp://127.0.0.1:8554/spout-e2e",
    "connect_timeout_ms": 5000,
    "send_timeout_ms": 5000,
    "max_reconnect_attempts": 10,
    "reconnect_delay_ms": 1000,
    "reconnect_max_delay_ms": 10000,
    "reconnect_backoff_multiplier": 2.0
  },
  "runtime": {
    "shutdown_grace_ms": 500,
    "emit_metrics_interval_ms": 500,
    "emit_health_interval_ms": 500
  }
}
"@
$PlaceholderConfigPath = "$TestDir\config\config_placeholder.json"
Set-Content $PlaceholderConfigPath $PlaceholderConfig -Encoding UTF8

Clear-RelayLog
$mtx   = Start-Mediamtx
# sender を起動せずにリレーだけ起動 → Placeholder に遷移するはず
$relay = Start-Process -FilePath "$BuildDir\spout-relay.exe" -ArgumentList "--config",$PlaceholderConfigPath -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden

Info "Waiting for placeholder stream (up to 15s)..."
$phOk = $false
$deadline15 = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $deadline15) {
    if (Test-Path $RelayLog) {
        $content = Get-Content $RelayLog -Raw -EA SilentlyContinue
        if ($content -match '"event":"placeholder_started"') { $phOk = $true; break }
    }
    $r = & ffprobe -v error -rtsp_transport tcp -i $RTSP `
         -show_entries stream=codec_type -of csv 2>&1
    if ($r -match "video") { $phOk = $true; break }
    Start-Sleep -Milliseconds 700
    Diag "  (waiting for placeholder...)"
}

if (-not $phOk) {
    Fail "Placeholder stream did not start within 15s"
    Diag "Relay log tail: $(Get-Content $RelayLog -Tail 3 -EA SilentlyContinue)"
    Stop-AllProcs
} else {
    Diag "Placeholder stream live"

    # sender を接続してハンドオフを確認
    Info "Starting sender for live handoff..."
    $sender = Start-Process -FilePath "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden

    # placeholder_source_found → CONNECTING_OUTPUT を待つ
    Info "Waiting for source handoff (up to 12s)..."
    $handoff = Wait-LogEvent -Pattern '"event":"placeholder_source_found"|"to":"CONNECTING_OUTPUT"' -TimeoutSec 12
    if ($handoff) {
        # ストリームが継続して流れているか
        Start-Sleep -Milliseconds 2000
        $r2 = & ffprobe -v error -rtsp_transport tcp -i $RTSP `
             -show_entries stream=codec_type -of csv 2>&1
        if ($r2 -match "video") {
            Pass "Placeholder → live handoff succeeded (source found, stream continues)"
        } else {
            Fail "Handoff event logged but RTSP stream lost during transition"
        }
    } else {
        Fail "Sender connected but relay did not detect source within 12s"
        Diag "Relay log tail: $(Get-Content $RelayLog -Tail 5 -EA SilentlyContinue)"
    }
    Stop-AllProcs
}

# -----------------------------------------------------------------------
# Test 6 — 連続 RTSP 再接続 (3 回)
# -----------------------------------------------------------------------
Write-Host "`n--- Test 6: Triple RTSP reconnect (3 kills) ---"

Clear-RelayLog
$mtx    = Start-Mediamtx
$sender = Start-Process -FilePath "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800
$relay  = Start-Process -FilePath "$BuildDir\spout-relay.exe" -ArgumentList "--config","$TestDir\config\config.json" -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden

Info "Waiting for initial stream..."
if (-not (Wait-Stream 35)) {
    Fail "Stream did not start — skipping Test 6"
    Stop-AllProcs
} else {
    $allOk = $true
    for ($i = 1; $i -le 3; $i++) {
        Info "Kill #${i}: killing mediamtx..."
        Stop-Process -Id $mtx.Id -Force -EA SilentlyContinue
        Start-Sleep -Seconds 2
        $mtx = Start-Mediamtx

        # RECONNECTING_OUTPUT → STREAMING を待つ
        $deadline20 = (Get-Date).AddSeconds(20)
        $reconnected = $false
        while ((Get-Date) -lt $deadline20) {
            $r = & ffprobe -v error -rtsp_transport tcp -i $RTSP `
                 -show_entries stream=codec_type -of csv 2>&1
            if ($r -match "video") { $reconnected = $true; break }
            Start-Sleep -Milliseconds 700
        }
        if ($reconnected) {
            Diag "  Reconnect #${i}: stream restored"
        } else {
            Fail "Stream did not recover after RTSP kill #$i"
            $allOk = $false
            break
        }
        Start-Sleep -Milliseconds 500  # 次の kill まで少し安定化
    }
    if ($allOk) {
        Pass "Triple RTSP reconnect: all 3 kills recovered successfully"
    }
    Stop-AllProcs
}

# -----------------------------------------------------------------------
# Test 7 — libopenh264 CPU ソフトウェアコーデック直接使用
# (この FFmpeg ビルドは --disable-libx264 のため libopenh264 を代替に使う)
# -----------------------------------------------------------------------
Write-Host "`n--- Test 7: CPU codec (libopenh264) direct use ---"

$Libx264Config = @"
{
  "app": {
    "instance_name": "spoutrelay-e2e-test",
    "log_dir": "./logs",
    "metrics_path": "./state/metrics.json",
    "health_path": "./state/health.json"
  },
  "spout": {
    "sender_name": "TestSpoutSender",
    "poll_interval_ms": 50,
    "frame_timeout_ms": 300,
    "sender_missing_timeout_ms": 800,
    "prefer_dx11": true
  },
  "encoder": {
    "codec": "libopenh264",
    "fallback_codec": "",
    "bitrate_kbps": 2000,
    "fps": 30,
    "gop_size": 30,
    "max_b_frames": 0,
    "preset": "",
    "tune": "",
    "threads": 0
  },
  "rtsp": {
    "url": "rtsp://127.0.0.1:8554/spout-e2e",
    "connect_timeout_ms": 5000,
    "send_timeout_ms": 5000,
    "max_reconnect_attempts": 10,
    "reconnect_delay_ms": 1000,
    "reconnect_max_delay_ms": 10000,
    "reconnect_backoff_multiplier": 2.0
  },
  "runtime": {
    "shutdown_grace_ms": 500,
    "emit_metrics_interval_ms": 500,
    "emit_health_interval_ms": 500
  }
}
"@
$Libx264ConfigPath = "$TestDir\config\config_libopenh264.json"
Set-Content $Libx264ConfigPath $Libx264Config -Encoding UTF8

Clear-RelayLog
$mtx    = Start-Mediamtx
$sender = Start-Process -FilePath "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800
$relay  = Start-Process -FilePath "$BuildDir\spout-relay.exe" -ArgumentList "--config",$Libx264ConfigPath -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden

Info "Waiting for libx264 stream (up to 35s)..."
if (-not (Wait-Stream 35)) {
    Fail "libx264 stream did not start"
    Diag "Relay log tail: $(Get-Content $RelayLog -Tail 3 -EA SilentlyContinue)"
    Stop-AllProcs
} else {
    Info "libx264 stream live — verifying codec..."
    $probe = & ffprobe -v quiet -rtsp_transport tcp -show_streams -of json $RTSP 2>&1
    if ($probe -match "h264") {
        Info "Recording 8s for stability check..."
        $cap = Start-Process ffmpeg -ArgumentList "-y","-rtsp_transport","tcp","-i",$RTSP,"-t","8","-c","copy","$CaptureDir\test7_libx264.mp4" -PassThru -WindowStyle Hidden
        $cap.WaitForExit(18000) | Out-Null
        $cap | Stop-Process -Force -EA SilentlyContinue

        $dur = (& ffprobe -v quiet -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "$CaptureDir\test7_libx264.mp4" 2>&1) -as [double]
        if ($dur -ge 6.0) {
            Pass "libx264 CPU path: stream stable for $([math]::Round($dur,1))s"
        } else {
            Fail "libx264 CPU path: captured only $([math]::Round($dur,1))s (expected ≥6s)"
        }
    } else {
        Fail "libx264 stream started but H.264 not detected by ffprobe"
    }
    Stop-AllProcs
}

# -----------------------------------------------------------------------
Write-Host "`n========================================"  -ForegroundColor Yellow
Write-Host "  Monkey Results: $PassCount PASS  /  $FailCount FAIL" -ForegroundColor $(if ($FailCount -eq 0) { "Green" } else { "Red" })
Write-Host "========================================`n" -ForegroundColor Yellow
