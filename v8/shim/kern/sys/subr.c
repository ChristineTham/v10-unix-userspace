/*
 * The twelve mechanical names, and the two ends they have to agree at.
 *
 * Named after sys/subr.c, where V8 keeps nulldev; min and iomove are
 * sys/rdwri.c's, gsignal and psignal sys/sig.c's, selwakeup sys/sys2.c's.
 * Ours, not Bell Labs' -- see shim/kern/h/param.h.
 *
 * shim/kern/sys/fio.c has the three names with a design in them.  These are
 * the rest, and the point of the file is that each is SHORT and each says what
 * upstream's did so the reduction is visible rather than implied.
 */

/*
 * SIGNAL NUMBERS, CHECKED RATHER THAN ASSUMED.
 *
 * shim/kern/h/param.h spells SIGHUP and SIGPIPE itself instead of doing what
 * upstream's param.h:48 does, which is `#include <signal.h>' -- dragging
 * Darwin's sigaction, ucontext and sigset_t through every 1985 K&R kernel
 * object to obtain two integers.  A spelled constant is a claim, so it gets
 * the same treatment src/include/PORTING.md gives a struct that v8cc and clang
 * each read one end of: capture the host's value first, hide the name, then
 * let the header speak and compare.
 *
 * The #undefs are what make the comparison real.  Without them param.h's
 * `#define SIGHUP 1' on top of signal.h's is a benign redefinition -- same
 * token, no diagnostic -- and the assertion below would be comparing the
 * host's value with itself.
 */
#include <signal.h>
enum {
	HOST_SIGHUP  = SIGHUP,  HOST_SIGINT  = SIGINT,
	HOST_SIGQUIT = SIGQUIT, HOST_SIGPIPE = SIGPIPE
};
#undef SIGHUP
#undef SIGINT
#undef SIGQUIT
#undef SIGPIPE

#include "../../v8sys/rawsys.h"
#include "../h/param.h"
#include "../../../src/sys/h/stream.h"
#include "../h/proc.h"
#include "../h/user.h"		/* brings <errno.h> in, as upstream's does */
#include "../h/buf.h"

_Static_assert(SIGHUP == HOST_SIGHUP,
    "SIGHUP must be the host's, because psignal delivers with kill(2)");
_Static_assert(SIGINT == HOST_SIGINT,
    "SIGINT must be the host's, because psignal delivers with kill(2)");
_Static_assert(SIGQUIT == HOST_SIGQUIT,
    "SIGQUIT must be the host's, because psignal delivers with kill(2)");
_Static_assert(SIGPIPE == HOST_SIGPIPE,
    "SIGPIPE must be the host's, because psignal delivers with kill(2)");

/*
 * u_error IS AN ERRNO, AND IT HAS TO BE V8'S.
 *
 * shim/kern/h/user.h takes the codes from the host's <errno.h>, exactly as
 * upstream's user.h:134 takes them from V8's.  That is only correct while the
 * two agree, and the numbers below are V8's own, transcribed from
 * rootfs/usr/include/errno.h -- which is V8's header, the one a ported program
 * compiles against.  They are V7's numbers and neither system renumbered them,
 * so all eleven match; if a future host ever disagrees this is where it says
 * so, rather than a V8 program printing the wrong message for a stream error.
 *
 * Eleven, because that is what this library can produce: the ten streamio.c
 * assigns to u.u_error plus the EMFILE ufalloc returns.
 */
_Static_assert(EINTR == 4 && EIO == 5 && ENXIO == 6 && EBADF == 9 &&
    ENOMEM == 12 && EFAULT == 14 && EINVAL == 22 && ENFILE == 23 &&
    EMFILE == 24 && ENOTTY == 25 && ENOSPC == 28,
    "the host's errno numbers must be V8's; see rootfs/usr/include/errno.h");

int	nselcoll;		/* select collisions; sys/sys2.c's */
int	selwait;		/* the address is the channel, not the value */
long	v8k_hostof(int v8pid);

