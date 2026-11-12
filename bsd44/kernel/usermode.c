/*
 * usermode.c - S3: ramfs + ELF exec with argv/envp + int 0x80 syscalls.
 *
 * A deliberately small VFS: a read-only ramfs table built from the
 * userland images embedded in the kernel.  execve() builds a fresh page
 * directory for the new image, copies argv/envp onto the new user stack
 * and switches CR3 from the syscall path.  This is the bridge to the
 * real 4.4BSD VFS/vm_map work later.
 *
 * Syscall convention (matches 4.4BSD i386 numbers):
 *	%eax	number
 *	%ebx %ecx %edx %esi %edi	arguments
 *	int	$0x80
 * On error %eax holds -errno; the libc stubs translate that to -1/errno.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/proc.h>
#include <sys/user.h>

#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_kern.h>

#include <machine/cpu.h>
#include <machine/pmap.h>

#define	USER_LOAD	0x08048000
#define	USER_STACK_TOP	0xBFC00000
#define	USER_STACK_SIZE	0x8000
#define	USER_ARG_PAGE	(USER_STACK_TOP - PAGE_SIZE)
#define	USER_ARGS_MAX	4096

#define	USER_HEAP_START	0x09000000
#define	USER_HEAP_MAX	0x0C000000

#define	SCRATCH_PD	0xFE200000
#define	SCRATCH_PT	0xFE201000
#define	SCRATCH_PAGE	0xFE202000

#define	NFD		16
#define	MAXARG		16
#define	MAXENV		8

/* 4.4BSD errno values we return */
#define	EPERM		1
#define	ENOENT		2
#define	ESRCH		3
#define	EINTR		4
#define	ENOEXEC		8
#define	EBADF		9
#define	ECHILD		10
#define	ENOMEM		12
#define	EACCES		13
#define	EFAULT		14
#define	ENOTDIR		20
#define	EISDIR		21
#define	EINVAL		22
#define	EMFILE		24
#define	ENOTTY		25
#define	EROFS		30
#define	ENOSYS		78

extern const unsigned char hello_elf[], init_elf[], sh_elf[], test_elf[];
extern const unsigned int hello_elf_len, init_elf_len, sh_elf_len, test_elf_len;
extern int _ucodesel, _udatasel;
extern void enter_user(unsigned pd, unsigned entry, unsigned esp);
extern void syscall_entry(void);
extern int cngetc(void);
extern void cnputc(int c);

struct file {
	const char		*f_name;
	const unsigned char	*f_data;
	const unsigned int	*f_len;
};

static struct file ramfs[] = {
	{ "/sbin/init",	init_elf,	&init_elf_len },
	{ "/bin/sh",	sh_elf,		&sh_elf_len },
	{ "/hello",	hello_elf,	&hello_elf_len },
	{ "/test",	test_elf,	&test_elf_len },
	{ "/etc/profile", 0, 0 },
	{ 0, 0, 0 }
};

struct fd {
	const unsigned char	*d;
	unsigned		len;
	unsigned		off;
	int			used;
};

static struct fd fds[NFD];
static unsigned next_pa = 0x200000;
static unsigned brk_mapped = USER_HEAP_START;	/* next heap page to map */
static unsigned umask_val = 022;

static unsigned
page_alloc(void)
{
	unsigned pa = next_pa;

	next_pa += PAGE_SIZE;
	if (next_pa >= 0x3f000000)
		panic("usermode: out of physical memory");
	return (pa);
}

static void *
map_page(unsigned va, unsigned pa)
{

	pmap_map((vm_offset_t)va, (vm_offset_t)pa,
	    (vm_offset_t)pa + PAGE_SIZE, VM_PROT_READ | VM_PROT_WRITE);
	return ((void *)va);
}

static void
zero_page(unsigned pa)
{
	char *p = map_page(SCRATCH_PAGE, pa);
	int i;

	for (i = 0; i < PAGE_SIZE; i++)
		p[i] = 0;
}

