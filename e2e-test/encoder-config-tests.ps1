#requires -Version 7
# SpoutRelay エンコーダー設定バリエーション E2E テストスクリプト (pwsh 7+ 必須)
# 使い方: cd e2e-test; pwsh ./encoder-config-tests.ps1
#
# config.json の encoder/spout セクションの値を変えた構成 (h264_mf 直接指定、
# 極端なビットレート、max_b_frames、prefer_dx11:false 等) で spout-relay が
# 正常に STREAMING に到達し、RTSP ストリームを配信できることを検証する。

$ErrorActionPreference = "Continue"
$Root       = Split-Path -Parent $PSScriptRoot
$BuildDir   = "$Root\build"
$TestDir    = $PSScriptRoot
$CaptureDir = "$TestDir\capture"
$LogDir     = "$TestDir\logs"
$ConfigDir  = "$TestDir\config"
$RTSP       = "rtsp://127.0.0.1:8554/spout-e2e"
$RelayLog   = "$LogDir\spoutrelay-e2e-test.jsonl"
$TmpConfig  = "$ConfigDir\config_encodercfg_tmp.json"

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
    param([int]$TimeoutSec = 30)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $RelayLog) {
            $content = Get-Content $RelayLog -Raw -EA SilentlyContinue
            if ($content -match '"event":"publish_started"') {
                Start-Sleep -Milliseconds 400
                return $true
            }
        }
        Start-Sleep -Milliseconds 700
    }
    return $false
}

# config.json をベースに encoder/spout の値を上書きした設定ファイルを生成する
function New-EncoderTestConfig {
    param([hashtable]$EncoderOverrides = @{}, [hashtable]$SpoutOverrides = @{})

    $base = Get-Content "$ConfigDir\config.json" -Raw | ConvertFrom-Json
    foreach ($k in $EncoderOverrides.Keys) { $base.encoder.$k = $EncoderOverrides[$k] }
    foreach ($k in $SpoutOverrides.Keys)   { $base.spout.$k   = $SpoutOverrides[$k] }
    $base | ConvertTo-Json -Depth 10 | Set-Content -Path $TmpConfig -Encoding UTF8
}

# 設定を起動し、publish_started まで到達して RTSP ストリームを取得できるか検証する
function Test-EncoderConfig {
    param([string]$Name)

    Clear-RelayLog
    $mtx    = Start-Mediamtx
    $sender = Start-Process -FilePath "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSender" -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
    $relay  = Start-Process -FilePath "$BuildDir\spout-relay.exe" -ArgumentList "--config",$TmpConfig -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden

    if (-not (Wait-Stream 30)) {
        Fail "$Name : stream did not start within 30s"
        Diag "Relay log tail: $(Get-Content $RelayLog -Tail 8 -EA SilentlyContinue)"
        Stop-AllProcs
        return
    }

    $r = & ffprobe -v error -rtsp_transport tcp -i $RTSP -show_entries stream=codec_type -of csv 2>&1
    if ($r -match "video") {
        Pass "$Name : reached STREAMING and RTSP stream is playable"
    } else {
        Fail "$Name : publish_started logged but RTSP stream is not playable: $r"
    }

    Stop-AllProcs
}

# -----------------------------------------------------------------------
Write-Host "`n========================================" -ForegroundColor Yellow
Write-Host "  SpoutRelay Encoder Config Variation Tests" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Yellow

Stop-AllProcs
Remove-Item "$CaptureDir\*" -Force -EA SilentlyContinue
New-Item -ItemType Directory -Force $CaptureDir | Out-Null
New-Item -ItemType Directory -Force $LogDir     | Out-Null

# -----------------------------------------------------------------------
Write-Host "--- Test 1: h264_mf codec directly (no fallback) ---"
New-EncoderTestConfig -EncoderOverrides @{ codec = "h264_mf"; fallback_codec = "" }
Test-EncoderConfig -Name "h264_mf direct"

# -----------------------------------------------------------------------
Write-Host "`n--- Test 2: extremely low bitrate (100 kbps) ---"
New-EncoderTestConfig -EncoderOverrides @{ bitrate_kbps = 100 }
Test-EncoderConfig -Name "bitrate_kbps=100"

# -----------------------------------------------------------------------
Write-Host "`n--- Test 3: extremely high bitrate (50000 kbps) ---"
New-EncoderTestConfig -EncoderOverrides @{ bitrate_kbps = 50000 }
Test-EncoderConfig -Name "bitrate_kbps=50000"

# -----------------------------------------------------------------------
Write-Host "`n--- Test 4: max_b_frames=2 with h264_mf ---"
New-EncoderTestConfig -EncoderOverrides @{ codec = "h264_mf"; fallback_codec = ""; max_b_frames = 2 }
Test-EncoderConfig -Name "max_b_frames=2 (h264_mf)"

# -----------------------------------------------------------------------
Write-Host "`n--- Test 5: prefer_dx11=false ---"
New-EncoderTestConfig -SpoutOverrides @{ prefer_dx11 = $false }
Test-EncoderConfig -Name "prefer_dx11=false"

# -----------------------------------------------------------------------
Write-Host "`n--- Test 6: low fps (5) ---"
New-EncoderTestConfig -EncoderOverrides @{ fps = 5 }
Test-EncoderConfig -Name "fps=5"

# -----------------------------------------------------------------------
Write-Host "`n--- Test 7: high fps (60) ---"
New-EncoderTestConfig -EncoderOverrides @{ fps = 60 }
Test-EncoderConfig -Name "fps=60"

Remove-Item $TmpConfig -Force -EA SilentlyContinue

# -----------------------------------------------------------------------
Write-Host "`n========================================"  -ForegroundColor Yellow
Write-Host "  Encoder Config Results: $PassCount PASS  /  $FailCount FAIL" -ForegroundColor $(if ($FailCount -eq 0) { "Green" } else { "Red" })
Write-Host "========================================`n" -ForegroundColor Yellow

if ($FailCount -gt 0) { exit 1 } else { exit 0 }
