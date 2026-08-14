/*
 * main.c -- the kernel tables the filesystem code indexes, and the startup
 * that makes them valid.  §8a step 5c.
 *
 * THIS FILE IS OURS, not Bell Labs', and it stands in for THREE upstream files
 * at once.  That merge is the first thing to understand about it, because each
 * of the three is a different kind of thing upstream and two of them describe a
 * machine that does not exist here:
 *
 *   sys/param.c	the size FORMULAE and the pointers.  Its own comment
 *			says why the pointers live there: "These have to be
 *			allocated somewhere; allocating them here forces loader
 *			errors if this file is omitted" (:39-41).  It is
 *			compiled per machine, "-DHZ=xx -DTIMEZONE=x -DDST=x
 *			-DMAXUSERS=xx" (:20), and MAXUSERS IS NOT IN THE
 *			SHIPPED TREE -- measured, `grep -rn MAXUSERS conf/'
 *			finds nothing.  So every formula there is unevaluable
 *			here, which is the same situation NTTY was in when
 *			ttyld.c landed: a per-configuration number config(8)
 *			derived from a machine description Bell Labs did not
 *			ship.  It is a layer-2 decision, and the rule from
 *			NTTY applies -- DERIVE IT, DO NOT PICK IT.
 *
 *   sys/machdep.c	the STORAGE.  nbuf comes from physmem at :81-84 and
 *			every table is carved out of the VAX's kernel virtual
 *			address space by valloc at :102-121.  There is no
 *			physmem here and no arena to carve, so the tables are
 *			plain bss and the sizes are constants.
 *
 *   sys/main.c		the STARTUP -- binit at :194 and iinit at :160, the
 *			two functions transcribed below.  This port has no
 *			main(): the shim is the startup, so this is where they
 *			have to go, and the file is named for the one of the
 *			three whose CODE is here rather than only its data.
 *
 * WHAT IT DELIBERATELY DOES NOT DO.  Nothing here is called automatically.
 * v8k_kinit() has to be invoked by whatever is standing up a filesystem, which
 * today is tests/streams/fsprobe.c and nothing else.  A constructor would make
 * every program that links libv8kern pay for a buffer cache it never uses, and
 * CLAUDE.md already records why libv8kern is a separate archive from libv8sys.
 */

#include "../h/param.h"
#include "../h/proc.h"
#include "../../../src/sys/h/dir.h"	/* struct direct, for user.h's u_dent */
#include "../h/user.h"
#include "../h/conf.h"
#include "../../../src/sys/h/inode.h"
#include "../../../src/sys/h/mount.h"
#include "../h/filsys.h"		/* forwards to src/include/sys/filsys.h */

/*
 * vlimit.h is AUTHENTIC and is included for LIM_FSIZE and INFINITY, which
 * v8k_uinit() below needs.  user.h:171 already points at it in a comment --
 * `int u_limit[8]; /_ :116 -- see src/sys/h/vlimit.h _/' -- so the header was
 * already the named authority for that array's indices and nothing had ever
 * included it.  It is #defines only: eight LIM_ indices, NLIMITS and
 * INFINITY, no struct and no declaration, so there is nothing in it that can
 * collide with the host.
 */
#include "../../../src/sys/h/vlimit.h"

/*
 * THE AUTHENTIC buf.h, BY ITS FULL PATH, AND THE PATH IS AN ASSERTION.
 *
 * `#include "../h/buf.h"' from this directory would find shim/kern/h/buf.h --
 * ours, two constants, no struct buf -- because KERNFLAGS' -Ishim/kern/dev
 * turns "../h/" into shim/kern/h/ for any file that does not have a nearer
 * candidate.  This file needs the real thing: struct buf, its b_un union,
 * BQUEUES and the B_ flags binit weaves the free lists with.
 *
 * shim/kern/h/buf.h WAS DELETED in §8a step 5c and its story is worth keeping,
 * because it is this tree's vestigial-file class arriving from the other
 * direction.  It existed to give streamio.c B_READ and B_WRITE without
 * importing 107 lines about a VAX buffer cache, and its header comment said so.
 * Then bio.c's import brought src/sys/h/buf.h into the tree -- and a quoted
 * include tries the includer's directory FIRST, so streamio.c's own
 * `#include "../h/buf.h"' silently started resolving to the authentic header
 * instead.  Measured with `clang -M', not reasoned.  Its two remaining
 * includers used neither constant, measured too.  So the file went from
 * load-bearing to dead WITHOUT ANYTHING BEING EDITED IN IT, purely because a
 * third file arrived; and a same-named shadow of an authentic header sitting
 * in the include path is a trap rather than a saving.
 */
