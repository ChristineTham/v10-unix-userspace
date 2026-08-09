/*
 * A probe for V8's FILESYSTEM -- PLAN.md §8a step 5, the six files imported in
 * task #54 and built in #55.  This is the first thing that ever EXECUTES them.
 *
 * WHAT IT IS FOR.  §8a step 5 ended with `libv8kern.a has seven new members and
 * imports exactly _longjmp _memcpy _setjmp'.  That is a statement about a build,
 * and a build says nothing about whether alloc.c's free-block map arithmetic is
 * right, whether bmap walks an indirect block correctly, or whether namei can
 * find a name.  Every one of those had run zero times.  This probe drives the
 * whole path Bell Labs wrote --
 *
 *	namei -> fsnami -> dsearch -> bread -> the driver
 *	      -> iget  -> bread -> iexpand
 *	      -> readi -> bmap  -> bread
 *
 * -- against an image that mkfs(8) wrote, and compares the bytes that come back
 * against the bytes that went in.  Writer and reader are different programs
 * from different decades, which is the property that makes the comparison mean
 * something: tests/mkfs already asserts the image matches what THIS PORT
 * believes a V8 filesystem is, and a shared misunderstanding would satisfy both
 * halves of that.  V8's own kernel is the independent reader.
 *
 * WHY THE DRIVER IS HERE AND NOT IN shim/kern/.  CLAUDE.md's unconsumed-
 * component rule: nothing in this port consumes a block device -- the image
 * tools open the image as an ordinary file through v8s_open -- so a driver in
 * the shim would be a component with no caller, which "invents a difference the
 * kernel does not have".  sioprobe.c's loopback and pipe drivers and
 * ttyprobe.c's tty driver are the precedents, and shim/kern/sys/ioconf.c's
 * v8k_bdconf() is the registration hook, argued there.
 *
 * WHY IT IS IN THE STREAMS SUITE.  Because tests/streams is already the
 * src/sys/ suite despite its name: the provenance hashes for stream.c, ttyld.c
 * and partab.c live there, and so does the diff-shape guard for alloc.c's
 * second NOLONG deviation.  Splitting the filesystem's guards from its probe
 * would put two halves of one argument in two files.
 *
 * Prints `key value' lines; run.sh asserts on them and keeps the reason for
 * each next to the assertion.  A DUPLICATED KEY IS SILENT -- run.sh's reader
 * prints every matching line -- so each key below is used once.
 */

#include "../../shim/kern/h/param.h"

/*
 * param.h's redirects are undone in ONE place; shim/kern/h/hostok.h says why.
 * Note the consequence for this file specifically: `mount' is the host's after
 * this, so the kernel's mount table has to be spelled v8k_mount below.
 */
#include "../../shim/kern/h/hostok.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "../../shim/kern/h/proc.h"
#include "../../src/sys/h/dir.h"	/* struct direct, for user.h's u_dent */
#include "../../shim/kern/h/user.h"
#include "../../src/sys/h/inode.h"
#include "../../src/sys/h/mount.h"
#include "../../src/sys/h/buf.h"	/* AUTHENTIC -- see below */
#include "../../shim/kern/h/conf.h"
#include "../../shim/kern/h/filsys.h"

/*
 * THIS PROBE IS COMPILED -DKERNEL AND THAT IS NOT OPTIONAL, unlike the other
 * three in this directory.
 *
 * inode.h:62-68 and buf.h:78-79 declare namei, iget, ialloc, bread, breada and
 * geteblk -- all POINTER-RETURNING -- inside `#ifdef KERNEL'.  Without the flag
 * none of them is declared, and KFLAGS carries
 * -Wno-implicit-function-declaration for the imported half's sake, so each call
 * would silently be an implicit `int' function.  On this target that TRUNCATES
 * THE RETURNED POINTER to 32 bits.
 *
 * That is not hypothetical here: it is the ps -T bug this port already
 * diagnosed, where a "wild pointer 0x53c5c" turned out to be the low half of
 * 0x100053c5c.  A probe that got it wrong would fault inside iget and read as a
 * bug in Bell Labs' code.  -DKERNEL also gives this object the tentative
 * definitions for inode/buf/v8k_mount, which merge with the strong ones in
 * shim/kern/sys/main.c exactly as the imported objects' do.
 *
 * buf.h is included BY ITS AUTHENTIC PATH for the same reason main.c spells it
 * out: struct buf, b_un and the B_ flags are needed, and shim/kern/h/buf.h --
 * which had two constants and no struct -- was deleted in §8a step 5c.
 */
