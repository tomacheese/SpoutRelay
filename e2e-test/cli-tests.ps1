#requires -Version 7
# SpoutRelay CLI E2E テストスクリプト (pwsh 7+ 必須)
# 使い方: cd e2e-test; pwsh ./cli-tests.ps1
#
# main.cpp の CLI 引数処理・終了コード・Ctrl+C グレースフルシャットダウンを検証する。
# mediamtx / Spout センダーは不要 (CONNECTING_OUTPUT/STREAMING に到達しない設定でも検証可能)。

$ErrorActionPreference = "Continue"
$Root     = Split-Path -Parent $PSScriptRoot
$BuildDir = "$Root\build"
$TestDir  = $PSScriptRoot
$Exe      = "$BuildDir\spout-relay.exe"
$ConfigDir = "$TestDir\config"
$CliTmpDir = "$TestDir\cli-tmp"

$PassCount = 0; $FailCount = 0
function Pass($msg) { Write-Host "[PASS] $msg" -ForegroundColor Green;  $script:PassCount++ }
function Fail($msg) { Write-Host "[FAIL] $msg" -ForegroundColor Red;    $script:FailCount++ }
function Info($msg) { Write-Host "  >> $msg"   -ForegroundColor Cyan }
function Diag($msg) { Write-Host "     $msg"   -ForegroundColor DarkGray }

function Stop-AllProcs {
    @("spout-relay") | ForEach-Object { Get-Process $_ -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue }
    Start-Sleep -Milliseconds 500
}

# -----------------------------------------------------------------------
# CTRL_BREAK_EVENT による Ctrl+C グレースフルシャットダウンテスト用ヘルパー。
# main.cpp の console_ctrl_handler は CTRL_BREAK_EVENT も処理するため、
# 新しいコンソールで起動したプロセスに対して送信することで再現できる。
# -----------------------------------------------------------------------
function Start-RelayWithNewProcessGroup {
    param([string[]]$ArgList)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName  = $Exe
    foreach ($a in $ArgList) { $psi.ArgumentList.Add($a) }
    $psi.WorkingDirectory = $TestDir
    # UseShellExecute=$true + WindowStyle=Hidden にすることで、呼び出し元 (pwsh) の
    # コンソールとは別の新しいコンソールでプロセスを起動する。
    # これにより AttachConsole(対象 PID) で対象プロセスのコンソールにのみ
    # アタッチでき、CTRL_BREAK_EVENT を呼び出し元へ波及させずに送信できる。
    $psi.UseShellExecute  = $true
    $psi.WindowStyle      = [System.Diagnostics.ProcessWindowStyle]::Hidden
    return [System.Diagnostics.Process]::Start($psi)
}

function Send-CtrlBreak([int]$TargetProcessId) {
    # AttachConsole + GenerateConsoleCtrlEvent(0) は呼び出し元プロセスが
    # アタッチしているコンソールの全プロセスに信号を送るため、このスクリプトを
    # 実行している pwsh 自身にも CTRL_BREAK が届いてしまう。
    # そのため、使い捨ての子 pwsh プロセス (send-ctrlbreak-helper.ps1) の中で
    # Attach/Generate を行い、成否はマーカーファイルの有無で判定する
    # (ヘルパー自身も CTRL_BREAK を受けて異常終了コードを返す場合があるため)。
    $markerFile = "$CliTmpDir\ctrlbreak-attached.marker"
    Remove-Item $markerFile -Force -EA SilentlyContinue

    Start-Process -FilePath "pwsh" `
        -ArgumentList @("-NoProfile", "-File", "$PSScriptRoot\send-ctrlbreak-helper.ps1",
                        "-TargetPid", "$TargetProcessId", "-MarkerFile", $markerFile) `
        -PassThru -Wait -WindowStyle Hidden | Out-Null

    return (Test-Path $markerFile)
}

# -----------------------------------------------------------------------
Write-Host "`n========================================" -ForegroundColor Yellow
Write-Host "  SpoutRelay CLI Tests" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Yellow

