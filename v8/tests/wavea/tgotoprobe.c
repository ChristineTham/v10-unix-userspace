/*
 * tgotoprobe -- exercise tgoto(3), which no installed program reaches.
 *
 * WHY A PROBE AND NOT A CASE ON A COMMAND.  ul(1) is libtermcap's only
 * consumer here and it does cursor addressing nowhere: it calls tgetent,
 * tgetflag, tgetstr and tputs, and never tgoto.  So the whole of tgoto.c --
 * 2376 bytes of object, the largest single member -- had no test looking at
 * it, which a mutation is what proved: dropping upstream's four -DCM_ flags
 * took the object to 2096 bytes and EVERY case stayed green.  That is the
 * non-firing mutation this repository treats as the informative one, and the
 * cause here is neither dead code nor a vacuous case but simply no consumer.
 *
 * WHAT THE FOUR FLAGS DO, and why a missing one is silent.  CM_N, CM_GT, CM_B
 * and CM_D each guard one case in tgoto's cursor-addressing interpreter.  With
 * the flag absent the case is not a compile error -- it falls through to
 * `default: goto toohard', and tgoto returns the string "OOPS" for every
 * terminal whose cm= uses that escape.  A cursor that never moves reads as a
 * broken terminal, not as a missing -D.
 *
 * The expectations below are tgoto's own documented arithmetic, worked through
 * by hand from the comment at the head of tgoto.c, so each case is a test of
 * the interpreter and not only of the flag.  Note the argument order is
 * tgoto(CM, destcol, destline) and `which' starts on destline.
 */
#include <stdio.h>

char *tgoto();

/* tgoto keeps these for the %. and %+ backspace hack; unused by these cases,
 * but they are the library's own globals and must exist for the link. */
extern char *UP, *BC;

static int fails = 0;

static void
expect(name, got, want)
	char *name, *got, *want;
{
	int i;

	for (i = 0; got[i] == want[i]; i++)
		if (got[i] == '\0') {
			printf("ok %s\n", name);
			return;
		}
	fails++;
	printf("FAIL %s: want [%s] got [%s]\n", name, want, got);
}

main()
{
	/*
	 * The unguarded path first, so a failure here says the interpreter is
	 * broken rather than that a flag is missing.  This is vt100's own cm:
	 * %i makes it one-origin, then row then column.
	 */
	expect("plain %i%d;%d", tgoto("\033[%i%d;%dH", 4, 9), "\033[10;5H");

	/* %B -- BCD, two decimal digits packed into one byte: 25 -> 0x25 = 37 */
	expect("CM_B  %B", tgoto("%B%d", 0, 25), "37");

	/* %D -- Delta Data, backwards bcd: 25 - 2*(25%16) = 25 - 18 = 7 */
	expect("CM_D  %D", tgoto("%D%d", 0, 25), "7");

	/*
	 * %>xy -- if the value exceeds x, add y.  x is ' ' (32) and y is 1
	 * here, so 40 > 32 and the value becomes 41.
	 */
	expect("CM_GT %>", tgoto("%> \001%d", 0, 40), "41");

	/*
	 * %n -- XOR both coordinates with 0140.  destline 5 becomes 101, and
	 * `which' is destline because oncol is still 0.
	 */
	expect("CM_N  %n", tgoto("%n%d", 0, 5), "101");

	printf("tgotoprobe: %d failed\n", fails);
	exit(fails != 0);
}
