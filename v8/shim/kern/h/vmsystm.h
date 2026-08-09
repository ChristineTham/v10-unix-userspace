/*
 * vmsystm.h -- deliberately empty; see shim/kern/h/vmparam.h for the argument,
 * which is the same one.
 *
 * THIS FILE IS OURS, not Bell Labs'.  It exists because src/sys/h/vm.h:10 is
 * authentic and includes it.
 *
 * Upstream's is 49 lines of pager state: freemem, avefree, deficit, nscan,
 * desscan, the swap scheduler's tick counters and the `struct vmtotal total'
 * that vmstat(8) reports.  Measured over all six imported files -- alloc.c,
 * iget.c, nami.c, rdwri.c, sys/subr.c and bio.c -- NOT ONE NAME from it is
 * referenced.  The counters bio.c does bump live in vmmeter.h and are there.
 *
 * So this is the honest shape: a file whose entire content is a record that
 * the question was asked and the answer was none.
 */

#ifndef V8KERN_VMSYSTM_H
#define V8KERN_VMSYSTM_H

/* intentionally empty -- see above */

#endif /* V8KERN_VMSYSTM_H */
