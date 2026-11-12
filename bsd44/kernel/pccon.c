/*
 * pccon.c - PC console (VGA text + COM1 mirror) for the 4.4BSD-Lite2
 *	     i386 ELF kernel.
 *
 * This is a replacement for the tty-bound pccons.c of 4.4BSD.  It plugs
 * into the genuine i386/i386/cons.c console layer through struct consdev,
 * so cninit()/cnputc()/cngetc() are the unmodified 4.4BSD code.
 *
 * Crtat is also consumed by locore.s before init386() runs; its initial
 * value must be the virtual address of a text buffer (KERNBASE + 0xB8000).
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/tty.h>

#include <i386/i386/cons.h>

#define	COM1		0x3f8
#define	COM1_LSR	(COM1 + 5)

#define	VGA_COLOR	((u_short *)0xFE0B8000)
#define	VGA_MONO	((u_short *)0xFE0B0000)

#define	COL		80
#define	ROW		25
#define	ATTR		0x07

u_short *Crtat = VGA_COLOR;

static int	crt_row, crt_col;
static u_short *crt_base;

static int	com_present;	/* COM1 really exists (not an unassigned port) */

static void	vga_cursor(void);

/*
 * Probe for a 16550-style UART.  On firmware that does not map COM1 (for
 * example a VMware VM with no serial port configured) reads to 0x3f8..
 * return 0xff, which would otherwise look like "LSR: data ready" and make
 * cngetc() return 0xff forever, so the keyboard was never polled.
 */
static int
com_probe(void)
{
	int lsr = inb(COM1_LSR);

	if (lsr == 0xff)
		return (0);
	/* THRE and TEMT must both be set, bit 7 (a 16550 has 0 there) clear */
	if ((lsr & 0xe0) != 0x60)
		return (0);
	return (1);
}

int
pccom_present(void)
{

	return (com_present);
}

static void
kbd_init(void)
{
	int i;

	/* drain whatever the BIOS left in the 8042 output buffer */
	for (i = 0; i < 32 && (inb(0x64) & 0x01); i++)
		(void)inb(0x60);
	/* wait for the input buffer to drain, then enable the keyboard */
	for (i = 0; i < 100000 && (inb(0x64) & 0x02); i++)
		;
	outb(0x64, 0xae);
}

static int
com_ready(void)
{
	int i;

	if (!com_present)
		return (0);
	for (i = 0; i < 100000; i++)
		if (inb(COM1_LSR) & 0x20)
			return (1);
	return (0);
}

static void
com_putc(int c)
{
	if (com_ready())
		outb(COM1, c);
}

static void
com_init(void)
{
	outb(COM1 + 1, 0x00);	/* no interrupts */
	outb(COM1 + 3, 0x80);	/* DLAB */
	outb(COM1 + 0, 0x01);	/* divisor 1 = 115200 */
	outb(COM1 + 1, 0x00);
	outb(COM1 + 3, 0x03);	/* 8N1 */
	outb(COM1 + 2, 0xc7);	/* FIFO on */
	outb(COM1 + 4, 0x03);	/* DTR|RTS */
}

int
pccnprobe(struct consdev *cp)
{

	/*
	 * Crtat has already been relocated by locore.s to the I/O window
	 * (atdevbase + 0x18000), which maps physical 0xB8000.  Do not
	 * overwrite it: with a sub-1MB kernel the raw 0xFE0B8000 address
	 * is not identity mapped.
	 */
	crt_base = Crtat;
	cp->cn_pri = CN_INTERNAL;
	return (0);
}

int
pccninit(struct consdev *cp)
{
	int i;

	crt_base = Crtat;
	for (i = 0; i < COL * ROW; i++)
		crt_base[i] = (ATTR << 8) | ' ';
	crt_row = crt_col = 0;
	vga_cursor();
	com_present = com_probe();
	if (com_present)
		com_init();
	kbd_init();
	return (0);
}

int
pccnputc(dev_t dev, int c)
{

	com_putc(c);
	if (c == '\r') {
		crt_col = 0;
		vga_cursor();
		return (0);
	}
	if (c == '\n') {
		crt_col = 0;
		crt_row++;
	} else if (c == '\b') {
		if (crt_col > 0)
			crt_col--;
		else if (crt_row > 0) {
			crt_row--;
			crt_col = COL - 1;
		}
		crt_base[crt_row * COL + crt_col] = (ATTR << 8) | ' ';
	} else if (c == '\t') {
		crt_col = (crt_col + 8) & ~7;
	} else {
		crt_base[crt_row * COL + crt_col] = (ATTR << 8) | (c & 0xff);
		crt_col++;
	}
	if (crt_col >= COL) {
		crt_col = 0;
		crt_row++;
	}
	if (crt_row >= ROW) {
		int i;

		for (i = 0; i < COL * (ROW - 1); i++)
			crt_base[i] = crt_base[i + COL];
		for (i = COL * (ROW - 1); i < COL * ROW; i++)
			crt_base[i] = (ATTR << 8) | ' ';
		crt_row = ROW - 1;
	}
	vga_cursor();
	return (0);
}

static void
vga_cursor(void)
{
	unsigned pos = crt_row * COL + crt_col;

	outb(0x3d4, 0x0f);
	outb(0x3d5, pos & 0xff);
	outb(0x3d4, 0x0e);
	outb(0x3d5, (pos >> 8) & 0xff);
}

/*
 * Scan code set 1 -> ASCII, unshifted / shifted.
 */
static const char scancode_map[128] = {
	0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
	'\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
	0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
	0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
	'*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static const char scancode_shift[128] = {
	0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
	'\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
	0, 'A','S','D','F','G','H','J','K','L',':','"','~',
	0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
	'*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static int shift_down;
static int ext_pending;

int
pccngetc(dev_t dev)
{
	unsigned char sc;
	static const char *map;
	int c;

	if (com_present && (inb(COM1_LSR) & 0x01))
		return (inb(COM1));

	if ((inb(0x64) & 0x01) == 0)
		return (0);

	sc = inb(0x60);
	if (sc == 0xe0) {
		ext_pending = 1;
		return (0);
	}
	if (ext_pending) {
		ext_pending = 0;
		return (0);
	}
	if (sc == 0x2a || sc == 0x36) {
		shift_down = 1;
		return (0);
	}
	if (sc == 0xaa || sc == 0xb6) {
		shift_down = 0;
		return (0);
	}
	if (sc & 0x80)
		return (0);
	map = shift_down ? scancode_shift : scancode_map;
	c = (unsigned char)map[sc];
	return (c);
}
