/*
 * The kernel's param.h, as much of it as V8's stream machinery asks for.
 *
 * THIS FILE IS OURS, not Bell Labs'.  src/sys/dev/stream.c is authentic and
 * byte-identical to upstream; this is the machine-dependent half it compiles
 * against, in the same relationship compiler/ccom-arm64/macdefs.h has to ccom.
 *
 * HOW THE INCLUDE PATH WORKS, because it is doing something deliberate.
 * stream.c says #include "../h/param.h", and a quoted include is resolved
 * against the INCLUDING FILE's directory first.  So from src/sys/dev/ it tries
 * src/sys/h/param.h, which does not exist, and only then falls through to -I.
 * The build passes -Ishim/kern/dev, so "../h/param.h" lands here.
 *
 * The consequence is the property worth having: WHERE AN AUTHENTIC HEADER
 * EXISTS IT WINS, and ours fills only the gaps.  src/sys/h/stream.h is real V8
 * and is picked up by the same rule, from the same #include syntax, with no
 * flag saying which is which.  Adding an authentic param.h later displaces this
 * one without touching a build rule.
 *
 * WHY NOT IMPORT V8'S param.h.  Measured: stream.c's entire dependency on the
 * kernel headers is nine names -- NULL, caddr_t, u_char, u_short, spl6, splx,
 * panic, printf and one uballoc.  V8's is 185 lines of VAX page sizes, cluster
 * counts and process limits, pulling <signal.h> and ../h/types.h behind it.
 * Importing it would mean carrying a description of a machine that is not here
 * in order to obtain four typedefs.
 *
 * STREAMIO.C WIDENED THIS AND DID NOT CHANGE THE ARGUMENT.  It asks for six
 * more constants and seven typedefs, and every one of them is copied from
 * upstream at the value upstream gives it -- the h/param.h line number is on
 * each.  What is still absent is what the reasoning above excludes: NBPG,
 * CLSIZE, MAXUPRC, the swap and cmap tunables, the u-area's virtual address.
 * Those describe a VAX.  A constant that is a pure number keeps its value; a
 * constant that is a fact about hardware is not here at all.
 */

#ifndef V8KERN_PARAM_H
#define V8KERN_PARAM_H

#ifndef NULL
#define NULL	0
#endif

/*
 * V8's jmp_buf, so that u_qsav and v8k_longjmp can be declared honestly.
 * shim/kern/h/user.h says why it is V8's and not the host's.
 *
 * THE GUARD IS OURS BECAUSE THE HEADER HAS NONE.  src/include/setjmp.h is a
 * bare `typedef long jmp_buf[24];' -- V8 headers predate include guards -- and
 * a repeated typedef is legal in C11 and an error in the gnu89 that
 * src/sys/sys/streamio.c is compiled in.  Guarding here rather than editing
 * src/include keeps a header a V8 program compiles against unchanged for a
 * reason that is entirely ours.
 */
#ifndef V8KERN_JMP_BUF
#define V8KERN_JMP_BUF
#include "../../../src/include/setjmp.h"
#endif

/*
 * Forward declarations, so the services below can be real prototypes.
 *
 * Four of them take pointers to structs this header does not describe --
 * proc.h and user.h are ours and inode.h and file.h are Bell Labs', and all
 * four are included AFTER this file by everything that reads it.  Naming the
 * tags here costs nothing and is what lets psignal, selwakeup, closef and iput
 * be checked at their call sites instead of being declared `()' and taken on
 * trust.
 */
struct proc;
struct file;
struct inode;
struct streamtab;

typedef char *		caddr_t;
typedef unsigned char	u_char;
typedef unsigned short	u_short;
typedef unsigned int	u_int;
typedef unsigned long	u_long;

/*
 * The rest of h/types.h, for the AUTHENTIC headers streamio.c pulls in --
 * src/sys/h/inode.h, file.h and dir.h are upstream's and are spelled in these.
 *
 * WIDTHS ARE UPSTREAM'S EXCEPT WHERE THIS PORT HAS ALREADY DECIDED OTHERWISE,
 * and the rule is the one hazard 2 states in src/sys/PORTING.md: A STRUCT THAT
 * CROSSES A SEAM KEEPS THAT SEAM'S WIDTH.  Nothing in libv8kern crosses to a
 * V8 program or to a disk record, so upstream's widths stand -- with one
 * exception, and it is an exception because the port made it globally:
 *
 *   daddr_t   upstream `long', 32 bits there under NOLONG.  src/include's
 *             daddr_t is already narrowed to int for exactly that reason
 *             (src/include/PORTING.md), and inode.h's i_addr[13] is an
 *             on-disk shape.  Narrowed here to agree, so that the day a v8fs
 *             server reads a real image the two declarations cannot disagree.
 *
 *   ino_t     u_short, dev_t u_short, off_t long, time_t long, size_t long,
 *             label_t long[14] -- upstream's, unchanged.  ino_t and dev_t are
 *             two of the 16-bit ranges CLAUDE.md tabulates; they are correct
 *             here because the numbers in them are the SHIM's, not the host's.
 *
 * label_t is declared for completeness and is deliberately NOT what
 * shim/kern/h/user.h uses for u_qsav -- see hazard 4 there.  56 bytes will not
 * hold an ARM64 jump buffer, and longjmp(u.u_qsav) has to work.
 */
