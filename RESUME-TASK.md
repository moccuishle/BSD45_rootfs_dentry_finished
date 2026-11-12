# BSD45-ELF-x86 任务恢复文本

更新时间：2026-11-13（Asia/Shanghai）
状态：**S5 完成**（fork/vfork/wait4，sh 可跑外部命令）。
下一步：tty/termios、目录支持、真 vnode/VFS。
仓库：https://github.com/moccuishle/BSD45_rootfs_shell_finished

本机环境（网吧 Windows，无管理员、防火墙不可关）：

| 组件 | 位置 |
|---|---|
| i686 工具链 | `C:\msys64\mingw32\bin`（GCC 16.2.0，pacman 装的 mingw-w64-i686-gcc） |
| mkisofs | `C:\Program Files (x86)\VMware\VMware Workstation\mkisofs.exe` |
| QEMU | `C:\msys64\ucrt64\bin\qemu-system-i386.exe`（11.1.3） |
| 4.4BSD 源码 | `src\4.4BSD-Lite2`（由 `..\4.4BSD-Lite2.tar.gz` 解出） |
| 4.3BSD-Reno 源码 | `C:\Users\Administrator\Desktop\work\reno-src-main`（ctype 等参考） |

> 注意：`C:\msys64\mingw32\i686-w64-mingw32\bin\as.exe` / `ld.exe` 是坏的
> （0xC0000139/0xC0000135）。所有编译命令必须带 `-B C:/msys64/mingw32/bin/`，
> 构建脚本里的 `Get-MingwBin` + `-B` 已处理。

---

## 1. 任务目标（原始需求）

在 4.4BSD-Lite2 i386 ELF 内核上实现 Solaris 式的 VFS/root 文件系统、
`init(8)` 与 `execve`，把 4.4BSD libc/sh 交叉编译成 i386 ELF 用户程序放进
ISO，最终 ISO 启动后进入用户态运行 BSD 的 `/bin/sh`。

硬约束：
- 用 mingw32 **i386** GCC 编译；
- 产物是 UNIX ELF32（不是 PE），ISO 用 El Torito 引导；
- 不使用 SIMD（`-march=i386 -mno-sse -mno-mmx`，用户态同样适用）。

## 2. 完成度

| 阶段 | 内容 | 状态 |
|---|---|---|
| S0 | 用户态 ELF 交叉编译链（COFF→PE→objcopy） | ✅ |
| S1 | ring3、用户页目录/栈、ELF32 装载、`int 0x80` | ✅ |
| S2 | ramfs、fd 表、`execve`（含 argv/envp）、`/sbin/init` → `/bin/sh` | ✅ |
| S3 | 4.4BSD libc 子集 + **真正的 BSD `/bin/sh`** | ✅ 本次完成 |
| S4 | ATAPI(PIO) + ISO9660 只读 root；`/sbin/init`、`/bin/sh` 是光盘文件 | ✅ 本次完成（核心） |
| S5 | fork/vfork/wait4、进程表与调度、`/bin/echo` 外部命令 | ✅ 本次完成 |

### S3 验收（`powershell -File tools\test_s3.ps1`）

```
bsd45> usermode
exec /sbin/init: entry 8048000 pd 200000
init: starting /bin/sh
execve /bin/sh: entry 8048000 esp bfbfffb8 pd 20e000 argc 3
hello
[user exit 3]
```

交互模式（monitor 输入 `shell`）：

```
bsd45> shell
execve /bin/sh: entry 8048000 esp bfbfffd8 pd 20e000 argc 1
echo hi there
hi there
echo $((2+3))
5
exit 7
[user exit 7]
```

即：ISO 里的 `/bin/sh` 已经是 **4.4BSD-Lite2 `usr/src/bin/sh`** 编译出来的
i386 ELF（175 KB），不是项目早期的最小内置 sh；`-c`、交互、`echo`、
算术展开 `$(( ))`、退出码传递都跑通。


### S4 验收（`powershell -File tools\test_s4.ps1`）

