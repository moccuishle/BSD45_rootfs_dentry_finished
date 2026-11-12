# 用户态路线图：VFS / rootfs / init / execve / BSD sh

目标不是“给内核加一个打印 hello 的 ring3 demo”，而是把 BSD45-ELF-x86 补成
能跑 4.4BSD 用户态的最小 Unix：**VFS + root 文件系统 + 进程/系统调用 +
execve + libc + /bin/sh**，最终 ISO 里是可执行的 i386 ELF 用户程序。

现状（基线）：

- ELF32 内核可从 El Torito ISO 启动，`locore.s`/`pmap.c`/`machdep.c` 为
  4.4BSD-Lite2 原文件；
- `pmap_bootstrap`、`pmap_map`、`pmap_enter` 已验证可用；
- 有 VGA+COM1 控制台和板载 monitor；
- **没有**：vm_page/vm_map 初始化、VFS、进程、系统调用、用户态。

## 分层

```
+-----------------------------------------------------------+
| /sbin/init, /bin/sh, /bin/cat ...   (i386 ELF, 静态)      |
+-----------------------------------------------------------+
| libc 子集: crt0, syscall stubs, stdio, malloc, string,    |
|            ctype, errno, signal/termios stubs             |
+-----------------------------------------------------------+
| syscall: execve exit write read open close fstat brk       |
|          vfork wait4 getpid ioctl chdir getcwd             |
+-----------------------------------------------------------+
| proc: proc0/init, 用户地址空间(pmap), trap/syscall 门      |
+-----------------------------------------------------------+
| vfs: vnode/vnodeops, mount, namei, ramfs (阶段2) -> FFS    |
+-----------------------------------------------------------+
| vm: vm_page + vm_map (阶段1) / 直接 pmap 映射(过渡)        |
+-----------------------------------------------------------+
| pmap (已有), locore, 8259/时钟, 块设备 (阶段4: ATA/ATAPI)  |
+-----------------------------------------------------------+
```

## 阶段与验收

### S0 用户态工具链（本轮完成）

- `build_userland.ps1`：COFF → PE(base 0x08048000) → `objcopy -O elf32-i386`；
- 产出 `out/userland/hello.elf`：`ELF32 EXEC, Intel 80386`，入口
  `0x08048000`，单条 PT_LOAD，`int $0x80` 系统调用。
- 验收：`readelf -h/-l` 正确；无 libc、无 PE 依赖。

### S1 进入用户态 + init + execve（内核核心）

- 物理页分配器（先 bump，后接 `vm_page`）；
- 构造用户页目录：复制内核 PDE（0x3F7–0x3FA），用户段 0x08048000、
  栈 0xBFBFF000 使用 `PG_U|PG_RW|PG_V`；
- `execve(path, argv, envp)`：解析 ELF32 PT_LOAD，建立用户栈；
- `int 0x80` 门（DPL=3），TSS `esp0` 指向内核栈；
- 系统调用：`write(1)`、`exit`、`read(0)`、`brk`；
- 验收：内核从 ramfs 加载 `/sbin/init`，ring3 打印后 `execve("/bin/sh")`，
  sh 能读一行并回显。

### S2 VFS / rootfs / 进程

- vnode/vnodeops、mount 表、namei、ramfs（读写内存文件）；
- initrd：引导器把归档装到内存，内核挂为 root；
- `vfork`/`wait4`（先同步语义，后 COW）、fd 表、pipe、tty 最小实现；
- 验收：`/sbin/init` 读 `/etc/rc`，`sh` 能执行内建命令和 `/bin/echo`。

### S3 libc + 4.4BSD sh ✅（2026-11-13 完成）

- 从 `usr/src/lib/libc` 抽取 sh 需要的子集（string/stdlib/stdio/gen），
  补 `crt0`、`setjmp/longjmp` 与 `int 0x80` syscall stub（`userland/libc/`）；
- 编译 `usr/src/bin/sh`（含 `var.c/exec.c/eval.c/...`），生成
  `nodes.c/syntax.c/builtins.c/init.c/arith.c/arith_lex.c`；
- 验收：ISO 内 `/bin/sh -c 'echo hello; exit 3'` 输出 hello、退出码 3。

实现要点与坑：

- 内核 `execve` 要把 argv/envp 拷到新用户栈（`argc, argv[], NULL, envp[]`）；
- 用户态同样必须 `-march=i386 -mno-sse -mno-mmx`，否则 ring3 执行 SSE 指令
  直接异常；