/*
 * THREE OF THESE ARE NAMES DARWIN ALSO OWNS, AND TWO OF THE THREE ARE A
 * DIFFERENT WIDTH.  Measured, by including <stdio.h> and <sys/types.h> ahead
 * of this file and reading the errors:
 *
 *	off_t	long (8)	vs __darwin_off_t   long long (8)
 *	dev_t	u_short (2)	vs __darwin_dev_t   int (4)
 *	ino_t	u_short (2)	vs __darwin_ino_t   u long long (8)
 *
 * off_t is the same width either way and harmless.  The other two are not:
 * src/sys/h/inode.h is `dev_t i_dev; long i_number; ...' and the host's widths
 * move every field after i_dev by twelve bytes.  So WHICH DEFINITION WINS
 * WOULD DEPEND ON INCLUDE ORDER, and two objects in one link could disagree
 * about the layout of struct inode with nothing to say so -- the same shape as
 * the DIRSIZ-in-three-headers trap, arriving through a host header instead of
 * through ours.
 *
 * The fix is the one a kernel header has always used: CLAIM DARWIN'S OWN GUARD
 * MACROS.  <sys/_types/_off_t.h> is `#ifndef _OFF_T', and _INO_T and _DEV_T
 * likewise, so defining them here makes the host's typedefs no-ops and ours
 * the only ones -- for any file that includes param.h first.
 *
 * And for a file that does NOT, the #error below is the whole point.  A silent
 * loss would be a struct layout differing between two objects; an #error is a
 * build failure with an instruction in it.  tests/streams/probe.c is the file
 * that has to obey it, and its includes are ordered for this reason.
 */
#if defined(_OFF_T) || defined(_INO_T) || defined(_DEV_T)
#error "include shim/kern/h/param.h BEFORE any host header: the kernel's dev_t \
and ino_t are narrower than Darwin's, so include order would silently change \
the layout of struct inode."
#endif
#define _OFF_T
#define _INO_T
#define _DEV_T

/*
 * SO THE INCLUDE ORDER FOR ANYTHING THAT WANTS BOTH THIS FILE AND HOST
 * HEADERS IS FIXED, AND IT IS NOT THE OBVIOUS ONE:
 *
 *	#include "../h/param.h"		-- first, to claim the types
 *	#undef printf			-- and the redirects, before the host
 *	#undef bcopy			   declares its own printf/psignal
 *	#undef psignal
 *	#undef longjmp
 *	#include <stdio.h>		-- now
 *
 * Both halves are forced.  param.h has to be first or the types are wrong;
 * the #undefs have to be before <stdio.h> or `#define printf v8k_printf'
 * rewrites stdio's own declaration into a conflicting prototype for
 * v8k_printf.  Nothing in shim/kern itself needs host headers -- rawsys.h
 * reaches the kernel directly -- so the file this applies to is
 * tests/streams/probe.c, which says so at the top.
 */

typedef int		daddr_t;	/* h/types.h:23 `long'; see above */
typedef u_short		ino_t;		/* h/types.h:25 */
typedef u_short		dev_t;		/* h/types.h:30 */
typedef long		off_t;		/* h/types.h:31 */
typedef long		v8k_time_t;	/* h/types.h:28 -- renamed; see below */
typedef long		label_t[14];	/* h/types.h:29 -- see hazard 4 */

/*
 * time_t AFTER ALL, AND THE NOTE BELOW SAYING WHY NOT IS STILL RIGHT -- what
 * changed is that §8a step 5 imported a file that spells the name.
 *
 * src/sys/h/systm.h:12-13 are `time_t time;' and `time_t bootime;', authentic
 * and now in the tree, so the type has to exist for the kernel side.  The
 * reason it was deferred was NOT "no file uses it"; it was that claiming
 * Darwin's _TIME_T guard would hand this directory's own raw syscalls a
 * 32-bit time_t.  That argument was about a 32-bit typedef, and it does not
 * apply to this one:
 *
 *	__darwin_time_t is `long'	(SDK arm/_types.h:119)
 *	ours is `long'			(the line below)
 *
 * -- so claiming _TIME_T makes the host's typedef a no-op that would have
 * produced the identical type.  Measured, not assumed.  Contrast dev_t and
 * ino_t above, where claiming the guard genuinely changes the width and the
 * #error exists to catch include order.  time_t is not added to that #error
 * for exactly that reason: a file that gets Darwin's first is unharmed.
 *
 * IT IS 64 BITS WHERE THE VAX'S WAS 32, AND THAT IS THE SEAM RULE RATHER THAN
 * A LAPSE.  `time' crosses nothing: it is a kernel global, not a member of any
 * record.  The on-disk times ARE narrowed, per field, in src/include/sys/ino.h
 * and filsys.h -- `v8_i32 di_mtime' -- so iget.c:274-276's `dp->di_atime =
 * *ta' truncates on assignment exactly where 1985's four bytes are wanted, and
 * nowhere else.  The hazard CLAUDE.md records is the ADDRESS of a narrowed
 * field being taken as a time_t*, and the five sites here go the safe way:
 * `iupdat(ip, &time, &time, 0)' takes the address of the WIDE global and reads
 * it through iget.c:235's `time_t *ta'.  Swept over all of src/sys -- zero
 * hits for the ctime/localtime/gmtime/asctime/time-of-& pattern.
 *
 * swblk_t exists for one declaration and no definition: systm.h:67 is
 * `swblk_t vtod();', a swap-map function nothing in the six imported files
 * calls.  It is `int' rather than `long' for daddr_t's reason -- upstream's
 * h/types.h:26 says `long', which is 32 bits under NOLONG, and it names a
 * block number.  Nothing can observe the choice today; making it agree with
 * daddr_t means the day something does, it agrees with the disk.
 */
#ifndef _TIME_T
#define _TIME_T
typedef long		time_t;		/* h/types.h:28; == __darwin_time_t */
#endif
typedef int		swblk_t;	/* h/types.h:26 `long'; see daddr_t */