#ifndef KERNEL
#error fsprobe.c must be compiled -DKERNEL; see the comment above
#endif

/*
 * K&R declarations, matching the ones the authentic headers already make where
 * they make any.  probe.c says why a modern prototype for one of these would be
 * a CONFLICTING type rather than a stricter one.  v8k_kinit and v8k_bdconf are
 * ours and get real prototypes.
 */
int	readi(), iput(), brelse(), iodone(), bwrite(), schar(), iupdat();
int	bhinit(), ihinit(), binit(), iinit();
daddr_t	bmap();

int	v8k_bdconf(struct bdevsw *bd);
void	v8k_bdunconf(void);
int	v8k_kinit(dev_t dev);

/* ------------------------------------------------------------------------
 * THE BLOCK DRIVER
 *
 * The smallest honest one: a host file, read and written where the buffer
 * header says.  It is SYNCHRONOUS -- the transfer happens inside strategy and
 * iodone() runs before it returns -- which means iowait() at bio.c:426 finds
 * B_DONE already set and never sleeps.  That is a real property of this device
 * rather than a shortcut: a file on APFS has no rotational latency to wait for,
 * and pretending otherwise would need a thread to make tsleep/wakeup do
 * something, i.e. a second machine to be wrong about.
 *
 * THE UNITS ARE THE ONE THING THAT CAN BE QUIETLY WRONG.  b_blkno is in
 * 512-byte DISK blocks, not filesystem blocks: getblk stores fsbtodb(dev,blkno)
 * and param.h defines that as `b * CLSIZE' with CLSIZE 2 for a 1024-byte
 * filesystem.  So the byte offset is b_blkno * 512, and a driver that used
 * BSIZE(dev) there would read every block at twice its address -- which for
 * block 1 would land on block 2 and return a plausible-looking wrong
 * superblock.
 */
static int	imgfd = -1;
static long	nread;			/* transfers the driver actually served */
static long	nwrite;

static int
diskopen(dev_t dev, int rw)
{
	return (0);
}

static int
diskclose(dev_t dev, int rw)
{
	return (0);
}

static int
diskstrategy(struct buf *bp)
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

/* ------------------------------------------------------------------------
 * Helpers.  Each namei() below is a fresh call with u_error cleared, because
 * ttyprobe.c learned the hard way that a case which inherits state from the
 * one before it reports a plausible wrong answer and makes the NEXT case look
 * broken.  A case has to be a pure function of what it asked for.
 */
static char	pathbuf[256];

static struct inode *
lookup(char *path)
{
	strncpy(pathbuf, path, sizeof(pathbuf) - 1);
	pathbuf[sizeof(pathbuf) - 1] = '\0';
	u.u_dirp = pathbuf;
	u.u_error = 0;
	return (namei(schar, (struct argnamei *)0, 1));
}

/*
 * Read a whole file through readi, into caller storage.  u_segflg is 1 --
 * "system space" -- which is what sends rdwri.c:104 down the bcopy arm instead
 * of copyout.  Returns the byte count, or -1 with u_error set.
 */
static int
readfile(struct inode *ip, char *dst, int max)
{
	u.u_base = dst;
	u.u_count = max;
	u.u_offset = 0;
	u.u_segflg = 1;
	u.u_error = 0;
	readi(ip);
	if (u.u_error)
		return (-1);
	return (max - (int)u.u_count);
}

/* A cheap content check that survives being printed on one line. */
static unsigned long
sum(char *p, int n)
{
	unsigned long s = 0;
	int i;

	for (i = 0; i < n; i++)
		s = s * 31 + (unsigned char)p[i];
	return (s);
}

