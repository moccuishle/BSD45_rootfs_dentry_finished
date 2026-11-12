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
#include <sys/termios.h>
#include <sys/ttycom.h>

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

#define	FD_FILE	0
#define	FD_DIR	1
#define	FD_PIPE	2

struct fd {
	const unsigned char	*d;
	unsigned		len;
	unsigned		off;
	int			used;
	int			type;		/* FD_FILE, FD_DIR or FD_PIPE */
	unsigned		lba;		/* directory start lba */
	unsigned		dsize;		/* directory size in bytes */
	unsigned		dpos;		/* next directory record offset */
	int			pipeidx;	/* pipe table slot */
	int			rw;		/* 1 = read end, 2 = write end */
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
	int		pipeidx;	/* pipe we are blocked on */
	int		piperw;		/* 1 = read, 2 = write */
	unsigned	pipeat;		/* pending user buffer */
	unsigned	pipelen;	/* pending user length */
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
			cur->fds[i].type = FD_FILE;
			cur->fds[i].d = f->f_data;
			cur->fds[i].len = *f->f_len;
			cur->fds[i].off = 0;
			cur->fds[i].lba = 0;
			cur->fds[i].dsize = 0;
			cur->fds[i].dpos = 0;
			cur->fds[i].pipeidx = -1;
			cur->fds[i].rw = 0;
			return (i);
		}
	}
	return (-1);
}

static int
fd_alloc_dir(unsigned lba, unsigned size)
{
	int i;

	for (i = 3; i < NFD; i++) {
		if (!cur->fds[i].used) {
			cur->fds[i].used = 1;
			cur->fds[i].type = FD_DIR;
			cur->fds[i].d = 0;
			cur->fds[i].len = size;
			cur->fds[i].off = 0;
			cur->fds[i].lba = lba;
			cur->fds[i].dsize = size;
			cur->fds[i].dpos = 0;
			cur->fds[i].pipeidx = -1;
			cur->fds[i].rw = 0;
			return (i);
		}
	}
	return (-1);
}


/* ---------------------------------------------------------------- pipes */

static struct uproc *proc_find(int pid);
static void proc_sched(void);

#define	NPIPE	8
#define	PIPESZ	4096

struct pipe {
	int		used;
	int		readers;
	int		writers;
	unsigned	rpos, wpos, count;
	int		rpid;		/* pid blocked in read */
	int		wpid;		/* pid blocked in write */
	unsigned char	buf[PIPESZ];
};

static struct pipe pipes[NPIPE];

static struct pipe *
pipe_alloc(void)
{
	int i;

	for (i = 0; i < NPIPE; i++) {
		if (!pipes[i].used) {
			bzero(&pipes[i], sizeof(pipes[i]));
			pipes[i].used = 1;
			return (&pipes[i]);
		}
	}
	return (0);
}

static void
pipe_copy(unsigned pd, unsigned va, const void *kbuf, unsigned n, int touser)
{
	unsigned save;

	__asm__ volatile("movl %%cr3,%0" : "=r"(save));
	__asm__ volatile("movl %0,%%cr3" :: "r"(pd) : "memory");
	if (touser)
		bcopy(kbuf, (void *)va, n);
	else
		bcopy((const void *)va, (void *)kbuf, n);
	__asm__ volatile("movl %0,%%cr3" :: "r"(save) : "memory");
}

/*
 * Hand pending data to a reader blocked inside read().  The copy happens in
 * the writer's context, so the reader's page directory is installed for the
 * duration; the kernel is mapped in both.
 */
static void
pipe_wake_reader(struct pipe *pp, int eof)
{
	struct uproc *p;
	unsigned n, first;

	if (pp->rpid == 0)
		return;
	p = proc_find(pp->rpid);
	pp->rpid = 0;
	if (p == 0 || p->state != P_BLOCKED || p->pipeidx != pp - pipes)
		return;
	if (eof) {
		p->regs.eax = 0;
		p->state = P_RUN;
		return;
	}
	n = pp->count;
	if (n > p->pipelen)
		n = p->pipelen;
	first = PIPESZ - pp->rpos;
	if (first > n)
		first = n;
	pipe_copy(p->pd, p->pipeat, pp->buf + pp->rpos, first, 1);
	if (n > first)
		pipe_copy(p->pd, p->pipeat + first, pp->buf, n - first, 1);
	pp->rpos = (pp->rpos + n) % PIPESZ;
	pp->count -= n;
	p->regs.eax = n;
	p->state = P_RUN;
}