/*
 * The four width names, and they are NOT upstream's -- they are this port's,
 * from src/include/sys/types.h:78-81, added by §8a step 4a when it turned out
 * that `int di_size' did not mean "an int" but "exactly four bytes, because a
 * VAX wrote four bytes there", with the declaration and the reason in
 * different files.
 *
 * They are here because shim/kern/h/{filsys,ino,fblk}.h forward to the
 * patched src/include/sys/ copies of three ON-DISK RECORDS -- one record,
 * one declaration -- and those copies are spelled in these names.  The
 * alternative was to include src/include/sys/types.h itself, which cannot be
 * done: it re-typedefs daddr_t, ino_t, dev_t and off_t, three of them at
 * widths this file deliberately narrows for the kernel side.
 *
 * SO THE SAME NAME IS NOW DECLARED IN TWO FILES, WHICH IS THE THING THIS
 * PORT KEEPS GETTING CAUGHT BY.  It is safe only if the two agree, and
 * "safe" is not a property to assert in a comment: tests/streams compares
 * the sizeof of every one of them against src/include/sys/types.h rather
 * than against a number typed here.  Same discipline as making the header
 * test compare NMASK(0) against the sizeof-derived NINDIR.
 */
typedef short		v8_i16;		/* src/include/sys/types.h:78 */
typedef unsigned short	v8_u16;		/* :79 */
typedef int		v8_i32;		/* :80 -- a VAX `long' */
typedef unsigned int	v8_u32;		/* :81 */

/*
 * time_t is NOT typedef'd here, and it is the one name where deferring beats
 * claiming.  Nothing in libv8kern needs a kernel time_t -- no field of any
 * struct these headers declare has that type -- so there is no layout to
 * protect, and claiming _TIME_T would hand the modern C in shim/kern a 32-bit
 * time_t for its raw syscalls.  v8k_time_t exists so that the day something
 * does need one, it is spelled once and visibly ours.
 */

/*
 * Tunables.  Values are upstream's, with h/param.h's line number, because a
 * tunable that quietly differs from V8's is a difference in behaviour wearing
 * the same name.
 */
#define	NOFILE	128		/* h/param.h:19 -- max open files per process */
#define	PRIBIO	20		/* h/param.h:32 -- sleep priority, block I/O */
#define	NZERO	20		/* h/param.h:40 -- the nice(2) origin */

/*
 * Return values from tsleep().  h/param.h:54-56.
 */
#define	TS_OK	0		/* normal wakeup */
#define	TS_TIME	1		/* timed-out wakeup */
#define	TS_SIG	2		/* asynchronous signal wakeup */

/*
 * THE FILESYSTEM TUNABLES -- §8a step 5.  Values are upstream's, with the
 * h/param.h line, by this file's standing policy.
 *
 * NICINOD AND NICFREE ARE DEFINED THREE TIMES UPSTREAM AND THE THREE DISAGREE,
 * which is worth saying because picking the wrong one is silent.  h/param.h
 * has a `#if CLSIZE==1' block (:102-103, NICFREE 50), a `#if CLSIZE==2' block
 * (:116-117, NICFREE 178) and a `#if CLSIZE==4' block (:132-133, NICFREE 434).
 * They are the superblock's free-block cache, whose length is a function of
 * the block size, so a wrong choice changes sizeof(struct filsys) -- an
 * ON-DISK RECORD.
 *
 * This port is CLSIZE==2, and that is not a decision taken here: it is already
 * committed in src/include/sys/param.h:110, which is the USERLAND half that
 * mkfs, icheck, dcheck, fsck, ncheck, dump and restor all compile against, and
 * whose src/include/sys/filsys.h:42 sizes S_free[NICFREE].  The kernel side
 * has to agree with it or the two halves of this port would describe the same
 * superblock differently.
 *
 * AND THAT AGREEMENT IS ASSERTED RATHER THAN CLAIMED.  shim/kern/sys/v8fs.c
 * carries a _Static_assert on sizeof(struct filsys), which is the quantity
 * that actually has to be right -- a transcription check on the number 178
 * would pass while the two headers still disagreed about something else.
 * Same discipline as making the header test compare NMASK(0) against the
 * sizeof-derived NINDIR rather than against a typed-in constant.
 */
#define	NMOUNT	62		/* h/param.h:10 -- mountable file systems */
#define	MSWAPX	15		/* h/param.h:11 -- pseudo mount index, swapdev */
#define	CANBSIZ	256		/* h/param.h:21 -- max typewriter line */
#define	NODEV	(dev_t)(-1)	/* h/param.h:72 */
#define	ROOTINO	((ino_t)2)	/* h/param.h:73 -- i number of all roots */
#define	SUPERB	((daddr_t)1)	/* h/param.h:74 -- block no. of the superblock */
#define	NICINOD	100		/* h/param.h:116 -- CLSIZE==2; see above */
#define	NICFREE	178		/* h/param.h:117 -- CLSIZE==2; see above */
#define	BUFSIZE	4096		/* h/param.h:109 -- CLSIZE==2 */

