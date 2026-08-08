/*
 * The u-area, as much of it as V8's streamio.c asks for.
 *
 * THIS FILE IS OURS, not Bell Labs'.  src/sys/sys/streamio.c is authentic and
 * differs from upstream in one recorded line; this is the machine-dependent
 * half it compiles against, in the same relationship shim/kern/h/param.h has
 * to src/sys/dev/stream.c.  The include path that reaches it is described
 * there and is the same one: an authentic header always wins, and ours fill
 * the gaps.  src/sys/h/user.h does not exist, so "../h/user.h" lands here.
 *
 * WHY THIS IS A STAND-IN AND NOT AN IMPORT -- src/sys/PORTING.md hazard 4 has
 * it measured, and the short form is THREE FIELDS.  streamio.c touches ten
 * u_ fields.  Seven are the same width on a VAX and on ARM64.  Three are not,
 * and all three for one reason: they are pointer-shaped, and the /proc ABI
 * (shim/libkmemu/procfs.c's `struct v8user') freezes them at VAX widths
 * because a V8 program reading /proc must see 1985's layout.
 *
 *	field		VAX			here
 *	u_procp		struct proc *, 4	8
 *	u_qsav		label_t = long[14], 56	jmp_buf, 192
 *	u_ofile		struct file *[128], 512	1024
 *
 * So there are two `struct user' in this port and that is a consequence to
 * state once rather than a smell to clean up later.  procfs.c's is offset-
 * declared and _Static_asserted against 1985's layout because a V8 program
 * reads it.  THIS one claims no layout at all, because nothing outside
 * streamio.c reads it -- there is no ABI here to be faithful to, only a set of
 * fields that must exist and hold what the code puts in them.
 *
 * Enumerating those three fields is also what found a live defect in the
 * /proc one: u_ofile was declared as sixteen pointers where NOFILE is 128, and
 * every _Static_assert in the file passed, because the pad after it had been
 * computed from the wrong length.  An offset-plus-total-size pair is blind to
 * an array's length.  Fixed there; the guard that can see it is a sizeof on
 * the member.
 *
 * THIRTEEN FIELDS, not ten.  The ten streamio.c names, plus u_base, u_offset
 * and u_segflg, which shim/kern/sys/subr.c's iomove() needs because it
 * reproduces sys/rdwri.c's.  Said out loud because "ten" is what
 * src/sys/PORTING.md counted and the count is about a different question.
 */

#ifndef V8KERN_USER_H
#define V8KERN_USER_H

/*
 * V8'S jmp_buf, NOT THE HOST'S, and the choice is the same one tests/streams
 * already makes about memcpy.
 *
 * libv8kern is linked into V8 programs, so a setjmp taken from <setjmp.h>
 * would resolve out of libSystem and leave the kernel side of a 1985 stream
 * saving its context with Apple's code -- the exact class tests/kmemu sweeps
 * the whole rootfs for.  src/include/setjmp.h is this port's own, 24 longs
 * because AAPCS64 needs x19-x28, x29, x30, sp and the low halves of d8-d15,
 * and compiler/setjmp.s implements it in ARM64 assembly to the same ABI clang
 * compiles this file with.  So the kernel and the V8 program above it use one
 * jump buffer and one implementation.
 *
 * The include is by relative path on purpose; shim/kern/sys/fio.c says why
 * this side of the seam spells its cross-tree includes out.  The guard is
 * param.h's and the reason it has to exist is there: V8's setjmp.h has none of
 * its own, and a repeated typedef is an error in gnu89.
 */
#ifndef V8KERN_JMP_BUF
#define V8KERN_JMP_BUF
#include "../../../src/include/setjmp.h"
#endif

/*
 * u_qsav is a REAL jump buffer, and that is the whole of hazard 4's second row.
 *
 * Upstream declares `label_t u_qsav', and label_t is long[14] -- 56 bytes on a
 * VAX, where NOLONG made a long 32 bits.  An ARM64 frame does not fit in 56
 * bytes; jmp_buf here is 192.  streamio.c's three `longjmp(u.u_qsav)' calls
 * have to actually work, so the field is that type and param.h's
 * `#define longjmp v8k_longjmp' supplies the one-argument spelling the kernel
 * source uses.
 *
 * WHAT SETS IT is the shim's stream entry point, and the shape is upstream's:
 * sys/trap.c:176 is
 *
 *	if (setjmp(u.u_qsav)) {
 *		if (u.u_error == 0 && u.u_eosys == JUSTRETURN)
 *			u.u_error = EINTR;
 *	} else
 *		(*(callp->sy_call))();
 *
 * -- the system-call dispatcher, aborting the call when a signal arrives
 * mid-sleep.  v8k_stcall() in shim/kern/sys/slp.c is that, with the one
 * u_eosys clause dropped because nothing here restarts a system call.
 */

struct user {
	char	u_segflg;		/* 0: user D; 1: system */
	char	u_error;		/* return error code */
	short	u_uid;			/* effective user id */
	short	u_gid;			/* effective group id */
	struct	proc *u_procp;		/* this process's proc entry */
	union {				/* syscall return values */
		struct {
			int	R_val1;
			int	R_val2;
		} u_rv;
#define	r_val1	u_rv.R_val1
#define	r_val2	u_rv.R_val2
		off_t	r_off;
	} u_r;
	caddr_t	u_base;			/* base address for IO */
	unsigned int u_count;		/* bytes remaining for IO */
	off_t	u_offset;		/* offset in file for IO */
	dev_t	u_ttydev;		/* dev,ino of controlling tty */
	ino_t	u_ttyino;
	struct	file *u_ofile[NOFILE];	/* open file table */
	jmp_buf	u_qsav;			/* non-local goto on interrupt */
};

/*
 * u_eosys values -- upstream user.h:129-131.  Only JUSTRETURN is reachable
 * here (nothing restarts a system call and nothing simulates an rti), so the
 * field itself is absent and these are kept for the reader of trap.c's idiom
 * quoted above.
 */
#define	JUSTRETURN	0

extern struct user u;

/*
 * u_error codes.  Upstream's user.h ends with `#include <errno.h>' and this
 * does the same thing for the same reason: u_error IS an errno, and the ten
 * codes streamio.c assigns (ENXIO ENFILE EFAULT EINVAL ENOMEM EIO ENOTTY
 * ENOSPC EBADF EINTR) have identical numbers in V8 and in Darwin -- they are
 * V7's, and neither system renumbered them.  Checked rather than assumed:
 * shim/kern/sys/subr.c _Static_asserts the ones this file's callers use.
 */
#include <errno.h>

#endif /* V8KERN_USER_H */
