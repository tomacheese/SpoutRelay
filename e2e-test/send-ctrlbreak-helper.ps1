#requires -Version 7
# Throwaway helper script invoked from cli-tests.ps1.
#
# AttachConsole + GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0) signals every process
# on the console the caller is attached to, so calling it directly from the
# cli-tests.ps1 (parent pwsh) process would terminate the parent too.
# Attach/Generate therefore runs inside this throwaway child process.
#
# Arguments:
#   -TargetPid    : PID of the process to send CTRL_BREAK_EVENT to
#   -MarkerFile   : path of the marker file created after AttachConsole succeeds
#
# Note: GenerateConsoleCtrlEvent also sends CTRL_BREAK to this helper itself, so
#       even with SetConsoleCtrlHandler(NULL, TRUE) suppressing it for this process
#       the exit code may not be returned normally. The caller must judge
#       AttachConsole/send success by the presence of the marker file.
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

# Ignore CTRL_BREAK to this process (it cannot always be fully prevented,
# so success is judged by the marker file)
[SpoutRelayCli.CtrlBreakHelper]::SetConsoleCtrlHandler([IntPtr]::Zero, $true) | Out-Null

Set-Content -Path $MarkerFile -Value "attached" -NoNewline
[SpoutRelayCli.CtrlBreakHelper]::GenerateConsoleCtrlEvent(1, 0) | Out-Null
exit 0
