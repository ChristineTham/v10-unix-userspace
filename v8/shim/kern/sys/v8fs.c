/*
 * v8fs.c -- the twenty kernel services §8a step 5's imported filesystem code
 * calls, and the tables it indexes.
 *
 * THIS FILE IS OURS, not Bell Labs', and it is the machine-dependent half of
 * src/sys/sys/{alloc,iget,nami,rdwri,subr}.c and src/sys/dev/bio.c in exactly
 * the relationship shim/kern/sys/slp.c has to streamio.c.  Modern C, because
 * it is ours; the K&R dialect flag is for the imported half.
 *
 * The twenty fall into three kinds, and which kind a name gets is a claim
 * about this port rather than a convenience:
 *
 *   REAL	the function does what V8's does, because something here
 *		depends on the answer.  Thirteen.
 *   ANSWER	the function returns the value V8's would return ON THIS
 *		MACHINE, which is a constant because the subsystem it
 *		queries does not exist.  Two, and both are argued below
 *		rather than assumed.
 *   PANIC	the function cannot be given a truthful answer, so it stops
 *		the program with V8's own panic() and names itself.  Five,
 *		all of them VAX virtual memory.
 *
 * A PANIC IS A BETTER STUB THAN A ZERO, and this port has the receipt.
 * CLAUDE.md records nulldev() returning register litter being counted as 42
 * signal deaths, and records a qopen returning -1 being read as an inode
 * pointer of 0xffffffff.  A stub that returns a plausible value converts a
 * missing subsystem into a wrong answer somewhere else; a panic converts it
 * into a message naming the function.  The five below are reachable only from
 * bio.c's swap paths, which this port has no swapper under.
 *
 * WHAT EACH DEFINITION ANSWERS TO.  Nothing here respells a declaration that
 * an authentic header already carries -- src/sys/h/systm.h declares min, max
 * and bmap; mount.h:22 declares findmount; cmap.h:36 declares mfind; inode.h
 * declares iget and ialloc.  Where upstream declares it, that declaration is
 * the prototype and this file must match it, which is checked by including
 * the same headers the imported files do.
 */

#include "../../v8sys/rawsys.h"
#include "../h/param.h"
#include "../h/proc.h"
#include "../../../src/sys/h/dir.h"
#include "../h/user.h"
#include "../h/conf.h"
#include "../../../src/sys/h/inode.h"
#include "../../../src/sys/h/mount.h"	/* struct mount, M_MOUNTED, findmount */
#include "../../../src/sys/h/cmap.h"	/* mfind's authentic declaration */
#include "../../../src/sys/h/acct.h"	/* ASU, for suser's u_acflag */
/*
 * filsys.h arrived with §8a step 5d, for the s_ronly arm access() below has
 * got back.  It is here for its DECLARATION and not only for the struct:
 * src/include/sys/filsys.h:57 is `struct filsys *getfs();' inside #ifdef
 * KERNEL, and without it the call is an implicit int under -std=gnu99 --
 * which clang refuses outright rather than truncating, and did.  That refusal
 * is the same guard tests/streams/fsprobe.c arranges with an #error, working
 * for free because the shim half is compiled as C99 while the imported half
 * is not.
 */
#include "../h/filsys.h"
/*
 * `#include "../h/buf.h"' stood here and was removed in §8a step 5c, for the
 * reason shim/kern/sys/subr.c gives at the same place: this file used neither
 * of that header's two constants, and the header is gone.  Nothing here needs
 * `struct buf' either -- mount.h's m_bufp is a pointer to an incomplete type,
 * which is all findmount and allocmount below ever handle.
 */

void	kvprintf(const char *fmt, __builtin_va_list ap);	/* machdep.c:175 */

/*
 * ---------------------------------------------------------------------------
 * THE TABLES
 * ---------------------------------------------------------------------------
 *
 * fstypsw's one row and why nfstyp is 1 rather than upstream's 4 is argued at
 * length in shim/kern/h/conf.h, including how the vestigial dev/conf.c names a
 * function (rnami) that V8 renamed to fsnami and never updated.  fsnami is
 * defined by the imported src/sys/sys/nami.c:202.
 */
int	fsnami();			/* src/sys/sys/nami.c:202 */

