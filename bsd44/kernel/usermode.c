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

#include "atapi.h"
#include "isofs.h"

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
#define	EAGAIN		35
#define	ENOSYS		78

extern const unsigned char hello_elf[], init_elf[];
extern const unsigned int hello_elf_len, init_elf_len;
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
	{ "/hello",	hello_elf,	&hello_elf_len },
	{ "/etc/profile", 0, 0 },
	{ 0, 0, 0 }
};

struct fd {
	const unsigned char	*d;
	unsigned		len;
	unsigned		off;
	int			used;
};

/* saved user context, filled by syscall_entry/enter_user */
struct sysregs {
	unsigned ds, es, edi, esi, ebp, esp, ebx, edx, ecx, eax;
	unsigned eip, cs, eflags, uesp, ss;
};

#define	NPROC		8
#define	P_FREE		0
#define	P_RUN		1
#define	P_BLOCKED	2
#define	P_ZOMBIE	3

struct uproc {
	int		state;
	int		pid;		/* 1 = init */
	int		ppid;
	unsigned	pd;		/* page directory */
	unsigned	brk_mapped;	/* next heap page to map */
	struct fd	fds[NFD];
	struct sysregs	regs;		/* context to resume */
	int		exitcode;
	int		wait_pid;	/* pid being waited for (0 = any) */
	unsigned	wait_status;	/* user status pointer for wait4 */
	int		vfork_wait;	/* blocked in vfork() */
	int		vfork_child;	/* shares the parent's address space */
};

static struct uproc	procs[NPROC];
static struct uproc	*cur;		/* current process */
static int		next_pid = 2;

static unsigned next_pa = 0x200000;
static unsigned page_alloc(void);

#define	CD_IMAGE_VA	0xFE400000	/* kernel window for a file read from CD */
static unsigned cd_mapped;		/* bytes mapped at CD_IMAGE_VA */

/*
 * Map (lazily) a kernel window big enough for one image read from the
 * ISO9660 root.  Physical pages come from the bump allocator, so they are
 * contiguous, but we map them one page at a time.
 */
static unsigned char *
cd_map(unsigned size)
{
	unsigned need = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	unsigned i;

	if (need == 0 || need > 4 * 1024 * 1024)
		return (0);
	for (i = cd_mapped; i < need; i += PAGE_SIZE) {
		unsigned pa = page_alloc();

		pmap_map((vm_offset_t)(CD_IMAGE_VA + i), (vm_offset_t)pa,
		    (vm_offset_t)pa + PAGE_SIZE,
		    VM_PROT_READ | VM_PROT_WRITE);
	}
	cd_mapped = need;
	return ((unsigned char *)CD_IMAGE_VA);
}
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

/*
 * Map one user page in a *specific* page directory.  The caller passes the
 * directory because after a fork/exec SCRATCH_PD no longer points at the
 * process we are editing (that bug silently unmapped the parent's heap).
 */
static void
user_map(unsigned pdpa, unsigned va, unsigned pa)
{
	unsigned pdi = va >> 22, pti = (va >> 12) & 0x3ff;
	unsigned *pd = map_page(SCRATCH_PD, pdpa);
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
			user_map(pdpa, vaddr + n, pa);
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
		user_map(pdpa, va, pa);
	}
}

/*
 * Lay out argc/argv/envp on a fresh page mapped at USER_ARG_PAGE.
 * Returns the user ESP to start with.
 */
static unsigned
build_args(unsigned pdpa, char **argv, int argc, char **envp, int envc)
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

	user_map(pdpa, USER_ARG_PAGE, pa);
	return (esp);
}