```
atapi: QEMU     QEMU DVD-ROM
atapi: capacity 2332 sectors
root: ISO9660 on ATAPI CD, root dir lba 24, 2048 bytes

bsd45> cd
ISO9660 root: lba 24, 2048 bytes
bsd45> usermode
exec /sbin/init: entry 8048000 pd 200000
init: starting /bin/sh
execve /bin/sh: from ISO9660 (175504 bytes, lba 353)
execve /bin/sh: entry 8048000 esp bfbfffb8 pd 239000 argc 3
hello
[user exit 3]
```

关键点：`/bin/sh` 不再来自内核数组，而是 **ISO9660 光盘上的 `/BIN/SH.ELF`**；
`execve` 里打印的 `from ISO9660 (… lba 353)` 就是证据。VMware 里同样通过
（`NECVMWar VMware IDE CDR10`）。

### S4 实现要点

- `kernel/atapi.c`：两条 IDE 通道 × 主/从 探测 ATAPI（IDENTIFY PACKET +
  word0 判定 CD-ROM），用 SCSI READ(12) 走 PIO 读 2048 字节扇区；
- `kernel/isofs.c`：读 LBA 16 的 PVD，校验 `CD001`，从 root 目录记录
  逐扇区走目录项；路径按“大写 + 无扩展名补 `.ELF`”映射
  （`/bin/sh` → `BIN/SH.ELF`，`/sbin/init` → `SBIN/INIT.ELF`）；
- `usermode.c`：`execve` 优先从 ISO9660 读文件（读到 `CD_IMAGE_VA 0xFE400000`
  的临时内核映射，按页 `pmap_map`），失败才回退到内嵌 ramfs；
- `build_iso.ps1`：把 `sh.elf/init.elf/hello.elf/test.elf` 放进 ISO 的
  `BIN/`、`SBIN/` 和根目录；内核里仍保留一份 ramfs 作为无盘启动的兜底；
- monitor 新增 `cd` 命令查看挂载信息。


### S5 验收（`powershell -File tools\test_proc.ps1`）

```
bsd45> test
exec /test: from ISO9660 (70588 bytes)
S:fork
C:fork
fork waited pid=2 code=5
S:vfork
execve /bin/echo: from ISO9660 (70412 bytes, lba 239)
vfork-exec-ok
P:vfork
vfork waited pid=3 code=0
done
[user exit 0]
```

sh 里跑外部命令（fork → exec → wait4 全部走通，`$?` 正确）：

```
bsd45> shell
execve /bin/sh: from ISO9660 (175504 bytes, lba 274)
/bin/echo one
execve /bin/echo: from ISO9660 (70412 bytes, lba 239)
one
/bin/echo two
two
echo st=$?
st=0
exit 0
[user exit 0]
```

### S5 实现要点

- `struct uproc` 进程表（8 槽）：pid/ppid、页目录、`brk` 水位、fd 表、
  保存的 `sysregs` 上下文、退出码、等待状态；
- `copy_address_space()`：新建 PD/PT，逐页复制用户地址空间（内核高半区共享）；
- `resume_user(pd, sysregs *)`（`usermode_asm.s`）：换 CR3、恢复通用寄存器、
  `iret` 回 ring3。系统调用栈是协程式复用的（每次 `int 0x80` 从 TSS esp0
  重新开始），不用保存内核栈；
- `fork` 复制地址空间、子进程 `eax=0`；`wait4` 若无僵尸子进程则阻塞父进程，
  子进程 `exit` 时写回状态并唤醒父进程（`reaped` 标记：被唤醒的父进程直接
  收走僵尸）；`vfork` = 复制地址空间 + 父进程阻塞到子进程 exec/exit；
- `getpid/getppid/kill` 按进程表返回；`stat/access` 现在也会查 ISO9660，
  这样 sh 的 `PATH` 查找能找到 `/bin/echo`。

## 3. S3 做了什么

### 3.1 内核（`kernel/usermode.c`）

