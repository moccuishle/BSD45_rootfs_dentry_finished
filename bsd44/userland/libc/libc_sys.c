/*
 * libc_sys.c - 4.4BSD userland C library glue for BSD45-ELF-x86.
 *
 * The 4.4BSD i386 libc enters the kernel with `lcall $7,$0`; this port uses
 * `int $0x80` with the arguments in %ebx/%ecx/%edx/%esi/%edi and a negative
 * errno in %eax.  Everything else here is the small set of libc entry
 * points the kernel/userland boundary needs on a machine with no real
 * VFS, no signals and no process table yet.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <dirent.h>
#include <termios.h>
#include <pwd.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define	SYS_exit	1
#define	SYS_fork	2
#define	SYS_read	3
#define	SYS_write	4
#define	SYS_open	5
#define	SYS_close	6
#define	SYS_wait4	7
#define	SYS_link	9
#define	SYS_unlink	10
#define	SYS_chdir	12
#define	SYS_chmod	15
#define	SYS_break	17
#define	SYS_getpid	20
#define	SYS_getuid	24
#define	SYS_geteuid	25
#define	SYS_access	33
#define	SYS_kill	37
#define	SYS_getppid	39
#define	SYS_dup		41
#define	SYS_pipe	42
#define	SYS_getegid	43
#define	SYS_sigaction	46
#define	SYS_getgid	47
#define	SYS_sigprocmask	48
#define	SYS_getlogin	49
#define	SYS_sigpending	52
#define	SYS_ioctl	54
#define	SYS_readlink	58
#define	SYS_execve	59
#define	SYS_umask	60
#define	SYS_vfork	66
#define	SYS_sbrk	69
#define	SYS_getpgrp	81
#define	SYS_setpgid	82
#define	SYS_setitimer	83
#define	SYS_getitimer	86
#define	SYS_getdtablesize 89
#define	SYS_dup2	90
#define	SYS_fcntl	92
#define	SYS_gettimeofday 116
#define	SYS_getrusage	117
#define	SYS_fstat	188
#define	SYS_stat	189
#define	SYS_lstat	190
#define	SYS_setitimer2	83

int	errno;
char	**environ;
char	*__progname = "sh";

static int
syscall_rv(int r)
{

	if (r < 0) {
		errno = -r;
		return (-1);
	}
	return (r);
}

static int
sys3(int n, int a, int b, int c)
{
	int r;

	__asm__ volatile("int $0x80"
	    : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
	return (r);
}

static int
sys1(int n, int a)
{

	return (sys3(n, a, 0, 0));
}

void
_exit(int status)
{

	sys1(SYS_exit, status);
	for (;;)
		;
}

ssize_t
read(int fd, void *buf, size_t n)
{

	return (syscall_rv(sys3(SYS_read, fd, (int)buf, n)));
}

ssize_t
write(int fd, const void *buf, size_t n)
{

	return (syscall_rv(sys3(SYS_write, fd, (int)buf, n)));
}

int
open(const char *path, int flags, ...)
{
	int mode = 0;

	if (flags & O_CREAT)
		mode = 0666;
	return (syscall_rv(sys3(SYS_open, (int)path, flags, mode)));
}

int
close(int fd)
{

	return (syscall_rv(sys1(SYS_close, fd)));
}

off_t
lseek(int fd, off_t off, int whence)
{

	return (syscall_rv(sys3(199, fd, (int)off, whence)));
}

int
fstat(int fd, struct stat *sb)
{

	return (syscall_rv(sys3(SYS_fstat, fd, (int)sb, 0)));
}

int
stat(const char *path, struct stat *sb)
{

	return (syscall_rv(sys3(SYS_stat, (int)path, (int)sb, 0)));
}

int
lstat(const char *path, struct stat *sb)
{

	return (stat(path, sb));
}

int
access(const char *path, int mode)
{

	return (syscall_rv(sys3(SYS_access, (int)path, mode, 0)));
}

int
unlink(const char *path)
{

	return (syscall_rv(sys1(SYS_unlink, (int)path)));
}

int
chdir(const char *path)
{

	return (syscall_rv(sys1(SYS_chdir, (int)path)));
}

int
chmod(const char *path, mode_t mode)
{

	return (syscall_rv(sys3(SYS_chmod, (int)path, mode, 0)));
}

int
dup(int fd)
{

	return (syscall_rv(sys1(SYS_dup, fd)));
}

int
dup2(int from, int to)
{

	return (syscall_rv(sys3(SYS_dup2, from, to, 0)));
}

int
pipe(int *fds)
{

	return (syscall_rv(sys1(SYS_pipe, (int)fds)));
}

int
fcntl(int fd, int cmd, ...)
{
	int arg = 0;

	return (syscall_rv(sys3(SYS_fcntl, fd, cmd, arg)));
}

int
ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	return (syscall_rv(sys3(SYS_ioctl, fd, (int)request, (int)arg)));
}

mode_t
umask(mode_t mask)
{

	return (sys1(SYS_umask, mask));
}

pid_t
getpid(void)
{

	return (sys1(SYS_getpid, 0));
}

pid_t
getppid(void)
{

	return (sys1(SYS_getppid, 0));
}

uid_t
getuid(void)
{

	return (sys1(SYS_getuid, 0));
}

uid_t
geteuid(void)
{

	return (sys1(SYS_geteuid, 0));
}

gid_t
getgid(void)
{

	return (sys1(SYS_getgid, 0));
}

gid_t
getegid(void)
{

	return (sys1(SYS_getegid, 0));
}

pid_t
getpgrp(void)
{

	return (sys1(SYS_getpgrp, 0));
}

int
setpgid(pid_t pid, pid_t pgid)
{

	return (syscall_rv(sys3(SYS_setpgid, pid, pgid, 0)));
}

pid_t
tcgetpgrp(int fd)
{

	return (1);
}

int
tcsetpgrp(int fd, pid_t pgrp)
{

	return (0);
}

pid_t
fork(void)
{

	return (syscall_rv(sys1(SYS_fork, 0)));
}

pid_t
vfork(void)
{

	return (syscall_rv(sys1(SYS_vfork, 0)));
}

pid_t
wait4(pid_t pid, int *status, int options, struct rusage *ru)
{

	return (syscall_rv(sys3(SYS_wait4, pid, (int)status, options)));
}

int
execve(const char *path, char *const *argv, char *const *envp)
{

	return (syscall_rv(sys3(SYS_execve, (int)path, (int)argv, (int)envp)));
}

int
kill(pid_t pid, int sig)
{

	return (syscall_rv(sys3(SYS_kill, pid, sig, 0)));
}

int
sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{

	return (syscall_rv(sys3(SYS_sigaction, sig, (int)act, (int)oact)));
}

int
sigprocmask(int how, const sigset_t *set, sigset_t *oset)
{

	return (syscall_rv(sys3(SYS_sigprocmask, how, (int)set, (int)oset)));
}

int
sigpending(sigset_t *set)
{

	return (syscall_rv(sys1(SYS_sigpending, (int)set)));
}

int
sigsuspend(const sigset_t *set)
{

	errno = EINTR;
	return (-1);
}

int
sigblock(int mask)
{

	return (0);
}

int
sigsetmask(int mask)
{

	return (0);
}

int
sigpause(int mask)
{

	return (0);
}

int
siginterrupt(int sig, int flag)
{

	return (0);
}

int
sigvec(int sig, struct sigvec *vec, struct sigvec *ovec)
{

	return (0);
}

int
getdtablesize(void)
{

	return (sys1(SYS_getdtablesize, 0));
}

char *
brk(const char *addr)
{
	int r;

	r = sys1(SYS_break, (int)addr);
	if (r < 0) {
		errno = -r;
		return ((char *)-1);
	}
	return ((char *)r);
}

char *
sbrk(int incr)
{
	static char *cur;
	unsigned r;
	int old;

	if (cur == 0)
		cur = (char *)0x09000000;
	old = (int)cur;
	r = (unsigned)sys1(SYS_break, (int)(cur + incr));
	if ((int)r < 0) {
		errno = -r;
		return ((char *)-1);
	}
	cur += incr;
	return ((char *)old);
}

int
getrlimit(int res, struct rlimit *rlp)
{

	rlp->rlim_cur = rlp->rlim_max = 16 * 1024 * 1024;
	return (0);
}

int
setrlimit(int res, const struct rlimit *rlp)
{

	return (0);
}

int
gettimeofday(struct timeval *tp, struct timezone *tzp)
{
	static struct timeval tv;

	tv.tv_sec = 784000000;		/* 1994-11-04, "4.4BSD time" */
	tv.tv_usec = 0;
	if (tp)
		*tp = tv;
	if (tzp) {
		tzp->tz_minuteswest = 0;
		tzp->tz_dsttime = 0;
	}
	return (0);
}