struct fstypsw fstypsw[] = {
	/* put get free updat read write trunc stat */
	{  0,  0,  0,   0,    0,   0,    0,    0,
	   fsnami,			/* the only slot dispatched unguarded */
	   0,				/* t_mount: nothing mounts yet */
	   0 },
};
int	nfstyp = 1;

/*
 * THE TWO DEVICE SWITCHES USED TO BE HERE and moved to shim/kern/sys/ioconf.c
 * in §8a step 5c.  That file is named for conf/ioconf.c, which is where
 * config(8) emits exactly these tables, so they were in the wrong one of our
 * two files all along -- this one holds the kernel SERVICES.  The move also
 * put bdevsw under ioconf.c's dense-prefix invariant, which it needs for the
 * same reason streamtab does and which is argued there.
 *
 * What did NOT change is that cdevsw stays empty and nchrdev stays 0.  What
 * changed is that nblkdev is no longer permanently 0: a probe registers a
 * block driver with v8k_bdconf() and bio.c:352's range check then admits it,
 * which is what makes it possible to run this code at all.
 */

/*
 * The proc table.  shim/kern/h/proc.h argues the size: slot 0 is pfind's chain
 * terminator, slot 2 is the address bio.c forms for the pagedaemon.
 */
static struct proc v8k_proctab[NPROC];
struct proc	*proc = v8k_proctab;
struct proc	*procNPROC = &v8k_proctab[NPROC];
short		pidhash[PIDHSZ];

/*
 * mem_no -- THE MAJOR NUMBER OF /dev/mem, AND IT IS -1 ON PURPOSE.
 *
 * It has no header declaration anywhere in V8; rdwri.c declares it locally,
 * `extern int mem_no;' at :37 and :136, which is why this is the file that
 * must define it.  Upstream's value is 3, in the vestigial dev/conf.c:613 --
 * see conf.h on why that citation is to dead code, though nothing turns on the
 * number being 3 rather than something else.
 *
 * What it is FOR is one guard, appearing identically in readi and writei:
 *
 *	if(u.u_offset < 0 && ((ip->i_mode&IFMT) != IFCHR || mem_no != major(dev)))
 *		{ u.u_error = EINVAL; return; }
 *
 * -- a negative file offset is an error UNLESS this is the memory special
 * file, where a negative offset is a legitimate kernel address.  Setting
 * mem_no to -1 makes `mem_no != major(dev)' true for every real device, so the
 * exemption is never granted and a negative offset is always rejected.
 *
 * That is the right answer rather than a lazy one: there is no /dev/mem here
 * (shim/libkmemu/ manufactures /dev/kmem for the grovelers, in userland, and
 * it is not this device), so a negative offset reaching readi cannot be
 * anything but a bug.  A major number of 3 would open the exemption for
 * whichever device happened to get major 3.
 */
int	mem_no = -1;

/*
 * The clock.  systm.h:12-13 declare `time_t time' and `time_t bootime' as K&R
 * tentative definitions; -fcommon merges them, and one strong definition here
 * is what gives them storage.  param.h renames `time' to v8k_time to keep it
 * off libv8stubs' time(2) -- the variable-against-function collision that
 * header describes at length.
 *
 * THIS PARAGRAPH USED TO SAY `nothing advances it yet ... a filesystem that
 * stamps every inode with 0 is visibly wrong rather than subtly wrong', and
 * both halves were false by the time §8a step 5c landed.  iinit() calls
 * v8fs_settime(fp->s_time), so the clock is not 0 -- it is the moment mkfs
 * wrote the image, frozen.  That is the WORST of the three possibilities and
 * the sentence had described the best: a stamp of 0 is visibly wrong, a stamp
 * of the real time is right, and a stamp of s_time is a plausible wrong answer
 * that no comparison of the image against itself can see.
 *
 * MEASURED, and it is why §8a step 5f begins here rather than at Twrite.
 * readi sets IACC (rdwri.c:50), so iput at i_count == 1 runs IUPDAT and
 * iupdat writes `dp->di_atime = *ta' -- with *ta == time == s_time, and mkfs
 * having written di_atime == di_mtime == di_ctime == s_time on every inode.
 * So the read path's write to the disk inode stores the bytes that are
 * already there.  An instrumented driver prints the pwrite; cmp on the image
 * prints nothing.  Perturb one di_atime first and exactly four bytes move.
 * Same class as ttldioc's ten-byte over-write: the write is real, the memory
 * ends up correct, and only an instrument between the two can see it.
 *
 * THE CLOCK TICK IS MACHINE-DEPENDENT, WHICH IS WHY IT IS HERE.  Upstream
 * advances `time' from the clock interrupt in sys/clock.c, about a VAX
 * interval timer; main.c's iinit already replaces that file's clkinit() with
 * the settime call below.  v8fs_clock() is the other half of the same
 * substitution and belongs in the same file for the same reason -- one place
 * decides what the kernel thinks the time is.
 *
 * A RAW SYSCALL, NOT time(3), and rawsys.h at the top of this file was already
 * included for it -- it had no consumer until now.  libv8kern must not name a
 * libc function: tests/kmemu asserts the archive's imports are exactly
 * _memcpy, _setjmp and _longjmp, so a `time()' here would be a new import and
 * a new failure.  gettimeofday's struct is two longs and is spelled out rather
 * than including <sys/time.h>, which would drag in the host's timeval
 * alongside a kernel that has its own opinions about widths.
 */
