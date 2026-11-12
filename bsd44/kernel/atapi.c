/*
 * atapi.c - minimal ATAPI (PIO) CD-ROM driver for the ISO9660 root.
 *
 * The kernel has no device framework yet, so this is deliberately small:
 * probe both IDE channels for an ATAPI packet device, then run SCSI
 * commands with PIO.  Everything polls with interrupts off.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>

#include "atapi.h"

#define	PRIMARY_IO	0x1f0
#define	PRIMARY_CTL	0x3f6
#define	SECONDARY_IO	0x170
#define	SECONDARY_CTL	0x376

#define	ATA_DATA	0
#define	ATA_ERROR	1
#define	ATA_FEATURES	1
#define	ATA_SECCOUNT	2
#define	ATA_LBA0	3
#define	ATA_LBA1	4
#define	ATA_LBA2	5
#define	ATA_DRIVE	6
#define	ATA_STATUS	7
#define	ATA_COMMAND	7

#define	ST_BSY		0x80
#define	ST_DRDY		0x40
#define	ST_DRQ		0x08
#define	ST_ERR		0x01

#define	CMD_PACKET	0xa0
#define	CMD_IDENTIFY_PACKET 0xa1

static int	atapi_io;	/* I/O base of the channel in use */
static int	atapi_ctl;
static int	atapi_slave;	/* 0 = master, 1 = slave */
static int	atapi_unit = -1;

static void
udelay(void)
{
	int i;

	for (i = 0; i < 4; i++)
		(void)inb(atapi_io + ATA_STATUS);
}

static int
wait_status(int mask, int value, int polls)
{
	int i, st;

	for (i = 0; i < polls; i++) {
		st = inb(atapi_io + ATA_STATUS);
		if ((st & mask) == value)
			return (0);
	}
	return (-1);
}

static void
insw(int port, void *addr, unsigned count)
{

	__asm__ volatile("cld\n\trep\n\tinsw"
	    : "+D"(addr), "+c"(count)
	    : "d"(port)
	    : "memory");
}

static void
ata_outw(int port, unsigned value)
{

	__asm__ volatile("outw %w0, %w1"
	    :: "a"((unsigned short)value), "d"((unsigned short)port));
}

static int
probe_one(int io, int ctl, int slave)
{
	unsigned short id[256];
	int st, word0;

	atapi_io = io;
	atapi_ctl = ctl;
	atapi_slave = slave;

	outb(io + ATA_DRIVE, 0xa0 | (slave << 4));
	udelay();
	outb(io + ATA_SECCOUNT, 0);
	outb(io + ATA_LBA0, 0);
	outb(io + ATA_LBA1, 0);
	outb(io + ATA_LBA2, 0);
	outb(io + ATA_COMMAND, CMD_IDENTIFY_PACKET);
	udelay();

	st = inb(io + ATA_STATUS);
	if (st == 0 || st == 0xff)		/* empty slot */
		return (-1);
	if (wait_status(ST_BSY, 0, 200000) < 0)
		return (-1);
	if (inb(io + ATA_STATUS) & ST_ERR)
		return (-1);			/* ATA device, not ATAPI */
	if (wait_status(ST_DRQ, ST_DRQ, 200000) < 0)
		return (-1);
	insw(io + ATA_DATA, id, 256);
	(void)wait_status(ST_BSY, 0, 200000);
	word0 = id[0];
	if ((word0 & 0x8000) == 0 || ((word0 >> 8) & 0x1f) != 5)
		return (-1);			/* not a CD-ROM */
	return (0);
}

/*
 * Run one ATAPI packet command.  `bytes' is the size of the data-in phase
 * (0 for commands without data).
 */
