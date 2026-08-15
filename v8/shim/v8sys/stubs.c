/*
 * The parts of the V8-named layer that are not plain syscall wrappers.
 *
 * The wrappers themselves are generated one object at a time from
 * syscalls.def via onestub.c -- see the note there for why the granularity
 * matters.  What is left here does not fit that mould:
 *
 *	errno		an object, not a function
 *	_exit		the raw syscall
 *	exit		has to flush stdio first, so it is a separate object
 *			from _exit -- as V8's own sys/_exit.s and sys/exit.s
 *			are, and for the reason the note there gives
 *	signal		takes a function pointer, so it does not fit the
 *			all-arguments-are-long wrapper shape
 *
 * These are split into FOUR objects rather than one because the same rule
 * applies: a program defining its own signal() must get its own.  In this file
 * that is done by keeping each in its own translation unit -- see the Makefile,
 * which compiles it four times with -DV8_PART_n.
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

#ifdef V8_PART_RAWEXIT
extern void v8s_exit();

/*
 * _exit is the raw syscall.  IT IS ITS OWN OBJECT, and the sentence that used
 * to sit here said why while the code did the opposite: "V8 keeps them in
 * separate files (sys/_exit.s and sys/exit.s) for exactly that reason."  Both
 * were compiled into one object anyway, so the granularity rule this file's
 * header states -- a program defining its own signal() must get its own -- was
 * applied to signal and not to exit.
 *
 * ex(1) is what found it, and it is not an exotic caller: a 1980s program that
 * wants to clean up before leaving defines its own exit() and finishes with
 * _exit().  With both wrappers in one member the linker pulls that member to
 * satisfy _exit, and the member's exit() then collides with the program's --
 * `duplicate symbol '_exit'', which names the C function _exit and is really
 * about C's exit, because Mach-O prefixes an underscore.  Splitting them lets
 * ex have its own exit() and still reach the raw one, which is exactly what
 * upstream's two .s files buy.
 */
void _exit(code) int code; { v8s_exit(code); }
#endif

#ifdef V8_PART_EXIT
extern void v8s_exit();

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
