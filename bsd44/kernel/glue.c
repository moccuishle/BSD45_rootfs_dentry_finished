/*
 * glue.c - minimal 4.4BSD-Lite2 kernel runtime for the i386 ELF port.
 *
 * The machine dependent core (locore.s, pmap.c, machdep.c, cons.c) is the
 * unmodified 4.4BSD-Lite2 source.  This file supplies only what a
 * stand-alone (no userland, no vfs) bring-up kernel needs:
 *	proc0 / curproc / proc0paddr
 *	kprintf / panic
 *	vm_set_page_size
 *	trap()
 *	main()
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/proc.h>
#include <sys/user.h>
#include <sys/reboot.h>

#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_kern.h>

#include <machine/cpu.h>
#include <machine/psl.h>
#include <machine/pmap.h>

/* GCC builtins: the mingw <stdarg.h> is only an include_next stub. */
typedef __builtin_va_list	va_list;
#define	va_start(ap, last)	__builtin_va_start(ap, last)
#define	va_arg(ap, type)	__builtin_va_arg(ap, type)
#define	va_end(ap)		__builtin_va_end(ap)

struct proc	proc0;
struct proc	*curproc = &proc0;
struct user	*proc0paddr;
struct vmmeter	cnt;

extern char	etext[], edata[], end[];

void	monitor(void);
int	pccom_present(void);
void	reboot_now(void);

/*
 * Called from pmap/machdep before page-size dependent code runs.
 */
void
vm_set_page_size(void)
{

	if (cnt.v_page_size == 0)
		cnt.v_page_size = DEFAULT_PAGE_SIZE;
	if (cnt.v_page_size != DEFAULT_PAGE_SIZE)
		panic("vm_set_page_size: page size mismatch");
	page_shift = 12;
	page_mask = (vm_size_t)cnt.v_page_size - 1;
}

/*
 * Formatting output.  Deliberately small: %c %s %d %u %x %o %p %b.
 */
static void
putnum(unsigned long v, int base, int width, int pad)
{
	char buf[32];
	int i = 0;

	if (v == 0)
		buf[i++] = '0';
	while (v) {
		int d = v % base;

		buf[i++] = d < 10 ? '0' + d : 'a' + d - 10;
		v /= base;
	}
	while (i < width--)
		buf[i++] = pad;
	while (i-- > 0)
		cnputc(buf[i]);
}

static void
vprintf(const char *fmt, va_list ap)
{
	const char *p;

	for (p = fmt; *p; p++) {
		int width = 0, pad = ' ', lflag = 0;

		if (*p != '%') {
			cnputc(*p);
			continue;
		}
		p++;
		if (*p == '-') { pad = ' '; p++; }
		if (*p == '0') { pad = '0'; p++; }
		while (*p >= '0' && *p <= '9')
			width = width * 10 + (*p++ - '0');
		while (*p == 'l') { lflag++; p++; }
		switch (*p) {
		case 'd': {
			long v = lflag ? va_arg(ap, long) : va_arg(ap, int);

			if (v < 0) { cnputc('-'); v = -v; }
			putnum(v, 10, width, pad);
			break;
		}
		case 'u':
			putnum(lflag ? va_arg(ap, unsigned long) :
			    va_arg(ap, unsigned int), 10, width, pad);
			break;
		case 'x':
			putnum(lflag ? va_arg(ap, unsigned long) :
			    va_arg(ap, unsigned int), 16, width, pad);
			break;
		case 'o':
			putnum(lflag ? va_arg(ap, unsigned long) :
			    va_arg(ap, unsigned int), 8, width, pad);
			break;
		case 'p':
			cnputc('0'); cnputc('x');
			putnum((unsigned long)va_arg(ap, void *), 16, 8, '0');
			break;
		case 'c':
			cnputc(va_arg(ap, int));
			break;
		case 's': {
			char *s = va_arg(ap, char *);

			if (s == NULL)
				s = "(null)";
			while (*s)
				cnputc(*s++);
			break;
		}
		case 'b': {
			int v = va_arg(ap, int);

			putnum(v, 2, width, pad);
			break;
		}
		case '%':
			cnputc('%');
			break;
		default:
			cnputc('%');
			cnputc(*p);
			break;
		}
	}
}

void
printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

void
kprintf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

void
panic(const char *fmt, ...)
{
	va_list ap;

	printf("\npanic: ");
	va_start(ap, fmt);
	/* simple: panic strings are plain */
	printf("%s", fmt);
	va_end(ap);
	printf("\n");
	for (;;)
		;
}

/*
 * Trap entry.  locore pushes a trapframe and calls trap().  Without the
 * VM fault machinery we report and stop rather than corrupt state.
 */
void
trap(struct trapframe *frame)
{
	static const char *trapname[] = {
		"divide", "debug", "nmi", "breakpoint", "overflow", "bounds",
		"invalid opcode", "no coprocessor", "double fault",
		"coprocessor overrun", "invalid tss", "segment not present",
		"stack fault", "general protection", "page fault", "reserved",
	};

	printf("\n\n*** TRAP %d (%s) err=%x\n", frame->tf_trapno,
	    frame->tf_trapno < 16 ? trapname[frame->tf_trapno] : "unknown",
	    frame->tf_err);	printf("eip=%x cs=%x eflags=%x esp=%x ss=%x\n",
	    frame->tf_eip, frame->tf_cs, frame->tf_eflags,
	    frame->tf_esp, frame->tf_ss);
	if ((frame->tf_cs & 3) != 0) {
		const unsigned char *p =
		    (const unsigned char *)(frame->tf_eip - 8);
		int i;

		printf("code:");
		for (i = 0; i < 16; i++)
			printf(" %x", p[i]);
		printf("\n");
	}
	printf("eax=%x ebx=%x ecx=%x edx=%x\n",
	    frame->tf_eax, frame->tf_ebx, frame->tf_ecx, frame->tf_edx);
	printf("esi=%x edi=%x ebp=%x ds=%x es=%x\n",
	    frame->tf_esi, frame->tf_edi, frame->tf_ebp,
	    frame->tf_ds, frame->tf_es);
	printf("cr2=%x\n", rcr2());
	printf("system halted.\n");
	for (;;)
		;
}

int
syscall(void)
{

	printf("syscall: not implemented\n");
	return (0);
}

/*
 * The 4.4BSD kernel proper enters userland here.  We have no userland:
 * hand control to the on-board monitor instead.
 */
void
main(vm_offset_t firstaddr)
{
	extern vm_offset_t avail_start, avail_end, virtual_avail, virtual_end;
	extern int IdlePTD;
	extern int Maxmem, physmem;

	printf("\n");
	printf("4.4BSD-Lite2 (i386) ELF kernel\n");
	printf("Kernel: ELF32 i386, KERNBASE %x, first free page %x\n",
	    KERNBASE, firstaddr);
	printf("Memory: maxmem %x, physmem %x, avail %x-%x\n",
	    ctob(Maxmem), ctob(physmem), avail_start, avail_end);
	printf("pmap: kernel_pmap %p pdir %p IdlePTD %x\n",
	    kernel_pmap, kernel_pmap->pm_pdir, IdlePTD);
	printf("VM: virtual_avail %x virtual_end %x\n",
	    virtual_avail, virtual_end);
	printf("Console: VGA 80x25, COM1 %s\n",
	    pccom_present() ? "present" : "absent");
	printf("\n");

	monitor();
}