static int
atapi_packet(const unsigned char *cdb, void *buf, unsigned bytes)
{
	unsigned i, words;
	int st;

	outb(atapi_io + ATA_DRIVE, 0xa0 | (atapi_slave << 4));
	udelay();
	if (wait_status(ST_BSY, 0, 200000) < 0)
		return (-1);
	outb(atapi_io + ATA_FEATURES, 0);
	outb(atapi_io + ATA_SECCOUNT, 0);
	outb(atapi_io + ATA_LBA0, 0);
	/* byte count limit (little endian) for the data-in phase */
	outb(atapi_io + ATA_LBA1, bytes & 0xff);
	outb(atapi_io + ATA_LBA2, (bytes >> 8) & 0xff);
	outb(atapi_io + ATA_COMMAND, CMD_PACKET);
	udelay();
	if (wait_status(ST_BSY | ST_DRQ, ST_DRQ, 200000) < 0)
		return (-1);

	/* 12-byte CDB => 6 words, low byte first */
	{
		unsigned short w[6];

		for (i = 0; i < 6; i++)
			w[i] = cdb[i * 2] | (cdb[i * 2 + 1] << 8);
		outsw(atapi_io + ATA_DATA, w, 6);
	}
	udelay();

	if (wait_status(ST_BSY, 0, 2000000) < 0)
		return (-1);
	st = inb(atapi_io + ATA_STATUS);
	if (st & ST_ERR)
		return (-1);
	if (bytes == 0)
		return (0);
	if ((st & ST_DRQ) == 0)
		return (-1);

	words = bytes / 2;
	insw(atapi_io + ATA_DATA, buf, words);
	(void)wait_status(ST_BSY, 0, 2000000);
	return (0);
}

int
atapi_probe(void)
{

	if (probe_one(PRIMARY_IO, PRIMARY_CTL, 0) == 0 ||
	    probe_one(PRIMARY_IO, PRIMARY_CTL, 1) == 0 ||
	    probe_one(SECONDARY_IO, SECONDARY_CTL, 0) == 0 ||
	    probe_one(SECONDARY_IO, SECONDARY_CTL, 1) == 0) {
		atapi_unit = (atapi_io << 4) | atapi_slave;
		return (0);
	}
	return (-1);
}

int
atapi_present(void)
{

	return (atapi_unit >= 0);
}

int
atapi_read(unsigned lba, unsigned nsec, void *buf)
{
	unsigned char cdb[12];
	unsigned char *bp = buf;
	unsigned n;
	unsigned got = 0;
	unsigned one = 1;

	if (atapi_unit < 0 || nsec == 0)
		return (-1);

	/*
	 * Some emulations are happier with one sector per READ(10); the CD
	 * is slow anyway and this keeps the PIO loop trivial.
	 */
	for (n = 0; n < nsec; n++) {
		bzero(cdb, sizeof(cdb));
		cdb[0] = 0xa8;			/* READ(12) */
		cdb[2] = (lba >> 24) & 0xff;
		cdb[3] = (lba >> 16) & 0xff;
		cdb[4] = (lba >> 8) & 0xff;
		cdb[5] = lba & 0xff;
		cdb[6] = 0;
		cdb[7] = 0;
		cdb[8] = 0;
		cdb[9] = one & 0xff;
		if (atapi_packet(cdb, bp, 2048) < 0)
			return (-1);
		bp += 2048;
		lba++;
		got++;
		if (got == nsec)
			break;
	}
	return (0);
}

int
atapi_capacity(unsigned *lastlba)
{
	unsigned char cdb[12];
	unsigned char data[8];

	bzero(cdb, sizeof(cdb));
	cdb[0] = 0x25;			/* READ CAPACITY */
	if (atapi_packet(cdb, data, sizeof(data)) < 0)
		return (-1);
	*lastlba = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
	return (0);
}

int
atapi_inquiry(char *vendor, char *product)
{
	unsigned char cdb[12];
	unsigned char data[36];
	int i;

	bzero(cdb, sizeof(cdb));
	cdb[0] = 0x12;			/* INQUIRY */
	cdb[4] = sizeof(data);
	if (atapi_packet(cdb, data, sizeof(data)) < 0)
		return (-1);
	for (i = 0; i < 8; i++)
		vendor[i] = data[8 + i] ? data[8 + i] : ' ';
	vendor[8] = 0;
	for (i = 0; i < 16; i++)
		product[i] = data[16 + i] ? data[16 + i] : ' ';
	product[16] = 0;
	return (0);
}
