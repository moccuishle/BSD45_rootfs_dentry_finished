/*
 * histedit_stub.c - no line editing on BSD45-ELF-x86 (no tty ioctls).
 *
 * The 4.4BSD sh always links against libedit.  This kernel has no termios
 * and no terminal, so provide the handful of entry points sh references
 * and keep `el' NULL: input.c then falls back to plain read(2).
 */
#include <histedit.h>

EditLine *el;
History *hist;
int displayhist;

void
histedit(void)
{
}

void
sethistsize(void)
{
}

int
histcmd(int argc, char **argv)
{
	return (0);
}

int
not_fcnumber(char *s)
{
	return (0);
}

int
str_to_event(char *str, int last)
{
	return (0);
}

__const char *
el_gets(EditLine *e, int *n)
{

	*n = 0;
	return (NULL);
}

__const HistEvent *
history(History *h, int op, ...)
{

	return (NULL);
}
