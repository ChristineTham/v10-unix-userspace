/*
 * Signals.
 *
 * V8's signal(2) is the V7 one: the handler is reset to SIG_DFL the moment it
 * is delivered, and the program is expected to re-arm it inside the handler.
 * That is NOT what a modern signal() does -- BSD and everything after it made
 * handlers persistent -- so it has to be requested explicitly through
 * sigaction, or every V8 program that catches a signal twice dies on the
 * second one.
 *
 * DELIVERY is the other half, and it lives partly in sigtramp.s: the kernel
 * enters a trampoline the process supplies, not the handler, and that
 * trampoline is what calls sigreturn(2) afterwards.  See v8sys_sigcall below.
 *
 * V8's numbering is almost the BSD numbering, which is macOS's, but not quite:
 * 16 is unused where BSD has SIGURG, and 23 is SIGTINT where BSD has SIGIO.
 * The rest line up.  Rather than rely on that, both directions go through a
 * table.
 *
 * NOT implemented, and it matters: V8 packs flags into the signal NUMBER
 * argument -- SIGDOPAUSE (0400) and SIGDORTI (01000), with SIGNUMMASK 0377
 * extracting the number.  They ask the kernel to pause or return-from-interrupt
 * in ways XNU has no equivalent for.  They are stripped and ignored here, which
 * is right for every ordinary use of signal(); job control (libjobs, csh) is
 * where it would show, and that is Wave B work.
 */

#include <signal.h>
#include "v8sys.h"
#include "rawsys.h"

/* v8/usr/include/signal.h */
#define V8_SIGHUP	1
#define V8_SIGINT	2
#define V8_SIGQUIT	3
#define V8_SIGILL	4
#define V8_SIGTRAP	5
#define V8_SIGIOT	6
#define V8_SIGEMT	7
#define V8_SIGFPE	8
#define V8_SIGKILL	9
#define V8_SIGBUS	10
#define V8_SIGSEGV	11
#define V8_SIGSYS	12
#define V8_SIGPIPE	13
#define V8_SIGALRM	14
#define V8_SIGTERM	15
/* 16 is unused in V8, where BSD has SIGURG */
#define V8_SIGSTOP	17
#define V8_SIGTSTP	18
#define V8_SIGCONT	19
#define V8_SIGCHLD	20
#define V8_SIGTTIN	21
#define V8_SIGTTOU	22
#define V8_SIGTINT	23	/* BSD/macOS put SIGIO here */
#define V8_SIGXCPU	24
#define V8_SIGXFSZ	25

#define V8_SIGNUMMASK	0377
#define V8_NSIG		32

static const int tohost[V8_NSIG] = {
	0,
	SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGEMT, SIGFPE,
	SIGKILL, SIGBUS, SIGSEGV, SIGSYS, SIGPIPE, SIGALRM, SIGTERM,
	0,			/* 16: unused in V8 */
	SIGSTOP, SIGTSTP, SIGCONT, SIGCHLD, SIGTTIN, SIGTTOU,
	SIGIO,			/* 23: V8 SIGTINT, closest host analogue */
	SIGXCPU, SIGXFSZ,
	0, 0, 0, 0, 0, 0,
};

int
v8sys_signo_to_host(int v8sig)
{
	v8sig &= V8_SIGNUMMASK;
	if (v8sig <= 0 || v8sig >= V8_NSIG) return (-1);
	return (tohost[v8sig] ? tohost[v8sig] : -1);
}

int
v8sys_signo_from_host(int hostsig)
{
	int i;

	for (i = 1; i < V8_NSIG; i++)
		if (tohost[i] == hostsig) return (i);
	return (0);
}

typedef void (*v8handler)();

