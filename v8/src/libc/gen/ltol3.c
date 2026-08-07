/* @(#)ltol3.c	4.1 (Berkeley) 12/21/80 */
#include <sys/types.h>
ltol3(cp, lp, n)
char	*cp;
daddr_t	*lp;			/* PORT: upstream `long *' -- see below */
int	n;
{
	register i;
	register char *a, *b;

	a = cp;
	b = (char *)lp;
	for(i=0;i<n;i++) {
#ifdef pdp11
		*a++ = *b++;
		b++;
		*a++ = *b++;
		*a++ = *b++;
#else
#ifdef vax
		*a++ = *b++;
		*a++ = *b++;
		*a++ = *b++;
		b++;
#else
#ifdef interdata
		b++;
		*a++ = *b++;
		*a++ = *b++;
		*a++ = *b++;
#else
#ifdef arm64
		/*
		 * PORT: this is the VAX arm, restored -- three bytes kept and
		 * ONE skipped.  It briefly skipped five, and that was two of
		 * this port's own patches disagreeing rather than a fact about
		 * the machine.  The array being walked is an inode's i_addr[],
		 * which is daddr_t; daddr_t was `long' and LP64 made it eight
		 * bytes, so five was right at the time.  <sys/types.h> now
		 * narrows daddr_t to four -- the width V8's own VAX compiler
		 * gave it, since it defined NOLONG -- and the VAX stride is
		 * right again.
		 *
		 * The declared parameter type moved with it.  Upstream says
		 * `long *' because on a VAX that WAS daddr_t; here the two have
		 * parted, and this port has been bitten enough times by a
		 * declaration that lies about a width to spend an include on
		 * saying which one is meant.  mkfs(8) is the only caller.
		 */
		*a++ = *b++;
		*a++ = *b++;
		*a++ = *b++;
		b++;
#else
	Unknown machine!
#endif
#endif
#endif
#endif
	}
}
