# run_qemu_test.ps1 - drive the BSD44 ISO in QEMU through a TCP serial port.
#
# QEMU exposes COM1 on 127.0.0.1:$SerialPort so that we can both read the
# console and type commands without depending on stdio redirection.
param(
    [string]$Iso  = "",
    [string[]]$Keys = @("usermode"),
    [string]$WaitFor = "",
    [int]$TimeoutSec = 40,
    [int]$BootTimeoutSec = 25,
    [int]$SerialPort = 5555,
    [int]$MonitorPort = 4444,
    [int]$SettleMs = 300
)

$ErrorActionPreference = "Stop"
$ws = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (!$Iso) { $Iso = Join-Path $ws "out\BSD44-ELF-x86.iso" }
. (Join-Path $ws "tools-find.ps1")
$qemu = Get-Qemu
if (!$qemu) { throw "qemu-system-i386.exe not found (set QEMU env var)" }

$log = Join-Path $ws "build\qemu-serial.log"
Remove-Item $log -Force -ErrorAction SilentlyContinue

$argList = @(
    "-cdrom", "`"$Iso`"", "-boot", "d", "-m", "64",
    "-display", "none", "-no-reboot",
    "-serial", "tcp:127.0.0.1:$SerialPort,server=on,wait=on",
    "-monitor", "tcp:127.0.0.1:$MonitorPort,server=on,nowait"
)
$p = Start-Process -FilePath $qemu -ArgumentList $argList -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 800

$client = New-Object System.Net.Sockets.TcpClient
$deadline = (Get-Date).AddSeconds(10)
while ($true) {
    try { $client.Connect('127.0.0.1', $SerialPort); break }
    catch {
        if ((Get-Date) -gt $deadline) { throw "cannot connect to serial port: $($_.Exception.Message)" }
        Start-Sleep -Milliseconds 250
    }
}
$ns = $client.GetStream()
$ns.ReadTimeout = 500
$sb = New-Object System.Text.StringBuilder
$buf = New-Object byte[] 4096

function Pump([int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        try {
            if ($ns.DataAvailable) {
                $n = $ns.Read($buf, 0, $buf.Length)
                if ($n -gt 0) { [void]$sb.Append([Text.Encoding]::ASCII.GetString($buf, 0, $n)) }
            } else {
                Start-Sleep -Milliseconds 60
            }
        } catch { Start-Sleep -Milliseconds 60 }
    }
}

function Wait-Text([string]$text, [int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        Pump 150
        if ($sb.ToString().Contains($text)) { return $true }
    }
    return $false
}

try {
    if (-not (Wait-Text "bsd45>" ($BootTimeoutSec * 1000))) { throw "boot timeout: no monitor prompt" }
    Start-Sleep -Milliseconds $SettleMs
    foreach ($k in $Keys) {
        $bytes = [Text.Encoding]::ASCII.GetBytes($k + "`r")
        $ns.Write($bytes, 0, $bytes.Length)
        $ns.Flush()
        Pump 200
        Start-Sleep -Milliseconds 500
    }
    if ($WaitFor) { [void](Wait-Text $WaitFor ($TimeoutSec * 1000)) }
    else { Pump 3000 }
    Pump 300
}
finally {
    try { $ns.Close(); $client.Close() } catch { }
    if (-not $p.HasExited) { $p.Kill() }
    Start-Sleep -Milliseconds 300
}

$text = $sb.ToString()
Set-Content -Path $log -Value $text -Encoding UTF8
Write-Output $text
