/*
 * A probe for V8's tty line discipline, src/sys/dev/ttyld.c.  Prints
 * `key value' lines that tests/streams/run.sh asserts on, like probe.c and
 * sioprobe.c, and it is a third probe rather than more of probe.c for the same
 * reason those two are separate: probe.c is the stream ENGINE, sioprobe.c the
 * syscall side, and this is a module that rides on top of both.
 *
 * TWO STACKS, BECAUSE THE OPEN PATH AND THE TRAFFIC PATHS NEED DIFFERENT ONES.
 * ttyld is a line discipline -- conf/devices:75, `standard line-discipline 0
 * tty tty info', not a device -- so on a real V8 it is pushed onto a terminal's
 * stream by init.c:377's ioctl(0, FIOPUSHLD, &tty_ld), with a hardware driver
 * beneath it and a stream head above.
 *
 *   1. A BARE QUEUE PAIR, for ttyopen and ttyclose.  ttyopen (ttyld.c:41-63)
 *      never dereferences q->next and sends nothing downstream, so it runs
 *      correctly with nothing attached -- which is what makes the tty[]
 *      exhaustion case affordable, since it needs NTTY+4 opens and would
 *      otherwise need NTTY+4 streams.  An earlier survey concluded the
 *      opposite ("ttyld has no bottom end so it cannot be exercised") from
 *      reading the module rather than the function.
 *
 *   2. THE REAL THREE-LAYER STACK, for everything below the open.  ttyldin,
 *      ttyinsrv, ttyosrv, outconv, ttysig and ttldioc all reach past their own
 *      queue -- ttyldin alone sends data UP through q->next and flow control
 *      DOWN through WR(q)->next -- so a discipline with one end is a
 *      discipline that cannot be driven.  Built the way init.c builds it:
 *
 *          stread / stwrite / stioctl     streamio.c, the stream head
 *                    |                    (AUTHENTIC, 2 recorded deviations)
 *                  ttyld                  ttyld.c
 *                    |                    (AUTHENTIC, byte-identical)
 *                 ttydrv                  this file, under 100 lines
 *
 *      stopen() the driver, v8k_stconf() the discipline, FIOPUSHLD to push it
 *      between them.  Only the bottom layer is ours, and it is the smallest
 *      thing that can absorb what comes down and originate what comes up.
 *
 * WHY THE DRIVER IS HERE AND NOT IN shim/kern/.  Nothing in the port consumes
 * one.  PLAN.md section 8a step 1b costed a host-fd stream driver to sit under
 * /dev/tty, and that was measured wrong four ways: V8's /dev/tty is a hard link
 * to /dev/fd/3 and opening it is dup(2) (proto-dev:91, conf/devices:55,
 * conf/conf.c:565, sys2.c:174).  So a driver in shim/kern/ would be a component
 * with no caller, which is the mirror of this port's recurring lesson -- an
 * unexercised rule cannot be seen to be incomplete, and an unconsumed component
 * invents a difference the kernel does not have.  sioprobe.c's loopback and
 * pipe drivers are the precedent: scaffolding lives with the probe.
 *
 * The include order is forced in both directions; probe.c's header says why at
 * length, and the short version is that shim/kern/h/param.h claims _OFF_T,
 * _INO_T and _DEV_T and must precede any host header or struct inode changes
 * shape.
 */
#include "../../shim/kern/h/param.h"

/*
 * param.h's redirects are undone in ONE place now -- there are thirteen of
 * them since §8a step 5 and a copied list decays.  shim/kern/h/hostok.h says why.
 */
#include "../../shim/kern/h/hostok.h"
/*
 * And two more than the bare-queue-pair half needed, for the same reason
 * sioprobe.c undefs them: param.h aims these names at the shim's kernel
 * versions, and <signal.h> declares a host psignal(int, const char *) that
 * then conflicts.  The kernel objects were compiled with the macros in force;
 * this translation unit is a caller, not part of the kernel.
 */

/*
 * Then ttyld.c's own five, in ttyld.c's own order, because it is load-bearing
 * rather than tidy: ttyld.h declares `struct tchars t_chr' and a union holding
 * `struct sgttyb', both of which live in ioctl.h, and struct streamtab is only
 * forward-declared in param.h -- conf.h completes it.  Getting the order wrong
 * gives "field has incomplete type", which reads like a missing header rather
 * than a misplaced one.
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "../../src/sys/h/stream.h"
#include "../../src/sys/h/ioctl.h"
#include "../../src/sys/h/ttyld.h"
#include "../../shim/kern/h/conf.h"
#include "../../src/sys/research/sparam.h"
#include "../../shim/kern/dev/tty.h"

/*
 * And the stream head's four, which the bare-queue-pair half did not need.
 * proc.h and user.h are the shim's; inode.h and file.h are V8's, and stopen
 * takes a struct inode by the same rule stread and stwrite take a u-area.
 */
#include "../../shim/kern/h/proc.h"
#include "../../src/sys/h/dir.h"	/* struct direct, for user.h's u_dent */
#include "../../shim/kern/h/user.h"
#include "../../src/sys/h/inode.h"
#include "../../src/sys/h/file.h"

extern struct streamtab ttyinfo;
extern char partab[];
extern struct queue *allocq();
extern void v8k_streaminit();
extern void v8k_procinit();

/*
 * K&R declarations matching stream.h's own -- probe.c says why a modern
 * prototype here would be a CONFLICTING type rather than a stricter one.
 */
int	qreply(), putq(), freeb(), putctl(), flushq(), putctl1();
struct block *allocb();

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

/* ------------------------------------------------------------------------
 * THE DRIVER.  What step 1c needed and step 1b did not have: a bottom end.
 *
 * It is deliberately dumb.  Everything interesting in this file is Bell Labs'
 * -- the canonicalisation, the tab expansion, the signal generation -- and the
 * driver's whole job is to be the terminal those functions talk to: absorb and
 * record what arrives from above, and originate what a UART interrupt would.
 *
 * ONE THING IT MUST GET RIGHT, AND IT IS THE ONE THAT HANGS.  stioctl
 * (streamio.c:759-786) sends an M_IOCTL down and then tsleeps on stq->iocblk
 * with a FIFTEEN-SECOND deadline.  A driver that frees an M_IOCTL instead of
 * acknowledging it does not fail -- it stalls for fifteen seconds and then
 * returns EIO, which in a suite of forty ioctls is ten minutes that reads as a
 * hung test rather than a missing reply.
 */
