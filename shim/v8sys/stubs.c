/*
 * The parts of the V8-named layer that are not plain syscall wrappers.
 *
 * The wrappers themselves are generated one object at a time from
 * syscalls.def via onestub.c -- see the note there for why the granularity
 * matters.  What is left here does not fit that mould:
 *
 *	errno		an object, not a function
 *	_exit, exit	exit() has to flush stdio first
 *	signal		takes a function pointer, so it does not fit the
 *			all-arguments-are-long wrapper shape
 *
 * These are split into three objects rather than one because the same rule
 * applies: a program defining its own signal() must get its own.  In this file
 * that is done by keeping each in its own translation unit -- see the Makefile,
 * which compiles it three times with -DV8_PART_n.
 *
 * Like onestub.c, this is compiled ONLY into the copy of the library the V8
 * world links with -nostdlib, and may not call the host libc.
 */

#include "v8sys.h"

#ifdef V8_PART_ERRNO
/*
 * errno.  V8 declares `extern int errno` and every program reads it directly,
 * so it has to be a real object with that name -- not a macro, and not
 * thread-local, both of which a modern libc would make it.
 */
int errno;
#endif

#ifdef V8_PART_EXIT
extern void v8s_exit();

/*
 * _exit is the raw syscall; exit() is libc's, which flushes stdio through
 * _cleanup first.  V8 keeps them in separate files (sys/_exit.s and sys/exit.s)
 * for exactly that reason.
 */
void _exit(code) int code; { v8s_exit(code); }

/*
 * exit() -- what V8's libc/sys/exit.s does: flush stdio through _cleanup(),
 * then leave through the kernel.
 *
 * Skipping the flush is invisible in a program that writes with write(2) and
 * fatal in one that uses stdio, which is most of them.  `tee` and `yes` worked
 * while `echo`, `wc`, `head`, `rev`, `tr`, `basename` and `sum` all printed
 * nothing -- the buffered output was simply discarded at exit.
 *
 * _cleanup lives in libc's flsbuf.c and walks _iob flushing every open stream.
 * It is weakly referenced so this file still links into a program built
 * without V8 libc (the freestanding tests do exactly that).
 */
extern void _cleanup() __attribute__((weak));

void
exit(code)
	int code;
{
	if (_cleanup) _cleanup();
	v8s_exit(code);
}
#endif

#ifdef V8_PART_SIGNAL
extern char *v8s_signal();
extern int errno;

char *
signal(sig, h)
	int sig;
	char *h;
{
	char *r = v8s_signal(sig, h);

	errno = v8_errno;
	return (r);
}
#endif
