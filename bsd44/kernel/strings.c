/*
 * strings.c - the few libkern primitives not already in locore.s.
 * (locore.s supplies bcopy, bzero and blkclr.)
 */

#include <sys/param.h>
#include <sys/types.h>

int
strncmp(const char *s1, const char *s2, unsigned int n)
{

	while (n-- > 0) {
		if (*s1 != *s2)
			return ((unsigned char)*s1 - (unsigned char)*s2);
		if (*s1 == 0)
			break;
		s1++;
		s2++;
	}
	return (0);
}

void *
memcpy(void *dst, const void *src, unsigned int n)
{

	bcopy(src, dst, n);
	return (dst);
}

void *
memset(void *dst, int c, unsigned int n)
{
	unsigned char *p = dst;

	while (n-- > 0)
		*p++ = (unsigned char)c;
	return (dst);
}

void *
memmove(void *dst, const void *src, unsigned int n)
{

	bcopy(src, dst, n);
	return (dst);
}