/*
 * THE SYSCALL DOES NOT TAKE THE STRUCT libc's sigaction() TAKES, and the two
 * are different sizes with a different field at offset 8:
 *
 *	struct sigaction    size 16   handler@0            mask@8   flags@12
 *	struct __sigaction  size 24   handler@0  tramp@8   mask@16  flags@20
 *
 * Both are in <sys/signal.h>; libc's sigaction() exists largely to convert one
 * into the other and fill in sa_tramp with its own trampoline.  This file used
 * to hand the syscall the userland struct, so the kernel read the zeroed
 * sa_mask as the trampoline pointer and took sa_flags and sa_mask from past
 * the end of it.  Every handler was installed with a null trampoline; the
 * syscall returned 0 and the fault only appeared on delivery.
 *
 * Asserted rather than remembered.  A wrong shape here is invisible at the
 * seam -- it costs nothing at install time and hangs or kills the process much
 * later -- so if a future SDK moves a field, the build is where that should
 * surface.
 */
_Static_assert(sizeof(struct __sigaction) == 24,
    "struct __sigaction is the 24-byte kernel form: handler, tramp, mask, flags");
_Static_assert(__builtin_offsetof(struct __sigaction, sa_tramp) == 8,
    "sa_tramp sits where the userland struct keeps sa_mask -- that is the bug");
_Static_assert(sizeof(struct sigaction) == 16,
    "the OLD action is copied out in the 16-byte userland form; see below");

/* The trampoline the kernel enters instead of the handler.  sigtramp.s. */
extern void v8sys_sigtramp();

/*
 * Called by the trampoline, with the kernel's own argument order so that
 * sigtramp.s can be a branch and nothing else.
 *
 * Two things happen here that cannot happen in the handler.
 *
 * THE NUMBER BECOMES A V8 NUMBER before any V8 code sees it.  sh's fault()
 * does `signal(sig, fault)' with the number it was handed and then indexes
 * trapcom[sig] and trapflg[sig] with it, so a host number would re-arm through
 * the wrong table entry and index a MAXTRAP array out of range.  V8's
 * numbering agrees with macOS's for every signal V8 names -- both are BSD's,
 * and the differences are the two signals V8 does NOT name -- so this is the
 * identity today.  It is here because the seam is where the translation
 * belongs, not because it currently changes a value.
 *
 * AND SIGRETURN, which is what resumes the interrupted code: it restores the
 * register set and the signal mask the kernel saved.  A handler that longjmps
 * out never reaches it, which is exactly what V8's sleep(3) and sh do -- see
 * SA_NODEFER below for why that is survivable here and would not be otherwise.
 *
 * The handler is called with three arguments and V8's handlers declare one.
 * That is safe rather than lucky: v8cc passes arguments positionally in x0-x7
 * and a V8 function reloads only the parameters it declared.  V7's second and
 * third arguments were a VAX trap `code' and a `struct sigcontext *'; what
 * arrives here instead is Darwin's siginfo_t * and ucontext_t *, the nearest
 * things that exist.  Nothing in this tree reads either -- checked, every
 * handler in src/cmd takes the signal number alone -- so the difference is
 * recorded rather than papered over.
 */
void
v8sys_sigcall(v8handler h, long infostyle, long hostsig, void *info, void *uctx,
	long token)
{
	void (*call)(int, void *, void *) = (void (*)(int, void *, void *))h;

	(*call)(v8sys_signo_from_host((int)hostsig), info, uctx);

	rawsys3(SYS_sigreturn, (long)uctx, infostyle, token);
	__builtin_trap();	/* sigreturn does not return */
}

