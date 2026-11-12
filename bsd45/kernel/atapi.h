/*
 * atapi.h - minimal ATAPI CD-ROM interface (PIO).
 */
#ifndef _ATAPI_H_
#define _ATAPI_H_

int	atapi_probe(void);
int	atapi_present(void);
int	atapi_read(unsigned lba, unsigned nsec, void *buf);
int	atapi_inquiry(char *vendor, char *product);
int	atapi_capacity(unsigned *lastlba);

#endif /* _ATAPI_H_ */
