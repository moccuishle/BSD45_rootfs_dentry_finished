BSD45-ELF-x86  -  4.4BSD-Lite2 i386 ELF kernel
==============================================

BSD45.ELF   UNIX System V ELF32, Intel 80386, ET_EXEC.
            Built from the 4.4BSD-Lite2 source tree (sys/i386: locore.s,
            pmap.c, machdep.c, cons.c, icu.s) with the MSYS2 mingw32
            i686-w64-mingw32 GCC 16.2.0, linked at KERNBASE 0xFE000000,
            then converted from COFF to ELF32 with objcopy.
            No SSE/MMX: -march=i386 -mno-sse -mno-sse2 -mno-mmx.

boot.img    El Torito no-emulation boot image:
              stage1 (512 bytes, real mode, INT 13h EDD)
              stage2 (32-bit ELF program-header loader)

The same ELF image is also appended at LBA 2048 of the CD, so stage1 can
read it with INT 13h extended reads without parsing ISO9660.  The ISO9660
directory nevertheless contains BSD45.ELF as a normal, readable file.

Boot it in VMware/QEMU and you get the 4.4BSD-Lite2 i386 kernel with its
Mach pmap and an on-board monitor:

  bsd45> help
  help  mem  pte <addr>  map  banner  reboot  halt
