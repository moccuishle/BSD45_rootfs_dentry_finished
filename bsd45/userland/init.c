/* init(8) - first user process; execs /bin/sh.
 *
 * Usage passed down by the kernel:
 *	init                -> interactive /bin/sh
 *	init -c <script>    -> /bin/sh -c <script>
 */

static int
sys_write(int fd, const char *b, unsigned n)
{
	int r;

	__asm__ volatile("int $0x80" : "=a"(r)
	    : "a"(4), "b"(fd), "c"(b), "d"(n) : "memory");
	return (r);
}

static int
sys_execve(const char *path, char **argv, char **envp)
{
	int r;

	__asm__ volatile("int $0x80" : "=a"(r)
	    : "a"(59), "b"(path), "c"(argv), "d"(envp) : "memory");
	return (r);
}

static void
sys_exit(int code)
{

	__asm__ volatile("int $0x80" :: "a"(1), "b"(code));
	for (;;)
		;
}

void
start_c(int argc, char **argv, char **envp)
{
	static const char msg[] = "init: starting /bin/sh\n";
	static char *sh_argv[4];
	char *sh_envp[2];
	int n = 0;

	sys_write(1, msg, sizeof(msg) - 1);

	sh_argv[n++] = "sh";
	if (argc >= 3 && argv[1][0] == '-' && argv[1][1] == 'c') {
		sh_argv[n++] = "-c";
		sh_argv[n++] = argv[2];
	} else {
		sh_argv[n++] = "-i";	/* interactive: sh prints its prompt */
	}
	sh_argv[n] = 0;

	sh_envp[0] = "PATH=/bin:/sbin";
	sh_envp[1] = 0;

	if (sys_execve("/bin/sh", sh_argv, sh_envp) < 0)
		sys_write(1, "init: exec /bin/sh failed\n", 26);
	sys_exit(1);
}

__asm__(
"	.text\n"
"	.globl _start\n"
"_start:\n"
"	movl	(%esp),%eax\n"		/* argc */
"	leal	4(%esp),%ecx\n"		/* argv */
"	leal	8(%esp,%eax,4),%edx\n"	/* envp = argv + argc + 1 */
"	pushl	%edx\n"
"	pushl	%ecx\n"
"	pushl	%eax\n"
"	call	_start_c\n"
"1:	jmp	1b\n"
);