time_t	time;
time_t	bootime;

void
v8fs_settime(time_t now)
{
	if (bootime == 0)
		bootime = now;
	time = now;
}

void
v8fs_clock(void)
{
	struct { long sec, usec; } tv;

	/*
	 * A FAILED gettimeofday LEAVES THE CLOCK ALONE rather than zeroing it,
	 * because the two answers are not equally wrong.  Freezing keeps the
	 * last good value and every stamp stays monotone; zeroing would write
	 * 1970 into an inode and iupdat would make it permanent.  This cannot
	 * fail on Darwin, which is exactly why the arm has to be written by
	 * argument rather than by observation.
	 */
	if (rawsys2(SYS_gettimeofday, (long)&tv, 0) == 0)
		v8fs_settime((time_t)tv.sec);
}

/*
 * ---------------------------------------------------------------------------
 * REAL -- the user/kernel byte movers
 * ---------------------------------------------------------------------------
 *
 * ONE ADDRESS SPACE, SO A FETCH IS A DEREFERENCE -- and the contracts are
 * still exact, because two of them are load-bearing in ways a casual stub
 * would break.  Same reasoning as copyin/copyout in shim/kern/sys/subr.c: fix
 * to the VAX's ANSWER, not to the absence of the fault.  A null address is
 * what the VAX's prober rejected and is what these reject; a non-null wild
 * pointer faults here where the VAX returned -1, which is the same gap the
 * rest of the shim has.
 *
 * fubyte ZERO-EXTENDS, and that is not a detail.  locore.s:776 is `movzbl
 * (r0),r0', so a successful fetch is 0..255 and -1 means fault and nothing
 * else.  A sign-extending version breaks BOTH consumers, differently:
 *
 *	subr.c:188	if((c = ...fubyte(u.u_base)...) < 0)
 *			-- every byte >= 0x80 would read as EFAULT
 *	nami.c:571-572	c = fubyte(u.u_dirp++); if(c == -1)
 *			-- byte 0xFF alone would, in a PATHNAME
 *
 * The second is the nastier: it tests for -1 exactly, so the bug would be a
 * single byte value failing to resolve, in one component of one path.
 *
 * fuibyte and suibyte are the I-space spellings.  On a VAX they are the same
 * routines -- asm.sed:36-37 and :45-47 rewrite both names to _Fubyte and
 * _Subyte -- and here there is likewise one address space, so they forward.
 * They are separate functions rather than #defines because subr.c calls them
 * through a conditional expression and a macro would be evaluated there too.
 */
int
fubyte(caddr_t addr)
{
	if (addr == NULL)
		return (-1);
	return (*(unsigned char *)addr);	/* movzbl: 0..255, never < 0 */
}

int
fuibyte(caddr_t addr)
{
	return (fubyte(addr));
}

