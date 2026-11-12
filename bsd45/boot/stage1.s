/*
 * stage1.s - El Torito no-emulation boot sector for BSD45-ELF-x86.
 *
 * Runs in 16-bit real mode at 0x7C00.  Reads the ELF kernel from a fixed
 * LBA (recorded in the manifest by mkiso.ps1), copies the 32-bit stage 2
 * to 0x90000, enables A20 and protected mode, and jumps to stage 2.
 */

	.code16
	.text
	.globl	_start

#define	STAGE2_ADDR	0x90000
#define	STAGE2_OFF	0x200

_start:
	cli
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	$0x7C00, %sp
	cld
	movb	%dl, boot_drive
	movb	$0x31, %al
	outb	%al, $0xE9

	movw	$msg_hello, %si
	call	puts

	/* find the manifest (linked into this image) */
	movl	manifest+4, %eax	/* kernel LBA */
	movl	%eax, dap_lba
	movl	manifest+8, %eax	/* kernel bytes */
	addl	$2047, %eax
	shrl	$11, %eax
	movl	%eax, remaining
	movw	$0x1000, bufseg

	movw	$msg_load, %si
	call	puts

read_loop:
	cmpl	$0, remaining
	je	read_done
	movl	remaining, %eax
	cmpl	$16, %eax
	jbe	1f
	movl	$16, %eax
1:
	movl	%eax, chunk
	movw	%ax, dap_count
	movw	$0, dap_off
	movw	bufseg, %ax
	movw	%ax, dap_seg
	call	read_sectors

	movl	chunk, %eax
	addl	%eax, dap_lba
	subl	%eax, remaining
	shlw	$7, %ax
	addw	%ax, bufseg
	jmp	read_loop
read_done:
	movb	$0x32, %al
	outb	%al, $0xE9

	movw	$msg_stage2, %si
	call	puts

	/* relocate stage 2 to 0x90000 */
	movw	manifest+12, %cx	/* stage 2 bytes */
	shrw	$1, %cx
	movw	$0x7C00+STAGE2_OFF, %si
	movw	$0x9000, %ax
	movw	%ax, %es
	xorw	%di, %di
	rep	movsw

	/* A20 gate */
	inb	$0x92, %al
	orb	$0x02, %al
	andb	$0xFE, %al
	outb	%al, $0x92

	cli
	movb	$0x33, %al
	outb	%al, $0xE9
	lgdt	gdt_desc
	movl	%cr0, %eax
	orl	$1, %eax
	movl	%eax, %cr0
	/* 32-bit far jump to stage 2 */
	.byte	0x66, 0xEA
	.long	STAGE2_ADDR
	.word	0x08

/* ---- INT 13h extended read: dap ---- */
read_sectors:
	movw	$3, %cx		/* retries */
retry:
	movw	$dap, %si
	movb	boot_drive, %dl
	movb	$0x42, %ah
	int	$0x13
	jnc	ok
	movw	$msg_err, %si
	call	puts
	decw	%cx
	jnz	retry
	jmp	halt
ok:
	ret

puts:
	lodsb
	testb	%al, %al
	jz	1f
	movb	$0x0E, %ah
	movw	$7, %bx
	int	$0x10
	jmp	puts
1:
	ret

halt:
	cli
	hlt
	jmp	halt

.align	8
manifest:
	.long	0x464D4442	/* "BDMF" */
	.long	0		/* kernel LBA */
	.long	0		/* kernel bytes */
	.long	0		/* stage 2 bytes */

.align	8
gdt:
	.quad	0
	.quad	0x00CF9A000000FFFF
	.quad	0x00CF92000000FFFF
gdt_desc:
	.word	23
	.long	gdt

.align	8
dap:
	.word	0x10
dap_count:	.word	0
dap_off:	.word	0
dap_seg:	.word	0
dap_lba:	.long	0
	.long	0
remaining:	.long	0
chunk:		.long	0
bufseg:		.word	0
boot_drive:	.byte	0

msg_hello:	.asciz "BSD45-ELF-x86 boot\r\n"
msg_load:	.asciz "loading ELF kernel...\r\n"
msg_stage2:	.asciz "entering protected mode...\r\n"
msg_err:	.asciz "disk read error\r\n"

	.org	510
	.word	0xAA55
