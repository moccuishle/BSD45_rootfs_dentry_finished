# build_kernel.ps1 - cross-build the 4.4BSD-Lite2 i386 kernel as UNIX ELF
# using the MSYS2 mingw32 (i686-w64-mingw32) gcc.
param(
    [switch]$NoLink,
    [string]$Only = ""
)

$ErrorActionPreference = "Continue"
$ws   = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $ws "src\4.4BSD-Lite2\usr\src\sys"
$bld  = Join-Path $ws "build"
$obj  = Join-Path $bld "obj"
$inc  = Join-Path $bld "inc"
$gen  = Join-Path $bld "gen"
. (Join-Path $ws "tools-find.ps1")
$bin  = Get-MingwBin
$GCC  = "$bin\gcc.exe"
$AS   = "$bin\as.exe"
$LD   = "$bin\ld.exe"
$OC   = "$bin\objcopy.exe"
$OD   = "$bin\objdump.exe"
$gccinc = Get-GccInclude $bin

New-Item -ItemType Directory -Force -Path $bld, $obj, $gen | Out-Null

# ---------------------------------------------------------------- sources
# The repository ships the original 4.4BSD-Lite2 tarball plus a small
# overlay of the files that were adjusted for modern GCC / PE linking.
$tarball = Join-Path $ws "..\4.4BSD-Lite2.tar.gz"
if (!(Test-Path $tarball)) { $tarball = Join-Path $ws "4.4BSD-Lite2.tar.gz" }
if (!(Test-Path "$src\i386\i386\locore.s") -or !(Test-Path "$src\sys\param.h")) {
    if (!(Test-Path $tarball)) {
        throw "4.4BSD-Lite2.tar.gz not found next to the project; put it in the parent directory"
    }
    Write-Host "extracting $tarball"
    if (Test-Path $src) { Remove-Item $src -Recurse -Force -ErrorAction SilentlyContinue }
    New-Item -ItemType Directory -Force -Path "$ws\src" | Out-Null
    # Use the Windows bsdtar: MSYS2's GNU tar (first on PATH here) mishandles
    # Windows -C paths.  bsdtar also skips the tree's broken symlinks for us.
    $tarExe = Join-Path $env:SystemRoot "System32\tar.exe"
    & $tarExe -xzf $tarball -C "$ws\src" 2>$null
    if (!(Test-Path "$src\i386\i386\locore.s")) {
        throw "extraction failed: $src\i386\i386\locore.s missing"
    }
}
$overlay = Join-Path $ws "overlay"
if (Test-Path $overlay) {
    Copy-Item "$overlay\*" $src -Recurse -Force
}

# ---------------------------------------------------------------- headers
function Copy-Headers([string]$from, [string]$to) {
    New-Item -ItemType Directory -Force -Path $to | Out-Null
    Get-ChildItem $from -File -Force -ErrorAction SilentlyContinue | ForEach-Object {
        try {
            Copy-Item $_.FullName (Join-Path $to $_.Name) -Force -ErrorAction Stop
        } catch {
            Write-Host "  skip header $($_.Name) (broken link)"
        }
    }
}
New-Item -ItemType Directory -Force -Path "$inc\sys", "$inc\vm", "$inc\machine", "$inc\i386" | Out-Null
Copy-Headers "$src\sys" "$inc\sys"
Copy-Headers "$src\vm" "$inc\vm"
Copy-Headers "$src\i386\include" "$inc\machine"
Copy-Headers "$src\i386\include" "$inc\i386"
Copy-Item "$src\hp300\include\ansi.h"  "$inc\machine\ansi.h"  -Force
Copy-Item "$src\hp300\include\limits.h" "$inc\machine\limits.h" -Force
Copy-Item "$src\hp300\include\float.h"  "$inc\machine\float.h"  -Force
New-Item -ItemType Directory -Force -Path "$inc\libkern" | Out-Null
Copy-Item "$src\libkern\libkern.h" "$inc\libkern\libkern.h" -Force
if (!(Select-String -Path "$inc\machine\types.h" -Pattern 'typedef int register_t;' -Quiet)) {
    Add-Content "$inc\machine\types.h" "`ntypedef int register_t;`ntypedef unsigned int u_register_t;" -Encoding ascii
}
@'
/* Minimal freestanding <string.h> for the kernel build. */
#ifndef _STRING_H_
#define _STRING_H_
#include <sys/types.h>
unsigned int	strlen(const char *);
int		strcmp(const char *, const char *);
int		strncmp(const char *, const char *, unsigned int);
int		bcmp(const void *, const void *, unsigned int);
void		bcopy(const void *, void *, unsigned int);
void		bzero(void *, unsigned int);
void *		memcpy(void *, const void *, unsigned int);
void *		memmove(void *, const void *, unsigned int);
void *		memset(void *, int, unsigned int);
#endif
'@ | Set-Content -Encoding ascii "$inc\string.h"
@'
#ifndef _MACHINE_ENDIAN_H_
#define _MACHINE_ENDIAN_H_
#define _LITTLE_ENDIAN 1234
#define _BIG_ENDIAN 4321
#define _PDP_ENDIAN 3412
#define _BYTE_ORDER _LITTLE_ENDIAN
#define LITTLE_ENDIAN _LITTLE_ENDIAN
#define BIG_ENDIAN _BIG_ENDIAN
#define PDP_ENDIAN _PDP_ENDIAN
#define BYTE_ORDER _BYTE_ORDER
#endif
'@ | Set-Content -Encoding ascii "$inc\machine\endian.h"
@'
#ifndef _MACHINE_STDARG_H_
#define _MACHINE_STDARG_H_
#include <stdarg.h>
#endif
'@ | Set-Content -Encoding ascii "$inc\machine\stdarg.h"
'#define NISA 0'              | Set-Content -Encoding ascii "$gen\isa.h"
'#define NCOM 0'              | Set-Content -Encoding ascii "$gen\com.h"
'#define NPX 0'               | Set-Content -Encoding ascii "$gen\npx.h"
'#define NWD 0'               | Set-Content -Encoding ascii "$gen\wd.h"
'#define NFD 0'               | Set-Content -Encoding ascii "$gen\fd.h"