#include "../../../src/sys/h/buf.h"

/*
 * systm.h is where `dev_t rootdev' lives (:48), as a K&R tentative definition
 * like everything else in these headers.  It is included LAST of the authentic
 * set for no reason but that iinit is the only thing here that needs it.
 */
#include "../../../src/sys/h/systm.h"

/*
 * FOUR DECLARATIONS UPSTREAM DOES NOT WRITE DOWN, and writing them is forced by
 * this file being modern C rather than by anything about the functions.  V8
 * leans on implicit int: bio.c defines brelse, bhinit and binit with no return
 * type and no prototype anywhere, and iget.c does the same for ihinit, so a
 * caller compiled in 1985 K&R needs nothing.  SHIMFLAGS is -std=gnu99, where an
 * undeclared call is an error -- and that is the good direction, since it is
 * exactly the diagnostic §8a step 5 had to turn OFF for the imported half and
 * which then hid fourteen macros compiled as calls to undefined functions.
 * Keeping it ON for our own code is the whole reason this file is not K&R.
 */
void		brelse(struct buf *bp);		/* bio.c:233 */
void		bhinit(void);			/* bio.c:48 */
void		ihinit(void);			/* iget.c:26 */
extern void	v8fs_settime(time_t now);	/* v8fs.c */

/*
 * ---------------------------------------------------------------------------
 * THE TABLES
 * ---------------------------------------------------------------------------
 *
 * Each of these is declared as a K&R TENTATIVE definition inside an authentic
 * header's #ifdef KERNEL block, so every imported object already emits a common
 * symbol for it and the linker merges them.  The definitions here are STRONG
 * and win, which is the ordinary and intended resolution -- not the silent
 * common-against-text collision §8a step 5 spent its time on, because there the
 * two definitions meant different things and here they mean the same one.
 * shim/kern/sys/fio.c:97-105 does exactly this for the file table and says so.
 *
 * AND A SENTENCE IN v8fs.c WAS MISLEADING ABOUT IT.  That file says the common
 * mechanism "supplies inode, inodeNINODE, ninode and rootdir ... none of the
 * four needs a definition here, and writing one would be four collisions found
 * the hard way."  True about LINKING and false about running: a common
 * `struct inode *inode' is a NULL POINTER and a common `int ninode' is ZERO, so
 * the inode table linked cleanly and had no storage.  ihinit() over ninode == 0
 * does not fail -- it writes i_hlink through a null pointer's element 0 -- so
 * the fault would have been a SIGSEGV in the first iget, blamed on iget.
 */

