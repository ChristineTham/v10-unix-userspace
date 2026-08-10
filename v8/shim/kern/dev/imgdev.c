/*
 * A V8 block device whose platter is a host file.  imgdev.h says why this is
 * in shim/kern/ now and was deliberately not before.
 *
 * The smallest honest driver: a host file, read and written where the buffer
 * header says.  It is SYNCHRONOUS -- the transfer happens inside strategy and
 * iodone() runs before it returns -- which means iowait() at bio.c:426 finds
 * B_DONE already set and never sleeps.  That is a real property of this device
 * rather than a shortcut: a file on APFS has no rotational latency to wait
 * for, and pretending otherwise would need a thread to make tsleep/wakeup do
 * something, i.e. a second machine to be wrong about.
 *
 * IT ALSO MATTERS TO THE SERVER, one layer up, and it is why that server can
 * be single-threaded: nothing in the kernel path sleeps, so a 9P request can
 * be carried to completion between two poll() returns with no interleaving
 * inside code that keeps its state in a global u-area.
 *
 * THE UNITS ARE THE ONE THING THAT CAN BE QUIETLY WRONG.  b_blkno is in
 * 512-byte DISK blocks, not filesystem blocks: getblk stores fsbtodb(dev,blkno)
 * and param.h defines that as `b * CLSIZE' with CLSIZE 2 for a 1024-byte
 * filesystem.  So the byte offset is b_blkno * 512, and a driver that used
 * BSIZE(dev) there would read every block at twice its address -- which for
 * block 1 would land on block 2 and return a plausible-looking wrong
 * superblock.
 */

#include "../h/param.h"

/*
 * param.h's redirects are undone in ONE place; hostok.h says why.  This file
 * needs the host's pread/pwrite, so it needs both halves.
 */
#include "../h/hostok.h"

#include <errno.h>
#include <unistd.h>

#include "../h/conf.h"
#include "../../../src/sys/h/buf.h"
#include "imgdev.h"

int	iodone();
int	v8k_bdconf(struct bdevsw *bd);

static int	imgfd = -1;
static long	nread;			/* transfers the driver actually served */
static long	nwrite;

/*
 * THE PARAMETERS ARE `int' AND NOT `dev_t', AND THAT IS THE FIX RATHER THAN A
 * SUPPRESSION.  struct bdevsw's slots are `int (*)()' -- 1985 K&R, no
 * prototype -- and bio.c calls through them as `(*bdp->d_open)(dev, rw)'.  An
 * argument passed through an unprototyped call gets the DEFAULT ARGUMENT
 * PROMOTIONS, so a dev_t (u_short) arrives as an int.  Declaring the parameter
 * dev_t therefore describes something the caller never sends, and clang says
 * so: `incompatible function pointer types assigning to int (*)() from
 * int (dev_t, int)'.  It is the same rule the Makefile records for findmount
 * and mfind, arriving at a function pointer instead of at a declaration.
 *
 * Worth knowing where this was hiding: tests/streams/fsprobe.c had the same
 * two functions spelled `dev_t' and built fine, because the suite's KFLAGS
 * carry -Wno-incompatible-function-pointer-types for the imported half's
 * sake.  A flag argued for 1985 code was covering ours.
 */
static int
imgopen(int dev, int rw)
{
	return (0);
}

static int
imgclose(int dev, int rw)
{
	return (0);
}

static int
imgstrategy(struct buf *bp)
{
	off_t off;
	ssize_t n;

	off = (off_t)bp->b_blkno * 512;		/* see the units note above */
	if (bp->b_flags & B_READ) {
		n = pread(imgfd, bp->b_un.b_addr, (size_t)bp->b_bcount, off);
		nread++;
	} else {
		n = pwrite(imgfd, bp->b_un.b_addr, (size_t)bp->b_bcount, off);
		nwrite++;
	}
	if (n < 0) {
		bp->b_flags |= B_ERROR;
		bp->b_error = EIO;
		bp->b_resid = bp->b_bcount;
	} else
		bp->b_resid = bp->b_bcount - n;
	iodone(bp);
	return (0);
}

void
v8k_imgrow(struct bdevsw *bd)
{
	bd->d_open = imgopen;
	bd->d_close = imgclose;
	bd->d_strategy = imgstrategy;
	bd->d_dump = 0;
	bd->d_flags = 0;
}

int
v8k_imgattach(int fd)
{
	struct bdevsw bd;

	if (fd < 0) return (-1);
	imgfd = fd;
	nread = nwrite = 0;
	v8k_imgrow(&bd);
	return (v8k_bdconf(&bd));
}

/*
 * The fd is NOT closed here, because this driver did not open it.  Whoever
 * chose O_RDONLY or O_RDWR owns the descriptor; a driver that closed it would
 * be the second thing in the process with an opinion about its lifetime.
 */
void
v8k_imgdetach(void)
{
	imgfd = -1;
}

long
v8k_imgreads(void)
{
	return (nread);
}

long
v8k_imgwrites(void)
{
	return (nwrite);
}
