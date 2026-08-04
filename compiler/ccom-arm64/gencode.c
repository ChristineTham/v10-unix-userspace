/*
 * ARM64 code generator for V8 ccom -- Phase 1b skeleton.
 *
 * This file will grow into the replacement for ccom/vax/gencode.c.  Right now
 * it is a stub, and its job is to prove a narrower point: that V8's
 * machine-independent pass 1 builds and runs on ARM64, handing us well-formed
 * trees.  Everything above this seam is original 1985 Bell Labs code.
 *
 * The whole pass-1 -> pass-2 interface is three symbols:
 *
 *	gencode(p)	entry point.  pjw.c calls it once per statement tree,
 *			after register allocation has annotated the nodes.
 *	Pflag		set by ccom's -P flag (see common/scan.c); the VAX
 *			generator consults it around basic-block boundaries.
 *	bbcnt		basic-block counter maintained by the generator.
 *
 * That is a remarkably small contract for a code generator, and it is small
 * because V8 discarded pcc's table-driven pass 2 (match.c/table.c/order.c are
 * still in the tree but unlinked -- vax/local.c even defines a stub codgen()
 * "so pcc2 stuff doesn't get loaded") in favour of the hand-written recursive
 * generator in vax/gencode.c.  We inherit that architecture: doit() walks the
 * tree recursively, asking for a value in a register / on the stack / in the
 * condition codes, and failing back up when it runs out of registers.
 */

# include "mfile2.h"

int acnt, Pflag, bbcnt;

gencode(p) NODE *p; {
	static int warned;

	if (!warned) {
		warned = 1;
		fprintf(stderr,
		    "ccom: ARM64 code generator not implemented yet (Phase 1b)\n");
	}
	/*
	 * Pass 1 has done its work by the time we get here; dropping the tree
	 * on the floor means ccom produces an empty .s file rather than wrong
	 * code, which is the honest failure mode while the backend is a stub.
	 */
}
