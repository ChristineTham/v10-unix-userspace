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
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include "v8sys.h"

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
 * V7 semantics on top of sigaction: no SA_RESTART (V8 programs expect a slow
 * read to fail with EINTR and check for it) and SA_RESETHAND, which is the
 * reset-on-delivery behaviour V8 code is written around.
 */
v8handler
v8s_signal(int v8sig, v8handler h)
{
	struct sigaction sa, old;
	int hs = v8sys_signo_to_host(v8sig);

	if (hs < 0) { v8_errno = V8_EINVAL; return ((v8handler)-1); }

	sa.sa_handler = (void (*)(int))h;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESETHAND;
	if (h == (v8handler)SIG_IGN || h == (v8handler)SIG_DFL)
		sa.sa_flags = 0;

	if (sigaction(hs, &sa, &old) < 0) {
		v8sys_fail();
		return ((v8handler)-1);
	}
	return ((v8handler)old.sa_handler);
}

/* libjobs' additions.  killpg and setpgrp map directly. */
int v8s_killpg(int pgrp, int sig)
{
	int h = v8sys_signo_to_host(sig);
	if (h < 0) { v8_errno = V8_EINVAL; return (-1); }
	return (killpg(pgrp, h) < 0 ? v8sys_fail() : 0);
}

int v8s_setpgrp(int pid, int pgrp)
{ return (setpgid(pid, pgrp) < 0 ? v8sys_fail() : 0); }
