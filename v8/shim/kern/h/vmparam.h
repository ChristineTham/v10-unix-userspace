/*
 * vmparam.h -- deliberately empty, and the emptiness is the statement.
 *
 * THIS FILE IS OURS, not Bell Labs'; see shim/kern/h/param.h.  It exists
 * because src/sys/h/vm.h is AUTHENTIC and its line 7 is
 * `#include "../h/vmparam.h"'.  An imported header's include list is a
 * consumer, so this is not the unconsumed-component case CLAUDE.md warns
 * about -- but nothing beyond the preprocessor reads it, and saying that out
 * loud is cheaper than leaving a reader to discover it.
 *
 * Upstream's is 95 lines of VAX address-space geometry: USRTEXT, USRSTACK,
 * MAXTSIZ, the swap-partition tunables, SAFERSS, and the maximum number of
 * pages a process may lock.  Every one is a fact about a machine with a page
 * table, and this port has one address space and the host's allocator.
 *
 * WHAT IT DOES NOT DEFINE IS THE POINT.  NBPG and PGSHIFT are the two names
 * from this area that ARE reachable -- bio.c:479 divides by NBPG and
 * shim/kern/h/vmmac.h's btop shifts by PGSHIFT -- and upstream puts both in
 * h/param.h (:65 and :67), not here.  They are therefore in
 * shim/kern/h/param.h with those citations.  Defining them here as well would
 * be a second spelling of one number, which is the class this port has been
 * bitten by four times (DIRSIZ, ten spellings, four values).
 *
 * If a future import needs a name from upstream's vmparam.h, add it HERE with
 * its line number rather than to param.h -- upstream's split is the one to
 * keep, because it is what makes a later real import a substitution rather
 * than a merge.
 *
 * AND THAT FUTURE ARRIVED BEFORE THE FILE WAS FIRST BUILT.  This said
 * "intentionally empty" and the build disagreed within the minute: bio.c:553
 * is
 *
 *	p2dp = ((bp - swbuf) * CLSIZE) * KLMAX;
 *
 * and KLMAX is vmparam.h:77.  The paragraph above was written from a survey of
 * what the six files "are about" rather than from compiling them, which is the
 * same mistake §8a step 5's header estimate made at a larger scale -- and it
 * is the second of two design claims in this directory that the compiler
 * falsified in one pass (shim/kern/h/pte.h records the other).
 *
 * One name, then, added under the rule the paragraph above states.
 */

#ifndef V8KERN_VMPARAM_H
#define V8KERN_VMPARAM_H

/*
 * The swap cluster limit.  h/vmparam.h:77, verbatim -- it is written in terms
 * of CLSIZE rather than as a number, so it is transcribed that way and picks
 * up shim/kern/h/param.h's CLSIZE.
 *
 * Its one reader is the swap-out path in bio.c's swap(), which computes an
 * offset into the pagedaemon's page tables.  vtopte() panics two lines later,
 * so the arithmetic is compiled and never run.
 */
#define	KLMAX	(32/CLSIZE)	/* h/vmparam.h:77 */

#endif /* V8KERN_VMPARAM_H */