/*
 * Print n bytes as one line, mapping anything unprintable to '.'.  Needed
 * rather than decorative: run.sh's readers are `awk $1==k', so a value
 * containing the file's own newline would silently become a two-line answer,
 * and the diagnostic for that is a `got' field that looks truncated.  Same
 * family as the duplicated-key trap run.sh already records.
 */
static void
prbytes(char *key, char *p, int n)
{
	int i, c;

	printf("%s ", key);
	for (i = 0; i < n; i++) {
		c = (unsigned char)p[i];
		putchar((c >= 040 && c < 0177) ? c : '.');
	}
	putchar('\n');
}

static char	big[64 * 1024];

int
main(int argc, char **argv)
{
	struct bdevsw	bd;
	struct bdevsw	bad;
	struct filsys	*fp;
	struct inode	*ip;
	struct buf	*bp;
	dev_t		dev;
	int		bdmaj, n;
	daddr_t		b0, b10;

	if (argc < 2) {
		fprintf(stderr, "usage: fsprobe image\n");
		return (2);
	}
	if ((imgfd = open(argv[1], O_RDWR)) < 0) {
		fprintf(stderr, "fsprobe: cannot open %s\n", argv[1]);
		return (2);
	}

	/* ---------------------------------------------------------------
	 * 1. Registration.  Before anything is mounted, nblkdev must be 0 --
	 * that is the state §8a step 5 shipped, and it is what makes
	 * bio.c:352 reject every device rather than index an empty table.
	 */
	printf("nblkdev-before %d\n", nblkdev);

	/*
	 * A row upstream's own counter would stop at.  main.c:218 ends the
	 * table on a null d_open, so a row without one could not have come out
	 * of config(8); ioconf.c refuses it, and this asks whether it does.
	 */
	bad.d_open = 0;
	bad.d_close = diskclose;
	bad.d_strategy = diskstrategy;
	bad.d_dump = 0;
	bad.d_flags = 0;
	printf("bdconf-rejects-nullopen %d\n", v8k_bdconf(&bad));

	bad.d_open = diskopen;
	bad.d_strategy = 0;
	printf("bdconf-rejects-nullstrat %d\n", v8k_bdconf(&bad));
	printf("nblkdev-after-rejects %d\n", nblkdev);

	bd.d_open = diskopen;
	bd.d_close = diskclose;
	bd.d_strategy = diskstrategy;
	bd.d_dump = 0;
	bd.d_flags = 0;
	bdmaj = v8k_bdconf(&bd);
	printf("bdconf-major %d\n", bdmaj);
	printf("nblkdev-after %d\n", nblkdev);

	/*
	 * MINOR 0, AND THE MINOR NUMBER IS THE BLOCK SIZE.  param.h defines
	 * BITFS(dev) as `dev & 64', so bit 6 of the device number selects a
	 * 4096-byte filesystem over a 1024-byte one.  mkfs writes 1024-byte
	 * blocks here, so the minor must keep that bit clear or every
	 * BSIZE/BMASK/itod in the kernel would describe a different disk.
	 */
	dev = makedev(bdmaj, 0);
	printf("bitfs %d\n", BITFS(dev) ? 1 : 0);
	printf("bsize %d\n", BSIZE(dev));

	/* ---------------------------------------------------------------
	 * 2. Startup.  This is the line that first executes bio.c and iget.c:
	 * binit weaves the free lists, iinit breads the superblock through the
	 * driver and hangs it on a mount, and the two igets read the root
	 * inode out of the ilist.
	 */
	if (v8k_kinit(dev) < 0) {
		printf("kinit -1\n");
		return (1);
	}
	printf("kinit 0\n");
	printf("reads-after-kinit %ld\n", nread);

	/*
	 * getfs() is the function that PANICS ("getfs") when nothing is
	 * mounted, so reaching this line at all is the assertion; the numbers
	 * say it found the right superblock.
	 */
	fp = getfs(dev);
	printf("fs-fsize %ld\n", (long)fp->s_fsize);
	printf("fs-isize %d\n", (int)fp->s_isize);
	printf("fs-fsmnt %s\n", fp->s_fsmnt);
	printf("fs-time-nonzero %d\n", fp->s_time != 0 ? 1 : 0);

	/*
	 * iinit pins the superblock with B_LOCKED so it can never be reused.
	 * Asserted because if it were reusable the free-block map would be
	 * silently re-read from disk mid-allocation.
	 */
	bp = incore(dev, (daddr_t)SUPERB) ? bread(dev, (daddr_t)SUPERB) : 0;
	printf("superb-locked %d\n", (bp && (bp->b_flags & B_LOCKED)) ? 1 : 0);
	if (bp)
		brelse(bp);

	/* ---------------------------------------------------------------
	 * 3. The root inode.
	 */
	printf("root-ino %ld\n", (long)rootdir->i_number);
	printf("root-isdir %d\n", (rootdir->i_mode & IFMT) == IFDIR ? 1 : 0);
	printf("root-count %d\n", rootdir->i_count);
	printf("root-nlink %d\n", rootdir->i_nlink);
	printf("cdir-is-root %d\n", u.u_cdir->i_number == rootdir->i_number);

	/* ---------------------------------------------------------------
	 * 4. namei.  "/" is the degenerate path -- fsnami's null-name arm at
	 * :245 -- and it must give back the root rather than an error.
	 */
	ip = lookup("/");
	printf("nami-slash %ld\n", ip ? (long)ip->i_number : -1L);
	if (ip)
		iput(ip);

	ip = lookup("/hello");
	if (ip == NULL) {
		printf("hello-ino -1\n");
		printf("hello-err %d\n", u.u_error);
	} else {
		printf("hello-ino %ld\n", (long)ip->i_number);
		printf("hello-mode %o\n", ip->i_mode);
		printf("hello-size %ld\n", (long)ip->i_size);

		n = readfile(ip, big, sizeof(big));
		printf("hello-n %d\n", n);
		if (n > 0)
			prbytes("hello-text", big, n);
		iput(ip);
	}

	/* Two components, so fsnami loops and dsearch runs on a subdirectory. */
	ip = lookup("/sub/deep");
	if (ip == NULL) {
		printf("deep-ino -1\n");
		printf("deep-err %d\n", u.u_error);
	} else {
		printf("deep-ino %ld\n", (long)ip->i_number);
		printf("deep-size %ld\n", (long)ip->i_size);

		/*
		 * THE INDIRECT BLOCK, WHICH IS THE POINT OF A BIG FILE.
		 * blocks 0..NADDR-4 (0..9) live in the inode; block 10 is the
		 * first that makes bmap walk the single indirect block, and
		 * that walk is `daddr_t *bap' over a bread'd buffer -- the
		 * same shape as alloc.c's free-map walk, which is where this
		 * import's second NOLONG deviation was found.
		 */
		b0 = bmap(ip, (daddr_t)0, B_READ);
		b10 = bmap(ip, (daddr_t)10, B_READ);
		printf("bmap-0-valid %d\n", b0 > 0 ? 1 : 0);
		printf("bmap-10-valid %d\n", b10 > 0 ? 1 : 0);
		printf("bmap-differs %d\n", b0 != b10 ? 1 : 0);

		n = readfile(ip, big, sizeof(big));
		printf("deep-n %d\n", n);
		printf("deep-sum %lu\n", sum(big, n > 0 ? n : 0));

		/*
		 * Head and tail as well as the sum, because a bmap that
		 * returned the wrong INDIRECT block gives a right beginning
		 * and a wrong end -- and a single scalar that is wrong tells
		 * you nothing about where it went wrong.
		 */
		if (n >= 32) {
			prbytes("deep-head", big, 16);
			prbytes("deep-tail", big + n - 16, 16);
		}

		/*
		 * AND THE BYTES THEMSELVES, WRITTEN OUT FOR cmp(1).  The sum
		 * above is a hash this file invented; a hash agreeing proves
		 * only that two implementations of the same invention agree.
		 * run.sh cmp's this against the file mkfs was given, which is
		 * the actual claim -- V8's kernel read back exactly what went
		 * in -- and needs nothing to be reimplemented anywhere.
		 */
		if (argc > 2 && n > 0) {
			int ofd = open(argv[2], O_WRONLY|O_CREAT|O_TRUNC, 0644);

			printf("deep-written %d\n",
			    (ofd >= 0 && write(ofd, big, (size_t)n) == n) ? 1 : 0);
			if (ofd >= 0)
				close(ofd);
		}
		iput(ip);
	}

	/* ---------------------------------------------------------------
	 * 5. The failure paths, which a probe that only reads a file never
	 * touches.  Both are namei returning NULL, and the two differ ONLY in
	 * u_error -- so a lookup that failed for the wrong reason would look
	 * identical without this pair.
	 */
	ip = lookup("/nosuchfile");
	printf("enoent-null %d\n", ip == NULL ? 1 : 0);
	printf("enoent-err %d\n", u.u_error);
	if (ip)
		iput(ip);

	ip = lookup("/hello/x");
	printf("notdir-null %d\n", ip == NULL ? 1 : 0);
	printf("notdir-err %d\n", u.u_error);
	if (ip)
		iput(ip);

	/* ---------------------------------------------------------------
	 * 6. The buffer cache, asked directly.  A second bread of a block that
	 * is already in core must not reach the driver -- that is the whole
	 * purpose of bio.c, and it is invisible to every case above, which
	 * would pass identically with a cache that never hit.
	 */
	{
		long before, after;

		bp = bread(dev, (daddr_t)SUPERB);
		brelse(bp);
		before = nread;
		bp = bread(dev, (daddr_t)SUPERB);
		after = nread;
		printf("cache-b-cache %d\n", (bp->b_flags & B_CACHE) ? 1 : 0);
		printf("cache-no-io %d\n", after == before ? 1 : 0);
		brelse(bp);

		/*
		 * And the negative control: a block NOT in core must reach the
		 * driver.  Without this the case above passes on a cache that
		 * never does any I/O at all.
		 */
		before = nread;
		bp = bread(dev, (daddr_t)(fp->s_isize + 3));
		after = nread;
		printf("cache-miss-io %d\n", after == before + 1 ? 1 : 0);
		brelse(bp);
	}

	/* ---------------------------------------------------------------
	 * 7. Nothing was written.  This probe reads; if a write happened, an
	 * image tests/mkfs validated has been modified underneath it and the
	 * next suite's answer would depend on this one having run.  That is
	 * the cross-suite version of the litter problem tests/crash-probe.sh
	 * records.
	 */
	printf("writes %ld\n", nwrite);
	printf("reads-total-positive %d\n", nread > 0 ? 1 : 0);

	/*
	 * 8. NO INODE WAS LEAKED, which is a real bug class and the only thing
	 * here that constrains the inode table at all.
	 *
	 * Every lookup above is paired with an iput, and fsnami releases each
	 * parent as it descends, so when the dust settles exactly ONE inode
	 * should be held: the root, with i_count 2 because rootdir and u_cdir
	 * are two igets of the same (dev, ROOTINO) and iget returns the same
	 * structure to the second caller rather than taking a second slot.
	 *
	 * MEASURED, and the measurement is why this case exists: mutating
	 * NINODE from 80 down to 3 and then to 2 changed nothing -- 308 passed
	 * either way -- and only NINODE 1 failed.  So the whole read path needs
	 * TWO slots, no test constrains the table's size, and a missing iput
	 * would have been invisible.  Counting what is held is the assertion
	 * that a size assertion could never have been.
	 */
	{
		int held = 0, i;

		for (i = 0; i < ninode; i++)
			if (inode[i].i_count != 0)
				held++;
		printf("inodes-held %d\n", held);
		printf("root-count-final %d\n", rootdir->i_count);
	}

	/*
	 * Reaching here means none of v8fs.c's five PANIC services was called
	 * and neither getfs nor namei panicked, since panic() does not return.
	 */
	printf("completed 1\n");
	return (0);
}
