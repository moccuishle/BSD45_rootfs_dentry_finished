# test_proc.ps1 - fork/vfork/wait4 acceptance.
#
# /test (on the ISO) forks a child that exits 5, then vforks a child that
# execs /bin/echo, and reaps both with wait4.
param([string]$Iso = "")

$ErrorActionPreference = "Stop"
$ws = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (!$Iso) { $Iso = Join-Path $ws "out\BSD45-ELF-x86.iso" }

$script = Join-Path $ws "tools\run_qemu_test.ps1"
$fails = 0

Write-Host "S5 acceptance: fork / vfork / wait4"
$log = & $script -Iso $Iso -Keys @("test") -WaitFor "user exit" -TimeoutSec 30
$log = $log -join "`n"

foreach ($n in @("S:fork", "C:fork", "fork waited pid=2 code=5",
                 "S:vfork", "vfork-exec-ok", "vfork waited pid=3 code=0",
                 "done", "[user exit 0]")) {
    if ($log.Contains($n)) {
        Write-Host ("  ok   found {0}" -f $n) -ForegroundColor Green
    } else {
        Write-Host ("  FAIL missing {0}" -f $n) -ForegroundColor Red
        $fails++
    }
}
if ($log -match 'TRAP') {
    Write-Host "  FAIL kernel trap" -ForegroundColor Red
    $fails++
}

if ($fails) {
    Write-Host "`nS5: FAILED ($fails)" -ForegroundColor Red
    exit 1
}
Write-Host "`nS5: PASSED" -ForegroundColor Green
exit 0
