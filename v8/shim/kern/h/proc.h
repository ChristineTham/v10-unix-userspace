/*
 * The proc entry, as much of it as V8's streamio.c asks for.
 *
 * THIS FILE IS OURS, not Bell Labs'; see shim/kern/h/param.h for why these
 * headers exist and how "../h/proc.h" reaches this one.
 *
 * WHY IT IS A STAND-IN.  Upstream's h/proc.h opens with
 *
 *	#include "../h/pcb.h"
 *	#include "../h/dmap.h"
 *	#include "../h/vtimes.h"
 *
 * -- the VAX process control block, the swap disk map, and the resource
 * accounting struct -- and its 51 fields are mostly page tables, swap
 * addresses, resident set sizes and run-queue links.  streamio.c reads FOUR:
 * p_pgrp, p_pid, p_nice and p_wchan.  Importing the header would mean carrying
 * a description of VAX virtual memory in order to obtain four fields.
 *
 * AND THE FIELD WIDTHS ARE UPSTREAM'S, WHICH IS THE POINT -- src/sys/PORTING.md
 * hazard 2.  It was recorded as "stream.h:67 is `short pgrp', and 16 bits
 * cannot hold a macOS pid".  Upstream contradicts the second half:
 * h/proc.h:28-29 declare `short p_pgrp' and `short p_pid', so on a VAX
 * stream.h's field is EXACTLY as wide as a process id, and streamio.c:573 --
 *
 *	stq->pgrp = u.u_procp->p_pgrp = u.u_procp->p_pid;
 *
 * -- loses nothing at all.  The narrowing was this port's own: p_pid and
 * p_pgrp are widened to int in src/include/sys/proc.h because a macOS pid
 * above 32767 reads back negative and ps(1) has to print the real one.
 *
 * So this is not V8 being too small for the host.  It is one of this port's
 * commitments meeting an authentic field that never needed the other, and the
 * answer is the one daddr_t already gets: A STRUCT THAT CROSSES A SEAM KEEPS
 * THAT SEAM'S WIDTH.  src/include/sys/proc.h crosses to ps and to the /proc
 * ABI, so it stays int.  This one crosses nothing -- streamio.c is its only
 * reader -- so it keeps upstream's short, and the shim hands it ids in that
 * range rather than raw host pids.  Every store, stream.h's own `short pgrp',
 * the :45 comparison and both gsignal() calls are then exact for every value
 * the port can produce.  shim/kern/sys/fio.c is where the mapping to the
 * host's real pid lives, and it is one function.
 *
 * ONE THING IS INHERITED RATHER THAN FIXED, and it is deliberate.
 * TIOCGPGRP/TIOCSPGRP through a stream (streamio.c:567, :576) copy
 * sizeof(stq->pgrp) -- TWO bytes -- to and from a user `int', leaving its top
 * half stale.  The VAX copied two bytes there too, so unlike the :713
 * copyout there is no coincidence to undo, and the fidelity contract says
 * reproduce it.
 */

#ifndef V8KERN_PROC_H
#define V8KERN_PROC_H

struct proc {
	char	p_nice;		/* h/proc.h:20 -- nice for cpu usage */
	int	p_flag;		/* h/proc.h:26 -- see SULOCK below */
	short	p_pgrp;		/* h/proc.h:28 -- process group leader */
	short	p_pid;		/* h/proc.h:29 -- unique process id */
	caddr_t	p_wchan;	/* h/proc.h:40 -- event process is awaiting */
	short	p_idhash;	/* h/proc.h:49 -- hash chain link; see pfind */
	struct	pte *p_p0br;	/* h/proc.h:44 -- page table base P0BR */
	long	p_tsize;	/* h/proc.h:33 `size_t'; see below */
};

/*
 * THE LAST TWO ARE THERE FOR A MACRO, NOT FOR A FIELD ACCESS, which is worth
 * saying because nothing in the six mentions either name.  shim/kern/h/vmmac.h
 * defines upstream's
 *
 *	#define dptopte(p, i)	((p)->p_p0br + (p)->p_tsize + (i))
 *
 * and bio.c:554 expands it.  So the members are reached through a macro whose
 * text lives in a different header, which is why a grep for `p_p0br' over the
 * imported sources finds nothing and the compiler still needs it.  Both are
 * null/zero and stay that way; vtopte() panics on the next line.
 *
 * p_tsize is `long' where upstream says `size_t', and that is the same type
 * rather than a substitution: h/types.h:27 is `typedef long size_t'.  Spelling
 * it out avoids claiming Darwin's _SIZE_T guard for a name libc owns at a
 * different signedness (unsigned long) -- the same judgement param.h records
 * for v8k_time_t, and the opposite of the one it reaches for time_t, where the
 * host's definition is identical and claiming is free.
 */