/*
 * subyte RETURNS EXACTLY 0 ON SUCCESS, and "exactly" is doing real work.
 * locore.s:815 is `clrl r0'.  The reason it matters is a precedence accident
 * in authentic source -- src/sys/sys/subr.c:162 is
 *
 *	if(id?suibyte(u.u_base, c):subyte(u.u_base, c) < 0) {
 *
 * and `?:' binds looser than `<', so this parses as
 *
 *	id ? suibyte(...) : (subyte(...) < 0)
 *
 * On the id != 0 arm -- u_segflg == 2, user I-space -- the RAW return value is
 * the truth value.  Any nonzero success return, including a byte count or a 1,
 * sets u.u_error = EFAULT on every successful store down that arm.  The
 * subyte arm is normalised by the `< 0' and would survive it, which is what
 * makes the bug arm-specific and quiet.
 *
 * Note this is upstream's own line, not a transcription error here: the file
 * is byte-identical to V8's and tests/streams hash-guards it.
 */
int
subyte(caddr_t addr, int c)
{
	if (addr == NULL)
		return (-1);
	*(unsigned char *)addr = (unsigned char)c;
	return (0);			/* clrl r0 -- EXACTLY zero */
}

int
suibyte(caddr_t addr, int c)
{
	return (subyte(addr, c));
}

/*
 * fustrlen RETURNS THE LENGTH INCLUDING THE NUL.  locore.s:795-797 computes
 * the difference to the NUL and then `incl r0'.
 *
 * Its one caller depends on that, and depends on it for the byte that
 * terminates a pathname -- nami.c:36-46:
 *
 *	if((i = fustrlen(u.u_dirp)) < 0)	{ EFAULT }
 *	if(i > BUFSIZE)				{ ENOENT }
 *	bcopy(u.u_dirp, p.nbp->b_un.b_addr, i);
 *
 * The bcopy moves exactly i bytes into the namei buffer, so a bare strlen
 * would copy an UNTERMINATED pathname and every component after the buffer's
 * previous contents would be part of the name.  The `i > BUFSIZE' bound is
 * calibrated to the NUL-inclusive count too.
 *
 * Upstream probes a page at a time and returns -1 on the first unreadable
 * one, with no length cap.  Here the only unreadable address we can detect
 * cheaply is null, matching fubyte and copyin.
 */
int
fustrlen(caddr_t addr)
{
	caddr_t p;

	if (addr == NULL)
		return (-1);
	for (p = addr; *p != '\0'; p++)
		;
	return ((int)(p - addr) + 1);	/* incl r0 -- the NUL is counted */
}

/*
 * ---------------------------------------------------------------------------
 * REAL -- scheduling and priority
 * ---------------------------------------------------------------------------
 *
 * spl0 IS NOT HERE.  It is the twentieth service and it lives in
 * shim/kern/dev/machdep.c beside spl6 and splx, because splevel is static to
 * that file and lowering the level has to go through splx() so a deferred
 * queuerun() actually runs.  machdep.c has the account, including asm.sed:2-3
 * settling that it returns the previous level rather than void.
 */

/*
 * sleep -- the void-returning V8 sleep(chan, pri), on top of the shim's
 * tsleep.  param.h renames it v8k_sleep; libv8c has a sleep(3).
 *
 * THE PRIORITY IS NOT DECORATION.  slp.c:46-60 makes a sleep at pri > PZERO
 * interruptible, and when a signal is pending it does NOT return to its
 * caller -- slp.c:78 is `longjmp(u.u_qsav)', marked NOTREACHED.  That is
 * the system-call abort path, and shim/kern/sys/slp.c's v8k_stcall() already
 * provides the setjmp end of it for streamio.c.
 *
 * Reproducing it matters because the callers are written for it.  iget.c:93
 * and alloc.c:89,215,295 sleep at PINOD (10) waiting for a locked inode; if a
 * signal arrives, upstream unwinds out of the whole system call rather than
 * returning into a loop that will sleep again.  A version that always returned
 * normally would turn every interruptible inode wait into an uninterruptible
 * one -- the process would be unkillable while another holds the lock.
 *
 * At pri <= PZERO (bio.c's PSWP and PRIBIO waits) it always returns normally,
 * which is upstream slp.c:61-65.
 */
void
sleep(caddr_t chan, int pri)
{
	if (tsleep(chan, pri, 0) == TS_SIG && pri > PZERO)
		longjmp(u.u_qsav);	/* slp.c:78 -- NOTREACHED */
}

