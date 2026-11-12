/*
 * cat(1) - copy stdin (or the named files) to stdout.
 *
 * Small on purpose: it is the reader at the end of a pipeline, so it
 * exercises the kernel's pipe read path.
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

static int
copy(int fd)
{
	char buf[512];
	int n;

	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		if (write(1, buf, n) != n)
			return (1);
	}
	return (n < 0);
}

int
main(int argc, char **argv)
{
	int i, rc = 0, fd;

	if (argc < 2)
		return (copy(0));
	for (i = 1; i < argc; i++) {
		fd = open(argv[i], O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "cat: %s: cannot open\n", argv[i]);
			rc = 1;
			continue;
		}
		rc |= copy(fd);
		close(fd);
	}
	return (rc);
}
