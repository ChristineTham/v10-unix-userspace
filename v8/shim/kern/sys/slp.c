/*
 * tsleep and wakeup -- the name that decided whether streamio.c could be
 * imported at all.
 *
 * Named after sys/slp.c, where V8 keeps them.  This is the machine-dependent
 * half of the syscall side of the stream machinery, in the same relationship
 * dev/machdep.c has to dev/stream.c.  RAW SYSCALLS ONLY, like the rest of
 * shim/ outside libkmemu.
 *
 * ---------------------------------------------------------------------------
 * WHAT tsleep MEANS WHEN THE SHIM IS PER-BINARY
 *
 * src/sys/PORTING.md has the survey; this is the conclusion it reached, and
 * the reason it is milder than the question sounded.  Three facts, all
 * measured against streamio.c before a line of this was written:
 *
 *  1. EVERY tsleep SITS INSIDE A CONDITION RE-TEST LOOP.  stopen:51 is
 *     `while (sp->flag&STWOPEN) { tsleep(...) }'; stread:217 is
 *     `for (;;) { ... if (getq(...) == NULL) { ... tsleep(...); continue; } }'.
 *     That is ordinary kernel discipline: sleep/wakeup is ADVISORY, and a
 *     tsleep that returns without the condition holding is harmless because
 *     the caller loops.  So this function is not obliged to reproduce "block
 *     until exactly this channel is signalled".  It is obliged not to spin and
 *     not to miss a wakeup.
 *
 *  2. EVERY wakeup THAT CAN RELEASE A SLEEPER IS IN streamio.c ITSELF, and all
 *     nine are in four functions -- five in strput, one in stwsrv, and one
 *     self-wake each in stopen and stioctl.  strdata = { strput, ... } and
 *     stwdata = { nulldev, stwsrv, ... } register the first two as the stream
 *     head's own qinit procedures, so they are reached by putnext and by
 *     queuerun(), NOT by an independent thread.
 *
 *  3. THE ENGINE CALLS NEITHER.  src/sys/dev/stream.c contains no tsleep and
 *     no wakeup; it is pure message passing and never blocks.
 *
 * So the only producer that is genuinely another process is the DRIVER AT THE
 * BOTTOM OF THE STACK -- and what sits at the bottom of a stack is a question
 * PLAN.md section 8a step 2 already answered for filesystems: the host.  A V8
 * stream's driver end is a host descriptor, so
 *
 *	tsleep = queuerun(), then poll() the descriptors standing in for the
 *	         devices that would have interrupted
 *
 * and that is FAITHFUL rather than a semantic change.  In the kernel tsleep
 * waits for the driver to interrupt; here it waits for the fd standing in for
 * the driver.  dev/machdep.c already had the first half -- splx() runs
 * queuerun() when the level returns to 0 -- and its comment anticipated this.
 *
 * ---------------------------------------------------------------------------
 * wakeup IS NOT A NO-OP, AND MAKING IT ONE WOULD HAVE THROWN AWAY THE ANSWER
 *
 * The survey said wakeup would be a no-op, because there is no second thread
 * to release.  That is true of what it must DO and false of what it is worth.
 * Writing it as an empty function leaves tsleep with a question it cannot
 * answer: queuerun() has just run some service procedures -- did any of them
 * produce anything this sleeper cares about?
 *
 * A counter answers it exactly.  Every wakeup in streamio.c is on the path
 * where a producer has just made progress, so incrementing a counter and
 * having tsleep compare it across queuerun() IS the wakeup, reduced to the
 * one bit a single-threaded kernel can use.  No polling of queue state, no
 * guessing, and the channel argument stays unread for the honest reason: with
 * one sleeper there is nothing to distinguish.
 */

#include <poll.h>

#include "../../v8sys/rawsys.h"
#include "../h/param.h"

#undef longjmp			/* param.h aims it at v8k_longjmp, which is
				 * defined below; this file owns both names */

#include "../../../src/sys/h/stream.h"
#include "../h/proc.h"
#include "../../../src/sys/h/dir.h"	/* struct direct, for user.h's u_dent */
#include "../h/user.h"		/* jmp_buf, u, and <errno.h> for EINTR */

