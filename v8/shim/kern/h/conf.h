/*
 * conf.h -- the kernel's device and stream configuration, reduced to the part
 * that has meaning here.  Ours, not Bell Labs'; see shim/kern/h/param.h.
 *
 * V8's is 76 lines and is mostly `struct cdevsw', `struct bdevsw' and
 * `struct fstypsw' -- the driver switch tables, one row per device on the
 * machine.  stream.c includes this header and, measured, references nothing
 * from it: it is included because every file in dev/ includes it.  So the
 * temptation is an empty file.
 *
 * `struct streamtab' is here instead, because it is the one thing in conf.h
 * that is about streams rather than about VAX peripherals, and it is the type a
 * stream MODULE must define to exist -- the pair of qinits, read side and write
 * side, that stopen() pushes.  streamio.c will want it.  Spelled exactly as
 * upstream spells it, so that importing V8's conf.h later is a substitution
 * rather than a merge.
 *
 * The switch tables are deliberately absent.  There is no cdevsw here because
 * there are no character devices here: the shim answers open(2) itself, and a
 * table of major numbers pointing at drivers for a DZ11 and an RP07 would be a
 * description of furniture this room does not have.
 */

#ifndef V8KERN_CONF_H
#define V8KERN_CONF_H

/*
 * stream processor table
 */
extern	struct streamtab {
	struct	qinit	*rdinit;
	struct	qinit	*wrinit;
} *streamtab[];

/*
 * §8a step 5: THE SWITCH TABLES ARRIVE, and the paragraph above saying they
 * are "deliberately absent" was right when it was written and is now half
 * wrong.  It argued that a cdevsw full of DZ11 and RP07 rows would be "a
 * description of furniture this room does not have", which still holds for the
 * ROWS.  What it did not anticipate is that the imported filesystem code
 * INDEXES these tables, so the types have to exist even where the tables are
 * empty:
 *
 *	iget.c, rdwri.c, nami.c	fstypsw[fstyp].t_*	13 sites
 *	rdwri.c:62,159		cdevsw[major(dev)].d_read/d_write
 *	bio.c			bdevsw[major(dev)].d_strategy, .d_flags
 *
 * The structs are spelled EXACTLY as upstream spells them, member for member
 * and in order, including the members none of the six touches -- t_stat,
 * t_mount, t_ioctl, d_open, d_close, d_reset, d_dump.  That is this file's
 * existing policy for struct streamtab and it is the opposite of the rule
 * param.h states for its signal list, deliberately: a signal number is a VALUE
 * that must be checked against the host, so a name added for completeness is
 * an unchecked claim; a switch table is a LAYOUT that nothing outside this
 * port reads, and matching upstream is what makes importing V8's real conf.h
 * later a substitution rather than a merge.
 *
 * ------------------------------------------------------------------------
 * WHERE THE FILESYSTEM TABLE COMES FROM, AND WHY dev/conf.c IS NOT IT.
 *
 * The obvious source is upstream's dev/conf.c:602-611, which defines
 * `struct fstypsw fstypsw[]' with four rows and `int nfstyp = 4'.  Row 0
 * there is
 *
 *	{ 0, 0, 0, 0, 0, 0, 0, 0, rnami, smount, 0}
 *
 * -- and rnami IS NOT DEFINED ANYWHERE IN THE V8 KERNEL.  Measured: the only
 * three occurrences of the name in the whole tree are that row, the
 * `extern int rnami()' above it at :592, and a COMMENT in sys/nami.c:167
 * reading "USED TO BE rnami" immediately above the definition of fsnami.
 *
 * Bell Labs say why, in their own words, at conf/config_diff:11 --
 *
 *	dev/conf.c is no more.  config makes a conf.c for each machine.
 *
 * -- and :13-14 lists the files "changed a little to make names regular"
 * when that happened.  nami.c is on the list.  So dev/conf.c is a VESTIGIAL
 * FILE that predates the rename, and every citation to it is a citation to
 * dead code.  This port has met that exact trap before: CLAUDE.md records
 * V7's syopen driver still sitting in sys/sys/sys.c, dead and uncompilable,
 * and calls a vestigial file that answers your question the worst kind of
 * evidence.  dev/param.c is a third instance, stale against sys/param.c.
 *
 * THE LIVE SOURCE IS conf/devices, which config_diff:20-21 names as the input
 * config(8) reads, and which this port already cites for the tty line
 * discipline (conf/devices:75) and for /dev/tty (:55).  Its lines 70-73 are
 * the filesystem handlers, and the columns are number, prefix, prefix, members
 * present:
 *
 *	file-system 0	fs  fs  nami mount
 *	file-system 1	na  na  put get free updat read write trunc stat nami mount
 *	file-system 2	pr  pr  ... ioctl
 *	file-system 3	mp  mp  ... ioctl
 *
 * -- so type 0's prefix is `fs', its two members are fsnami and fsmount, and
 * THAT is the name src/sys/sys/nami.c:202 actually defines.  The generated
 * conf.c would have said fsnami where the vestigial one says rnami.
 *
 * NFSTYP IS 1 HERE, NOT 4, and the three that go are named rather than
 * dropped: 1 is `na', sys/neta.c, the network-attach filesystem; 2 is `pr',
 * sys/proca.c, Killian's process filesystem; 3 is `mp', sys/mp.c, multiplexed
 * files.  None is imported.  Keeping four rows with null t_nami would turn
 * nami.c:78's `if(p.dp->i_fstyp >= nfstyp) panic("namei nfstyp")' -- a guard
 * that catches a corrupt i_fstyp -- into a null call one line later at :80,
 * which is the same guard reporting success and then faulting.  One row means
 * the panic fires, with V8's own message.
 *
 * Note what row 0 is NOT: t_get, t_put, t_read and the rest are all null, and
 * every one of the 13 call sites guards on `ip->i_fstyp &&' or on the pointer
 * being non-null before dispatching.  t_nami at :80 is the ONE site with
 * neither guard, which is why it is the one slot that must be filled.
 */