/*
 * NINODE -- DERIVED FROM BELL LABS' OWN TWO FORMULAE AND THIS PORT'S NFILE.
 *
 * sys/param.c:29-30 sizes the two tables together, off one quantity:
 *
 *	ninode = 3 * (NPROC + 16 + MAXUSERS) + 32
 *	nfile  = 2 * (NPROC + 16 + MAXUSERS) + 32
 *
 * Neither is computable here -- MAXUSERS was never shipped -- but the RATIO
 * between them is, and it survives the missing term: eliminating the common
 * subexpression gives ninode - 32 = 3/2 * (nfile - 32).  Half again as many
 * inodes as open files, over a floor of 32 each.  That is a statement about how
 * many files a process holds open versus how many directories it walks through
 * to reach them, and it is as true here as on a VAX.
 *
 * shim/kern/sys/fio.c:91 has already fixed NFILE at 64 for this port, with its
 * own argument.  So NINODE = 3*(64-32)/2 + 32 = 80, and the number moves if and
 * only if NFILE moves -- which is what makes it a derivation rather than a
 * guess.  Do NOT substitute this port's NPROC into the formula directly:
 * shim/kern/h/proc.h's NPROC is 4 because slot 0 is pfind's chain terminator
 * and slot 2 is the address bio.c forms for the pagedaemon, which has nothing
 * to do with how many processes run.  Mixing the two constants would be
 * numerology.
 *
 * NOTHING IN THE TREE CONSTRAINS THIS NUMBER, AND THAT IS SAID OUT LOUD
 * BECAUSE IT WAS MEASURED RATHER THAN ASSUMED.  Mutating NINODE from 80 to 3
 * and then to 2 left tests/streams at 308 passed, 0 failed both times; only
 * NINODE 1 failed.  The read path needs exactly TWO slots -- rootdir and
 * u_cdir are two igets of the same (dev, ROOTINO), so iget hands back the same
 * structure with i_count 2 rather than taking a second slot, and fsnami
 * releases each parent as it descends.
 *
 * So the derivation above is not a rationalisation of a number a test pinned;
 * it is the ONLY justification there is, which is what a configuration
 * constant's justification should look like.  What the mutation did buy is a
 * case worth having: tests/streams now counts the inodes still held when the
 * probe finishes, which catches a missing iput -- a real bug class that no
 * amount of table size would have exposed.
 */
#define	NINODE		(3 * (64 - 32) / 2 + 32)	/* 80; see above */

static struct inode	v8k_inodetab[NINODE];

struct inode	*inode = v8k_inodetab;
struct inode	*inodeNINODE = &v8k_inodetab[NINODE];
int		ninode = NINODE;

/*
 * NBUF -- UPSTREAM'S OWN FLOOR, because upstream's formula needs a number this
 * machine cannot supply in V8's units.
 *
 *	sys/machdep.c:81-84
 *	if (nbuf == 0) {
 *		nbuf = (32 * physmem) / btoc(1024*1024);
 *		if (nbuf < 32)
 *			nbuf = 32;
 *	}
 *
 * physmem is in VAX pages of 512 bytes.  Handing it this host's memory would
 * produce a buffer cache sized for a machine that is neither a VAX nor
 * this one, in the same way substituting the host's page size into NBPG would
 * -- shim/kern/h/param.h refuses that one for the identical reason.  So the
 * answer is the arm Bell Labs wrote for a small machine: thirty-two.
 *
 * It is comfortably above what the read path holds at once, which is the
 * property that actually matters, since running out of buffers is a DEADLOCK
 * rather than an error -- getblk sleeps on bfreelist[0] and only brelse wakes
 * it.  Held simultaneously on a namei/iget/readi walk: one directory block,
 * one inode block, one data block, plus the superblock, which iinit marks
 * B_LOCKED so it never returns to the reusable queues at all.  Four.
 *
 * The cost is NBUF * BUFSIZE = 128 KB of bss plus the headers, and it is paid
 * only by things that link libv8kern -- which is a separate archive from
 * libv8sys for exactly this reason, and which no installed V8 binary carries.
 */
#define	NBUF		32		/* machdep.c:83-84, the floor */

static struct buf	v8k_buftab[NBUF];
static char		v8k_bufmem[NBUF * BUFSIZE];

struct buf	*buf = v8k_buftab;
char		*buffers = v8k_bufmem;
int		nbuf = NBUF;

/*
 * ---------------------------------------------------------------------------
 * THE STARTUP
 * ---------------------------------------------------------------------------
 */

/*
 * binit -- sys/main.c:194-220, transcribed, with the two tails that describe
 * absent subsystems left off and named here rather than silently dropped.
 *
 *   THE bdevsw COUNTING LOOP (:218-219) IS OMITTED, and omitting it is
 *   required rather than tidy.  Upstream counts the config(8)-generated table
 *   to derive nblkdev; here shim/kern/sys/ioconf.c's v8k_bdconf() maintains
 *   nblkdev as drivers register, so running the loop as well would count every
 *   registered driver TWICE and put nblkdev past the populated prefix -- which
 *   is precisely the hole ioconf.c argues at length that nothing may create.
 *
 *   THE SWAP TAIL (:220-231: nswdev, swdevt, nswap, maxpgio, swfree) IS
 *   OMITTED because there is no swapper.  Note that upstream PANICS there if
 *   no swap device is configured -- `if (nswdev == 0) panic("binit")' -- so
 *   transcribing it faithfully would abort every run.  A machine with no swap
 *   is not a machine V8 would boot, and that is the honest reading: this is
 *   V8's buffer cache without V8's pager, which is what §8a step 5 imported.
 */