- `execve(path, argv, envp)` 现在把 argv/envp **复制到新用户栈**：
  `argc, argv[], NULL, envp[], NULL, 字符串`，ESP 指向 `argc`；
- `brk`/`sbrk` 真正的堆：按页映射，`brk_mapped` 是**页对齐**的映射水位
  （早期版本用它未对齐的地址当起点，会重复映射同一页，malloc 立刻 #PF）；
- `struct stat` 按 4.4BSD i386 布局（96 字节）填充，`st_size` 在偏移 48
  （`off_t` 是 64 位）；早期按 128 字节乱写会踩坏调用者的栈；
- syscall 表补齐：`read/write/open/close/execve/exit/break/getpid/getppid/
  uid-gid/getpgrp/setpgid/access/signal 类/ioctl(ENOTTY)/umask/fcntl/
  stat(189)/fstat(188)/lseek(199)/getdtablesize/wait4(ECHILD)/dup2/
  mprotect/munmap/msync`；错误统一返回**负 errno**；
- 新增 `/test` ramfs 项与 `start_prog(path, script)`，monitor 里 `test`
  直接跑 libc 冒烟测试程序。

### 3.2 用户态 libc 子集（`userland/libc/`）

| 文件 | 作用 |
|---|---|
| `crt0.s` | `_start`：取 argc/argv/envp，设 `environ`，调 `main`，`exit` |
| `jmp.s` | `setjmp/longjmp`（i386，`jmp_buf` 同 4.4BSD 布局） |
| `libc_sys.c` | 全部 `int 0x80` syscall stub + errno + 无 tty/无信号/无目录的降级实现 |
| `dtoa.c` | 给 4.4BSD `vfprintf` 用的最小 `__dtoa`（sh 不打浮点） |
| `ctype.h` + `ctype_table.c` | 4.3BSD-Reno 的经典 `_ctype_` 表（4.4BSD 的 ctype 依赖 rune/locale，太重） |
| `histedit.h` + `histedit_stub.c` | 4.4BSD sh 强制链接 libedit；本内核无 termios，`el` 保持 NULL，退回 `read(2)` |
| `errlst.c` | 4.4BSD `sys_errlist`（`sys_nerr` 去掉 const 以匹配头文件） |

4.4BSD 源码里直接编的子集：`string/`（含 bcopy→memcpy/memmove、
memset→bzero、index→strchr 的多重定义编法）、`stdlib/`（malloc/calloc/
exit/atexit/getenv/qsort/strtol/rand/abort/getopt）、`stdio/`（整个
FILE/缓冲/printf 家族）、`gen/`（isatty/setmode/wait3/exec）。

`build_sh.ps1` 负责：生成 `token.def`/`builtins.c,h`/`nodes.c,h`/
`syntax.c,h`/`init.c`（4.4BSD 的 mk* 主机工具，用 `-include hostshim.h`
补 `__P`、补 `link()`），bison/flex 生成 `arith.c`/`arith_lex.c`，
最后 `ld -m i386pe -Ttext 0x08048000` + `objcopy -O elf32-i386`（外加
`libgcc.a` 提供 `__divdi3` 等）。

## 4. 已知限制（S4 及以后）

- 无 `fork/vfork/wait4`：**外部命令不能执行**，只有内建命令（`echo`,
  `cd` 退化, `exit`, `$(( ))` 等）可用；`vfork` 返回 ENOSYS；
- 无 tty：`isatty` 恒假、无行编辑/作业控制/`fc` 历史；
- 无目录：`opendir/readdir` 返回 NULL，`chdir` 恒 ENOENT，`getcwd` 恒 `/`；
- `ramfs` 仍是内核数组，ISO9660 里没有 `/bin/sh` 文件；
- 信号全为 no-op（`signal/sigaction` 返回 0），`kill` 返回 0。

## 5. 构建 / 运行 / 测试