/*
 * plock -- lock an inode, sleeping until it is free.  pipe.c:105-114.
 *
 * IT HAS TO BE A REAL FUNCTION HERE, and which of the six gets the macro and
 * which gets the function is decided by an include line rather than by
 * anything visible at the call:
 *
 *	iget.c:13	includes "../h/inline.h"  -> plock is a MACRO
 *	nami.c		does not			 -> plock is a CALL
 *
 * and it is nami.c that calls it, three times (:74, :389, :433); iget.c never
 * does.  So the macro form is dead in this port and the out-of-line one is
 * load-bearing -- the exact opposite of what reading inline.h first suggests.
 * Upstream keeps both for the same reason and undefs the macro at pipe.c:94-96
 * so the definition survives.
 *
 * The body is upstream's, transcribed rather than reduced, because every line
 * is still meaningful: prele() sets IWANT and wakes this channel, and dropping
 * the loop would make a second waiter miss the wakeup.
 */
void
plock(struct inode *ip)
{
	while (ip->i_flag & ILOCK) {
		ip->i_flag |= IWANT;
		sleep((caddr_t)ip, PINOD);
	}
	ip->i_flag |= ILOCK;
}

/*
 * ---------------------------------------------------------------------------
 * REAL -- permission
 * ---------------------------------------------------------------------------
 *
 * access() IS INVERTED FROM THE access(2) EVERY C PROGRAMMER KNOWS: 0 means
 * PERMITTED and 1 means DENIED (fio.c:174-204).  param.h renames it v8k_access
 * because libv8stubs.a(access.o) has the userland one, and the two would
 * otherwise be one symbol with opposite polarity -- the worst shape of link
 * collision, since it links cleanly and inverts every permission check.
 *
 * AND THE RETURN VALUE IS NOT THE ONLY CHANNEL.  nami.c:232 is
 *
 *	(void) access(dp, IEXEC);
 *
 * -- the result discarded -- and nami.c:240 then tests `if(u.u_error)'.  So a
 * version that returned the right answer without setting u.u_error would leave
 * directory-traversal permission unchecked while looking correct at five of
 * the six call sites.  Each denial arm sets its own errno, and they differ.
 *
 * THE s_ronly ARM IS BACK, §8a step 5d, AND THE NOTE THAT SAID IT COULD NOT BE
 * WAS RIGHT WHEN WRITTEN AND WENT STALE WITHOUT BEING TOUCHED.  It read:
 *
 *	the s_ronly check needs a mounted filesystem, and there is no mount
 *	table populated here yet ... Restore the s_ronly arm when v8fs learns
 *	to mount read-only.
 *
 * §8a step 5c gave it one -- shim/kern/sys/main.c's iinit() calls allocmount()
 * and sets fp->s_ronly -- so getfs(ip->i_dev) has answered since that step
 * landed, and this arm was simply still absent.  Same shape as conf.h's
 * "the switch tables are deliberately absent" paragraph, and as the
 * shim/kern/h/buf.h that died when a third file arrived: A NOTE RECORDING
 * WHY SOMETHING IS IMPOSSIBLE DOES NOT NOTICE WHEN IT BECOMES POSSIBLE.  What
 * made it visible is that step 5d is the first step to CALL access() with
 * IWRITE at all -- nami.c:485, :496 and :517 are the only sites that do, and
 * all three are in the create path.
 *
 * The guard `!ip->i_fstyp' is upstream's own (fio.c:181) and is load-bearing
 * here rather than decorative: a non-zero i_fstyp means the inode belongs to
 * one of the filesystem types this port did not import, whose device number
 * getfs() would not find a mount for -- and getfs() PANICS on a miss.
 *
 * ONE ARM IS STILL DROPPED and it stays dropped: ITEXT/xrele needs shared
 * text, which this port does not have (see xrele below).
 */
int
v8k_access_impl(struct inode *ip, int mode)
{
	register int m = mode;

	if (m == IWRITE) {			/* fio.c:180 */
		if (!ip->i_fstyp && getfs(ip->i_dev)->s_ronly != 0) {
			u.u_error = EROFS;
			return (1);		/* fio.c:181-184 */
		}
		/* fio.c:185-189, the ITEXT/xrele arm, omitted -- no shared text */
	}
	if (u.u_uid == 0)
		return (0);			/* fio.c:193 */
	if (u.u_uid != ip->i_uid) {
		m >>= 3;
		if (u.u_gid != ip->i_gid)
			m >>= 3;
	}
	if ((ip->i_mode & m) != 0)
		return (0);			/* fio.c:200 */
	u.u_error = EACCES;
	return (1);				/* fio.c:203 */
}