void
binit(void)
{
	register struct buf *bp;
	register struct buf *dp;
	register int i;

	for (dp = bfreelist; dp < &bfreelist[BQUEUES]; dp++) {
		dp->b_forw = dp->b_back = dp->av_forw = dp->av_back = dp;
		dp->b_flags = B_HEAD;
	}
	dp--;				/* dp = &bfreelist[BQUEUES-1]; */
	for (i = 0; i < nbuf; i++) {
		bp = &buf[i];
		bp->b_dev = NODEV;
		bp->b_un.b_addr = buffers + i * BUFSIZE;
		bp->b_back = dp;
		bp->b_forw = dp->b_forw;
		dp->b_forw->b_back = bp;
		dp->b_forw = bp;
		bp->b_flags = B_BUSY|B_INVAL;
		brelse(bp);
	}
}

/*
 * iinit -- sys/main.c:160-187, transcribed.  It reads the root superblock,
 * pins it, and hangs it off a mount entry; after this getfs(rootdev) answers
 * instead of panicking, which is the single gate between "the code links" and
 * "the code runs".
 *
 * ONE DEVIATION, AT THE LAST TWO LINES.  Upstream ends
 *
 *	clkinit(fp->s_time);
 *	bootime = time;
 *
 * -- set the wall clock from the filesystem's last-update stamp, then record
 * the boot moment.  clkinit is in sys/clock.c, which is not imported and which
 * is about a VAX interval timer.  v8fs_settime() in v8fs.c is this port's
 * single place for both assignments and does them in that order, so the call
 * below is the same two statements behind one name.  It is NOT a widening
 * hazard even though s_time is v8_i32 and time_t is 8 bytes here: the value is
 * PASSED, not addressed, so it widens by the ordinary conversion.  The
 * dangerous spelling is `&fp->s_time', which CLAUDE.md's narrowed-field sweep
 * exists to catch and which does not appear here.
 *
 * AND ONE ADDITION, §8a step 5f: THE MOUNT IS READ-WRITE OR READ-ONLY, AND THE
 * FLAG IS BELL LABS' OWN.  Upstream's iinit hardcodes s_ronly = 0 because it
 * describes the ROOT filesystem, which a VAX always mounted read-write.  The
 * general form is fsmount() at sys/sys3.c:299-316, the mount(2) syscall:
 *
 *	(*bdevsw[major(dev)].d_open)(dev, !uap->ronly);
 *	...
 *	fp->s_ronly = uap->ronly & 1;
 *
 * -- two lines, and they are transcribed rather than invented.  What they buy
 * is that "read only" becomes a property of the FILESYSTEM instead of a
 * refusal in the protocol: iupdat returns early on s_ronly (iget.c:248), so
 * not even an atime reaches the disk, and access() refuses IWRITE through the
 * arm §8a step 5d restored (shim/kern/sys/v8fs.c).  A server that answered
 * EROFS to Twrite while leaving s_ronly at 0 had neither guarantee.
 */
void
iinit(int ronly)
{
	register struct buf *bp;
	register struct filsys *fp;
	register int i;
	register struct mount *mp;

	(*bdevsw[major(rootdev)].d_open)(rootdev, !ronly);	/* sys3.c:299 */
	bp = bread(rootdev, SUPERB);
	if (u.u_error)
		panic("iinit");
	bp->b_flags |= B_LOCKED;		/* block can never be re-used */
	brelse(bp);
	mp = allocmount(0, rootdev);
	if (mp == NULL)
		panic("iinit");
	mp->m_bufp = bp;
	fp = bp->b_un.b_filsys;
	fp->s_flock = 0;
	fp->s_ilock = 0;
	fp->s_ronly = ronly & 1;		/* sys3.c:316 */
	fp->s_lasti = 1;
	fp->s_nbehind = 0;
	fp->s_fsmnt[0] = '/';
	for (i = 1; i < sizeof(fp->s_fsmnt); i++)
		fp->s_fsmnt[i] = 0;
	v8fs_settime((time_t)fp->s_time);	/* was clkinit; see above */
}