static void
pipe_wake_writer(struct pipe *pp, int epipe)
{
	struct uproc *p;
	unsigned space, n, first;

	if (pp->wpid == 0)
		return;
	p = proc_find(pp->wpid);
	pp->wpid = 0;
	if (p == 0 || p->state != P_BLOCKED || p->pipeidx != pp - pipes)
		return;
	if (epipe) {
		p->regs.eax = (unsigned)-EPIPE;
		p->state = P_RUN;
		return;
	}
	space = PIPESZ - pp->count;
	n = p->pipelen;
	if (n > space)
		n = space;
	first = PIPESZ - pp->wpos;
	if (first > n)
		first = n;
	pipe_copy(p->pd, p->pipeat, pp->buf + pp->wpos, first, 0);
	if (n > first)
		pipe_copy(p->pd, p->pipeat + first, pp->buf, n - first, 0);
	pp->wpos = (pp->wpos + n) % PIPESZ;
	pp->count += n;
	p->regs.eax = n;
	p->state = P_RUN;
}

static void
fd_close(int fd)
{
	struct fd *fp;
	struct pipe *pp;

	if (fd < 0 || fd >= NFD || !cur->fds[fd].used)
		return;
	fp = &cur->fds[fd];
	if (fp->type == FD_PIPE && fp->pipeidx >= 0 &&
	    fp->pipeidx < NPIPE) {
		pp = &pipes[fp->pipeidx];
		if (fp->rw == 1)
			pp->readers--;
		else if (fp->rw == 2)
			pp->writers--;
		if (pp->readers <= 0)
			pipe_wake_writer(pp, 1);
		if (pp->writers <= 0)
			pipe_wake_reader(pp, 1);
		if (pp->readers <= 0 && pp->writers <= 0 &&
		    pp->wpid == 0 && pp->rpid == 0) {
			pp->used = 0;
			fp->pipeidx = -1;
		}
	}
	fp->used = 0;
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
	{
		int i;

		for (i = 0; i < NFD; i++)
			if (p->fds[i].used && p->fds[i].type == FD_PIPE &&
			    p->fds[i].pipeidx >= 0) {
				if (p->fds[i].rw == 1)
					pipes[p->fds[i].pipeidx].readers++;
				else if (p->fds[i].rw == 2)
					pipes[p->fds[i].pipeidx].writers++;
			}
	}
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

	{
		int fd;

		for (fd = 0; fd < NFD; fd++)
			if (cur->fds[fd].used)
				fd_close(fd);
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


/*
 * Canonical console input for fd 0: echo, erase (BS/DEL), kill (^U),
 * word-erase (^W) and EOF (^D on an empty line).
 */
static int
console_read(char *buf, unsigned len)
{
	unsigned i = 0;
	int c;

	while (i < len - 1) {
		c = cngetc();
		if (c == 0)
			continue;
		if (c == '\r' || c == '\n') {
			cnputc('\n');
			buf[i++] = '\n';
			break;
		}
		if (c == '\b' || c == 0x7f) {
			if (i > 0) {
				i--;
				cnputc('\b');
				cnputc(' ');
				cnputc('\b');
			}
			continue;
		}
		if (c == 0x15) {		/* ^U: kill line */
			while (i > 0) {
				i--;
				cnputc('\b');
				cnputc(' ');
				cnputc('\b');
			}
			continue;
		}
		if (c == 0x17) {		/* ^W: kill word */
			while (i > 0 && (buf[i - 1] == ' ' || buf[i - 1] == '\t')) {
				i--;
				cnputc('\b');
				cnputc(' ');
				cnputc('\b');
			}
			while (i > 0 && buf[i - 1] != ' ' && buf[i - 1] != '\t') {
				i--;
				cnputc('\b');
				cnputc(' ');
				cnputc('\b');
			}
			continue;
		}
		if (c == 0x04)			/* ^D: EOF on an empty line */
			break;
		if (c == 0x03) {		/* ^C: drop the line */
			cnputc('^');
			cnputc('C');
			cnputc('\n');
			i = 0;
			continue;
		}
		if (c < 0x20 && c != '\t')
			continue;
		cnputc(c);
		buf[i++] = c;
	}
	return ((int)i);
}

/*
 * Termios for the console.  There is no real UART line discipline: the
 * console is always in canonical mode with echo, which is exactly what
 * isatty(3)/sh want to see.
 */
static int
console_termios(unsigned arg, int get)
{
	struct termios *t = (struct termios *)arg;

	if (!user_ok(arg, sizeof(*t)))
		return (-EFAULT);
	if (get) {
		bzero(t, sizeof(*t));
		t->c_iflag = ICRNL | IXON;
		t->c_oflag = OPOST | ONLCR;
		t->c_cflag = CS8 | CREAD | CLOCAL | B9600;
		t->c_lflag = ICANON | ECHO | ECHOE | ISIG | IEXTEN;
		t->c_cc[VINTR] = 3;
		t->c_cc[VEOF] = 4;
		t->c_cc[VERASE] = 0x7f;
		t->c_cc[VKILL] = 0x15;
		t->c_cc[VSTART] = 0x11;
		t->c_cc[VSTOP] = 0x13;
		t->c_cc[VSUSP] = 0x1a;
		t->c_cc[VMIN] = 1;
		t->c_cc[VTIME] = 0;
	}
	/* TIOCSETA*: accept and ignore, the console flags are fixed */
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

		if (!user_ok(buf, len)) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		if (fd >= NFD) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		if (cur->fds[fd].used && cur->fds[fd].type == FD_PIPE) {
			struct pipe *pp = &pipes[cur->fds[fd].pipeidx];
			unsigned space, n, first;

			if (cur->fds[fd].rw != 2) {
				r->eax = (unsigned)-EBADF;
				break;
			}
			if (pp->readers <= 0) {
				r->eax = (unsigned)-EPIPE;
				break;
			}
			space = PIPESZ - pp->count;
			if (space == 0) {
				cur->regs = *r;
				cur->state = P_BLOCKED;
				cur->pipeidx = pp - pipes;
				cur->piperw = 2;
				cur->pipeat = buf;
				cur->pipelen = len;
				pp->wpid = cur->pid;
				proc_sched();
				break;		/* resumed by the reader */
			}
			n = len;
			if (n > space)
				n = space;
			first = PIPESZ - pp->wpos;
			if (first > n)
				first = n;
			bcopy((char *)buf, pp->buf + pp->wpos, first);
			if (n > first)
				bcopy((char *)buf + first, pp->buf, n - first);
			pp->wpos = (pp->wpos + n) % PIPESZ;
			pp->count += n;
			pipe_wake_reader(pp, 0);
			r->eax = n;
			break;
		}
		if (cur->fds[fd].used && fd >= 3) {	/* file: not writable */
			r->eax = (unsigned)-EBADF;
			break;
		}
		if (fd < 0 || fd > 2) {
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

		if (!user_ok(buf, len) || len == 0) {
			r->eax = (unsigned)-EINVAL;
			break;
		}
		if (fd == 0 && (fd >= NFD || !cur->fds[0].used)) {
			r->eax = (unsigned)console_read((char *)buf, len);
			break;
		}
		if (fd >= NFD || !cur->fds[fd].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		if (cur->fds[fd].type == FD_DIR) {
			r->eax = (unsigned)-EISDIR;
			break;
		}
		if (cur->fds[fd].type == FD_PIPE) {
			struct pipe *pp = &pipes[cur->fds[fd].pipeidx];
			unsigned n, first;

			if (cur->fds[fd].rw != 1) {
				r->eax = (unsigned)-EBADF;
				break;
			}
			if (pp->count == 0) {
				if (pp->writers <= 0) {		/* EOF */
					r->eax = 0;
					break;
				}
				cur->regs = *r;
				cur->state = P_BLOCKED;
				cur->pipeidx = pp - pipes;
				cur->piperw = 1;
				cur->pipeat = buf;
				cur->pipelen = len;
				pp->rpid = cur->pid;
				proc_sched();
				break;		/* resumed by the writer */
			}
			n = pp->count;
			if (n > len)
				n = len;
			first = PIPESZ - pp->rpos;
			if (first > n)
				first = n;
			bcopy(pp->buf + pp->rpos, (char *)buf, first);
			if (n > first)
				bcopy(pp->buf, (char *)buf + first, n - first);
			pp->rpos = (pp->rpos + n) % PIPESZ;
			pp->count -= n;
			pipe_wake_writer(pp, 0);
			r->eax = n;
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
		unsigned lba, dsize;
		int fd, isdir;

		if (user_str(r->ebx, path, sizeof(path)) < 0) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		f = fs_lookup(path);
		if (f != 0) {
			fd = fd_alloc(f);
			r->eax = (fd < 0) ? (unsigned)-EMFILE : (unsigned)fd;
			break;
		}
		if (iso_mounted() &&
		    iso_stat(path, &lba, &dsize, &isdir) == 0 && isdir) {
			fd = fd_alloc_dir(lba, dsize);
			r->eax = (fd < 0) ? (unsigned)-EMFILE : (unsigned)fd;
			break;
		}
		r->eax = (unsigned)-ENOENT;
		break;
	}
	case 6:				/* close(fd) */
		fd_close((int)r->ebx);
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
		unsigned lba, size;
		int isdir;

		if (user_str(r->ebx, path, sizeof(path)) < 0) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		if (fs_lookup(path) != 0 || isofile_exists(path) ||
		    (iso_mounted() && iso_stat(path, &lba, &size, &isdir) == 0))
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
	case 54: {			/* ioctl(fd, request, arg) */
		unsigned fd = r->ebx;
		unsigned long req = r->ecx;
		unsigned arg = r->edx;

		if (fd > 2) {
			r->eax = (unsigned)-ENOTTY;
			break;
		}
		switch (req) {
		case TIOCGETA:
			r->eax = (unsigned)console_termios(arg, 1);
			break;
		case TIOCSETA:
		case TIOCSETAW:
		case TIOCSETAF:
			r->eax = (unsigned)console_termios(arg, 0);
			break;
		case TIOCGWINSZ: {
			struct winsize *w = (struct winsize *)arg;

			if (!user_ok(arg, sizeof(*w))) {
				r->eax = (unsigned)-EFAULT;
				break;
			}
			w->ws_row = 25;
			w->ws_col = 80;
			w->ws_xpixel = 0;
			w->ws_ypixel = 0;
			r->eax = 0;
			break;
		}
		case TIOCGPGRP:
			if (!user_ok(arg, 4)) {
				r->eax = (unsigned)-EFAULT;
				break;
			}
			*(int *)arg = 1;
			r->eax = 0;
			break;
		case TIOCSPGRP:
			r->eax = 0;
			break;
		default:
			r->eax = (unsigned)-ENOTTY;
			break;
		}
		break;
	}
	case 60: {			/* umask(mask) */
		unsigned old = umask_val;

		umask_val = r->ebx;
		r->eax = old;
		break;
	}
	case 189: {			/* stat(path, sb) */
		char path[128];
		struct file *f;
		unsigned lba, size;
		int isdir;

		if (user_str(r->ebx, path, sizeof(path)) < 0 ||
		    !user_ok(r->ecx, STAT_SIZE)) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		if (iso_mounted() &&
		    iso_stat(path, &lba, &size, &isdir) == 0) {
			fill_stat(isdir ? 0040755 : 0100755, size,
			    (unsigned *)r->ecx);
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
		fill_stat(cur->fds[r->ebx].type == FD_DIR ? 0040755 : 0100755,
		    cur->fds[r->ebx].len, (unsigned *)r->ecx);
		r->eax = 0;
		break;
	}
	case 196: {			/* getdirentries(fd, buf, n, basep) */
		unsigned fd = r->ebx, buf = r->ecx, nb = r->edx, basep = r->esi;
		struct fd *fp;
		char name[48];
		unsigned elba, esize, pos;
		int eflags, rv, namlen, reclen;

		if (fd >= NFD || !cur->fds[fd].used ||
		    cur->fds[fd].type != FD_DIR) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		if (!user_ok(buf, nb) || nb < 16) {
			r->eax = (unsigned)-EINVAL;
			break;
		}
		fp = &cur->fds[fd];
		pos = fp->dpos;
		rv = iso_readdir(fp->lba, fp->dsize, &pos, name, &elba,
		    &esize, &eflags);
		if (rv < 0) {
			r->eax = (unsigned)-EIO;
			break;
		}
		if (basep != 0 && user_ok(basep, 4))
			*(unsigned *)basep = 0;
		if (rv > 0) {		/* end of directory */
			fp->dpos = pos;
			r->eax = 0;
			break;
		}
		namlen = strlen(name);
		reclen = (8 + namlen + 1 + 3) & ~3;
		if ((unsigned)reclen > nb) {
			r->eax = (unsigned)-EINVAL;
			break;
		}
		bzero((char *)buf, reclen);
		*(unsigned *)(buf + 0) = elba;		/* d_fileno */
		*(unsigned short *)(buf + 4) = reclen;	/* d_reclen */
		((char *)buf)[6] = (eflags & 2) ? 4 : 8;	/* d_type */
		((char *)buf)[7] = namlen;		/* d_namlen */
		bcopy(name, (char *)buf + 8, namlen + 1);
		fp->dpos = pos;
		r->eax = reclen;
		break;
	}
	case 199: {			/* lseek(fd, off, whence) */
		unsigned fd = r->ebx, off = r->ecx, wh = r->edx;

		if (fd >= NFD || !cur->fds[fd].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		if (cur->fds[fd].type == FD_DIR) {
			if (wh == 0)
				cur->fds[fd].dpos = off;
			else if (wh == 1)
				cur->fds[fd].dpos += off;
			else
				cur->fds[fd].dpos = cur->fds[fd].dsize;
			r->eax = cur->fds[fd].dpos;
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
	case 92: {			/* fcntl(fd, cmd, arg) */
		int fd = (int)r->ebx, cmd = (int)r->ecx, arg = (int)r->edx;

		if (fd < 0 || fd >= NFD || !cur->fds[fd].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		if (cmd == 0) {		/* F_DUPFD: lowest free fd >= arg */
			int newfd;

			for (newfd = arg; newfd < NFD; newfd++)
				if (!cur->fds[newfd].used)
					break;
			if (newfd >= NFD) {
				r->eax = (unsigned)-EMFILE;
				break;
			}
			cur->fds[newfd] = cur->fds[fd];
			if (cur->fds[newfd].type == FD_PIPE &&
			    cur->fds[newfd].pipeidx >= 0) {
				if (cur->fds[newfd].rw == 1)
					pipes[cur->fds[newfd].pipeidx].readers++;
				else
					pipes[cur->fds[newfd].pipeidx].writers++;
			}
			r->eax = newfd;
			break;
		}
		if (cmd >= 1 && cmd <= 4) {	/* F_GETFD/F_SETFD/F_GETFL/F_SETFL */
			r->eax = 0;
			break;
		}
		r->eax = (unsigned)-EINVAL;
		break;
	}
	case 41: {			/* dup(fd) */
		int from = (int)r->ebx, to;

		if (from < 0 || from >= NFD || !cur->fds[from].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		for (to = 3; to < NFD; to++)
			if (!cur->fds[to].used)
				break;
		if (to >= NFD) {
			r->eax = (unsigned)-EMFILE;
			break;
		}
		cur->fds[to] = cur->fds[from];
		if (cur->fds[to].type == FD_PIPE && cur->fds[to].pipeidx >= 0) {
			if (cur->fds[to].rw == 1)
				pipes[cur->fds[to].pipeidx].readers++;
			else
				pipes[cur->fds[to].pipeidx].writers++;
		}
		r->eax = to;
		break;
	}
	case 42: {			/* pipe(fds) */
		struct pipe *pp;
		unsigned fds[2];
		int i, nfd = 0;

		if (!user_ok(r->ebx, 2 * sizeof(int))) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		pp = pipe_alloc();
		if (pp == 0) {
			r->eax = (unsigned)-EMFILE;
			break;
		}
		pp->readers = 1;
		pp->writers = 1;
		for (i = 3; i < NFD && nfd < 2; i++) {
			if (!cur->fds[i].used) {
				fds[nfd++] = i;
				cur->fds[i].used = 1;
				cur->fds[i].type = FD_PIPE;
				cur->fds[i].pipeidx = pp - pipes;
				cur->fds[i].d = 0;
				cur->fds[i].len = 0;
				cur->fds[i].off = 0;
				cur->fds[i].lba = 0;
				cur->fds[i].dsize = 0;
				cur->fds[i].dpos = 0;
				cur->fds[i].rw = (nfd == 1) ? 1 : 2;
			}
		}
		if (nfd < 2) {
			pp->used = 0;
			r->eax = (unsigned)-EMFILE;
			break;
		}
		((int *)r->ebx)[0] = fds[0];
		((int *)r->ebx)[1] = fds[1];
		r->eax = 0;
		break;
	}
	case 90: {			/* dup2(from, to) */
		int from = (int)r->ebx, to = (int)r->ecx;

		if (from < 0 || from >= NFD || to < 0 || to >= NFD ||
		    !cur->fds[from].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		if (from == to) {
			r->eax = to;
			break;
		}
		fd_close(to);
		cur->fds[to] = cur->fds[from];
		if (cur->fds[to].type == FD_PIPE && cur->fds[to].pipeidx >= 0) {
			if (cur->fds[to].rw == 1)
				pipes[cur->fds[to].pipeidx].readers++;
			else
				pipes[cur->fds[to].pipeidx].writers++;
		}
		r->eax = to;
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