- 4.4BSD ctype 依赖 rune/locale，改用 4.3BSD-Reno 的经典 `_ctype_` 表；
- sh 强制包含 `<histedit.h>`/链接 libedit，本内核无 termios，用 stub 让
  `el` 保持 NULL，输入退回 `read(2)`；
- `brk` 按页对齐推进；`struct stat` 必须按 4.4BSD i386 的 96 字节布局填。

### S4 真 root 文件系统 ✅（核心完成）

- `kernel/atapi.c`：两条 IDE 通道探测 ATAPI CD-ROM（IDENTIFY PACKET），
  SCSI READ(12) + PIO 读 2048 字节扇区；
- `kernel/isofs.c`：ISO9660 PVD/目录项解析，`/bin/sh` → `BIN/SH.ELF`；
- `execve` 优先从光盘读镜像（`CD_IMAGE_VA 0xFE400000` 临时映射），
  失败回退内嵌 ramfs；
- `build_iso.ps1` 把 `sh.elf/init.elf/hello.elf/test.elf` 放进 ISO；
- 验收：`tools/test_s4.ps1` —— `root: ISO9660 on ATAPI CD` +
  `execve /bin/sh: from ISO9660 (175504 bytes, lba 353)` + `hello` + 退出码 3。

尚未做：`/etc/rc`、`/dev/console`、mount 表、真 vnode/VFS、多 extent 文件。

### S5 进程与外部命令 ✅

- `struct uproc` 进程表（8 槽）+ `copy_address_space()` + `resume_user()`
  （换 CR3、恢复寄存器、`iret`）；系统调用栈按协程式复用；
- `fork/vfork/wait4/exit`：fork 复制地址空间；wait4 阻塞/唤醒；vfork 为
  “复制 + 父进程阻塞到 exec/exit”的安全实现；
- `getpid/getppid` 按进程表；`stat/access` 支持 ISO9660（sh 的 PATH 查找）；
- `userland/echo.c` → `/BIN/ECHO.ELF`，sh 可以 `fork+execve+wait4` 跑它；
- 验收：`tools/test_proc.ps1`（fork 退出 5、vfork+exec echo、wait4 回收）
  以及 `shell` 里连续执行 `/bin/echo`、`$?` 为 0。

尚未做：tty/termios、目录、pipe、页回收、真 vnode/VFS。

### S6 控制台 tty ✅

- 内核 `ioctl`：`TIOCGETA/TIOCSETA*/TIOCGWINSZ/TIOCGPGRP`（fd 0..2）；
- 行规程：回显、退格、`^U`/`^W`/`^D`/`^C`；
- libc `tcgetattr/tcsetattr` 走 ioctl，`isatty()` 因此为真；
- init 交互模式给 sh 传 `-i`，出现 `# ` 提示符；
- 验收：`tools/test_tty.ps1`（isatty=1/1/1、ICANON|ECHO、交互 sh）。

尚未做：目录遍历、pipe/重定向、真 vnode/VFS、页回收。


## 关键接口草案

- 系统调用号沿用 4.4BSD i386：`exit=1`, `read=3`, `write=4`, `open=5`,
  `close=6`, `execve=59`, `brk=17`, `vfork=66`, `wait4=7`;
- `execve` 只接受静态 ELF32（`ET_EXEC`，无 interpreter），拒绝 PE；
- initrd 归档格式：`BFS1` + 目录项 `{name[32], size, offset}` + 数据，
  由 `build_iso.ps1` 生成，引导器装到 `0x200000`；
- 用户栈布局：`argc, argv[], NULL, envp[], NULL, 字符串区`。

## 风险与取舍

| 风险 | 处理 |
|---|---|
| 无 gdb，ring3/页表问题难查 | 串口 + `0xE9` 标记 + QEMU `-d int`；必要时下载便携 gdb |
| `vm_map` 依赖 kmem/zone，移植量大 | S1 先用直接 pmap 映射；S2 再引入 genuine `vm_page`/`vm_map` |
| `fork` COW 复杂 | 先实现同步 `vfork` 语义，sh 的单命令执行足够 |
| 4.4BSD libc 与 GCC 16 冲突 | 只编 sh 需要的子集，缺失函数自行实现 |
| FFS/ATA 驱动工作量大 | 先用 ramfs/initrd 跑通用户态，S4 再做 ISO9660/FFS |

## 本轮产出

- `docs/USERLAND-PLAN.md`（本文件）
- `build_userland.ps1`
- `out/userland/hello.elf`（可执行的用户态 ELF32 样例）
