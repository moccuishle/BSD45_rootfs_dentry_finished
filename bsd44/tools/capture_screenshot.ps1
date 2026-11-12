# capture_screenshot.ps1 - grab the VGA text console after the S3 demo runs.
param(
    [string]$Iso = "",
    [string]$OutPpm = ""
)

$ErrorActionPreference = "Stop"
$ws = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (!$Iso) { $Iso = Join-Path $ws "out\BSD44-ELF-x86.iso" }
if (!$OutPpm) { $OutPpm = Join-Path $ws "build\screenshot.ppm" }
. (Join-Path $ws "tools-find.ps1")
$qemu = Get-Qemu
if (!$qemu) { throw "qemu-system-i386.exe not found" }

$serialPort = 5565
$monPort = 4465
Remove-Item $OutPpm -Force -ErrorAction SilentlyContinue

$p = Start-Process -FilePath $qemu -ArgumentList @(
    "-cdrom", "`"$Iso`"", "-boot", "d", "-m", "64",
    "-display", "none", "-no-reboot",
    "-serial", "tcp:127.0.0.1:$serialPort,server=on,wait=on",
    "-monitor", "tcp:127.0.0.1:$monPort,server=on,nowait"
) -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 700

$serial = New-Object System.Net.Sockets.TcpClient
$serial.Connect('127.0.0.1', $serialPort)
$sns = $serial.GetStream()
$sns.ReadTimeout = 500
$buf = New-Object byte[] 4096
$sb = New-Object System.Text.StringBuilder
function Pump([int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        try {
            if ($sns.DataAvailable) {
                $n = $sns.Read($buf, 0, $buf.Length)
                if ($n -gt 0) { [void]$sb.Append([Text.Encoding]::ASCII.GetString($buf, 0, $n)) }
            } else { Start-Sleep -Milliseconds 50 }
        } catch { Start-Sleep -Milliseconds 50 }
    }
}
function Wait-Text([string]$t, [int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $end) {
        Pump 120
        if ($sb.ToString().Contains($t)) { return $true }
    }
    return $false
}

try {
    if (-not (Wait-Text "bsd44>" 20000)) { throw "no monitor prompt" }
    $bytes = [Text.Encoding]::ASCII.GetBytes("usermode`r")
    $sns.Write($bytes, 0, $bytes.Length); $sns.Flush()
    [void](Wait-Text "user exit 3" 20000)
    Start-Sleep -Milliseconds 500

    $mon = New-Object System.Net.Sockets.TcpClient
    $mon.Connect('127.0.0.1', $monPort)
    $mns = $mon.GetStream()
    $cmd = [Text.Encoding]::ASCII.GetBytes("screendump $OutPpm`n")
    $mns.Write($cmd, 0, $cmd.Length); $mns.Flush()
    Start-Sleep -Milliseconds 1500
    $mns.Close(); $mon.Close()
}
finally {
    try { $sns.Close(); $serial.Close() } catch { }
    if (-not $p.HasExited) { $p.Kill() }
    Start-Sleep -Milliseconds 300
}

if (Test-Path $OutPpm) {
    Write-Output "captured $OutPpm ($((Get-Item $OutPpm).Length) bytes)"
} else {
    Write-Output "screendump failed"
}
