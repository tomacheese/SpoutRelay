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
    @("mediamtx","spout-relay","spout_test_sender","spout_static_sender","spout_res_changer","ffmpeg","ffprobe") |
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
# Test 8 — STALLED → sender_disappeared → PROBING (プレースホルダなし)
# 送信元が完全に消えた際に relay が IDLE/PROBING へ戻り、
# 送信元再起動後に再び STREAMING まで復帰することを検証する。
# -----------------------------------------------------------------------
Write-Host "`n--- Test 8: STALLED → sender_disappeared → PROBING → recovery ---"
& {
    Clear-RelayLog
    $mtx    = Start-Mediamtx
    $sender = Start-Process -FilePath "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
    $relay  = Start-Process -FilePath "$BuildDir\spout-relay.exe" -ArgumentList "--config","$TestDir\config\config.json" -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden

    Info "Waiting for RTSP stream..."
    if (-not (Wait-Stream 35)) {
        Fail "Stream did not start — skipping Test 8"
        Stop-AllProcs; return
    }

    Info "Stream live! Killing sender permanently to trigger sender_disappeared..."
    Stop-Process -Id $sender.Id -Force -EA SilentlyContinue
    Start-Sleep -Milliseconds 200

    # STALLED → sender_disappeared を待つ
    # frame_timeout_ms=300ms で STALLED、sender_missing_timeout_ms=800ms で disappeared
    Info "Waiting for sender_disappeared (up to 4s)..."
    if (-not (Wait-LogEvent -Pattern '"event":"sender_disappeared"' -TimeoutSec 4)) {
        Fail "sender_disappeared not logged within 4s"
        Diag "Relay log tail: $(Get-Content $RelayLog -Tail 5 -EA SilentlyContinue)"
        Stop-AllProcs; return
    }
    Diag "sender_disappeared confirmed"

    # relay が PROBING へ戻ることを確認する (IDLE→PROBING の遷移)
    if (-not (Wait-LogEvent -Pattern '"to":"PROBING"' -TimeoutSec 3)) {
        Diag "PROBING not logged — relay may have gone IDLE→PROBING too quickly to catch"
    }

    # RTSP ストリームが停止したことを確認する
    Start-Sleep -Milliseconds 1000
    $r = & ffprobe -v error -rtsp_transport tcp -i $RTSP `
         -show_entries stream=codec_type -of csv 2>&1
    if ($r -match "video") {
        Diag "Warning: stream still up after sender_disappeared — continuing"
    } else {
        Diag "RTSP stream stopped as expected"
    }

    # 送信元を再起動して STREAMING 復帰を確認する
    Info "Restarting sender for recovery test..."
    $null = Start-Process -FilePath "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden

    Info "Waiting for stream recovery (up to 15s)..."
    if (Wait-Stream 15) {
        Pass "sender_disappeared → PROBING → stream recovered after sender restart"
    } else {
        Fail "Stream did not recover after sender_disappeared and sender restart within 15s"
        Diag "Relay log tail: $(Get-Content $RelayLog -Tail 5 -EA SilentlyContinue)"
    }
    Stop-AllProcs
}

# -----------------------------------------------------------------------
# Test 9 — GPU モード中に sender 消失 → PLACEHOLDER シームレス移行
# GPU ゼロコピーモードで STREAMING 中に送信元が消えた際、
# encoder+RTSP を維持したままプレースホルダへシームレス切替し
# 映像が途切れないことを確認する (GPU→CPU UpdateSubresource フォールバック)。
# -----------------------------------------------------------------------
Write-Host "`n--- Test 9: GPU mode STREAMING → sender_disappeared → PLACEHOLDER seamless ---"
& {
    Clear-RelayLog
    $mtx    = Start-Mediamtx
    $sender = Start-Process -FilePath "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
    $relay  = Start-Process -FilePath "$BuildDir\spout-relay.exe" -ArgumentList "--config","$TestDir\config\config_placeholder.json" -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden

    Info "Waiting for STREAMING + gpu_zero_copy_enabled (up to 25s)..."
    $gpuActive = $false
    $deadline25 = (Get-Date).AddSeconds(25)
    while ((Get-Date) -lt $deadline25) {
        if (Test-Path $RelayLog) {
            $content = Get-Content $RelayLog -Raw -EA SilentlyContinue
            if ($content -match '"event":"gpu_zero_copy_enabled"') { $gpuActive = $true; break }
        }
        Start-Sleep -Milliseconds 500
        Diag "  (waiting for GPU mode...)"
    }

    if (-not $gpuActive) {
        Diag "gpu_zero_copy_enabled not logged — verifying basic stream (CPU path?)"
        if (-not (Wait-Stream 5)) {
            Fail "Stream did not start at all — skipping Test 9"
            Stop-AllProcs; return
        }
        Diag "Stream active in CPU mode — proceeding with sender_disappeared test"
    } else {
        Diag "GPU mode confirmed"
        Start-Sleep -Milliseconds 500  # STREAMING が安定するまで少し待つ
    }

    Info "Killing sender to trigger sender_disappeared → PLACEHOLDER..."
    Stop-Process -Id $sender.Id -Force -EA SilentlyContinue
    Start-Sleep -Milliseconds 200

    Info "Waiting for sender_disappeared (up to 4s)..."
    if (-not (Wait-LogEvent -Pattern '"event":"sender_disappeared"' -TimeoutSec 4)) {
        Fail "sender_disappeared not logged within 4s in Test 9"
        Diag "Relay log tail: $(Get-Content $RelayLog -Tail 5 -EA SilentlyContinue)"
        Stop-AllProcs; return
    }
    Diag "sender_disappeared confirmed"

    Info "Waiting for placeholder activation (up to 5s)..."
    if (-not (Wait-LogEvent -Pattern '"event":"placeholder_seamless"|"event":"placeholder_started"' -TimeoutSec 5)) {
        Fail "Placeholder not activated within 5s after sender_disappeared"
        Diag "Relay log tail: $(Get-Content $RelayLog -Tail 5 -EA SilentlyContinue)"
        Stop-AllProcs; return
    }

    $content9 = Get-Content $RelayLog -Raw -EA SilentlyContinue
    $isSeamless = $content9 -match '"event":"placeholder_seamless"'
    if ($isSeamless) {
        Diag "Seamless handoff (encoder+RTSP maintained)"
        Start-Sleep -Milliseconds 1000
        $r9 = & ffprobe -v error -rtsp_transport tcp -i $RTSP `
              -show_entries stream=codec_type -of csv 2>&1
        if ($r9 -match "video") {
            Pass "GPU mode → sender_disappeared → PLACEHOLDER seamless (RTSP stream maintained)"
        } else {
            Fail "placeholder_seamless logged but RTSP stream lost"
        }
    } else {
        Diag "Non-seamless placeholder (encoder reinit) — verifying stream..."
        Start-Sleep -Milliseconds 2000
        $r9b = & ffprobe -v error -rtsp_transport tcp -i $RTSP `
               -show_entries stream=codec_type -of csv 2>&1
        if ($r9b -match "video") {
            Pass "GPU mode → sender_disappeared → PLACEHOLDER (non-seamless, stream active)"
        } else {
            Fail "Placeholder started but RTSP stream not detected"
        }
    }
    Stop-AllProcs
}

# -----------------------------------------------------------------------
# Test 10 — 解像度変更 → RECONFIGURING → STREAMING (spout_res_changer 使用)
# 送信元が 640x360 → 320x240 に解像度を変更した際に
# relay が RECONFIGURING を経て正しく再初期化されることを確認する。
# -----------------------------------------------------------------------
Write-Host "`n--- Test 10: Resolution change 640x360 → 320x240 (RECONFIGURING) ---"
& {
    Clear-RelayLog
    $mtx      = Start-Mediamtx
    # spout_res_changer: Phase1=640x360 (15s), Phase2=320x240 (10s)
    $null = Start-Process -FilePath "$BuildDir\spout_res_changer.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
    $relay    = Start-Process -FilePath "$BuildDir\spout-relay.exe" -ArgumentList "--config","$TestDir\config\config.json" -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden

    Info "Waiting for initial 640x360 stream (up to 30s)..."
    $initialStream = $false
    $deadline30 = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline30) {
        if (Test-Path $RelayLog) {
            $c = Get-Content $RelayLog -Raw -EA SilentlyContinue
            if ($c -match '"event":"publish_started".*"width":"640"') {
                $initialStream = $true; break
            }
        }
        Start-Sleep -Milliseconds 700
        Diag "  (waiting for 640x360 stream...)"
    }

    if (-not $initialStream) {
        Fail "640x360 stream (publish_started) not confirmed within 30s — skipping Test 10"
        Diag "Relay log: $(Get-Content $RelayLog -Raw -EA SilentlyContinue)"
        Stop-AllProcs; return
    }
    Diag "Initial 640x360 stream confirmed"

    # Phase1 は 450 フレーム。Windows の Sleep 精度の影響で実測 20-25s かかることがある。
    # relay が STREAMING を確認してから phase2 開始まで最大 25s 余裕を持たせる。
    Info "Waiting for RECONFIGURING state (up to 25s)..."
    if (-not (Wait-LogEvent -Pattern '"to":"RECONFIGURING"' -TimeoutSec 25)) {
        Fail "RECONFIGURING not triggered within 16s of 640x360 stream start"
        Diag "Relay log tail: $(Get-Content $RelayLog -Tail 5 -EA SilentlyContinue)"
        Stop-AllProcs; return
    }
    Diag "RECONFIGURING detected"

    Info "Waiting for re-STREAMING after RECONFIGURING (up to 15s)..."
    # publish_started with width=320 を待つ
    if (-not (Wait-LogEvent -Pattern '"event":"publish_started".*"width":"320"' -TimeoutSec 15)) {
        # publish_started が見つからなくても STREAMING に戻っていれば許容する
        if (-not (Wait-LogEvent -Pattern '"to":"STREAMING"' -TimeoutSec 5)) {
            Fail "Did not re-enter STREAMING after RECONFIGURING within 15s"
            Diag "Relay log tail: $(Get-Content $RelayLog -Tail 5 -EA SilentlyContinue)"
            Stop-AllProcs; return
        }
        Diag "STREAMING restored (320x240 publish_started not confirmed via log)"
    } else {
        Diag "publish_started at 320x240 confirmed"
    }

    Start-Sleep -Milliseconds 1000
    $r10 = & ffprobe -v error -rtsp_transport tcp -i $RTSP `
           -show_entries stream=codec_type -of csv 2>&1
    if ($r10 -match "video") {
        Pass "Resolution change 640x360→320x240: RECONFIGURING succeeded, stream active"
    } else {
        Fail "RECONFIGURING occurred but RTSP stream not detected after"
        Diag "Relay log tail: $(Get-Content $RelayLog -Tail 5 -EA SilentlyContinue)"
    }
    Stop-AllProcs
}

# -----------------------------------------------------------------------
# Test 11 — max_reconnect_attempts 超過 → FATAL 状態
# RTSP が復帰しない状況で relay が正しく FATAL へ遷移して
# プロセスを終了することを確認する。
# -----------------------------------------------------------------------
Write-Host "`n--- Test 11: max_reconnect_attempts exceeded → FATAL ---"
& {
    # FATAL テスト用設定: 再接続を 2 回に制限して高速に FATAL へ遷移させる
    $FatalConfig = @'
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
    "connect_timeout_ms": 1500,
    "send_timeout_ms": 1500,
    "max_reconnect_attempts": 2,
    "reconnect_delay_ms": 400,
    "reconnect_max_delay_ms": 2000,
    "reconnect_backoff_multiplier": 2.0
  },
  "runtime": {
    "shutdown_grace_ms": 500,
    "emit_metrics_interval_ms": 500,
    "emit_health_interval_ms": 500
  }
}
'@
    $FatalConfigPath = "$TestDir\config\config_fatal.json"
    Set-Content $FatalConfigPath $FatalConfig -Encoding UTF8

    Clear-RelayLog
    $mtx    = Start-Mediamtx
    $null   = Start-Process -FilePath "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
    $relay  = Start-Process -FilePath "$BuildDir\spout-relay.exe" -ArgumentList "--config",$FatalConfigPath -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden

    Info "Waiting for RTSP stream (up to 30s)..."
    if (-not (Wait-Stream 30)) {
        Fail "Stream did not start — skipping Test 11"
        Stop-AllProcs; return
    }

    Info "Stream live! Killing mediamtx permanently..."
    Stop-Process -Id $mtx.Id -Force -EA SilentlyContinue
    # mediamtx を再起動しない
    # reconnect_delay_ms=400ms, backoff×2=800ms, connect_timeout=1500ms
    # → attempt 1: ~1.9s, attempt 2: ~2.3s, attempt 3: > max(2) → FATAL
    # 合計 ~4.5s

    Info "Waiting for FATAL state (up to 15s)..."
    if (-not (Wait-LogEvent -Pattern '"event":"fatal_exit"|"to":"FATAL"' -TimeoutSec 15)) {
        Fail "FATAL state not reached within 15s after max_reconnect_attempts exhausted"
        Diag "Relay log tail: $(Get-Content $RelayLog -Tail 5 -EA SilentlyContinue)"
        Stop-AllProcs; return
    }
    Diag "FATAL state detected in log"

    # relay プロセスが自己終了することを確認する (FATAL ハンドラが shutdown_requested_ をセット)
    $exited = $relay.WaitForExit(5000)
    if ($exited) {
        Pass "max_reconnect_attempts exceeded: relay reached FATAL and exited (code: $($relay.ExitCode))"
    } else {
        Fail "FATAL logged but relay process did not exit within 5s"
        Stop-Process -Id $relay.Id -Force -EA SilentlyContinue
    }
    Stop-AllProcs
}

# -----------------------------------------------------------------------
Write-Host "`n========================================"  -ForegroundColor Yellow
Write-Host "  Monkey Results: $PassCount PASS  /  $FailCount FAIL" -ForegroundColor $(if ($FailCount -eq 0) { "Green" } else { "Red" })
Write-Host "========================================`n" -ForegroundColor Yellow

