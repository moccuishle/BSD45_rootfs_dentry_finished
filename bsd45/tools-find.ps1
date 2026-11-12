# tools-find.ps1 - locate MSYS2 minw32 / mkisofs / QEMU on this machine.
# Dot-source it from the build scripts:  . (Join-Path $ws "tools-find.ps1")

function Find-Msys2Root {
    $cands = @()
    if ($env:MSYS2_ROOT) { $cands += $env:MSYS2_ROOT }
    $cands += @(
        "C:\msys64",
        "C:\msys2",
        (Join-Path $env:ProgramFiles "msys64"),
        (Join-Path ${env:ProgramFiles(x86)} "msys64")
    )
    foreach ($r in $cands) {
        if ($r -and (Test-Path (Join-Path $r "mingw32\bin\gcc.exe"))) {
            return (Resolve-Path $r).Path
        }
    }
    $g = Get-Command "i686-w64-mingw32-gcc.exe" -ErrorAction SilentlyContinue
    if (-not $g) { $g = Get-Command "gcc.exe" -ErrorAction SilentlyContinue }
    if ($g -and $g.Source -match "^(.*)\\mingw32\\bin\\[^\\]+$") { return $matches[1] }
    return $null
}

function Get-MingwBin {
    $root = Find-Msys2Root
    if (-not $root) {
        throw @"
MSYS2 mingw32 not found.
Install MSYS2 (https://www.msys2.org) and run:
  pacman -S mingw-w64-i686-gcc mingw-w64-i686-binutils
or set the MSYS2_ROOT environment variable to the MSYS2 install directory.
"@
    }
    $bin = Join-Path $root "mingw32\bin"
    $usr = Join-Path $root "usr\bin"
    $env:PATH = "$bin;$usr;" + $env:PATH
    return $bin
}

function Get-GccInclude([string]$bin) {
    $gcc = Join-Path $bin "gcc.exe"
    $inc = (& $gcc -print-file-name=include 2>$null | Select-Object -First 1)
    if ($inc -and (Test-Path $inc)) { return ($inc -replace "\\", "/") }
    $base = Join-Path $bin "..\lib\gcc\i686-w64-mingw32"
    $d = Get-ChildItem $base -Directory -ErrorAction SilentlyContinue |
         Sort-Object Name -Descending | Select-Object -First 1
    if ($d) { return ((Join-Path $d.FullName "include") -replace "\\", "/") }
    throw "cannot locate the GCC include directory under $bin"
}

function Get-Mkisofs {
    $cands = @()
    if ($env:MKISOFS) { $cands += $env:MKISOFS }
    $cands += @(
        (Join-Path ${env:ProgramFiles(x86)} "VMware\VMware Workstation\mkisofs.exe"),
        (Join-Path $env:ProgramFiles "VMware\VMware Workstation\mkisofs.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "VMware\VMware Player\mkisofs.exe"),
        "C:\msys64\usr\bin\mkisofs.exe",
        "C:\msys64\usr\bin\xorriso.exe"
    )
    foreach ($c in $cands) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    foreach ($n in "mkisofs.exe", "xorriso.exe") {
        $cmd = Get-Command $n -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    throw @"
mkisofs / xorriso not found.
Install cdrtools/xorriso, or VMware Workstation (ships mkisofs.exe),
or set the MKISOFS environment variable to the executable.
"@
}

function Get-Qemu {
    $cands = @()
    if ($env:QEMU) { $cands += $env:QEMU }
    $msys = Find-Msys2Root
    if ($msys) {
        $cands += @(
            (Join-Path $msys "ucrt64\bin\qemu-system-i386.exe"),
            (Join-Path $msys "mingw64\bin\qemu-system-i386.exe")
        )
    }
    $cands += @(
        (Join-Path $env:ProgramFiles "qemu\qemu-system-i386.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "qemu\qemu-system-i386.exe")
    )
    foreach ($c in $cands) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    $cmd = Get-Command "qemu-system-i386.exe" -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}