Stop-AllProcs
New-Item -ItemType Directory -Force "$TestDir\logs"  | Out-Null
New-Item -ItemType Directory -Force "$TestDir\state" | Out-Null
New-Item -ItemType Directory -Force $CliTmpDir       | Out-Null

if (-not (Test-Path $Exe)) {
    Fail "spout-relay.exe not found at $Exe (build first)"
    Write-Host "`n========================================" -ForegroundColor Yellow
    Write-Host "  RESULT: $PassCount passed, $FailCount failed" -ForegroundColor Yellow
    Write-Host "========================================`n" -ForegroundColor Yellow
    exit 1
}

# -----------------------------------------------------------------------
# Test 1 — --help は使用方法を表示して終了コード 0
# -----------------------------------------------------------------------
Write-Host "--- Test 1: --help prints usage and exits 0 ---"
$out = & $Exe --help 2>&1 | Out-String
$exit = $LASTEXITCODE
if ($exit -eq 0) { Pass "--help exits with code 0" } else { Fail "--help exited with code $exit (expected 0)" }
if ($out -match "Usage") { Pass "--help output contains 'Usage'" } else { Fail "--help output did not contain 'Usage': $out" }

# -----------------------------------------------------------------------
# Test 2 — -h も同様に使用方法を表示して終了コード 0
# -----------------------------------------------------------------------
Write-Host "`n--- Test 2: -h prints usage and exits 0 ---"
$out = & $Exe -h 2>&1 | Out-String
$exit = $LASTEXITCODE
if ($exit -eq 0) { Pass "-h exits with code 0" } else { Fail "-h exited with code $exit (expected 0)" }
if ($out -match "Usage") { Pass "-h output contains 'Usage'" } else { Fail "-h output did not contain 'Usage': $out" }

# -----------------------------------------------------------------------
# Test 3 — 存在しない設定ファイルを指定した場合、終了コード 1 でエラーを表示
# -----------------------------------------------------------------------
Write-Host "`n--- Test 3: missing config file → exit 1 with [ERROR] ---"
$missing = "$ConfigDir\does_not_exist.json"
$out = & $Exe --config $missing 2>&1 | Out-String
$exit = $LASTEXITCODE
if ($exit -eq 1) { Pass "missing config exits with code 1" } else { Fail "missing config exited with code $exit (expected 1)" }
if ($out -match "\[ERROR\]") { Pass "missing config prints [ERROR]" } else { Fail "missing config output did not contain [ERROR]: $out" }

# -----------------------------------------------------------------------
# Test 4 — 不正な JSON の設定ファイルを指定した場合、終了コード 1 でエラーを表示
# -----------------------------------------------------------------------
Write-Host "`n--- Test 4: invalid JSON config → exit 1 with [ERROR] ---"
$invalidJson = "$CliTmpDir\config_invalid.json"
Set-Content -Path $invalidJson -Value "{ this is not valid json " -NoNewline
$out = & $Exe --config $invalidJson 2>&1 | Out-String
$exit = $LASTEXITCODE
if ($exit -eq 1) { Pass "invalid JSON config exits with code 1" } else { Fail "invalid JSON config exited with code $exit (expected 1)" }
if ($out -match "\[ERROR\]") { Pass "invalid JSON config prints [ERROR]" } else { Fail "invalid JSON config output did not contain [ERROR]: $out" }

# -----------------------------------------------------------------------
# Test 5 — -c (短縮形) でも --config と同様に動作する
# -----------------------------------------------------------------------
Write-Host "`n--- Test 5: -c (short form) behaves like --config ---"
$out = & $Exe -c $missing 2>&1 | Out-String
$exit = $LASTEXITCODE
if ($exit -eq 1 -and $out -match "\[ERROR\]") { Pass "-c <missing> exits 1 with [ERROR]" } else { Fail "-c <missing> got exit=$exit out=$out" }

