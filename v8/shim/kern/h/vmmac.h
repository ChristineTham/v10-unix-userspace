/*
 * vmmac.h -- the virtual-memory macros, reduced to the one bio.c uses.
 *
 * THIS FILE IS OURS, not Bell Labs'; see shim/kern/h/param.h.  Reached because
 * src/sys/h/vm.h:8 is authentic and says `#include "../h/vmmac.h"'.
 *
 * Upstream's is 92 lines of address arithmetic over the VAX page tables --
 * dptopte, sptopte, vtotp, isatsv and the rest, every one of them indexing a
 * `struct pte' this port deliberately leaves incomplete (shim/kern/h/pte.h).
 * ONE macro out of it is reachable here, and it is reachable exactly once:
 *
 *	bio.c:555	vpte = vtopte(p, btop(addr));
 *
 * -- inside swap()'s physical-I/O path, which panics.  So btop is present for
 * the compile rather than for the run, and it is upstream's line verbatim so
 * that the day a caller does run, the arithmetic is V8's.
 *
 * PGSHIFT COMES FROM param.h, NOT FROM HERE, and that is upstream's layout
 * too: h/param.h:67 is `#define PGSHIFT 9'.  Spelling it again in this file
 * would be a second definition of a number, which is the DIRSIZ trap in
 * miniature -- CLAUDE.md's rule is one number per layer, and the layer that
 * owns a page size is param.h.
 */

#ifndef V8KERN_VMMAC_H
#define V8KERN_VMMAC_H

#define	btop(x)		(((unsigned)(x)) >> PGSHIFT)	/* h/vmmac.h:38 */

/*
 * TWO, not one -- the sentence above saying "ONE macro out of it is reachable"
 * was written before the file was compiled and bio.c:554 disagreed:
 *
 *	dpte = dptopte(&proc[2], p2dp);
 *
 * one line before the vtopte call it was written for.  Left rather than
 * rewritten because the shape of the mistake is the point: a survey of what a
 * file is *about* undercounts what it *references*, every time, and this
 * directory produced three instances of it in one pass (see also
 * shim/kern/h/pte.h and vmparam.h).
 *
 * dptopte indexes a process's data page tables: p_p0br is the page-table base
 * register value and p_tsize the text size in pages, so `base + tsize + i' is
 * the i'th data pte.  Both members are in shim/kern/h/proc.h and both are
 * null/zero here, which is safe only because vtopte() panics on the next line
 * before anything dereferences the result.
 */
#define	dptopte(p, i)	((p)->p_p0br + (p)->p_tsize + (i))	/* h/vmmac.h:33 */

#endif /* V8KERN_VMMAC_H */
