/*
 * jmp.s - setjmp/longjmp for the i386 ELF userland.
 *
 * jmp_buf layout (same as the 4.4BSD i386 port):
 *	0  return address
 *	4  %ebx   8  %esp   12 %ebp   16 %esi   20 %edi
 *
 * The 4.4BSD version also saved the signal mask; this kernel has no
 * signals, so the save/restore calls are dropped.
 */

	.text

	.globl	_setjmp
	.globl	__setjmp
_setjmp:
__setjmp:
	movl	4(%esp), %ecx
	movl	(%esp), %edx
	movl	%edx, 0(%ecx)
	movl	%ebx, 4(%ecx)
	movl	%esp, 8(%ecx)
	movl	%ebp, 12(%ecx)
	movl	%esi, 16(%ecx)
	movl	%edi, 20(%ecx)
	xorl	%eax, %eax
	ret

	.globl	_longjmp
	.globl	__longjmp
_longjmp:
__longjmp:
	movl	4(%esp), %edx
	movl	8(%esp), %eax
	movl	0(%edx), %ecx
	movl	4(%edx), %ebx
	movl	8(%edx), %esp
	movl	12(%edx), %ebp
	movl	16(%edx), %esi
	movl	20(%edx), %edi
	cmpl	$0, %eax
	jne	1f
	movl	$1, %eax
1:	movl	%ecx, 0(%esp)
	ret
