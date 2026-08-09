/*
 * vmmeter.h -- the paging statistics counters, reduced to the four bio.c bumps.
 *
 * THIS FILE IS OURS, not Bell Labs'; see shim/kern/h/param.h.  It is reached
 * because src/sys/h/vm.h is AUTHENTIC and its line 9 is
 * `#include "../h/vmmeter.h"' -- so this header has a consumer even though
 * nothing in the port reads a counter back.  That is the distinction
 * CLAUDE.md draws for /dev/fd: an unconsumed component invents a difference,
 * and this one is consumed by an imported file's include list.
 *
 * WHY A STAND-IN.  Upstream's is 97 lines: `struct vmmeter' with 24 counters,
 * `struct vmtotal' with 14 more, and the three instances cnt, rate and sum
 * (h/vmmeter.h:39).  It is the data behind vmstat(8), which this port does not
 * have and which would need a VAX pager underneath to say anything true.
 *
 * FOUR MEMBERS, BECAUSE FOUR IS WHAT IS WRITTEN.  Measured over all six
 * imported files rather than transcribed from the struct -- only dev/bio.c
 * touches either instance, at exactly four sites:
 *
 *	bio.c:478	cnt.v_pgout++
 *	bio.c:479	cnt.v_pgpgout += bp->b_bcount / NBPG
 *	bio.c:548	sum.v_pswpin  += btoc(nbytes)
 *	bio.c:550	sum.v_pswpout += btoc(nbytes)
 *
 * All four are upstream `unsigned' (h/vmmeter.h:12,13,15,17) and all four are
 * WRITE-ONLY here: nothing in this port reads them.  They are kept rather than
 * `#define'd away because the writes are in authentic source and the fidelity
 * contract says compile Bell Labs' statement, not a version of it with the
 * bookkeeping deleted.
 *
 * `rate' and `struct vmtotal' are absent because no imported file names them.
 * Keep it that way: a counter added for completeness is a claim nothing checks,
 * which is the rule shim/kern/h/param.h states for its signal list.
 */

#ifndef V8KERN_VMMETER_H
#define V8KERN_VMMETER_H

struct vmmeter {
	unsigned v_pswpin;	/* h/vmmeter.h:12 -- pages swapped in */
	unsigned v_pswpout;	/* h/vmmeter.h:13 -- pages swapped out */
	unsigned v_pgout;	/* h/vmmeter.h:15 -- pageouts */
	unsigned v_pgpgout;	/* h/vmmeter.h:17 -- pages paged out */
};

/*
 * Upstream h/vmmeter.h:39 is `struct vmmeter cnt, rate, sum;' -- a K&R
 * tentative definition inside #ifdef KERNEL, merged by the linker across every
 * object that includes it.  -fcommon in STREAMIOFLAGS is what keeps that idiom
 * working here, and the reason it is in the flag set is recorded in the
 * Makefile beside it.  `rate' is dropped; see above.
 */
struct	vmmeter cnt, sum;

#endif /* V8KERN_VMMETER_H */
