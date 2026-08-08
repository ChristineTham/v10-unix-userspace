/*
 * streamtab[] and nstream -- the stream configuration.
 *
 * Named after conf/ioconf.c, which on a real V8 is GENERATED: config(8) reads
 * the machine's configuration file and writes out the device and stream switch
 * tables for that machine, with nstream set to the number of entries it
 * emitted.  Ours, not Bell Labs' -- see shim/kern/h/param.h.
 *
 * WHY IT IS FILLED IN AT RUN TIME.  What line disciplines exist is a property
 * of the shim, not of a VAX's peripheral list, and the shim has none yet: the
 * only streamtab in the tree today is nilinfo, the black hole streamio.c
 * defines for itself and deliberately does NOT register.  A generated table
 * would therefore be a generated empty table.  v8k_stconf() is what a driver
 * or a test calls to say "this discipline exists and here is its number",
 * which is the same statement config(8) made, made later.
 *
 * AND THE TABLE MUST STAY CONTIGUOUS, which is the part that is an invariant
 * rather than a convenience.  streamio.c:627 checks a discipline number three
 * ways --
 *
 *	if (ld.ld<0 || ld.ld>=nstream || streamtab[ld.ld]==NULL)
 *
 * -- so a hole is rejected there.  But FIOLOOKLD at :705-706 is
 *
 *	for (fmt=0; fmt<nstream; fmt++)
 *		if (streamtab[fmt]->wrinit == q->next->qinfo)
 *
 * with NO null check, so a hole below nstream is a null dereference.  On a VAX
 * that read the a.out header bytes at address 0, compared unequal and kept
 * looping; here it is a SIGSEGV, and it is CLAUDE.md's address-0 class again.
 *
 * Upstream never meets it because config(8) emits a dense table.  The
 * conclusion is not to patch streamio.c -- upstream is correct given its own
 * invariant -- but to KEEP THE INVARIANT: v8k_stconf appends, nstream counts
 * the populated prefix, and there is no way to write a hole.  tests/streams
 * asserts the FIOLOOKLD walk over a full table, because a rule nothing
 * exercises cannot be seen to be broken.
 */

#include "../h/param.h"
#include "../../../src/sys/h/stream.h"
#include "../h/conf.h"

/*
 * Sixteen.  V8's research machine configured six stream modules; the number
 * here is the shim's to choose, in the same way NQUEUE is Bell Labs' to choose
 * in sparam.h, and it costs 128 bytes.
 */
#define	NSTRCONF	16

struct streamtab	*streamtab[NSTRCONF];
int			nstream;	/* the POPULATED prefix; see above */

int
v8k_stconf(struct streamtab *st)
{
	if (st == NULL || nstream >= NSTRCONF)
		return (-1);
	streamtab[nstream] = st;
	return (nstream++);
}

/*
 * Forget them all.  Not a kernel operation -- config(8)'s table never
 * shrank -- and it exists for tests, which need to run a configuration and
 * then run a different one in the same process.  Named apart from the
 * authentic vocabulary so it is visibly not one of V8's.
 */
void
v8k_stunconf(void)
{
	while (nstream > 0)
		streamtab[--nstream] = NULL;
}
