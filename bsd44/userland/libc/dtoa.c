/*
 * dtoa.c - minimal __dtoa for the 4.4BSD vfprintf port.
 *
 * 4.4BSD's vfprintf calls __dtoa() to print floating point values.  sh
 * never prints floats, but stdio must still link, so provide a simple
 * fixed/engineering converter instead of dragging in David Gay's gdtoa.
 */

#include <sys/types.h>
#include <math.h>

#define	NI	40

char *
__dtoa(double d, int mode, int ndigits, int *decpt, int *sign, char **rve)
{
	static char buf[NI + 1];
	char *p;
	int n, i;

	if (d < 0) {
		*sign = 1;
		d = -d;
	} else
		*sign = 0;

	if (ndigits < 0)
		ndigits = 0;
	if (ndigits > NI - 2)
		ndigits = NI - 2;

	if (d == 0.0) {
		*decpt = 1;
		buf[0] = '0';
		buf[1] = 0;
		if (rve)
			*rve = buf + 1;
		return (buf);
	}

	n = 0;
	while (d >= 10.0 && n < 309) {
		d /= 10.0;
		n++;
	}
	while (d < 1.0 && n > -323) {
		d *= 10.0;
		n--;
	}
	*decpt = n + 1;

	p = buf;
	for (i = 0; i < ndigits + 1; i++) {
		int dig;

		if (d > 1e300)
			break;
		dig = (int)d;
		if (dig < 0)
			dig = 0;
		if (dig > 9)
			dig = 9;
		*p++ = '0' + dig;
		d = (d - dig) * 10.0;
	}
	if (p == buf)
		*p++ = '0';
	*p = 0;
	if (rve)
		*rve = p;
	if (ndigits > 0 && (int)(p - buf) > ndigits)
		*(buf + ndigits) = 0;
	return (buf);
}