/*
 * THE BLOCK-GEOMETRY MACROS, AND LEAVING THEM OUT WAS SILENT.
 *
 * These are the rest of upstream's CLSIZE==2 block (h/param.h:107-122).  The
 * first draft of this section added the three constants above and stopped,
 * on the reasoning that a constant is what a header owes and the macros would
 * follow if anything wanted them.  Everything compiled.  Everything was wrong:
 *
 *	KERNFLAGS carries -Wno-implicit-function-declaration, because the
 *	imported half is 1985 K&R and that diagnostic would fire on every
 *	line.  So `BSIZE(dev)' with no macro in scope is not an error and not
 *	even a warning -- it is a CALL to an undefined function named BSIZE,
 *	compiled without complaint, left as an undefined symbol in the object.
 *
 * FOURTEEN OF THEM.  Found with `nm -u' on libv8kern.a, subtracting what the
 * archive defines from what it undefines -- the same instrument tests/streams
 * uses for libc leaks, and the only one that could see this, because the
 * compiler had been told by us not to speak.  The list was BITFS BMASK BSHIFT
 * BSIZE INOPB MIN NINDIR NMASK NSHIFT dbtofsb fsbtodb itod itoo major.
 *
 * That is the sharpest instance yet of a rule this file already states for
 * src/sys/: A SUPPRESSION ARGUED FOR ONCE COVERS CODE NOBODY ARGUED ABOUT.
 * -Wno-implicit-function-declaration was argued for K&R declarations and it
 * silently also covers a missing macro, which is a different thing entirely --
 * and unlike a missing declaration, a missing macro changes what the code
 * MEANS.  `itod(dev, ino)' is address arithmetic; as a function call it is a
 * link error at best and a call to something else's itod at worst.
 *
 * The values are upstream's CLSIZE==2 arm verbatim, which is the arm this port
 * is on for the reason NICFREE above gives at length.  BITFS(dev) is the bit
 * that selects 4096-byte blocks over 1024, so every one of these is a function
 * of the device rather than a constant -- that parameterisation is exactly why
 * they are macros upstream and why the 1024/4096 pair cannot be flattened.
 */
#define BITFS(dev)	((dev) & 64)			/* h/param.h:108 */
#define	BSIZE(dev)	(BITFS(dev)? 4096: 1024)	/* :110 */
#define	INOPB(dev)	(BITFS(dev)? 64: 16)		/* :111 */
#define	BMASK(dev)	(BITFS(dev)? 07777: 01777)	/* :112 */
#define	BSHIFT(dev)	(BITFS(dev)? 12: 10)		/* :113 */
#define	NMASK(dev)	(BITFS(dev)? 01777: 0377)	/* :114 */
#define	NSHIFT(dev)	(BITFS(dev)? 10: 8)		/* :115 */
#define itod(dev, x)	((daddr_t)((((unsigned)(x)+2*INOPB(dev)-1)/INOPB(dev))))
#define itoo(dev, x)	((int)(((x)+2*INOPB(dev)-1)%INOPB(dev)))
#define fsbtodb(dev, b)	(BITFS(dev)? (b)*8: (b)*CLSIZE)	/* :120 */
#define dbtofsb(dev, b)	(BITFS(dev)? (b)/8: (b)/CLSIZE)	/* :121 */
#define NINDIR(dev)	(BSIZE(dev)/sizeof(daddr_t))	/* :122 */

#define	MIN(a,b) (((a)<(b))?(a):(b))	/* h/param.h:149 */
#define	MAX(a,b) (((a)>(b))?(a):(b))	/* h/param.h:150 */

/*
 * major/minor/makedev -- h/types.h:9,12,15.  They are in TYPES rather than in
 * param upstream, and they are here because this file is where the kernel side
 * spells its types (dev_t is narrowed a few lines below for the same reason).
 * A dev_t is `major<<8 | minor', 16 bits, and that packing is on disk in every
 * IFCHR and IFBLK inode -- so these three are on-disk arithmetic, not
 * convenience, and they must not be replaced by the host's <sys/types.h>
 * versions, which unpack Darwin's 32-bit dev_t at a different shift.
 */
#define	major(x)	((int)(((unsigned)(x)>>8)&0377))	/* h/types.h:9 */
#define	minor(x)	((int)((x)&0377))		/* h/types.h:12 */
#define	makedev(x,y)	((dev_t)(((x)<<8) | (y)))	/* h/types.h:15 */

/*
 * Sleep priorities.  h/param.h:30,31,34.  PZERO is the boundary: a sleep at a
 * priority ABOVE it is interruptible and longjmps out of the system call, one
 * at or below it is not.  shim/kern/sys/v8fs.c's sleep() is where that split
 * is implemented, and it is the whole reason PZERO is here rather than only
 * PINOD -- a sleep() that ignored the priority would turn every interruptible
 * inode wait into an uninterruptible one.
 */
#define	PSWP	0		/* h/param.h:30 */
#define	PINOD	10		/* h/param.h:31 */
#define	PZERO	25		/* h/param.h:34 -- the interruptible boundary */

/*
 * Page size.  h/param.h:65,67 -- upstream keeps these in param.h and NOT in
 * vmparam.h, and shim/kern/h/vmparam.h records that split rather than
 * duplicating them.  One number per layer; see CLAUDE.md's DIRSIZ account for
 * what happens when that slips.
 *
 * 512 is the VAX's page, not this host's (16384).  It is right anyway, because
 * the only two readers are arithmetic on quantities that are already in VAX
 * terms: bio.c:479 divides a buffer's byte count by NBPG to bump a paging
 * counter nothing reads, and vmmac.h's btop feeds vtopte, which panics.
 * Substituting the host's page size would make those two lines describe a
 * machine that is neither one.
 */
#define	NBBY	8		/* h/param.h:63 -- bits in a byte */
#define	NBPG	512		/* h/param.h:65 -- VAX page size */
#define	PGSHIFT	9		/* h/param.h:67 -- LOG2(NBPG) */

