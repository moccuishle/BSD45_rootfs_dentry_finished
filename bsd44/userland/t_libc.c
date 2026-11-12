/* t_libc - smoke test for the 4.4BSD libc subset (printf/malloc/string). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
	write(1, "A\n", 2);
	printf("B\n");
	write(1, "C\n", 3);
	return (0);
}
