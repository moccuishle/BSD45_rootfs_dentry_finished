# build_all.ps1 - build kernel + ISO in one shot
param(
    [switch]$NoSh      # keep the small built-in sh instead of the 4.4BSD one
)

$ErrorActionPreference = "Stop"
$ws = Split-Path -Parent $MyInvocation.MyCommand.Path

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $ws "build_userland.ps1")
if ($LASTEXITCODE -ne 0) { throw "userland build failed" }

if (-not $NoSh) {
    # S3: the real 4.4BSD /bin/sh (overwrites out\userland\sh.elf).
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $ws "build_sh.ps1") |
        Select-Object -Last 3
    if (!(Test-Path (Join-Path $ws "out\userland\sh.elf"))) { throw "sh build failed" }
}

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $ws "build_kernel.ps1")
if ($LASTEXITCODE -ne 0) { throw "kernel build failed" }

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $ws "build_iso.ps1")
if ($LASTEXITCODE -ne 0) { throw "ISO build failed" }

Write-Host ""
Write-Host "Artifacts:"
Get-ChildItem (Join-Path $ws "out") | Select-Object Name, Length

exit 0
