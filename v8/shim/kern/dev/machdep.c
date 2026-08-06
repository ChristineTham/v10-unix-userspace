/*
 * The machine-dependent half of V8's stream machinery.
 *
 * src/sys/dev/stream.c is authentic Bell Labs kernel source, byte-identical to
 * upstream -- `git hash-object' against src/sys/PROVENANCE says so.  This file
 * is everything that source assumes about the machine underneath it, written
 * for a userspace process on ARM64 instead of a VAX in kernel mode.  It is the
 * same division of labour as compiler/ccom-arm64: an authentic body, and a
 * machine-dependent half with the names the body expects.
 *
 * Measured, before any of it was written: stream.c's whole dependency on the
 * kernel is nine names.  Four are types (shim/kern/h/param.h), one is a VAX
 * privileged-register write (mtpr.h), one is Unibus DMA that has nothing to map
 * here, and the rest are below -- spl6, splx, panic, printf.  A 483-line
 * message-passing engine with a nine-name footprint is why PLAN.md section 8a
 * puts streams first: it is the piece of V8's kernel that barely knows what
 * machine it is on.
 *
 * RAW SYSCALLS ONLY, like the rest of shim/ outside libkmemu.  v8k_printf
 * writes with SYS_write rather than calling anything, and v8k_bcopy is written
 * out rather than taken from libSystem -- see below for why that one matters
 * more than it looks.
 */

#include "../../v8sys/rawsys.h"
#include "../h/param.h"
#include "../h/mtpr.h"

#undef printf			/* param.h aims it here; this file IS here */

void queuerun(void);
void qinit(void);

char	*panicstr;		/* queuerun() checks it, to stop early in a panic */
int	queueflag;		/* nonzero while inside queuerun(); its own guard */

static int	splevel;	/* nesting depth of spl6() */
static int	qsched;		/* a setqsched() is pending at this level */
static int	inited;

/*
 * spl6 / splx -- interrupt priority level.
 *
 * On the VAX these wrote the processor's IPL, and stream.c uses them the way
 * every V7-descended kernel does: raise around anything that touches a queue,
 * restore afterwards.  There is no IPL in a userspace process, so they are a
 * NESTING COUNTER.
 *
 * The counter is not decoration, and that is the point of doing it this way
 * rather than making them no-ops.  setqsched() consults it: a qenable() inside
 * a critical section must NOT run the service procedures immediately, it must
 * defer them to the splx() that ends the section -- which is exactly what the
 * VAX's software interrupt did, held off by the priority level until the level
 * dropped.  So the semantics survive, and they are observable, which means
 * tests/streams can assert them rather than take them on trust.
 *
 * WHAT THEY DO NOT DO YET, said plainly: they do not block signals.  Nothing in
 * this port delivers stream messages from a signal handler -- there are no
 * device interrupts here and no asynchronous producer -- so a sigprocmask on
 * every putq would cost two syscalls on the hot path to exclude something that
 * cannot happen, and could not be tested, and a guard that has never been seen
 * to fail is not a guard.  When a signal-driven source arrives (a tty, a
 * socket), spl6 gains the mask and the counter stays exactly as it is.
 */
int
spl6(void)
{
	return (splevel++);
}

void
splx(int s)
{
	splevel = s;
	if (splevel == 0 && qsched && !queueflag) {
		qsched = 0;
		queuerun();
	}
}

/*
 * v8k_mtpr -- the VAX privileged-register write, honouring exactly one register.
 *
 * V8's stream.h says `#define setqsched() mtpr(SIRR, 0x1);' and that macro is
 * authentic and compiles unchanged.  Writing 1 to the Software Interrupt
 * Request Register means "run the queue scheduler as soon as the priority level
 * allows"; here that is a pending flag plus the same test splx() makes.
 *
 * Any other register panics.  Every one of them describes hardware that is not
 * here, and a stream module writing one would be doing something this port
 * cannot answer for -- a silent no-op is how a machine-dependent gap becomes a
 * program that runs and is wrong.
 */
void
v8k_mtpr(int reg, long val)
{
	if (reg != SIRR) {
		panic("mtpr to register %x, which this machine does not have\n",
		    reg);
		return;
	}
	if (val == 0)
		return;
	qsched = 1;
	if (splevel == 0 && !queueflag) {
		qsched = 0;
		queuerun();
	}
}

/*
 * v8k_streaminit -- what the kernel's main() did, done on demand instead.
 *
 * qinit() threads every block onto its size-class freelist, which touches all
 * of cblock[] and blkdata[] -- about 60 KB of pages that would otherwise stay
 * clean.  A program that never opens a stream should not pay that, so this is
 * called by stream users rather than by a constructor, and the whole library
 * is a separate archive for the same reason libkmemu is: cost that only its
 * users carry.  Idempotent, so callers need not coordinate.
 */
void
v8k_streaminit(void)
{
	if (inited)
		return;
	inited = 1;
	qinit();
}

