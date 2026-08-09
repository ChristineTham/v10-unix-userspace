/*
 * A probe for V8's stream SYSCALL layer -- src/sys/sys/streamio.c, PLAN.md
 * section 8a step 1.
 *
 * The companion to probe.c, and split from it for the reason the source is
 * split: probe.c exercises dev/stream.c, the message-passing ENGINE, which
 * knows nothing about processes.  This one exercises the layer above, where a
 * system call meets a stream -- stopen, stread, stwrite, stioctl, stclose, the
 * module stack and file passing.  That layer is the one with a u-area, a proc
 * entry, a file table and tsleep behind it, so a failure here is a failure of
 * shim/kern/sys/ far more often than of Bell Labs' code.
 *
 * Prints `key value' lines; run.sh asserts on them and keeps the reason for
 * each next to the assertion.
 *
 * See shim/kern/h/param.h for why the includes are in this order.
 */

#include "../../shim/kern/h/param.h"

/*
 * param.h's redirects are undone in ONE place now -- there are thirteen of
 * them since §8a step 5 and a copied list decays.  shim/kern/h/hostok.h says why.
 */
#include "../../shim/kern/h/hostok.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "../../src/sys/h/stream.h"
#include "../../src/sys/research/sparam.h"
#include "../../shim/kern/h/proc.h"
#include "../../src/sys/h/dir.h"	/* struct direct, for user.h's u_dent */
#include "../../shim/kern/h/user.h"
#include "../../src/sys/h/inode.h"
#include "../../src/sys/h/file.h"
#include "../../src/sys/h/ioctl.h"
#include "../../shim/kern/h/conf.h"

extern struct qinit	strdata;
extern struct block	*qfreelist[4];
extern int		nselcoll;

/*
 * K&R declarations, matching the ones stream.h already makes -- probe.c says
 * why a modern prototype here would be a CONFLICTING type rather than a
 * stricter one.  v8k_falloc is ours and gets a real one.
 */
int	qreply(), putq(), freeb(), putctl(), flushq();
struct block *allocb();
struct file *v8k_falloc(struct inode *ip, int flag);

/* ------------------------------------------------------------------------
 * A loopback driver.  Whatever is written down the stream comes straight back
 * up, which is the smallest thing that makes a stream head observable without
 * a device.  A real driver would read a host descriptor; the pipe driver
 * further down does exactly that.
 */
static int	seencount;	/* stp->count as seen from INSIDE the stream */
static struct inode ino;

static int
loopput(struct queue *q, struct block *bp)
{
	if (ino.i_sptr)
		seencount = ino.i_sptr->count;
	qreply(q, bp);
	return (0);
}

static long loopopen(struct queue *q, int dev)  { return (1); }
static int  loopclose(struct queue *q)          { return (0); }

static struct qinit looprd = { putq,    0, loopopen, loopclose, 512, 256 };
static struct qinit loopwr = { loopput, 0, loopopen, loopclose, 512, 256 };
static struct streamtab loopinfo = { &looprd, &loopwr };

/*
 * A pass-through line discipline, for FIOPUSHLD / FIOPOPLD / FIOLOOKLD.  Those
 * three are the module stack, which src/sys/PORTING.md named as the part of
 * the file tests/streams could not reach before this import.
 */
static int  ldput(struct queue *q, struct block *bp)
	{ (*q->next->qinfo->putp)(q->next, bp); return (0); }
static long ldopen(struct queue *q, int dev)  { return (1); }
static int  ldclose(struct queue *q)          { return (0); }
static struct qinit ldrd = { putq,  0, ldopen, ldclose, 512, 256 };
static struct qinit ldwr = { ldput, 0, ldopen, ldclose, 512, 256 };
static struct streamtab ldinfo = { &ldrd, &ldwr };

/* A second one, so FIOLOOKLD has to pick the right entry rather than the
 * only entry. */
static struct qinit ld2rd = { putq,  0, ldopen, ldclose, 512, 256 };
static struct qinit ld2wr = { ldput, 0, ldopen, ldclose, 512, 256 };
static struct streamtab ld2info = { &ld2rd, &ld2wr };

static int
freecount(int class)
{
	struct block *bp;
	int n = 0;

	for (bp = qfreelist[class]; bp; bp = bp->next)
		n++;
	return (n);
}

/* stwrite/stread through the u-area, the way a system call would drive them. */
static void
uwrite(char *s, int n)
{
	u.u_base = s;
	u.u_count = n;
	u.u_offset = 0;
	u.u_error = 0;
	stwrite(&ino);
}

static int
uread(char *buf, int n)
{
	u.u_base = buf;
	u.u_count = n;
	u.u_offset = 0;
	u.u_error = 0;
	stread(&ino);
	return (n - (int)u.u_count);
}