/*
 * min and max -- upstream sys/rdwri.c:249 and :235, and the types are
 * upstream's, which they were NOT until max arrived and the declaration was
 * read rather than recalled.  This said min had "no declared return type, so
 * int(unsigned, unsigned)"; rdwri.c puts `unsigned' on the line above the
 * name, both times, so both return unsigned.  param.h has the account.
 *
 * Worth writing out rather than "improving" to int(int, int): streamio.c calls
 * min(u.u_count, bp->wptr - bp->rptr), where the first is unsigned and the
 * second is a pointer difference.  On the VAX that difference was 32 bits and
 * the conversion was free; here it is 64 and the conversion truncates --
 * correctly, because a stream block is at most 1024 bytes, and identically to
 * what the VAX did with the same declaration.
 *
 * They are here rather than imported because sys/rdwri.c is the file I/O
 * layer -- readi, writei, iomove -- and taking sixteen lines of arithmetic
 * would mean taking all of it.  Same judgement as the printf/bcopy/uballoc
 * redirections in param.h, and recorded for the same reason: this is our
 * spelling of Bell Labs' function, not Bell Labs' file.
 */
unsigned
min(unsigned a, unsigned b)
{
	if (a < b)
		return (a);
	return (b);
}

unsigned
max(unsigned a, unsigned b)
{
	if (a > b)
		return (a);
	return (b);
}

/*
 * nulldev -- upstream sys/subr.c:212 is `nulldev() { }', which falls off the
 * end and returns whatever was in r0.
 *
 * Returning 0 instead is a deliberate difference and the only one in this
 * file.  This port has already been bitten once by a V8 main() that fell off
 * the end and returned register litter -- tests/crash-probe.sh counted 42 of
 * primes' garbage exit statuses as signal deaths -- so the deterministic
 * non-answer is the better non-answer.
 *
 * THE FIRST VERSION OF THIS NOTE GAVE THE WRONG REASON IT IS SAFE, which is
 * worth keeping because a wrong reason behind a right fix leaves no failing
 * test.  It said "every call site discards the result".  Measured: false.
 * streamio.c:25 puts nulldev in strdata.qopen, and all three qopen call sites
 * -- :70, :120, :645 -- consume what comes back and compare it against NULL
 * and against 1.
 *
 * What actually makes it safe is a topology fact.  strdata is installed on the
 * stream HEAD's read queue (:105), and qopen is only ever invoked on a queue
 * BELOW the head, so strdata.qopen is unreachable.  stwdata.putp and
 * nilw.qclose really are called for effect only.
 *
 * And the live consequence, which the wrong reason hid: NULLDEV IS NOT A
 * USABLE qopen FOR A DRIVER.  A streamtab whose rdinit->qopen is nulldev
 * returns 0 at :120, :124 takes the NULL branch, and stopen fails the open
 * with HUNGUP and ENXIO.  Failing closed and deterministically is right --
 * upstream's would have been unpredictable -- but a driver that wants to open
 * must supply a qopen that returns 1.
 */
int
nulldev(void)
{
	return (0);
}

/*
 * copyin / copyout -- upstream's are VAX assembly (sys/vax/locore.s), moving
 * across the user/kernel boundary with a fault handler armed so a bad user
 * address returns -1 instead of panicking.
 *
 * ONE ADDRESS SPACE, SO THE MOVE IS bcopy.  A V8 program and its shim are the
 * same process here; there is nothing to copy between.  That was the easy half
 * and it is two lines.
 *
 * THE HARD HALF IS THAT THEY MUST STILL BE ABLE TO FAIL.  Six of the nine
 * copyout/copyin sites in stioctl test the result and set EFAULT, and one of
 * them -- FIONREAD at :562 -- has NO null check on arg, because on a VAX a
 * copyout to user address 0 landed in the read-only text segment, faulted, and
 * came back -1.  So `ioctl(fd, FIONREAD, 0)' returned EFAULT there.  A version
 * of copyout here that always succeeded would instead write four bytes to
 * address 0 and take SIGSEGV -- which is CLAUDE.md's address-0 class arriving
 * from the other direction, in code we wrote rather than in code we imported.
 *
 * Fixing to the VAX's ANSWER rather than to the absence of the fault means
 * rejecting the null pointer and returning -1, which is what the fault handler
 * did.  A non-null wild pointer still faults here where the VAX returned -1;
 * that is a real gap, it is the same gap the rest of the shim has, and closing
 * it would mean a signal handler or an address probe on every ioctl.
 */
int
copyin(caddr_t from, caddr_t to, unsigned long n)
{
	if (from == NULL || to == NULL)
		return (-1);
	bcopy(from, to, n);
	return (0);
}

int
copyout(caddr_t from, caddr_t to, unsigned long n)
{
	if (from == NULL || to == NULL)
		return (-1);
	bcopy(from, to, n);
	return (0);
}