int
access(struct inode *ip, int mode)
{
	return (v8k_access_impl(ip, mode));
}

/*
 * suser -- 1 IS PERMITTED HERE, which is the opposite polarity from access()
 * one function above.  fio.c:234-243.  Two permission predicates in one
 * kernel with inverted senses is upstream's own inconsistency and is
 * reproduced rather than harmonised; nami.c:315 spells it `!suser()'.
 *
 * u_acflag |= ASU is upstream's accounting side effect and is kept: it is one
 * bit, it costs nothing, and acct.h is imported.
 */
int
suser(void)
{
	if (u.u_uid == 0) {
		u.u_acflag |= ASU;		/* fio.c:238 */
		return (1);
	}
	u.u_error = EPERM;
	return (0);
}

/*
 * findmount -- sys3.c:224-235, transcribed.  Declared by the AUTHENTIC
 * src/sys/h/mount.h:22, so that declaration is the prototype and this
 * definition answers to it.
 *
 * The two callers differ in what they do about a NULL: alloc.c:418's getfs()
 * PANICS, at :426, and alloc.c:461's getfsx() returns -1, at :470.  Both are
 * correct answers for a system with nothing mounted, and the panic is the one
 * that fires first when v8fs is handed an image with no mount entry -- which
 * is the right way to find out.  Since §8a step 5c there IS one: main.c's
 * iinit() calls allocmount below, and tests/streams/fsprobe.c reaching getfs
 * at all is the assertion that it worked.
 *
 * THREE THINGS IN THAT SENTENCE WERE WRONG AND ONE OF THEM IS BELL LABS'.
 * It used to read `alloc.c:381's getfs() panics with "no fs" and alloc.c:425's
 * getfsx() returns -1'.
 *
 *   THE LINE NUMBERS were written before this port put a 43-line PORT comment
 *   at the top of alloc.c for the second NOLONG deviation, and inserting a
 *   comment silently moves every citation below it.  Eight citations across
 *   five files went stale the same way, all by exactly +43.
 *
 *   THE PANIC STRING IS `getfs', not "no fs" -- and "no fs" is not invented.
 *   It is upstream's own words at alloc.c:414, in the comment block directly
 *   above the function: `panic: no fs -- the device is not mounted.'  Twelve
 *   lines below it the code says panic("getfs").  So BELL LABS' COMMENT IS
 *   STALE AGAINST BELL LABS' CODE, and this port read the comment and wrote it
 *   down as the behaviour.  CLAUDE.md's rule is that a recorded diagnosis is a
 *   hypothesis until re-measured; it turns out that applies to the imported
 *   half's own comments too, which is the one place the fidelity contract
 *   guarantees we will not have checked them.
 */
/*
 * THE DEFINITION IS OLD-STYLE, IN A FILE THAT IS OTHERWISE MODERN C, AND THAT
 * IS FORCED RATHER THAN STYLISTIC.
 *
 * src/sys/h/mount.h:22 is authentic and says `extern struct mount
 * *findmount();' -- an empty parameter list.  A prototyped definition here
 * conflicts with it, and NOT merely by C99's rule that `()' means `(void)':
 * it is incompatible in C89 too, because the second parameter is dev_t, which
 * this port narrows to u_short (shim/kern/h/param.h), and a u_short parameter
 * IS affected by the default argument promotions.  C89 says a `()' declaration
 * is compatible with a prototype only when no parameter is.
 *
 * So the choice is to edit an authentic header or to write the definition the
 * way upstream wrote it (sys3.c:224-226, K&R, `dev_t dev' on its own line).
 * The second is the fidelity contract's answer and costs one deprecation
 * warning, suppressed for this file in the Makefile with this citation.
 * mfind below has the identical situation via cmap.h:36.
 */
