# BSD4.4_rootfs_filesystem_x86_ISO_ELF_El-Torito

用 **MSYS2 mingw32 的 i386 GCC**（`i686-w64-mingw32-gcc 16.2.0`）编译
**4.4BSD-Lite2 `sys/i386`** 的机器相关核心，生成 **UNIX System V ELF32
(Intel 80386)** 内核，并做成 **El Torito 无仿真引导** 的 ISO。
ISO 里的内核是 **ELF**，不是 PE/EXE。

![screenshot](screenshot.png)

## 产物

| 文件 | 说明 |
|---|---|
| `BSD44-ELF-x86.iso` | 4.5 MB，VMware / QEMU 可直接从光盘启动 |
| `BSD44.ELF` | 内核：`ELF32 EXEC, Intel 80386`，KERNBASE `0xFE000000`，无 SSE/MMX |
| `screenshot.png` | QEMU 中 VGA 文本控制台实拍 |
| `bsd44/` | 完整工程：构建脚本、引导器、内核 glue、文档、VMware vmx |
| `4.4BSD-Lite2.tar.gz` | 原始 4.4BSD-Lite2 源码（构建时自动解包） |

启动后串口/VGA 输出：

```
4.4BSD-Lite2 (i386) ELF kernel
Kernel: ELF32 i386, KERNBASE fe000000, first free page 50000
Memory: maxmem 4000000, physmem 3fa0000, avail 100000-3ffe000
pmap: kernel_pmap 0xfe047010 pdir 0xfe049000 IdlePTD 49000
VM: virtual_avail fe0bd000 virtual_end ff7ff000

BSD/386 monitor - type 'help' for commands
bsd44> mem
Maxmem    16384 pages  (67108864K)
...
bsd44> map
mapped page at 0xfe0bd000
pte new page: 100063
readback 4f534442 444e4521
ok
```

`map` 走的是 4.4BSD 真正的 `pmap_bootstrap_alloc() → pmap_map() →
pmap_enter()` 路径。

## 构建

依赖（本机路径）：

```
C:\msys64\mingw32\bin\gcc.exe        i686-w64-mingw32-gcc 16.2.0
C:\msys64\mingw32\bin\ld.exe          GNU ld 2.47 (i386pe)
C:\msys64\mingw32\bin\objcopy.exe     支持 -O elf32-i386
C:\Program Files (x86)\VMware\VMware Workstation\mkisofs.exe
C:\Program Files\qemu\qemu-system-i386.exe
```

```powershell
cd bsd44
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_all.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\verify.ps1
```

`build_kernel.ps1` 会自动：

1. 若 `src/` 不存在，把上层目录的 `4.4BSD-Lite2.tar.gz` 解包；
2. 把 `overlay/` 里针对现代 GCC 修补过的 4 个文件覆盖上去；
3. 准备 `machine/ sys/ vm/ i386/ libkern/` 头文件视图；
4. `gcc -m32 -c` → `ld -m i386pe --image-base 0xFE000000` →
   `objcopy -O elf32-i386`，得到 `ET_EXEC` ELF32；
5. 用 `--defsym _end/_edata/_etext` 补齐 `locore.s` 需要的边界符号。

工具链关键点：mingw 的 `ld` 只有 `i386pe` 仿真，不能直接
`--oformat elf32-i386`；必须先链接成 PE 再 `objcopy` 成 ELF。
`gcc -B C:/msys64/mingw32/bin/` 必须有，否则驱动找不到 `as.exe`。

## 引导链

```
BIOS → El Torito (no-emulation) 加载 boot.img 到 0x7C00
  stage1 (512 B, 实模式)
    - 从镜像内 BDMF manifest 读内核 LBA (2048) 和字节数
    - INT 13h AH=42h，按 CD 的 2048 字节扇区，每次 16 扇区读入 0x10000
    - 复制 stage2 到 0x90000，开 A20，装 GDT，进保护模式
  stage2 (32 位, 链接在 0x90000)
    - 快照 ELF 头到 0x8F000
    - 每个 PT_LOAD: dst = p_vaddr - 0xFE000000，rep movsb，清 bss
    - 压 howto/bootdev/unit/cyloffset，跳 e_entry - 0xFE000000
内核 locore.s:start
    - 自建 GDT/页表，物理 0..kernel_end 映射到 0xFE000000+
    - call init386: GDT/IDT/LDT/TSS、RTC 测内存、pmap_bootstrap
    - main() → 板载 monitor
```

