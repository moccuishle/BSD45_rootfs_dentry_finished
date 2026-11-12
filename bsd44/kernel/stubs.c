/*
 * stubs.c - minimal kernel services that the stand-alone bring-up
 * kernel needs in order to link the genuine 4.4BSD machine-dependent
 * code (locore.s, pmap.c, machdep.c, cons.c, icu.s).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/proc.h>
#include <sys/malloc.h>

#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_kern.h>

#include <machine/cpu.h>
#include <machine/pmap.h>

int	bootdev;
int	page_shift = 12;
vm_size_t page_mask = 0xfff;
boolean_t vm_page_startup_initialized;

int	astpending;
int	want_resched;
int	whichqs;
int	netisr;

/*
 * pmap.c's debug printf macro (PMAP_DEBUG is off in a normal build).
 */
int
pg(const char *fmt, ...)
{

	return (0);
}

/*
 * Classify interrupts / timer callbacks.  The bring-up kernel keeps the
 * PIC masked and polls, so these are never entered.
 */
int
rawintr(int unit)
{

	return (0);
}

void
isa_strayintr(int d)
{

	printf("stray ISA interrupt %d\n", d);
}

void
softclock(void)
{
}

/*
 * kernel malloc: a bump allocator over a static arena.  Only pmap's pv
 * entry allocation uses it, and the monitor never grows it without bound.
 */
#define	HEAP_SIZE	(256 * 1024)
static char	heap[HEAP_SIZE] __attribute__((aligned(16)));
static unsigned	heap_used;

void *
malloc(unsigned long size, int type, int flags)
{
	void *p;

	size = (size + 15) & ~15;
	if (heap_used + size > HEAP_SIZE)
		panic("malloc: out of kernel heap");
	p = heap + heap_used;
	heap_used += size;
	return (p);
}

void
free(void *addr, int type)
{
	/* bump allocator: no-op */
}

void
blkclr(void *p, unsigned int len)
{

	bzero(p, len);
}

/* mingw/gcc inserts a call to __main() at the start of main(). */
void
__main(void)
{
}
