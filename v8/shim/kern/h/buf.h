/*
 * The block-I/O buffer header -- or rather the two bits of it that mean
 * anything here.  Ours, not Bell Labs'; see shim/kern/h/param.h.
 *
 * V8's h/buf.h is 107 lines describing the VAX buffer cache: `struct buf' with
 * its free-list and device queue links, a b_un union over four kinds of
 * address, b_blkno, b_resid, the Unibus map registers, and thirty-one B_ flags
 * for a disk driver's state machine.  There is no buffer cache here and no
 * disk driver, so importing it would put a description of hardware in the tree
 * to obtain two constants.
 *
 * TWO CONSTANTS IS THE WHOLE OF IT.  streamio.c includes buf.h at :4 and uses
 * B_READ once (:241) and B_WRITE once (:459), both as iomove's direction
 * argument.  It never names `struct buf' -- src/sys/h/inode.h's `struct nx'
 * mentions one, but only as a pointer to an incomplete type, which needs no
 * declaration.
 *
 * The values are upstream's, with the line numbers, because B_WRITE being ZERO
 * is not an arbitrary choice: it is a pseudo-flag, the absence of B_READ, and
 * code that tests `flag == B_WRITE' rather than `!(flag & B_READ)' depends on
 * it.  sys/rdwri.c's iomove does exactly that, and so does ours.
 */

#ifndef V8KERN_BUF_H
#define V8KERN_BUF_H

#define	B_WRITE		0x000000	/* h/buf.h:87 -- non-read pseudo-flag */
#define	B_READ		0x000001	/* h/buf.h:88 -- read when I/O occurs */

#endif /* V8KERN_BUF_H */