/*
 * V8's setjmp/longjmp, declared here rather than in a header.
 *
 * src/include/setjmp.h is a typedef and nothing else -- V8's declares no
 * functions, because in 1985 an undeclared one returned int and that was
 * correct for both.  Declaring them in shim/kern/h/user.h instead would put
 * `void longjmp(jmp_buf, int)' in the path of param.h's
 * `#define longjmp v8k_longjmp', which would give v8k_longjmp two parameters
 * in every file that includes user.h and one in this one.
 *
 * returns_twice is written out rather than left to clang's recognition of the
 * name.  Without it the compiler may keep a value live in a register across
 * the setjmp and restore a stale copy after the longjmp, which is the kind of
 * bug that appears only under optimisation and only sometimes.
 */
extern int	setjmp(jmp_buf) __attribute__((returns_twice));
extern void	longjmp(jmp_buf, int);

void queuerun(void);

#define	V8K_NDRVFD	16	/* how many devices can hang off this process */

/*
 * A registered driver descriptor IS a registered interrupt handler, and
 * saying so is the point of the isr argument.
 *
 * On the VAX a stream driver's bottom end was a device that raised an
 * interrupt; the handler read the silo and putnext()ed a block up the stream.
 * Here the device is a host descriptor and the interrupt is poll() reporting
 * it ready -- so the thing a driver must register is not "an fd to watch" but
 * "an fd, and what to run when it is ready".  Registering only the fd would
 * make tsleep return TS_OK to a caller that then finds its queue still empty,
 * because nobody read the device: a spin, which is the one thing fact 1 above
 * says this function may not do.
 */
struct drvfd {
	int	fd;
	void	(*isr)(int, void *);
	void	*arg;
};

static struct drvfd	drvfd[V8K_NDRVFD];
static int		ndrvfd;

static volatile int	wakecnt;	/* see the header comment */

int
v8k_drvfd(int fd, void (*isr)(int, void *), void *arg)
{
	int i;

	for (i = 0; i < ndrvfd; i++)
		if (drvfd[i].fd == fd) {	/* re-registration replaces */
			drvfd[i].isr = isr;
			drvfd[i].arg = arg;
			return (0);
		}
	if (ndrvfd >= V8K_NDRVFD)
		return (-1);
	drvfd[ndrvfd].fd = fd;
	drvfd[ndrvfd].isr = isr;
	drvfd[ndrvfd].arg = arg;
	ndrvfd++;
	return (0);
}

int
v8k_drvclose(int fd)
{
	int i;

	for (i = 0; i < ndrvfd; i++)
		if (drvfd[i].fd == fd) {
			drvfd[i] = drvfd[--ndrvfd];
			return (0);
		}
	return (-1);
}

int
v8k_ndrvfd(void)
{
	return (ndrvfd);
}

/*
 * wakeup -- record that a producer ran.  See the header comment.
 */
void
wakeup(caddr_t chan)
{
	(void)chan;
	wakecnt++;
}

/*
 * tsleep(chan, pri, seconds) -- upstream sys/slp.c:93.
 *
 * The third argument is SECONDS, not ticks: upstream stores it in
 * proc.p_tsleep and sys/clock.c:315 decrements it once per second.  Measured
 * rather than assumed, because getting it wrong would turn stioctl's
 * fifteen-second ack timeout into fifteen clock ticks and every ioctl through
 * a module into an EIO.
 *
 * pri is unread.  On the VAX it chose a run-queue and decided whether the
 * sleep was interruptible; here there is one process, and interruptibility is
 * whatever the host does to a poll() when a signal is delivered -- which is
 * the right answer rather than an approximation of one.
 */