/*
 * v8k_uinit -- the u-area fields main() sets before it calls ihinit, and the
 * §8a step 5d addition to this file.  sys/main.c:52-79, the block between
 * `p = &proc[0]' and `clkstart()'.
 *
 * IT IS FORCED, NOT COMPLETENESS.  writei() at rdwri.c:164-169 is
 *
 *	if ((ip->i_mode&IFMT)==IFREG &&
 *	    u.u_offset + u.u_count > u.u_limit[LIM_FSIZE]) {
 *		psignal(u.u_procp, SIGXFSZ);
 *		u.u_error = EMFILE;
 *
 * -- and a u-area that has never been initialised has u_limit[] all zero out
 * of bss, so EVERY write to a regular file takes that arm.  Not "large writes
 * fail": all of them, including a one-byte write at offset 0, because
 * `0 + 1 > 0'.  The read half never touched u_limit, which is why this was
 * invisible through §8a step 5c.
 *
 * AND THE FAILURE WOULD HAVE READ AS A PORT BUG IN THE WRONG FILE.  EMFILE is
 * "too many open files"; upstream's own choice, not ours, and nothing to do
 * with a size limit.  A first write returning EMFILE points an investigation
 * at the file table, which this port does size, rather than at a limit nobody
 * had set.
 *
 * THREE ARMS OF UPSTREAM'S LOOP ARE OMITTED and they are the three that name
 * VAX memory: LIM_STACK is 512*1024, LIM_DATA is ctob(MAXDSIZ) and LIM_TEXT
 * is ctob(MAXTSIZ).  MAXDSIZ and MAXTSIZ are vmparam.h numbers about a VAX
 * address space; measured, nothing in libv8kern reads u_limit at any index
 * but LIM_FSIZE.  The bare sweep is NOT the thing to quote, because writing
 * this paragraph put nine more matches in the tree -- the same class as the
 * `time(&' population that grew every time someone recorded a find.  What
 * stays comparable is:
 *
 *	grep -rn 'u\.u_limit\[' src/sys shim/kern | grep -v '\.md:' |
 *	    grep -v ':[0-9]*:[[:blank:]]*\*'
 *
 * THREE hits today and all three are code: rdwri.c:165, the only READ and the
 * one that indexes LIM_FSIZE; and the two lines of the loop below, one of
 * which reads only the array's sizeof.  (The first draft of this filter said
 * "two" and returned five, because it tried to strip a comment line with
 * `^[^:]*: *\*' and a grep -n prefix has TWO colons in it.  Which is the
 * instrument rule arriving inside a sentence about the instrument rule.)
 * Agrees under /usr/bin/grep, which is the one CI has.  Naming them here rather than inventing values for them is the
 * same policy the binit() transcription below already follows for the swap
 * tail, and the same one conf.h follows for nfstyp: an arm with no consumer
 * is a claim nothing can check.
 *
 * u_cmask IS TRANSCRIBED THOUGH ITS VALUE IS THE BSS DEFAULT.  CMASK is 0
 * (h/param.h:71) so the assignment changes nothing today -- and it is here
 * because nami.c:502's `flagp->mode & ~u.u_cmask' is about to read it, and a
 * zero that is stated is a different thing from a zero that is left over.
 * Same reasoning as spelling the on-disk field widths out loud.
 *
 * u_uid AND u_gid ARE DELIBERATELY LEFT AT ZERO, which is root, and that is
 * upstream's answer rather than a convenience: main.c sets neither, because
 * process 0 IS root.  shim/kern/sys/fio.c's v8k_procinit() folds the HOST's
 * uid into them, and it exists for the stream side, where the u-area
 * describes a real host process to ps.  A kernel standing up a filesystem is
 * not that; calling it here would make v8fs.c's access() take its
 * uid-comparison arms against inodes mkfs wrote as uid 0, so whether a write
 * was permitted would depend on who ran the test.  That is the
 * host-property class the test suites are swept for, arriving through a
 * u-area field.
 *
 * STATIC, because v8k_kinit below is the only entry point that stands a
 * filesystem up and there is no second caller to have.  An exported name with
 * one in-file caller would be a component the collision sweep in tests/kmemu
 * has to carry for nothing.
 */
