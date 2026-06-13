#requires -Version 7
# SpoutRelay 複数インスタンス同時起動 E2E テストスクリプト (pwsh 7+ 必須)
# 使い方: cd e2e-test; pwsh ./multi-instance-tests.ps1
#
# instance_name / sender_name / rtsp.url / metrics_path / health_path を
# それぞれ変えた 2 つの spout-relay インスタンスを同一マシン上で同時に起動し、
# ログファイル・state ファイル・RTSP ストリームが互いに競合・混在しないことを検証する。

$ErrorActionPreference = "Continue"
$Root       = Split-Path -Parent $PSScriptRoot
$BuildDir   = "$Root\build"
$TestDir    = $PSScriptRoot
$CaptureDir = "$TestDir\capture"
$LogDir     = "$TestDir\logs"
$StateDir   = "$TestDir\state"
$ConfigDir  = "$TestDir\config"

$RtspA = "rtsp://127.0.0.1:8554/spout-e2e-multi-a"
$RtspB = "rtsp://127.0.0.1:8554/spout-e2e-multi-b"
$LogA  = "$LogDir\spoutrelay-e2e-multi-a.jsonl"
$LogB  = "$LogDir\spoutrelay-e2e-multi-b.jsonl"
$HealthA  = "$StateDir\health_a.json"
$HealthB  = "$StateDir\health_b.json"
$MetricsA = "$StateDir\metrics_a.json"
$MetricsB = "$StateDir\metrics_b.json"

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

function Wait-LogEvent {
    param([string]$Path, [string]$Pattern, [int]$TimeoutSec = 30)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $Path) {
            $content = Get-Content $Path -Raw -EA SilentlyContinue
            if ($content -match $Pattern) { return $true }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

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
            } catch {}
        }
        Start-Sleep -Milliseconds 300
    }
    return $null
}

# -----------------------------------------------------------------------
Write-Host "`n========================================" -ForegroundColor Yellow
Write-Host "  SpoutRelay Multi-Instance Tests" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Yellow

Stop-AllProcs
Remove-Item "$CaptureDir\*" -Force -EA SilentlyContinue
New-Item -ItemType Directory -Force $CaptureDir | Out-Null
New-Item -ItemType Directory -Force $LogDir     | Out-Null
New-Item -ItemType Directory -Force $StateDir   | Out-Null
Remove-Item $LogA, $LogB, $HealthA, $HealthB, $MetricsA, $MetricsB -Force -EA SilentlyContinue

# -----------------------------------------------------------------------
# インスタンス A / B 用の設定ファイルを生成
# instance_name / sender_name / rtsp.url / metrics_path / health_path を
# すべて変えることで、ログファイル・state ファイル・RTSP パスの競合を避ける。
# -----------------------------------------------------------------------
function New-MultiConfig {
    param([string]$InstanceName, [string]$SenderName, [string]$RtspUrl,
          [string]$MetricsPath, [string]$HealthPath, [string]$OutPath)

    $cfg = [ordered]@{
        app = [ordered]@{
            instance_name = $InstanceName
            log_dir        = "./logs"
            metrics_path   = $MetricsPath
            health_path    = $HealthPath
        }
        spout = [ordered]@{
            sender_name              = $SenderName
            poll_interval_ms         = 50
            frame_timeout_ms         = 300
            sender_missing_timeout_ms = 800
            prefer_dx11              = $true
        }
        encoder = [ordered]@{
            codec          = "h264_nvenc"
            fallback_codec = "h264_mf"
            bitrate_kbps   = 2000
            fps            = 30
            gop_size       = 30
            max_b_frames   = 0
            preset         = "fast"
            tune           = "zerolatency"
            threads        = 0
        }
        rtsp = [ordered]@{
            url                          = $RtspUrl
            connect_timeout_ms           = 5000
            send_timeout_ms              = 5000
            max_reconnect_attempts       = 10
            reconnect_delay_ms           = 1000
            reconnect_max_delay_ms       = 10000
            reconnect_backoff_multiplier = 2.0
        }
        runtime = [ordered]@{
            shutdown_grace_ms        = 500
            emit_metrics_interval_ms = 500
            emit_health_interval_ms  = 500
        }
    }
    $cfg | ConvertTo-Json -Depth 10 | Set-Content -Path $OutPath -Encoding UTF8
}

$ConfigA = "$ConfigDir\config_multi_a.json"
$ConfigB = "$ConfigDir\config_multi_b.json"
New-MultiConfig -InstanceName "spoutrelay-e2e-multi-a" -SenderName "TestSpoutSenderMultiA" `
    -RtspUrl $RtspA -MetricsPath "./state/metrics_a.json" -HealthPath "./state/health_a.json" -OutPath $ConfigA
New-MultiConfig -InstanceName "spoutrelay-e2e-multi-b" -SenderName "TestSpoutSenderMultiB" `
    -RtspUrl $RtspB -MetricsPath "./state/metrics_b.json" -HealthPath "./state/health_b.json" -OutPath $ConfigB

# -----------------------------------------------------------------------
# Test 1 — 2 つの spout-relay インスタンスを同時起動し、それぞれが独立して
#          STREAMING に到達すること
# -----------------------------------------------------------------------
Write-Host "--- Test 1: two spout-relay instances start independently ---"
$mtx = Start-Mediamtx
$senderA = Start-Process -FilePath "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSenderMultiA" -PassThru -WindowStyle Hidden
$senderB = Start-Process -FilePath "$BuildDir\spout_test_sender.exe" -ArgumentList "TestSpoutSenderMultiB" -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800

