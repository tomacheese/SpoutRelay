#requires -Version 7
# cli-tests.ps1 から呼び出される使い捨てヘルパースクリプト。
#
# AttachConsole + GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0) は
# 呼び出し元プロセスがアタッチしているコンソールの全プロセスに信号を送るため、
# cli-tests.ps1 (親 pwsh) のプロセスから直接呼ぶと親自身も終了してしまう。
# そのため、この使い捨て子プロセスの中で Attach/Generate を行う。
#
# 引数:
#   -TargetPid    : CTRL_BREAK_EVENT を送信する対象プロセスの PID
#   -MarkerFile   : AttachConsole 成功後に作成するマーカーファイルのパス
#
# 注意: GenerateConsoleCtrlEvent はこのヘルパー自身にも CTRL_BREAK を送るため、
#       SetConsoleCtrlHandler(NULL, TRUE) で自プロセスへの影響を抑止しても
#       終了コードが正常に返らない場合がある。呼び出し側はマーカーファイルの
#       有無で AttachConsole/送信の成否を判定すること。
param(
    [int]$TargetPid,
    [string]$MarkerFile
)

Add-Type -Name CtrlBreakHelper -Namespace SpoutRelayCli -MemberDefinition '
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool GenerateConsoleCtrlEvent(uint dwCtrlEvent, uint dwProcessGroupId);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool AttachConsole(uint dwProcessId);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool FreeConsole();
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool SetConsoleCtrlHandler(IntPtr HandlerRoutine, bool Add);
'

[SpoutRelayCli.CtrlBreakHelper]::FreeConsole() | Out-Null
$attached = [SpoutRelayCli.CtrlBreakHelper]::AttachConsole([uint32]$TargetPid)
if (-not $attached) { exit 1 }

# 自プロセスへの CTRL_BREAK を無視する (完全に防げない場合があるため
# 成否判定はマーカーファイルで行う)
[SpoutRelayCli.CtrlBreakHelper]::SetConsoleCtrlHandler([IntPtr]::Zero, $true) | Out-Null

Set-Content -Path $MarkerFile -Value "attached" -NoNewline
[SpoutRelayCli.CtrlBreakHelper]::GenerateConsoleCtrlEvent(1, 0) | Out-Null
exit 0