## 4.4BSD 原文件

以下文件来自 `sys/i386`，未重写：

- `i386/i386/locore.s`（Jolitz）
- `i386/i386/pmap.c`（Mach pmap，Reno i386 版）
- `i386/i386/machdep.c`（`init386`、`setidt` 等）
- `i386/i386/cons.c`（控制台抽象层）
- `i386/isa/icu.s`（8259、`spl*`、`doreti`）

`kernel/` 下是本项目补的 glue：`main`、kprintf、`proc0`、VGA+COM1 驱动、
trap、monitor、少量 stub。

## 现代 GCC 修补（overlay/）

| 文件 | 修补 | 原因 |
|---|---|---|
| `locore.s` | `ENTRY/ALTENTRY/IDTVEC` 改 `_##name`；`movb %eax`→`%al`；`movl %ax,%ds`→`movw` | 现代 cpp 把 `/**/` 变成空格；gas 拒绝寄存器宽度不匹配 |
| `icu.s`（构建时 include） | 增加 `#include "icu.h"`、`"isa.h"` | 原构建由 config 提供 `IO_ICU1` 等常量 |
| `pmap.c` | `static boolean_t pmap_testbit();` | C99 起 static 后置声明是错误 |
| `machdep.c` | `sendsig(..., u_long code)`、`const char *panicstr` | 与头文件原型一致 |
| `machdep.c` | `lgdt(&r_gdt); lidt(&r_idt);` | 本树 `lgdt` 只接受 `region_descriptor *` |
| `segments.h` | `region_descriptor` 6 字节 packed；`segment_descriptor` 两个 32 位字 | GCC 16 的 16+24 位域跨 32 位单元，GDT 步长错 |

详细清单与构建参数见 [`bsd44/README.md`](bsd44/README.md)。

## 启动

```powershell
# QEMU
& "C:\Program Files\qemu\qemu-system-i386.exe" `
  -cdrom BSD44-ELF-x86.iso -boot d -m 64 -serial stdio

# VMware: File -> Open -> bsd44\vms\BSD44-ELF-x86.vmx
```

VMware 提示：启动后先**点一下 VM 窗口**再敲键盘（VMware 需要捕获输入）。
没有配置串口的 VM，启动行会显示 `Console: VGA 80x25, COM1 absent`，这是正常的；
早期 ISO 在这种情况下键盘会失灵（VMware 未映射端口读回 `0xff`，被误判成串口有数据），
现已修复并加了 `tools/test_kbd.ps1` 回归。

## 边界说明

这是一颗 **可启动的 4.4BSD-Lite2 i386 内核 + 最小用户态**：保护模式、分页、
GDT/IDT/TSS、物理内存探测、Mach pmap 引导、可交互 monitor 均已跑通。

S1–S4 之后，ISO 的内核会自己用 ATAPI(PIO) 读光盘、挂上 **ISO9660 root**，
`/sbin/init` 与**真正的 4.4BSD `/bin/sh`** 都是光盘上的文件（`/SBIN/INIT.ELF`、
`/BIN/SH.ELF`），不再是内核数组；monitor 里 `usermode` 执行
`sh -c 'echo hello; exit 3'`，`shell` 进入交互 sh，`cd` 显示挂载信息。

它**还不是**完整系统：没有 `fork/wait`（外部命令不能执行）、没有 tty
（无行编辑/作业控制）、没有真 vnode/VFS（root 是只读 ISO9660，ramfs 只作
无盘兜底）。“ISO 内装 ELF 内核 + El Torito 引导、内核不是 PE”与
“从光盘挂 root 跑 BSD `/bin/sh`”这两个目标都已达成。


## 许可

`4.4BSD-Lite2.tar.gz` 与 `bsd44/overlay/` 中的 4.4BSD 代码遵循其原始
BSD 版权许可以及 UC Berkeley 的 advertising 条款；本项目新增的脚本、
引导器与 glue 代码可视为同一 BSD 风格许可。

## 从仓库恢复 / 重建

```bash
# 防火墙可能会重置 HTTPS 大文件传输，用 SSH 更稳（本机已配置 key）
git clone git@github.com:moccuishle/BSD4.4_rootfs_filesystem_x86_ISO_ELF_El-Torito-.git
cd BSD4.4_rootfs_filesystem_x86_ISO_ELF_El-Torito-/bsd44
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_all.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\verify.ps1
```

仓库自带 `4.4BSD-Lite2.tar.gz`、`overlay/` 与全部脚本，构建时会自动解包并
应用修补，不需要再找源码。产物 `BSD44-ELF-x86.iso` / `BSD44.ELF` 也已在
仓库根目录，即使不重新编译也能直接启动。

### 在另一台 Windows 电脑上

脚本会自动查找工具链，不再依赖固定路径（`bsd44/tools-find.ps1`）：

| 工具 | 自动查找顺序 / 覆盖方式 |
|---|---|
| MSYS2 mingw32 | `%MSYS2_ROOT%`、`C:\msys64`、`C:\msys2`、`%ProgramFiles%\msys64`、PATH 里的 `i686-w64-mingw32-gcc` |
| GCC include | `gcc -print-file-name=include`（自动跟随实际 GCC 版本） |
| mkisofs | `%MKISOFS%`、VMware Workstation/Player、`C:\msys64\usr\bin\mkisofs.exe` 或 `xorriso.exe`、PATH |
| QEMU | `%QEMU%`、`%ProgramFiles%\qemu\...`、PATH；找不到时 `verify.ps1` 跳过运行时检查并返回成功 |

安装（任选）：

```powershell
# MSYS2: https://www.msys2.org
pacman -S mingw-w64-i686-gcc mingw-w64-i686-binutils

