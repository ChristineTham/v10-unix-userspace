/*
 * NEW CODE.  Replaces libc/gen/memcpy.s, memset.s, memcmp.s, memchr.s and
 * memccpy.s, which are VAX assembly built around the `movc3`/`cmpc3`/`locc`
 * block instructions and have no portable form.
 *
 * Semantics are taken from V8's own memory(3) man page, not from ANSI C, and
 * they differ in one place that matters: memccpy returns "a pointer to the
 * character after the copy of c in s1, or zero if c was not found in the first
 * n characters of s2".
 *
 * These five live in one file rather than five, unlike the rest of V8's libc.
 * The one-function-per-file convention exists so the archive pulls in only what
 * a program uses; that costs nothing here, because a program using one of these
 * almost always uses several, and together they are under a hundred lines.
 *
 * bcopy, bzero and bcmp are deliberately NOT here.  They are BSD routines V8's
 * libc never had -- cmd/ex and cmd/compat each ship their own bcopy.c -- and
 * adding them to libc would quietly change what the tree links against.
 */

char *
memcpy(s1, s2, n)
	register char *s1, *s2;
	register int n;
{
	register char *d = s1;

	while (--n >= 0)
		*d++ = *s2++;
	return (s1);
}

char *
memset(s, c, n)
	register char *s;
	register int c, n;
{
	register char *d = s;

	while (--n >= 0)
		*d++ = c;
	return (s);
}

memcmp(s1, s2, n)
	register char *s1, *s2;
	register int n;
{
	/*
	 * Unsigned comparison.  V8's char is signed (see macdefs.h CHSIGN), so
	 * comparing the chars directly would order 0x80 below 0x00 and make
	 * memcmp disagree with strcmp on high-bit bytes.
	 */
	while (--n >= 0) {
		if (*s1 != *s2)
			return ((*s1 & 0377) - (*s2 & 0377));
		s1++;
		s2++;
	}
	return (0);
}

char *
memchr(s, c, n)
	register char *s;
	register int c, n;
{
	c &= 0377;
	while (--n >= 0) {
		if ((*s & 0377) == c)
			return (s);
		s++;
	}
	return ((char *)0);
}

char *
memccpy(s1, s2, c, n)
	register char *s1, *s2;
	register int c, n;
{
	c &= 0377;
	while (--n >= 0) {
		*s1++ = *s2;
		if ((*s2++ & 0377) == c)
			return (s1);	/* the character AFTER the copy of c */
	}
	return ((char *)0);
}
