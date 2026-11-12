/*
 * stage2.s - 32-bit protected mode ELF loader, linked at 0x90000.
 *
 * Parses the ELF32 image staged at 0x10000 by stage 1, copies every
 * PT_LOAD segment to its physical address (p_vaddr - KERNBASE), zeroes
 * bss, and enters the 4.4BSD locore entry point.
 */

	.code32
	.text
	.globl	_start32
_start32:
	cli
	movb	$0x34, %al
	outb	%al, $0xE9
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs
	movw	%ax, %ss
	movl	$0x9F000, %esp

	/* snapshot the ELF header (it is overwritten while loading) */
	movl	$0x10000, %esi
	movl	$0x8F000, %edi
	movl	$1024, %ecx
	cld
	rep	movsl

	movb	$0x61, %al
	outb	%al, $0xE9
	cmpl	$0x464C457F, 0x8F000
	jne	bad

	movl	0x8F01C, %ebx		/* e_phoff */
	addl	$0x8F000, %ebx
	movzwl	0x8F02C, %ecx		/* e_phnum */
	movl	0x8F018, %ebp		/* e_entry */
	subl	$0xFE000000, %ebp	/* virtual -> physical */

ph_loop:
	testl	%ecx, %ecx
	jz	ph_done
	cmpl	$1, (%ebx)
	jne	ph_next

	movl	4(%ebx), %esi		/* p_offset */
	addl	$0x10000, %esi
	movl	8(%ebx), %edi		/* p_vaddr */
	subl	$0xFE000000, %edi
	movl	0x10(%ebx), %edx	/* p_filesz */
	pushl	%ecx
	movl	%edx, %ecx
	rep	movsb

	movl	0x14(%ebx), %ecx	/* p_memsz */
	subl	%edx, %ecx
	xorl	%eax, %eax
	rep	stosb
	popl	%ecx

ph_next:
	addl	$32, %ebx
	decl	%ecx
	jmp	ph_loop
ph_done:

	movb	$0x35, %al
	outb	%al, $0xE9
	/* "ELF kernel loaded" on the VGA text buffer */
	movl	$0xB8000, %edi
	movl	$0x07420742, %eax
	movl	%eax, 0(%edi)

	movl	$0x9F000, %esp
	pushl	$0		/* cyloffset */
	pushl	$0		/* unit */
	pushl	$0		/* bootdev */
	pushl	$0		/* howto */
	pushl	$0		/* return address */
	jmp	*%ebp

bad:
	movb	$0x45, %al
	outb	%al, $0xE9
	movl	$0xB8000, %edi
	movl	$0x0C420C42, %eax
	movl	%eax, 0(%edi)
	cli
1:	hlt
	jmp	1b