# ISO 工具：任选其一
pacman -S cdrtools      # 或 xorriso
# 或者安装 VMware Workstation，用自带的 mkisofs.exe

# 可选，仅 verify.ps1 需要
# 安装 QEMU for Windows
```

GCC 版本不必是 16.2.0：include 路径由 `gcc -print-file-name=include` 得到。
不过内核是 1994 年的 K&R 代码，更新的 GCC 可能引入新的诊断；仓库默认参数在
**16.2.0** 上验证通过。

**不装工具链也能用**：仓库里已有 `BSD44-ELF-x86.iso` 与 `BSD44.ELF`，
另一台电脑只要装 VMware 或 QEMU 就能直接启动，不需要编译。

**Linux / macOS**：这些 `.ps1` 脚本不适用。需要 `i686-w64-mingw32-*`
交叉工具链（或 i686-elf-gcc）并改写构建命令；COFF→PE→objcopy 的思路不变，
`overlay/` 里的源码修补与引导器可以直接复用。

## 用户态进度（S4）

- 已完成：ring3、用户页目录/栈、`execve`（含 argv/envp）、`int 0x80` 系统调用、
  4.4BSD libc 子集、**真正的 4.4BSD `/bin/sh`**、**ATAPI + ISO9660 root**；
- monitor 命令：
  - `usermode` → 从光盘加载 `init -c 'echo hello; exit 3'`（S3/S4 验收路径）；
  - `shell` → 交互 `/bin/sh`（内建命令、`$(( ))` 算术、退出码传递）；
  - `test` → libc 冒烟测试（printf/malloc/string）；
  - `cd` → 打印 ISO9660 挂载信息（root 目录 LBA/大小）；
- 启动日志会打印 `atapi: <设备型号>`、`atapi: capacity N sectors`、
  `root: ISO9660 on ATAPI CD, root dir lba N, N bytes`；
- 仍然没有：`fork/wait`（跑不了外部命令）、tty/termios、真 vnode/VFS
  （ISO9660 只读、无目录遍历）。路线见
  [`bsd44/docs/USERLAND-PLAN.md`](bsd44/docs/USERLAND-PLAN.md)。

用户态程序用 `bsd44/build_userland.ps1`（小工具）与 `bsd44/build_sh.ps1`
（libc + 4.4BSD sh）构建：COFF → PE(0x08048000) → `objcopy -O elf32-i386`；
`build_iso.ps1` 再把它们放进 ISO 的 `/BIN`、`/SBIN`。用户态必须
`-march=i386 -mno-sse`。