```powershell
cd C:\Users\Administrator\Desktop\work\bsd45
$env:QEMU='C:\msys64\ucrt64\bin\qemu-system-i386.exe'
$env:MKISOFS='C:\Program Files (x86)\VMware\VMware Workstation\mkisofs.exe'

powershell -NoProfile -ExecutionPolicy Bypass -File .\build_all.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\test_s3.ps1
```

`build_all.ps1` = 小用户态（hello/init）→ `build_sh.ps1`（真 sh 覆盖
`out\userland\sh.elf`）→ 内核 → ISO。加 `-NoSh` 可退回内置小 sh。

手动 QEMU：

```powershell
& "C:\msys64\ucrt64\bin\qemu-system-i386.exe" `
  -cdrom out\BSD45-ELF-x86.iso -boot d -m 64 -serial stdio
```

启动后在 monitor 里：
- `usermode`：`init -c 'echo hello; exit 3'` → sh -c 验收路径；
- `shell`：交互 `/bin/sh`；
- `test`：libc 冒烟测试（printf/malloc/string）。

`tools/run_qemu_test.ps1` 是 QEMU 驱动（TCP 串口，`server=on,wait=on`，
必须 wait=on 才能收到启动输出）。`tools/test_kbd.ps1` 是**纯键盘**回归
（`-serial none`，模拟 VMware 这种没有串口的机器）。

### VMware

打开 `vms\BSD45-ELF-x86.vmx`（cdrom 指向 `..\out\BSD45-ELF-x86.iso`）。
启动后**先点一下 VM 窗口**再敲键盘（VMware 要捕获输入）。启动行会打印
`Console: VGA 80x25, COM1 present` 或 `COM1 absent`——没有串口的 VM 显示
absent 是正常的。

> 旧版 ISO 在 VMware 里键盘没反应：VMware 对未映射的 I/O 端口返回 `0xff`，
> 而 `pccngetc()` 先读 COM1 的 LSR，`0xff & 1 == 1` 被当成"串口有数据"，
> 于是永远返回 `0xff`、从不轮询键盘。现在 `com_probe()` 会识别这种情况
> 并跳过 COM1，另外 `kbd_init()` 会清空 8042 输出缓冲并发送 `0xae`
> 使能键盘接口。想抓串口日志可在 vmx 里加：
>
> ```
> serial0.present = "TRUE"
> serial0.fileType = "file"
> serial0.fileName = "bsd45-serial.log"
> ```

## 6. 已踩过并修复的关键坑（别重犯）

1. `pmap_map()` 没有 pmap 参数：`pmap_map(virt, start, end, prot)`。
2. `locore.s` 的 `calltrap` 要 `pushl %esp` 给 `trap()` 传 trapframe 指针。
3. IDT 要 `idt[256]` 才放得下 `int 0x80`。
4. 用户 PTE 用 `PG_u`(0x4)，不是 `PG_U`(0x20)。
5. 用户页目录必须复制 PDE `0x3F6`（proc0 内核栈）。
6. ISO 末尾内核补零到 2048 对齐。
7. stage1 用 CD 的 2048 字节扇区计数。
8. 解包用 `System32\tar.exe`（MSYS2 GNU tar 处理 Windows `-C` 路径有坑）。
9. `$ErrorActionPreference` 在 build_iso 必须是 Continue。
10. GCC16 下 `region_descriptor` 6 字节 packed、`segment_descriptor` 8 字节。
11. **用户态也必须 `-march=i386 -mno-sse ...`**：默认 `-march=pentium4` 会生成
    `movd %eax,%xmm0`，内核没开 SSE，一进 ring3 就异常。
12. **PE 汇编符号带下划线**：`_start` 里 `call _start_c`；且 `static` 函数被
    内联掉会导致 asm 引用找不到符号（去掉 static 或加 `used`）。
13. **COFF 不支持 `.size`** 指令（ELF 才有）。
14. **`brk` 必须按页对齐推进**，否则同一页被反复映射，malloc 写新页 #PF。
15. **`struct stat` 是 96 字节不是 128**，越界写会踩坏调用者栈（症状：
    `__swhatbuf` 里写 NULL 指针）。
16. 4.4BSD `signal.h` 把 `sigemptyset/sigfillset/...` 定义成宏，libc 内部要
    真函数时必须先 `#undef`（且放在所有 `#include` 之后）。