/*
 * V7 semantics on top of sigaction, which is three flags and the absence of a
 * fourth:
 *
 *   NO SA_RESTART.  V8 programs expect a slow read to fail with EINTR and
 *   check for it.
 *
 *   SA_RESETHAND -- the reset-on-delivery behaviour V8 code is written around,
 *   re-arming inside the handler.
 *
 *   SA_NODEFER, because V7 had no signal mask at all, and because without it
 *   this port would trade one hang for a subtler one.  sigaction blocks the
 *   signal for the duration of the handler and sigreturn unblocks it; a
 *   handler that longjmps out never reaches sigreturn, so the signal would
 *   stay blocked FOREVER.  V8's sleep(3) longjmps out of its SIGALRM handler
 *   and sh longjmps out of its SIGINT handler, and our setjmp.s saves
 *   registers only -- no signal mask, since the VAX original had none to save.
 *   So the first sleep(3) would work and every later one would hang in
 *   pause().  V8's own header says the same thing from the other side: it
 *   spells deferral as an OPT-IN, DEFERSIG(handler) setting the low bit of the
 *   handler address, which means the default is undeferred.  Nothing in this
 *   tree uses it, and it is not implemented.
 *
 * The old action comes back in the SMALLER struct.  That asymmetry looks like
 * a bug and is the kernel's actual interface: __sigaction copies the new
 * action IN as struct __sigaction and the old one OUT as struct sigaction,
 * which has no sa_tramp to report.  libc's sigaction() passes its caller's
 * plain struct straight through for the same reason.
 */
v8handler
v8s_signal(int v8sig, v8handler h)
{
	struct __sigaction sa;
	struct sigaction old;
	int hs = v8sys_signo_to_host(v8sig);

	if (hs < 0) { v8_errno = V8_EINVAL; return ((v8handler)-1); }

	sa.sa_handler = (void (*)(int))h;
	sa.sa_tramp = (void (*)(void *, int, int, siginfo_t *, void *))
	    v8sys_sigtramp;
	sa.sa_mask = 0;
	sa.sa_flags = SA_RESETHAND | SA_NODEFER;
	if (h == (v8handler)SIG_IGN || h == (v8handler)SIG_DFL)
		sa.sa_flags = 0;

	if (rawsys3(SYS_sigaction, hs, (long)&sa, (long)&old) < 0) {
		v8_errno = V8_EINVAL;
		return ((v8handler)-1);
	}
	return ((v8handler)old.sa_handler);
}

/*
 * sigsys(n, action) -- 4.1BSD call 48, and libjobs' single primitive.  Every
 * one of sigset/sighold/sigpause/sigrelse/sigignore in libjobs' sigset.c is
 * written on it, so csh reaches this 88 times.
 *
 * The contract is in signal.s' own header:
 *
 *	sigsys(n, SIG_DFL)		default action
 *	sigsys(n, SIG_HOLD)		block the signal, leave the action
 *	sigsys(n, SIG_IGN)		ignore
 *	sigsys(n, label)		handler
 *	sigsys(n, DEFERSIG(label))	handler, entered with the signal held
 *	returns the old label
 *
 * THE THREE MAGIC VALUES MUST BE TESTED BEFORE THE DEFER BIT, because SIG_IGN
 * IS 1 -- its low bit is set, so decoding first would read it as a deferred
 * handler at address 0.  sigset.c makes the same test in the same order at
 * :38 and :75, which is where the ordering was read from rather than guessed.
 *
 * DEFERRAL IS IMPLEMENTED HERE AND IS NOT IN v8s_signal ABOVE, and the reason
 * is a property of the CALLER rather than a change of mind.  That function's
 * comment declines it because a handler which longjmps out never reaches
 * sigreturn, so a masked signal would stay masked forever -- true of V8's
 * sleep(3) and of sh.  csh does not rely on sigreturn: pintr1 (sh.c:587)
 * calls sigrelse(SIGINT) as its FIRST statement and only then longjmps, which
 * is precisely the discipline System V reliable signals ask for.  So the
 * hazard is real, csh is written around it, and the mask is dropped through
 * an explicit sigprocmask rather than by returning.
 *
 * The defer bit also separates the two protocols sigset.c implements on one
 * primitive: its signal() passes a bare handler (V7 semantics, reset on
 * delivery) and its sigset() always passes DEFERSIG (persistent).  So
 * undeferred gets SA_RESETHAND|SA_NODEFER exactly as v8s_signal does, and
 * deferred gets neither.
 *
 * SIGDOPAUSE is honoured rather than stripped -- sigpause() is `set the action
 * and wait for it', and stripping the bit would turn it into a busy return.
 * SIGDORTI is not: it asks the VAX to resume an interrupted frame, and there
 * is no XNU equivalent.  Nothing in libjobs sends it.
 */