/*
 * CLSIZE -- the cluster, in pages.  h/param.h:86.
 *
 * It is 2 here for the reason the NICINOD/NICFREE block above gives at length:
 * src/include/sys/param.h:110 already commits this port to CLSIZE 2, and the
 * on-disk superblock is sized from it.  Spelled again on this side because the
 * kernel half cannot include that file (its daddr_t is `long'), and the two
 * are checked against each other by sizeof rather than by transcription.
 *
 * Its readers here are not the filesystem, though: bio.c:553 uses it through
 * shim/kern/h/vmparam.h's KLMAX, in a swap path that panics.  The filesystem
 * uses it only implicitly, through the CLSIZE==2 constants above.
 */
#define	CLSIZE	2		/* h/param.h:86 -- see NICFREE above */
#define	ctob(x)	((x)<<9)			/* h/param.h:163 */
#define	btoc(x)	((((unsigned)(x)+511)>>9))	/* h/param.h:166 */

/*
 * The signals the imported kernel source raises, and only those.
 *
 * Upstream's param.h says `#include <signal.h>' and takes all 31.  Including
 * the HOST's signal.h into 1985 K&R kernel code would drag sigaction, the
 * ucontext machinery and Darwin's own sigset_t through every kernel object, to
 * obtain a handful of integers.  So they are spelled, and shim/kern/sys/subr.c
 * -- which is modern C and can see both -- _Static_asserts that each equals
 * the host's.  That is the same two-ends discipline src/include/PORTING.md
 * states for a struct that v8cc and clang each read one end of.
 *
 * It was two, for streamio.c.  ttyld.c brought SIGINT and SIGQUIT -- the
 * interrupt and quit characters, ttyld.c:101,146,150 -- and nothing else:
 * `grep -oE SIG[A-Z]+' over it yields exactly those two plus the word SIGNAL
 * in a comment.  The numbers are V8's own, usr/include/signal.h:4-6 and :24,
 * and they are Darwin's too, which is what the asserts exist to keep true.
 *
 * KEEP THIS LIST MINIMAL RATHER THAN COMPLETE.  Spelling all 31 would end the
 * need to touch this block again and would also end the property that makes it
 * safe: every name here is one an imported file demonstrably raises, so the
 * assert list and the raise sites stay the same set.  A name added "for
 * completeness" is a claim nothing checks.
 */
#define	SIGHUP	1
#define	SIGINT	2
#define	SIGQUIT	3
#define	SIGPIPE	13

/*
 * §8a step 5 brought two more, and the sweep that found them matched a third
 * that is not a signal -- which is this block's own documented trap firing
 * again, in the paragraph that documents it.
 *
 * `grep -oE SIG[A-Z]+' over the six imported files yields SIGKILL, SIGXFSZ and
 * SIGNAL.  The third is alloc.c:141, an upstream comment in capitals --
 * "SHOULD RATHER SEND A SIGNAL AND SUSPEND THE PROCESS" -- and it is the
 * fourth time in this port that a sweep has counted prose as an instance.  The
 * filter that separates them is to grep the RAISE, not the name:
 *
 *	rdwri.c:166	psignal(u.u_procp, SIGXFSZ)	writei past u_limit
 *	bio.c:628	psignal(p, SIGKILL)		swkill, out of swap
 *
 * Both numbers are V8's own -- usr/include/signal.h:19 and :36, reached by
 * h/param.h:47-49's `#include <signal.h>' rather than defined in param.h
 * itself, which is where this port's own note had them.  shim/kern/sys/subr.c
 * asserts each against the host's, as it does for the four above.
 *
 * SIGKILL IS THE ONE TO BE CAREFUL WITH, because v8k_psignal delivers by
 * calling kill(2) on a real host pid.  bio.c's swkill() only runs when the
 * swap map is exhausted, and this port has no swapper, so the site is
 * unreachable -- but it is unreachable by topology rather than by the number
 * being wrong, and if it ever were reached it would kill a live process for
 * real.  shim/kern/sys/v8fs.c does not weaken it; bio.c's swap paths panic
 * before they get there.
 */
#define	SIGKILL	9	/* usr/include/signal.h:19 */
#define	SIGXFSZ	25	/* usr/include/signal.h:36 */

/*
 * Interrupt priority level.  See shim/kern/dev/machdep.c -- these are a nesting
 * counter here rather than a write to the VAX's IPL, and the counter is not
 * decoration: setqsched() consults it, so a qenable() inside a critical section
 * defers its queuerun() to the splx() that ends the section, exactly as the
 * software interrupt would have been held off by the level.
 */
int	spl6(void);
void	splx(int s);
int	spl0(void);			/* §8a step 5; machdep.c says why it is there */
void	kvprintf(const char *fmt, __builtin_va_list ap);

void	panic(const char *fmt, ...);

/*
 * KERNEL printf, AND IT MUST NOT BE THE PROGRAM'S.  stream.c calls printf() for
 * its diagnostics.  libv8c defines printf too, for the V8 program, and in a
 * link that has both the kernel's four diagnostics would go through the
 * program's stdio -- buffered with its output, and lost entirely if it has
 * redirected or closed stdout.  Redirecting the name here keeps stream.c
 * byte-identical and sends kernel messages to fd 2 where they belong.
 */
#define printf	v8k_printf
void	v8k_printf(const char *fmt, ...);

/*
 * KERNEL bcopy, for the same reason and a sharper one.  putq() coalesces a
 * small M_DATA block into the tail of the previous one with bcopy(), and
 * NEITHER libv8c NOR libv8sys defines bcopy -- so omitting it would not fail
 * the link.  It would resolve out of libSystem, work perfectly, and leave a
 * 1985 Bell Labs stream engine copying its messages with Apple's code.  That is
 * the class tests/kmemu sweeps the whole rootfs for; here it is closed before
 * it opened.  V7's argument order, bcopy(from, to, n).
 */
#define bcopy	v8k_bcopy
void	v8k_bcopy(const void *from, void *to, unsigned long n);

