/*
 * The kernel's param.h, as much of it as V8's stream machinery asks for.
 *
 * THIS FILE IS OURS, not Bell Labs'.  src/sys/dev/stream.c is authentic and
 * byte-identical to upstream; this is the machine-dependent half it compiles
 * against, in the same relationship compiler/ccom-arm64/macdefs.h has to ccom.
 *
 * HOW THE INCLUDE PATH WORKS, because it is doing something deliberate.
 * stream.c says #include "../h/param.h", and a quoted include is resolved
 * against the INCLUDING FILE's directory first.  So from src/sys/dev/ it tries
 * src/sys/h/param.h, which does not exist, and only then falls through to -I.
 * The build passes -Ishim/kern/dev, so "../h/param.h" lands here.
 *
 * The consequence is the property worth having: WHERE AN AUTHENTIC HEADER
 * EXISTS IT WINS, and ours fills only the gaps.  src/sys/h/stream.h is real V8
 * and is picked up by the same rule, from the same #include syntax, with no
 * flag saying which is which.  Adding an authentic param.h later displaces this
 * one without touching a build rule.
 *
 * WHY NOT IMPORT V8'S param.h.  Measured: stream.c's entire dependency on the
 * kernel headers is nine names -- NULL, caddr_t, u_char, u_short, spl6, splx,
 * panic, printf and one uballoc.  V8's is 185 lines of VAX page sizes, cluster
 * counts and process limits, pulling <signal.h> and ../h/types.h behind it.
 * Importing it would mean carrying a description of a machine that is not here
 * in order to obtain four typedefs.
 */

#ifndef V8KERN_PARAM_H
#define V8KERN_PARAM_H

#ifndef NULL
#define NULL	0
#endif

typedef char *		caddr_t;
typedef unsigned char	u_char;
typedef unsigned short	u_short;

/*
 * Interrupt priority level.  See shim/kern/dev/machdep.c -- these are a nesting
 * counter here rather than a write to the VAX's IPL, and the counter is not
 * decoration: setqsched() consults it, so a qenable() inside a critical section
 * defers its queuerun() to the splx() that ends the section, exactly as the
 * software interrupt would have been held off by the level.
 */
int	spl6(void);
void	splx(int s);

void	panic(const char *fmt, ...);

/*
 * KERNEL printf, AND IT MUST NOT BE THE PROGRAM'S.  stream.c calls printf() for
 * its diagnostics.  libv8c defines printf too, for the V8 program, and in a
 * link that has both the kernel's four diagnostics would go through the
 * program's stdio -- buffered with its output, and lost entirely if it has
 * redirected or closed stdout.  Redirecting the name here keeps stream.c
 * byte-identical and sends kernel messages to fd 2 where they belong.
 */
#define printf	v8k_printf
void	v8k_printf(const char *fmt, ...);

/*
 * KERNEL bcopy, for the same reason and a sharper one.  putq() coalesces a
 * small M_DATA block into the tail of the previous one with bcopy(), and
 * NEITHER libv8c NOR libv8sys defines bcopy -- so omitting it would not fail
 * the link.  It would resolve out of libSystem, work perfectly, and leave a
 * 1985 Bell Labs stream engine copying its messages with Apple's code.  That is
 * the class tests/kmemu sweeps the whole rootfs for; here it is closed before
 * it opened.  V7's argument order, bcopy(from, to, n).
 */
#define bcopy	v8k_bcopy
void	v8k_bcopy(const void *from, void *to, unsigned long n);

void	v8k_streaminit(void);	/* what the kernel's main() called qinit() for */

/*
 * THERE IS NO UNIBUS AND NO DMA, so qinit()'s uballoc() has nothing to map.
 *
 * A macro rather than a three-line deletion, because the deletion buys nothing
 * and costs the strongest claim available: stream.c's blob hash still matches
 * PROVENANCE, so `git hash-object' says it is upstream's file rather than a
 * PORTING.md saying it nearly is.
 *
 * Nothing observes the value.  blkubad is read in exactly two places in the
 * whole V8 kernel -- dev/ill.c (the Interlan Ethernet board) and dev/kdi.c
 * (Datakit) -- both converting a block address into a bus address for a device
 * that is going to DMA out of it.  Neither board is here.  Returning a non-zero
 * token satisfies qinit()'s check and is read by nobody; if a driver ever wants
 * a real bus address this stops compiling, which is the right way to find out.
 */
#define uballoc(uban, addr, size, flags)	((long)1)

#endif /* V8KERN_PARAM_H */
