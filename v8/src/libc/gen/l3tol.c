/* @(#)l3tol.c	4.1 (Berkeley) 12/21/80 */
l3tol(lp, cp, n)
long	*lp;
char	*cp;
int	n;
{
	register i;
	register char *a, *b;

	a = (char *)lp;
	b = cp;
	for(i=0;i<n;i++) {
#ifdef pdp11
		*a++ = *b++;
		*a++ = 0;
		*a++ = *b++;
		*a++ = *b++;
#else
#ifdef vax
		*a++ = *b++;
		*a++ = *b++;
		*a++ = *b++;
		*a++ = 0;
#else
#ifdef interdata
		*a++ = 0;
		*a++ = *b++;
		*a++ = *b++;
		*a++ = *b++;
#else
#ifdef arm64
		/*
		 * PORT: like the VAX -- little-endian, low byte first -- but a
		 * long is EIGHT bytes here, so five must be cleared, not one.
		 * Copying the VAX arm verbatim would leave the top four bytes
		 * of every block number holding whatever was in the buffer.
		 *
		 * Adding a machine to this list is what the file is built for:
		 * the alternative arm is a bare `Unknown machine!`, which is a
		 * deliberate syntax error.
		 */
		*a++ = *b++;
		*a++ = *b++;
		*a++ = *b++;
		*a++ = 0;
		*a++ = 0;
		*a++ = 0;
		*a++ = 0;
		*a++ = 0;
#else
	Unknown machine!
#endif
#endif
#endif
#endif
	}
}