# ---------------------------------------------------------------- assym.s
# Build and run the genuine 4.4BSD genassym on the host to get the same
# struct offsets / constants that locore.s expects.
$ga = "$bld\genassym.exe"
& $GCC -B "C:/msys64/mingw32/bin/" -m32 -O1 -std=gnu89 -fno-strict-aliasing `
    -mno-ms-bitfields -I $inc -DKERNEL -o $ga "$src\i386\i386\genassym.c" 2>$null
if ($LASTEXITCODE -ne 0) { throw "genassym build failed" }
$p = Start-Process -FilePath $ga -NoNewWindow -Wait -PassThru -RedirectStandardOutput "$bld\assym.s"
if ($p.ExitCode -ne 0) { throw "genassym run failed" }
if (!(Test-Path "$bld\assym.s")) { throw "genassym run failed" }
@'
/*
 * vector.s - device interrupt stubs.
 * Normally generated by config(8); this kernel attaches no ISA drivers,
 * so the table is empty.  8259 stubs come from i386/isa/icu.s.
 */
'@ | Set-Content -Encoding ascii "$bld\vector.s"

# ---------------------------------------------------------------- userland
# Embed the userland ELF images as C arrays (ramfs contents).
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("/* generated by build_kernel.ps1 */")
foreach ($name in "hello", "init", "sh", "test") {
    $path = Join-Path $ws "out\userland\$name.elf"
    $ub = @()
    if (Test-Path $path) { $ub = [IO.File]::ReadAllBytes($path) }
    [void]$sb.AppendLine("const unsigned char ${name}_elf[] = {")
    for ($i = 0; $i -lt $ub.Length; $i += 16) {
        $end = [Math]::Min($i + 15, $ub.Length - 1)
        $line = ($ub[$i..$end] | ForEach-Object { "0x{0:x2}" -f $_ }) -join ","
        [void]$sb.AppendLine("$line,")
    }
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("const unsigned int ${name}_elf_len = $($ub.Length);")
}
[IO.File]::WriteAllText("$bld\embed.c", $sb.ToString())

# ---------------------------------------------------------------- headers
$cflags = @(
    "-m32", "-std=gnu89", "-O2", "-ffreestanding",
    "-fno-pie", "-fno-pic", "-fcommon", "-fno-builtin",
    "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
    "-fno-unwind-tables", "-fno-tree-loop-distribute-patterns",
    "-march=i386", "-mno-sse", "-mno-sse2", "-mno-mmx", "-mno-avx",
    "-mno-ms-bitfields", "-mpreferred-stack-boundary=2",
    "-ffunction-sections", "-fdata-sections",
    "-nostdinc", "-I", $inc, "-I", $gen, "-I", $src,
    "-isystem", $gccinc, "-DKERNEL", "-B", "C:/msys64/mingw32/bin/"
)

function Compile-C([string]$file, [string]$out) {
    $args = $cflags + @("-c", "-o", $out, $file)
    $log = & $GCC $args 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL $file" -ForegroundColor Red
        $log | Select-Object -First 40 | ForEach-Object { Write-Host "   $_" }
        return $false
    }
    return $true
}

function Assemble([string]$file, [string]$out) {
    $args = @(
        "-m32", "-c", "-x", "assembler-with-cpp",
        "-nostdinc", "-I", $bld, "-I", $inc, "-I", $gen, "-I", $src, "-I", $src,
        "-I", (Join-Path $inc "sys"), "-DKERNEL", "-DLOCORE",
        "-B", "C:/msys64/mingw32/bin/",
        "-o", $out, $file
    )
    $log = & $GCC $args 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL $file" -ForegroundColor Red
        $log | Select-Object -First 40 | ForEach-Object { Write-Host "   $_" }
        return $false
    }
    return $true
}

# ---------------------------------------------------------------- sources
$c_sources = [ordered]@{
    "locore.o"   = @("asm", "$src\i386\i386\locore.s")
    "pmap.o"     = @("c",   "$src\i386\i386\pmap.c")
    "machdep.o"  = @("c",   "$src\i386\i386\machdep.c")
    "cons.o"     = @("c",   "$src\i386\i386\cons.c")
    "bcmp.o"     = @("c",   "$src\libkern\bcmp.c")
    "strcmp.o"   = @("c",   "$src\libkern\strcmp.c")
    "strlen.o"   = @("c",   "$src\libkern\strlen.c")
    "strings.o"  = @("c",   "$ws\kernel\strings.c")
    "usermode.o" = @("c",   "$ws\kernel\usermode.c")
    "usermode_asm.o" = @("asm", "$ws\kernel\usermode_asm.s")
    "embed.o"    = @("c",   "$bld\embed.c")
    "pccon.o"    = @("c",   "$ws\kernel\pccon.c")
    "stubs.o"    = @("c",   "$ws\kernel\stubs.c")
    "glue.o"     = @("c",   "$ws\kernel\glue.c")
    "mon.o"      = @("c",   "$ws\kernel\mon.c")
}

$objs = @()
$fail = 0
foreach ($k in $c_sources.Keys) {
    if ($Only -and $k -ne $Only) { if (Test-Path "$obj\$k") { $objs += "$obj\$k" }; continue }
    $kind, $path = $c_sources[$k]
    $out = "$obj\$k"
    if ($kind -eq "asm") { $ok = Assemble $path $out } else { $ok = Compile-C $path $out }
    if ($ok) { $objs += $out } else { $fail++ }
}

if ($fail) { Write-Host "$fail file(s) failed" -ForegroundColor Red; exit 1 }
if ($NoLink) { Write-Host "objects built"; exit 0 }

# ---------------------------------------------------------------- link
$ldflags = @(
    "-m", "i386pe", "--image-base", "0xFE000000", "-Ttext", "0xFE001000",
    "--subsystem", "console", "-e", "start", "--gc-sections",
    "--file-alignment", "0x1000", "--section-alignment", "0x1000"
)

# pass 1: discover section layout with a harmless _end
& $LD @ldflags "--defsym", "_end=0xFE080000", "--defsym", "_edata=0xFE040000", "--defsym", "_etext=0xFE040000" -o "$bld\kernel.pe" @objs 2>&1 | Select-Object -First 40
if ($LASTEXITCODE -ne 0) { Write-Host "link pass1 failed" -ForegroundColor Red; exit 1 }

$sect = & $OD -h "$bld\kernel.pe" 2>&1
$maxEnd = [long]0
$textEnd = [long]0
$dataEnd = [long]0
foreach ($l in $sect) {
    if ($l -match '^\s*(\d+)\s+(\.\S+)\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+([0-9a-f]{8})') {
        $name = $matches[2]
        $size = [long][Convert]::ToUInt32($matches[3], 16)
        $vma  = [long][Convert]::ToUInt32($matches[4], 16)
        if ($name -eq ".text") { $textEnd = $vma + $size }
        if ($name -eq ".bss")  { $dataEnd = $vma }
        if ($name -ne ".reloc" -and ($vma + $size) -gt $maxEnd) { $maxEnd = $vma + $size }
    }
}
$endAddr = [long](([math]::Floor(($maxEnd + 0xFFF) / 0x1000)) * 0x1000)
if ($textEnd -eq 0) { $textEnd = [long]0xFE001000 }
if ($dataEnd -eq 0) { $dataEnd = $maxEnd }
Write-Host ("sections end {0:x}, _end {1:x}, _edata {2:x}, _etext {3:x}" -f $maxEnd, $endAddr, $dataEnd, $textEnd)

$endSym  = "_end=0x{0:x}"   -f $endAddr
$dataSym = "_edata=0x{0:x}" -f $dataEnd
$textSym = "_etext=0x{0:x}" -f $textEnd
& $LD @ldflags "--defsym", $endSym, "--defsym", $dataSym, "--defsym", $textSym -o "$bld\kernel.pe" @objs 2>&1 | Select-Object -First 40
if ($LASTEXITCODE -ne 0) { Write-Host "link pass2 failed" -ForegroundColor Red; exit 1 }

& $OC -O elf32-i386 -R .idata -R .reloc "$bld\kernel.pe" "$bld\bsd44.elf"
if ($LASTEXITCODE -ne 0) { Write-Host "objcopy failed" -ForegroundColor Red; exit 1 }

Copy-Item "$bld\bsd44.elf" (Join-Path $ws "out\BSD44.ELF") -Force -ErrorAction SilentlyContinue
Write-Host "built $bld\bsd44.elf"
& $OD -h "$bld\bsd44.elf" | Select-Object -First 12









