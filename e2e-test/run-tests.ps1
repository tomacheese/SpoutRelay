#requires -Version 7
# SpoutRelay E2E test script (requires pwsh 7+)
# Usage: cd e2e-test; pwsh ./run-tests.ps1

$ErrorActionPreference = "Continue"
$Root       = Split-Path -Parent $PSScriptRoot
$BuildDir   = "$Root\build"
$TestDir    = $PSScriptRoot
$CaptureDir = "$TestDir\capture"
$LogDir     = "$TestDir\logs"
$RTSP       = "rtsp://127.0.0.1:8554/spout-e2e"
# The relay starts with WorkingDirectory=$TestDir, so it writes under the logs/ subdirectory
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

# Start mediamtx from the e2e-test directory (so it reads mediamtx.yml)
function Start-Mediamtx {
    $p = Start-Process mediamtx -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 1200
    return $p
}

# Delete the relay log before each test so only new entries are seen
function Clear-RelayLog {
    Remove-Item $RelayLog -Force -EA SilentlyContinue
}

# Wait for the stream to start:
#   1) wait until publish_started appears in the relay log (more reliable than ffprobe)
#   2) also try ffprobe as a fallback if that does not work
function Wait-Stream {
    param([int]$TimeoutSec = 35)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        # --- primary: relay log ---
        if (Test-Path $RelayLog) {
            $content = Get-Content $RelayLog -Raw -EA SilentlyContinue
            if ($content -match '"event":"publish_started"') {
                Start-Sleep -Milliseconds 400   # wait briefly until mediamtx exposes the path
                return $true
            }
        }
        # --- fallback: ffprobe ---
        $r = & ffprobe -v error -rtsp_transport tcp -i $RTSP `
             -show_entries stream=codec_type -of csv 2>&1
        if ($r -match "video") { return $true }
        Diag "  (no stream yet, retrying...)"
        Start-Sleep -Milliseconds 700
    }
    return $false
}

function Count-Keyframes {
    param([int]$Seconds = 5)
    # -show_frames reports decoded frame info, so I-frames are detected accurately
    # even when RTSP packets carry no keyframe flag.
    # Detected when the pict_type column is "I" (not valid for interlaced, but fine for H.264)
    # or the key_frame column (4th csv field) is 1.
    $out = & ffprobe -v quiet -rtsp_transport tcp -select_streams v:0 `
           -read_intervals "%+$Seconds" -show_frames -of csv $RTSP 2>&1
    return ($out -split "`n" | Where-Object { $_ -match "^frame,video" -and $_ -match ",1," }).Count
}

# Detect that the relay reconnected after mediamtx restarted.
# Waits until the RECONNECTING_OUTPUT->STREAMING event count in the relay log exceeds $BaseCount.
# (handle_reconnecting_output() does not re-emit publish_started, so this event is used)
#
# Usage:
#   $base = Get-ReconnectCount   # get the count before the kill
#   # ... kill and restart mediamtx ...
#   Wait-Reconnect -BaseCount $base   # wait for a new reconnect after the kill
function Get-ReconnectCount {
    if (-not (Test-Path $RelayLog)) { return 0 }
    $lines = Get-Content $RelayLog -EA SilentlyContinue
    return ($lines | Where-Object {
        $_ -match '"event":"state_changed"' -and
        $_ -match '"from":"RECONNECTING_OUTPUT"' -and
        $_ -match '"to":"STREAMING"'
    }).Count
}

