/*
 * Concatenate s2 on the end of s1.  S1's space must be large enough.
 * At most n characters are moved.
 * Return s1.
 */

char *
strcatn(s1, s2, n)
register char *s1, *s2;
register n;
{
	register char *os1;

	os1 = s1;
	while (*s1++)
		;
	--s1;
	/*
	 * PORT: read at most n bytes of s2.  See strncat.C, whose body this is
	 * character for character -- strcatn is the V7-spelled twin.
	 *
	 * ONE THING DIFFERS AND IT IS THE JUSTIFICATION, NOT THE CODE.  strncat
	 * has an assembler sibling upstream, strncat.s, which tests `n <= 0'
	 * before touching s2 and scans with a bounded `locc'; there, removing
	 * the overread RESTORES what a VAX executed.  There is no strcatn.s, so
	 * this C body is what V8 ran and this note records a deviation.
	 *
	 * Taken anyway, for the reason sed's trans[] was: w.c is the only
	 * caller and both its calls pass a fixed-width field -- utmp.ut_line at
	 * :389 and getargs() at :555 -- so the byte past the end is the next
	 * struct member or the next record, and reading it was never intended.
	 */
	while (--n >= 0 && (*s1 = *s2++))
		s1++;
	*s1 = '\0';
	return(os1);
}