void	v8k_streaminit(void);	/* what the kernel's main() called qinit() for */

/*
 * The kernel services streamio.c expects, beyond the four above.
 *
 * shim/kern/sys/ has the definitions, in files named after the ones V8 kept
 * them in: slp.c (tsleep, wakeup), fio.c (ufalloc, closef, iput), subr.c (the
 * rest), ioconf.c (the configuration table).
 *
 * ONE SIGNATURE IS NOT UPSTREAM'S, AND IT USED TO SAY TWO BECAUSE THIS NOTE
 * WAS WRONG ABOUT min.
 *
 * It said min is "`min(a, b) unsigned a, b;' -- sys/rdwri.c:250, no declared
 * return type, so int(unsigned, unsigned)".  Read at the source, rdwri.c:249
 * is the word `unsigned' on a line of its own and :250 is `min(a, b)' -- the
 * return type IS declared, and it is unsigned.  There is exactly one min in
 * the whole kernel and no min macro in h/, so nothing else could have been
 * meant.  min and max are both `unsigned' here now, which is upstream's, and
 * the difference this paragraph existed to justify does not exist.
 *
 * Nothing observable changed, which is why it survived: `register n' in
 * streamio.c is an implicit int, and every call is bounded by a stream block
 * of at most 1024 bytes, so bit 31 is clear and int and unsigned have the same
 * bits and the same extension.  The half worth keeping is the warning it
 * carried -- "improve it to int(int, int)" is the wrong move, because
 * streamio.c calls it with u.u_count against a pointer difference.
 *
 * iomove takes `void *' where upstream takes caddr_t, and that is the one
 * concession to the type checker in this header.  streamio.c hands it
 * bp->rptr, a `u_char *', and caddr_t is `char *' -- so a faithful prototype
 * would emit -Wincompatible-pointer-types on authentic code and force either a
 * suppression that hides real mistakes or an edit to Bell Labs' source.  void*
 * accepts both, changes no ABI, and leaves one cast inside subr.c where it is
 * visible.  The alternative -- declaring it `()' and unchecked, as V8's own
 * headers did -- gives up the checking on every OTHER argument to buy nothing.
 */
int	tsleep(caddr_t chan, int pri, int seconds);
void	wakeup(caddr_t chan);
int	copyin(caddr_t from, caddr_t to, unsigned long n);
int	copyout(caddr_t from, caddr_t to, unsigned long n);
unsigned min(unsigned a, unsigned b);
unsigned max(unsigned a, unsigned b);
int	ufalloc(void);
void	gsignal(int pgrp, int sig);
void	selwakeup(struct proc *p, int coll);
void	closef(struct file *fp);
int	iput(struct inode *ip);

/*
 * THREE OF THE ABOVE CHANGED WHEN §8a step 5 LANDED, AND THE TWO THAT WENT
 * ARE MORE INTERESTING THAN THE ONE THAT STAYED.
 *
 * min, max, iomove, nulldev and iput were all OURS, written for streamio.c
 * because sys/rdwri.c and sys/iget.c were not in the tree.  Now they are, and
 * a shim spelling of a function whose authentic file is imported is a second
 * definition of it -- so all five were retired and the imports supply them.
 * shim/kern/sys/subr.c and fio.c record what each one cost.
 *
 * iomove's PROTOTYPE had to go too, not just the body.  It stood here as
 * `void iomove(void *cp, ...)' and upstream's rdwri.c:266 is
 * `iomove(cp, n, flag) register caddr_t cp;' -- implicit int return, char*
 * first argument.  Against a visible prototype that is a hard error twice
 * over, and neither half can move: the void* was deliberate (streamio.c hands
 * it a `u_char *', and a caddr_t prototype would emit
 * -Wincompatible-pointer-types on authentic code), and the definition is
 * Bell Labs'.  So the declaration is deleted rather than reconciled.  Nothing
 * modern C calls iomove -- measured, the only caller is streamio.c:241,459 --
 * and K&R reaches it implicitly, which is how it reached it in 1985.
 *
 * iput kept its declaration and changed its RETURN TYPE, `void' to `int',
 * because iget.c:176 is `iput(ip)' with no type on the line above it.  Nothing
 * reads the value; the type still has to match or the definition and the
 * declaration disagree.
 */

/*
 * KERNEL psignal, AND LIBC HAS TAKEN THE NAME.
 *
 * V8's psignal(p, sig) posts a signal to a process.  Darwin's <signal.h>:106
 * declares `void psignal(int, const char *)' -- BSD's psignal(3), which PRINTS
 * a message for a signal number, an entirely different function that arrived
 * long after 1985.  Two functions, one name, incompatible signatures.
 *
 * So it joins printf, bcopy and longjmp on the redirect list, for the sharpest
 * of the four reasons: those three would have worked while being the wrong
 * code, and this one does not even typecheck.  The redirect keeps streamio.c's
 * `psignal(u.u_procp, SIGPIPE)' spelled as Bell Labs wrote it.  gsignal is NOT
 * redirected -- Darwin has no gsignal(3), measured with a dlsym probe over
 * every file-scope name in the archive, so a redirect would be superstition.
 *
 * THAT SWEEP FOUND ONE MORE COLLISION AND IT IS DELIBERATELY LEFT ALONE.
 * `panic' is exported by /usr/lib/system/libsystem_kernel.dylib, and stream.c
 * and streamio.c both call it.  The distinction that decides it is DEFINING
 * versus IMPORTING: psignal would have been taken FROM libc, with the wrong
 * signature, so the name had to move.  panic is ours -- dev/machdep.c defines
 * it, every caller in the archive means the kernel's, and the archive member
 * wins the link.  Nothing is silently taken from the host, which is the
 * property the redirect list exists to protect.  What is true is that
 * libv8kern shadows a libSystem symbol for the whole link; recorded because
 * tests/kmemu's nm -u sweep cannot see it -- _panic is DEFINED here, not
 * imported, and that sweep only watches imports.
 */