/* ------------------------------------------------------------------------
 * Kernel services stream.c expects to find in the kernel it is part of.
 */

static void
kputs(const char *s, long n)
{
	rawsys3(SYS_write, 2, (long)s, n);
}

static void
kputn(unsigned long v, int base, int sgn)
{
	char buf[24];
	int i = (int)sizeof buf;
	int neg = 0;

	if (sgn && (long)v < 0) { neg = 1; v = (unsigned long)(-(long)v); }
	if (v == 0)
		buf[--i] = '0';
	while (v) {
		int d = (int)(v % (unsigned)base);
		buf[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		v /= (unsigned)base;
	}
	if (neg)
		buf[--i] = '-';
	kputs(buf + i, (long)sizeof buf - i);
}

/*
 * KERNEL printf, and it is deliberately NOT the program's.
 *
 * param.h redirects the name; the reasoning is there.  The short version: a
 * kernel diagnostic must not go through the V8 program's stdio, where it would
 * be buffered with the program's output and lost entirely if stdout has been
 * closed or redirected.  fd 2, unbuffered, one write per fragment.
 *
 * %s %d %x %c %% and nothing else, because that is what the kernel sources use
 * -- stream.c wants %x for a pointer and plain strings.  An unknown directive
 * prints itself rather than being skipped, so a future %o shows up in the
 * output as `%o' instead of silently consuming an argument and shifting every
 * later one.
 */
static void
kvprintf(const char *fmt, __builtin_va_list ap)
{
	const char *p, *run;

	for (p = run = fmt; *p; p++) {
		if (*p != '%')
			continue;
		if (p > run)
			kputs(run, (long)(p - run));
		switch (*++p) {
		case 's': {
			const char *s = __builtin_va_arg(ap, const char *);
			const char *e = s;
			if (s == 0) { kputs("(null)", 6); break; }
			while (*e) e++;
			kputs(s, (long)(e - s));
			break;
		}
		case 'd': kputn((unsigned long)(long)__builtin_va_arg(ap, int), 10, 1); break;
		case 'x': kputn((unsigned long)__builtin_va_arg(ap, unsigned long), 16, 0); break;
		case 'c': { char c = (char)__builtin_va_arg(ap, int); kputs(&c, 1); break; }
		case '%': kputs("%", 1); break;
		case '\0': kputs("%", 1); p--; break;
		default:  kputs(p - 1, 2); break;
		}
		run = p + 1;
	}
	if (p > run)
		kputs(run, (long)(p - run));
}

void
v8k_printf(const char *fmt, ...)
{
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);
	kvprintf(fmt, ap);
	__builtin_va_end(ap);
}

/*
 * panic -- and it really does stop.
 *
 * panicstr is set before printing, because queuerun() reads it to bail out
 * early "to minimize destruction", which is V8's comment and V8's intent.  The
 * process then aborts: a kernel that panics does not return to the code that
 * called it, and returning here would let a corrupted freelist keep being used
 * with only a line of output to say so.
 *
 * IT FORMATS, and the first draft did not.  Every panic in stream.c is a plain
 * string -- "allocb out of blocks", "Free of free block", "backq" -- so a
 * version that printed the format verbatim looked correct against every caller
 * in the authentic source.  Then v8k_mtpr above panicked with the offending
 * register number and printed a literal `%x': the one detail that made the
 * message worth having, dropped, in the one caller written after the shortcut
 * was taken.  Hence kvprintf as the shared core.
 */
void
panic(const char *fmt, ...)
{
	__builtin_va_list ap;

	panicstr = (char *)fmt;
	kputs("panic: ", 7);
	__builtin_va_start(ap, fmt);
	kvprintf(fmt, ap);
	__builtin_va_end(ap);
	kputs("\n", 1);
	rawsys1(SYS_exit, 2);
	for (;;)
		;
}

/*
 * v8k_bcopy -- and the reason it is here rather than absent.
 *
 * putq() coalesces a small M_DATA block into the tail of the previous one with
 * bcopy().  Neither libv8c nor libv8sys defines bcopy, so leaving it out would
 * not fail the link: it would resolve out of libSystem, silently and correctly,
 * and the stream engine of a 1985 Bell Labs kernel would be copying its
 * messages with Apple's code.  That is the exact class tests/kmemu sweeps the
 * whole rootfs for, caught here before it happened rather than after.
 *
 * Argument order is V7's bcopy(from, to, n), which BSD's agrees with; it is
 * memmove's, reversed.  Overlap is handled because putq's source and
 * destination are both inside blkdata[] and nothing promises they are disjoint.
 */
void
v8k_bcopy(const void *from, void *to, unsigned long n)
{
	const unsigned char *s = (const unsigned char *)from;
	unsigned char *d = (unsigned char *)to;

	if (d == s || n == 0)
		return;
	if (d < s)
		while (n--) *d++ = *s++;
	else {
		d += n; s += n;
		while (n--) *--d = *--s;
	}
}