struct mount *
findmount(fstyp, dev)
	int fstyp;
	dev_t dev;
{
	register struct mount *mp;

	for (mp = mount; mp < mount + NMOUNT; mp++)
		if (mp->m_flags & M_MOUNTED && mp->m_dev == dev &&
		    mp->m_fstyp == fstyp)
			return (mp);
	return (NULL);
}

/*
 * allocmount -- sys3.c:239-259, transcribed.  findmount's sibling, declared by
 * the same authentic header (mount.h:23), old-style for the same dev_t reason,
 * and here because §8a step 5c gave it its first caller: shim/kern/sys/main.c's
 * iinit(), which is where a root filesystem gets its mount entry.
 *
 * THE `if' AT :247 IS TRANSCRIBED EXACTLY AND IT LOOKS LIKE A BUG.  Upstream is
 *
 *	if(!mp->m_flags&M_MOUNTED) {
 *
 * -- and `!' binds tighter than `&', so what it computes is
 * `(mp->m_flags == 0) & M_MOUNTED', not `!(mp->m_flags & M_MOUNTED)'.  Those
 * are different expressions and they agree here for one reason: M_MOUNTED is 1
 * (mount.h:27) and it is THE ONLY FLAG the structure has.  A free slot has
 * m_flags 0, so `1 & 1' is 1; a mounted slot has m_flags 1, so `0 & 1' is 0.
 * Correct, by a coincidence between an operator precedence and a bit value.
 *
 * It is left exactly as written, because the fidelity contract says a change to
 * imported logic must be forced by the target and this one would be forced by
 * taste -- and because "correct today, wrong the day a second flag is defined"
 * is upstream's risk to have taken, and recording it is worth more than
 * quietly repairing it.  Note which direction it would fail in: a second flag
 * on a mounted slot leaves the expression at 0, so the slot stays unavailable;
 * a second flag on a FREE slot makes `!m_flags' 0 and the slot becomes
 * invisible to the allocator.  Both lose a slot rather than double-mounting
 * one, which is the safe direction.
 */
struct mount *
allocmount(fstyp, dev)
	int fstyp;
	dev_t dev;
{
	register struct mount *mp, *free;

	free = NULL;
	for (mp = mount; mp < mount + NMOUNT; mp++) {
		if (!mp->m_flags & M_MOUNTED) {
			if (free == NULL)
				free = mp;
		} else if (mp->m_dev == dev && mp->m_fstyp == fstyp)
			return NULL;	/* mounted twice */
	}
	if (free != NULL) {
		free->m_flags |= M_MOUNTED;
		free->m_dev = dev;
		free->m_fstyp = fstyp;
	}
	return free;
}

/*
 * NOTE THIS FILE DOES NOT DEFINE `mount', though findmount walks it.
 * src/sys/h/mount.h:21 is `struct mount mount[NMOUNT];' -- a K&R TENTATIVE
 * definition inside #ifdef KERNEL, so every imported file that includes it
 * emits a common symbol and the linker merges them into one array in bss.
 * That is the 1985 idiom -fcommon exists to keep working, and adding a strong
 * definition here would collide with it rather than complete it.
 *
 * This file is therefore compiled with -DKERNEL and -fcommon like the imported
 * half, not with bare SHIMFLAGS -- see the Makefile, which says so.  The same
 * mechanism supplies inode, inodeNINODE, ninode and rootdir from inode.h's
 * guarded block: none of the four needs a definition here, and writing one
 * would be four collisions found the hard way.
 */

/*
 * tablefull -- prf.c:183-188, verbatim.  One caller, iget.c:114, which then
 * sets ENFILE and returns NULL.
 */
void
tablefull(char *tab)
{
	printf("%s: table is full\n", tab);
}

