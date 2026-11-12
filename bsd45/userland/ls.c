/*
 * ls(1) - a small directory lister for the BSD45 userland.
 *
 * It exists to exercise the S7 directory interface: opendir/readdir/closedir
 * over the kernel's getdirentries(), plus stat() for the arguments.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static int
list(const char *path, int show_head)
{
	DIR *d;
	struct dirent *de;
	struct stat st;

	d = opendir(path);
	if (d == 0) {
		if (stat(path, &st) == 0) {
			printf("%s\n", path);
			return (0);
		}
		fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
		return (1);
	}
	if (show_head)
		printf("%s:\n", path);
	while ((de = readdir(d)) != 0)
		printf("%s\n", de->d_name);
	closedir(d);
	return (0);
}

int
main(int argc, char **argv)
{
	int i, rc = 0;

	if (argc < 2)
		return (list(".", 0));
	for (i = 1; i < argc; i++)
		rc |= list(argv[i], argc > 2);
	return (rc);
}
