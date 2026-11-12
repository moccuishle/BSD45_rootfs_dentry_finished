/*
 * crt0.s - C runtime start-up for BSD45-ELF-x86 user programs.
 *
 * The kernel enters a new image with the stack holding
 *	argc, argv[0..argc-1], NULL, envp[0..], NULL, strings...
 */

	.text
	.globl	_start
_start:
	movl	(%esp), %eax		/* argc */
	leal	4(%esp), %ecx		/* argv */
	leal	8(%esp,%eax,4), %edx	/* envp */
	movl	%edx, _environ
	pushl	%edx
	pushl	%ecx
	pushl	%eax
	call	_main
	pushl	%eax
	call	_exit
1:	jmp	1b