#define psignal	v8k_psignal
void	v8k_psignal(struct proc *p, int sig);

/*
 * KERNEL longjmp, and it takes ONE argument.
 *
 * streamio.c says `longjmp(u.u_qsav)' at :231, :437 and :978 -- the V8 kernel's
 * longjmp restores a label_t and returns 1 into the matching setjmp, with no
 * value argument.  libc's takes two.  Redirecting the name is the same move
 * param.h already makes for printf and bcopy: the authentic source keeps its
 * spelling and ours is the one that moves.
 *
 * A .c file in shim/kern that wants the REAL longjmp must #undef it first, as
 * dev/machdep.c does for printf.  slp.c does exactly that, because it is the
 * file that owns both.
 */
#define longjmp	v8k_longjmp
void	v8k_longjmp(jmp_buf env);

/*
 * Driver descriptors, which is what makes tsleep able to wait.
 *
 * PLAN.md section 8a step 2 answered "what is at the bottom of a stack" for
 * filesystems with "the host", and this is the same answer one level down: a
 * V8 stream's driver end is a host descriptor, so tsleep() is queuerun() and
 * then poll() on the descriptors standing in for the devices that would have
 * interrupted.  A driver registers here when it opens and withdraws when it
 * closes.  shim/kern/sys/slp.c has the account, including why what is
 * registered is a descriptor AND a handler rather than a descriptor alone.
 */
int	v8k_drvfd(int fd, void (*isr)(int, void *), void *arg);
int	v8k_drvclose(int fd);	/* withdraw */
int	v8k_ndrvfd(void);	/* how many are registered */

/*
 * The setjmp half of streamio.c's three `longjmp(u.u_qsav)' calls, which
 * upstream keeps in sys/trap.c:176 -- the system-call dispatcher.  Runs fn,
 * returns u.u_error, and turns an interrupted sleep into EINTR.
 */
int	v8k_stcall(void (*fn)(void *), void *arg);

/*
 * THE STREAM SYSTEM CALLS, DECLARED -- and upstream declares not one of them.
 *
 * In 1985 an undeclared function returned int and that was right for six of
 * these seven.  It is not right for stopen, which returns `struct inode *'
 * (src/sys/sys/streamio.c:33), and on LP64 a caller that writes `int stopen()'
 * -- or simply calls it with no declaration in scope, which is what
 * -Wno-implicit-function-declaration permits throughout this build -- takes
 * the top half off the returned pointer with no diagnostic at all.  That is
 * hazard 1's classic shape, in a file that does not exist yet: the shim's
 * stream integration, and any driver written against it.
 *
 * They are HERE rather than in a header of their own because param.h is
 * streamio.c's first include, so these prototypes are checked against the
 * definitions every time the file is compiled.  A separate header nobody
 * includes would record the types without ever verifying them.
 *
 * The six int returns are written out rather than left implicit, so that a
 * definition drifting to `void' is a build failure instead of a caller reading
 * whatever was in x0.
 */
struct stdata;
struct streamtab;
struct inode *stopen(struct streamtab *qinfo, int dev, int flag,
	    struct inode *ip);
int	stclose(struct inode *ip, int sleepOK);
int	stread(struct inode *ip);
int	stwrite(struct inode *ip);
int	stioctl(struct inode *ip, int cmd, caddr_t arg);
int	istread(struct inode *ip, caddr_t addr, int count);
int	istwrite(struct inode *ip, caddr_t addr, int count);
int	stselect(struct stdata *stp, int rw, int anyyet);
struct stdata *stenter(struct inode *ip);
int	stexit(struct inode *ip);

/*
 * Stream configuration -- what V8's config(8) wrote into ioconf.c.
 *
 * streamio.c reads `streamtab[]' and `nstream' to resolve a line-discipline
 * number for FIOPUSHLD / FIOPOPLD / FIOLOOKLD.  Upstream those are generated
 * at build time from the machine's configuration file; here they are filled in
 * at run time, because what disciplines exist is a property of the shim rather
 * than of a VAX's peripheral list.  shim/kern/sys/ioconf.c says why the table
 * must stay CONTIGUOUS.
 */
int	v8k_stconf(struct streamtab *st);	/* -> ld number, or -1 */
void	v8k_stunconf(void);	/* forget them all; for tests */

/*
 * THERE IS NO UNIBUS AND NO DMA, so qinit()'s uballoc() has nothing to map.
 *
 * A macro rather than a three-line deletion, because the deletion buys nothing
 * and costs the strongest claim available: stream.c's blob hash still matches
 * PROVENANCE, so `git hash-object' says it is upstream's file rather than a
 * PORTING.md saying it nearly is.
 *
 * Nothing observes the value.  blkubad is read in exactly two places in the
 * whole V8 kernel -- dev/ill.c (the Interlan Ethernet board) and dev/kdi.c
 * (Datakit) -- both converting a block address into a bus address for a device
 * that is going to DMA out of it.  Neither board is here.  Returning a non-zero
 * token satisfies qinit()'s check and is read by nobody; if a driver ever wants
 * a real bus address this stops compiling, which is the right way to find out.
 */
#define uballoc(uban, addr, size, flags)	((long)1)

