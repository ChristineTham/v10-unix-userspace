/*
 * libm, as V8 actually shipped it.
 *
 * Eleven upstream makefiles link -lm -- pic, grap, troff, awk, hoc, ideal,
 * graph, map, factor, primes, wwb -- which reads as a claim that V8 had a math
 * library.  It did not, in the sense that matters.  v8/usr/lib/libm.a is 216
 * bytes.  It holds one member, dummy.o, 62 bytes, whose entire symbol table is
 * the single name `_________'.  There is no source for it anywhere in the
 * tree, because there is nothing to compile: the archive defines no functions.
 *
 * V8's math is in libc/math -- sin, cos, exp, log, pow, sqrt, atan, gamma, the
 * Bessels -- and this port links that into libv8c.  So -lm on V8 resolved to an
 * empty archive and contributed nothing to the link, and the makefiles that
 * name it were writing for portability to systems where it mattered.
 *
 * Reproducing the stub is cheaper than special-casing the flag, and it is the
 * more honest of the two: cc's -l handling stays a general rule about where
 * libraries live (see libpath() in src/cmd/cc.c) instead of growing a name it
 * knows about.  Without this file -lm reaches the macOS SDK, whose libm is a
 * libSystem re-export, and the link fails on an _errno that has no address.
 *
 * The name is upstream's.  In a.out the compiler prepended an underscore, so
 * the nine in the archive are eight here; Mach-O prepends one too, which makes
 * the emitted symbol identical to V8's by construction rather than by copying.
 */
char	________;
