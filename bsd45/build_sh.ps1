# build_sh.ps1 - build the 4.4BSD /bin/sh (and a libc smoke test) as a static
# i386 ELF for BSD45-ELF-x86.
#
#	COFF objects -> PE (image base 0x08048000) -> objcopy -O elf32-i386
#
# Sources: 4.4BSD-Lite2 usr/src/bin/sh plus a port of the libc subset it
# needs (libc_sys.c supplies the int 0x80 syscall layer).
param(
    [string]$Target = "sh",     # "sh" or "test"
    [switch]$KeepObjects
)

$ErrorActionPreference = "Continue"
$ws   = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ws "tools-find.ps1")
$bin  = Get-MingwBin
$GCC  = "$bin\gcc.exe"; $LD = "$bin\ld.exe"; $OC = "$bin\objcopy.exe"
$gccinc = Get-GccInclude $bin

$usr   = Join-Path $ws "src\4.4BSD-Lite2\usr\src"
$shsrc = Join-Path $usr "bin\sh"
$lc    = Join-Path $usr "lib\libc"
$bld   = Join-Path $ws "build"
$incu  = Join-Path $bld "incu"
$gen   = Join-Path $bld "shgen"
$obj   = Join-Path $bld "shobj"
$outd  = Join-Path $ws "out\userland"
New-Item -ItemType Directory -Force -Path $bld, $incu, $gen, $obj, $outd | Out-Null

function Write-Step([string]$s) { Write-Host "== $s" }

# ---------------------------------------------------------------- headers
function Prepare-Headers {
    New-Item -ItemType Directory -Force -Path $incu | Out-Null
    Copy-Item (Join-Path $ws "userland\include\stdarg.h") "$incu\stdarg.h" -Force
    Copy-Item (Join-Path $ws "userland\libc\ctype.h") "$incu\ctype.h" -Force
    Copy-Item (Join-Path $ws "userland\libc\histedit.h") "$incu\histedit.h" -Force
    if (Test-Path (Join-Path $incu "errno.h")) { return }
    Write-Step "preparing libc header view"
    New-Item -ItemType Directory -Force -Path "$incu\sys", "$incu\machine" | Out-Null
    Get-ChildItem (Join-Path $usr "include") -File -Filter *.h | Copy-Item -Destination $incu -Force
    Get-ChildItem (Join-Path $usr "sys\sys") -File -Filter *.h | ForEach-Object {
        $dst = Join-Path $incu $_.Name
        if (!(Test-Path $dst)) { Copy-Item $_.FullName $dst -Force }
    }
    Get-ChildItem (Join-Path $usr "sys\sys") -File -Filter *.h | Copy-Item -Destination "$incu\sys" -Force
    Get-ChildItem (Join-Path $usr "sys\i386\include") -File -Filter *.h | Copy-Item -Destination "$incu\machine" -Force
    Copy-Item (Join-Path $usr "sys\hp300\include\ansi.h") "$incu\machine\ansi.h" -Force
    Copy-Item (Join-Path $usr "sys\hp300\include\limits.h") "$incu\machine\limits.h" -Force
    Copy-Item (Join-Path $usr "sys\hp300\include\float.h") "$incu\machine\float.h" -Force
    Copy-Item (Join-Path $usr "sys\sys\termios.h") "$incu\termios.h" -Force
    Copy-Item (Join-Path $ws "userland\include\stdarg.h") "$incu\stdarg.h" -Force
    Copy-Item (Join-Path $ws "userland\libc\ctype.h") "$incu\ctype.h" -Force
    Copy-Item (Join-Path $ws "userland\libc\histedit.h") "$incu\histedit.h" -Force
}
Prepare-Headers

# ---------------------------------------------------------------- flags
$common = @(
    "-m32", "-std=gnu89", "-O2", "-ffreestanding", "-fno-builtin",
    "-fno-pie", "-fno-pic", "-fno-stack-protector",
    "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
    "-fno-strict-aliasing", "-fcommon",
    "-march=i386", "-mno-sse", "-mno-sse2", "-mno-mmx", "-mno-avx",
    "-mpreferred-stack-boundary=2",
    "-nostdinc", "-I", $incu, "-I", $gen, "-I", $shsrc,
    "-isystem", $gccinc, "-DSHELL",
    "-B", "C:/msys64/mingw32/bin/"
)

