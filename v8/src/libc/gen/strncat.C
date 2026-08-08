/*
 * Concatenate s2 on the end of s1.  S1's space must be large enough.
 * At most n characters are moved.
 * Return s1.
 */

char *
strncat(s1, s2, n)
register char *s1, *s2;
register n;
{
	register char *os1;

	os1 = s1;
	while (*s1++)
		;
	--s1;
	/*
	 * PORT: read at most n bytes of s2, which is what V8 SHIPPED.
	 *
	 * The loop this replaces read s2[n] before testing n -- it copied the
	 * byte, then noticed --n < 0 and overwrote it with the NUL.  The output
	 * was therefore always right and only the READ was out of bounds, which
	 * is why nothing ever noticed.  Diagnostic, needing no guard page:
	 * strncat(buf, (char *)1, 0) faults on the old loop and does not touch
	 * s2 on this one.
	 *
	 * THE AUTHORITY FOR THE CHANGE IS UPSTREAM'S OWN ASSEMBLER, and this is
	 * the file that disagrees with it.  libc/gen/strncat.s is what a VAX
	 * actually executed; it opens
	 *
	 *	movl	12(ap),r8	# max src length (arg `n')
	 *	bleq	L6		# done if <= 0
	 *
	 * -- returning without touching s2 at all -- and scans with
	 * `locc $0,r8,(r7)', bounded to exactly n bytes.  This .C file is the
	 * portable reference beside it, and its header calls itself "the
	 * `standard' for the C-library" while reading one byte more than the
	 * code shipped next to it.  So the overread is an artefact of THIS PORT
	 * substituting the reference for the assembler, and removing it restores
	 * what V8 ran rather than departing from it.
	 *
	 * Every caller passes a fixed-width, deliberately unterminated field --
	 * dumpdir.c:183 and libc/gen/ttyname.c:62 pass a directory entry's
	 * d_name, who.c:80 and w.c:332 pass utmp.ut_line -- which is the same
	 * shape as the %.Ns bug in this port's doprnt.c, and for the same
	 * reason: a count argument exists precisely because the source need not
	 * be terminated.
	 *
	 * strcatn.c is the V7-named twin, identical body; it has no .s upstream,
	 * so its note records a deviation rather than a restoration.
	 */
	while (--n >= 0 && (*s1 = *s2++))
		s1++;
	*s1 = '\0';
	return(os1);
}
