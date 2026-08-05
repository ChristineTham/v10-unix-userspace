/*
 * A probe for V8's stream machinery.  Prints `key value' lines; tests/run.sh
 * asserts on them, so the expected values live in the shell script next to the
 * reason for each, as the other suites do it.
 *
 * This links src/sys/dev/stream.c -- authentic Bell Labs kernel source -- and
 * shim/kern/dev/machdep.c, and runs them as ordinary host code.  Nothing here
 * is a V8 binary: the stream engine is the KERNEL side of the seam, so it is
 * exercised the way the shim is, not the way a ported program is.
 */

#include <stdio.h>
#include "../../shim/kern/h/param.h"
#include "../../src/sys/h/stream.h"
#include "../../src/sys/research/sparam.h"

#undef printf
#undef bcopy

extern struct block cblock[];
extern struct block *qfreelist[4];
extern struct queue queue[];
extern struct queue *qhead, *qtail;
extern int cblockC[];
extern int queueflag;

/*
 * K&R declarations, matching stream.h's own -- it already declares getq, putq,
 * allocb, backq and allocq that way, and a modern prototype for putq here is a
 * CONFLICTING type rather than a stricter one.  The probe is testing 1985 code
 * and is compiled in its dialect.
 */
int	freeb(), putbq(), flushq(), qenable(), queuerun();
int	putctl(), putd(), putcpy(), qreply(), qpctl();

/* How many times each queue's service procedure has run. */
static int	served, servedq2;

static int srv(struct queue *q)   { served++; return 0; }
static int srv2(struct queue *q)  { servedq2++; return 0; }
/*
 * putp IS putq here, which matters more than it looks: putctl() and putcpy()
 * do not call putq, they call `(*q->qinfo->putp)(q, bp)' -- the queue's own put
 * procedure, because a module's job is to decide what happens to a message
 * rather than to assume it is enqueued.  A probe whose putp discarded blocks
 * therefore made both of them look broken.  (It did, first time round.)
 */
static struct qinit rinit = { putq, srv,  0, 0, 40, 10 };
static struct qinit winit = { putq, srv2, 0, 0, 40, 10 };

static int
freecount(int class)
{
	struct block *bp;
	int n = 0;
	for (bp = qfreelist[class]; bp; bp = bp->next)
		n++;
	return (n);
}

static struct queue *
newq(void)
{
	struct queue *q = allocq();
	q->qinfo = &rinit;
	WR(q)->qinfo = &winit;
	return (q);
}

static struct block *
data(const char *s, int n)
{
	struct block *bp = allocb(n);
	int i;
	for (i = 0; i < n; i++)
		bp->wptr[i] = (unsigned char)s[i];
	bp->wptr += n;
	return (bp);
}