/*
 * §8a step 5 added the last two members and the table below.  SIX fields now,
 * measured over the imported files rather than taken from upstream's 51 --
 * p_flag for bio.c:629's `p->p_flag |= SULOCK', p_idhash for the hash walk in
 * sys/subr.c:235.
 *
 * ONE FLAG BIT, out of upstream's twenty.  h/proc.h:90.  The rule is param.h's
 * signal rule and not conf.h's layout rule: a flag bit is a VALUE that a
 * future import must agree with, so each one present should be one an imported
 * file demonstrably sets.  Measured -- `grep -oE "\b(SULOCK|SLOCK|SDLYU|SLOAD|
 * SRUN)\b"' over all six yields SULOCK and nothing else.
 */
#define	SULOCK	0x00000040	/* h/proc.h:90 -- user settable lock in core */
#define	SPHYSIO	0x00000800	/* h/proc.h:95 -- doing physical i/o (bio.c) */

/*
 * THE PROC TABLE, AND IT IS FOUR ENTRIES BECAUSE OF ONE INDEX IN bio.c.
 *
 * The paragraph above is still true -- the shim is per-binary, so there is one
 * real process -- but "one process" is not the same as "one table entry", and
 * two imported call sites decide the size:
 *
 *	sys/subr.c:235	for (p = &proc[pidhash[PIDHASH(pid)]];
 *			     p != &proc[0]; p = &proc[p->p_idhash])
 *	dev/bio.c:482,600	wakeup((caddr_t)&proc[2])
 *	dev/bio.c:554		dpte = dptopte(&proc[2], p2dp)   -- NOT a wakeup;
 *				this line used to be listed under the label
 *				above, which was wrong about what it does and
 *				right about what it needs
 *
 * pfind's loop uses proc[0] as its CHAIN TERMINATOR, so slot 0 can never be a
 * real process -- it is upstream's null, spelled as an index because p_idhash
 * is a short rather than a pointer.  And bio.c names proc[2] literally: that
 * is the pagedaemon, which on a V8 system is process 2 by construction
 * (sys/main.c forks init as 1 and the pager as 2).  Those three sites are in
 * swap paths that panic here, but the ADDRESS is formed before the panic, so
 * the slot has to exist.
 *
 * NPROC is 4: slot 0 the terminator, 1 this process, 2 the pagedaemon's
 * address-that-is-never-dereferenced, 3 spare so procNPROC is one past a real
 * entry rather than one past the last used one.  Upstream's NPROC is a
 * config(8) tunable in the hundreds; here it is the smallest number that keeps
 * every index in the imported source in bounds, which is the honest one.
 *
 * pidhash is upstream's width and length -- `short pidhash[PIDHSZ]',
 * h/proc.h:58 and :54, PIDHSZ 63 -- because PIDHASH(pid) is `pid % PIDHSZ' and
 * a different modulus would put a process in a different bucket from the one
 * pfind looks in.  All zero, so every chain starts at proc[0] and pfind
 * returns null until something registers a process.  That is correct rather
 * than provisional: nothing in this port calls pfind.
 */
#define	PIDHSZ		63		/* h/proc.h:54 */
#define	PIDHASH(pid)	((pid) % PIDHSZ)	/* h/proc.h:55 */
#define	NPROC		4		/* see above; NOT upstream's tunable */

extern short pidhash[PIDHSZ];		/* h/proc.h:58 */
extern struct proc *proc, *procNPROC;	/* h/proc.h:64 */

/*
 * There is exactly one, because the shim is PER-BINARY.
 *
 * That is the same fact hazard 3 turns on: stream.h's `char count' is "#
 * processes in stream routines", stenter() increments it and stexit() tests
 * for zero, and 128 nested entries would wrap it.  stenter has six callers,
 * all of them system-call entry points, none reachable from another and none
 * stored in a qinit -- so one process contributes at most 1, and here there is
 * one process.  count is 0 or 1.
 *
 * The obligation that leaves is a rule for this directory rather than a fact
 * the import guarantees: a module or driver put routine runs with a process
 * already inside stenter, so it must not re-enter the stream through one of
 * those six.  Upstream's cannot; ours must not either.
 */
extern struct proc v8k_proc0;

#endif /* V8KERN_PROC_H */
