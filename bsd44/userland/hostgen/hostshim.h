/*
 * hostshim.h - makes the 4.4BSD sh host tools build with a modern
 * mingw/UCRT toolchain.  Forced in with -include so it lands before the
 * tool sources are parsed.
 */
#ifndef _HOSTSHIM_H_
#define _HOSTSHIM_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __P
#define	__P(args)	args
#endif

#ifndef _HOSTSHIM_LINK
#define	_HOSTSHIM_LINK
int link(const char *, const char *);
#endif

#endif /* _HOSTSHIM_H_ */