/*
 * uprintf -- GENUINELY VARIADIC, and this is the one service that CANNOT be a
 * transcription.
 *
 * Upstream is prf.c:55-61:
 *
 *	uprintf(fmt, x1) char *fmt; unsigned x1;
 *	{ prf(fmt, &x1, 2); }
 *
 * -- take the address of the first named argument and stride forward through
 * the others, which prf() does with `register u_int *adx' and `*adx++'.  That
 * is the &args idiom CLAUDE.md records being handled in seven files, and it is
 * IMPOSSIBLE under AAPCS64 rather than merely wrong: variadic arguments are
 * not a contiguous array beginning at &x1.  So the prototype in param.h is
 * `(const char *, ...)' and the body uses stdarg -- if it were declared with a
 * fixed argument list, the three call sites would compile as non-variadic
 * calls and the compiler would place arguments where this body will not look.
 *
 * WHERE IT GOES IS A DELIBERATE DEVIATION.  Upstream's third argument to prf
 * is 2, meaning "to the user", and prf.c:215-219 has that branch COMMENTED OUT
 * -- so V8's uprintf reaches the kernel message buffer and never a terminal.
 * Reproducing that faithfully would make alloc.c's two "file system is full"
 * warnings and bio.c's "sorry, pid %d was killed" silent.  They are diagnostics
 * whose entire purpose is to reach a person, so they go to the kernel printf,
 * which machdep.c already routes.  Recorded because it is a place this port
 * deliberately does not reproduce 1985.
 */
void
uprintf(const char *fmt, ...)
{
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);
	kvprintf(fmt, ap);
	__builtin_va_end(ap);
}

/*
 * ---------------------------------------------------------------------------
 * ANSWER -- two, and both are the value V8's own code would produce here
 * ---------------------------------------------------------------------------
 *
 * mfind -- vmmem.c:422-435 searches the core map for a page holding a given
 * disk block.  There is no core map, so nothing is ever found, and the answer
 * is NULL for every argument.
 *
 * IT MUST BE DECLARED struct cmap *, not int.  cmap.h:36 -- authentic, and
 * included by rdwri.c:10 -- says `struct cmap *mfind();'.  Its one caller is
 * rdwri.c:182, `if (bn && mfind(dev, bn))', a presence test that discards the
 * pointer; so an int-returning version would truncate an address that is
 * always null anyway and nothing would ever show it.  The type is right
 * because the declaration is upstream's, not because a test could catch it.
 *
 * And returning NULL is not merely safe, it is what keeps munhash unreachable:
 * rdwri.c:183 calls munhash ONLY inside this guard, which is the caller
 * contract munhash panics to enforce.
 */
struct cmap *
mfind(dev, bn)			/* K&R for findmount's reason; cmap.h:36 */
	dev_t dev;
	daddr_t bn;
{
	return ((struct cmap *)0);
}

/*
 * xrele -- text.c:229-239 releases a shared-text segment.  Empty here, and
 * "empty" is exactly faithful rather than a simplification: upstream's first
 * statement is `if ((ip->i_flag&ITEXT)==0) return;', ITEXT is never set in a
 * system with no shared text, so upstream's own body reduces to the same
 * nothing.
 *
 * It is not even entered.  Its one call site among the six, nami.c:320, is
 * itself guarded by `if(dip->i_flag&ITEXT)' on the line above.
 */
void
xrele(struct inode *ip)
{
}

/*
 * ---------------------------------------------------------------------------
 * PANIC -- five, all VAX virtual memory, all reached only from bio.c's swap
 * ---------------------------------------------------------------------------
 *
 * Each one names itself, because panic()'s message is the only thing a person
 * will have.  V8's own convention is the bare function name (vmmem.c:394's
 * `panic("munhash")'), and it is kept.
 *
 * These are NOT unreachable-by-luck.  physio() and swap() are the callers, and
 * this port has no swap device and no raw disk, so nothing constructs the
 * arguments.  If one ever fires it means a path was opened that needs a pager
 * underneath, which is a decision rather than a bug -- and the panic is what
 * makes it a decision someone takes rather than one that happens.
 */
void
munhash(dev_t dev, daddr_t bn)
{
	panic("munhash: no core map (vmmem.c:385)");
}

int
useracc(caddr_t base, unsigned count, int rw)
{
	panic("useracc: no page tables (locore.s:845)");
	return (0);			/* NOTREACHED; keeps the type honest */
}

void
vslock(caddr_t base, int count)
{
	panic("vslock: no page tables (vmmem.c:567)");
}

void
vsunlock(caddr_t base, int count, int rw)
{
	panic("vsunlock: no page tables (vmmem.c:594)");
}

struct pte *
vtopte(struct proc *p, unsigned v)
{
	panic("vtopte: no page tables (vmsubr.c:76)");
	return ((struct pte *)0);	/* NOTREACHED */
}
