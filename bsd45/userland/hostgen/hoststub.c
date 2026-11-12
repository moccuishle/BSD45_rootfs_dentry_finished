/*
 * hoststub.c - host-side helpers missing from the mingw CRT.
 *
 * mkinit(8) renames init.c.new to init.c with link()+unlink().  mingw has
 * no link(2), so emulate it with a copy.
 */
#include <stdio.h>

int
link(const char *from, const char *to)
{
	FILE *in, *out;
	char buf[4096];
	size_t n;

	if ((in = fopen(from, "rb")) == NULL)
		return (-1);
	if ((out = fopen(to, "wb")) == NULL) {
		fclose(in);
		return (-1);
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(in);
	fclose(out);
	return (0);
}