#define V8_SIG_HOLD	3		/* signal.h: (int (*)())3 */
#define V8_SIGDOPAUSE	0400		/* signal.h: pause after setting */
#define SIG_BLOCK_	1		/* <sys/signal.h> */
#define SIG_UNBLOCK_	2

/*
 * Wait for one delivery of the signals in `mask'.  sigsuspend(2) takes the
 * mask to install WHILE waiting, so the signal being waited for must be
 * CLEAR in it -- passing the mask itself would block the very thing that is
 * supposed to wake us and hang forever.
 */
static void
v8s_sigpause_wait(long mask)
{
	unsigned int cur = 0;

	/*
	 * THE TWO SYSCALLS DISAGREE ABOUT POINTER VERSUS VALUE, which is not a
	 * symmetry anyone would guess: XNU's sigprocmask takes `int how,
	 * sigset_t *set, sigset_t *oset' and its sigsuspend takes the mask BY
	 * VALUE.  Passing &cur to sigsuspend blocks on whatever the address
	 * looked like as a mask and never wakes -- measured as csh producing
	 * correct output and then hanging, which reads as a wait-for-child bug
	 * rather than a wrong argument.
	 */
	rawsys3(SYS_sigprocmask, SIG_BLOCK_ /* a null set makes this a QUERY */,
	    0, (long)&cur);
	cur &= ~(unsigned int)mask;
	rawsys1(SYS_sigsuspend, (long)cur);
}

char *
v8s_sigsys(int v8sig, char *action)
{
	struct __sigaction sa;
	struct sigaction old;
	long mask;
	int hs, dopause, defer;
	v8handler h;

	dopause = (v8sig & V8_SIGDOPAUSE) != 0;
	hs = v8sys_signo_to_host(v8sig);	/* masks with SIGNUMMASK */
	if (hs < 0) { v8_errno = V8_EINVAL; return ((v8handler)-1); }
	mask = 1L << (hs - 1);

	/* SIG_HOLD blocks and leaves the action alone, so it needs a query. */
	if (action == (v8handler)V8_SIG_HOLD) {
		if (rawsys3(SYS_sigaction, hs, 0, (long)&old) < 0) {
			v8_errno = V8_EINVAL;
			return ((v8handler)-1);
		}
		if (rawsys3(SYS_sigprocmask, SIG_BLOCK_, (long)&mask, 0) < 0) {
			v8_errno = V8_EINVAL;
			return ((v8handler)-1);
		}
		if (dopause) v8s_sigpause_wait(mask);
		return ((v8handler)old.sa_handler);
	}

	defer = 0;
	h = action;
	if (action != (v8handler)0 && action != (v8handler)1) {
		defer = ((long)action & 1) != 0;
		h = (v8handler)((long)action & ~1L);
	}

	sa.sa_handler = (void (*)(int))h;
	sa.sa_tramp = (void (*)(void *, int, int, siginfo_t *, void *))
	    v8sys_sigtramp;
	sa.sa_mask = defer ? mask : 0;
	sa.sa_flags = defer ? 0 : (SA_RESETHAND | SA_NODEFER);
	if (h == (v8handler)SIG_IGN || h == (v8handler)SIG_DFL) {
		sa.sa_mask = 0;
		sa.sa_flags = 0;
	}

	if (rawsys3(SYS_sigaction, hs, (long)&sa, (long)&old) < 0) {
		v8_errno = V8_EINVAL;
		return ((v8handler)-1);
	}

	/*
	 * Installing an action RELEASES the signal.  That is what makes
	 * sigrelse() work: sigset.c spells it as sigsys(n, DEFERSIG(catcher)),
	 * i.e. re-install, and on the VAX re-installing implied not-held.
	 *
	 * BUT NOT WHEN PAUSING, and that is a RACE rather than a tidiness.
	 * sigset.c spells sigpause as ONE call -- sigsys(n|SIGDOPAUSE, act) --
	 * because the VAX set the action and slept atomically.  Unblocking here
	 * and then suspending leaves a window in which the signal arrives, is
	 * delivered, and is gone before the sleep begins; the sleep then waits
	 * for something that already happened.  sigsuspend(2) installs its mask
	 * and waits as one operation, so it is what must do the unblocking.
	 *
	 * Measured, and the shape is worth recording: csh produced CORRECT
	 * OUTPUT and then hung, intermittently -- the same binary and the same
	 * script passing and hanging minutes apart.  That intermittency is the
	 * evidence, because a wrong mask or a wrong argument would fail every
	 * time.  It was nearly filed against the terminal and against a missing
	 * /dev, both of which were measured innocent first.
	 */
	if (!dopause) {
		if (rawsys3(SYS_sigprocmask, SIG_UNBLOCK_, (long)&mask, 0) < 0) {
			v8_errno = V8_EINVAL;
			return ((v8handler)-1);
		}
	} else {
		v8s_sigpause_wait(mask);
	}
	return ((v8handler)old.sa_handler);
}

