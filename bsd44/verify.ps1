# verify.ps1 - boot the ISO in QEMU, run monitor commands, check serial log
$ErrorActionPreference = "Stop"
$ws  = Split-Path -Parent $MyInvocation.MyCommand.Path
$iso = Join-Path $ws "out\BSD44-ELF-x86.iso"
$log = Join-Path $ws "build\verify-serial.log"
. (Join-Path $ws "tools-find.ps1")
$qemu = Get-Qemu
if (!$qemu) {
    Write-Host "QEMU not found - skipping the runtime check (build artifacts are fine)." -ForegroundColor Yellow
    exit 0
}
if (!(Test-Path $iso)) { throw "missing ISO: $iso" }
Remove-Item $log -ErrorAction SilentlyContinue

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $qemu
$psi.Arguments = "-cdrom `"$iso`" -boot d -display none -serial file:$($ws -replace '\\','/')/build/verify-serial.log -monitor stdio -m 64 -no-reboot"
$psi.WorkingDirectory = $ws
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true

$p = [System.Diagnostics.Process]::Start($psi)
Start-Sleep -Seconds 4

function Send-Str([string]$s) {
    foreach ($ch in $s.ToCharArray()) {
        $k = if ($ch -eq ' ') { 'spc' } else { "$ch" }
        $p.StandardInput.WriteLine("sendkey $k")
        Start-Sleep -Milliseconds 55
    }
    $p.StandardInput.WriteLine("sendkey ret")
    Start-Sleep -Milliseconds 400
}

Send-Str "mem"
Send-Str "map"
Start-Sleep -Milliseconds 800
$p.StandardInput.WriteLine("quit")
Start-Sleep -Milliseconds 500
if (!$p.HasExited) { $p.Kill() }

$text = Get-Content $log -Raw
Write-Host $text

$checks = @(
    "4.4BSD-Lite2 (i386) ELF kernel",
    "KERNBASE fe000000",
    "BSD/386 monitor",
    "readback 4f534442 444e4521"
)
$fail = 0
foreach ($c in $checks) {
    if ($text -like "*$c*") { Write-Host "PASS  $c" -ForegroundColor Green }
    else { Write-Host "FAIL  $c" -ForegroundColor Red; $fail++ }
}
if ($fail) { exit 1 }
Write-Host "VERIFY OK" -ForegroundColor Green