static unsigned
user_pd_create(void)
{
	unsigned pdpa = page_alloc();
	unsigned *pd = map_page(SCRATCH_PD, pdpa);
	int i;

	for (i = 0; i < PAGE_SIZE / 4; i++)
		pd[i] = 0;
	/* share kernel high-half + proc0 kernel stack mappings */
	for (i = 0x3f6; i <= 0x3fa; i++)
		pd[i] = ((unsigned *)kernel_pmap->pm_pdir)[i];
	return (pdpa);
}

static void
user_map(unsigned va, unsigned pa)
{
	unsigned pdi = va >> 22, pti = (va >> 12) & 0x3ff;
	unsigned *pd = (unsigned *)SCRATCH_PD;
	unsigned *pt;
	unsigned ptpa;
	int i;

	if ((pd[pdi] & PG_V) == 0) {
		ptpa = page_alloc();
		pt = map_page(SCRATCH_PT, ptpa);
		for (i = 0; i < PAGE_SIZE / 4; i++)
			pt[i] = 0;
		pd[pdi] = ptpa | PG_V | PG_RW | PG_u;
	} else {
		ptpa = pd[pdi] & PG_FRAME;
		pt = map_page(SCRATCH_PT, ptpa);
	}
	pt[pti] = pa | PG_V | PG_RW | PG_u;
}

static void
load_elf(unsigned pdpa, const unsigned char *img, unsigned len, unsigned *entry)
{
	const unsigned *eh = (const unsigned *)img;
	unsigned phoff = eh[7];
	unsigned phnum = *(const unsigned short *)(img + 0x2c);
	unsigned i;

	*entry = eh[6];
	for (i = 0; i < phnum; i++) {
		const unsigned *ph = (const unsigned *)(img + phoff + i * 32);
		unsigned type = ph[0], off = ph[1], vaddr = ph[2];
		unsigned filesz = ph[4], memsz = ph[5];
		unsigned n;

		if (type != 1)
			continue;
		for (n = 0; n < memsz; n += PAGE_SIZE) {
			unsigned pa = page_alloc();
			char *dst;
			unsigned chunk = memsz - n;

			if (chunk > PAGE_SIZE)
				chunk = PAGE_SIZE;
			zero_page(pa);
			dst = map_page(SCRATCH_PAGE, pa);
			if (n < filesz) {
				unsigned c = filesz - n;

				if (c > chunk)
					c = chunk;
				bcopy(img + off + n, dst, c);
			}
			user_map(vaddr + n, pa);
		}
	}
}

static void
map_stack(unsigned pdpa)
{
	unsigned va, pa;

	for (va = USER_STACK_TOP - USER_STACK_SIZE; va < USER_ARG_PAGE;
	    va += PAGE_SIZE) {
		pa = page_alloc();
		zero_page(pa);
		user_map(va, pa);
	}
}

/*
 * Lay out argc/argv/envp on a fresh page mapped at USER_ARG_PAGE.
 * Returns the user ESP to start with.
 */
static unsigned
build_args(char **argv, int argc, char **envp, int envc)
{
	unsigned pa = page_alloc();
	char *pg;
	unsigned esp = USER_STACK_TOP;
	unsigned aptr[MAXARG], eptr[MAXENV];
	int i, n;

	zero_page(pa);
	pg = map_page(SCRATCH_PAGE, pa);

	for (i = 0; i < envc; i++) {
		n = strlen(envp[i]) + 1;
		esp -= n;
		bcopy(envp[i], pg + (esp - USER_ARG_PAGE), n);
		eptr[i] = esp;
	}
	for (i = 0; i < argc; i++) {
		n = strlen(argv[i]) + 1;
		esp -= n;
		bcopy(argv[i], pg + (esp - USER_ARG_PAGE), n);
		aptr[i] = esp;
	}
	esp &= ~3;

	esp -= 4;
	*(unsigned *)(pg + (esp - USER_ARG_PAGE)) = 0;		/* envp NULL */
	for (i = envc - 1; i >= 0; i--) {
		esp -= 4;
		*(unsigned *)(pg + (esp - USER_ARG_PAGE)) = eptr[i];
	}
	esp -= 4;
	*(unsigned *)(pg + (esp - USER_ARG_PAGE)) = 0;		/* argv NULL */
	for (i = argc - 1; i >= 0; i--) {
		esp -= 4;
		*(unsigned *)(pg + (esp - USER_ARG_PAGE)) = aptr[i];
	}
	esp -= 4;
	*(unsigned *)(pg + (esp - USER_ARG_PAGE)) = argc;

	user_map(USER_ARG_PAGE, pa);
	return (esp);
}

