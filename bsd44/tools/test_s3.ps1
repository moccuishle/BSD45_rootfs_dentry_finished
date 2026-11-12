# test_s3.ps1 - S3 acceptance test for BSD44-ELF-x86.
#
#	/bin/sh -c 'echo hello; exit 3'   -> "hello" + exit status 3
#	interactive /bin/sh               -> echo works, "exit 7" propagates
#
# Boots the ISO in QEMU and drives the monitor over the serial console.
param(
    [string]$Iso = ""
)

$ErrorActionPreference = "Stop"
$ws = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (!$Iso) { $Iso = Join-Path $ws "out\BSD44-ELF-x86.iso" }
if (!(Test-Path $Iso)) { throw "ISO not found: $Iso" }

$script = Join-Path $ws "tools\run_qemu_test.ps1"
$fails = 0

function Check([string]$name, [string]$log, [string[]]$needles) {
    foreach ($n in $needles) {
        if ($log.Contains($n)) {
            Write-Host ("  ok   {0}: found {1}" -f $name, $n) -ForegroundColor Green
        } else {
            Write-Host ("  FAIL {0}: missing {1}" -f $name, $n) -ForegroundColor Red
            $script:fails++
        }
    }
}

Write-Host "S3 acceptance: /bin/sh -c 'echo hello; exit 3'"
$log1 = & $script -Iso $Iso -Keys @("usermode") -WaitFor "user exit" -TimeoutSec 25
$log1 = $log1 -join "`n"
Check "sh -c" $log1 @("execve /bin/sh", "hello", "[user exit 3]")
if ($log1 -match 'TRAP') {
    Write-Host "  FAIL sh -c: kernel trap" -ForegroundColor Red
    $fails++
}

Write-Host "S3 acceptance: interactive /bin/sh"
$log2 = & $script -Iso $Iso -Keys @("shell", "echo hi there", "exit 7") -WaitFor "user exit" -TimeoutSec 25
$log2 = $log2 -join "`n"
Check "interactive" $log2 @("hi there", "[user exit 7]")
if ($log2 -match 'TRAP') {
    Write-Host "  FAIL interactive: kernel trap" -ForegroundColor Red
    $fails++
}

if ($fails) {
    Write-Host "`nS3: FAILED ($fails check(s))" -ForegroundColor Red
    exit 1
}
Write-Host "`nS3: PASSED" -ForegroundColor Green
exit 0
