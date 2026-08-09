/*
 * Drive sh's spelling corrector directly, because the suite cannot reach it.
 *
 * spname() runs from xec.c:712, on a `cd' that failed, and only when
 * flags&ttyflg -- so `sh -c' can never call it and every case in run.sh is
 * blind to the whole file.  This links the REAL build/stage0/sh/spname.o and
 * calls it, which is the same trick tests/streams uses for the kernel.
 *
 * Sized against DIRSIZ, so it is the guard for the class CLAUDE.md tabulates
 * (mv, mkdir, rmdir, sed): static char best[DIRSIZ+1] filled by an unbounded
 * copy from a d_name that this port widened 14 -> 254.
 */
#include <stdio.h>

char *spname();

main(argc, argv)
char **argv;
{
	int score;
	char *r;

	if (argc < 2) {
		printf("usage: spnameprobe path\n");
		exit(2);
	}
	score = -1;
	r = spname(argv[1], &score);
	printf("%s %d\n", r ? r : "(null)", score);
	exit(0);
}
