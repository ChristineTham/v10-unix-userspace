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

/*
 * struct vtimes -- TWO members, and it is declared HERE rather than in a
 * shim/kern/h/vtimes.h of its own.
 *
 * Upstream's user.h gets it from h/vtimes.h, and the reason not to copy that
 * arrangement is the /dev/fd rule: a header nothing includes is an unconsumed
 * component.  Measured -- NOT ONE of the six imported files includes
 * "../h/vtimes.h", so a file by that name would be reached by nothing and
 * would exist only to look like upstream.  Contrast shim/kern/h/vmmeter.h,
 * which IS reached, because the authentic src/sys/h/vm.h:9 includes it.
 *
 * The two members are the two that are written: rdwri.c bumps vm_inblk on
 * every readi and vm_oublk on every writei, four sites and two sites.  Both
 * are upstream `int' (h/vtimes.h:17,18).  Nothing reads them back here --
 * vtimes(2) is not ported -- but the writes are in authentic source and the
 * contract says compile Bell Labs' statement rather than a version with the
 * bookkeeping removed.
 */
struct vtimes {
	int	vm_inblk;		/* h/vtimes.h:17 -- block reads */
	int	vm_oublk;		/* h/vtimes.h:18 -- block writes */
};

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
	/*
	 * §8a step 5 -- the filesystem half of the u-area.  TEN members, and
	 * the count is measured over alloc.c, iget.c, nami.c, rdwri.c,
	 * sys/subr.c and bio.c rather than taken from upstream's ninety.
	 * Types and order are upstream's, with the h/user.h line on each.
	 *
	 * u_dbuf IS THE ONE WITH A HAZARD, and it is not its width -- it is
	 * that nami.c reads it with a hand-unrolled compare rather than with
	 * strncmp.  nami.c:179-182 is
	 *
	 *	*(int *)&nm[0] == ... && *(short *)&nm[12] == ...
	 *
	 * where nm is u.u_dbuf -- four-byte reads at offsets 0, 4 and 8 and a
	 * two-byte read at 12, covering bytes 0..13 exactly.  That is in
	 * bounds only while DIRSIZ is 14, and it is: src/sys/h/dir.h:2 is
	 * upstream's 14 and CLAUDE.md's rule is that it must stay 14 here,
	 * because src/sys/ describes a DISK RECORD.  Measured through this
	 * header's own include path rather than assumed -- DIRSIZ=14,
	 * sizeof(struct direct)=16.
	 *
	 * So declaring it `char u_dbuf[DIRSIZ]' is not a formality: any
	 * smaller array turns a compare into a read past the member, and the
	 * value read would be whichever field followed.  shim/kern/sys/v8fs.c
	 * asserts the size and the alignment rather than this comment.
	 */
	struct	inode *u_cdir;		/* h/user.h:52 -- current directory */
	struct	inode *u_rdir;		/* :53 -- this process's root */
	char	u_dbuf[DIRSIZ];		/* :54 -- current pathname component */
	caddr_t	u_dirp;			/* :55 -- pathname pointer */
	struct	direct u_dent;		/* :56 -- current directory entry */
	char	u_acflag;		/* :102 -- accounting flags */
	short	u_cmask;		/* :104 -- mask for file creation */
	struct	vtimes u_vm;		/* :108 -- stats for this proc */
	int	u_limit[8];		/* :116 -- see src/sys/h/vlimit.h */
	int	u_nbadio;		/* :117 -- IO on hungup streams */
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