/* ------------------------------------------------------------------------
 * The pipe driver: a real host descriptor at the bottom of a stream, which is
 * what makes tsleep's poll() path reachable.  Its `interrupt handler' reads
 * the descriptor and sends the bytes up, exactly as a tty driver's would.
 */
static int	pipefd[2];
static struct queue *pipewq;	/* the driver's write queue, for qreply */

static void
pipeisr(int fd, void *arg)
{
	struct block *bp;
	char buf[64];
	long n;

	n = read(fd, buf, sizeof buf);
	if (n <= 0)
		return;
	if ((bp = allocb((int)n)) == NULL)
		return;
	bp->type = M_DATA;
	memcpy(bp->wptr, buf, (size_t)n);
	bp->wptr += n;
	qreply(pipewq, bp);
}

int
main(void)
{
	char buf[128];
	struct inode *r;
	struct stdata *sp;
	struct queue *q;
	struct file *fp;
	struct block *bp;
	struct insld ld;
	struct passfd pfd;
	struct kpassfd *kp;
	int free4, free16, free64, freebig;
	int nld, pg, i, rc;
	long sentinel;

	v8k_streaminit();
	v8k_procinit();

	free4 = freecount(0); free16 = freecount(1);
	free64 = freecount(2); freebig = freecount(3);

	/* --- stopen ------------------------------------------------------ */
	ino.i_count = 1;
	ino.i_number = 42;
	ino.i_dev = 7;
	ino.i_sptr = NULL;
	r = (struct inode *)stopen(&loopinfo, 0, 0, &ino);
	printf("openret %d\n", r == NULL);
	printf("openerr %d\n", u.u_error);
	printf("attached %d\n", ino.i_sptr != NULL);
	sp = ino.i_sptr;
	printf("opencount %d\n", sp->count);
	printf("openpgrp %d\n", sp->pgrp);

	/* --- write and read a record ------------------------------------- */
	seencount = -1;
	uwrite("hello", 5);
	printf("writeerr %d\n", u.u_error);
	/* Hazard 3: stenter() incremented count on the way in, and the
	 * driver's put procedure runs while the process is still inside. */
	printf("insidecount %d\n", seencount);
	printf("outsidecount %d\n", sp->count);

	buf[0] = 0;
	i = uread(buf, sizeof buf);
	buf[i] = 0;
	printf("readn %d\n", i);
	printf("readbuf %s\n", buf);
	printf("readerr %d\n", u.u_error);

	/* --- FIONREAD ----------------------------------------------------- */
	uwrite("abcd", 4);
	nld = -1;
	u.u_error = 0;
	stioctl(&ino, FIONREAD, (caddr_t)&nld);
	printf("fionread %d\n", nld);
	printf("fionreaderr %d\n", u.u_error);

	/*
	 * FIONREAD with a null argument.  streamio.c:562 is the ONE copyout in
	 * stioctl with no null check on arg, because on a VAX a copyout to
	 * user address 0 landed in read-only text, faulted, and came back -1.
	 * Reproducing the ANSWER rather than the absence of the fault is why
	 * shim/kern/sys/subr.c's copyout rejects NULL.
	 */
	u.u_error = 0;
	stioctl(&ino, FIONREAD, (caddr_t)0);
	printf("fionreadnull %d\n", u.u_error);

	i = uread(buf, sizeof buf);		/* drain */
	buf[i] = 0;
	printf("drained %s\n", buf);

	/* --- TIOCSPGRP / TIOCGPGRP ---------------------------------------- */
	u.u_error = 0;
	stioctl(&ino, TIOCSPGRP, (caddr_t)0);	/* arg 0: adopt this process */
	printf("spgrp %d\n", sp->pgrp == u.u_procp->p_pid);
	printf("ttydev %d\n", u.u_ttydev == ino.i_dev);
	printf("ttyino %d\n", (long)u.u_ttyino == ino.i_number);

	/*
	 * TIOCGPGRP copies sizeof(stq->pgrp) -- TWO bytes -- into a user int,
	 * leaving its top half whatever it was.  The VAX copied two bytes
	 * there too, so this is INHERITED and deliberate; src/sys/PORTING.md
	 * hazard 2 says why it is not the :713 case.  Asserting it keeps a
	 * future "fix" from being silent.
	 */
	pg = 0x7f7f7f7f;
	u.u_error = 0;
	stioctl(&ino, TIOCGPGRP, (caddr_t)&pg);
	printf("gpgrplow %d\n", (pg & 0xffff) == (sp->pgrp & 0xffff));
	printf("gpgrphigh %d\n", (unsigned)(pg & ~0xffff) == 0x7f7f0000u);

	/* --- TIOCEXCL / TIOCNXCL ------------------------------------------ */
	u.u_error = 0;
	stioctl(&ino, TIOCEXCL, (caddr_t)0);
	printf("excl %d\n", (sp->flag & EXCL) != 0);
	stioctl(&ino, TIOCNXCL, (caddr_t)0);
	printf("nxcl %d\n", (sp->flag & EXCL) == 0);

	/* --- the module stack: FIOPUSHLD, FIOLOOKLD, FIOPOPLD -------------- */
	v8k_stunconf();
	printf("ld0 %d\n", v8k_stconf(&ldinfo));
	printf("ld1 %d\n", v8k_stconf(&ld2info));

	ld.ld = 1;			/* the SECOND one, so a right answer
					 * cannot come from an empty search */
	ld.level = 0;
	u.u_error = 0;
	stioctl(&ino, FIOPUSHLD, (caddr_t)&ld);
	printf("pusherr %d\n", u.u_error);
	q = sp->wrq;
	printf("pushed %d\n", q->next->qinfo == &ld2wr);

	/*
	 * FIOLOOKLD with a null argument returns the discipline number in
	 * u.u_r.r_val1, which is the syscall return value.
	 */
	u.u_error = 0;
	u.u_r.r_val1 = -1;
	stioctl(&ino, FIOLOOKLD, (caddr_t)0);
	printf("lookval %d\n", u.u_r.r_val1);
	printf("lookerr %d\n", u.u_error);

	/*
	 * FIOLOOKLD WITH AN ARGUMENT IS THE RECORDED DEVIATION.  Upstream is
	 * `copyout((caddr_t)&fmt, arg, sizeof(arg))' where fmt is int and arg
	 * is caddr_t -- four bytes on a VAX by coincidence, EIGHT here, so it
	 * reads past a 4-byte object and writes 8 bytes to user memory.  The
	 * import changes it to sizeof(fmt); this is what says so.  A sentinel
	 * in the high half must survive.
	 */
	sentinel = 0x5a5a5a5a00000000L;
	u.u_error = 0;
	stioctl(&ino, FIOLOOKLD, (caddr_t)&sentinel);
	printf("lookarg %d\n", (int)(sentinel & 0xffffffff));
	printf("lookhigh %d\n", (unsigned long)sentinel >> 32 == 0x5a5a5a5aUL);

	nld = 0;
	u.u_error = 0;
	stioctl(&ino, FIOPOPLD, (caddr_t)&nld);
	printf("poperr %d\n", u.u_error);
	printf("popped %d\n", sp->wrq->next->qinfo == &loopwr);

	/* An unconfigured discipline number is EINVAL, not a crash. */
	ld.ld = 9;
	ld.level = 0;
	u.u_error = 0;
	stioctl(&ino, FIOPUSHLD, (caddr_t)&ld);
	printf("pushbad %d\n", u.u_error);

	/* --- file passing: FIOSNDFD and FIORCVFD --------------------------- */
	/*
	 * FIOSNDFD on a DEVICE-backed stream is ENXIO, and that is upstream's
	 * answer rather than a gap here.  sndfile (:926) walks the write chain
	 * looking for a queue whose qinfo is &strdata -- a stream HEAD -- and
	 * on a stream with a driver at the bottom there is no second head to
	 * find.  The topology that has one is pipe(2), which sys/pipe.c builds
	 * by cross-connecting two streams, and which this port answers with
	 * the host's pipe instead.
	 */
	fp = v8k_falloc(&ino, FREAD|FWRITE);
	printf("falloc %d\n", fp != NULL);
	u.u_ofile[3] = fp;
	i = 3;
	u.u_error = 0;
	stioctl(&ino, FIOSNDFD, (caddr_t)&i);
	printf("sndfd %d\n", u.u_error);

	/*
	 * FIORCVFD, with the M_PASS block put up the stream the way a driver
	 * would -- qreply from the driver's write queue reaches strput, which
	 * is the stream head's own put procedure.  This is what exercises
	 * ufalloc, u_ofile and the urcvfile caddr_t fix: a stack address on
	 * ARM64 macOS is well above 4 GB, so an `arg' truncated to int would
	 * not survive the copyout below.
	 */
	bp = allocb(sizeof(struct kpassfd));
	bp->type = M_PASS;
	kp = (struct kpassfd *)bp->rptr;
	kp->f.fp = fp;
	kp->uid = 101;
	kp->gid = 202;
	kp->nice = 3;
	bp->wptr += sizeof(struct kpassfd);
	fp->f_count++;
	qreply(sp->wrq->next, bp);

	memset(&pfd, 0, sizeof pfd);
	u.u_error = 0;
	stioctl(&ino, FIORCVFD, (caddr_t)&pfd);
	printf("rcvfderr %d\n", u.u_error);
	printf("rcvfd %d\n", pfd.fd);
	printf("rcvuid %d\n", pfd.uid);
	printf("rcvgid %d\n", pfd.gid);
	printf("rcvslot %d\n", u.u_ofile[pfd.fd] == fp);

	/* --- tsleep over a real host descriptor ---------------------------- */
	/*
	 * The design PLAN.md section 8a step 1 settled: a V8 stream's driver
	 * end is a host descriptor, tsleep is queuerun() then poll() on it,
	 * and wakeup records that a producer ran.  A pipe is the smallest
	 * device that can be driven from one process -- fill it first, then
	 * read, and the read blocks in poll exactly once before the handler
	 * runs.
	 */
	if (pipe(pipefd) == 0) {
		pipewq = sp->wrq->next;
		v8k_drvfd(pipefd[0], pipeisr, 0);
		printf("ndrv %d\n", v8k_ndrvfd());
		write(pipefd[1], "device", 6);
		i = uread(buf, sizeof buf);
		buf[i] = 0;
		printf("drvread %s\n", buf);
		v8k_drvclose(pipefd[0]);
		printf("ndrvafter %d\n", v8k_ndrvfd());
		close(pipefd[0]);
		close(pipefd[1]);
	} else
		printf("drvread PIPEFAILED\n");

	/*
	 * And with no device below, a TIMED sleep is a timeout rather than an
	 * error.  One second, which is tsleep's unit -- upstream stores the
	 * argument in p_tsleep and sys/clock.c:315 decrements it once a
	 * second.  Getting that wrong would turn stioctl's fifteen-second ack
	 * timeout into fifteen ticks.
	 */
	printf("tsleeptime %d\n", tsleep((caddr_t)0, 0, 1));

	/* --- hangup, and what it does to the file table -------------------- */
	/*
	 * An M_HANGUP arriving at the stream head sets HUNGUP, signals the
	 * process group, and calls forceclose() -- which walks file[0] to
	 * fileNFILE and poisons every open file whose inode points at this
	 * stream.  That reach out of the stream and into the process's open
	 * files is the whole reason shim/kern/sys/fio.c has a real table
	 * rather than a stub.
	 */
	sp->pgrp = 0;			/* no SIGHUP: run.sh's hup.c tests that */
	fp->f_flag = FREAD|FWRITE;
	putctl(sp->wrq->next, M_HANGUP);
	printf("hungup %d\n", (sp->flag & HUNGUP) != 0);
	printf("fhungup %d\n", (fp->f_flag & FHUNGUP) != 0);
	/*
	 * WRITE ONLY.  strput's M_HANGUP arm is `forceclose(stp, FWRITE)' --
	 * it poisons writing and leaves READING alone, so a process can still
	 * drain what the device queued before it vanished.  It is stclose that
	 * passes FREAD|FWRITE.  Asserting both halves, because a forceclose
	 * that cleared everything would look correct against the FHUNGUP line
	 * above and would silently discard the last of a terminal's output.
	 */
	printf("fnowrite %d\n", (fp->f_flag & FWRITE) == 0);
	printf("freadkept %d\n", (fp->f_flag & FREAD) != 0);

	/* A write to a hung-up stream is ENXIO -- and SIGPIPE, which the
	 * separate program below checks because it kills this one. */
	signal(SIGPIPE, SIG_IGN);
	uwrite("gone", 4);
	printf("writehung %d\n", u.u_error);

	/* --- teardown, and nothing leaked ---------------------------------- */
	while (u.u_ofile[3]) { closef(u.u_ofile[3]); u.u_ofile[3] = NULL; }
	for (i = 0; i < NOFILE; i++)
		if (u.u_ofile[i]) { closef(u.u_ofile[i]); u.u_ofile[i] = NULL; }
	stclose(&ino, 0);
	printf("closed %d\n", ino.i_sptr == NULL);

	printf("conserved4 %d\n", freecount(0) == free4);
	printf("conserved16 %d\n", freecount(1) == free16);
	printf("conserved64 %d\n", freecount(2) == free64);
	printf("conservedbig %d\n", freecount(3) == freebig);

	/* --- the sizes hazard 4 is about ----------------------------------- */
	printf("ofilesize %d\n", (int)sizeof(u.u_ofile));
	printf("qsavsize %d\n", (int)sizeof(u.u_qsav));
	printf("pidsize %d\n", (int)sizeof(v8k_proc0.p_pid));
	printf("pgrpsize %d\n", (int)sizeof(sp->pgrp));
	printf("pidrange %d\n", v8k_proc0.p_pid > 0 && v8k_proc0.p_pid <= 30000);
	printf("nselcoll %d\n", nselcoll);
	rc = 0;
	return (rc);
}