$relayA = Start-Process -FilePath "$BuildDir\spout-relay.exe" -ArgumentList "--config",$ConfigA -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden
$relayB = Start-Process -FilePath "$BuildDir\spout-relay.exe" -ArgumentList "--config",$ConfigB -WorkingDirectory $TestDir -PassThru -WindowStyle Hidden

Info "Waiting for instance A to start publishing (up to 30s)..."
$aOk = Wait-LogEvent -Path $LogA -Pattern '"event":"publish_started"' -TimeoutSec 30
if ($aOk) { Pass "instance A reached publish_started" } else { Fail "instance A did not reach publish_started within 30s" }

Info "Waiting for instance B to start publishing (up to 30s)..."
$bOk = Wait-LogEvent -Path $LogB -Pattern '"event":"publish_started"' -TimeoutSec 30
if ($bOk) { Pass "instance B reached publish_started" } else { Fail "instance B did not reach publish_started within 30s" }

# -----------------------------------------------------------------------
# Test 2 — ログファイルがインスタンス毎に分離され、互いのイベントが
#          混在していないこと
# -----------------------------------------------------------------------
Write-Host "`n--- Test 2: log files are isolated per instance ---"
if ((Test-Path $LogA) -and (Test-Path $LogB)) {
    Pass "both $LogA and $LogB exist"

    $contentA = Get-Content $LogA -Raw
    $contentB = Get-Content $LogB -Raw
    if ($contentA -match "spoutrelay-e2e-multi-b" -or $contentA -match "TestSpoutSenderMultiB") {
        Fail "log A contains references to instance B"
    } else {
        Pass "log A does not reference instance B"
    }
    if ($contentB -match "spoutrelay-e2e-multi-a" -or $contentB -match "TestSpoutSenderMultiA") {
        Fail "log B contains references to instance A"
    } else {
        Pass "log B does not reference instance A"
    }
} else {
    Fail "one or both log files do not exist (A exists=$(Test-Path $LogA), B exists=$(Test-Path $LogB))"
}

# -----------------------------------------------------------------------
# Test 3 — state ファイル (health.json / metrics.json) もインスタンス毎に
#          分離され、それぞれ自身の sender_name / rtsp_url を反映すること
# -----------------------------------------------------------------------
Write-Host "`n--- Test 3: state files are isolated and reflect per-instance values ---"
Start-Sleep -Milliseconds 1500  # emit_*_interval_ms (=500ms) より長く待つ

$metricsA = Wait-JsonFile -Path $MetricsA -TimeoutSec 10
$metricsB = Wait-JsonFile -Path $MetricsB -TimeoutSec 10

if ($null -ne $metricsA -and $null -ne $metricsB) {
    Pass "both metrics_a.json and metrics_b.json exist and are valid JSON"

    if ($metricsA.rtsp_url -eq $RtspA) { Pass "metrics_a.json rtsp_url == '$RtspA'" }
    else { Fail "metrics_a.json rtsp_url == '$($metricsA.rtsp_url)' (expected '$RtspA')" }

    if ($metricsB.rtsp_url -eq $RtspB) { Pass "metrics_b.json rtsp_url == '$RtspB'" }
    else { Fail "metrics_b.json rtsp_url == '$($metricsB.rtsp_url)' (expected '$RtspB')" }

    if ($metricsA.sender_name -eq "TestSpoutSenderMultiA") { Pass "metrics_a.json sender_name == 'TestSpoutSenderMultiA'" }
    else { Fail "metrics_a.json sender_name == '$($metricsA.sender_name)' (expected 'TestSpoutSenderMultiA')" }

    if ($metricsB.sender_name -eq "TestSpoutSenderMultiB") { Pass "metrics_b.json sender_name == 'TestSpoutSenderMultiB'" }
    else { Fail "metrics_b.json sender_name == '$($metricsB.sender_name)' (expected 'TestSpoutSenderMultiB')" }
} else {
    Fail "metrics_a.json and/or metrics_b.json missing or invalid (A=$($null -ne $metricsA), B=$($null -ne $metricsB))"
}

$healthA = Wait-JsonFile -Path $HealthA -TimeoutSec 10
$healthB = Wait-JsonFile -Path $HealthB -TimeoutSec 10
if ($null -ne $healthA -and $null -ne $healthB) {
    Pass "both health_a.json and health_b.json exist and are valid JSON"
} else {
    Fail "health_a.json and/or health_b.json missing or invalid (A=$($null -ne $healthA), B=$($null -ne $healthB))"
}

# -----------------------------------------------------------------------
# Test 4 — 両方の RTSP ストリームが個別に再生可能であること
#          (一方のインスタンスがもう一方のパスに干渉していないことの確認)
# -----------------------------------------------------------------------
Write-Host "`n--- Test 4: both RTSP streams are independently playable ---"
$rA = & ffprobe -v error -rtsp_transport tcp -i $RtspA -show_entries stream=codec_type -of csv 2>&1
if ($rA -match "video") { Pass "RTSP stream A ($RtspA) is playable" } else { Fail "RTSP stream A is not playable: $rA" }

$rB = & ffprobe -v error -rtsp_transport tcp -i $RtspB -show_entries stream=codec_type -of csv 2>&1
if ($rB -match "video") { Pass "RTSP stream B ($RtspB) is playable" } else { Fail "RTSP stream B is not playable: $rB" }

Stop-AllProcs
Remove-Item $ConfigA, $ConfigB -Force -EA SilentlyContinue

# -----------------------------------------------------------------------
Write-Host "`n========================================"  -ForegroundColor Yellow
Write-Host "  Multi-Instance Results: $PassCount PASS  /  $FailCount FAIL" -ForegroundColor $(if ($FailCount -eq 0) { "Green" } else { "Red" })
Write-Host "========================================`n" -ForegroundColor Yellow

if ($FailCount -gt 0) { exit 1 } else { exit 0 }
