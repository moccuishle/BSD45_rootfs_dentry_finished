# test_s7.ps1 - S7 acceptance: directories, globbing and pipes.
param([string]$Iso = "")

$ErrorActionPreference = "Stop"
$ws = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (!$Iso) { $Iso = Join-Path $ws "out\BSD45-ELF-x86.iso" }

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

Write-Host "S7 acceptance: directory listing / glob / pipe"
$log = (& $script -Iso $Iso -Keys @(
    "shell",
    "ls /",
    "ls /bin",
    "echo /bin/*",
    "echo hello-pipe | cat",
    "exit 0") -WaitFor "user exit" -TimeoutSec 45) -join "`n"

Check "root" $log @("bin", "sbin", "hello.elf", "readme.txt")
Check "bin"  $log @("cat.elf", "echo.elf", "ls.elf", "sh.elf")
Check "glob" $log @("/bin/cat.elf", "/bin/echo.elf", "/bin/ls.elf", "/bin/sh.elf")
Check "pipe" $log @("hello-pipe")
Check "exit" $log @("[user exit 0]")
if ($log -match 'TRAP') {
    Write-Host "  FAIL kernel trap" -ForegroundColor Red
    $fails++
}

if ($fails) {
    Write-Host "`nS7: FAILED ($fails)" -ForegroundColor Red
    exit 1
}
Write-Host "`nS7: PASSED" -ForegroundColor Green
exit 0
