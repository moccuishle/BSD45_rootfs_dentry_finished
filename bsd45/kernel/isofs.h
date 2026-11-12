/*
 * isofs.h - read-only ISO9660 root filesystem.
 */
#ifndef _ISOF_H_
#define _ISOF_H_

int	iso_mount(void);
int	iso_mounted(void);
int	iso_lookup(const char *path, unsigned *lba, unsigned *size);
int	iso_read_file(const char *path, void *buf, unsigned max,
	    unsigned *lenp);
unsigned iso_root(void);
unsigned iso_root_size(void);

#endif /* _ISOF_H_ */

/* directory support */
int	iso_stat(const char *path, unsigned *lba, unsigned *size, int *isdir);
int	iso_readdir(unsigned lba, unsigned size, unsigned *posp, char *name,
	    unsigned *elba, unsigned *esize, int *eflags);