17. 4.4BSD `arith.y` 用 `= {` 标动作、`yyerror` 里用 `yyerrok`，bison 不认，
    生成前要 sed 掉。
18. 复制 sh 源码时源树里的 `obj` 坏符号链接会让 `Copy-Item` 管道中断，
    要过滤 ReparsePoint。
19. `Expand-Archive` 解 Reno 源码会因非法文件名失败，用 `7za x`。
20. **VMware 无串口时键盘失灵**：未映射 I/O 端口读回 `0xff`，`pccngetc()`
    把 `0xff` 当串口数据，永远不读键盘。修复：`com_probe()` 判断 LSR 的
    `0xe0` 位（`0xff` 或非 `0x60` 视为无串口）+ `kbd_init()` 清 8042 缓冲并
    发 `0xae`；启动时打印 `Console: ... COM1 present/absent` 便于确认。

21. **ATAPI 的 PACKET 命令必须设置 byte count limit**（LBA mid/high，
    小端）。填 0 时 VMware 的 CDR10 会把 READ(10)/READ(12) 直接 ABRT
    （st 0x51 err 0x04），而 INQUIRY / READ CAPACITY 却正常，极具迷惑性。
    正确做法：按本次数据相长度填 `bytes & 0xff` / `(bytes >> 8) & 0xff`。
22. ATAPI 探测不要迷信 LBA mid/high 的 0x14/0xeb 签名（QEMU 读回 0），
    用 IDENTIFY PACKET 返回的 word0（bit15=1 且设备类型 5 = CD-ROM）判定。
23. 读 CD 的缓冲区不要放进内核静态数组：会撑大 ELF，stage1 把内核读到
    0x10000、stage2 在 0x90000，>512 KB 的内核会被覆盖。改用运行时
    `pmap_map` 的临时窗口。

24. **`user_map()` 必须显式接收页目录**：它内部用 `SCRATCH_PD` 编辑页表，
    fork/exec 之后 `SCRATCH_PD` 指向的可能已经是子进程的 PD，父进程 `brk`
    会把页表改到别处（症状：父进程堆页缺失、`#PF`）。
25. **内核镜像必须 < 512 KB**：stage1 把内核读到 `0x10000`，stage2 放在
    `0x90000`；内核超过 512 KB 会覆盖自己的尾部（内嵌镜像数组），表现为
    用户程序某几页是内核代码。S5 起 sh/echo/test 只放在 ISO9660 上，
    内核只嵌 `hello/init`，镜像回到 ~334 KB。
26. **僵尸进程要回收**：父进程阻塞在 `wait4` 时由子进程 `exit` 唤醒，这条
    路径必须把子进程标成 FREE（`reaped`），否则下一次 `wait4` 会拿到已经
    报过到旧 pid。
27. **vfork 子进程不要碰栈**：严格共享地址空间时，子进程自己的函数调用会
    踩父进程的栈帧（我们的测试程序就踩过）。本实现改为“复制 + 父进程阻塞”，
    是 vfork 的安全超集。

## 7. 下一步（S5 之后）

1. tty/termios：`/dev/console` + `isatty` 为真，行编辑与作业控制；
2. 目录：`getdirentries` + `opendir/readdir`，让 `ls`/通配符可用；
3. pipe + `dup2`：`|` 与重定向（目前 `dup2` 有、`pipe` 返回 ENOSYS）；
4. 真 vnode/VFS 与 vm_map，替换现在的 ramfs 数组 + ISO9660 特判；
5. 简单文件系统缓存与页回收（现在 fork/exec 的页只分配不回收）。

恢复方式：打开本文件，按第 5 节构建，从第 7 节继续。
