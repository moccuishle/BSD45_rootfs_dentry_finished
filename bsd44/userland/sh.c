/* /bin/sh - minimal interactive shell over BSD45 syscalls */
static char buf[128];

static int
sys_write(int fd, const char *b, unsigned n)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(4), "b"(fd), "c"(b), "d"(n) : "memory");
	return r;
}

static int
sys_read(int fd, char *b, unsigned n)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(3), "b"(fd), "c"(b), "d"(n) : "memory");
	return r;
}

static void
sys_exit(int code)
{
	__asm__ volatile("int $0x80" :: "a"(1), "b"(code));
	for (;;)
		;
}

static int
streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (*a == *b);
}

static int
startswith(const char *s, const char *p)
{
	while (*p)
		if (*s++ != *p++)
			return (0);
	return (1);
}

static void
puts(const char *s)
{
	unsigned n = 0;

	while (s[n])
		n++;
	sys_write(1, s, n);
}

void
start_c(int argc, char **argv)
{
	if (argc >= 3 && streq(argv[1], "-c")) {
		puts("sh(tiny): -c ");
		puts(argv[2]);
		puts("\n");
		sys_exit(0);
	}
	puts("sh(tiny): interactive\n");
	for (;;) {
		int n;

		puts("sh> ");
		n = sys_read(0, buf, sizeof(buf) - 1);
		if (n <= 0)
			continue;
		if (buf[n - 1] == '\n')
			n--;
		buf[n] = 0;
		if (n == 0)
			continue;
		if (streq(buf, "exit"))
			sys_exit(0);
		if (streq(buf, "help")) {
			puts("builtins: echo <text>, help, exit\n");
			continue;
		}
		if (startswith(buf, "echo ")) {
			puts(buf + 5);
			puts("\n");
			continue;
		}
		puts("sh: not found: ");
		puts(buf);
		puts("\n");
	}
}

__asm__(
"	.text\n"
"	.globl _start\n"
"_start:\n"
"	movl	(%esp),%eax\n"
"	leal	4(%esp),%ecx\n"
"	pushl	%ecx\n"
"	pushl	%eax\n"
"	call	_start_c\n"
"1:	jmp	1b\n"
);
