/*
 * The super-block, for the kernel side -- BY FORWARDING TO THE ONE COPY,
 * not by importing a second.
 *
 * THIS FILE IS OURS.  What it contains is one #include, and the reason it
 * exists at all is the most expensive thing §8a step 5's survey got wrong.
 *
 * V8 ships h/filsys.h at TWO paths -- v8/usr/sys/h/filsys.h and
 * v8/usr/include/sys/filsys.h -- and they are the same git blob, byte for
 * byte.  (So are dir.h inode.h ino.h fblk.h buf.h proc.h conf.h user.h
 * systm.h mount.h acct.h vlimit.h param.h.)  The survey costed the kernel
 * headers by line count and by VAX-reference count and concluded they were
 * "1286 lines of ordinary structure".  Neither measure can see what this one
 * is.
 *
 * IT IS AN ON-DISK RECORD.  §8a step 4a narrowed four fields of it --
 * s_time, and both arms of the free-list union -- to v8_i32, because V8's
 * VAX compiler defined NOLONG and a `long' on the disk is four bytes.  That
 * patch lives in src/include/sys/filsys.h and mkfs, icheck, dcheck, fsck,
 * ncheck, quot, dump, restor and dumpdir all read it.
 *
 * So importing the pristine kernel copy into src/sys/h/ would have given the
 * kernel an 8-byte s_time over images mkfs writes with 4 -- step 4a's bug
 * reintroduced on the far side of one disk, WHERE NOTHING IN THIS TREE COULD
 * HAVE SEEN IT, because every reader would still be using the patched header
 * and only the kernel the pristine one.  The two would have disagreed about
 * the superblock's length by hundreds of bytes and each would have been
 * self-consistent.
 *
 * The rule that follows is stronger than "patch it in both places", and it
 * is why this is a forward rather than a copy: A RECORD WRITTEN TO A DISK
 * MUST HAVE EXACTLY ONE DECLARATION IN THE TREE.  Two copies cannot be kept
 * in step by anything except vigilance, and the failure is silent.
 *
 * HOW IT IS REACHED.  src/sys/sys/alloc.c says #include "../h/filsys.h".  A
 * quoted include tries the includer's directory first, so src/sys/h/filsys.h
 * -- which deliberately does not exist -- and then falls through to -I.
 * KERNFLAGS passes -Ishim/kern/dev, and "../h/" from there is this
 * directory.  Same mechanism as shim/kern/h/param.h, and src/sys/PORTING.md
 * has the general statement: an authentic header wins, ours fill gaps.  Here
 * the gap is deliberate.
 *
 * The guard is ours because the header has none; V8 headers predate them,
 * and a repeated struct definition is an error in the gnu89 these files are
 * compiled in.  Same reasoning as param.h's V8KERN_JMP_BUF.
 */

#ifndef V8KERN_FILSYS_H
#define V8KERN_FILSYS_H

#include "../../../src/include/sys/filsys.h"

#endif
