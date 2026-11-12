# build_userland.ps1 - build static i386 ELF user programs for BSD45-ELF-x86
#
# The mingw toolchain cannot emit ELF directly, so: COFF object ->
# PE (image base 0x08048000) -> objcopy -O elf32-i386.  The kernel's
# execve only accepts static ET_EXEC ELF32 with no interpreter.
param(
    [string]$OutDir = ""
)

$ErrorActionPreference = "Continue"
$ws = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ws "tools-find.ps1")
$bin = Get-MingwBin
$GCC = "$bin\gcc.exe"; $LD = "$bin\ld.exe"; $OC = "$bin\objcopy.exe"
if (!$OutDir) { $OutDir = Join-Path $ws "out\userland" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$cflags = @(
    "-m32", "-O2", "-ffreestanding", "-fno-pie", "-fno-pic",
    "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
    "-fno-unwind-tables", "-nostdlib", "-nostdinc",
    "-march=i386", "-mno-sse", "-mno-sse2", "-mno-mmx", "-mno-avx",
    "-mpreferred-stack-boundary=2",
    "-B", "C:/msys64/mingw32/bin/"
)

function Build-UserElf([string]$src, [string]$name) {
    $obj = Join-Path $OutDir "$name.o"
    $pe  = Join-Path $OutDir "$name.pe"
    $elf = Join-Path $OutDir "$name.elf"
    & $GCC @cflags -c -o $obj $src
    if ($LASTEXITCODE) { throw "compile $src failed" }
    & $LD -m i386pe -Ttext 0x08048000 -e _start --subsystem console -o $pe $obj 2>$null
    if ($LASTEXITCODE) { throw "link $name failed" }
    & $OC -O elf32-i386 -R .idata -R .reloc $pe $elf
    if ($LASTEXITCODE) { throw "objcopy $name failed" }
    Write-Host "  $name.elf"
    return $elf
}

Write-Host "building userland ELF32"
Build-UserElf (Join-Path $ws "userland\hello.c") "hello" | Out-Null
Build-UserElf (Join-Path $ws "userland\init.c")  "init"  | Out-Null
Build-UserElf (Join-Path $ws "userland\sh.c")    "sh"    | Out-Null
Write-Host "done -> $OutDir"