/*
 * §8a step 5: SEVEN NAMES THE KERNEL AND THE C LIBRARY BOTH DEFINE.
 *
 * libv8kern is linked into V8 programs alongside libv8c and libv8stubs, so a
 * kernel function and a library function of the same name are two definitions
 * in one link.  These get the psignal treatment above -- a macro, renaming both
 * the definition and every call uniformly, so Bell Labs' source keeps its
 * spelling and the archive exports a name nothing else claims.
 *
 * THE LIST IS SEVEN AND WAS COSTED AS FOUR, and the way to find them is the
 * point.  `nm -u' -- the sweep tests/kmemu runs over the whole rootfs -- sees
 * what an object IMPORTS, and a collision is about what it DEFINES.  So the
 * measurement is `nm -g' on the archives, filtered to T and D:
 *
 *	free	libv8c.a(malloc.o)	alloc.c:156 defines free(dev, bno)
 *	ialloc	libv8c.a(malloc.o)	alloc.c:232 defines ialloc(dev)
 *	min	libv8c.a(min.o)		rdwri.c:250
 *	max	libv8c.a(max.o)		rdwri.c:236
 *	sleep	libv8c.a(sleep.o)	shim/kern/sys/v8fs.c
 *	access	libv8stubs.a(access.o)	shim/kern/sys/v8fs.c
 *	time	libv8stubs.a(time.o)	src/sys/h/systm.h:12
 *	timezone libv8c.a(timezone.o)	src/sys/h/systm.h:7
 *	mount	libv8stubs.a(mount.o)	src/sys/h/mount.h:21
 *
 * -- NINE, and the count went four, then seven, then eight, then nine as the
 * INSTRUMENT improved rather than as the code changed.  Four was the survey's.
 * Seven came from `nm -g' over the archives.  The eighth and ninth came from
 * tests/kmemu's cross-archive sweep, once it was extended to a fourth archive
 * -- libv8stubs.a, which it had never opened, and which holds the syscall
 * stubs, i.e. exactly the names a kernel is most likely to also define.
 *
 * SIX ARE FUNCTION-AGAINST-FUNCTION.  THREE ARE A VARIABLE AGAINST A FUNCTION,
 * AND THAT SPLIT IS THE WHOLE REASON THE COUNT KEPT MOVING.  A duplicate
 * function is a duplicate-symbol error: the linker refuses and you find it the
 * first time you link.  A COMMON symbol resolving against a text definition is
 * what a linker is SUPPOSED to do, so it happens in silence -- which means the
 * variable-versus-function class is invisible to the build and visible only to
 * `nm -g'.  Both of the ones nobody predicted are in that class.
 *
 * The first is `time'.  The
 * kernel's `time' is systm.h:12, `time_t time;' -- a K&R tentative definition,
 * so a COMMON symbol.  libv8stubs' is the time(2) stub, a text symbol.  A
 * linker resolves a common against a definition by taking the definition, so
 * the kernel's clock variable would quietly become the ADDRESS OF time() --
 * and `dp->di_ctime = time' (iget.c:276) would write a code address into an
 * inode as a timestamp.  No diagnostic, no crash, wrong dates on disk.
 *
 * The third is `mount', found last and the plainest of the three: the kernel's
 * is `struct mount mount[NMOUNT]' (mount.h:21, a tentative definition, so a
 * common in alloc.o, iget.o and nami.o) and libv8stubs' is the mount(2)
 * SYSCALL STUB.  A mount table against the call that fills one -- related in
 * meaning, unrelated as objects, and the common would have resolved to the
 * stub's entry point.  findmount() would then have walked the text segment.
 *
 * The second is `timezone', and it is the same shape with the roles further
 * apart: systm.h:7 is `int timezone;', minutes west of Greenwich, while
 * src/libc/gen/timezone.c:24 is `char *timezone(zone, dst)' -- a FUNCTION
 * returning the local zone's NAME.  Not two spellings of one idea, two
 * unrelated things, exactly like the kernel's psignal against BSD's
 * psignal(3).  Nothing in the six reads the variable, so today the only
 * casualty would be a future reader getting a code address; the rename costs
 * one line and removes the question.
 *
 * That is why the rename is applied to `time' and `timezone' even though
 * nothing forced either: the other six fail loudly at link time, and these two
 * do not fail at all.
 *
 * free and ialloc are the two whose DEFINITIONS are in authentic source, which
 * is the case the macro form exists for -- alloc.c includes this header, so
 * `free(dev, bno)' at :156 is compiled as v8k_free and Bell Labs' file is
 * untouched.  Its blob hash still matches PROVENANCE.
 */
#define	free	v8k_free
#define	ialloc	v8k_ialloc
#define	min	v8k_min
#define	max	v8k_max
#define	sleep	v8k_sleep
#define	access	v8k_access
#define	time	v8k_time
#define	timezone v8k_timezone
#define	mount	v8k_mount

/*
 * §8a step 5 services.  Prototypes for the ones modern C in this directory
 * calls or defines; the K&R half reaches them through src/sys/h/systm.h,
 * mount.h, cmap.h and inode.h, which are AUTHENTIC and already declare
 * min, max, findmount, mfind, iget and ialloc.  Nothing here respells one of
 * those -- see shim/kern/sys/v8fs.c, which says which declaration each
 * definition answers to.
 */
int	fubyte(caddr_t addr);
int	fuibyte(caddr_t addr);
int	subyte(caddr_t addr, int c);
int	suibyte(caddr_t addr, int c);
int	fustrlen(caddr_t addr);
int	spl0(void);
void	sleep(caddr_t chan, int pri);
void	plock(struct inode *ip);
void	tablefull(char *tab);
void	uprintf(const char *fmt, ...);
void	xrele(struct inode *ip);
int	suser(void);

#endif /* V8KERN_PARAM_H */