struct fstypsw {
	int		(*t_put)();	/* h/conf.h:41 */
	struct inode	*(*t_get)();	/* :42 */
	int		(*t_free)();	/* :43 */
	int		(*t_updat)();	/* :44 */
	int		(*t_read)();	/* :45 */
	int		(*t_write)();	/* :46 */
	int		(*t_trunc)();	/* :47 */
	int		(*t_stat)();	/* :48 */
	int		(*t_nami)();	/* :49 -- the only one row 0 fills */
	int		(*t_mount)();	/* :50 */
	int		(*t_ioctl)();	/* :51 */
};
extern struct fstypsw fstypsw[];	/* h/conf.h:54 */
extern int nfstyp;			/* h/conf.h:55 is `extern nfstyp;' */

/*
 * The device switches.  Both tables are EMPTY here and both `n' counts are
 * zero, which is not a placeholder -- it is the accurate description of a
 * machine with no block devices and no character drivers, where the shim
 * answers open(2) itself.  What that buys is that the two range checks in the
 * imported source do their job: bio.c:352's `if (major(dev) >= nblkdev)'
 * rejects every block device rather than indexing an empty array, and
 * rdwri.c's cdevsw dispatch is reached only for an IFCHR inode, which this
 * port's v8fs image does not contain.
 *
 * A zero-length array is not C89, so each gets one all-null row and the count
 * stays 0.  The row exists to give the array a size; nothing may index it,
 * and the counts are what say so.
 */
struct cdevsw {
	int	(*d_open)();		/* h/conf.h:27 */
	int	(*d_close)();		/* :28 */
	int	(*d_read)();		/* :29 */
	int	(*d_write)();		/* :30 */
	int	(*d_ioctl)();		/* :31 */
	int	(*d_reset)();		/* :32 */
	struct	streamtab *qinfo;	/* :33 */
};
extern struct cdevsw cdevsw[];		/* h/conf.h:36 */
extern int nchrdev;

struct bdevsw {
	int	(*d_open)();		/* h/conf.h:11 */
	int	(*d_close)();		/* :12 */
	int	(*d_strategy)();	/* :13 */
	int	(*d_dump)();		/* :14 */
	int	d_flags;		/* :15 */
};
extern struct bdevsw bdevsw[];		/* h/conf.h:19 */
extern int nblkdev;			/* h/systm.h:26 */

#endif /* V8KERN_CONF_H */
