/*
 * usermode_asm.s - ring 3 entry and int 0x80 stub.
 */

	.text

/*
 * void enter_user(unsigned pd, unsigned entry, unsigned esp)
 *
 * Build the whole ring-3 return frame first (while the kernel's own page
 * directory is still active), then switch CR3 and iret.  Reading the
 * selector variables after the CR3 switch is what broke on VMware: the
 * new directory does not necessarily keep the kernel's low mappings that
 * the pushl from memory needed.
 */
	.globl	_enter_user
	.align	4
_enter_user:
	movl	4(%esp), %eax		/* page directory */
	movl	8(%esp), %ecx		/* entry */
	movl	12(%esp), %edx		/* user esp */
	movzwl	__udatasel, %esi
	movzwl	__ucodesel, %edi
	pushl	%esi			/* user ss */
	pushl	%edx			/* user esp */
	pushfl
	popl	%ebx
	andl	$0xfffffdff, %ebx	/* IF=0: no timer IRQ yet */
	pushl	%ebx
	pushl	%edi			/* user cs */
	pushl	%ecx			/* eip */
	movw	%si, %ds
	movw	%si, %es
	movw	%si, %fs
	movw	%si, %gs
	movl	%eax, %cr3
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

/*
 * void resume_user(unsigned pd, struct sysregs *r)
 *
 * Restore a saved user context (struct sysregs: ds, es, edi, esi, ebp,
 * esp, ebx, edx, ecx, eax, eip, cs, eflags, uesp, ss) and return to ring 3.
 * Never returns.
 */
	.globl	_resume_user
	.align	4
_resume_user:
	movl	4(%esp), %eax		/* page directory */
	movl	8(%esp), %esi		/* saved context */
	movl	%eax, %cr3
	movw	__udatasel, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs
	pushl	56(%esi)		/* ss */
	pushl	52(%esi)		/* user esp */
	pushl	48(%esi)		/* eflags */
	pushl	44(%esi)		/* cs */
	pushl	40(%esi)		/* eip */
	movl	24(%esi), %ebx
	movl	28(%esi), %edx
	movl	32(%esi), %ecx
	movl	36(%esi), %eax
	movl	16(%esi), %ebp
	movl	8(%esi), %edi
	movl	12(%esi), %esi		/* last: clobbers the frame pointer */
	iret
