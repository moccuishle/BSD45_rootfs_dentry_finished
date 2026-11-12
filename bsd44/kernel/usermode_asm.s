/*
 * usermode_asm.s - ring 3 entry and int 0x80 stub.
 */

	.text

/*
 * void enter_user(unsigned pd, unsigned entry, unsigned esp)
 */
	.globl	_enter_user
	.align	4
_enter_user:
	movl	4(%esp), %eax		/* page directory */
	movl	8(%esp), %ecx		/* entry */
	movl	12(%esp), %edx		/* user esp */
	movl	%eax, %cr3
	movw	__udatasel, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs
	pushl	%eax			/* user ss */
	pushl	%edx			/* user esp */
	pushfl
	popl	%eax
	andl	$0xfffffdff, %eax	/* IF=0: no timer IRQ yet */
	pushl	%eax
	pushl	__ucodesel
	pushl	%ecx
	iret

/*
 * int 0x80 entry.  Frame: ds es edi esi ebp esp ebx edx ecx eax
 *                      eip cs eflags uesp ss
 */
	.globl	_syscall_entry
	.align	4
_syscall_entry:
	pushal
	push	%es
	push	%ds
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	pushl	%esp
	call	_syscall_dispatch
	addl	$4, %esp
	pop	%ds
	pop	%es
	popal
	iret