static struct queue *drvwq;		/* the driver's WRITE queue */
static char	drvbuf[1024];		/* bytes that reached the `terminal' */
static int	drvlen;
static int	drvioc;			/* M_IOCTLs acknowledged */
static int	drvstop, drvstart;	/* flow control seen from above */
static int	drvflush, drvbreak, drvdelim, drvack;
static int	drvdelay, drvdelayval;	/* M_DELAY, outconv's 1970s timing */
static int	drvnak;			/* ioctls refused, cons.c's default arm */

static void
drvreset()
{
	drvlen = drvstop = drvstart = drvflush = drvbreak = drvdelim = 0;
	drvack = drvioc = drvdelay = drvdelayval = drvnak = 0;
	memset(drvbuf, 0, sizeof drvbuf);
}

static int
drvput(q, bp)
struct queue *q;
struct block *bp;
{
	int n;

	switch (bp->type) {

	case M_DATA:
		n = bp->wptr - bp->rptr;
		if (n > (int)sizeof drvbuf - drvlen)
			n = sizeof drvbuf - drvlen;
		if (n > 0) {
			memcpy(drvbuf + drvlen, bp->rptr, (size_t)n);
			drvlen += n;
		}
		break;

	case M_IOCTL:
		/*
		 * The acknowledgement stioctl is asleep on -- and the ONE
		 * PLACE THIS DRIVER HAS TO COPY V8's, because the reply's
		 * length is the reply.  streamio.c:793-798 copies
		 * `wptr - rptr' back to the caller's `arg', and stioctl built
		 * the block with `wptr += sizeof(union stmsg)' -- 20 bytes,
		 * unconditionally, whatever the command.  ttldioc's TIOCSETP
		 * arm does not touch wptr, so an ack left as-is copies 16
		 * bytes (20 less the 4-byte com) into a `struct sgttyb' that
		 * is SIX bytes long.  Ten bytes past the caller's object.
		 *
		 * A first draft of this driver acked everything unchanged,
		 * and the comment here recorded "streamio.c copies wptr-rptr
		 * either way" as though that were a reason not to care.  It
		 * is the reason TO care, and Bell Labs' own drivers say so in
		 * one line: dev/cons.c:56-58 resets wptr for TIOCSETP and
		 * TIOCSETN and FALLS THROUGH to TIOCGETP, which must not
		 * reset it because it has a payload to return; dev/dz.c:229
		 * is the same. The default arm is theirs too -- an M_IOCNAK
		 * with no payload byte, which streamio.c:803-809 turns into
		 * ENOTTY.
		 *
		 * The 8-into-6 that TIOCGETP still does IS upstream's, since
		 * cons.c leaves wptr alone there as well.  Reproduce it; do
		 * not repair it.
		 */
		switch (((union stmsg *)bp->rptr)->ioc0.com) {

		case TIOCSETP:
		case TIOCSETN:
			bp->wptr = bp->rptr;
		case TIOCGETP:
			drvioc++;
			bp->type = M_IOCACK;
			qreply(q, bp);
			return (0);

		default:
			drvnak++;
			bp->type = M_IOCNAK;
			bp->wptr = bp->rptr;
			qreply(q, bp);
			return (0);
		}

	case M_STOP:	drvstop++;	break;
	case M_START:	drvstart++;	break;
	case M_FLUSH:	drvflush++;	break;
	case M_BREAK:	drvbreak++;	break;
	case M_DELIM:	drvdelim++;	break;
	case M_IOCACK:	drvack++;	break;

	/*
	 * A padding delay.  outconv computes one from the delay bits in
	 * t_flags and sends it as a one-byte M_DELAY, which on a real V8 the
	 * device driver turns into that many clock ticks of silence -- the
	 * carriage of a tn 300 physically could not get back before the next
	 * character arrived.  Recording the VALUE and not just the count,
	 * because the four algorithms differ only in the number.
	 */
	case M_DELAY:
		drvdelay++;
		if (bp->wptr > bp->rptr)
			drvdelayval = *bp->rptr & 0377;
		break;
	}
	freeb(bp);
	return (0);
}

/*
 * Return 1, never -1.  CLAUDE.md's rule, and it is harsher than it looks:
 * streamio.c:650 is `else if (nip!=1) panic("pushld qopen returns inode", nip)'
 * on the FIOPUSHLD path, so a driver that borrowed ufalloc()'s -1 convention
 * would widen it to 0x00000000ffffffff and panic the kernel rather than fail
 * the open.
 */
static long drvopen(q, dev)  struct queue *q; { drvwq = WR(q); return (1); }
static int  drvclose(q)      struct queue *q; { drvwq = 0; return (0); }

static struct qinit drvrd = { putq,   0, drvopen, drvclose, 512, 256 };
static struct qinit drvwr = { drvput, 0, drvopen, drvclose, 512, 256 };
static struct streamtab drvinfo = { &drvrd, &drvwr };

/*
 * Input, the way a UART's receive interrupt delivers it: build an M_DATA and
 * qreply it from the driver's write queue, which is OTHERQ(q)->next -- the
 * read side of whatever is above, i.e. ttyldin.
 */
static void
drvinput(s, n)
char *s;
{
	struct block *bp;

	if (drvwq == 0 || (bp = allocb(n)) == NULL)
		return;
	/*
	 * THE BOUND IS WHAT WAS DELIVERED, NOT WHAT WAS ASKED FOR.  allocb
	 * (stream.c:44-46) answers a request above 64 with a SIXTY-FOUR byte
	 * block when class 3's freelist is empty and class 2's is not, so
	 * `n' is an upper bound on the request and not on the capacity.
	 * Unreachable here -- the largest call is eight bytes -- and kept
	 * because this driver is the port's only worked example of one, and
	 * a missing bound is exactly what gets inherited by the next.
	 */
	if (n > bp->lim - bp->wptr)
		n = bp->lim - bp->wptr;
	bp->type = M_DATA;
	memcpy(bp->wptr, s, (size_t)n);
	bp->wptr += n;
	qreply(drvwq, bp);
}

/* The u-area half, as sioprobe.c does it: a system call is a u-area plus a
 * call, and driving stread/stwrite any other way would be driving something
 * else. */
static struct inode tino;

static void
uwrite(s, n)
char *s;
{
	u.u_base = s;
	u.u_count = n;
	u.u_offset = 0;
	u.u_error = 0;
	stwrite(&tino);
}

