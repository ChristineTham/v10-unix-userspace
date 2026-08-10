/*
 * v8_foldid -- a host user or group id, narrowed into V8's 16 bits.
 *
 * THE CONTRACT IS TWO PROPERTIES AND NOT A FORMULA, because the magic value
 * differs per field and only one of them matters here:
 *
 *	root maps to root		(0 -> 0)
 *	non-root NEVER maps to root	(nothing else -> 0)
 *	everything that fits stays exact (1..32767 unchanged)
 *
 * The second is the one with teeth.  `struct user's u_uid and `struct stat's
 * st_uid are `short' -- V8's own widths, at user.h:33-34 and sys/stat.h -- and
 * a host id is 32 bits, so a bare `(short)' cast silently maps every multiple
 * of 65536 onto ZERO.  Measured: 65536 -> 0 and 131072 -> 0.  Zero is root, and
 * root is the identity fio.c:193's access() bypasses entirely and
 * streamio.c:44 lets past a stream's exclusive-use lock, so the cast does not
 * produce a wrong number, it produces a PRIVILEGE.
 *
 * WHY IT IS A HEADER RATHER THAN A FUNCTION SOMEBODY LINKS.  Three components
 * narrow an id and no two of them may share an archive: libv8sys must not link
 * libv8kern (56 symbol collisions over 29 programs, 25 of them silent -- see
 * shim/kern/NOTES.md), and libkmemu is the one component that may link host
 * libc while the other two may not.  A pure arithmetic rule needs no link edge
 * at all, so this is `static' and each translation unit gets its own copy of
 * the same sentence.  The alternative -- spelling the arithmetic out three
 * times -- is exactly the antipattern kmem.c's one-table rule exists to refuse,
 * and it is how the third site came to be missed in the first place.
 *
 * THE HISTORY, because it is a straight line and it is not finished.  fio.c
 * folded p_pid and then cast u_uid and u_gid with a bare `(short)' ON THE NEXT
 * TWO LINES, directly under its own paragraph explaining why a truncation is
 * wrong; an auditor found it.  This function is that fix, moved somewhere it
 * can be shared, after a sweep found the identical cast still standing in
 * syscall.c's stat_translate (every ls -l) and in procfs.c's u-area (ps's uid
 * column).  Three files, one rule, and the fix had reached one of them.
 * CLAUDE.md's shape for this is "the fix landed on one line and the line beside
 * it kept the assumption" -- here the line beside it was in another component.
 *
 * THE MODULO IS UNSIGNED ON PURPOSE.  C's `%' keeps the sign of the dividend,
 * so `id % 32766 + 1' on a NEGATIVE id yields 0 -- root -- which is the one
 * answer this function exists to make impossible.  No caller passes a negative
 * today (a uid_t is unsigned and widens into a positive long), so it is a hole
 * rather than a bug; doing the arithmetic in unsigned closes it totally and
 * cannot overflow the way negating LONG_MIN would.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO: it is not applied to getuid(2).  See the
 * note above v8s_getuid in syscall.c -- that value flows back OUT to the host,
 * and mv.c:56 is `setuid(getuid())'.
 */

#ifndef V8_ID_H
#define V8_ID_H

static short
v8_foldid(long id)
{
	if (id == 0)
		return (0);				/* root is root */
	if (id > 0 && id <= 32767)
		return ((short)id);			/* the ordinary case, exact */
	return ((short)((unsigned long)id % 32766 + 1));	/* 1..32766, never 0 */
}

#endif /* V8_ID_H */
