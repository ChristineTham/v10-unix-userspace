/*
 * The free-list block, for the kernel side -- forwarded, not imported.
 *
 * THIS FILE IS OURS, and shim/kern/h/filsys.h beside it carries the full
 * reasoning: one upstream blob at two paths, a DISK RECORD, and therefore
 * exactly one declaration in the tree.
 *
 * struct fblk deserves the forward more than either of its siblings,
 * because it is the one that already caught this tree out once.  It had
 * NEVER BEEN IMPORTED -- src/include/sys/fblk.h was added only when someone
 * checked -- and until then rootfs/usr/include served 1985's, which measured
 * 716 bytes anyway by two coincidences at once: `int' is 32 here as on the
 * VAX, and daddr_t came from a header that HAD been patched.  Right by
 * accident, and invisible to an audit of "the port's headers" because it was
 * not among them.  src/include/PORTING.md has it.
 *
 * A pristine kernel copy would be the same accident a third time, and there
 * is no reason to spend it: alloc.c reads and writes this block, and
 * df_nfree must agree with what mkfs wrote.
 *
 * Reached through KERNFLAGS' -Ishim/kern/dev; see filsys.h.
 */

#ifndef V8KERN_FBLK_H
#define V8KERN_FBLK_H

#include "../../../src/include/sys/fblk.h"

#endif