static int
uread(buf, n)
char *buf;
{
	u.u_base = buf;
	u.u_count = n;
	u.u_offset = 0;
	u.u_error = 0;
	stread(&tino);
	return (n - (int)u.u_count);
}

/*
 * Read one canonical LINE, however many messages it takes -- and it takes more
 * than one whenever the line is longer than canonb, because ttyinsrv returns
 * early when the stream head's queue is full (ttyld.c:234).
 *
 * THIS EXISTS BECAUSE A SHORT READ LEAKS INTO THE NEXT CASE.  The TANDEM case
 * sent 401 characters and read them into a 256-byte buffer; the 145 left in
 * the stream were then read by the case after it, which reported a plausible
 * wrong answer and made the case after THAT look broken.  Nothing was wrong
 * with the discipline.  Same shape as tests/crash-probe.sh's programs reading
 * each other's litter, arriving inside one process instead of one directory:
 * a case has to be a pure function of what it sent.
 *
 * Bounded at eight reads, because a read past the last delimiter blocks and
 * the suite's deadline is the only thing that would notice.
 */
static int lastreads;

static int
readline(buf, n)
char *buf;
{
	int tot = 0, got;

	lastreads = 0;
	while (lastreads < 8 && tot < n) {
		got = uread(buf + tot, n - tot);
		lastreads++;
		if (got <= 0)
			break;
		tot += got;
		if (buf[tot-1] == '\n')
			break;
	}
	return (tot);
}

/*
 * Render what the driver received so a shell can compare it.  Printable ASCII
 * as itself, everything else as \NNN -- because the whole point of outconv is
 * the bytes that are NOT printable, and a raw write to stdout would let a
 * terminal or a shell eat exactly the evidence.
 *
 * THE SPACE IS ESCAPED TOO, AND THAT IS NOT FASTIDIOUSNESS.  run.sh reads
 * these lines with `awk '$1==k {$1=""; print}'', and assigning to a field makes
 * awk REBUILD the record with OFS between fields -- so seven spaces come back
 * as one.  The XTABS case is a case about a run of spaces, so rendering them
 * raw would have compared the wrong thing and passed.  One token per value,
 * no whitespace inside it.
 */
static void
show(key, p, n)
char *key, *p;
{
	int i, c;

	printf("%s ", key);
	if (n <= 0)
		printf("(nothing)");
	for (i = 0; i < n; i++) {
		c = p[i] & 0377;
		if (c > ' ' && c < 0177 && c != '\\')
			putchar(c);
		else
			printf("\\%03o", c);
	}
	putchar('\n');
}

static void
drvshow(key)
char *key;
{
	show(key, drvbuf, drvlen);
}

/* --- the signals ttysig generates, caught rather than defaulted ---------- */
static int sigint, sigquit;
static void onint(s)  { sigint++; }
static void onquit(s) { sigquit++; }

/*
 * Drive the discipline that init(8) drives: a stream head above, ttyld in the
 * middle, the driver below.  Everything here is a path step 1b left dark.
 */
