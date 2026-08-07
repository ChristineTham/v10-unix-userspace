/* @(#)l3tol.c	4.1 (Berkeley) 12/21/80 */
#include <sys/types.h>
l3tol(lp, cp, n)
daddr_t	*lp;			/* PORT: upstream `long *' -- see below */
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
		 * PORT: the VAX arm, restored -- three bytes then ONE zero.
		 * It cleared five while daddr_t was eight bytes wide, which was
		 * right at the time and stopped being right when
		 * <sys/types.h> narrowed daddr_t to the four V8's own VAX
		 * compiler gave it.  ltol3.c is the inverse and has the whole
		 * argument; the two must agree, and nothing but a filesystem
		 * checker will ever notice if they do not, because l3tol has no
		 * caller in this port yet -- fsck and icheck are its customers
		 * and neither is imported.
		 *
		 * Adding a machine to this list is what the file is built for:
		 * the alternative arm is a bare `Unknown machine!`, which is a
		 * deliberate syntax error.
		 */
		*a++ = *b++;
		*a++ = *b++;
		*a++ = *b++;
		*a++ = 0;
#else
	Unknown machine!
#endif
#endif
#endif
#endif
	}
}