int
tsleep(caddr_t chan, int pri, int seconds)
{
	struct pollfd pfd[V8K_NDRVFD];
	int before, i, n, timeout;

	(void)chan;
	(void)pri;

	/*
	 * Anything already in the stream, first.  This is the in-stream
	 * producer -- a module's service procedure that putnext()ed a block
	 * up to strput, which called wakeup.  If it ran, we are already
	 * awake and the caller's re-test loop will find its message.
	 */
	before = wakecnt;
	queuerun();
	if (wakecnt != before)
		return (TS_OK);

	timeout = seconds > 0 ? seconds * 1000 : -1;

	/*
	 * NO DEVICE BELOW AND NO TIMEOUT IS A DEADLOCK, AND IT PANICS.
	 *
	 * In the kernel tsleep(chan, pri, 0) blocks until somebody calls
	 * wakeup(chan).  Here the set of somebodies is empty and provably so:
	 * nothing outside this process can change the stream, because no
	 * device is registered, and queuerun() has just established that
	 * nothing inside it will either.  Returning TS_OK would spin, TS_TIME
	 * would invent a timeout nobody asked for, and TS_SIG would invent a
	 * signal.  poll(NULL, 0, -1) would simply hang, which is the same
	 * mistake with no message attached.
	 *
	 * The case is not reachable from a stream the shim itself opens --
	 * those have a host fd at the bottom, which is the whole design above.
	 * It is reachable from a stream whose bottom module is pure software
	 * and produces nothing: a test's mistake, or a driver that forgot
	 * v8k_drvfd().  The configuration is wrong one frame up, and this is
	 * where it becomes visible.
	 *
	 * A TIMEOUT with no device is NOT an error, so it falls through to the
	 * poll below with nfds 0 -- which is the classic portable sleep, and
	 * one syscall rather than two.  (Darwin has no SYS_nanosleep to reach
	 * for: nanosleep(3) there is built on __semwait_signal.)  Every timed
	 * caller in streamio.c handles the TS_TIME that comes back --
	 * istread:302 and istwrite:490 return -1, stioctl:760 gives up on the
	 * ack.
	 */
	if (ndrvfd == 0 && timeout < 0)
		panic("tsleep: no device below, and no timeout\n");

	for (i = 0; i < ndrvfd; i++) {
		pfd[i].fd = drvfd[i].fd;
		pfd[i].events = POLLIN | POLLPRI;
		pfd[i].revents = 0;
	}
	n = (int)rawsys3(SYS_poll, (long)(ndrvfd ? pfd : 0), (long)ndrvfd,
	    (long)timeout);
	if (n < 0)
		return (TS_SIG);	/* EINTR: a signal arrived, which is
					 * precisely what TS_SIG means */
	if (n == 0)
		return (TS_TIME);

	/*
	 * Run the handlers, then the queues they fed.  Two passes, in that
	 * order, because an isr putnext()s into the stream and the service
	 * procedures that carry the block up to the head run from queuerun().
	 *
	 * The isr list is walked by index against a snapshot count: an isr
	 * that closes its own device calls v8k_drvclose(), which compacts the
	 * array from the end.  Re-reading ndrvfd each iteration would then
	 * skip the entry that moved into the hole.
	 */
	{
		int nfd = ndrvfd;

		for (i = 0; i < nfd && i < ndrvfd; i++)
			if (pfd[i].revents && drvfd[i].fd == pfd[i].fd &&
			    drvfd[i].isr)
				(*drvfd[i].isr)(pfd[i].fd, drvfd[i].arg);
	}
	queuerun();
	return (TS_OK);
}

/*
 * The kernel's one-argument longjmp.  param.h redirects the name; the reason
 * is there.  Returning 1 into the matching setjmp is what V8's does.
 */
void
v8k_longjmp(jmp_buf env)
{
	longjmp(env, 1);
}

/*
 * v8k_stcall -- the setjmp half, which upstream keeps in sys/trap.c:176.
 *
 * streamio.c aborts a system call by longjmp(u.u_qsav) when a signal arrives
 * mid-sleep, so SOMETHING has to have done the setjmp.  In V8 that is the
 * system-call dispatcher:
 *
 *	if (setjmp(u.u_qsav)) {
 *		if (u.u_error == 0 && u.u_eosys == JUSTRETURN)
 *			u.u_error = EINTR;
 *	} else
 *		(*(callp->sy_call))();
 *
 * This is that, with the u_eosys clause dropped because nothing here restarts
 * a system call -- RESTARTSYS is set by the VAX trap handler for a page fault
 * during argument fetch, which has no counterpart.
 *
 * A THUNK RATHER THAN A VARIADIC CALL.  The seven entry points have four
 * different signatures (stopen takes four arguments and returns a pointer,
 * stread takes one and returns nothing), and calling through a pointer with
 * more arguments than the callee declares happens to work in this ABI while
 * being undefined in the language.  This side of the seam is modern C, so it
 * gets the modern idiom: two lines of caller-side thunk, and no lie about
 * types.
 */
int
v8k_stcall(void (*fn)(void *), void *arg)
{
	u.u_error = 0;
	if (setjmp(u.u_qsav)) {
		if (u.u_error == 0)
			u.u_error = EINTR;
	} else
		(*fn)(arg);
	return (u.u_error);
}