static unsigned
build_image(const unsigned char *img, unsigned len, unsigned *entry,
    char **argv, int argc, char **envp, int envc, unsigned *uesp)
{
	unsigned pd = user_pd_create();

	load_elf(pd, img, len, entry);
	map_stack(pd);
	*uesp = build_args(argv, argc, envp, envc);
	return (pd);
}

/* ------------------------------------------------------------------ ramfs */

static int
user_str(unsigned p, char *dst, int max)
{
	int i;

	if (p < USER_LOAD || p >= 0xC0000000)
		return (-1);
	for (i = 0; i < max - 1; i++) {
		dst[i] = ((const char *)p)[i];
		if (dst[i] == 0)
			return (i);
	}
	dst[max - 1] = 0;
	return (-1);
}

static int
user_word(unsigned p, unsigned *vp)
{

	if (p < USER_LOAD || p >= 0xC0000000)
		return (-1);
	*vp = *(const unsigned *)p;
	return (0);
}

static struct file *
fs_lookup(const char *name)
{
	struct file *f;

	for (f = ramfs; f->f_name; f++)
		if (!strcmp(f->f_name, name) && f->f_data != 0)
			return (f);
	return (0);
}

static int
fd_alloc(struct file *f)
{
	int i;

	for (i = 3; i < NFD; i++) {
		if (!fds[i].used) {
			fds[i].used = 1;
			fds[i].d = f->f_data;
			fds[i].len = *f->f_len;
			fds[i].off = 0;
			return (i);
		}
	}
	return (-1);
}

/* --------------------------------------------------------------- syscalls */

struct sysregs {
	unsigned ds, es, edi, esi, ebp, esp, ebx, edx, ecx, eax;
	unsigned eip, cs, eflags, uesp, ss;
};

static int
user_ok(unsigned p, unsigned len)
{

	if (len == 0)
		return (1);
	if (p < USER_LOAD || p >= 0xC0000000)
		return (0);
	if (len > 0xC0000000 - p)
		return (0);
	return (1);
}

/*
 * Fill a 4.4BSD i386 `struct stat` (96 bytes):
 *	0 dev, 4 ino, 8 mode(16), 10 nlink(16), 12 uid, 16 gid, 20 rdev,
 *	24 atimespec, 32 mtimespec, 40 ctimespec,
 *	48 size(64), 56 blocks(64), 64 blksize, 68 flags, 72 gen,
 *	76 lspare, 80 qspare[2]
 */
#define	STAT_SIZE	96

static void
fill_stat(unsigned mode, unsigned size, unsigned *sb)
{
	char *p = (char *)sb;
	int i;

	for (i = 0; i < STAT_SIZE; i++)
		p[i] = 0;
	*(unsigned *)(p + 0) = 0;		/* st_dev */
	*(unsigned *)(p + 4) = 1;		/* st_ino */
	*(unsigned short *)(p + 8) = (unsigned short)mode;
	*(unsigned short *)(p + 10) = 1;	/* st_nlink */
	*(unsigned *)(p + 48) = size;		/* st_size */
	*(unsigned *)(p + 52) = 0;
	*(unsigned *)(p + 64) = 4096;		/* st_blksize */
}