/*
 * sigpeel(n, action) -- libjobs' other assembly routine, and here it RETURNS,
 * which upstream marks NOTREACHED.
 *
 * signal.s describes its job as "peel back frames to the last one, then call
 * the system to reenable the signal with newact, arranging to clean the stack
 * before the signal can happen again".  That is three things, and on this
 * target the kernel already does all three:
 *
 *	peel the frames	 sigreturn(2), which v8sys_sigcall calls on the way
 *			 out of every handler -- see the trampoline above
 *	reenable	 sigaction installs a PERSISTENT handler when the
 *			 action was deferred (v8s_sigsys sets no
 *			 SA_RESETHAND in that arm), so it is still armed
 *	unblock		 sigreturn restores the mask the kernel saved
 *
 * So the honest implementation is to do nothing and let _sigcatch fall off its
 * own end.  It is NOT a stub standing in for unwritten work: the VAX needed an
 * explicit unwind because its signal frames were the program's to clean, and
 * XNU's are not.  Writing anything here would be inventing a difference the
 * kernel does not have, which is the /dev/fd rule.
 *
 * The one thing that would break if this were wrong is invisible at the call:
 * _sigcatch would return into a frame that had already been peeled.  It is
 * exercised by tests/wavea driving csh through a caught SIGINT, which is the
 * only way to reach it at all.
 */
/* ARGSUSED */
int
v8s_sigpeel(int v8sig, char *action)
{
	return (0);
}

/* libjobs' additions.  killpg and setpgrp map directly. */
int v8s_killpg(int pgrp, int sig)
{
	int h = v8sys_signo_to_host(sig);
	if (h < 0) { v8_errno = V8_EINVAL; return (-1); }
	if (rawsys3(SYS_kill, -pgrp, h, 0) < 0) { v8_errno = V8_EPERM; return (-1); }
	return (0);
}

/*
 * getpgrp(pid) TAKES AN ARGUMENT, which is the half of the pair that has since
 * been dropped -- V8 files it under deprecated(2), and POSIX kept only the
 * no-argument form.  csh calls getpgrp(0), meaning its own group, so the host
 * call is getpgid(2) with the pid passed straight through.
 *
 * Added with sigsys rather than with setpgrp above because nothing needed it
 * until csh: setpgrp had a caller and its partner did not, which is how one
 * half of a pair sits missing without anything saying so.
 */
int v8s_getpgrp(int pid)
{
	long r = rawsys1(SYS_getpgid, pid);
	if (r < 0) { v8_errno = V8_EPERM; return (-1); }
	return ((int)r);
}

int v8s_setpgrp(int pid, int pgrp)
{
	if (rawsys2(SYS_setpgid, pid, pgrp) < 0) { v8_errno = V8_EPERM; return (-1); }
	return (0);
}
