# build_iso.ps1 - build the El Torito boot image and the BSD44-ELF-x86 ISO
param(
    [string]$Kernel = "",
    [string]$OutIso = "",
    [int]$KernelLba = 2048
)

$ErrorActionPreference = "Continue"
$ws  = Split-Path -Parent $MyInvocation.MyCommand.Path
$bld = Join-Path $ws "build"
$boot= Join-Path $ws "boot"
$out = Join-Path $ws "out"
. (Join-Path $ws "tools-find.ps1")
$bin = Get-MingwBin
$GCC = "$bin\gcc.exe"; $LD = "$bin\ld.exe"; $OC = "$bin\objcopy.exe"
$MKISO = Get-Mkisofs
$isodir = Join-Path $bld "isodir"
if (!$Kernel) { $Kernel = Join-Path $bld "bsd44.elf" }
if (!$OutIso) { $OutIso = Join-Path $out "BSD44-ELF-x86.iso" }
New-Item -ItemType Directory -Force -Path $out, $isodir | Out-Null

function Build-Stage([string]$src, [string]$text, [string]$name, [string]$entry = "_start") {
    $o = "$bld\$name.o"
    & $GCC -B "C:/msys64/mingw32/bin/" -m32 -c -x assembler-with-cpp -o $o $src
    if ($LASTEXITCODE) { throw "assemble $name failed" }
    $pe = "$bld\$name.pe"
    & $LD -m i386pe -Ttext $text -e $entry --subsystem console -o $pe $o 2>$null
    if ($LASTEXITCODE) { throw "link $name failed" }
    $raw = "$bld\$name.bin"
    & $OC -O binary --only-section=.text $pe $raw
    if ($LASTEXITCODE) { throw "objcopy $name failed" }
    return $raw
}

$s1 = Build-Stage "$boot\stage1.s" "0x7C00" "stage1"
$s2 = Build-Stage "$boot\stage2.s" "0x90000" "stage2" "_start32"

$b1 = [IO.File]::ReadAllBytes($s1)
$b2 = [IO.File]::ReadAllBytes($s2)
if ($b1.Length -ne 512) { throw "stage1 must be 512 bytes, got $($b1.Length)" }
Write-Host ("stage1 {0} bytes, stage2 {1} bytes" -f $b1.Length, $b2.Length)

# boot image = stage1 || stage2, padded to 8 virtual sectors (4 KiB)
$img = New-Object byte[] 4096
[Array]::Copy($b1, 0, $img, 0, 512)
[Array]::Copy($b2, 0, $img, 512, $b2.Length)

# patch the manifest: "BDMF" lba bytes stage2size
$magic = [byte[]](0x42, 0x44, 0x4D, 0x46)
$off = -1
for ($i = 0; $i -le 512 - 16; $i++) {
    if ($img[$i] -eq 0x42 -and $img[$i+1] -eq 0x44 -and $img[$i+2] -eq 0x4D -and $img[$i+3] -eq 0x46) { $off = $i; break }
}
if ($off -lt 0) { throw "manifest magic not found" }
[BitConverter]::GetBytes([uint32]$KernelLba).CopyTo($img, $off + 4)
[BitConverter]::GetBytes([uint32](Get-Item $Kernel).Length).CopyTo($img, $off + 8)
[BitConverter]::GetBytes([uint32]$b2.Length).CopyTo($img, $off + 12)
[IO.File]::WriteAllBytes("$bld\boot.img", $img)

Copy-Item $Kernel "$isodir\BSD44.ELF" -Force

# S4: the userland programs live on the ISO9660 volume, not in the kernel.
New-Item -ItemType Directory -Force -Path "$isodir\BIN", "$isodir\SBIN" | Out-Null
$sh = Join-Path $ws "out\userland\sh.elf"
$init = Join-Path $ws "out\userland\init.elf"
$hello = Join-Path $ws "out\userland\hello.elf"
$test = Join-Path $ws "out\userland\test.elf"
if (Test-Path $sh)    { Copy-Item $sh    "$isodir\BIN\SH.ELF"   -Force }
if (Test-Path $init)  { Copy-Item $init  "$isodir\SBIN\INIT.ELF" -Force }
if (Test-Path $hello) { Copy-Item $hello "$isodir\HELLO.ELF"    -Force }
if (Test-Path $test)  { Copy-Item $test  "$isodir\TEST.ELF"     -Force }
Copy-Item "$bld\boot.img" "$isodir\boot.img" -Force
Copy-Item "$ws\README-ISO.txt" "$isodir\README.TXT" -Force -ErrorAction SilentlyContinue

if (Test-Path $OutIso) { Remove-Item $OutIso -Force }
$mkArgs = @()
if ([IO.Path]::GetFileNameWithoutExtension($MKISO) -eq "xorriso") { $mkArgs += @("-as", "mkisofs") }
& $MKISO @mkArgs -quiet -iso-level 1 -V "BSD44_ELF_X86" -b boot.img -c boot.cat `
    -no-emul-boot -boot-load-size 8 -o $OutIso $isodir
if ($LASTEXITCODE) { throw "mkisofs failed" }

# pad the ISO to the kernel LBA and append the ELF kernel there
$isoBytes = [IO.File]::ReadAllBytes($OutIso)
$target = [long]$KernelLba * 2048
if ($isoBytes.Length -gt $target) { throw "ISO already larger than kernel LBA ($($isoBytes.Length) > $target)" }
$kbytes = [IO.File]::ReadAllBytes($Kernel)
$fs = [IO.File]::Open($OutIso, [IO.FileMode]::Append)
$pad = New-Object byte[] ($target - $isoBytes.Length)
$fs.Write($pad, 0, $pad.Length)
$fs.Write($kbytes, 0, $kbytes.Length)
# CD sectors are 2048 bytes; stage1 reads whole sectors, so pad the last one.
$pad2 = (2048 - ($kbytes.Length % 2048)) % 2048
if ($pad2 -gt 0) {
    $fs.Write((New-Object byte[] $pad2), 0, $pad2)
}
$fs.Close()

Write-Host ("ISO: {0} ({1:N0} bytes), kernel at LBA {2}" -f $OutIso, (Get-Item $OutIso).Length, $KernelLba)