static int
exec_image(const char *path, unsigned uargv, unsigned uenvp, struct sysregs *r)
{
	struct file *f;
	unsigned pd, entry, esp;
	char *argv[MAXARG], *envp[MAXENV];
	static char argbuf[1024];
	int argc = 0, envc = 0;
	unsigned p, n;
	char *b = argbuf;

	f = fs_lookup(path);
	if (f == 0)
		return (-ENOENT);

	if (uargv != 0) {
		while (argc < MAXARG && user_word(uargv, &p) == 0 && p != 0) {
			n = user_str(p, b, 128);
			if ((int)n < 0)
				return (-EFAULT);
			argv[argc++] = b;
			b += n + 1;
			uargv += 4;
		}
	}
	envp[envc++] = "PATH=/bin:/sbin";

	pd = build_image(f->f_data, *f->f_len, &entry, argv, argc, envp, envc,
	    &esp);
	brk_mapped = USER_HEAP_START;
	printf("execve %s: entry %x esp %x pd %x argc %d\n",
	    path, entry, esp, pd, argc);
	__asm__ volatile("movl %0, %%cr3" :: "r"(pd) : "memory");
	r->eip = entry;
	r->uesp = esp;
	r->eax = 0;
	return (0);
}

void
syscall_dispatch(struct sysregs *r)
{
	unsigned n = r->eax;

	switch (n) {
	case 4: {			/* write(fd, buf, len) */
		unsigned fd = r->ebx, buf = r->ecx, len = r->edx;
		unsigned i;

		if ((fd != 1 && fd != 2) || !user_ok(buf, len)) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		for (i = 0; i < len; i++)
			cnputc(((const char *)buf)[i]);
		r->eax = len;
		break;
	}
	case 3: {			/* read(fd, buf, len) */
		unsigned fd = r->ebx, buf = r->ecx, len = r->edx;
		unsigned i = 0;
		int c;

		if (!user_ok(buf, len) || len == 0) {
			r->eax = (unsigned)-EINVAL;
			break;
		}
		if (fd == 0) {
			while (i < len - 1) {
				c = cngetc();
				if (c == 0)
					continue;
				if (c == '\r' || c == '\n') {
					cnputc('\n');
					((char *)buf)[i++] = '\n';
					break;
				}
				if (c == '\b') {
					if (i > 0) {
						i--;
						cnputc('\b');
						cnputc(' ');
						cnputc('\b');
					}
					continue;
				}
				cnputc(c);
				((char *)buf)[i++] = c;
			}
			r->eax = i;
			break;
		}
		if (fd >= NFD || !fds[fd].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		while (i < len && fds[fd].off < fds[fd].len)
			((char *)buf)[i++] = fds[fd].d[fds[fd].off++];
		r->eax = i;
		break;
	}
	case 5: {			/* open(path, flags, mode) */
		char path[128];
		struct file *f;
		int fd;

		if (user_str(r->ebx, path, sizeof(path)) < 0) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		f = fs_lookup(path);
		if (f == 0) {
			r->eax = (unsigned)-ENOENT;
			break;
		}
		fd = fd_alloc(f);
		r->eax = (fd < 0) ? (unsigned)-EMFILE : (unsigned)fd;
		break;
	}
	case 6:				/* close(fd) */
		if (r->ebx < NFD)
			fds[r->ebx].used = 0;
		r->eax = 0;
		break;
	case 59: {			/* execve(path, argv, envp) */
		char path[128];

		if (user_str(r->ebx, path, sizeof(path)) < 0) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		n = (unsigned)exec_image(path, r->ecx, r->edx, r);
		if ((int)n < 0)
			r->eax = n;
		break;
	}
	case 1:				/* exit(code) */
		printf("\n[user exit %d]\n", r->ebx);
		for (;;)
			__asm__ volatile("cli; hlt");
	case 17: {			/* break(addr) */
		unsigned a = r->ebx;

		if (a < USER_LOAD || a > USER_HEAP_MAX) {
			r->eax = (unsigned)-EINVAL;
			break;
		}
		while (brk_mapped < a) {
			user_map(brk_mapped, page_alloc());
			brk_mapped += PAGE_SIZE;
		}

		r->eax = a;
		break;
	}
	case 20:			/* getpid */
		r->eax = 1;
		break;
	case 39:			/* getppid */
		r->eax = 0;
		break;
	case 24: case 25: case 43: case 47:	/* uid/gid */
		r->eax = 0;
		break;
	case 81:			/* getpgrp */
		r->eax = 1;
		break;
	case 82:			/* setpgid */
		r->eax = 0;
		break;
	case 33: {			/* access(path, mode) */
		char path[128];

		if (user_str(r->ebx, path, sizeof(path)) < 0) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		r->eax = fs_lookup(path) ? 0 : (unsigned)-ENOENT;
		break;
	}
	case 46: case 48: case 52: case 53:	/* signals: accept and ignore */
		r->eax = 0;
		break;
	case 37:			/* kill */
		r->eax = 0;
		break;
	case 54:			/* ioctl */
		r->eax = (unsigned)-ENOTTY;
		break;
	case 60: {			/* umask(mask) */
		unsigned old = umask_val;

		umask_val = r->ebx;
		r->eax = old;
		break;
	}
	case 92:			/* fcntl */
		r->eax = 0;
		break;
	case 189: {			/* stat(path, sb) */
		char path[128];
		struct file *f;

		if (user_str(r->ebx, path, sizeof(path)) < 0 ||
		    !user_ok(r->ecx, STAT_SIZE)) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		f = fs_lookup(path);
		if (f == 0) {
			r->eax = (unsigned)-ENOENT;
			break;
		}
		fill_stat(0100755, *f->f_len, (unsigned *)r->ecx);
		r->eax = 0;
		break;
	}
	case 188: {			/* fstat(fd, sb) */
		if (!user_ok(r->ecx, STAT_SIZE)) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		if (r->ebx < 3) {
			fill_stat(0020620, 0, (unsigned *)r->ecx);
			r->eax = 0;
			break;
		}
		if (r->ebx >= NFD || !fds[r->ebx].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		fill_stat(0100755, fds[r->ebx].len, (unsigned *)r->ecx);
		r->eax = 0;
		break;
	}
	case 199: {			/* lseek(fd, off, whence) */
		unsigned fd = r->ebx, off = r->ecx, wh = r->edx;

		if (fd >= NFD || !fds[fd].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		if (wh == 0)
			fds[fd].off = off;
		else if (wh == 1)
			fds[fd].off += off;
		else
			fds[fd].off = fds[fd].len;
		r->eax = fds[fd].off;
		break;
	}
	case 89:			/* getdtablesize */
		r->eax = NFD;
		break;
	case 7:				/* wait4 */
		r->eax = (unsigned)-ECHILD;
		break;
	case 2: case 66:		/* fork / vfork */
		r->eax = (unsigned)-ENOSYS;
		break;
	case 41:			/* dup */
		r->eax = (unsigned)-ENOSYS;
		break;
	case 42:			/* pipe */
		r->eax = (unsigned)-ENOSYS;
		break;
	case 90: {			/* dup2(from, to) */
		if (r->ecx < NFD && (int)r->ebx >= 0 && r->ebx < NFD) {
			fds[r->ecx] = fds[r->ebx];
			r->eax = r->ecx;
		} else {
			r->eax = (unsigned)-EBADF;
		}
		break;
	}
	case 12:			/* chdir */
		r->eax = (unsigned)-ENOENT;
		break;
	case 74: case 73: case 65:	/* mprotect/munmap/msync */
		r->eax = 0;
		break;
	default:
		r->eax = (unsigned)-ENOSYS;
		break;
	}
}

void
syscall_gate_init(void)
{

	setidt(0x80, (char *)&syscall_entry, SDT_SYS386TGT, SEL_UPL);
}

void
start_prog(const char *path, const char *script)
{
	struct file *f;
	unsigned pd, entry, esp;
	char *argv[4];
	int argc;
	char *envp[2];

	f = fs_lookup(path);
	if (f == 0) {
		printf("ramfs: no %s\n", path);
		return;
	}
	argv[0] = (char *)path;
	argc = 1;
	if (script != 0) {
		argv[argc++] = "-c";
		argv[argc++] = (char *)script;
	}
	envp[0] = "PATH=/bin:/sbin";

	pd = build_image(f->f_data, *f->f_len, &entry, argv, argc, envp, 1,
	    &esp);
	brk_mapped = USER_HEAP_START;
	syscall_gate_init();
	printf("exec %s: entry %x pd %x\n", path, entry, pd);
	enter_user(pd, entry, esp);
}

void
start_userland(const char *script)
{

	start_prog("/sbin/init", script);
}
