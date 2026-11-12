# test_s4.ps1 - S4 acceptance: the root filesystem comes from the ISO9660 CD.
#
#	- the kernel probes ATAPI and mounts the ISO9660 volume
#	- /sbin/init and /bin/sh are loaded from the disc, not from the kernel
#	- sh -c 'echo hello; exit 3' still runs
param(
    [string]$Iso = ""
)

$ErrorActionPreference = "Stop"
$ws = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (!$Iso) { $Iso = Join-Path $ws "out\BSD45-ELF-x86.iso" }
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

Write-Host "S4 acceptance: ISO9660 root + sh from the CD"
$log = & $script -Iso $Iso -Keys @("cd", "usermode") -WaitFor "user exit" -TimeoutSec 30
$log = $log -join "`n"

Check "mount" $log @("ISO9660 on ATAPI CD", "ISO9660 root: lba")
Check "exec from CD" $log @("from ISO9660", "hello", "[user exit 3]")
Check "no fallback" $log @("execve /bin/sh: from ISO9660")
if ($log -match 'from ramfs') {
    Write-Host "  FAIL the shell came from the embedded ramfs, not the CD" -ForegroundColor Red
    $fails++
}
if ($log -match 'TRAP') {
    Write-Host "  FAIL kernel trap" -ForegroundColor Red
    $fails++
}

if ($fails) {
    Write-Host "`nS4: FAILED ($fails check(s))" -ForegroundColor Red
    exit 1
}
Write-Host "`nS4: PASSED" -ForegroundColor Green
exit 0
