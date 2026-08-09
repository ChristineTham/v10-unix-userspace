/*
 * streamtab[], bdevsw[], cdevsw[] and their counts -- the device and stream
 * configuration.
 *
 * Named after conf/ioconf.c, which on a real V8 is GENERATED: config(8) reads
 * the machine's configuration file and writes out the device and stream switch
 * tables for that machine, with nstream set to the number of entries it
 * emitted.  Ours, not Bell Labs' -- see shim/kern/h/param.h.
 *
 * THE TWO DEVICE SWITCHES MOVED HERE FROM v8fs.c IN §8a STEP 5c, and the move
 * is the point rather than tidying: they are exactly the tables the sentence
 * above says config(8) emits, and they were sitting in the file that holds the
 * kernel SERVICES.  Putting them beside streamtab also puts them under the
 * dense-prefix invariant this file already states, which bdevsw turns out to
 * need for the identical reason -- see v8k_bdconf below.
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

/*
 * ---------------------------------------------------------------------------
 * THE DEVICE SWITCHES
 * ---------------------------------------------------------------------------
 *
 * cdevsw[] stays EMPTY and nchrdev stays 0, which is the accurate description
 * of a machine where the shim answers open(2) itself.  rdwri.c:62,159 dispatch
 * through it only for an IFCHR inode, and no image this port writes contains
 * one.  A zero-length array is not C89, so it gets one all-null row and the
 * count says nothing may index it.
 *
 * bdevsw[] is different since §8a step 5c, because the filesystem code has to
 * reach a disk to be run at all, and a block driver is the one thing that
 * cannot be faked: bread() ultimately calls d_strategy and waits for the bytes.
 *
 * IT IS FILLED IN AT RUN TIME FOR THIS FILE'S EXISTING REASON, and the reason
 * is stronger here than it was for streamtab.  What block devices exist is a
 * property of the shim, and the shim has none: CLAUDE.md's unconsumed-component
 * rule says a driver in shim/kern/ with no caller "invents a difference the
 * kernel does not have", and nothing in this port consumes one -- the image
 * tools open the image as an ordinary file through v8s_open.  So the driver
 * belongs in the PROBE, exactly as sioprobe.c's loopback and pipe drivers and
 * ttyprobe.c's tty driver do, and v8k_bdconf() is how the probe says "this
 * device exists and here is its major number".  That is the same statement
 * config(8) made, made later.
 *
 * AND THE DENSE-PREFIX INVARIANT IS THE SAME ONE, ARRIVING THROUGH A DIFFERENT
 * PAIR OF LINES.  bio.c checks a major number one way --
 *
 *	bio.c:352	if (major(dev) >= nblkdev) panic("blkdev");
 *
 * -- so a number at or past the count is caught.  But every site that actually
 * dispatches has NO null check at all.  FIVE d_strategy calls, measured with
 * `grep -c d_strategy src/sys/dev/bio.c', plus one d_open in a different file:
 *
 *	bio.c:115	(*bdevsw[major(dev)].d_strategy)(bp);		bread
 *	bio.c:141	(*bdevsw[major(dev)].d_strategy)(bp);		breada
 *	bio.c:155	(*bdevsw[major(dev)].d_strategy)(rabp);		breada, ra
 *	bio.c:185	(*bdevsw[major(bp->b_dev)].d_strategy)(bp);	bwrite
 *	bio.c:573	(*bdevsw[major(dev)].d_strategy)(bp);		swap
 *	main.c:167	(*bdevsw[major(rootdev)].d_open)(rootdev, 1);	iinit
 *
 * THIS COMMENT HAS NOW BEEN WRONG ABOUT THAT COUNT TWICE.  A first draft said
 * THREE and named the wrong lines for two of them.  The correction said SIX and
 * cited a grep that yields five -- because the sixth row is a d_open in
 * upstream's main.c, and the row was counted while the command named only
 * bio.c.  A count and the command that produced it have to describe the same
 * population, and a list with a heading is not a measurement of the list.
 *
 * -- so a HOLE below nblkdev is a null call, which is precisely the FIOLOOKLD
 * shape argued above.  Upstream never meets it because config(8) emits a dense
 * table; the conclusion is again to keep the invariant rather than to patch
 * Bell Labs' source.  (bio.c:573 is inside swap(), which this port reaches
 * only through the five PANIC services v8fs.c documents, so it is listed for
 * completeness rather than as a live path.)
 *
 * REQUIRING d_open AND d_strategy IS UPSTREAM'S OWN RULE, not an invention.
 * main.c:218-219 counts the table with
 *
 *	for (bdp = bdevsw; bdp->d_open; bdp++) nblkdev++;
 *
 * -- a null d_open IS the terminator, so every row config(8) emitted had one.
 * d_strategy is the other one because bread dereferences it unguarded.  A row
 * missing either would be a row upstream could not have produced.
 */
struct cdevsw cdevsw[] = { { 0, 0, 0, 0, 0, 0, 0 } };
int		nchrdev;

/*
 * Eight.  The count is the shim's to choose in the same way NSTRCONF above is,
 * and it costs 8 * sizeof(struct bdevsw) = 320 bytes.  A major number is
 * (dev>>8)&0377 so the format allows 256 of them; eight is the smallest round
 * number comfortably above the one this port has ever needed, which is one.
 */
#define	NBLKCONF	8

struct bdevsw	bdevsw[NBLKCONF];
int		nblkdev;	/* the POPULATED prefix; see above */

int
v8k_bdconf(struct bdevsw *bd)
{
	if (bd == NULL || nblkdev >= NBLKCONF)
		return (-1);
	if (bd->d_open == NULL || bd->d_strategy == NULL)
		return (-1);	/* a row upstream's own counter would end at */
	bdevsw[nblkdev] = *bd;
	return (nblkdev++);
}

/*
 * v8k_stunconf's counterpart, and it exists for the same reason: a test that
 * configures a device, runs, and then wants a machine with no block devices.
 * Zeroing the row rather than only dropping the count is deliberate -- the
 * count is what nblkdev asserts, and leaving a live function pointer above it
 * would make a mistaken index find a driver instead of a null.
 */
void
v8k_bdunconf(void)
{
	while (nblkdev > 0) {
		nblkdev--;
		bdevsw[nblkdev].d_open = NULL;
		bdevsw[nblkdev].d_close = NULL;
		bdevsw[nblkdev].d_strategy = NULL;
		bdevsw[nblkdev].d_dump = NULL;
		bdevsw[nblkdev].d_flags = 0;
	}
}