time_t
time(time_t *tp)
{
	struct timeval tv;

	gettimeofday(&tv, 0);
	if (tp)
		*tp = tv.tv_sec;
	return (tv.tv_sec);
}

int
getrusage(int who, struct rusage *ru)
{

	memset(ru, 0, sizeof(*ru));
	return (0);
}

unsigned int
alarm(unsigned int sec)
{

	return (0);
}

int
setitimer(int which, const struct itimerval *v, struct itimerval *o)
{

	if (o)
		memset(o, 0, sizeof(*o));
	return (0);
}

int
getitimer(int which, struct itimerval *v)
{

	memset(v, 0, sizeof(*v));
	return (0);
}

char *
getlogin(void)
{
	static char name[16] = "root";

	return (name);
}

int
setlogin(const char *name)
{

	return (0);
}

int
readlink(const char *path, char *buf, int bufsiz)
{

	errno = EINVAL;
	return (-1);
}

int
link(const char *a, const char *b)
{

	errno = EROFS;
	return (-1);
}

int
symlink(const char *a, const char *b)
{

	errno = EROFS;
	return (-1);
}

int
getpagesize(void)
{

	return (4096);
}

void
__main(void)
{
	/* GCC emits a call to __main in main()'s prologue for PE targets. */
}

int
fchdir(int fd)
{

	errno = ENOTTY;
	return (-1);
}

