/*
 * printx -- ccom's own output formatter.
 *
 * IN ITS OWN FILE ON PURPOSE.  Every caller in the V8 sources calls printx with
 * no prototype in scope, because K&R C had none.  Keeping the definition out of
 * every other translation unit means all those calls go through the identical
 * unprototyped path -- including the ones in this back end, so no file gets a
 * prototype and starts passing arguments differently from the rest.
 *
 * EIGHT FIXED ARGUMENTS, NOT VARARGS.  On Apple's ARM64 ABI a variadic callee
 * reads its arguments from the STACK, while an unprototyped call passes them in
 * x0-x7 like any ordinary call.  So a `void printx(char *, ...)` definition
 * looks in entirely the wrong place and prints garbage -- observed, before this
 * was understood, as:
 *
 *	.globl	<garbage>
 *	(null)	(null), [x29, #0]
 *
 * Named long parameters put them back in the registers an unprototyped caller
 * actually uses.  This mirrors what V8 itself did -- printx(fmt, list), with a
 * fixed parameter whose address it took -- minus the stack walking, which is
 * precisely the part AAPCS64 invalidates.
 *
 * Eight arguments covers every format string in the tree; anything longer is
 * reported rather than silently mangled.
 */

# include <stdio.h>
# include "mfile2.h"

extern int emitout();

printx(fmt, a1, a2, a3, a4, a5, a6, a7, a8)
	char *fmt;
	long a1, a2, a3, a4, a5, a6, a7, a8;
{
	long av[8];
	char buf[8192];
	char spec[32];
	char *f, *o, *e, *s;
	int ai, si;

	av[0] = a1; av[1] = a2; av[2] = a3; av[3] = a4;
	av[4] = a5; av[5] = a6; av[6] = a7; av[7] = a8;
	ai = 0;
	o = buf;
	e = buf + sizeof buf - 64;

	for (f = fmt; *f && o < e; f++) {
		if (*f != '%') { *o++ = *f; continue; }
		if (f[1] == '%') { *o++ = '%'; f++; continue; }

		/*
		 * Copy the conversion spec verbatim and hand it to snprintf with
		 * exactly one argument, so %ld, %#x, %.3s and the rest keep
		 * working without reimplementing printf here.
		 */
		si = 0;
		spec[si++] = *f++;
		while (*f && si < (int)sizeof spec - 4 &&
		    !(*f == 'd' || *f == 'i' || *f == 'u' || *f == 'o' ||
		      *f == 'x' || *f == 'X' || *f == 'c' || *f == 's'))
			spec[si++] = *f++;
		if (*f == '\0') break;

		if (ai >= 8) cerror("printx: more than 8 arguments: %s", fmt);

		if (*f == 's') {
			spec[si++] = 's';
			spec[si] = '\0';
			s = (char *)av[ai++];
			o += snprintf(o, e - o, spec, s ? s : "<null>");
		} else if (*f == 'c') {
			spec[si++] = 'c';
			spec[si] = '\0';
			o += snprintf(o, e - o, spec, (int)av[ai++]);
		} else {
			/*
			 * Force the long form.  Callers pass ints and longs
			 * interchangeably and every argument arrived in a
			 * 64-bit register, so reading it as an int would lose
			 * range on anything wide.
			 */
			if (si < 2 || spec[si - 1] != 'l')
				spec[si++] = 'l';
			spec[si++] = *f;
			spec[si] = '\0';
			o += snprintf(o, e - o, spec, av[ai++]);
		}
	}
	*o = '\0';
	emitout(buf, (int)(o - buf));
	return (0);
}
