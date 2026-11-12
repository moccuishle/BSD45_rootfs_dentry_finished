# test_kbd.ps1 - keyboard-only console test (no serial port at all).
#
# This mirrors a VMware VM that has no serial port configured: unassigned
# I/O ports read 0xff, which used to look like "COM1 has data" and starved
# the keyboard.  We boot with -serial none, type "usermode" through QEMU's
# monitor, and require the VGA screen to change.
param(
    [string]$Iso = ""
)

$ErrorActionPreference = "Stop"
$ws = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (!$Iso) { $Iso = Join-Path $ws "out\BSD45-ELF-x86.iso" }
. (Join-Path $ws "tools-find.ps1")
$qemu = Get-Qemu
if (!$qemu) { throw "qemu-system-i386.exe not found" }

$monPort = 4479
$before = Join-Path $ws "build\kbd-before.ppm"
$after  = Join-Path $ws "build\kbd-after.ppm"
Remove-Item $before, $after -Force -ErrorAction SilentlyContinue

$p = Start-Process -FilePath $qemu -ArgumentList @(
    "-cdrom", "`"$Iso`"", "-boot", "d", "-m", "64",
    "-display", "none", "-serial", "none", "-no-reboot",
    "-monitor", "tcp:127.0.0.1:$monPort,server=on,wait=on"
) -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 1200

$c = New-Object System.Net.Sockets.TcpClient
$c.Connect('127.0.0.1', $monPort)
$ns = $c.GetStream()
function MCmd([string]$cmd) {
    $b = [Text.Encoding]::ASCII.GetBytes("$cmd`n")
    $ns.Write($b, 0, $b.Length); $ns.Flush(); Start-Sleep -Milliseconds 150
}

try {
    Start-Sleep -Seconds 5
    MCmd "screendump $before"
    Start-Sleep -Milliseconds 800
    foreach ($k in 'u','s','e','r','m','o','d','e','ret') { MCmd "sendkey $k" }
    Start-Sleep -Milliseconds 1200
    MCmd "screendump $after"
    Start-Sleep -Milliseconds 800
}
finally {
    $ns.Close(); $c.Close()
    if (-not $p.HasExited) { $p.Kill() }
    Start-Sleep -Milliseconds 300
}

if (!(Test-Path $before) -or !(Test-Path $after)) { throw "screendump failed" }
$h1 = (Get-FileHash $before).Hash
$h2 = (Get-FileHash $after).Hash
if ($h1 -eq $h2) {
    Write-Host "keyboard test: FAILED (screen unchanged - keystrokes ignored)" -ForegroundColor Red
    exit 1
}
Write-Host "keyboard test: PASSED (console reacted to keystrokes)" -ForegroundColor Green
exit 0
