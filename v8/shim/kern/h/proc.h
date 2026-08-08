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
	short	p_pgrp;		/* h/proc.h:28 -- process group leader */
	short	p_pid;		/* h/proc.h:29 -- unique process id */
	caddr_t	p_wchan;	/* h/proc.h:40 -- event process is awaiting */
};

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