function Wait-Reconnect {
    param([int]$BaseCount = 0, [int]$TimeoutSec = 20)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ((Get-ReconnectCount) -gt $BaseCount) {
            Start-Sleep -Milliseconds 300
            return $true
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

# -----------------------------------------------------------------------
Write-Host "`n========================================" -ForegroundColor Yellow
Write-Host "  SpoutRelay E2E Tests" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Yellow

Stop-AllProcs
Remove-Item "$CaptureDir\*" -Force -EA SilentlyContinue
New-Item -ItemType Directory -Force $CaptureDir | Out-Null
New-Item -ItemType Directory -Force $LogDir     | Out-Null
New-Item -ItemType Directory -Force "$TestDir\state" | Out-Null

# -----------------------------------------------------------------------
# Test 1 - Bug #3: is an IDR sent right after RTSP reconnect?
# -----------------------------------------------------------------------
Write-Host "--- Test 1: IDR after RTSP reconnect (Bug #3) ---"

Clear-RelayLog
$mtx    = Start-Mediamtx
$sender = Start-Process -FilePath "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800
$relay  = Start-Process -FilePath "$BuildDir\spout-relay.exe" -ArgumentList "--config","$TestDir\config\config.json" -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden

Diag "mediamtx PID=$($mtx.Id)  sender PID=$($sender.Id)  relay PID=$($relay.Id)"
Info "Waiting for RTSP stream (up to 35s)..."

if (-not (Wait-Stream 35)) {
    Fail "Stream did not start — check if spout_test_sender and SpoutRelay are working"
    Diag "mediamtx running: $(-not $mtx.HasExited)  sender running: $(-not $sender.HasExited)  relay running: $(-not $relay.HasExited)"
    if (Test-Path $RelayLog) { Diag "Relay log tail: $(Get-Content $RelayLog -Tail 3 -EA SilentlyContinue)" }
    Stop-AllProcs
} else {
    Info "Stream live! Stabilising 5s..."
    Start-Sleep -Seconds 5

    # Recording the count before the kill clearly separates reconnects before the kill
    # (e.g. STALLED timeout) from the one after it
    $baseReconnects = Get-ReconnectCount

    Info "Killing mediamtx to force RTSP disconnect..."
    Stop-Process -Id $mtx.Id -Force -EA SilentlyContinue
    Start-Sleep -Seconds 2

    Info "Restarting mediamtx..."
    $mtx = Start-Mediamtx

    Info "Waiting for relay to reconnect (up to 20s)..."
    if (-not (Wait-Reconnect -BaseCount $baseReconnects -TimeoutSec 20)) {
        Diag "Warning: relay reconnect not confirmed via log — proceeding with ffprobe check"
    }

    Info "Checking keyframes in first 4s after reconnect..."
    $kf = Count-Keyframes -Seconds 4
    Diag "Keyframes in first 4s: $kf"

    if ($kf -ge 1) {
        Pass "IDR detected within 4s of reconnect ($kf I-frames)"
    } else {
        Fail "No keyframe in first 4s after reconnect"
    }

    Stop-AllProcs
}

# -----------------------------------------------------------------------
# Test 2 - Bug #5': the stream resumes immediately with the freeze frame after reconnect
# -----------------------------------------------------------------------
Write-Host "`n--- Test 2: Freeze frame preserved across RTSP reconnect (Bug #5') ---"
Write-Host "  [Scope] STREAMING->RTSP_ERROR->RECONNECTING path" -ForegroundColor DarkGray

Clear-RelayLog
$mtx    = Start-Mediamtx
$sender = Start-Process -FilePath "$BuildDir\spout_static_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800
$relay  = Start-Process -FilePath "$BuildDir\spout-relay.exe" -ArgumentList "--config","$TestDir\config\config.json" -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden

Info "Waiting for stream (up to 35s)..."
if (-not (Wait-Stream 35)) {
    Fail "Stream did not start — skipping Test 2"
    Stop-AllProcs
} else {
    Info "Stream live. Waiting 12s (static sender stops sending at 10s)..."
    Start-Sleep -Seconds 12

    $baseReconnects2 = Get-ReconnectCount

    Info "Forcing RTSP reconnect via mediamtx restart..."
    Stop-Process -Id $mtx.Id -Force -EA SilentlyContinue
    Start-Sleep -Seconds 2
    $mtx = Start-Mediamtx

    Info "Waiting for relay to reconnect after mediamtx restart (up to 20s)..."
    if (-not (Wait-Reconnect -BaseCount $baseReconnects2 -TimeoutSec 20)) {
        Diag "Warning: relay reconnect not confirmed via log — proceeding with ffprobe check"
    }

    Info "Checking for video within 5s of reconnect..."
    $kf2 = Count-Keyframes -Seconds 5
    Diag "Keyframes in first 5s after reconnect: $kf2"

    if ($kf2 -ge 1) {
        Pass "Video resumed immediately after reconnect — freeze frame preserved ($kf2 keyframes)"
    } else {
        Fail "No video in 5s after reconnect — freeze frame may have been lost"
    }

    Stop-AllProcs
}

# -----------------------------------------------------------------------
# Test 3 - Bug #7: does the stream stay stable over a long run?
# (FFmpeg's RTSP/RTP muxer handles RTP timestamp uint32 wraparound itself, so
#  no manual frame_count reset is needed. Confirms 15 s of error-free streaming.)
# -----------------------------------------------------------------------
Write-Host "`n--- Test 3: Stream stability / no encoder errors over 15s (Bug #7) ---"

Clear-RelayLog
$mtx    = Start-Mediamtx
$sender = Start-Process -FilePath "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800
$relay  = Start-Process -FilePath "$BuildDir\spout-relay.exe" -ArgumentList "--config","$TestDir\config\config.json" -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden

Info "Waiting for stream..."
if (-not (Wait-Stream 35)) {
    Fail "Stream did not start — skipping Test 3"
    Stop-AllProcs
} else {
    Info "Recording 15s to verify stream stability..."
    $cap = Start-Process ffmpeg -ArgumentList "-y","-rtsp_transport","tcp","-i",$RTSP,"-t","15","-c","copy","$CaptureDir\test3.mp4" -PassThru -WindowStyle Hidden
    # Wait for ffmpeg to exit on its own (RTSP connect + 15s recording + moov write = up to 25s)
    $cap.WaitForExit(25000) | Out-Null
    $cap | Stop-Process -Force -EA SilentlyContinue  # safety net on timeout

    $dur = (& ffprobe -v quiet -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "$CaptureDir\test3.mp4" 2>&1) -as [double]

    # Check the relay JSONL log for encoder/RTSP errors
    $hasError = $false
    if (Test-Path $RelayLog) {
        $logText  = Get-Content $RelayLog -Raw -EA SilentlyContinue
        $hasError = $logText -match '"event":"error","code":"(ENCODER_ENCODE_FAILED|RTSP_SEND_FAILED)'
    }

    Diag "Captured: $([math]::Round($dur,1))s | Encoder/RTSP errors: $hasError"

    if ($dur -ge 13.0 -and -not $hasError) {
        Pass "Stream stable: ran $([math]::Round($dur,1))s without encoder or RTSP errors"
    } elseif ($dur -ge 13.0) {
        Fail "Stream ran $([math]::Round($dur,1))s but encoder/RTSP errors detected in log"
    } else {
        Fail "Stream duration too short: $([math]::Round($dur,1))s (expected >=13s)"
    }

    Stop-AllProcs
}

# -----------------------------------------------------------------------
Write-Host "`n========================================"
$color = if ($FailCount -eq 0) { "Green" } else { "Red" }
Write-Host "  Results: $PassCount PASS  /  $FailCount FAIL" -ForegroundColor $color
Write-Host "========================================`n"

$files = Get-ChildItem $CaptureDir -Filter "*.mp4" -EA SilentlyContinue
if ($files) {
    Write-Host "Captured files (open for manual inspection):"
    $files | ForEach-Object { Write-Host "  $($_.FullName)" }
}

if ($FailCount -gt 0) { exit 1 } else { exit 0 }
