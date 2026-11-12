# test_tty.ps1 - S6 acceptance: console tty / termios.
#
#	- /test reports isatty() true for fd 0/1/2 and a canonical termios
#	- interactive /bin/sh prints its prompt and executes commands
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

Write-Host "S6 acceptance: console tty"
$log1 = (& $script -Iso $Iso -Keys @("test") -WaitFor "user exit" -TimeoutSec 30) -join "`n"
Check "tty" $log1 @("tty: isatty=1/1/1 tcgetattr=0", "tty: iflag=", "lflag=")
if ($log1 -match 'lflag=([0-9a-f]+)') {
    $lf = [Convert]::ToInt32($matches[1], 16)
    foreach ($bit in @(0x2, 0x8)) {   # ICANON, ECHO
        if ($lf -band $bit) {
            Write-Host ("  ok   tty: lflag has 0x{0:x}" -f $bit) -ForegroundColor Green
        } else {
            Write-Host ("  FAIL tty: lflag missing 0x{0:x}" -f $bit) -ForegroundColor Red
            $script:fails++
        }
    }
} else {
    Write-Host "  FAIL tty: no lflag value" -ForegroundColor Red
    $script:fails++
}
if ($log1 -match 'TRAP') { Write-Host "  FAIL tty: kernel trap" -ForegroundColor Red; $fails++ }

Write-Host "S6 acceptance: interactive sh prompt"
$log2 = (& $script -Iso $Iso -Keys @("shell", "echo hi-tty", "exit 0") -WaitFor "user exit" -TimeoutSec 30) -join "`n"
Check "sh" $log2 @("# echo hi-tty", "hi-tty", "[user exit 0]")
if ($log2 -match 'TRAP') { Write-Host "  FAIL sh: kernel trap" -ForegroundColor Red; $fails++ }

if ($fails) {
    Write-Host "`nS6: FAILED ($fails)" -ForegroundColor Red
    exit 1
}
Write-Host "`nS6: PASSED" -ForegroundColor Green
exit 0