$fail = 0
function Compile-C([string]$file, [string]$out, [string[]]$extra = @()) {
    if (!(Test-Path $file)) { Write-Host "  missing $file" ; $script:fail++; return $false }
    $log = & $GCC @common @extra -c -o $out $file 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL $(Split-Path $file -Leaf)" -ForegroundColor Red
        $log | Select-Object -First 12 | ForEach-Object { Write-Host "   $_" }
        $script:fail++
        return $false
    }
    return $true
}
function Compile-Asm([string]$file, [string]$out) {
    $log = & $GCC -m32 -x assembler-with-cpp -nostdinc -I $incu `
        -B "C:/msys64/mingw32/bin/" -c -o $out $file 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL $(Split-Path $file -Leaf)" -ForegroundColor Red
        $log | Select-Object -First 12 | ForEach-Object { Write-Host "   $_" }
        $script:fail++
        return $false
    }
    return $true
}

$objs = @()

# ---------------------------------------------------------------- startup
Write-Step "startup objects"
if (Compile-Asm (Join-Path $ws "userland\libc\crt0.s") "$obj\crt0.o") { $objs += "$obj\crt0.o" }
if (Compile-Asm (Join-Path $ws "userland\libc\jmp.s")  "$obj\jmp.o")  { $objs += "$obj\jmp.o" }

# ---------------------------------------------------------------- our libc glue
Write-Step "port glue"
if (Compile-C (Join-Path $ws "userland\libc\libc_sys.c") "$obj\libc_sys.o") { $objs += "$obj\libc_sys.o" }
if (Compile-C (Join-Path $ws "userland\libc\dtoa.c") "$obj\dtoa.o") { $objs += "$obj\dtoa.o" }
if (Compile-C (Join-Path $ws "userland\libc\histedit_stub.c") "$obj\histedit_stub.o") { $objs += "$obj\histedit_stub.o" }

# ---------------------------------------------------------------- 4.4BSD libc
Write-Step "4.4BSD libc subset"
$libc_files = @(
    "string\strcat.c","string\strcmp.c","string\strcpy.c",
    "string\strcspn.c","string\strdup.c","string\strlen.c","string\strncat.c",
    "string\strncmp.c","string\strncpy.c","string\strpbrk.c",
    "string\strsep.c","string\strspn.c","string\strstr.c","string\strtok.c",
    "string\strcasecmp.c","string\strerror.c",
    "string\memchr.c","string\memcmp.c",
    "stdlib\atoi.c","stdlib\atol.c","stdlib\strtol.c",
    "stdlib\strtoul.c","stdlib\malloc.c",
    "stdlib\calloc.c","stdlib\exit.c","stdlib\atexit.c","stdlib\getenv.c",
    "stdlib\qsort.c","stdlib\abs.c","stdlib\div.c","stdlib\ldiv.c","stdlib\rand.c",
    "stdlib\abort.c","stdlib\getopt.c","gen\setmode.c","gen\wait3.c","gen\exec.c",
    "stdio\findfp.c","stdio\flags.c","stdio\fclose.c","stdio\fopen.c",
    "stdio\fprintf.c","stdio\fgets.c","stdio\fputs.c","stdio\fread.c",
    "stdio\fwrite.c","stdio\fputc.c","stdio\putc.c","stdio\putchar.c",
    "stdio\printf.c","stdio\vfprintf.c","stdio\sprintf.c","stdio\snprintf.c",
    "stdio\vsnprintf.c","stdio\vsprintf.c","stdio\perror.c","stdio\fflush.c",
    "stdio\refill.c","stdio\rget.c","stdio\wbuf.c","stdio\wsetup.c",
    "stdio\makebuf.c","stdio\setvbuf.c","stdio\fwalk.c","stdio\fpurge.c",
    "stdio\fseek.c","stdio\ftell.c","stdio\rewind.c","stdio\fdopen.c",
    "stdio\fileno.c","stdio\feof.c","stdio\ferror.c","stdio\clrerr.c",
    "stdio\ungetc.c","stdio\gets.c","stdio\puts.c","stdio\getc.c",
    "stdio\getchar.c","stdio\fgetc.c",
    "gen\isatty.c",
    "stdio\stdio.c","stdio\fvwrite.c"
)
foreach ($f in $libc_files) {
    $src = Join-Path $lc $f
    $o = "$obj\lc_" + ((Split-Path $f -Leaf) -replace '\.c$', '.o')
    $extra = @()
    if ($f -like "stdio\*") { $extra = @("-I", (Join-Path $lc "stdio")) }
    if (Compile-C $src $o $extra) { $objs += $o }
}

# Files that 4.4BSD compiles several times with different macros.
$multi = @(
    @{ f = "string\bcopy.c";   d = @();           o = "bcopy" },
    @{ f = "string\bcopy.c";   d = @("-DMEMCOPY"); o = "memcpy" },
    @{ f = "string\bcopy.c";   d = @("-DMEMMOVE"); o = "memmove" },
    @{ f = "string\memset.c";  d = @();           o = "memset" },
    @{ f = "string\memset.c";  d = @("-DBZERO");   o = "bzero" },
    @{ f = "string\index.c";   d = @();           o = "index" },
    @{ f = "string\index.c";   d = @("-DSTRCHR");  o = "strchr" },
    @{ f = "string\rindex.c";  d = @();           o = "rindex" },
    @{ f = "string\rindex.c";  d = @("-DSTRRCHR"); o = "strrchr" },
    @{ f = "string\swab.c";    d = @();           o = "swab" }
)
foreach ($m in $multi) {
    $src = Join-Path $lc $m.f
    if (!(Test-Path $src)) { continue }
    if (Compile-C $src "$obj\lc_$($m.o).o" $m.d) { $objs += "$obj\lc_$($m.o).o" }
}
# Classic (pre-rune) ctype table, from 4.3BSD-Reno.
if (Compile-C (Join-Path $ws "userland\libc\errlst.c") "$obj\lc_errlst.o") { $objs += "$obj\lc_errlst.o" }
if (Compile-C (Join-Path $ws "userland\libc\ctype_table.c") "$obj\ctype_table.o") { $objs += "$obj\ctype_table.o" }

if ($fail) { Write-Host "$fail file(s) failed" -ForegroundColor Red; exit 1 }

# libc-only object set (used for /bin/echo too)
$libobjs = @($objs)

# ---------------------------------------------------------------- sh sources
if ($Target -eq "sh") {
    Write-Step "generating sh sources"
    Get-ChildItem $shsrc -File |
        Where-Object { -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) } |
        ForEach-Object { Copy-Item $_.FullName (Join-Path $gen $_.Name) -Force -ErrorAction SilentlyContinue }
    Copy-Item (Join-Path $ws "userland\libc\*") $gen -Force -ErrorAction SilentlyContinue

    $bash = Join-Path (Find-Msys2Root) "usr\bin\bash.exe"
    Push-Location $gen
    & $bash -c "sh mktokens" 2>&1 | Select-Object -First 5
    if (!(Test-Path "$gen\token.def")) { Write-Host "mktokens failed" -ForegroundColor Red; $fail++ }
    & $bash -c "sh mkbuiltins ." 2>&1 | Select-Object -First 5
    if (!(Test-Path "$gen\builtins.c")) { Write-Host "mkbuiltins failed" -ForegroundColor Red; $fail++ }

    Write-Step "generating nodes/syntax/init"
    $shim = Join-Path $ws "userland\hostgen\hostshim.h"
    $hflags = @("-std=gnu89", "-O1", "-B", "C:/msys64/mingw32/bin/",
        "-include", $shim, "-I", $shsrc)
    & $GCC @hflags -o "$gen\mknodes.exe" (Join-Path $ws "userland\hostgen\mknodes.c") 2>&1 |
        Where-Object { $_ -match 'error' } | Select-Object -First 5
    & $GCC @hflags -o "$gen\mksyntax.exe" (Join-Path $shsrc "mksyntax.c") 2>&1 |
        Where-Object { $_ -match 'error' } | Select-Object -First 5
    & $GCC @hflags -o "$gen\mkinit.exe" (Join-Path $shsrc "mkinit.c") (Join-Path $ws "userland\hostgen\hoststub.c") 2>&1 |
        Where-Object { $_ -match 'error' } | Select-Object -First 5
    if (!(Test-Path "$gen\mknodes.exe") -or !(Test-Path "$gen\mksyntax.exe") -or !(Test-Path "$gen\mkinit.exe")) {
        Write-Host "host tool build failed" -ForegroundColor Red
        $fail++
    } else {
    & "$gen\mknodes.exe" nodetypes nodes.c.pat 2>&1 | Select-Object -First 5
    & "$gen\mksyntax.exe" 2>&1 | Select-Object -First 5
    $srcs = (Get-ChildItem $shsrc -File -Filter *.c | Where-Object { $_.Name -notin @("mknodes.c","mksyntax.c","mkinit.c") } | ForEach-Object { $_.Name })
    & "$gen\mkinit.exe" "cc -c init.c" @srcs *>&1 | Out-Null   # exits 1: it cannot exec /bin/sh on the host
    }
    Pop-Location
    if (!(Test-Path "$gen\init.c")) { Write-Host "mkinit failed" -ForegroundColor Red; $fail++ }
    if (!(Test-Path "$gen\nodes.c")) { Write-Host "mknodes failed" -ForegroundColor Red; $fail++ }
    if (!(Test-Path "$gen\syntax.c")) { Write-Host "mksyntax failed" -ForegroundColor Red; $fail++ }

    Write-Step "bison/flex"
    Push-Location $gen
    # 4.4BSD's arith.y marks every action with "= {" (a local yacc dialect);
    # plain bison wants "{".  Old yacc also let yyerror() use yyerrok/yyclearin,
    # which bison scopes to yyparse, so drop those two statements.
    (Get-Content arith.y) -replace '= \{', '{' |
        Where-Object { $_ -notmatch '^\s*(yyerrok|yyclearin);' } |
        Set-Content arith_sane.y -Encoding ASCII
    & $bash -c "bison -y -d arith_sane.y" 2>&1 | Select-Object -First 8
    if (Test-Path y.tab.c) { Copy-Item y.tab.c arith.c -Force }
    & $bash -c "flex -o arith_lex.c arith_lex.l" 2>&1 | Select-Object -First 8
    Pop-Location

    Write-Step "compiling sh"
    $sh_c = @(
        "alias.c","cd.c","error.c","eval.c","exec.c","expand.c","input.c",
        "jobs.c","mail.c","main.c","memalloc.c","miscbltin.c","mystring.c",
        "options.c","parser.c","redir.c","show.c","trap.c","output.c","var.c",
        "builtins.c","init.c","nodes.c","syntax.c","arith.c","arith_lex.c"
    )
    foreach ($f in $sh_c) {
        $o = "$obj\sh_" + ($f -replace '\.c$', '.o')
        if (Compile-C (Join-Path $gen $f) $o) { $objs += $o }
    }
    foreach ($f in @("bltin\echo.c")) {
        $o = "$obj\sh_" + ($f -replace '.*\\', '' -replace '\.c$', '.o')
        if (Compile-C (Join-Path $shsrc $f) $o) { $objs += $o }
    }
    $printf = Join-Path $usr "usr.bin\printf\printf.c"
    if (Test-Path $printf) {
        if (Compile-C $printf "$obj\sh_printf.o" @("-I", (Join-Path $usr "usr.bin\printf"))) { $objs += "$obj\sh_printf.o" }
    }
} else {
    Write-Step "compiling libc test program"
    if (Compile-C (Join-Path $ws "userland\t_libc.c") "$obj\t_libc.o") { $objs += "$obj\t_libc.o" }
}

if ($fail) { Write-Host "$fail file(s) failed" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------- link
Write-Step "linking"
$pe  = "$bld\sh.pe"
$elf = "$outd\sh.elf"
if ($Target -eq "test") { $elf = "$outd\test.elf"; $pe = "$bld\test.pe" }
$libgcc = (& $GCC -print-libgcc-file-name).Trim()
& $LD -m i386pe -Ttext 0x08048000 -e _start --subsystem console -o $pe @objs $libgcc 2>&1 | Select-Object -First 30
if ($LASTEXITCODE -ne 0) { Write-Host "link failed" -ForegroundColor Red; exit 1 }
& $OC -O elf32-i386 -R .idata -R .reloc $pe $elf
if ($LASTEXITCODE -ne 0) { Write-Host "objcopy failed" -ForegroundColor Red; exit 1 }
Write-Host ("built {0} ({1:N0} bytes)" -f $elf, (Get-Item $elf).Length)

if ($Target -eq "sh") {
    # /bin/echo: the first external command, linked against the same libc.
    $echoobj = "$obj\echo.o"
    if (Compile-C (Join-Path $ws "userland\echo.c") $echoobj) {
        $eobjs = @($libobjs) + @($echoobj)
        & $LD -m i386pe -Ttext 0x08048000 -e _start --subsystem console -o "$bld\echo.pe" @eobjs $libgcc 2>&1 | Select-Object -First 20
        if ($LASTEXITCODE -eq 0) {
            & $OC -O elf32-i386 -R .idata -R .reloc "$bld\echo.pe" "$outd\echo.elf"
            if ($LASTEXITCODE -eq 0) {
                Write-Host ("built {0} ({1:N0} bytes)" -f "$outd\echo.elf", (Get-Item "$outd\echo.elf").Length)
            }
        }
    }
}

