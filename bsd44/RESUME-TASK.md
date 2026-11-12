# BSD44-ELF-x86 任务恢复文本

更新时间：2026-11-13（Asia/Shanghai）
状态：**S3 完成**，下一步 S4（真 root 文件系统 / ISO9660）。
仓库：https://github.com/moccuishle/BSD4.4_rootfs_filesystem_x86_ISO_ELF_El-Torito-

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
| S4 | 真 VFS/vnode、vm_map、FFS 或 ISO9660 root、从光盘挂载 | ⬜ 未开始 |

### S3 验收（`powershell -File tools\test_s3.ps1`）

```
bsd44> usermode
exec /sbin/init: entry 8048000 pd 200000
init: starting /bin/sh
execve /bin/sh: entry 8048000 esp bfbfffb8 pd 20e000 argc 3
hello
[user exit 3]
```

交互模式（monitor 输入 `shell`）：

```
bsd44> shell
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
cd C:\Users\Administrator\Desktop\work\bsd44
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
  -cdrom out\BSD44-ELF-x86.iso -boot d -m 64 -serial stdio
```

启动后在 monitor 里：
- `usermode`：`init -c 'echo hello; exit 3'` → sh -c 验收路径；
- `shell`：交互 `/bin/sh`；
- `test`：libc 冒烟测试（printf/malloc/string）。

`tools/run_qemu_test.ps1` 是 QEMU 驱动（TCP 串口，`server=on,wait=on`，
必须 wait=on 才能收到启动输出）。`tools/test_kbd.ps1` 是**纯键盘**回归
（`-serial none`，模拟 VMware 这种没有串口的机器）。

### VMware

打开 `vms\BSD44-ELF-x86.vmx`（cdrom 指向 `..\out\BSD44-ELF-x86.iso`）。
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
> serial0.fileName = "bsd44-serial.log"
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

## 7. 下一步（S4）

1. 真 root 文件系统：ATA/ATAPI + ISO9660（`isofs/cd9660`），把 `/bin/sh`
   作为光盘文件加载，而不是内核数组；
2. `fork/vfork` + `wait4`（先同步语义）→ 能让 sh 跑外部命令；
3. 最小 tty：`/dev/console` + termios ioctl，让 `isatty`/行编辑成立；
4. 目录支持：ramfs 目录项 + `getdirentries`，`opendir/readdir` 不再是 stub；
5. 之后才是真 vnode/VFS 与 vm_map。

恢复方式：打开本文件，按第 5 节构建，从第 7 节继续。