static unsigned
build_image(const unsigned char *img, unsigned len, unsigned *entry,
    char **argv, int argc, char **envp, int envc, unsigned *uesp)
{
	unsigned pd = user_pd_create();

	load_elf(pd, img, len, entry);
	map_stack(pd);
	*uesp = build_args(pd, argv, argc, envp, envc);
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
isofile_exists(const char *path)
{
	unsigned lba, size;

	return (iso_mounted() && iso_lookup(path, &lba, &size) == 0);
}

static int
fd_alloc(struct file *f)
{
	int i;

	for (i = 3; i < NFD; i++) {
		if (!cur->fds[i].used) {
			cur->fds[i].used = 1;
			cur->fds[i].d = f->f_data;
			cur->fds[i].len = *f->f_len;
			cur->fds[i].off = 0;
			return (i);
		}
	}
	return (-1);
}

/* --------------------------------------------------------------- process */

static int user_ok(unsigned p, unsigned len);
extern void resume_user(unsigned pd, struct sysregs *r);

#define	SCRATCH2_PD	0xFE203000
#define	SCRATCH2_PT	0xFE204000
#define	SCRATCH2_PAGE	0xFE205000

/*
 * Deep copy of a user address space: new page directory, new page tables
 * and new pages with the same contents.  The kernel's high mappings are
 * shared, as usual.
 */
static unsigned
copy_address_space(unsigned srcpd)
{
	unsigned dstpd = user_pd_create();
	unsigned *spd = map_page(SCRATCH_PD, srcpd);
	unsigned *dpd = map_page(SCRATCH2_PD, dstpd);
	int i, j;

	for (i = 0; i < 0x3f6; i++) {
		unsigned *spt, *dpt;
		unsigned dptpa;

		if ((spd[i] & PG_V) == 0)
			continue;
		dptpa = page_alloc();
		spt = map_page(SCRATCH_PT, spd[i] & PG_FRAME);
		dpt = map_page(SCRATCH2_PT, dptpa);
		for (j = 0; j < PAGE_SIZE / 4; j++)
			dpt[j] = 0;
		for (j = 0; j < PAGE_SIZE / 4; j++) {
			unsigned spa, dpa;
			char *s, *d;

			if ((spt[j] & PG_V) == 0)
				continue;
			spa = spt[j] & PG_FRAME;
			dpa = page_alloc();
			s = map_page(SCRATCH_PAGE, spa);
			d = map_page(SCRATCH2_PAGE, dpa);
			bcopy(s, d, PAGE_SIZE);
			dpt[j] = dpa | (spt[j] & 0xfff);
		}
		dpd[i] = dptpa | (spd[i] & 0xfff);
	}
	return (dstpd);
}

static struct uproc *
proc_alloc(void)
{
	int i;

	for (i = 0; i < NPROC; i++)
		if (procs[i].state == P_FREE) {
			bzero(&procs[i], sizeof(procs[i]));
			return (&procs[i]);
		}
	return (0);
}

static struct uproc *
proc_find(int pid)
{
	int i;

	for (i = 0; i < NPROC; i++)
		if (procs[i].state != P_FREE && procs[i].pid == pid)
			return (&procs[i]);
	return (0);
}

static int
proc_children(int pid)
{
	int i, n = 0;

	for (i = 0; i < NPROC; i++)
		if (procs[i].state != P_FREE && procs[i].ppid == pid)
			n++;
	return (n);
}

/*
 * Pick the next runnable process and return to user mode.
 * Never returns.
 */
static void
proc_sched(void)
{
	struct uproc *me = cur;
	int i;

	for (i = 0; i < NPROC; i++) {
		struct uproc *p = &procs[i];

		if (p->state == P_RUN && p != me) {
			cur = p;
			resume_user(p->pd, &p->regs);
		}
	}
	if (me->state == P_RUN) {
		cur = me;
		resume_user(me->pd, &me->regs);
	}
	printf("\n[no runnable process]\n");
	for (;;)
		__asm__ volatile("cli; hlt");
}

static int
do_fork(struct sysregs *r)
{
	struct uproc *p;
	unsigned pd;

	if ((p = proc_alloc()) == 0)
		return (-EAGAIN);
	pd = copy_address_space(cur->pd);
	if (pd == 0) {
		p->state = P_FREE;
		return (-ENOMEM);
	}
	p->state = P_RUN;
	p->pid = next_pid++;
	p->ppid = cur->pid;
	p->pd = pd;
	p->brk_mapped = cur->brk_mapped;
	bcopy(cur->fds, p->fds, sizeof(p->fds));
	p->regs = *r;
	p->regs.eax = 0;		/* the child returns 0 from fork() */

	return (p->pid);
}

static int
do_vfork(struct sysregs *r)
{
	struct uproc *p;
	unsigned pd;

	if ((p = proc_alloc()) == 0)
		return (-EAGAIN);
	/*
	 * A strict vfork shares the address space, but our userland has no
	 * way to protect the parent's stack frame while the child runs (the
	 * child's own libc calls would clobber it).  A private copy plus the
	 * "parent waits for exec/exit" rule is a safe superset.
	 */
	pd = copy_address_space(cur->pd);
	if (pd == 0) {
		p->state = P_FREE;
		return (-ENOMEM);
	}
	p->state = P_RUN;
	p->pid = next_pid++;
	p->ppid = cur->pid;
	p->pd = pd;
	p->brk_mapped = cur->brk_mapped;
	bcopy(cur->fds, p->fds, sizeof(p->fds));
	p->regs = *r;
	p->regs.eax = 0;
	p->vfork_child = 1;

	/* the parent blocks until the child execs or exits */
	cur->regs = *r;
	cur->state = P_BLOCKED;
	cur->vfork_wait = 1;
	proc_sched();
	return (0);
}

static void
proc_exit(int code)
{
	struct uproc *p;
	int i, reaped = 0;

	for (i = 0; i < NPROC; i++) {
		p = &procs[i];
		if (p->state != P_BLOCKED || p->pid != cur->ppid)
			continue;
		if (p->vfork_wait) {
			p->vfork_wait = 0;
			p->regs.eax = cur->pid;
			p->state = P_RUN;
			continue;
		}
		if (p->wait_pid != 0 && p->wait_pid != cur->pid)
			continue;
		if (p->wait_status != 0 && user_ok(p->wait_status, 4)) {
			unsigned save = cur->pd;

			__asm__ volatile("movl %0,%%cr3"
			    :: "r"(p->pd) : "memory");
			*(int *)p->wait_status = (code & 0xff) << 8;
			__asm__ volatile("movl %0,%%cr3"
			    :: "r"(save) : "memory");
		}
		p->regs.eax = cur->pid;
		p->state = P_RUN;
		reaped = 1;		/* the parent's wait4 gets the status */
	}

	cur->exitcode = code;
	cur->state = reaped ? P_FREE : P_ZOMBIE;
	if (cur->ppid == 0) {
		printf("\n[user exit %d]\n", code);
		for (;;)
			__asm__ volatile("cli; hlt");
	}
	proc_sched();
}

static int
do_wait4(int want, unsigned statusp, struct sysregs *r)
{
	struct uproc *p;
	int i, pid;

	for (i = 0; i < NPROC; i++) {
		p = &procs[i];
		if (p->state != P_ZOMBIE || p->ppid != cur->pid)
			continue;
		if (want > 0 && p->pid != want)
			continue;
		pid = p->pid;
		if (statusp != 0 && user_ok(statusp, 4))
			*(int *)statusp = (p->exitcode & 0xff) << 8;
		p->state = P_FREE;
		r->eax = pid;
		return (0);
	}
	if (proc_children(cur->pid) == 0) {
		r->eax = (unsigned)-ECHILD;
		return (0);
	}
	cur->regs = *r;
	cur->wait_pid = (want > 0) ? want : 0;
	cur->wait_status = statusp;
	cur->state = P_BLOCKED;
	proc_sched();
	r->eax = 0;
	return (0);
}

/* --------------------------------------------------------------- syscalls */

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
	const unsigned char *img;
	unsigned len;
	unsigned pd, entry, esp;
	char *argv[MAXARG], *envp[MAXENV];
	static char argbuf[1024];
	int argc = 0, envc = 0;
	unsigned p, n;
	char *b = argbuf;

	/*
	 * Prefer the ISO9660 root: S4 moved /sbin/init and /bin/sh onto the
	 * CD.  The embedded ramfs images stay as a fallback for a kernel
	 * booted without a disc.
	 */
	f = 0;
	{
		unsigned lba, fsize;
		unsigned char *buf;

		if (iso_mounted() &&
		    iso_lookup(path, &lba, &fsize) == 0 &&
		    (buf = cd_map(fsize)) != 0 &&
		    atapi_read(lba, (fsize + 2047) / 2048, buf) == 0) {
			img = buf;
			len = fsize;
			printf("execve %s: from ISO9660 (%d bytes, lba %d)\n",
			    path, fsize, lba);
		} else if ((f = fs_lookup(path)) != 0) {
			img = f->f_data;
			len = *f->f_len;
			printf("execve %s: from ramfs (%d bytes)\n", path, len);
		} else {
			return (-ENOENT);
		}
	}

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

	pd = build_image(img, len, &entry, argv, argc, envp, envc, &esp);
	cur->pd = pd;
	cur->brk_mapped = USER_HEAP_START;
	printf("execve %s: entry %x esp %x pd %x argc %d\n",
	    path, entry, esp, pd, argc);
	__asm__ volatile("movl %0, %%cr3" :: "r"(pd) : "memory");
	r->eip = entry;
	r->uesp = esp;
	r->eax = 0;
	if (cur->vfork_child) {
		struct uproc *pp = proc_find(cur->ppid);

		cur->vfork_child = 0;
		if (pp != 0 && pp->state == P_BLOCKED && pp->vfork_wait) {
			pp->vfork_wait = 0;
			pp->regs.eax = cur->pid;
			pp->state = P_RUN;
			cur->regs = *r;
			cur->state = P_RUN;
			cur = pp;
			resume_user(pp->pd, &pp->regs);
		}
	}
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
		if (fd >= NFD || !cur->fds[fd].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		while (i < len && cur->fds[fd].off < cur->fds[fd].len)
			((char *)buf)[i++] = cur->fds[fd].d[cur->fds[fd].off++];
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
			cur->fds[r->ebx].used = 0;
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
		proc_exit((int)r->ebx);
		break;
	case 17: {			/* break(addr) */
		unsigned a = r->ebx;

		if (a < USER_LOAD || a > USER_HEAP_MAX) {
			r->eax = (unsigned)-EINVAL;
			break;
		}
		while (cur->brk_mapped < a) {
			user_map(cur->pd, cur->brk_mapped, page_alloc());
			cur->brk_mapped += PAGE_SIZE;
		}
		r->eax = a;
		break;
	}
	case 20:			/* getpid */
		r->eax = cur->pid;
		break;
	case 39:			/* getppid */
		r->eax = cur->ppid;
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
		if (fs_lookup(path) != 0 || isofile_exists(path))
			r->eax = 0;
		else
			r->eax = (unsigned)-ENOENT;
		break;
	}
	case 46: case 48: case 52: case 53:	/* signals: accept and ignore */
		r->eax = 0;
		break;
	case 37:			/* kill(pid, sig) */
		if (r->ecx == 0)
			r->eax = proc_find((int)r->ebx) ? 0 : (unsigned)-ESRCH;
		else
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
		unsigned lba, size;

		if (user_str(r->ebx, path, sizeof(path)) < 0 ||
		    !user_ok(r->ecx, STAT_SIZE)) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		if (iso_mounted() && iso_lookup(path, &lba, &size) == 0) {
			fill_stat(0100755, size, (unsigned *)r->ecx);
			r->eax = 0;
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
		if (r->ebx >= NFD || !cur->fds[r->ebx].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		fill_stat(0100755, cur->fds[r->ebx].len, (unsigned *)r->ecx);
		r->eax = 0;
		break;
	}
	case 199: {			/* lseek(fd, off, whence) */
		unsigned fd = r->ebx, off = r->ecx, wh = r->edx;

		if (fd >= NFD || !cur->fds[fd].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		if (wh == 0)
			cur->fds[fd].off = off;
		else if (wh == 1)
			cur->fds[fd].off += off;
		else
			cur->fds[fd].off = cur->fds[fd].len;
		r->eax = cur->fds[fd].off;
		break;
	}
	case 89:			/* getdtablesize */
		r->eax = NFD;
		break;
	case 7:				/* wait4(pid, status, options, rusage) */
		do_wait4((int)r->ebx, r->ecx, r);
		break;
	case 2:				/* fork */
		r->eax = (unsigned)do_fork(r);
		break;
	case 66:			/* vfork */
		r->eax = (unsigned)do_vfork(r);
		break;
	case 41:			/* dup */
		r->eax = (unsigned)-ENOSYS;
		break;
	case 42:			/* pipe */
		r->eax = (unsigned)-ENOSYS;
		break;
	case 90: {			/* dup2(from, to) */
		if (r->ecx < NFD && (int)r->ebx >= 0 && r->ebx < NFD) {
			cur->fds[r->ecx] = cur->fds[r->ebx];
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
	const unsigned char *img;
	unsigned len;
	unsigned pd, entry, esp;
	char *argv[4];
	int argc;
	char *envp[2];

	img = 0;
	len = 0;
	if ((f = fs_lookup(path)) != 0) {
		img = f->f_data;
		len = *f->f_len;
	} else {
		unsigned lba, fsize;
		unsigned char *buf;

		if (iso_mounted() && iso_lookup(path, &lba, &fsize) == 0 &&
		    (buf = cd_map(fsize)) != 0 &&
		    atapi_read(lba, (fsize + 2047) / 2048, buf) == 0) {
			img = buf;
			len = fsize;
			printf("exec %s: from ISO9660 (%d bytes)\n",
			    path, fsize);
		} else {
			printf("no image for %s\n", path);
			return;
		}
	}
	argv[0] = (char *)path;
	argc = 1;
	if (script != 0) {
		argv[argc++] = "-c";
		argv[argc++] = (char *)script;
	}
	envp[0] = "PATH=/bin:/sbin";

	pd = build_image(img, len, &entry, argv, argc, envp, 1, &esp);


	/* pid 1: the first user process, no parent */
	bzero(procs, sizeof(procs));
	cur = &procs[0];
	cur->state = P_RUN;
	cur->pid = 1;
	cur->ppid = 0;
	cur->pd = pd;
	cur->brk_mapped = USER_HEAP_START;
	next_pid = 2;

	syscall_gate_init();
	printf("exec %s: entry %x pd %x\n", path, entry, pd);
	enter_user(pd, entry, esp);
}

void
start_userland(const char *script)
{

	start_prog("/sbin/init", script);
}
