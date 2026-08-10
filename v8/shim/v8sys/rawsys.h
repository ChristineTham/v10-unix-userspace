/*
 * Raw system calls.
 *
 * WHY THIS EXISTS.  The V8 world calls write(), and the shim implements write()
 * BY DOING THE WORK write() does.  If the shim reached the kernel by calling the
 * host's write(), then in a link where the shim also DEFINES write -- which is
 * the whole point, since the shim is libc as far as a V8 program is concerned --
 * the shim's call binds to itself and recurses until the stack faults.  Linker
 * aliasing does not help: the alias is global, so it captures the shim's own
 * call too.  (Observed, before this: EXC_BAD_ACCESS in v8s_write+4.)
 *
 * So the shim names no libc function at all.  It goes straight to the kernel,
 * which also means a V8 program can be linked with -nostdlib and be genuinely
 * self-contained -- no host libc in the image.
 *
 * MACOS.  Syscall number in x16, arguments in x0-x5, `svc #0x80`.  The CARRY
 * FLAG signals failure and x0 then holds the positive errno; this is why the
 * asm has to `cset` immediately after the svc, before anything else can disturb
 * the flags.  Numbers come from <sys/syscall.h>, which is a header of constants
 * and pulls in no symbols.
 *
 * LINUX.  Number in x8, arguments in x0-x5, `svc #0`, and a negative return
 * value is -errno with no flag involved.
 *
 * The wrappers return the kernel result, or a NEGATED errno on failure, so
 * every caller checks `< 0` regardless of platform.
 */

#ifndef RAWSYS_H
#define RAWSYS_H

#include <sys/syscall.h>

#if defined(__APPLE__)

#define RAWSYS_BODY(nargs, ...)						\
	register long _x16 __asm__("x16") = n;				\
	register long _err __asm__("x8");				\
	__VA_ARGS__							\
	__asm__ volatile("svc #0x80\n\tcset x8, cs"			\
	    : "+r"(_r0), "=r"(_err)					\
	    : "r"(_x16) RAWSYS_IN(nargs)				\
	    : "memory", "cc");						\
	return (_err ? -_r0 : _r0);

#else	/* Linux */

#define RAWSYS_BODY(nargs, ...)						\
	register long _x8 __asm__("x8") = n;				\
	__VA_ARGS__							\
	__asm__ volatile("svc #0"					\
	    : "+r"(_r0)							\
	    : "r"(_x8) RAWSYS_IN(nargs)					\
	    : "memory", "cc");						\
	return (_r0);

#endif

/* Explicit per-arity wrappers: clearer than macro contortions, and the
 * register-constraint lists have to be written out anyway. */

static inline long
rawsys0(long n)
{
	register long r0 __asm__("x0");
#if defined(__APPLE__)
	register long x16 __asm__("x16") = n;
	register long err __asm__("x8");
	__asm__ volatile("svc #0x80\n\tcset x8, cs"
	    : "=r"(r0), "=r"(err) : "r"(x16) : "memory", "cc");
	return (err ? -r0 : r0);
#else
	register long x8 __asm__("x8") = n;
	__asm__ volatile("svc #0" : "=r"(r0) : "r"(x8) : "memory", "cc");
	return (r0);
#endif
}

static inline long
rawsys1(long n, long a)
{
	register long r0 __asm__("x0") = a;
#if defined(__APPLE__)
	register long x16 __asm__("x16") = n;
	register long err __asm__("x8");
	__asm__ volatile("svc #0x80\n\tcset x8, cs"
	    : "+r"(r0), "=r"(err) : "r"(x16) : "memory", "cc");
	return (err ? -r0 : r0);
#else
	register long x8 __asm__("x8") = n;
	__asm__ volatile("svc #0" : "+r"(r0) : "r"(x8) : "memory", "cc");
	return (r0);
#endif
}

static inline long
rawsys2(long n, long a, long b)
{
	register long r0 __asm__("x0") = a;
	register long r1 __asm__("x1") = b;
#if defined(__APPLE__)
	register long x16 __asm__("x16") = n;
	register long err __asm__("x8");
	__asm__ volatile("svc #0x80\n\tcset x8, cs"
	    : "+r"(r0), "=r"(err) : "r"(x16), "r"(r1) : "memory", "cc");
	return (err ? -r0 : r0);
#else
	register long x8 __asm__("x8") = n;
	__asm__ volatile("svc #0" : "+r"(r0) : "r"(x8), "r"(r1) : "memory", "cc");
	return (r0);
#endif
}