# -----------------------------------------------------------------------
# Test 6 — 設定ファイル未指定時、デフォルトパス (config/config.json) が使われ、
#          存在しない場合は終了コード 1 となる
# -----------------------------------------------------------------------
Write-Host "`n--- Test 6: no --config uses default path config/config.json ---"
$defaultCwd = "$CliTmpDir\no-default-config"
New-Item -ItemType Directory -Force $defaultCwd | Out-Null
Push-Location $defaultCwd
try {
    $out = & $Exe 2>&1 | Out-String
    $exit = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($exit -eq 1) { Pass "no --config (missing default path) exits with code 1" } else { Fail "no --config exited with code $exit (expected 1)" }
if ($out -match "\[ERROR\]") { Pass "no --config (missing default path) prints [ERROR]" } else { Fail "no --config output did not contain [ERROR]: $out" }

# -----------------------------------------------------------------------
# Test 7 — Supervisor::init 失敗時 (log_dir がファイルに占有されている) は
#          終了コード 1 でエラーを表示する
# -----------------------------------------------------------------------
Write-Host "`n--- Test 7: Supervisor::init failure (log_dir blocked by a file) → exit 1 ---"
$blockedLogDir = "$CliTmpDir\blocked-log-dir"
Remove-Item $blockedLogDir -Force -EA SilentlyContinue
New-Item -ItemType File -Path $blockedLogDir -Force | Out-Null  # ディレクトリの代わりにファイルを置く

$baseConfig = Get-Content "$ConfigDir\config.json" -Raw | ConvertFrom-Json
$baseConfig.app.log_dir      = $blockedLogDir
$baseConfig.app.instance_name = "spoutrelay-cli-test-blocked"
$blockedConfigPath = "$CliTmpDir\config_blocked_log.json"
$baseConfig | ConvertTo-Json -Depth 10 | Set-Content -Path $blockedConfigPath

$out = & $Exe --config $blockedConfigPath 2>&1 | Out-String
$exit = $LASTEXITCODE
if ($exit -eq 1) { Pass "Supervisor::init failure (blocked log_dir) exits with code 1" } else { Fail "blocked log_dir exited with code $exit (expected 1): $out" }
if ($out -match "\[ERROR\]") { Pass "Supervisor::init failure prints [ERROR]" } else { Fail "blocked log_dir output did not contain [ERROR]: $out" }

Remove-Item $blockedLogDir -Force -EA SilentlyContinue

# -----------------------------------------------------------------------
# Test 8 — Ctrl+C (CTRL_BREAK_EVENT) によるグレースフルシャットダウン
# -----------------------------------------------------------------------
Write-Host "`n--- Test 8: graceful shutdown on CTRL_BREAK_EVENT ---"
Remove-Item "$TestDir\state\health.json" -Force -EA SilentlyContinue

$proc = Start-RelayWithNewProcessGroup -ArgList @("--config", "$ConfigDir\config.json")
Start-Sleep -Seconds 2

if ($proc.HasExited) {
    Fail "spout-relay exited prematurely before CTRL_BREAK_EVENT was sent (exit code $($proc.ExitCode))"
} else {
    Info "spout-relay running (PID=$($proc.Id)), sending CTRL_BREAK_EVENT"
    $sent = Send-CtrlBreak -TargetProcessId $proc.Id
    if (-not $sent) {
        Fail "GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT) failed"
        $proc.Kill()
    } else {
        $exited = $proc.WaitForExit(10000)
        if ($exited) {
            Pass "spout-relay shuts down gracefully after CTRL_BREAK_EVENT (exit code $($proc.ExitCode))"
        } else {
            Fail "spout-relay did not exit within 10s after CTRL_BREAK_EVENT"
            $proc.Kill()
        }
    }
}

Stop-AllProcs

# -----------------------------------------------------------------------
Write-Host "`n========================================" -ForegroundColor Yellow
Write-Host "  RESULT: $PassCount passed, $FailCount failed" -ForegroundColor Yellow
Write-Host "========================================`n" -ForegroundColor Yellow

if ($FailCount -gt 0) { exit 1 } else { exit 0 }
