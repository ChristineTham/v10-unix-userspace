/*
 * A probe for V8's tty line discipline, src/sys/dev/ttyld.c.  Prints
 * `key value' lines that tests/streams/run.sh asserts on, like probe.c and
 * sioprobe.c, and it is a third probe rather than more of probe.c for the same
 * reason those two are separate: probe.c is the stream ENGINE, sioprobe.c the
 * syscall side, and this is a module that rides on top of both.
 *
 * WHAT CAN BE EXERCISED TODAY, AND WHY IT IS THE OPEN PATH.  ttyld is a line
 * discipline -- conf/devices:75, `standard line-discipline 0 tty tty info',
 * not a device -- so on a real V8 it is pushed onto a terminal's stream by
 * init.c:377's ioctl(0, FIOPUSHLD, &tty_ld) and has a hardware driver beneath
 * it.  There is no driver beneath it here.  That bounds the traffic paths and
 * not the open path: ttyopen (ttyld.c:41-63) never dereferences q->next and
 * sends nothing downstream, so it runs correctly on a bare queue pair.  An
 * earlier survey concluded the opposite -- "ttyld has no bottom end so it
 * cannot be exercised" -- from reading the module rather than the function.
 *
 * The include order is forced in both directions; probe.c's header says why at
 * length, and the short version is that shim/kern/h/param.h claims _OFF_T,
 * _INO_T and _DEV_T and must precede any host header or struct inode changes
 * shape.
 */
#include "../../shim/kern/h/param.h"

#undef printf
#undef bcopy

/*
 * Then ttyld.c's own five, in ttyld.c's own order, because it is load-bearing
 * rather than tidy: ttyld.h declares `struct tchars t_chr' and a union holding
 * `struct sgttyb', both of which live in ioctl.h, and struct streamtab is only
 * forward-declared in param.h -- conf.h completes it.  Getting the order wrong
 * gives "field has incomplete type", which reads like a missing header rather
 * than a misplaced one.
 */
#include <stdio.h>
#include "../../src/sys/h/stream.h"
#include "../../src/sys/h/ioctl.h"
#include "../../src/sys/h/ttyld.h"
#include "../../shim/kern/h/conf.h"
#include "../../src/sys/research/sparam.h"
#include "../../shim/kern/dev/tty.h"

extern struct streamtab ttyinfo;
extern char partab[];
extern struct queue *allocq();
extern void v8k_streaminit();

/*
 * Attach a fresh queue pair and push the discipline onto it the way the kernel
 * does -- through qinfo->qopen rather than by calling ttyopen by name, so the
 * `long (*)()' slot the Makefile suppresses a warning about is the thing being
 * exercised.  Returns what qopen returned.
 */
static int
pushtty(qp)
struct queue **qp;
{
	struct queue *q = allocq();

	if (q == 0) return (-99);
	q->qinfo = ttyinfo.rdinit;
	WR(q)->qinfo = ttyinfo.wrinit;
	*qp = q;
	return ((*q->qinfo->qopen)(q, 0));
}

main()
{
	struct queue *q, *q2;
	struct ttyld *tp;
	int i, r, nopen, firstfail, negative;

	setvbuf(stdout, (char *)0, _IONBF, 0);
	v8k_streaminit();

	/* --- the number config(8) would have generated -------------------- */
	printf("ntty %d\n", NTTY);
	printf("ttyldsize %d\n", (int)sizeof(struct ttyld));
	/*
	 * How many entries tty[] really has is NOT asked here.  This file
	 * includes the same tty.h ttyld.c did, so `sizeof tty / sizeof tty[0]'
	 * would be two readings of one number -- it would agree even if the
	 * object had been compiled against a different NTTY.  run.sh measures
	 * the common symbol's size out of the archive instead.
	 */

	/*
	 * --- partab is real data, not an empty stub -----------------------
	 *
	 * An undefined `extern char partab[]' would link as a common symbol
	 * of zeros and every read would be 0, so the values are checked
	 * rather than the symbol.  Three, from three parts of the table, and
	 * transcribed from src/sys/sys/partab.c rather than from a run: NUL's
	 * 0001, tab's 0004 -- a class in the low six bits, which is what the
	 * file's own comment says they are for -- and '@' at 0200, a parity
	 * bit with no class.  ('A' is 0000 and would have proved nothing; it
	 * was the first guess here and the table said otherwise.)
	 */
	printf("partab0 %d\n",   partab[0]    & 0377);
	printf("partabTab %d\n", partab['\t'] & 0377);
	printf("partabAt %d\n",  partab['@']  & 0377);

	/* --- a discipline pushed onto a bare queue pair -------------------- */
	r = pushtty(&q);
	printf("open1 %d\n", r);
	tp = (struct ttyld *)q->ptr;
	printf("ptrset %d\n", tp != 0);
	printf("wrsame %d\n", (struct ttyld *)WR(q)->ptr == tp);
	printf("qdelim %d\n", (q->flag & QDELIM) != 0);
	printf("qnoenb %d\n", (q->flag & QNOENB) != 0);

	/* --- the state ttyopen sets, which is V8's default terminal -------- */
	printf("ttuse %d\n", (tp->t_state & TTUSE) != 0);
	printf("echo %d\n",  (tp->t_flags & ECHO) != 0);
	printf("crmod %d\n", (tp->t_flags & CRMOD) != 0);
	printf("erase %d\n", tp->t_erase & 0377);
	printf("kill %d\n",  tp->t_kill & 0377);
	printf("intrc %d\n", tp->t_chr.t_intrc & 0377);
	printf("quitc %d\n", tp->t_chr.t_quitc & 0377);
	printf("delct %d\n", tp->t_delct);
	printf("col %d\n",   tp->t_col);

	/* --- pushing twice is idempotent: ttyld.c:47 returns on qp->ptr ---- */
	r = (*q->qinfo->qopen)(q, 0);
	printf("open2 %d\n", r);
	printf("samescnd %d\n", (struct ttyld *)q->ptr == tp);

	/* --- close releases the slot -------------------------------------- */
	(*q->qinfo->qclose)(q);
	printf("closed %d\n", (tp->t_state & TTUSE) == 0);

	/*
	 * --- exhaustion, which is the case that matters most ---------------
	 *
	 * tty[] holds NTTY disciplines and ttyopen refuses past the end.  Two
	 * things are being asserted, and the second is the load-bearing one:
	 * that the refusal happens at NTTY rather than one either side, and
	 * that it is spelled 0.  CLAUDE.md's rule is that a qopen must never
	 * return a negative int, because stopen:124 does not see
	 * 0x00000000ffffffff as NULL and :131 does not see it as 1, so a
	 * `return -1' here would look like SUCCESS and hand the caller an
	 * inode pointer of 0xffffffff.  ufalloc() in the same tree does return
	 * -1, so this is not hypothetical -- it is the shape to watch for in
	 * every driver that follows this one.
	 */
	nopen = 0; firstfail = -1; negative = 0;
	for (i = 0; i < NTTY + 4; i++) {
		r = pushtty(&q2);
		if (r == -99) break;			/* out of queues, not slots */
		if (r < 0) negative++;
		if (r == 1) nopen++;
		else if (firstfail < 0) firstfail = i;
	}
	printf("nopen %d\n", nopen);
	printf("firstfail %d\n", firstfail);
	printf("negative %d\n", negative);
	return (0);
}