static inline long
rawsys3(long n, long a, long b, long c)
{
	register long r0 __asm__("x0") = a;
	register long r1 __asm__("x1") = b;
	register long r2 __asm__("x2") = c;
#if defined(__APPLE__)
	register long x16 __asm__("x16") = n;
	register long err __asm__("x8");
	__asm__ volatile("svc #0x80\n\tcset x8, cs"
	    : "+r"(r0), "=r"(err) : "r"(x16), "r"(r1), "r"(r2) : "memory", "cc");
	return (err ? -r0 : r0);
#else
	register long x8 __asm__("x8") = n;
	__asm__ volatile("svc #0" : "+r"(r0) : "r"(x8), "r"(r1), "r"(r2)
	    : "memory", "cc");
	return (r0);
#endif
}

static inline long
rawsys4(long n, long a, long b, long c, long d)
{
	register long r0 __asm__("x0") = a;
	register long r1 __asm__("x1") = b;
	register long r2 __asm__("x2") = c;
	register long r3 __asm__("x3") = d;
#if defined(__APPLE__)
	register long x16 __asm__("x16") = n;
	register long err __asm__("x8");
	__asm__ volatile("svc #0x80\n\tcset x8, cs"
	    : "+r"(r0), "=r"(err) : "r"(x16), "r"(r1), "r"(r2), "r"(r3)
	    : "memory", "cc");
	return (err ? -r0 : r0);
#else
	register long x8 __asm__("x8") = n;
	__asm__ volatile("svc #0" : "+r"(r0) : "r"(x8), "r"(r1), "r"(r2), "r"(r3)
	    : "memory", "cc");
	return (r0);
#endif
}

/*
 * FIVE, WHICH THE FILE SKIPPED UNTIL setsockopt(2) NEEDED IT.  The gap was not
 * a decision -- nothing had taken five arguments -- and the tempting shortcut
 * is rawsys6 with a trailing zero, since the kernel reads only the registers
 * the syscall declares.  It is written out instead because a 6-argument call
 * with five real arguments says the wrong thing about the syscall to the next
 * reader, and the cost of being honest is fourteen lines that already exist
 * four times above.
 */
static inline long
rawsys5(long n, long a, long b, long c, long d, long e)
{
	register long r0 __asm__("x0") = a;
	register long r1 __asm__("x1") = b;
	register long r2 __asm__("x2") = c;
	register long r3 __asm__("x3") = d;
	register long r4 __asm__("x4") = e;
#if defined(__APPLE__)
	register long x16 __asm__("x16") = n;
	register long err __asm__("x8");
	__asm__ volatile("svc #0x80\n\tcset x8, cs"
	    : "+r"(r0), "=r"(err)
	    : "r"(x16), "r"(r1), "r"(r2), "r"(r3), "r"(r4)
	    : "memory", "cc");
	return (err ? -r0 : r0);
#else
	register long x8 __asm__("x8") = n;
	__asm__ volatile("svc #0"
	    : "+r"(r0) : "r"(x8), "r"(r1), "r"(r2), "r"(r3), "r"(r4)
	    : "memory", "cc");
	return (r0);
#endif
}

static inline long
rawsys6(long n, long a, long b, long c, long d, long e, long f)
{
	register long r0 __asm__("x0") = a;
	register long r1 __asm__("x1") = b;
	register long r2 __asm__("x2") = c;
	register long r3 __asm__("x3") = d;
	register long r4 __asm__("x4") = e;
	register long r5 __asm__("x5") = f;
#if defined(__APPLE__)
	register long x16 __asm__("x16") = n;
	register long err __asm__("x8");
	__asm__ volatile("svc #0x80\n\tcset x8, cs"
	    : "+r"(r0), "=r"(err)
	    : "r"(x16), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
	    : "memory", "cc");
	return (err ? -r0 : r0);
#else
	register long x8 __asm__("x8") = n;
	__asm__ volatile("svc #0" : "+r"(r0)
	    : "r"(x8), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
	    : "memory", "cc");
	return (r0);
#endif
}

/*
 * The host errno from a raw result.  Callers do:
 *	r = rawsys...;  if (r < 0) { v8_errno = v8sys_errno(-r); return -1; }
 */
#define RAWERR(r) ((int)-(r))

#endif /* RAWSYS_H */
