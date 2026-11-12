/* echo(1) - the first external command for BSD45-ELF-x86. */
#include <stdio.h>
#include <string.h>

int
main(int argc, char **argv)
{
	int i, nflag = 0;

	if (argc > 1 && strcmp(argv[1], "-n") == 0) {
		nflag = 1;
		argv++;
		argc--;
	}
	for (i = 1; i < argc; i++) {
		if (i > 1)
			putchar(' ');
		fputs(argv[i], stdout);
	}
	if (!nflag)
		putchar('\n');
	fflush(stdout);
	return (0);
}