int
main(void)
{
	struct block *b4, *b16, *b64, *bbig, *bp;
	struct queue *q, *q2;
	int i, n;

	setvbuf(stdout, (char *)0, _IONBF, 0);	/* a panic exits without flushing */
	v8k_streaminit();

	/* --- the freelists qinit() built, one per size class -------------- */
	printf("free4 %d\n",   freecount(0));
	printf("free16 %d\n",  freecount(1));
	printf("free64 %d\n",  freecount(2));
	printf("freebig %d\n", freecount(3));

	/* --- allocb picks a class by size, and lim reflects the REAL size,
	 *     which is not the same as the size used for queue accounting.  */
	b4   = allocb(3);    printf("cls3 %d\nlim3 %ld\n",     b4->class,   (long)(b4->lim - b4->base));
	b16  = allocb(16);   printf("cls16 %d\nlim16 %ld\n",   b16->class,  (long)(b16->lim - b16->base));
	b64  = allocb(40);   printf("cls40 %d\nlim40 %ld\n",   b64->class,  (long)(b64->lim - b64->base));
	bbig = allocb(900);  printf("cls900 %d\nlim900 %ld\n", bbig->class, (long)(bbig->lim - bbig->base));
	printf("newtype %d\n", b4->type);		/* allocb sets M_DATA */

	/* --- freeb returns the block to its own class ---------------------- */
	n = freecount(0);
	freeb(b4);
	printf("freed4delta %d\n", freecount(0) - n);
	freeb(b16); freeb(b64); freeb(bbig);

	/* --- FIFO ----------------------------------------------------------
	 * FULL blocks, and that is the point rather than an accident.  putq
	 * coalesces a partial M_DATA block into the tail of the previous one,
	 * so three one-byte writes are ONE block on the queue -- correct, and
	 * it made the first version of this test read "A" and call it a
	 * failure.  A 4-byte payload in a 4-byte block leaves no room, so these
	 * stay three messages and the ordering is what is being measured.     */
	q = newq();
	putq(q, data("AAAA", 4));
	putq(q, data("BBBB", 4));
	putq(q, data("CCCC", 4));
	{
		char got[8]; int k = 0;
		while ((bp = getq(q)) != NULL) { got[k++] = (char)*bp->rptr; freeb(bp); }
		got[k] = 0;
		printf("fifo %s\n", got);
	}

	/* --- a priority message overtakes ordinary data --------------------
	 * putq puts type >= QPCTL after any other priority messages but ahead
	 * of everything else, which is what makes M_FLUSH and M_HANGUP mean
	 * anything on a queue that is already backed up.                      */
	putq(q, data("aaaa", 4));
	putq(q, data("bbbb", 4));
	bp = allocb(1); bp->type = M_STOP; *bp->wptr++ = 'S';
	putq(q, bp);
	bp = allocb(1); bp->type = M_START; *bp->wptr++ = 'T';
	putq(q, bp);
	{
		char got[8]; int k = 0;
		while ((bp = getq(q)) != NULL) { got[k++] = (char)*bp->rptr; freeb(bp); }
		got[k] = 0;
		printf("pri %s\n", got);
	}

	/* --- COALESCING, which is the path through bcopy -------------------
	 * Two small M_DATA blocks in a row become one, when the second fits in
	 * what is left of the first.  If bcopy were wrong this is where it
	 * would show, and it is also the only caller of it in stream.c.       */
	putq(q, data("hello", 5));
	putq(q, data("world", 5));
	bp = getq(q);
	{
		char got[16]; int k;
		for (k = 0; k < bp->wptr - bp->rptr; k++) got[k] = (char)bp->rptr[k];
		got[k] = 0;
		printf("coalesced %s\n", got);
		printf("coalescedrest %d\n", getq(q) == NULL);
	}
	freeb(bp);

	/* --- putbq puts back at the front, but still after priority --------- */
	putq(q, data("2222", 4));
	bp = allocb(1); bp->type = M_STOP; *bp->wptr++ = 'P';
	putq(q, bp);
	putbq(q, data("1111", 4));
	{
		char got[8]; int k = 0;
		while ((bp = getq(q)) != NULL) { got[k++] = (char)*bp->rptr; freeb(bp); }
		got[k] = 0;
		printf("putbq %s\n", got);
	}

	/* --- flushq empties, and gives the blocks back ---------------------- */
	n = freecount(0);
	for (i = 0; i < 5; i++) putq(q, data("xxxx", 4));
	printf("beforeflush %d\n", q->count != 0);
	flushq(q, 0);
	printf("afterflush %d\n", q->count);
	printf("flushreturned %d\n", freecount(0) == n);

	/* --- the high-water mark sets QFULL --------------------------------
	 * qinfo->limit is 40 above and accounting is in bsize[] units, so 4-byte
	 * blocks count 4 apiece: ten of them reach it exactly.  Full blocks
	 * again -- ten one-byte ones would coalesce down to 12 and never reach
	 * the mark, which is what this test said before it said anything.     */
	printf("fullbefore %d\n", (q->flag & QFULL) != 0);
	for (i = 0; i < 10; i++) putq(q, data("yyyy", 4));
	printf("fullafter %d\n", (q->flag & QFULL) != 0);
	printf("fullcount %d\n", q->count);
	flushq(q, 0);

	/* --- backq: the queue whose output feeds this one ------------------- */
	/* backq(q) is "the queue behind me, going my way": it crosses to the
	 * other side of the pair, follows that side's next, and crosses back.
	 * So for WR(q) to have WR(q2) behind it, the link to set is on the READ
	 * side -- q->next = q2 -- and getting that backwards is a panic rather
	 * than a wrong answer, which is how this test was written wrong once. */
	q2 = newq();
	q->next = q2;
	printf("backq %d\n", backq(WR(q)) == WR(q2));

	/* --- qenable and queuerun: the service procedure actually runs ------ */
	served = 0;
	qenable(q);
	printf("servedimmediate %d\n", served);

	/* --- THE DEFERRAL, which is the machine-dependent half's whole claim.
	 * On the VAX setqsched() requested a software interrupt at IPL 1, held
	 * off until the priority level dropped.  Here spl6/splx are a nesting
	 * counter and setqsched consults it -- so a qenable inside a critical
	 * section must NOT run the service procedure until the splx that ends
	 * it.  Made observable on purpose: an spl6/splx pair that did nothing
	 * would pass every other case in this file.                           */
	served = 0;
	i = spl6();
	qenable(q);
	printf("deferredinside %d\n", served);
	splx(i);
	printf("deferredafter %d\n", served);

	/* ...and nested, because splx(s) restores a LEVEL rather than dropping
	 * one, so an inner splx must not release an outer section.            */
	served = 0;
	{
		int outer = spl6();
		int inner = spl6();
		qenable(q);
		splx(inner);
		printf("nestedinner %d\n", served);
		splx(outer);
		printf("nestedouter %d\n", served);
	}

	/* --- putctl, putd and putcpy: the convenience producers ------------- */
	putctl(q, M_HANGUP);
	bp = getq(q);
	printf("putctl %d\n", bp ? bp->type : -1);
	if (bp) freeb(bp);

	qpctl(q, M_FLUSH);	/* ... and qpctl is the same thing via putq */
	bp = getq(q);
	printf("qpctl %d\n", bp ? bp->type : -1);
	if (bp) freeb(bp);
	flushq(q, 0);

	putd(putq, q, 'z');
	bp = getq(q);
	printf("putd %c%d\n", bp ? (char)*bp->rptr : '?', bp ? (int)(bp->wptr - bp->rptr) : -1);
	if (bp) freeb(bp);

	putcpy(q, "abcdef", 6);
	bp = getq(q);
	{
		char got[16]; int k;
		for (k = 0; bp && k < bp->wptr - bp->rptr; k++) got[k] = (char)bp->rptr[k];
		got[k] = 0;
		printf("putcpy %s\n", got);
	}
	if (bp) freeb(bp);
	flushq(q, 0);

	/* --- allocation is conserved: everything taken has been given back -- */
	printf("conserved4 %d\n", freecount(0));
	printf("conserved16 %d\n", freecount(1));
	printf("conserved64 %d\n", freecount(2));
	printf("conservedbig %d\n", freecount(3));

	/* --- allocq hands out pairs, reader first --------------------------- */
	printf("qpair %d %d\n", (q->flag & QREADR) != 0, (WR(q)->flag & QREADR) == 0);
	printf("otherq %d %d\n", OTHERQ(q) == WR(q), OTHERQ(WR(q)) == q);

	return (0);
}
