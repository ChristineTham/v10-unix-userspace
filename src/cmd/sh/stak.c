/*	@(#)stak.c	1.4	*/
/*
 * UNIX shell
 *
 * Bell Telephone Laboratories
 *
 */

#include	"defs.h"


/* ========	storage allocation	======== */

char *
getstak(asize)			/* allocate requested stack */
int	asize;
{
	register char	*oldstak;
	register int	size;

	size = round(asize, BYTESPERWORD);
	oldstak = stakbot;
	staktop = stakbot += size;
	/*
	 * PORT: keep the break ahead of the stack.
	 *
	 * getstak() moves stakbot with no check at all; the shell relies on
	 * locstak() -- three functions up -- having extended the break first,
	 * which every path reaches through the usestak() macro.  setenv() is the
	 * exception: it calls getstak() directly, twice, and staknam() then
	 * copies each "name=value" straight to staktop with movstr().
	 *
	 * On the VAX the initial 3*BRKINCR of arena covered an environment and
	 * the gap never opened.  Here every structure in the arena is two to
	 * three times bigger -- BYTESPERWORD is 8, and `struct namnod` is three
	 * pointers and two {char,char *} pairs -- and it opens immediately:
	 *
	 *	bot/top/end  00000003000031f8 00000003000031f8 0000000300002800
	 *
	 * stakbot 2552 bytes past brkend, so movstr() wrote into unmapped
	 * memory.  The signal handler stdsigs() installed returned, the faulting
	 * instruction retried, and the shell HUNG instead of crashing -- in the
	 * child of a fork, which made it look like a fork or wait problem.
	 *
	 * Checking here rather than in setenv() puts it where stakbot actually
	 * moves, so any other direct caller is covered too.  Same condition and
	 * same growth step as locstak().
	 */
	stakroom(BRKINCR);
	return(oldstak);
}

/*
 * set up stack for local use
 * should be followed by `endstak'
 */
/*
 * PORT: guarantee `room' writable bytes above stakbot.
 *
 * The shell's rule is that there is always BRKINCR of headroom above stakbot,
 * established by locstak() before anything is pushed.  Two things break that
 * rule on a 64-bit host with a modern environment:
 *
 *   - setenv() never calls locstak().  It calls getstak() directly, and
 *     getstak() moves stakbot with no check at all.
 *   - staknam() copies a whole "name=value" to staktop with movstr() and only
 *     afterwards tells getstak() how many bytes it used.  So the headroom has
 *     to cover the LONGEST SINGLE ITEM, not a fixed step.  BRKINCR was 512
 *     bytes, which was ample in 1985; PATH in the environment this was debugged
 *     in is 4315 bytes.
 *
 * The symptom was neither a crash nor a diagnostic: movstr() ran into unmapped
 * memory, the handler stdsigs() had installed returned, the faulting
 * instruction retried, and the shell HUNG -- in the child of a fork, so it
 * looked like a problem with fork or wait rather than with memory.
 *
 * Extending once, sized to the gap, rather than looping around setbrk(): a loop
 * spins forever the moment setbrk stops making progress, which would trade one
 * hang for another.
 */
stakroom(room)
	int room;
{
	if (brkend - stakbot < room)
	{
		long need = (stakbot + room) - brkend;

		if (need < (long)brkincr)
			need = brkincr;
		if (setbrk((int)need) == (char *)-1)
			error(nostack);
		if (brkincr < BRKMAX)
			brkincr += 256;
	}
}

char *
locstak()
{
	if (brkend - stakbot < BRKINCR)
	{
		if (setbrk(brkincr) == -1)
			error(nostack);
		if (brkincr < BRKMAX)
			brkincr += 256;
	}
	return(stakbot);
}

char *
savstak()
{
	assert(staktop == stakbot);
	return(stakbot);
}

char *
endstak(argp)		/* tidy up after `locstak' */
register char	*argp;
{
	register char	*oldstak;

	*argp++ = 0;
	oldstak = stakbot;
	stakbot = staktop = (char *)round(argp, BYTESPERWORD);
	return(oldstak);
}

tdystak(x)		/* try to bring stack back to x */
register char	*x;
{
	while ((char *)(stakbsy) > (char *)(x))
	{
		free(stakbsy);
		stakbsy = stakbsy->word;
	}
	staktop = stakbot = max((char *)(x), (char *)(stakbas));
	rmtemp(x);
}

stakchk()
{
	if ((brkend - stakbas) > BRKINCR + BRKINCR)
		setbrk(-BRKINCR);
}

char *
cpystak(x)
char	*x;
{
	return(endstak(movstr(x, locstak())));
}