/*
 * iomove -- upstream sys/rdwri.c, reproduced including the u_segflg arm.
 *
 * This is the one function here that is a transcription rather than a
 * reduction, because every line of it is still meaningful: it is what moves
 * bytes between a stream block and the user's buffer, and it is what advances
 * u_base/u_offset/u_count so stread's loop terminates.  Getting the direction
 * wrong is a read that returns the caller's own buffer contents.
 *
 * u_segflg is kept even though copyin and copyout are bcopy here, so the two
 * arms are now the same operation.  It stays because it is the field that says
 * WHOSE address u_base is, and a future stream driver doing internal I/O
 * (istread's caller, or a v8fs server) sets it for exactly that reason.  An
 * arm that is currently indistinguishable is not the same thing as an arm that
 * is wrong.
 */
void
iomove(void *vcp, unsigned n, int flag)
{
	caddr_t cp = (caddr_t)vcp;	/* the one cast param.h's void* costs */
	int t;

	if (n == 0)
		return;
	if (u.u_segflg != 1) {
		if (flag == B_WRITE)
			t = copyin(u.u_base, cp, n);
		else
			t = copyout(cp, u.u_base, n);
		if (t) {
			u.u_error = EFAULT;
			return;
		}
	} else if (flag == B_WRITE)
		bcopy(u.u_base, cp, n);
	else
		bcopy(cp, u.u_base, n);
	u.u_base += n;
	u.u_offset += n;
	u.u_count -= n;
}

/*
 * psignal -- upstream sys/sig.c posts the bit in p->p_sig and lets issig()
 * notice it on the way out of the kernel.
 *
 * Here it is kill(2) on the host process the proc entry stands for, which is
 * a real difference worth naming: upstream's is asynchronous with respect to
 * the caller and this is not, so a stwrite to a hung-up stream takes SIGPIPE
 * before psignal returns rather than on the way back to user mode.  Both
 * amount to "the write does not complete and the process gets the signal",
 * and the second is what a single-threaded shim can actually do.
 *
 * v8k_hostof undoes shim/kern/sys/fio.c's fold from a Darwin pid into the
 * range a VAX `short p_pid' could hold.  A proc entry this process does not
 * own has no host pid, and nothing is sent -- which is right, and is also the
 * only sensible answer while there is one process.
 */
void
psignal(struct proc *p, int sig)	/* param.h aims the name at v8k_psignal */
{
	long hp;

	if (p == NULL || sig <= 0)
		return;
	hp = v8k_hostof(p->p_pid);
	if (hp < 0)
		return;
	rawsys2(SYS_kill, hp, (long)sig);
}

/*
 * gsignal -- upstream sys/sig.c walks proc[] and psignals every entry whose
 * p_pgrp matches.  There is one entry.
 *
 * The `pgrp == 0' guard is upstream's and it is not decoration: streamio.c:370
 * is `if (stp->pgrp) gsignal(stp->pgrp, SIGHUP)', but :379 is a bare
 * `gsignal(stp->pgrp, *bp->rptr)' on the M_SIGNAL path with no such test.  A
 * stream that has never had TIOCSPGRP done to it has pgrp 0, so without the
 * guard a driver-generated signal would go to process group 0.
 */
void
gsignal(int pgrp, int sig)
{
	if (pgrp == 0)
		return;
	if (v8k_proc0.p_pgrp == (short)pgrp)
		psignal(&v8k_proc0, sig);
}

/*
 * selwakeup -- upstream sys/sys2.c.
 *
 * Reachable only through stselect(), which sets stp->rsel / stp->wsel, and
 * nothing calls stselect yet: V8's select(2) lives in sys2.c, which is not
 * imported, and the shim answers select(2) itself.  So both pointers are NULL
 * today and strput's and stwsrv's calls fall through the second arm.
 *
 * It is written out anyway rather than stubbed, because the two halves say
 * different things and only one of them is missing here.  The COLLISION half
 * -- two processes selecting on the same stream -- is a broadcast on the
 * selwait channel, and that reduces exactly: nselcoll counts, wakeup does what
 * wakeup does.  The setrun half has no counterpart at all with one process,
 * and clearing SSEL needs a p_flag this port's kernel-side proc does not
 * carry.  Naming the gap beats a comment saying "TODO select".
 */
void
selwakeup(struct proc *p, int coll)
{
	if (coll) {
		nselcoll++;
		wakeup((caddr_t)&selwait);
	}
	if (p != NULL && p->p_wchan == (caddr_t)&selwait)
		p->p_wchan = NULL;	/* setrun(p) has no counterpart here */
}
