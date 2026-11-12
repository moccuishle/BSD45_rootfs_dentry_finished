/*
 * isofs.c - read-only ISO9660 root filesystem on the ATAPI CD-ROM.
 *
 * The kernel only needs what execve() does: mount the primary volume,
 * resolve a path to (LBA, size) and read the file.  Directory records are
 * walked sector by sector (ISO9660 never lets a record cross a sector).
 */

#include <sys/param.h>
#include <sys/systm.h>

#include "atapi.h"
#include "isofs.h"

#define	ISO_SECTOR	2048
#define	ISO_ROOT_REC	156	/* offset of the root directory record in the PVD */

static unsigned char	secbuf[ISO_SECTOR];
static unsigned		root_lba, root_size;
static int		mounted;

static unsigned
le32(const unsigned char *p)
{

	return (p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

/*
 * ISO9660 names are upper case; the kernel's exec paths are not, and files
 * are shipped as NAME.ELF so that mkisofs -iso-level 1 (8.3) accepts them.
 */
static void
iso_name(char *dst, const char *src, int n, int isfile)
{
	int i, dot = 0;

	for (i = 0; i < n && i < 40; i++) {
		char c = src[i];

		if (c == '.')
			dot = 1;
		if (c >= 'a' && c <= 'z')
			c = c - 'a' + 'A';
		dst[i] = c;
	}
	if (isfile && !dot) {
		dst[i++] = '.';
		dst[i++] = 'E';
		dst[i++] = 'L';
		dst[i++] = 'F';
	}
	dst[i] = 0;
}

static int
name_eq(const unsigned char *nm, int nlen, const char *want)
{
	int i = 0;

	while (i < nlen && nm[i] != ';') {
		if (want[i] == 0 || nm[i] != (unsigned char)want[i])
			return (0);
		i++;
	}
	return (want[i] == 0);
}

/*
 * Find `want' in the directory at (lba, size); fill in the record fields.
 */
static int
find_in_dir(unsigned lba, unsigned size, const char *want, unsigned *rlba,
    unsigned *rsize, int *rflags)
{
	unsigned off, pos;

	for (off = 0; off < size; off += ISO_SECTOR) {
		if (atapi_read(lba + off / ISO_SECTOR, 1, secbuf) < 0)
			return (-1);
		pos = 0;
		while (pos + 33 < ISO_SECTOR) {
			int len = secbuf[pos];
			int nlen;

			if (len == 0)
				break;		/* rest of the sector is padding */
			if (pos + len > ISO_SECTOR)
				break;
			nlen = secbuf[pos + 32];
			if (nlen > 0 && pos + 33 + nlen <= ISO_SECTOR &&
			    name_eq(secbuf + pos + 33, nlen, want)) {
				*rlba = le32(secbuf + pos + 2);
				*rsize = le32(secbuf + pos + 10);
				*rflags = secbuf[pos + 25];
				return (0);
			}
			pos += len;
		}
	}
	return (-1);
}

int
iso_mount(void)
{

	if (!atapi_present())
		return (-1);
	if (atapi_read(16, 1, secbuf) < 0) {
		printf("isofs: sector 16 read failed\n");
		return (-1);
	}

	if (secbuf[0] != 1 || bcmp(secbuf + 1, "CD001", 5) != 0) {
		printf("isofs: not a primary volume descriptor\n");
		return (-1);
	}
	root_lba = le32(secbuf + ISO_ROOT_REC + 2);
	root_size = le32(secbuf + ISO_ROOT_REC + 10);
	if (root_lba == 0 || root_size == 0)
		return (-1);
	mounted = 1;
	return (0);
}

int
iso_mounted(void)
{

	return (mounted);
}

unsigned
iso_root(void)
{

	return (root_lba);
}

unsigned
iso_root_size(void)
{

	return (root_size);
}

int
iso_stat(const char *path, unsigned *lba, unsigned *size, int *isdir)
{
	char name[64];
	const char *p = path;
	unsigned cur_lba = root_lba, cur_size = root_size;
	int flags, isfile;

	if (!mounted)
		return (-1);
	while (*p == '/')
		p++;
	if (*p == 0) {			/* the root directory itself */
		*lba = root_lba;
		*size = root_size;
		*isdir = 1;
		return (0);
	}

	for (;;) {
		const char *slash = p;
		int n;

		while (*slash && *slash != '/')
			slash++;
		if (slash == p)
			return (-1);
		n = slash - p;
		if (n > 40)
			return (-1);
		isfile = (*slash == 0);
		iso_name(name, p, n, isfile);
		if (find_in_dir(cur_lba, cur_size, name, lba, size, &flags) < 0) {
			/*
			 * The last component may be a directory: iso_name()
			 * appends ".ELF" to dotless names for exec paths, so
			 * retry with the bare name before giving up.
			 */
			if (!isfile)
				return (-1);
			iso_name(name, p, n, 0);
			if (find_in_dir(cur_lba, cur_size, name, lba,
			    size, &flags) < 0)
				return (-1);
			isfile = 0;
		}
		if (isfile) {
			*isdir = 0;
			return ((flags & 2) ? -1 : 0);
		}
		if ((flags & 2) == 0)		/* not a directory */
			return (-1);
		if (*slash == 0) {		/* the path ends at a directory */
			*isdir = 1;
			return (0);
		}
		cur_lba = *lba;
		cur_size = *size;
		p = slash;
		while (*p == '/')
			p++;
	}
}

int
iso_lookup(const char *path, unsigned *lba, unsigned *size)
{
	int isdir;

	if (iso_stat(path, lba, size, &isdir) < 0 || isdir)
		return (-1);
	return (0);
}

/*
 * Return the next directory entry at (lba, size), starting at *posp.
 * Names come back lower case with the ";1" version suffix and any trailing
 * dot removed.  Returns 0 on success, 1 at end of directory, -1 on error.
 */
int
iso_readdir(unsigned lba, unsigned size, unsigned *posp, char *name,
    unsigned *elba, unsigned *esize, int *eflags)
{
	unsigned pos = *posp;
	unsigned char *rec;
	int len, nlen, i, j;

	while (pos < size) {
		if (atapi_read(lba + pos / ISO_SECTOR, 1, secbuf) < 0)
			return (-1);
		rec = secbuf + (pos % ISO_SECTOR);
		len = rec[0];
		if (len == 0) {			/* padding to the next sector */
			pos = (pos / ISO_SECTOR + 1) * ISO_SECTOR;
			continue;
		}
		if ((pos % ISO_SECTOR) + len > ISO_SECTOR) {
			pos = (pos / ISO_SECTOR + 1) * ISO_SECTOR;
			continue;
		}
		nlen = rec[32];
		if (nlen == 1 && (rec[33] == 0 || rec[33] == 1)) {
			pos += len;		/* "." and ".." */
			continue;
		}
		if (nlen <= 0 || nlen > 48) {
			pos += len;
			continue;
		}
		/* copy, lower case, stop at ';' */
		for (i = 0, j = 0; i < nlen && j < 40; i++) {
			char c = rec[33 + i];

			if (c == ';')
				break;
			if (c >= 'A' && c <= 'Z')
				c = c - 'A' + 'a';
			name[j++] = c;
		}
		while (j > 0 && name[j - 1] == '.')
			j--;
		name[j] = 0;
		*elba = le32(rec + 2);
		*esize = le32(rec + 10);
		*eflags = rec[25];
		*posp = pos + len;
		return (0);
	}
	return (1);
}

int
iso_read_file(const char *path, void *buf, unsigned max, unsigned *lenp)
{
	unsigned lba, size, nsec;

	if (iso_lookup(path, &lba, &size) < 0)
		return (-1);
	if (size > max)
		return (-1);
	nsec = (size + ISO_SECTOR - 1) / ISO_SECTOR;
	if (nsec == 0)
		return (-1);
	if (atapi_read(lba, nsec, buf) < 0)
		return (-1);
	*lenp = size;
	return (0);
}
