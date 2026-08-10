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
 * THE DRIVER USED TO BE HERE AND IS NOW IN shim/kern/dev/imgdev.c, and both
 * halves of that are the unconsumed-component rule rather than a change of
 * mind.  It was here because nothing in this port consumed a block device -- so
 * a driver in the shim would have been a component with no caller, which
 * "invents a difference the kernel does not have".  §8a step 5e supplies the
 * caller: the v8fs server holds an image open and serves it over 9P.
 *
 * SHARING IT WITH THE SERVER IS THE PART WORTH HAVING.  The cases below drive
 * namei/iget/bmap/readi/writei down to a driver 236 times; if the server had
 * one of its own, not one of them would say anything about it.  The three
 * bdevsw rejection cases still build their rows from the REAL pointers, via
 * v8k_imgrow, so they stay cases about v8k_bdconf rather than about three local
 * stubs.  sioprobe.c's loopback and pipe drivers and ttyprobe.c's tty driver
 * are still the precedent for a driver that has no consumer outside its probe.
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
#include "../../src/sys/h/vlimit.h"	/* LIM_FSIZE and INFINITY -- §8a step 5d */

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

/*
 * §8a step 5d, the write half.  ONE OF THESE IS SPELLED v8k_ AND IT IS NOT A
 * SHIM FUNCTION -- it is Bell Labs' ialloc(), reached by the name param.h
 * renames it to.  hostok.h:43-52 #undefs the redirects so this file can have
 * <stdlib.h> as well, and the three names that costs us are `free', `ialloc'
 * and `time', which belong to the HOST in this translation unit.  Calling
 * free(dev, bno) would compile -- the host's takes one pointer, and this is
 * K&R -- and hand a device number to the C library's allocator.  The file's
 * header comment records the same trap for `mount'.
 *
 * THE COUNT ABOVE SAID THREE, AND IT WAS COUNTING THE RENAMED NAMES RATHER
 * THAN THE DECLARATIONS BELOW.  Only ialloc is declared here; free and time
 * are never called from this file.  It also declared itrunc() and v8k_free()
 * that nothing calls -- an unconsumed declaration standing in for a trap
 * instead of defending against one, which is the unconsumed-component rule
 * applied to a line rather than to a file.  Both removed, found by the
 * lp64-auditor.
 */
int	writei(), update(), bflush();
struct inode	*v8k_ialloc();

int	v8k_bdconf(struct bdevsw *bd);
void	v8k_bdunconf(void);
int	v8k_kinit(dev_t dev, int ronly);

/*
 * The block driver is shim/kern/dev/imgdev.c now -- see the header comment.
 * imgdev.h is reached through -Ishim/kern/dev, the same way the seam's other
 * machine-dependent headers are.
 */
#include "imgdev.h"

static int	imgfd = -1;

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

/*
 * The write side of the same thing, §8a step 5d.  Returns the byte count
 * actually written, or -1 with u_error set.
 *
 * u_segflg 1 matters MORE here than it does for the read.  rdwri.c:191-197 is
 *
 *	if (u.u_segflg != 1) { if (copyin(...)) { u.u_error = EFAULT; ... } }
 *	else bcopy(u.u_base, bp->b_un.b_addr+on, n);
 *
 * -- and shim/kern/sys/subr.c's copyin is a bcopy with a NULL guard, so BOTH
 * arms would work and the flag would look like a formality.  It is not: it is
 * what fsnami sets at nami.c:251 before it writes a directory entry, so a
 * probe that left it 0 would exercise a different arm from the one the kernel
 * uses on itself.
 */
static int
writefile(struct inode *ip, off_t off, char *src, int len)
{
	u.u_base = src;
	u.u_count = len;
	u.u_offset = off;
	u.u_segflg = 1;
	u.u_error = 0;
	writei(ip);
	if (u.u_error)
		return (-1);
	return (len - (int)u.u_count);
}

/*
 * Read a fixed span out of a file, so a write can be checked where it landed
 * rather than only from offset 0.
 */