static void
traffic()
{
	struct stdata *sp;
	struct queue *ttq;		/* ttyld's READ queue */
	struct ttyld *tp;
	struct insld ld;
	struct sgttyb sg;
	struct tchars tc;
	char buf[256];
	char big[1024];		/* the canonb-overflow line does not fit in buf */
	int n, i, lds;

	memset(&sg, 0, sizeof sg);	/* stioctl copies the WHOLE struct in,
					 * and ttldioc reads three of its five
					 * fields -- so the two speeds would
					 * otherwise come off uninitialised
					 * stack every time. */

	/* --- build it, the way init.c:368-382 does ------------------------ */
	/*
	 * open(tty,2) then ioctl(0, FIOPUSHLD, &tty_ld).  Two calls, and the
	 * second is the one that makes this a tty rather than a raw device --
	 * "controlling terminal" is a userspace convention in V8, and so is
	 * "terminal": what a line looks like is decided by which discipline
	 * init pushed onto it.
	 */
	tino.i_count = 1;
	tino.i_number = 11;
	tino.i_dev = 3;
	tino.i_sptr = NULL;
	u.u_error = 0;
	printf("drvopenret %d\n", stopen(&drvinfo, 0, 0, &tino) == NULL);
	printf("drvopenerr %d\n", u.u_error);
	sp = tino.i_sptr;
	printf("drvattached %d\n", sp != NULL);
	if (sp == NULL)
		return;

	lds = v8k_stconf(&ttyinfo);
	printf("ttyldnum %d\n", lds >= 0);

	ld.ld = lds;
	ld.level = 0;
	u.u_error = 0;
	stioctl(&tino, FIOPUSHLD, (caddr_t)&ld);
	printf("pusherr %d\n", u.u_error);

	/*
	 * And the discipline really is between the two, not beside them.
	 * sp->wrq is the stream head's write queue, so sp->wrq->next is
	 * whatever it now writes into -- ttyld -- and RD() of that is the
	 * queue ttyldin will be called on.
	 */
	ttq = RD(sp->wrq->next);
	tp = (struct ttyld *)ttq->ptr;
	printf("pushedptr %d\n", tp != 0);
	printf("pushedttuse %d\n", tp && (tp->t_state & TTUSE) != 0);
	printf("pushedbelow %d\n", sp->wrq->next->next != 0);
	/*
	 * And stop here if it did not, rather than reading through a null tp
	 * forty lines later.  A SIGSEGV would take the probe down and every
	 * key below it would come back empty -- fifty cases failing for one
	 * cause, with the cause the least conspicuous of them.
	 */
	if (tp == 0)
		return;

	/* ================= the read path: ttyldin + ttyinsrv ============== */
	/*
	 * Canonical mode, which is what ttyopen sets: ECHO|CRMOD, no RAW, no
	 * CBREAK.  ttyldin queues each byte and, on the newline, enqueues an
	 * M_DELIM and qenables the queue; ttyinsrv then runs at splx(0) and
	 * gathers the line into canonb before sending it up.  Both functions
	 * are needed for one read, which is why they could not be split.
	 */
	drvreset();
	drvinput("hi\n", 3);
	n = readline(buf, sizeof buf);
	printf("canonn %d\n", n);
	show("canon", buf, n);
	printf("canonerr %d\n", u.u_error);

	/*
	 * ECHO is on, so those same three bytes went out the write side as
	 * they arrived -- ttyldin putd's each onto WR(q), ttyosrv drains it
	 * through outconv, and the driver sees them.  A real terminal shows
	 * you what you typed because the KERNEL sends it back, and this is
	 * that loop closing.  CRMOD makes the newline \r\n on the way out.
	 */
	drvshow("echo1");

	/* --- erase, which is ttyinsrv's work and not ttyldin's ------------ */
	/*
	 * ttyldin queues 'h','x',\010,'i' verbatim; only ttyinsrv interprets
	 * the \010, by backing op up over the 'x'.  So a wrong answer here is
	 * specifically the canonicaliser, and the echo line beside it proves
	 * the bytes did arrive.
	 */
	drvreset();
	drvinput("hx\010i\n", 5);
	n = readline(buf, sizeof buf);
	show("canonerase", buf, n);

	/* --- kill: '@' by default, and it discards the line so far -------- */
	drvreset();
	drvinput("junk@hi\n", 8);
	n = readline(buf, sizeof buf);
	show("canonkill", buf, n);

	/* --- CRMOD on input: \r arrives, \n is what the program reads ----- */
	drvreset();
	drvinput("cr\r", 3);
	show("crmodin", buf, n = readline(buf, sizeof buf));
	printf("crmodinn %d\n", n);

	/* ============ the write path: ttyosrv + outconv =================== */
	/*
	 * A tab and a newline, the two characters outconv classifies through
	 * partab (TAB and NEWLINE).  CRMOD turns the \n into \r\n, so what
	 * the program wrote and what reached the wire differ by a byte.
	 *
	 * THE TAB DOES NOT EXPAND HERE, AND THIS CASE ASSERTS THAT IT DOES
	 * NOT.  outconv's expansion loop is guarded by
	 * `(tp->t_flags&TBDELAY)==XTABS' (ttyld.c:385), and ttyopen sets
	 * ECHO|CRMOD only -- XTABS means "this terminal cannot do tabs
	 * itself, expand them for it", which is a property of the hardware
	 * rather than a default.  A first draft of this case expected
	 * `a       b' from reading the loop and not its guard; the measured
	 * answer is the literal tab, and the case below is what actually
	 * reaches the loop.
	 *
	 * t_col ends at 0 and that is the CR/LF dance rather than a reset:
	 * `a' takes it to 1, the tab does `t_col |= 07; t_col++' to 8, `b' to
	 * 9, the injected \r zeroes it, and the \n that follows leaves it
	 * alone because CRMOD is set (ttyld.c:441).
	 */
	drvreset();
	uwrite("a\tb\n", 4);
	printf("outerr %d\n", u.u_error);
	drvshow("outconv");
	printf("outcol %d\n", tp->t_col);

	/* --- and now the expansion loop, with the flag that unlocks it ---- */
	/*
	 * XTABS alone: no ECHO to add bytes from the other direction and no
	 * CRMOD to add a \r, so what the driver receives is exactly what
	 * outconv's tab arm produced.  `a' leaves t_col at 1 and the loop
	 * writes spaces until (t_col & 07) == 0, which is seven of them.
	 */
	sg.sg_erase = CERASE;
	sg.sg_kill  = CKILL;
	sg.sg_flags = XTABS;
	u.u_error = 0;
	stioctl(&tino, TIOCSETP, (caddr_t)&sg);
	printf("xtabserr %d\n", u.u_error);
	drvreset();
	uwrite("a\tb", 3);
	drvshow("xtabs");
	printf("xtabscol %d\n", tp->t_col);

	/* Back to what ttyopen set, so the next section's preconditions are
	 * the ones its comment claims. */
	sg.sg_flags = ECHO|CRMOD;
	u.u_error = 0;
	stioctl(&tino, TIOCSETP, (caddr_t)&sg);

	/* ================= ttysig: a byte becomes a signal ================ */
	/*
	 * The end-to-end one.  DEL (0177) is t_intrc, ttyldin recognises it
	 * and calls ttysig, ttysig flushes both queues and sends M_SIGNAL up,
	 * and the stream head's strput turns that into gsignal(stp->pgrp,
	 * SIGINT) -- which in this shim is a REAL kill(2) to this process.
	 * So the assertion is that a handler ran, not that a flag was set.
	 *
	 * pgrp must be set first: streamio.c:379 is a bare gsignal with no
	 * `if (stp->pgrp)' guard, and a stream that has never had TIOCSPGRP
	 * done to it has pgrp 0.  shim/kern/sys/subr.c carries the guard for
	 * exactly that reason.
	 */
	signal(SIGINT, onint);
	signal(SIGQUIT, onquit);
	sp->pgrp = u.u_procp->p_pgrp;
	printf("pgrpset %d\n", sp->pgrp != 0);

	drvreset();
	sigint = 0;
	drvinput("abc\177", 4);
	printf("intsig %d\n", sigint);
	printf("intflush %d\n", drvflush);	/* WR(q)->next got M_FLUSH */
	printf("intdelct %d\n", tp->t_delct);

	sigquit = 0;
	drvinput("\034", 1);
	printf("quitsig %d\n", sigquit);

	/*
	 * And the flush was not cosmetic: the three characters typed before
	 * the DEL are gone.  A read now would block, so ask the queue.
	 */
	printf("intdropped %d\n", ttq->count);

	/* ============ ttldioc from the process side (fromdev 0) =========== */
	/*
	 * stioctl packages an M_IOCTL, sends it down the write side, and
	 * sleeps on the acknowledgement.  ttyosrv picks it off the queue and
	 * calls ttldioc(q, bp, RD(q), 0) -- and for TIOCSETP the zero matters:
	 * ttldioc passes the block FURTHER DOWN to the driver rather than
	 * answering it, so the ack that wakes stioctl is the driver's.
	 */
	drvreset();
	sg.sg_ispeed = sg.sg_ospeed = 0;
	sg.sg_erase = '#';
	sg.sg_kill  = '%';
	sg.sg_flags = RAW;
	u.u_error = 0;
	stioctl(&tino, TIOCSETP, (caddr_t)&sg);
	printf("setperr %d\n", u.u_error);
	printf("setpraw %d\n", (tp->t_flags & RAW) != 0);
	printf("setperase %d\n", tp->t_erase & 0377);
	printf("setpkill %d\n", tp->t_kill & 0377);
	printf("setpioc %d\n", drvioc);		/* the driver DID see it */
	/* RAW clears QDELIM|QNOENB on the reader -- ttldioc's last act */
	printf("setpqdelim %d\n", (ttq->flag & QDELIM) != 0);

	/* --- and RAW traffic really is raw -------------------------------- */
	/*
	 * The same \r that CRMOD converted above must now arrive unchanged,
	 * and the erase must not erase.  This is ttyldin's RAW branch and
	 * ttyinsrv's (CBREAK|RAW) branch, neither of which the canonical
	 * cases reach.
	 */
	drvreset();
	drvinput("a\010b\r", 4);
	n = uread(buf, sizeof buf);
	printf("rawn %d\n", n);
	printf("rawbyte3 %d\n", n >= 4 ? (buf[3] & 0377) : -1);
	printf("rawecho %d\n", drvlen);		/* RAW does not echo */

	/* --- TIOCGETP reads back what TIOCSETP set ------------------------ */
	memset(&sg, 0, sizeof sg);
	u.u_error = 0;
	stioctl(&tino, TIOCGETP, (caddr_t)&sg);
	printf("getperr %d\n", u.u_error);
	printf("getperase %d\n", sg.sg_erase & 0377);
	printf("getpkill %d\n", sg.sg_kill & 0377);
	printf("getpraw %d\n", (sg.sg_flags & RAW) != 0);

	/* --- TIOCSETC / TIOCGETC: ttyld answers these ITSELF --------------- */
	/*
	 * The behavioural difference worth having a case for.  ttldioc's
	 * TIOCSETP arm passes the block down to the device; its TIOCSETC arm
	 * is `qreply(q, bp)' with fromdev 0, which turns the block round at
	 * the discipline.  So the driver's ioctl counter must NOT move, and
	 * the same stioctl must still complete -- one command reaches the
	 * hardware and the other does not, which is invisible from the
	 * syscall's return value alone.
	 */
	drvreset();
	tc.t_intrc  = 003;		/* ^C rather than DEL */
	tc.t_quitc  = 034;
	tc.t_startc = 021;
	tc.t_stopc  = 023;
	tc.t_eofc   = 004;
	tc.t_brkc   = 0377;
	u.u_error = 0;
	stioctl(&tino, TIOCSETC, (caddr_t)&tc);
	printf("setcerr %d\n", u.u_error);
	printf("setcintrc %d\n", tp->t_chr.t_intrc & 0377);
	printf("setcioc %d\n", drvioc);		/* stayed 0: never went down */

	memset(&tc, 0, sizeof tc);
	u.u_error = 0;
	stioctl(&tino, TIOCGETC, (caddr_t)&tc);
	printf("getcerr %d\n", u.u_error);
	printf("getcintrc %d\n", tc.t_intrc & 0377);

	/* --- CBREAK, the mode between the two ----------------------------- */
	/*
	 * ttyldin has three branches and RAW plus canonical is only two.
	 * CBREAK is the middle: no line gathering, so a character is readable
	 * the instant it arrives and no newline is needed -- but the special
	 * characters are still interpreted and ECHO still happens, and neither
	 * of those is true in RAW.  So the assertions are that `ab' reads back
	 * without a newline being typed, that the driver saw the echo, and
	 * that an interrupt character still signals -- which together are
	 * exactly what separates this mode from the two either side of it.
	 *
	 * intrc is ^C here, not DEL: TIOCSETC above replaced the whole tchars
	 * struct, and this case running after it rather than before is what
	 * lets the same byte prove both.
	 */
	sg.sg_erase = CERASE;
	sg.sg_kill  = CKILL;
	sg.sg_flags = CBREAK|ECHO;
	u.u_error = 0;
	stioctl(&tino, TIOCSETP, (caddr_t)&sg);
	printf("cbreakerr %d\n", u.u_error);
	drvreset();
	drvinput("ab", 2);
	n = uread(buf, sizeof buf);
	printf("cbreakn %d\n", n);
	show("cbreak", buf, n);
	printf("cbreakecho %d\n", drvlen);	/* CBREAK echoes; RAW did not */
	sigint = 0;
	drvinput("\003", 1);
	printf("cbreaksig %d\n", sigint);

	/* --- back to canonical, so the flow-control cases can run --------- */
	sg.sg_erase = CERASE;
	sg.sg_kill  = CKILL;
	sg.sg_flags = ECHO|CRMOD;
	u.u_error = 0;
	stioctl(&tino, TIOCSETP, (caddr_t)&sg);
	printf("recanon %d\n", (tp->t_flags & (RAW|CBREAK)) == 0);
	printf("recanonqdelim %d\n", (ttq->flag & QDELIM) != 0);

	/* ================= flow control: ^S and ^Q ======================== */
	/*
	 * t_stopc arriving from the terminal sets TTSTOP and sends M_STOP
	 * DOWN to the device -- WR(q)->next, not q->next -- and t_startc
	 * clears it and sends M_START.  Both directions in one function, and
	 * the reason the driver had to exist rather than a second module:
	 * a module above would never see these.
	 *
	 * The characters are consumed, not delivered: ttyldin `continue's
	 * past them, so the line reads as if they were never typed.
	 */
	drvreset();
	drvinput("\023", 1);			/* ^C is intrc now; ^S is 023 */
	printf("stopstate %d\n", (tp->t_state & TTSTOP) != 0);
	printf("stopsent %d\n", drvstop);

	drvinput("\021", 1);			/* ^Q */
	printf("startstate %d\n", (tp->t_state & TTSTOP) == 0);
	printf("startsent %d\n", drvstart);

	drvreset();
	drvinput("x\023y\021z\n", 6);
	n = readline(buf, sizeof buf);
	show("flowline", buf, n);

	/* ============ LCASE and maptab[]: a Model 33 with no lower case ==== */
	/*
	 * The most 1970s thing in the file.  A Teletype 33 has no lower case
	 * and no braces, so V8 lets you type them: `\a' for A, `\(' for {,
	 * and a bare A is folded DOWN to a.  Two functions share the work and
	 * the split is the interesting part -- ttyldin marks the escaped
	 * character by setting bit 7 (`c |= 0200') and never consults
	 * maptab[]; ttyinsrv sees the marked byte and does the lookup.  So
	 * the map is applied at canonicalisation, one queue later than the
	 * escape that requested it.
	 *
	 * `A\a\(' therefore comes back `aA{': the plain A folded down, the
	 * escaped a mapped up, the escaped ( mapped to a brace it cannot
	 * type.  maptab is 128 bytes of authentic data (ttyld.c:14-32) and
	 * this is the only thing in the port that reads it.
	 */
	sg.sg_erase = CERASE;
	sg.sg_kill  = CKILL;
	sg.sg_flags = LCASE;		/* no ECHO: keep the device side clean */
	u.u_error = 0;
	stioctl(&tino, TIOCSETP, (caddr_t)&sg);
	printf("lcaseerr %d\n", u.u_error);
	drvreset();
	drvinput("A\\a\\(\n", 6);
	n = readline(buf, sizeof buf);
	show("lcase", buf, n);

	/* --- and the same escape with LCASE off keeps the backslash ------- */
	/*
	 * ttyinsrv's else-arm has three outcomes for a marked byte and only
	 * the middle one is obvious.  With no LCASE: an ordinary character
	 * keeps its backslash (`\z' is two characters), but a character that
	 * IS the erase, kill or eof character is emitted ALONE -- dropping
	 * the backslash, because escaping it is how you type a literal one.
	 * `@' is the default kill, so `\@' is a bare @ and `\z' is not.
	 */
	sg.sg_flags = 0;
	u.u_error = 0;
	stioctl(&tino, TIOCSETP, (caddr_t)&sg);
	drvreset();
	drvinput("\\z\\@\n", 5);
	n = readline(buf, sizeof buf);
	show("escape", buf, n);

	/*
	 * And a doubled backslash, which is measured rather than predicted.
	 * ttyldin:171-175 clears TTESC, notices the byte is an escaped
	 * backslash, strips bit 7 -- and SETS TTESC AGAIN.  So the literal
	 * backslash is queued unmarked (and survives canonicalisation), and
	 * the character after it is treated as escaped even though no second
	 * backslash was typed.  That is upstream's, whatever one thinks of
	 * it; recorded here so a future reader meets the behaviour rather
	 * than the intention.
	 */
	drvreset();
	drvinput("\\\\z\n", 4);
	n = readline(buf, sizeof buf);
	show("dblesc", buf, n);

	/* ================= TANDEM: back-pressure to the device ============ */
	/*
	 * The flow control that runs the other way.  ^S from the terminal
	 * stops OUTPUT; TANDEM is the discipline noticing its own INPUT queue
	 * filling and sending a stop character back so the sender pauses --
	 * XON/XOFF as seen from the receiving end.
	 *
	 * The threshold is upstream's and derived from ttrinit's own numbers:
	 * (limit + lolimit) / 2, and ttrinit is {..., 600, 60} (ttyld.c:34),
	 * so 330.  Below it, and above ttyhog's hard stop at 512, this arm
	 * never runs -- which is why a test has to send hundreds of bytes
	 * with no newline rather than a line.
	 */
	sg.sg_flags = TANDEM;
	u.u_error = 0;
	stioctl(&tino, TIOCSETP, (caddr_t)&sg);
	printf("tandemerr %d\n", u.u_error);
	drvreset();
	for (n = 0; n < 400; n++)
		drvinput("x", 1);
	printf("tandemcount %d\n", ttq->count);
	printf("tandemblocked %d\n", (tp->t_state & TTBLOCK) != 0);
	printf("tandemstopped %d\n", drvlen);	/* the stop char went out */
	printf("tandemstopc %d\n", drvlen > 0 ? (drvbuf[0] & 0377) : -1);

	/*
	 * And it releases.  ttyinsrv's tail (ttyld.c:285-289) clears TTBLOCK
	 * and sends t_startc once the queue has drained to lolimit -- so the
	 * unblock is the SERVICE procedure's, not the put procedure's, and it
	 * only happens because something read.
	 */
	drvreset();
	drvinput("\n", 1);
	n = readline(big, sizeof big);		/* the WHOLE line, or it leaks */
	printf("tandemread %d\n", n);
	printf("tandemunblocked %d\n", (tp->t_state & TTBLOCK) == 0);
	printf("tandemstartc %d\n", drvlen > 0 ? (drvbuf[0] & 0377) : -1);

	/* ================= outconv's delays, for four real terminals ====== */
	/*
	 * V8 still carried padding for the tty 37, vt05, tn 300 and ti 700 in
	 * 1985.  A carriage return on a tn 300 took longer than the next
	 * character took to arrive, so the discipline emits an M_DELAY the
	 * driver turns into silence.  CR1 selects it and the count is 5
	 * (ttyld.c:466-468); the algorithm lives in bits 12-13, which is why
	 * the flag word is worth more than a boolean.
	 *
	 * No CRMOD here, or the \r would be manufactured rather than written.
	 */
	sg.sg_flags = CR1;
	u.u_error = 0;
	stioctl(&tino, TIOCSETP, (caddr_t)&sg);
	printf("crdelayerr %d\n", u.u_error);
	drvreset();
	uwrite("\r", 1);
	printf("crdelayn %d\n", drvdelay);
	printf("crdelayval %d\n", drvdelayval);
	printf("crdelaycol %d\n", tp->t_col);

	/* A negative control: the same \r with the delay bits clear emits no
	 * M_DELAY at all, so the case above is measuring the algorithm and
	 * not merely the presence of a carriage return. */
	sg.sg_flags = 0;
	u.u_error = 0;
	stioctl(&tino, TIOCSETP, (caddr_t)&sg);
	drvreset();
	uwrite("\r", 1);
	printf("nodelayn %d\n", drvdelay);

	/* ---- the other three delay algorithms, and max()'s ONLY caller ---- */
	/*
	 * `max()' was written for this import -- it was the one name ttyld.c
	 * needed that the shim did not have, and writing it is what found
	 * `min()' misdeclared in two files.  It has exactly ONE call site in
	 * the entire tree, ttyld.c:439, inside the tty 37 newline delay, and
	 * until this case nothing had ever executed it.
	 *
	 * Two lines, because max() has two branches and one input decides
	 * which: count is `max(t_col>>4 + 3, 6)', so a short line takes the
	 * constant and a long one takes the computed value.  `abc' gives
	 * max(3, 6) = 6; 64 characters give max(4+3, 6) = 7.
	 */
	sg.sg_flags = NL1;		/* no CRMOD: a bare \n, not a \r\n */
	u.u_error = 0;
	stioctl(&tino, TIOCSETP, (caddr_t)&sg);
	drvreset();
	uwrite("abc\n", 4);
	printf("nl1short %d\n", drvdelayval);
	drvreset();
	uwrite("0123456789012345678901234567890123456789012345678901234567890123\n", 65);
	printf("nl1long %d\n", drvdelayval);

	/*
	 * The tab delay, which is the arm outconv reaches when TBDELAY names
	 * an algorithm rather than XTABS.  `1 - (t_col | ~07)' at column 0 is
	 * 1 - (-8) = 9, and the tab still moves the column to 8.
	 */
	sg.sg_flags = TAB1;
	u.u_error = 0;
	stioctl(&tino, TIOCSETP, (caddr_t)&sg);
	drvreset();
	uwrite("\t", 1);
	printf("tab1delay %d\n", drvdelayval);
	printf("tab1col %d\n", tp->t_col);

	/*
	 * And the vertical one, which is NOT reached by a vertical tab.
	 * partab.c:12 gives 013 (VT) class 1, `non-printing'; it is 014, FORM
	 * FEED, that is class 5.  So the flag spelled VTDELAY is a form-feed
	 * delay -- 127 ticks, the longest in the file, because a printer
	 * ejecting a page is the slowest thing a terminal does.
	 */
	sg.sg_flags = VTDELAY;
	u.u_error = 0;
	stioctl(&tino, TIOCSETP, (caddr_t)&sg);
	drvreset();
	uwrite("\013", 1);			/* VT: class 1, no delay */
	printf("vtnone %d\n", drvdelay);
	drvreset();
	uwrite("\014", 1);			/* FF: class 5 */
	printf("ffdelay %d\n", drvdelayval);

	/* ============ ttyhog, and canonb overflow ========================= */
	/*
	 * Two limits inside the read path that a normal line never reaches.
	 *
	 * ttyhog (ttyld.c:176) is the older one and the ruder: once the read
	 * queue holds 512, a character that is not a newline is REPLACED by
	 * \007 and never queued -- so the terminal beeps at you instead of
	 * accepting more, and the only thing that still gets through is the
	 * newline that would end the line.  ECHO is on, so what the driver
	 * receives is the bell.
	 */
	sg.sg_erase = CERASE;
	sg.sg_kill  = CKILL;
	sg.sg_flags = ECHO;
	u.u_error = 0;
	stioctl(&tino, TIOCSETP, (caddr_t)&sg);
	drvreset();
	for (n = 0; n < 600; n++)
		drvinput("y", 1);
	printf("hogcount %d\n", ttq->count);
	for (n = 0, i = 0; i < drvlen; i++)
		if ((drvbuf[i] & 0377) == 0007)
			n++;
	printf("hogbells %d\n", n > 0);
	printf("hogcapped %d\n", ttq->count < 600);

	/*
	 * And canonb: ttyinsrv gathers into a 256-byte buffer and flushes at
	 * CANBSIZ-1, so the ~498 characters ttyhog let through cross it at
	 * least twice.  WHAT IS ASSERTED IS THAT THE READER CANNOT TELL --
	 * one read returns the whole line, because stread loops on the
	 * DELIMITER rather than on a message boundary, so an internal buffer
	 * smaller than the line is invisible from outside.
	 *
	 * Two drafts of this case were wrong before the measurement.  The
	 * first expected `more than 255 bytes in one read'; the second, after
	 * seeing 145, concluded the line arrives in PIECES and asserted that
	 * -- and 145 was not a piece at all, it was the TANDEM case's
	 * unread remainder leaking in.  With readline() closing that hole the
	 * answer is 498 in a single read.  Both wrong guesses came from
	 * reasoning about the buffer instead of asking it.
	 */
	drvreset();
	drvinput("\n", 1);
	n = readline(big, sizeof big);
	printf("canonbtotal %d\n", n);
	printf("canonbonepiece %d\n", lastreads == 1);
	printf("canonbend %d\n", n > 0 ? (big[n-1] & 0377) : -1);

	/* ============ two arms only the DEVICE can originate ============== */
	/*
	 * ttyldin's switch has five non-data arms and the driver has been
	 * sending only M_IOCTL and M_BREAK up.  These are the other two.
	 *
	 * M_DELIM is dropped on the floor (`freeb(bp); return'), because a
	 * device has no business telling a canonical discipline where a line
	 * ends -- that is the discipline's own judgement.  So the assertion
	 * is that nothing came out, which needs the block count to show the
	 * message was consumed rather than forwarded.
	 */
	sg.sg_flags = ECHO|CRMOD;
	u.u_error = 0;
	stioctl(&tino, TIOCSETP, (caddr_t)&sg);
	drvreset();
	putctl(ttq, M_DELIM);
	drvinput("ok\n", 3);
	n = readline(big, sizeof big);
	show("delimdropped", big, n);

	/*
	 * And M_IOCACK, which goes straight THROUGH to the stream head
	 * (ttyld.c:105-107, shared with M_IOCNAK and M_HANGUP).  Nothing in
	 * this port had sent one upward: every ack so far has travelled the
	 * other way, from the driver in reply to a request.  Here the device
	 * volunteers one, and stioctl is not waiting for it -- streamio.c's
	 * strput takes the `(stp->flag&IOCWAIT)==0' branch and frees it,
	 * which is the case that proves an unsolicited ack cannot corrupt the
	 * next real one.
	 */
	{
		struct block *bp;

		if ((bp = allocb(4)) != NULL) {
			bp->type = M_IOCACK;
			putq(ttq, bp);
			qenable(ttq);
		}
		/* the next real ioctl must still work */
		sg.sg_flags = ECHO|CRMOD;
		u.u_error = 0;
		stioctl(&tino, TIOCSETP, (caddr_t)&sg);
		printf("afterstrayack %d\n", u.u_error);
	}

	/*
	 * ttyosrv's M_FLUSH arm, and the note that used to stand here was
	 * WRONG ABOUT HOW TO REACH IT.  It said the arm "needs TIOCFLUSH,
	 * which stioctl handles itself", as though handling it were the
	 * obstacle.  It is the mechanism: streamio.c:594 is
	 * `putctl(stq->wrq->next, M_FLUSH)', and stq->wrq->next IS ttyld's
	 * write queue -- so the block lands on it, ttyosrv runs, flushes, and
	 * passes the M_FLUSH down to the device.  Accurate citation, opposite
	 * conclusion; the same shape as the constraint that blocked the inode
	 * fix for months.
	 */
	drvreset();
	drvinput("junk", 4);		/* something for the flush to discard */
	u.u_error = 0;
	stioctl(&tino, TIOCFLUSH, (caddr_t)0);
	printf("flusherr %d\n", u.u_error);
	printf("flushtodev %d\n", drvflush);
	/*
	 * What is asserted is that the arm was REACHED, not that its flushq
	 * discarded anything -- and the difference is worth saying out loud
	 * rather than dressing up.  ttyosrv drains its own write queue
	 * eagerly, so there is normally nothing on it for the M_FLUSH arm to
	 * throw away; making the flushq observable would need the queue
	 * backed up behind a QFULL device, which is machinery this case does
	 * not have.  A mutation deleting `flushq(q, 0)' would NOT turn this
	 * red.  Recorded so the next reader does not mistake the case for
	 * more than it is.
	 */

	/* ============ the ack the driver must SHORTEN, and the NAK ======== */
	/*
	 * TIOCHPCL is in neither switch -- not stioctl's, not ttldioc's -- so
	 * ttldioc's default arm passes it down and the driver is the thing
	 * that has to answer.  cons.c:64-67 answers with an M_IOCNAK carrying
	 * no payload byte, and streamio.c:803-809 turns exactly that into
	 * ENOTTY.  A driver that acked everything would report success for a
	 * command nothing implements.
	 */
	u.u_error = 0;
	stioctl(&tino, TIOCHPCL, (caddr_t)0);
	printf("nakerr %d\n", u.u_error);
	printf("naked %d\n", drvnak);

	/*
	 * AND THE LENGTH OF AN ACK IS PART OF THE ACK, which is testable only
	 * against a page boundary.  stioctl builds every M_IOCTL 20 bytes
	 * long, ttldioc's TIOCSETP arm does not touch wptr, and
	 * streamio.c:793-798 copies `wptr - rptr' back -- so an unshortened
	 * ack writes 16 bytes into a 6-byte struct sgttyb.  Ten bytes past
	 * the caller's object, on every set.
	 *
	 * A VALUE SENTINEL CANNOT SEE IT: the ten bytes written are the ten
	 * bytes copyin read from that same address moments earlier, so the
	 * write round-trips and every byte in memory ends up correct.  The
	 * only observable is the fault, so the case arranges one -- sg at the
	 * last six bytes of a writable page, with the next page READABLE but
	 * not writable.  Readable matters: copyin's authentic 20-byte
	 * over-READ must still succeed, so that what faults is the write and
	 * nothing else.
	 *
	 * In a child, because the failure is a signal.  Remove
	 * `bp->wptr = bp->rptr' from the driver and this goes red.
	 */
	{
		char *pg;
		long psz = (long)getpagesize();
		struct sgttyb *gsg;
		int st;
		pid_t kid;

		pg = (char *)mmap((void *)0, (size_t)(2*psz),
		    PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
		if (pg == MAP_FAILED)
			printf("guardsig MMAPFAILED\n");
		else if (mprotect(pg + psz, (size_t)psz, PROT_READ) != 0)
			printf("guardsig MPROTECTFAILED\n");
		else {
			gsg = (struct sgttyb *)(pg + psz - sizeof(struct sgttyb));
			gsg->sg_ispeed = gsg->sg_ospeed = 0;
			gsg->sg_erase = CERASE;
			gsg->sg_kill  = CKILL;
			gsg->sg_flags = ECHO|CRMOD;
			fflush(stdout);
			if ((kid = fork()) == 0) {
				alarm(10);	/* never inherit a hang */
				u.u_error = 0;
				stioctl(&tino, TIOCSETP, (caddr_t)gsg);
				_exit(u.u_error ? 1 : 0);
			}
			if (kid < 0)
				printf("guardsig FORKFAILED\n");
			else {
				waitpid(kid, &st, 0);
				printf("guardsig %d\n", st & 0177);
				printf("guardexit %d\n", (st >> 8) & 0377);
			}
			munmap((void *)pg, (size_t)(2*psz));
		}
	}

	/* --- back to canonical for the last three cases ------------------- */
	sg.sg_flags = ECHO|CRMOD;
	u.u_error = 0;
	stioctl(&tino, TIOCSETP, (caddr_t)&sg);

	/* ================= ttldioc from the DEVICE side (fromdev 1) ======= */
	/*
	 * The other arm, and the only one a driver can reach: an M_IOCTL sent
	 * UP arrives at ttyldin, which calls ttldioc(WR(q), bp, q, 1).  With
	 * fromdev set every arm ends in `qreply(rdq, bp)' -- back DOWN to the
	 * device as an M_IOCACK -- so a modem that asks the discipline what
	 * the line settings are gets its answer without the process ever
	 * being involved.  Nothing in this port had ever taken that arm.
	 */
	{
		struct block *bp;
		union stmsg *m;

		drvreset();
		if ((bp = allocb(sizeof(union stmsg))) != NULL) {
			m = (union stmsg *)bp->wptr;
			m->ioc0.com = TIOCGETP;
			bp->wptr += sizeof(union stmsg);
			bp->type = M_IOCTL;
			qreply(drvwq, bp);
			printf("devioc %d\n", drvack);
		} else
			printf("devioc ALLOCFAILED\n");
	}

	/* --- M_BREAK in canonical mode is an interrupt -------------------- */
	/*
	 * ttyldin's M_BREAK arm: not RAW, so it is `ttysig(q, SIGINT)' and
	 * the block is freed.  A line break and a DEL key reach the same
	 * place by different routes, which is a claim about the switch at
	 * the top of ttyldin rather than about signals.
	 */
	sigint = 0;
	drvreset();
	putctl(ttq, M_BREAK);
	printf("breaksig %d\n", sigint);

	/* --- M_HANGUP is passed up, not consumed -------------------------- */
	/*
	 * The last arm of that switch, and it goes THROUGH: ttyldin forwards
	 * M_HANGUP to q->next, the stream head sets HUNGUP.  It is last here
	 * because it poisons the stream -- every ioctl after it is ENXIO.
	 */
	sp->pgrp = 0;			/* no SIGHUP: sioprobe.c owns that case */
	putctl(ttq, M_HANGUP);
	printf("hungup %d\n", (sp->flag & HUNGUP) != 0);

	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);

	/*
	 * Close it, so the tty[] slot goes back and the exhaustion case that
	 * follows still measures NTTY rather than NTTY-1.  ttyclose zeroes
	 * t_state, which is the whole of releasing a slot.
	 */
	stclose(&tino, 0);
	printf("stclosed %d\n", (tp->t_state & TTUSE) == 0);
}

main()
{
	struct queue *q, *q2;
	struct ttyld *tp;
	int i, r, nopen, firstfail, negative;

	setvbuf(stdout, (char *)0, _IONBF, 0);
	v8k_streaminit();
	v8k_procinit();

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

	/* ==================================================================
	 * THE REAL STACK.  Everything from here to the exhaustion case runs
	 * against stream head / ttyld / driver, built the way init.c builds
	 * it, and it is the half step 1b could not reach.
	 * ================================================================== */
	traffic();

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