char *
getcwd(char *buf, size_t size)
{

	if (size < 2) {
		errno = ERANGE;
		return (NULL);
	}
	buf[0] = '/';
	buf[1] = 0;
	return (buf);
}

int
tcgetattr(int fd, struct termios *t)
{

	errno = ENOTTY;
	return (-1);
}

int
tcsetattr(int fd, int actions, const struct termios *t)
{

	errno = ENOTTY;
	return (-1);
}

int
writev(int fd, const struct iovec *iov, int iovcnt)
{
	int i, n, total = 0;

	for (i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len == 0)
			continue;
		n = write(fd, iov[i].iov_base, iov[i].iov_len);
		if (n < 0)
			return (total ? total : -1);
		total += n;
	}
	return (total);
}

int
isnan(double d)
{

	return (d != d);
}

int
isinf(double d)
{

	if (d != d)
		return (0);
	if (d > 1.7976931348623157e308)
		return (1);
	if (d < -1.7976931348623157e308)
		return (-1);
	return (0);
}

void
(*signal(int sig, void (*func)(int)))(int)
{

	return (NULL);
}

int
killpg(int pgrp, int sig)
{

	return (kill(-pgrp, sig));
}

int
yywrap(void)
{

	return (1);
}

double
atof(const char *s)
{
	double v = 0.0, scale = 1.0;
	int neg = 0;

	while (*s == ' ' || *s == '\t' || *s == '\n')
		s++;
	if (*s == '-') {
		neg = 1;
		s++;
	} else if (*s == '+')
		s++;
	while (*s >= '0' && *s <= '9') {
		v = v * 10.0 + (*s - '0');
		s++;
	}
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9') {
			v = v * 10.0 + (*s - '0');
			scale *= 10.0;
			s++;
		}
	}
	v /= scale;
	return (neg ? -v : v);
}

const char *const sys_siglist[32] = {
	"signal 0", "SIGHUP", "SIGINT", "SIGQUIT", "SIGILL", "SIGTRAP",
	"SIGABRT", "SIGEMT", "SIGFPE", "SIGKILL", "SIGBUS", "SIGSEGV",
	"SIGSYS", "SIGPIPE", "SIGALRM", "SIGTERM", "SIGURG", "SIGSTOP",
	"SIGTSTP", "SIGCONT", "SIGCHLD", "SIGTTIN", "SIGTTOU", "SIGIO",
	"SIGXCPU", "SIGXFSZ", "SIGVTALRM", "SIGPROF", "SIGWINCH", "SIGINFO",
	"SIGUSR1", "SIGUSR2"
};

DIR *
opendir(const char *name)
{

	errno = ENOENT;
	return (NULL);
}

struct dirent *
readdir(DIR *dp)
{

	return (NULL);
}

int
closedir(DIR *dp)
{

	return (-1);
}

#undef sigaddset
#undef sigdelset
#undef sigemptyset
#undef sigfillset
#undef sigismember

int
sigemptyset(sigset_t *s)
{

	*s = 0;
	return (0);
}

int
sigfillset(sigset_t *s)
{

	*s = ~(sigset_t)0;
	return (0);
}

int
sigaddset(sigset_t *s, int n)
{

	*s |= (sigset_t)(1 << (n - 1));
	return (0);
}

int
sigdelset(sigset_t *s, int n)
{

	*s &= ~(sigset_t)(1 << (n - 1));
	return (0);
}

int
sigismember(const sigset_t *s, int n)
{

	return ((*s & (sigset_t)(1 << (n - 1))) ? 1 : 0);
}

struct passwd *
getpwnam(const char *name)
{

	return (NULL);
}

struct passwd *
getpwuid(uid_t uid)
{

	return (NULL);
}

unsigned int
sleep(unsigned int secs)
{

	return (0);
}