static void
v8k_uinit(void)
{
	int i;

	u.u_procp = &v8k_proc0;			/* main.c:60 */
	u.u_cmask = CMASK;			/* main.c:61 */
	for (i = 1; i < (int)(sizeof(u.u_limit)/sizeof(u.u_limit[0])); i++)
		u.u_limit[i] = INFINITY;	/* main.c:62-77, see above */
}

/*
 * v8k_kinit -- this port's main(), reduced to the part that stands up a
 * filesystem.  Ours, and named apart from V8's vocabulary because it is not
 * one of their functions.
 *
 * THE ORDER IS UPSTREAM'S AND IT IS LOAD-BEARING, sys/main.c:86-95:
 *
 *	ihinit(); bhinit(); qinit(); binit(); bswinit(); iinit();
 *	rootdir = iget(rootdev, ROOTINO, 0);   rootdir->i_flag &= ~ILOCK;
 *	u.u_cdir = iget(rootdev, ROOTINO, 0);  u.u_cdir->i_flag &= ~ILOCK;
 *	u.u_rdir = NULL;
 *
 * bhinit before binit, because binit's brelse() puts each buffer on a free
 * list and getblk later finds it through the hash that bhinit wove.  iinit
 * after both, because it does a bread.  And the two igets after iinit, because
 * iget reads the ilist through getfs, which needs the mount iinit made.
 *
 * TWO OMISSIONS AND ONE ADDITION.
 *
 *   qinit() is the stream engine's, already called by the stream probes; it is
 *   left out here because a filesystem does not need it and calling it twice
 *   would rebuild the free block lists under any stream already open.
 *   bswinit() is swap.
 *
 *   ROOTDEV IS AN ARGUMENT.  Upstream's is a compile-time constant config(8)
 *   emits into conf.c; here the caller has just registered a driver and knows
 *   the major number it was given, so the number cannot be known before the
 *   call.  Same shape as v8k_stconf returning the discipline number.
 *
 *   AND SO IS RONLY, §8a step 5f, for a reason one step removed from the
 *   first.  Upstream cannot pass it here because upstream's root is always
 *   read-write and the general case is mount(2), which this port does not
 *   have -- there is no second filesystem to mount onto the first.  So the one
 *   mount this kernel ever makes is the one kinit makes, and the argument
 *   fsmount() reads out of the syscall's u_ap has to arrive as a parameter
 *   instead.  It is handed straight to iinit, which is where both of Bell
 *   Labs' uses of it are.
 *
 * TWO INODES ARE TAKEN FOR ONE DIRECTORY AND THAT IS NOT REDUNDANT.  rootdir
 * and u_cdir are separate iget()s of the same (dev, ROOTINO), so i_count is 2
 * and releasing the current directory later cannot free the root.  Both are
 * unlocked immediately, which is what lets namei lock them again as it walks.
 */
int
v8k_kinit(dev_t dev, int ronly)
{
	rootdev = dev;

	v8k_uinit();		/* main.c:52-79, BEFORE ihinit -- see above */

	ihinit();
	bhinit();
	binit();
	iinit(ronly);

	rootdir = iget(rootdev, (ino_t)ROOTINO, 0);
	if (rootdir == NULL)
		return (-1);
	rootdir->i_flag &= ~ILOCK;

	u.u_cdir = iget(rootdev, (ino_t)ROOTINO, 0);
	if (u.u_cdir == NULL)
		return (-1);
	u.u_cdir->i_flag &= ~ILOCK;

	u.u_rdir = NULL;
	return (0);
}