static int
readat(struct inode *ip, off_t off, char *dst, int len)
{
	u.u_base = dst;
	u.u_count = len;
	u.u_offset = off;
	u.u_segflg = 1;
	u.u_error = 0;
	readi(ip);
	if (u.u_error)
		return (-1);
	return (len - (int)u.u_count);
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
	v8k_imgrow(&bad);
	bad.d_open = 0;
	printf("bdconf-rejects-nullopen %d\n", v8k_bdconf(&bad));

	v8k_imgrow(&bad);
	bad.d_strategy = 0;
	printf("bdconf-rejects-nullstrat %d\n", v8k_bdconf(&bad));
	printf("nblkdev-after-rejects %d\n", nblkdev);

	bdmaj = v8k_imgattach(imgfd);
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
	/*
	 * RONLY 0, AND SECTION 8 IS WHY.  §8a step 5f gave kinit the argument
	 * fsmount() reads out of mount(2); this probe creates a file, drains
	 * the superblock's free list and deletes it again, so it is the
	 * read-WRITE caller by construction.  Section 8h-bis sets s_ronly by
	 * hand afterwards to reach access()'s arm and clears it again, which
	 * is a different thing from mounting read-only and stays where it is.
	 */
	if (v8k_kinit(dev, 0) < 0) {
		printf("kinit -1\n");
		return (1);
	}
	printf("kinit 0\n");
	printf("reads-after-kinit %ld\n", v8k_imgreads());

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
		before = v8k_imgreads();
		bp = bread(dev, (daddr_t)SUPERB);
		after = v8k_imgreads();
		printf("cache-b-cache %d\n", (bp->b_flags & B_CACHE) ? 1 : 0);
		printf("cache-no-io %d\n", after == before ? 1 : 0);
		brelse(bp);

		/*
		 * And the negative control: a block NOT in core must reach the
		 * driver.  Without this the case above passes on a cache that
		 * never does any I/O at all.
		 */
		before = v8k_imgreads();
		bp = bread(dev, (daddr_t)(fp->s_isize + 3));
		after = v8k_imgreads();
		printf("cache-miss-io %d\n", after == before + 1 ? 1 : 0);
		brelse(bp);
	}

	/* ---------------------------------------------------------------
	 * 7. THE READ PATH WROTE NOTHING, asserted here rather than at the end
	 * because everything after this line writes on purpose.
	 *
	 * THIS CASE HAS NOW BEEN WRONG ABOUT ITSELF TWICE, and the second time
	 * was measured rather than argued.
	 *
	 * It first said a write would modify "an image tests/mkfs validated ...
	 * underneath it".  The image is built fresh by run.sh in $TMP for this
	 * probe alone, so there was no shared artefact to protect.
	 *
	 * The replacement said the read path "reaches no bdwrite and dirties no
	 * buffer".  IT DIRTIES ONE ON EVERY LOOKUP.  rdwri.c:50 is
	 * `ip->i_flag |= IACC' at the top of readi, and iput's IUPDAT
	 * (iget.c:198) then writes the inode back -- with bdwrite, because
	 * waitfor is 0.  Mutating bio.c's bdwrite to take bawrite's synchronous
	 * arm turned this case red, which is how it was found; the claim was
	 * plausible, cited nothing, and was false.
	 *
	 * So what it asserts is exactly bdwrite's contract and nothing more: a
	 * read leaves dirty buffers IN THE CACHE and sends nothing to the
	 * DEVICE.  That is still worth having -- it is what says the cache is a
	 * write-back cache rather than a write-through one -- and it is the
	 * pair to `w-inplace-delayed' below, which asserts the same thing for a
	 * write and is the case that names it.
	 */
	printf("writes-after-read %ld\n", v8k_imgwrites());
	printf("reads-total-positive %d\n", v8k_imgreads() > 0 ? 1 : 0);

	/* ---------------------------------------------------------------
	 * 8. THE WRITE HALF -- §8a step 5d, and none of it had ever executed.
	 *
	 * Step 5c drove namei -> iget -> bmap -> readi -> bread.  Every one of
	 * those has a sibling on this side that the read path structurally
	 * cannot reach: bmap's ALLOCATING arm, alloc() and free(), ialloc() and
	 * ifree(), writei, itrunc, and nami.c's NI_CREAT and NI_DEL.
	 *
	 * THE INSTRUMENT IS THE SUPERBLOCK'S OWN ACCOUNTING, not a count of
	 * device writes.  s_tfree and s_tinode are what alloc/free and
	 * ialloc/ifree maintain, they are exact, and they are what icheck
	 * independently recomputes at the end of the suite by walking the
	 * image.  A device-write count would depend on when the 32-buffer cache
	 * happened to evict something, which is the host-property class the
	 * suites are swept for.  Device counts appear in exactly one case
	 * below, where the point IS that a delayed write does not do one.
	 */
	{
		long		tfree0, tinode0, w0;
		long		tfreeA, tinodeA;
		struct inode	*nip;
		struct argnamei	arg;
		int		i, k, got;
		char		small[64];

		tfree0 = (long)fp->s_tfree;
		tinode0 = (long)fp->s_tinode;
		/*
		 * tfreeA IS THE BASELINE THE ROUND-TRIP CASE COMPARES
		 * AGAINST, and it is set here as well as in 8b/8c/8f because
		 * both of those live inside `if (... != NULL)'.  With
		 * /hello missing AND NI_CREAT failing it was read
		 * uninitialised -- so on exactly the run that needs a
		 * diagnosis, the strongest case in this section reported an
		 * arbitrary 1 or 0.  Seeding it with tfree0 also makes it
		 * MEAN something in that case: nothing was allocated, so
		 * nothing should have been freed.  clang says so under
		 * -Wconditional-uninitialized, which is not in -Wall and so
		 * not in KFLAGS; found by the lp64-auditor.
		 */
		tfreeA = tfree0;
		printf("w-tfree-start-positive %d\n", tfree0 > 0 ? 1 : 0);
		printf("w-tinode-start-positive %d\n", tinode0 > 0 ? 1 : 0);

		/*
		 * THE U-AREA'S FILE-SIZE LIMIT, ASSERTED BEFORE ANYTHING USES
		 * IT, because it is what §8a step 5d cost a debugging round
		 * to.  writei's IFREG arm is
		 *
		 *	u.u_offset + u.u_count > u.u_limit[LIM_FSIZE]
		 *
		 * and out of bss that array is all zero, so `0 + 5 > 0' is
		 * true and EVERY write to a regular file fails.  It failed
		 * with EMFILE -- upstream's own choice at rdwri.c:167, and
		 * "too many open files" points at the file table rather than
		 * at a limit nobody had set.
		 *
		 * shim/kern/sys/main.c's v8k_uinit() sets it from upstream's
		 * main.c:62-77, and this case is what says v8k_kinit called
		 * it.  A relation the port controls end to end: the value is
		 * vlimit.h's INFINITY, not anything the host decides.
		 */
		printf("w-limit-fsize %d\n",
		    u.u_limit[LIM_FSIZE] == INFINITY ? 1 : 0);
		printf("w-limit-cmask %d\n", u.u_cmask == CMASK ? 1 : 0);

		/* -----------------------------------------------------
		 * 8a. Overwrite inside a block that already exists.
		 *
		 * /hello is 27 bytes, so block 0 is allocated and blocks 1
		 * upward are not.  A 5-byte write at offset 0 therefore
		 * allocates NOTHING, and writei takes its bread + bdwrite arm
		 * (rdwri.c:187 and :222) because n+on is not BSIZE.
		 *
		 * bdwrite IS THE ONE PLACE A DEVICE COUNT IS THE ASSERTION.
		 * It sets B_DELWRI|B_DONE and brelse()s -- rdwri.c hands the
		 * block to the cache and the cache does not hand it to the
		 * driver.  So `writes' must not move here, and that is the
		 * only observable difference between a delayed write and a
		 * synchronous one.
		 */
		ip = lookup("/hello");
		if (ip == NULL) {
			printf("w-hello-found 0\n");
		} else {
			printf("w-hello-found 1\n");
			w0 = v8k_imgwrites();
			n = writefile(ip, (off_t)0, "HELLO", 5);
			printf("w-inplace-n %d\n", n);
			printf("w-inplace-noalloc %d\n",
			    (long)fp->s_tfree == tfree0 ? 1 : 0);
			printf("w-inplace-delayed %d\n",
			    v8k_imgwrites() == w0 ? 1 : 0);

			got = readat(ip, (off_t)0, small, 5);
			small[got > 0 ? got : 0] = '\0';
			printf("w-inplace-readback %d\n",
			    (got == 5 && strncmp(small, "HELLO", 5) == 0)
			    ? 1 : 0);

			/* ---------------------------------------------
			 * 8b. Extend into a direct block that does not
			 * exist yet: bmap's `nb == 0' arm at subr.c:26-38,
			 * which is the first line of alloc() this port has
			 * ever run.  One block, so s_tfree drops by exactly
			 * one -- and i_size becomes the offset plus the
			 * count, because writei:224-226 only grows it.
			 */
			tfreeA = (long)fp->s_tfree;
			n = writefile(ip, (off_t)2048, "second", 6);
			printf("w-extend-n %d\n", n);
			printf("w-extend-alloc1 %d\n",
			    tfreeA - (long)fp->s_tfree == 1 ? 1 : 0);
			printf("w-extend-size %ld\n", (long)ip->i_size);

			/*
			 * AND THE HOLE READS AS ZERO.  Block 1 was skipped,
			 * so it has no disk block at all, and reading it must
			 * not hand back whatever was on the platter.
			 *
			 * THE MECHANISM IS NOT WHAT IT LOOKS LIKE, and this
			 * comment said the wrong thing until the guard was
			 * read.  bmap for B_READ on an unallocated block does
			 * not return 0 -- subr.c:31 is
			 * `if(rwflg==B_READ || (bp = alloc(...))==NULL)
			 *  return((daddr_t)-1)', i.e. MINUS ONE -- and
			 * rdwri.c:85-87 answers that with
			 * `bp = geteblk(); clrbuf(bp);', a buffer attached to
			 * no device at all.  So the zeros come from an empty
			 * buffer, not from a cleared block, and no I/O
			 * happens.  Same shape as the ttyld tab case: the
			 * answer that surprises you is the one to go and read
			 * the guard for.
			 */
			got = readat(ip, (off_t)1100, small, 8);
			for (i = 0, k = 0; i < got; i++)
				if (small[i] != 0)
					k++;
			printf("w-hole-zero %d\n", (got == 8 && k == 0) ? 1 : 0);

			/* ---------------------------------------------
			 * 8c. Past block 9, so bmap has to allocate the
			 * INDIRECT BLOCK as well as the data block -- two
			 * allocations for one logical block, subr.c:69-80
			 * then :91-113.  s_tfree drops by exactly 2, and
			 * that pair is the only thing that distinguishes
			 * this from 8b.
			 *
			 * The indirect one is bwrite (synchronous, "so that
			 * indirect blocks never point at garbage") while the
			 * data block is bdwrite, so a device count here
			 * would be 1 -- true, and fragile, because getblk
			 * may also evict.  The block count is not.
			 */
			tfreeA = (long)fp->s_tfree;
			n = writefile(ip, (off_t)(10 * 1024), "indirect", 8);
			printf("w-indirect-n %d\n", n);
			printf("w-indirect-alloc2 %d\n",
			    tfreeA - (long)fp->s_tfree == 2 ? 1 : 0);
			b10 = bmap(ip, (daddr_t)10, B_READ);
			printf("w-indirect-bmap %d\n", b10 > 0 ? 1 : 0);

			got = readat(ip, (off_t)(10 * 1024), small, 8);
			printf("w-indirect-readback %d\n",
			    (got == 8 && strncmp(small, "indirect", 8) == 0)
			    ? 1 : 0);
			iput(ip);
		}

		/* -----------------------------------------------------
		 * 8c-bis. THE SIGNAL THAT MUST NOT BECOME A BROADCAST.
		 *
		 * writei's over-limit arm is `psignal(u.u_procp, SIGXFSZ)'
		 * followed by EMFILE, and this port's psignal is a REAL
		 * kill(2) rather than a bit set in p_sig.  So the arm is one
		 * of the few places where imported kernel code reaches out and
		 * touches the host, and it deserves to be exercised on purpose
		 * rather than only by accident.
		 *
		 * IT WAS FOUND BY ACCIDENT FIRST, and the accident is why this
		 * case exists.  Mutating v8k_uinit to leave u_limit at 0 made
		 * every write take this arm -- and the mutation did not
		 * produce a failing case, it KILLED THE TEST RUNNER AND THE
		 * SHELL ABOVE IT.  fsprobe does not call v8k_procinit (it
		 * stands up a filesystem, not a process description), so
		 * v8k_hostpid was 0, v8k_hostof returned 0, psignal's guard
		 * was `hp < 0', and the syscall was kill(0, SIGXFSZ) -- the
		 * whole process group.  Both functions now refuse a host pid
		 * of 0; shim/kern/sys/fio.c has the account.
		 *
		 * The assertion is that the process is STILL HERE afterwards,
		 * which is a strange-looking thing to assert and is the only
		 * thing that can be asserted about a signal that must not
		 * arrive.  It is paired with EMFILE so that a writei which
		 * simply never reached the arm would not pass it.
		 */
		u.u_limit[LIM_FSIZE] = 8;	/* absurdly small, on purpose */
		ip = lookup("/hello");
		if (ip == NULL) {
			printf("w-xfsz-found 0\n");
		} else {
			n = writefile(ip, (off_t)0, "0123456789", 10);
			printf("w-xfsz-refused %d\n", n < 0 ? 1 : 0);
			printf("w-xfsz-emfile %d\n", u.u_error);
			iput(ip);
		}
		u.u_limit[LIM_FSIZE] = INFINITY;
		printf("w-xfsz-survived 1\n");

		/* -----------------------------------------------------
		 * 8d. ialloc and ifree, asked directly and in isolation.
		 *
		 * ialloc's fast arm is `ino = fp->s_inode[--fp->s_ninode]'
		 * (alloc.c:298), the superblock's cache of known-free inode
		 * numbers, which mkfs filled.  A fresh inode must have
		 * i_mode 0 -- that is ialloc's own test at :302 for whether
		 * the number it pulled is really free -- and a number above
		 * ROOTINO, since :302 refuses to hand out 1 or 2.
		 *
		 * FREEING IT IS NOT ifree(): it is iput() with i_nlink 0,
		 * which is how every caller does it, and it reaches ifree
		 * through iget.c:196.  Calling ifree by hand would leave the
		 * inode in core and prove less.
		 */
		tinodeA = (long)fp->s_tinode;
		nip = v8k_ialloc(dev);
		if (nip == NULL) {
			printf("w-ialloc-ok 0\n");
		} else {
			printf("w-ialloc-ok 1\n");
			printf("w-ialloc-mode0 %d\n", nip->i_mode == 0 ? 1 : 0);
			printf("w-ialloc-above-root %d\n",
			    (long)nip->i_number > (long)ROOTINO ? 1 : 0);
			printf("w-ialloc-tinode %d\n",
			    tinodeA - (long)fp->s_tinode == 1 ? 1 : 0);

			nip->i_nlink = 0;
			iput(nip);
			printf("w-ifree-tinode %d\n",
			    (long)fp->s_tinode == tinodeA ? 1 : 0);
		}

		/* -----------------------------------------------------
		 * 8e. namei with NI_CREAT -- the whole create path in one
		 * call: dsearch fails, nami.c:494 runs ialloc, iupdat writes
		 * the new inode, and writei(dp) puts the name into the
		 * PARENT DIRECTORY.  That last one is the part no direct
		 * ialloc can reach, and it is a write to a directory, which
		 * is a different arm of writei from a write to a file.
		 *
		 * flagp->mode CARRIES NO IFMT, on purpose: nami.c:503-504
		 * adds IFREG when the caller supplies none, and asking for
		 * that arm is worth more than spelling IFREG here.
		 *
		 * access(dp, IWRITE) AT :496 IS WHY §8a step 5d RESTORED THE
		 * s_ronly CHECK in shim/kern/sys/v8fs.c.  This is the first
		 * call in the port's history that reaches it.
		 */
		arg.flag = NI_CREAT;
		arg.ino = 0;
		arg.idev = 0;
		arg.mode = 0644;
		strncpy(pathbuf, "/created", sizeof(pathbuf) - 1);
		pathbuf[sizeof(pathbuf) - 1] = '\0';
		u.u_dirp = pathbuf;
		u.u_error = 0;
		nip = namei(schar, &arg, 1);
		if (nip == NULL) {
			printf("w-creat-ok 0\n");
			printf("w-creat-err %d\n", u.u_error);
		} else {
			printf("w-creat-ok 1\n");
			printf("w-creat-isreg %d\n",
			    (nip->i_mode & IFMT) == IFREG ? 1 : 0);
			printf("w-creat-perm %o\n", nip->i_mode & 07777);
			printf("w-creat-nlink %d\n", nip->i_nlink);
			printf("w-creat-size %ld\n", (long)nip->i_size);

			/* ---------------------------------------------
			 * 8f. DRAIN THE SUPERBLOCK'S FREE LIST, which is
			 * the case this whole section exists for.
			 *
			 * alloc() hands out fp->s_free[--s_nfree] until
			 * s_nfree reaches 0, and only THEN does it follow
			 * the chain (alloc.c:163-176): bread the block it
			 * just handed out, take df_nfree and df_free from
			 * it, and carry on.  That is V7's free-list format,
			 * struct fblk, 716 bytes -- the one sys/fblk.h had
			 * never been imported for -- and mkfs wrote it.
			 *
			 * So this is the second half of the same claim step
			 * 5c made about data: a format one program wrote in
			 * 2025 and a kernel from 1985 walks.  Reaching it
			 * needs MORE THAN s_nfree allocations, so the count
			 * is read from the superblock rather than assumed,
			 * and the drain is asserted by s_nfree going UP --
			 * a refill is the only thing that can raise it.
			 */
			k = (int)fp->s_nfree;
			printf("w-nfree-before %d\n", k);
			tfreeA = (long)fp->s_tfree;

			for (i = 0; i < sizeof(big); i++)
				big[i] = (char)('A' + (i % 26));

			got = 0;
			for (i = 0; i < k + 24; i++)
				if (writefile(nip, (off_t)i * 1024,
				    big + (i % 97), 1024) == 1024)
					got++;
			printf("w-bulk-blocks %d\n", got == k + 24 ? 1 : 0);
			printf("w-bulk-size %d\n",
			    (long)nip->i_size == (long)(k + 24) * 1024
			    ? 1 : 0);

			/*
			 * The drain: s_nfree cannot rise unless alloc()
			 * refilled it from a chained block, and the only
			 * code that does that is the arm above.
			 */
			printf("w-drain-refilled %d\n",
			    (int)fp->s_nfree > 0 &&
			    (long)fp->s_tfree < tfreeA ? 1 : 0);

			/*
			 * AND THE ACCOUNTING IS EXACT.  k+24 data blocks,
			 * plus the indirect block that blocks 10.. need.
			 * Written as a range rather than a number because
			 * how many indirect blocks a file of this size uses
			 * is a fact about NINDIR, not about alloc(); the
			 * lower bound is what says nothing leaked.
			 */
			printf("w-bulk-tfree-delta %ld\n",
			    tfreeA - (long)fp->s_tfree);
			printf("w-bulk-tfree-atleast %d\n",
			    tfreeA - (long)fp->s_tfree >= (long)(k + 24)
			    ? 1 : 0);

			/*
			 * Read one block back from the far end, through the
			 * indirect block that the bulk write allocated.
			 */
			got = readat(nip, (off_t)(k + 20) * 1024, small, 8);
			printf("w-bulk-readback %d\n",
			    (got == 8 &&
			     strncmp(small, big + ((k + 20) % 97), 8) == 0)
			    ? 1 : 0);
			iput(nip);
		}

		/* -----------------------------------------------------
		 * 8g. The name is really in the directory -- a SECOND,
		 * ordinary namei, with no flag, finding what the create
		 * path put there.  Without this, 8e proves only that
		 * ialloc returned an inode; the entry writei(dp) wrote
		 * into the parent is what makes it a file with a name.
		 */
		ip = lookup("/created");
		printf("w-lookup-found %d\n", ip != NULL ? 1 : 0);
		if (ip != NULL) {
			printf("w-lookup-size-matches %d\n",
			    (long)ip->i_size > 0 ? 1 : 0);
			iput(ip);
		}

		/* -----------------------------------------------------
		 * 8h. NI_DEL -- unlink, and the round trip closes.
		 *
		 * nami.c:298-328 clears d_ino in the entry, writei()s the
		 * directory (which is writei's ONE synchronous arm,
		 * rdwri.c:207-217, guarded on the entry being cleared) and
		 * iput()s the target with i_nlink now 0 -- reaching itrunc,
		 * which walks NADDR-1 down to 0 handing every block back to
		 * free(), and then ifree for the inode itself.
		 *
		 * A SUCCESSFUL DELETE RETURNS NULL, which is not a failure:
		 * fsnami's NI_DEL arm ends `goto out', and namei's case 1
		 * iputs the parent and returns NULL.  u_error is the only
		 * thing that separates the two, which is the same shape as
		 * the enoent/notdir pair in section 5.
		 */
		arg.flag = NI_DEL;
		arg.ino = 0;
		arg.idev = 0;
		arg.mode = 0;
		strncpy(pathbuf, "/created", sizeof(pathbuf) - 1);
		pathbuf[sizeof(pathbuf) - 1] = '\0';
		u.u_dirp = pathbuf;
		u.u_error = 0;
		nip = namei(schar, &arg, 1);
		printf("w-del-null %d\n", nip == NULL ? 1 : 0);
		printf("w-del-err %d\n", u.u_error);
		if (nip != NULL)
			iput(nip);

		ip = lookup("/created");
		printf("w-del-gone %d\n", ip == NULL ? 1 : 0);
		printf("w-del-gone-err %d\n", u.u_error);
		if (ip != NULL)
			iput(ip);

		/*
		 * THE ROUND TRIP, and it is the strongest single number
		 * here.  Everything 8e/8f took -- one inode and several
		 * hundred blocks -- has come back, so s_tinode is exactly
		 * what it was before the create and s_tfree is exactly what
		 * it was after 8c.  An off-by-one anywhere in alloc, free,
		 * ialloc, ifree or itrunc moves one of these.
		 */
		printf("w-roundtrip-tinode %d\n",
		    (long)fp->s_tinode == tinode0 ? 1 : 0);
		printf("w-roundtrip-tfree %d\n",
		    (long)fp->s_tfree == tfreeA ? 1 : 0);

		/* -----------------------------------------------------
		 * 8h-bis. THE s_ronly ARM, MADE TO FIRE.
		 *
		 * §8a step 5d restored upstream's read-only check to
		 * v8fs.c's access() -- and the lp64-auditor pointed out that
		 * restoring it is not the same as exercising it: iinit sets
		 * fp->s_ronly = 0 and nothing ever sets it otherwise, so the
		 * arm was READ on every create and could never be TAKEN.
		 * "A guard that has never been seen to fail is not a guard."
		 *
		 * Setting the field by hand is legitimate rather than a
		 * cheat: s_ronly is a superblock field and mount(2) is what
		 * would normally set it -- the same shape as v8k_bdconf
		 * standing in for config(8).  It is put BACK immediately,
		 * because everything after it writes.
		 *
		 * THAT USED TO SAY `smount is not imported', which was
		 * asking about a function that does not exist.  dev/conf.c's
		 * row 0 names smount and the whole 18k-line kernel mentions
		 * it in exactly two lines, both in that dead file; the live
		 * mount syscall is fsmount() at sys/sys3.c:273.  §8a step 5f
		 * transcribed its two s_ronly lines into iinit, so a server
		 * now mounts read-only for real -- and this probe still sets
		 * the field by hand, because it wants the flag to change
		 * UNDER a filesystem it has already written to.
		 *
		 * EROFS rather than EACCES is the whole point: uid is 0
		 * here, so every other arm of access() short-circuits at
		 * `u.u_uid == 0' and this is the only one that can refuse a
		 * root create at all.
		 */
		fp->s_ronly = 1;
		arg.flag = NI_CREAT;
		arg.ino = 0;
		arg.idev = 0;
		arg.mode = 0644;
		strncpy(pathbuf, "/onro", sizeof(pathbuf) - 1);
		pathbuf[sizeof(pathbuf) - 1] = '\0';
		u.u_dirp = pathbuf;
		u.u_error = 0;
		nip = namei(schar, &arg, 1);
		printf("w-ronly-refused %d\n", nip == NULL ? 1 : 0);
		printf("w-ronly-err %d\n", u.u_error);
		if (nip != NULL)
			iput(nip);
		fp->s_ronly = 0;

		/* And it really was the flag: the same create now works. */
		arg.flag = NI_CREAT;
		arg.ino = 0;
		arg.idev = 0;
		arg.mode = 0644;
		strncpy(pathbuf, "/onro", sizeof(pathbuf) - 1);
		pathbuf[sizeof(pathbuf) - 1] = '\0';
		u.u_dirp = pathbuf;
		u.u_error = 0;
		nip = namei(schar, &arg, 1);
		printf("w-ronly-cleared %d\n", nip != NULL ? 1 : 0);
		if (nip != NULL)
			iput(nip);

		/*
		 * AND IT HAS TO GO BACK THROUGH NI_DEL, not by hand.  The
		 * first draft did `nip->i_nlink = 0; iput(nip)', which frees
		 * the inode and leaves the DIRECTORY ENTRY in / pointing at
		 * it -- and fsck said so, twice: "FILE SYSTEM WAS MODIFIED"
		 * and a cmp difference at byte 180289.  Which is the
		 * acceptance test doing its job on the probe rather than on
		 * the kernel, and a small demonstration of why it is there:
		 * three probe cases went green on an image the checkers
		 * rejected.
		 */
		arg.flag = NI_DEL;
		arg.ino = 0;
		arg.idev = 0;
		arg.mode = 0;
		strncpy(pathbuf, "/onro", sizeof(pathbuf) - 1);
		pathbuf[sizeof(pathbuf) - 1] = '\0';
		u.u_dirp = pathbuf;
		u.u_error = 0;
		nip = namei(schar, &arg, 1);
		printf("w-ronly-cleaned %d\n", u.u_error == 0 ? 1 : 0);
		if (nip != NULL)
			iput(nip);

		/* -----------------------------------------------------
		 * 8i. Flush.  update() is sync(2)'s internal name and it does
		 * the whole job: modified superblocks by bwrite, modified
		 * inodes by iupdat, and then -- its own last statement, at
		 * alloc.c:530 -- bflush(NODEV), which pushes every remaining
		 * B_DELWRI buffer at the driver.  Until it runs, most of the
		 * work above is in a 32-buffer cache and the image on disk is
		 * a lie.
		 *
		 * THERE WAS A bflush(NODEV) OF OUR OWN HERE AND IT WAS DEAD.
		 * The comment described update() and bflush as a sequence, in
		 * this file and in run.sh, and update() had already called it
		 * -- so deleting the line changed nothing, which is the
		 * vacuous shape CLAUDE.md names.  Found by the lp64-auditor,
		 * confirmed by deleting it and re-running.  What
		 * `w-flush-wrote' measures is update()'s own flush, which is
		 * the property worth having and was never the one described.
		 *
		 * The suite's real acceptance test comes after the probe
		 * exits: icheck, dcheck and fsck -- three programs of Bell
		 * Labs' that know nothing about this one -- are given the
		 * image and asked whether it is a filesystem.
		 */
		w0 = v8k_imgwrites();
		update();
		printf("w-flush-wrote %d\n", v8k_imgwrites() > w0 ? 1 : 0);
		printf("w-writes-total-positive %d\n", v8k_imgwrites() > 0 ? 1 : 0);
	}

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
