/*
 * v8fsd -- the v8fs file server.  PLAN.md §8a step 5e.
 *
 * Bell Labs' filesystem code, in a process of its own, answering 9P2000 over a
 * Unix-domain socket.  A V8 program opens a name under the mount, the shim
 * connects, and namei/iget/bmap/readi run over a real disk image.
 *
 * WHY IT IS A SERVER AND NOT A FOURTH TYPE IN vfs.c.  §8a step 5e was costed
 * as exactly that and it cannot be built; shim/kern/NOTES.md has both
 * measurements and either one alone is fatal.
 *
 *   SYMBOLS.  Linking libv8kern beside a V8 program is 56 collisions over 29
 *   programs on the 1985 vocabulary -- buf, bread, alloc, bmap, tty, file,
 *   panic -- and 25 of them are silent, because a K&R tentative definition is
 *   a COMMON and a linker is supposed to resolve it against a real one.
 *   `cat's `char buf[4096]' becomes an eight-byte pointer; the natural link
 *   exits 0 with byte-identical output, having written 4088 bytes over the
 *   buffer cache's own pointers.
 *
 *   EXEC.  vfs.c:167 had already recorded it: a v8fs descriptor would be an
 *   inode pointer and an offset in process memory, and process memory does not
 *   survive a program replacing itself.  `cat /mnt/a > /mnt/b' could never
 *   work, because the shell opens the file and then execs cat.
 *
 * Out here neither arises.  This is an ordinary host binary -- the same
 * relationship tests/streams/fsprobe.c already has to libv8kern, which is what
 * says the archive links cleanly in a program that is not a V8 one -- and a
 * connected socket is a descriptor the kernel itself carries across exec.
 *
 * ONE PROCESS, poll(2), NO THREADS, and that is forced twice over.  The
 * buffer cache must have exactly one authority or two writers corrupt an
 * image; and V8's kernel keeps its per-call state in a GLOBAL u-area, so it
 * cannot be re-entered at all.  What makes single-threading sufficient rather
 * than merely necessary is that nothing in the path sleeps: the image driver
 * is synchronous, so iowait() at bio.c:426 finds B_DONE already set.  Each
 * request is carried to completion between two poll() returns.
 *
 * WRITABLE SINCE §8a STEP 5f, AND "READ ONLY" MEANS SOMETHING DIFFERENT NOW.
 * Until 5f this file answered Rerror to Twrite, Tcreate, Tremove and Twstat --
 * a boundary in the PROTOCOL, with a filesystem underneath it that was never
 * read-only at all.  readi sets IACC (rdwri.c:50), so iput ran IUPDAT and
 * dirtied the disk inode on every read; the only reason nothing reached the
 * image was that nothing called bflush().
 *
 * THREE THINGS CHANGED AND EACH ONE IS BELL LABS' OWN ANSWER TO A QUESTION 5f
 * had to ask:
 *
 *   -r IS A MOUNT FLAG, NOT A DISPATCH ARM.  fsmount() at sys/sys3.c:299-316
 *   opens the device `!ronly' and stores `ronly & 1' in the superblock, and
 *   v8k_kinit now takes the same argument.  With it set, iupdat returns at
 *   iget.c:248 before it breads anything and access() refuses IWRITE through
 *   the arm §8a step 5d restored -- so not even an atime moves.  That is a
 *   guarantee the EROFS arm never gave.
 *
 *   THE CLOCK ADVANCES.  `time' was set once, by iinit from the superblock's
 *   s_time, and nothing moved it -- upstream's clock interrupt is in
 *   sys/clock.c, about a VAX interval timer, and is not imported.  So every
 *   stamp a write laid down would have been the moment mkfs wrote the image.
 *   v8fs_clock() is the substitution's other half and serve1 calls it once per
 *   request, which is what a clock interrupt would have done for free.
 *
 *   SOMETHING FLUSHES.  iupdat and writei mostly bdwrite, which marks the
 *   buffer B_DELWRI and returns; the write reaches the disk when the cache
 *   recycles the buffer, which on an image this size is never.  Measured:
 *   O_RDWR with no flush produced ZERO pwrites across twenty-two reads.
 *   update() -- alloc.c:486, the body of sync(2) -- is what upstream's
 *   sched() runs every thirty seconds, and the poll loop runs it once per
 *   wakeup.
 *
 * SPLITTING IT the same way steps 5c and 5d were split keeps the unit
 * reviewable and keeps a failure attributable, so 5f deliberately left the
 * path-taking syscalls refusing with MOUNTED() -- a lie about a writable
 * filesystem, left loud so that the follow-on step had to remove it one call
 * at a time.
 *
 * §8a step 5f-b IS THAT STEP, and only link and symlink still refuse: they
 * have no 9P2000 message at all, and a V7 image holds no symlink to read back
 * anyway.  access, unlink, mkdir, rmdir and mknod's directory arm became slots
 * in 5f; chmod, chown and utime in 5f-b, as one Twstat between them.
 *
 * (This paragraph named "the TEN path-taking syscalls" and then listed NINE --
 * link, mknod, unlink, chmod, chown, mkdir, rmdir, symlink, utime -- because
 * link guards both of its names and the count was of MOUNTED() calls while the
 * sentence was of names.  Counting one thing while describing another is a
 * shape this repo has recorded more than once, and syscall.c's own enumeration
 * of the same set had made the same slip in the same direction.)
 */

#include "../kern/h/param.h"

/*
 * param.h's redirects are undone in ONE place; hostok.h says why.  The
 * consequence here is the same one fsprobe.c records: `mount' is the host's
 * after this, so the kernel's mount table would have to be spelled v8k_mount,
 * and `free', `time' and `access' belong to libc in this translation unit.
 * Nothing below calls the kernel's versions of those.
 */
#include "../kern/h/hostok.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "../kern/h/proc.h"
#include "../../src/sys/h/dir.h"	/* struct direct, for user.h's u_dent */
#include "../kern/h/user.h"
#include "../../src/sys/h/inode.h"
#include "../../src/sys/h/mount.h"
#include "../../src/sys/h/buf.h"
#include "../kern/h/conf.h"
#include "../kern/h/filsys.h"
/*
 * struct dinode -- OURS by the same rule as filsys.h beside it, because it is
 * a DISK RECORD and §8a step 4a narrowed four of its fields to v8_i32 so the
 * image agrees with 1985.  Needed here because the three timestamps a stat
 * reports live only on disk; the in-core inode has no room for them.
 */
#include "../kern/h/ino.h"
#include "../kern/dev/imgdev.h"
#include "../p9/p9.h"

/*
 * -DKERNEL IS NOT OPTIONAL, for fsprobe.c's reason exactly: inode.h:62-68 and
 * buf.h:78-79 declare namei, iget and bread -- all POINTER-RETURNING -- inside
 * `#ifdef KERNEL', and the kernel flags carry
 * -Wno-implicit-function-declaration for the imported half's sake.  Without
 * the flag each call would be an implicit `int' function and the returned
 * pointer would be TRUNCATED to 32 bits.  That is the ps -T bug this port
 * already diagnosed once, where a "wild pointer 0x53c5c" was the low half of
 * 0x100053c5c.
 */
#ifndef KERNEL
#error v8fsd.c must be compiled -DKERNEL; see the comment above
#endif

/*
 * K&R declarations, matching the authentic headers where they make any.
 *
 * v8k_access IS BELL LABS' access(), reached by the name param.h renames it
 * to, and spelling it out is not decoration.  hostok.h above #undefs that
 * redirect so this file can also have <unistd.h> -- which means a plain
 * `access(ip, IREAD)' here binds to LIBC's access(const char *, int) and
 * compiles, because K&R.  fsprobe.c records the identical trap for `free' and
 * `ialloc'; this is the first time it has actually been walked into.
 *
 * §8a step 5f ADDS writei, itrunc AND iupdat AND NO MORE THAN THAT, because a
 * declaration with no call site is an unconsumed component standing in for a
 * trap instead of defending against one -- fsprobe.c had to delete two of
 * exactly that shape.  In particular the create and delete paths go through
 * namei with NI_CREAT and NI_DEL, so ialloc, ifree and free are reached only
 * by Bell Labs' own code and are not named here.  Which is just as well for
 * `free': hostok.h has given that name back to the C library in this file, so
 * `free(dev, bno)' would compile -- K&R, one argument too many -- and hand a
 * device number to malloc's partner.
 *
 * AND `time' IS libc's time(2) HERE, NOT THE KERNEL'S CLOCK, which is why the
 * clock is advanced by calling v8fs_clock() rather than by an assignment in
 * this file.  The same #undef that makes access() a trap makes the kernel's
 * most-read global unreachable by its own name.
 */
int		readi(), writei(), iput(), schar(), itrunc(), iupdat();
int		update(), brelse();
/*
 * AND THE PARAGRAPH ABOVE WAS TRUE OF ITSELF AND FALSE OF THE LINE UNDER IT.
 * "A declaration with no call site is an unconsumed component" was written
 * about writei/itrunc/iupdat, correctly, directly above a line declaring
 * iinit, binit, bhinit and ihinit -- none of which this file calls.  v8k_kinit
 * calls all four; the server reaches them only through it.  An auditor found
 * it, and the rule was being stated and broken in adjacent lines.
 *
 * THE ONE THAT MATTERED WAS iinit, because 5f CHANGED IT.  main.c:304 is
 * `void iinit(int ronly)' -- the mount flag, which is what -r is -- and the
 * declaration here still said `int iinit()'.  A K&R declaration that
 * contradicts the definition is exactly what this port keeps finding, and it
 * was harmless only because nothing called it.  Deleted rather than corrected:
 * the fix for an unconsumed declaration is not a better declaration.
 *
 * update() and brelse() STAY, and they are the reason the line has to be split
 * rather than removed -- the poll loop calls update() after every message and
 * itimes() calls brelse().  Four of the six were dead and two were live, on
 * one line, which is how the dead four kept their cover.
 */
int		v8k_access();
int		v8k_kinit(dev_t dev, int ronly);
extern void	v8fs_clock(void);		/* shim/kern/sys/v8fs.c */

/*
 * rootdev is systm.h's -- a K&R tentative definition, so every object that
 * includes that header emits a common and the linker merges them.  Declared
 * extern here rather than including systm.h, which would bring in the three
 * warnings CLAUDE.md records for it (a caddr_t calloc() against the builtin,
 * and two tentative arrays) for the sake of one dev_t.
 */
extern dev_t	rootdev;

/*
 * THE KERNEL'S CLOCK, BY ITS RENAMED NAME, and the rename is the only reason
 * this declaration is here rather than being had from systm.h with the rest.
 * param.h maps `time' to v8k_time so the variable does not collide with
 * libv8stubs' time(2); hostok.h #undefs that so this file can have <unistd.h>.
 * Between them, the kernel's most-read global has no unqualified spelling in
 * this translation unit -- so it is named once, here, and kmkdir below is the
 * one place that needs its address.
 */
extern time_t	v8k_time;

/*
 * P9_NAMELEN must exceed the longest name the image can hold, and p9.h says
 * the assertion belongs where DIRSIZ is in scope.  It is.
 */
_Static_assert(P9_NAMELEN > DIRSIZ + 1, "P9_NAMELEN cannot hold a V8 name");

/* --------------------------------------------------------------- errors */

/*
 * 9P2000 CARRIES A STRING, NOT AN ERRNO, and that is the one place plain
 * 9P2000 costs this port something.  The .u extension adds a numeric errno and
 * PLAN.md §8a rules it out for a good reason (its Unix extensions carry things
 * V8's userspace does not have), so the number has to travel inside the text.
 *
 * The string is the SYMBOLIC NAME -- "ENOENT", not "No such file or
 * directory".  One token, no parsing ambiguity, exactly reversible by the
 * client, and still the thing a systems programmer reads when a foreign 9P
 * client (9pfuse, plan9port's 9p) prints it.  strerror()'s text would be
 * prose, which differs between C libraries and cannot be mapped back.
 *
 * The cost is stated rather than hidden: a FOREIGN SERVER talking to our
 * client sends its own prose and the client cannot map it, so it falls back to
 * EIO.  That is the right direction to fail -- an unrecognised error is still
 * an error -- and it is a limit of 9P2000 rather than of this table.
 */
static const struct { int e; const char *name; } errnames[] = {
	{ EPERM, "EPERM" },	{ ENOENT, "ENOENT" },	{ EIO, "EIO" },
	{ ENXIO, "ENXIO" },	{ EBADF, "EBADF" },	{ EACCES, "EACCES" },
	{ EEXIST, "EEXIST" },	{ ENOTDIR, "ENOTDIR" },	{ EISDIR, "EISDIR" },
	{ EINVAL, "EINVAL" },	{ ENFILE, "ENFILE" },	{ EMFILE, "EMFILE" },
	{ EFBIG, "EFBIG" },	{ ENOSPC, "ENOSPC" },	{ EROFS, "EROFS" },
	{ EMLINK, "EMLINK" },	{ ENOTEMPTY, "ENOTEMPTY" },
	{ ENAMETOOLONG, "ENAMETOOLONG" },
	/*
	 * ENOMEM is the server's OWN error -- gendir sets it when a listing
	 * will not fit -- and it was missing, so it reached the client as
	 * "EIO" through the fallback below.  That fallback is documented for a
	 * FOREIGN server's prose; letting our own errno take it means the one
	 * case where the string is exactly reversible was not.
	 */
	{ ENOMEM, "ENOMEM" },
	{ 0, 0 }
};

static const char *
ename(int e)
{
	int i;

	for (i = 0; errnames[i].name; i++)
		if (errnames[i].e == e) return (errnames[i].name);
	return ("EIO");
}

/* ---------------------------------------------------------- connections */

/*
 * FIDS ARE PER CONNECTION and there are very few of them.  The client's
 * arrangement is one connection per open file, so a conversation is
 * Tversion, Tattach, Twalk, Topen and then reads -- three fids at the high
 * water mark.  Sixteen is room for a client that walks in stages, and a
 * linear scan over sixteen is cheaper than a hash on a number the client
 * chooses.
 */
#define NFID		16
#define NCONN		64

struct fid {
	p9_u32		 f_fid;		/* P9_NOFID when the slot is free */
	struct inode	*f_ip;
	int		 f_omode;	/* -1 until Topen; else the 9P mode */
	char		 f_name[P9_NAMELEN];
	unsigned char	*f_dir;		/* generated directory listing */
	long		 f_dirlen;
	/*
	 * THE CURSOR, AND IT IS THE OPEN FILE DESCRIPTION'S OFFSET -- p9.h's
	 * extension note has the argument.  It is here rather than in the
	 * client because a dup, a fork and a program replacing itself all
	 * share one offset and share this connection, and share nothing in
	 * client memory.
	 *
	 * Only P9_OFFCUR reads or writes it.  An explicit offset is 9P's own
	 * pread and leaves it alone, which is what lets a conforming client
	 * use this server without discovering the extension exists.
	 */
	p9_u64		 f_off;

	/*
	 * THE PARENT, AS A NUMBER, AND Tremove IS WHY -- §8a step 5f.
	 *
	 * 9P's Tremove carries a fid and nothing else; V7's unlink takes a
	 * DIRECTORY and a NAME, because what it removes is an entry rather than
	 * a file.  Nothing in a struct inode can bridge that: `..' is a
	 * directory entry, so it exists for a directory and not for a plain
	 * file, and there is no parent pointer in core.
	 *
	 * A FIRST VERSION USED THE ROOT for a plain file and it was wrong the
	 * moment anything lived one level down -- `rm /mnt/newdir/f' asked the
	 * server to unlink `f' from the ROOT, found nothing there, and reported
	 * a failure the shell's rm swallowed.  Found by running it, not by
	 * reading it: the walk and the remove are in different functions and
	 * each looks right alone.
	 *
	 * A NUMBER RATHER THAN A POINTER, so that a fid holds no reference it
	 * would have to release: iget() answers from its own hash for a
	 * directory the client has just walked through, and the alternative is
	 * an i_count this file has to balance across clone, walk and clunk.
	 * ZERO means "no parent known" -- an attach, or a clone that has not
	 * walked -- and Tremove refuses it rather than guessing, which is also
	 * what refuses a remove of the root.
	 */
	ino_t		 f_pino;
};

struct conn {
	int		c_fd;		/* -1 when the slot is free */
	long		c_msize;
	struct fid	c_fid[NFID];
};

static struct conn	conns[NCONN];
static unsigned char	rxbuf[P9_MSIZE], txbuf[P9_MSIZE];
static int		verbose;

/*
 * THE MOUNT'S READ-ONLY FLAG, KEPT HERE AS WELL AS IN THE SUPERBLOCK, and the
 * duplication is deliberate rather than sloppy.  s_ronly is the kernel's copy
 * and is what iupdat and access() consult; this one is the SERVER's, and it
 * exists so that a refusal can be reported before Bell Labs' code is entered
 * at all.  The difference shows in the errno: reaching access() with s_ronly
 * set gives EROFS from inside a create that has already walked the path, while
 * refusing here answers the same EROFS having touched nothing.  Both are
 * correct and only the second can be relied on for Twstat, which does not go
 * through access() at all.
 */
static int		mntronly;

static struct fid *
fidof(struct conn *c, p9_u32 f)
{
	int i;

	if (f == P9_NOFID) return (0);
	for (i = 0; i < NFID; i++)
		if (c->c_fid[i].f_fid == f) return (&c->c_fid[i]);
	return (0);
}

static struct fid *
fidnew(struct conn *c, p9_u32 f)
{
	struct fid *p;
	int i;

	if (f == P9_NOFID) return (0);
	for (i = 0; i < NFID; i++) {
		if (c->c_fid[i].f_fid != P9_NOFID) continue;
		p = &c->c_fid[i];
		p->f_fid = f;
		p->f_ip = 0;
		p->f_omode = -1;
		p->f_name[0] = '\0';
		p->f_dir = 0;
		p->f_dirlen = 0;
		p->f_off = 0;
		p->f_pino = 0;
		return (p);
	}
	return (0);
}

static void
fidfree(struct fid *p)
{
	if (p->f_ip) { iput(p->f_ip); p->f_ip = 0; }
	if (p->f_dir) { free(p->f_dir); p->f_dir = 0; }
	p->f_dirlen = 0;
	p->f_omode = -1;
	p->f_fid = P9_NOFID;
}

static void
connclose(struct conn *c)
{
	int i;

	for (i = 0; i < NFID; i++)
		if (c->c_fid[i].f_fid != P9_NOFID) fidfree(&c->c_fid[i]);
	if (c->c_fd >= 0) close(c->c_fd);
	c->c_fd = -1;
}

/* ---------------------------------------------------------------- qids */

/*
 * A qid is the server's name for a file, and it must be unique and stable for
 * the life of the file.  The inode number is exactly that on a V7
 * filesystem -- it is what the directory entry stores and what iget is keyed
 * on -- so q_path is i_number and nothing has to be invented.
 *
 * q_vers IS ZERO, which is 9P's way of saying a server does not track
 * versions, and it is the honest answer here rather than a stub.  A version is
 * for a client that caches a fid across a change, and the obvious candidate --
 * the modification time -- IS NOT IN THE IN-CORE INODE.  V7 keeps the three
 * times only in the DISK inode: struct inode (src/sys/h/inode.h:15-53) has
 * i_mode, i_nlink, i_uid, i_gid, i_size and the block addresses, and iupdat
 * (iget.c:250-273) is what breads the dinode and writes di_atime/di_mtime/
 * di_ctime back.  So a version would cost a disk read on every walk, and a
 * wrong version is worse than none.
 */
static void
qidof(struct inode *ip, struct p9qid *q)
{
	q->q_type = ((ip->i_mode & IFMT) == IFDIR) ? P9_QTDIR : P9_QTFILE;
	q->q_vers = 0;
	q->q_path = (p9_u64)ip->i_number;
}

/*
 * The two times a stat needs, out of the DISK inode -- see above for why they
 * are not in the one in memory.  This is iupdat's own read, at iget.c:250-256,
 * with the write half left off:
 *
 *	bp = bread(ip->i_dev, itod(ip->i_dev, ip->i_number));
 *	dp = bp->b_un.b_dino;  dp += itoo(ip->i_dev, ip->i_number);
 *
 * Zeroes on a read error rather than failing the stat: a Tstat that could not
 * report a timestamp has still answered the question the client asked, and
 * every other field came from an inode that is already in core.
 */
static void
itimes(struct inode *ip, p9_u32 *at, p9_u32 *mt)
{
	struct buf *bp;
	struct dinode *dp;

	*at = *mt = 0;
	/*
	 * bread does not return NULL -- bio.c has only `return(bp)', and getblk
	 * sleeps rather than failing -- so the error is in the flags and there
	 * is no pointer to check.  A NULL test was here and is gone: an
	 * unconsumed guard standing in for a hazard that does not exist is the
	 * unconsumed-component rule applied to a line, which fsprobe.c records
	 * having got wrong the same way.
	 */
	bp = bread(ip->i_dev, itod(ip->i_dev, ip->i_number));
	if (!(bp->b_flags & B_ERROR)) {
		dp = bp->b_un.b_dino + itoo(ip->i_dev, ip->i_number);
		*at = (p9_u32)dp->di_atime;
		*mt = (p9_u32)dp->di_mtime;
	}
	brelse(bp);
}

/*
 * A stat from an inode.
 *
 * THE LENGTH OF A DIRECTORY IS REPORTED HONESTLY, against the 9P convention
 * that it is zero.  The convention is right for a Plan 9 client, which never
 * looks; it is wrong here, because the thing on the far end is a V8 program
 * and CLAUDE.md already records what a zero costs: ps(1)'s getdir() sizes an
 * array from st_size and then demands that read(2) return exactly that many
 * bytes.  Reporting i_size is also what the seam rule asks for -- below the
 * seam we translate, above it the V8 world must not find itself in a system
 * V8 never had.
 *
 * THE OWNER IS A DECIMAL STRING, not a login name.  9P wants a name and a V7
 * inode stores a number; turning one into the other means reading a passwd
 * file, and there are two of them here (the host's and the jail's) with no
 * principled way to choose.  A number renders as itself in any 9P client and
 * the client maps it straight back into st_uid.
 */
static void
statof(struct inode *ip, const char *name, struct p9stat *s)
{
	memset(s, 0, sizeof *s);
	s->s_type = 0;
	s->s_dev = 0;
	qidof(ip, &s->s_qid);
	s->s_mode = (p9_u32)(ip->i_mode & 07777);
	if ((ip->i_mode & IFMT) == IFDIR) s->s_mode |= P9_DMDIR;
	itimes(ip, &s->s_atime, &s->s_mtime);
	s->s_length = (p9_u64)(unsigned long)ip->i_size;
	strncpy(s->s_name, name, sizeof s->s_name - 1);
	snprintf(s->s_uid, sizeof s->s_uid, "%d", ip->i_uid);
	snprintf(s->s_gid, sizeof s->s_gid, "%d", ip->i_gid);
	strncpy(s->s_muid, s->s_uid, sizeof s->s_muid - 1);
}

/* ------------------------------------------------------- the kernel side */

/*
 * Read a span of a file through readi.  u_segflg is 1 -- "system space" --
 * which is what sends rdwri.c:104 down the bcopy arm instead of copyout, and
 * is what fsnami itself sets at nami.c:251 before it writes a directory entry.
 * A server that left it 0 would exercise a different arm from the one the
 * kernel uses on itself.
 */
static long
kread(struct inode *ip, long off, char *dst, long len)
{
	u.u_base = dst;
	u.u_count = len;
	u.u_offset = off;
	u.u_segflg = 1;
	u.u_error = 0;
	readi(ip);
	if (u.u_error) return (-1);
	return (len - (long)u.u_count);
}

/*
 * The write half of kread, §8a step 5f, and u_segflg matters MORE here than it
 * does for a read.  rdwri.c:191-197 is
 *
 *	if (u.u_segflg != 1) { if (copyin(...)) { u.u_error = EFAULT; ... } }
 *	else bcopy(u.u_base, bp->b_un.b_addr+on, n);
 *
 * -- so a server that left it 0 would send the server's own buffer address
 * through copyin as though it were a user address.  fsprobe.c's writefile()
 * records the same thing and this is the same four lines.
 *
 * THE RETURN IS WHAT WAS TRANSFERRED, not what was asked for, because writei
 * decrements u_count as it goes and can stop early -- on ENOSPC with some
 * blocks already written, which is exactly the case a Twrite has to report
 * honestly.  A short count is not an error and the caller must not turn it
 * into one.
 */
static long
kwrite(struct inode *ip, long off, char *src, long len)
{
	u.u_base = src;
	u.u_count = len;
	u.u_offset = off;
	u.u_segflg = 1;
	u.u_error = 0;
	writei(ip);
	if (u.u_error && (long)u.u_count == len) return (-1);
	return (len - (long)u.u_count);
}

/*
 * ONE COMPONENT, THROUGH BELL LABS' OWN namei, and doing it that way rather
 * than calling dsearch directly is the point of the exercise: the lookup a 9P
 * Twalk performs has to be the lookup the kernel performs.
 *
 * namei starts a relative path at u.u_cdir (nami.c, the `else p.dp =
 * u.u_cdir' arm), so the walk is expressed by moving u_cdir and handing it one
 * name.  It takes its OWN reference -- `plock(p.dp); p.dp->i_count++' -- and
 * releases it as it advances, so the caller's inode is neither consumed nor
 * leaked.
 *
 * It returns the inode LOCKED.  Everything here holds inodes across requests
 * (a fid outlives the message that made it), so the lock is cleared exactly as
 * v8k_kinit does for rootdir: there is one thread, nothing can contend, and an
 * inode left locked would deadlock the next plock on it.
 */
static struct inode *
kwalk(struct inode *dir, char *name)
{
	struct inode *save = u.u_cdir, *nip;
	char buf[P9_NAMELEN];

	strncpy(buf, name, sizeof buf - 1);
	buf[sizeof buf - 1] = '\0';
	u.u_cdir = dir;
	u.u_dirp = buf;
	u.u_error = 0;
	nip = namei(schar, (struct argnamei *)0, 1);
	u.u_cdir = save;
	if (nip) nip->i_flag &= ~ILOCK;
	return (nip);
}

/*
 * kwalk's write-side siblings, §8a step 5f.  All three drive namei with a
 * flag, in the same one-component-relative-to-a-directory shape kwalk uses,
 * because a Tcreate names a directory fid and one name and so does a Tremove
 * from the parent's side.
 *
 * NI_NXCREAT RATHER THAN NI_CREAT, and the difference is 9P's rather than
 * ours.  The two arms are one `case' apart at nami.c:494-495 and differ only
 * at nami.c:91, where an existing name is EEXIST for NXCREAT and a plain
 * lookup for CREAT.  9P's Tcreate is defined to fail if the name exists -- it
 * is creat(2)'s cousin only in name -- so NXCREAT is the arm that matches, and
 * O_CREAT's tolerate-it-if-present behaviour is assembled on the CLIENT out of
 * a walk and a Tcreate.  Getting this backwards would make `> file' silently
 * truncate a name a concurrent client had just made.
 */
static struct inode *
kcreate(struct inode *dir, char *name, int mode)
{
	struct inode *save = u.u_cdir, *nip;
	struct argnamei arg;
	char buf[P9_NAMELEN];

	strncpy(buf, name, sizeof buf - 1);
	buf[sizeof buf - 1] = '\0';
	arg.flag = NI_NXCREAT;
	arg.ino = 0;
	arg.idev = 0;
	arg.mode = (short)mode;
	u.u_cdir = dir;
	u.u_dirp = buf;
	u.u_error = 0;
	nip = namei(schar, &arg, 1);	/* follow, as creat does: sys2.c:154 */
	u.u_cdir = save;
	if (nip) nip->i_flag &= ~ILOCK;
	return (nip);
}

/*
 * mkdir -- sys/sys2.c:223-257, transcribed, and the transcription is what
 * makes it worth doing at all.  NI_MKDIR allocates the inode, sets i_nlink to
 * 2 and bumps the parent's, and writes the NAME into the parent -- and stops
 * there.  It does NOT write `.' and `..' into the new directory: that is the
 * syscall's own eleven lines, and a server that called namei and returned
 * would have made a directory fsck calls unattached and dcheck calls a
 * link-count mismatch.
 *
 * nmarg.ino IS THE PARENT'S NUMBER ON THE WAY OUT, which reads like a bug and
 * is not.  nami.c's NI_MKDIR arm does `flagp->ino = dp->i_number' while dp is
 * still the parent, and only then `dp = dip'.  So x[1] -- `..' -- takes it,
 * and x[0] -- `.' -- takes ip->i_number, the inode namei returned.  Upstream's
 * two lines, in upstream's order, because reasoning about which is which from
 * the names alone gets it the wrong way round.
 *
 * AND THE TIMESTAMP ARGUMENTS ARE SPELLED v8k_time, WHICH IS THE FILE'S OWN
 * TRAP AND WAS WALKED INTO WRITING THIS FUNCTION.  Upstream's line is
 * `iupdat(ip, &time, &time, 1)'.  Here hostok.h has handed `time' back to the
 * C library, so that line passes THE ADDRESS OF libc's time() as a time_t* --
 * and it compiles, because iupdat is declared K&R.  iupdat would then read
 * four bytes of the function's instructions as a timestamp and write them into
 * the inode.  Third instance of the hostok.h class after access() and free(),
 * and the first where the wrong thing is a variable rather than a call.
 */
static struct inode *
kmkdir(struct inode *dir, char *name, int mode)
{
	struct inode *save = u.u_cdir, *ip;
	struct argnamei arg;
	struct direct x[2];
	char buf[P9_NAMELEN];
	int i;

	strncpy(buf, name, sizeof buf - 1);
	buf[sizeof buf - 1] = '\0';
	arg.flag = NI_MKDIR;
	arg.mode = (short)((mode & 0777) | IFDIR);	/* sys2.c:234 */
	arg.ino = 0;
	arg.idev = 0;
	u.u_cdir = dir;
	u.u_dirp = buf;
	u.u_error = 0;
	ip = namei(schar, &arg, 0);
	u.u_cdir = save;
	if (ip == NULL)
		return (NULL);
	if (arg.ino == 0) {			/* sys2.c:240-245: it existed */
		if (u.u_error == 0) u.u_error = EEXIST;
		iput(ip);
		return (NULL);
	}
	x[0].d_ino = ip->i_number;
	x[1].d_ino = arg.ino;
	for (i = 0; i < DIRSIZ; i++)
		x[0].d_name[i] = x[1].d_name[i] = 0;
	x[0].d_name[0] = x[1].d_name[0] = x[1].d_name[1] = '.';
	u.u_count = 2 * sizeof(struct direct);
	u.u_base = (caddr_t)x;
	u.u_offset = 0;
	u.u_segflg = 1;
	iupdat(ip, &v8k_time, &v8k_time, 1);	/* sys2.c:253, see above */
	writei(ip);
	ip->i_flag &= ~ILOCK;
	return (ip);
}

/*
 * unlink and rmdir, which are one function here because 9P's Tremove is one
 * message.  sys/nami.c:298 and :330 are the two arms and sys2.c's rmdir() is
 * literally `nmarg.flag = NI_RMDIR; namei(uchar, &nmarg, 0)'.
 *
 * A SUCCESSFUL DELETE RETURNS NULL AND THAT IS NOT A FAILURE -- fsnami's NI_DEL
 * arm ends `goto out' and namei's case 1 iputs the parent and returns NULL, so
 * u_error is the only thing separating done from refused.  fsprobe.c records
 * the same shape; it is the enoent/notdir pair again.
 */
static int
kremove(struct inode *dir, char *name, int isdir)
{
	struct inode *save = u.u_cdir;
	struct argnamei arg;
	char buf[P9_NAMELEN];

	strncpy(buf, name, sizeof buf - 1);
	buf[sizeof buf - 1] = '\0';
	arg.flag = (short)(isdir ? NI_RMDIR : NI_DEL);
	arg.ino = 0;
	arg.idev = 0;
	arg.mode = 0;
	u.u_cdir = dir;
	u.u_dirp = buf;
	u.u_error = 0;
	(void)namei(schar, &arg, 0);
	u.u_cdir = save;
	return (u.u_error ? -1 : 0);
}

/*
 * The directory listing a Tread answers from, generated once per rewind.
 *
 * "." AND ".." ARE IN IT, against the Plan 9 convention that omits them, and
 * the reason is the seam rule rather than convenience.  They are entries this
 * filesystem contains -- mkfs wrote them and tests/mkfs asserts their byte
 * offsets -- and hiding them would be adopting Plan 9 SEMANTICS above the
 * seam, which PLAN.md §8a rules out in as many words: a V8 program must not
 * find itself in a world V8 never had.  A FUSE mount synthesises both anyway
 * and tolerates seeing them twice.
 *
 * The on-disk record is 16 bytes with a 14-character name, because the image
 * was written by `mkfs -DDIRSIZ=14'.  That is the one place in this port where
 * DIRSIZ is 14 rather than 254 and it is deliberate: src/sys/h/dir.h describes
 * a DISK RECORD.  So this loop reads 14-character names and the client builds
 * 254-character ones on the far side, and neither is wrong.
 */
static int
gendir(struct fid *f)
{
	struct direct d;
	struct inode *ip;
	struct p9stat st;
	struct p9buf b;
	unsigned char *out;
	long off, cap, used, n;
	char nm[DIRSIZ + 1];

	if (f->f_dir) { free(f->f_dir); f->f_dir = 0; f->f_dirlen = 0; }
	cap = 8192;
	if ((out = malloc((size_t)cap)) == 0) { u.u_error = ENOMEM; return (-1); }
	used = 0;

	for (off = 0; ; off += sizeof d) {
		n = kread(f->f_ip, off, (char *)&d, (long)sizeof d);
		if (n < 0) { free(out); return (-1); }
		if (n < (long)sizeof d) break;
		if (d.d_ino == 0) continue;		/* V7's deleted slot */

		strncpy(nm, d.d_name, DIRSIZ);
		nm[DIRSIZ] = '\0';
		if ((ip = iget(f->f_ip->i_dev, (ino_t)d.d_ino, 0)) == NULL) {
			free(out);
			return (-1);
		}
		ip->i_flag &= ~ILOCK;
		statof(ip, nm, &st);
		iput(ip);

		/*
		 * Grown before the encode rather than after a failure, because
		 * p9_pstat's cursor reports "did not fit" by a sticky flag and
		 * a half-written entry would already be in the buffer.
		 */
		if (cap - used < 2 + 2 + 512) {
			unsigned char *bigger = realloc(out, (size_t)(cap * 2));
			if (bigger == 0) { free(out); u.u_error = ENOMEM; return (-1); }
			out = bigger;
			cap *= 2;
		}
		p9_init(&b, out + used, cap - used);
		p9_pstat(&b, &st);
		if (!p9_ok(&b)) { free(out); u.u_error = EIO; return (-1); }
		used += p9_len(&b);
	}
	f->f_dir = out;
	f->f_dirlen = used;
	return (0);
}

/*
 * Is `off' the start of an entry in the generated listing (or its end)?  Each
 * entry begins with its own size[2], so the only way to know is to walk from
 * the beginning -- there is no index, and building one would be a second
 * description of the same bytes.
 */
static int
dirboundary(struct fid *f, p9_u64 off)
{
	long pos = 0, sz;

	/*
	 * UNSIGNED, AND THE CAST TO long HAPPENS ONLY AFTER THIS TEST.  `off'
	 * comes off the wire as a p9_u64; converting it to a signed long first
	 * is what let every offset at or above 2^63 read as negative and pass.
	 */
	if (off > (p9_u64)f->f_dirlen) return (0);
	while (pos < (long)off) {
		if (f->f_dirlen - pos < 2) return (0);
		sz = (long)f->f_dir[pos] | ((long)f->f_dir[pos + 1] << 8);
		pos += 2 + sz;
	}
	return (pos == (long)off);
}

/* -------------------------------------------------------- 9P dispatch */

/*
 * Send, and drop the connection if the send fails.  The type and tag are
 * already in the buffer -- p9_hdr put them there -- so they are deliberately
 * NOT parameters: a second copy of either would be a thing that could disagree
 * with the message actually going out, which is the class of bug this file's
 * one-codec arrangement exists to avoid one level up.
 */
static void
reply(struct conn *c, struct p9buf *b)
{
	if (p9_send(c->c_fd, b) < 0) connclose(c);
}

static void
rerror(struct conn *c, p9_u32 tag, int e)
{
	struct p9buf b;

	p9_hdr(&b, txbuf, sizeof txbuf, P9_Rerror, tag);
	p9_pstr(&b, ename(e));
	if (verbose) fprintf(stderr, "v8fsd: Rerror %s\n", ename(e));
	reply(c, &b);
}

static void
do_version(struct conn *c, p9_u32 tag, struct p9buf *in)
{
	struct p9buf b;
	char ver[64];
	p9_u32 msize;
	long want;

	msize = p9_g32(in);
	if (p9_gstr(in, ver, sizeof ver) < 0 || !p9_ok(in)) {
		rerror(c, tag, EINVAL);
		return;
	}
	/*
	 * VALIDATE BEFORE RESETTING, and the order is the fix rather than the
	 * style.  Tversion resets the connection -- the spec says every
	 * outstanding fid is clunked -- but the clunk loop used to run FIRST,
	 * so a Tversion this server then refused had already thrown the
	 * client's state away: it was told the negotiation failed and silently
	 * lost every fid it held.  Measured by a review subagent.
	 */
	want = (long)msize;
	if (want > P9_MSIZE) want = P9_MSIZE;
	if (want < P9_HDRSZ + 64) { rerror(c, tag, EINVAL); return; }

	{
		int i;
		for (i = 0; i < NFID; i++)
			if (c->c_fid[i].f_fid != P9_NOFID) fidfree(&c->c_fid[i]);
	}
	c->c_msize = want;

	p9_hdr(&b, txbuf, sizeof txbuf, P9_Rversion, tag);
	p9_p32(&b, (p9_u32)want);
	/*
	 * "unknown" is the spec's answer to a version this server does not
	 * speak, and it is a successful Rversion rather than an Rerror.  The
	 * prefix test is the spec's too: a client may offer "9P2000.u" and a
	 * server that speaks only the base protocol answers "9P2000".
	 */
	/*
	 * ...AND THE EXTENSION IS NEGOTIATED HERE, which is what makes it an
	 * extension rather than a divergence.  A client that asks for
	 * "9P2000.v8" is told it has it; anything else starting with "9P2000"
	 * gets the base protocol, exactly as a conforming server would answer.
	 * p9.h says why the client needs the distinction: it sends P9_OFFCUR on
	 * every read, and against a server with no cursor that is a silent
	 * empty file rather than an error.
	 */
	p9_pstr(&b, strcmp(ver, P9_VERSION_V8) == 0 ? P9_VERSION_V8 :
	    strncmp(ver, "9P2000", 6) == 0 ? P9_VERSION : "unknown");
	reply(c, &b);
}

static void
do_attach(struct conn *c, p9_u32 tag, struct p9buf *in)
{
	struct p9buf b;
	struct fid *f;
	char uname[P9_NAMELEN], aname[P9_NAMELEN];
	p9_u32 fid, afid;
	struct inode *ip;

	fid = p9_g32(in);
	afid = p9_g32(in);
	if (p9_gstr(in, uname, sizeof uname) < 0 ||
	    p9_gstr(in, aname, sizeof aname) < 0 || !p9_ok(in)) {
		rerror(c, tag, EINVAL);
		return;
	}
	/*
	 * afid must be NOFID: this server offers no authentication, so a
	 * client that presents an auth fid is asking for something that was
	 * never negotiated.  Tauth answers Rerror for the same reason.
	 */
	if (afid != P9_NOFID) { rerror(c, tag, EPERM); return; }
	/*
	 * TWO WAYS TO FAIL, AND THEY WERE ONE.  fidnew used to refuse both a
	 * fid already in use and a full table, and the caller reported EEXIST
	 * for either -- so a client that had leaked fids was told it had made
	 * a naming mistake.  Asked separately now.
	 */
	if (fidof(c, fid)) { rerror(c, tag, EEXIST); return; }
	if ((f = fidnew(c, fid)) == 0) { rerror(c, tag, EMFILE); return; }

	if ((ip = iget(rootdev, (ino_t)ROOTINO, 0)) == NULL) {
		fidfree(f);
		rerror(c, tag, u.u_error ? u.u_error : EIO);
		return;
	}
	ip->i_flag &= ~ILOCK;
	f->f_ip = ip;
	strcpy(f->f_name, "/");

	p9_hdr(&b, txbuf, sizeof txbuf, P9_Rattach, tag);
	{ struct p9qid q; qidof(ip, &q); p9_pqid(&b, &q); }
	reply(c, &b);
}

static void
do_walk(struct conn *c, p9_u32 tag, struct p9buf *in)
{
	struct p9buf b;
	struct fid *f, *nf;
	struct inode *ip, *nip;
	char names[P9_MAXWELEM][P9_NAMELEN];
	struct p9qid qids[P9_MAXWELEM];
	p9_u32 fid, newfid, nwname;
	ino_t pino;
	int i, walked;

	fid = p9_g32(in);
	newfid = p9_g32(in);
	nwname = p9_g16(in);
	if (nwname > P9_MAXWELEM) { rerror(c, tag, EINVAL); return; }
	for (i = 0; i < (int)nwname; i++)
		if (p9_gstr(in, names[i], sizeof names[i]) < 0) break;
	if (!p9_ok(in)) { rerror(c, tag, EINVAL); return; }

	if ((f = fidof(c, fid)) == 0) { rerror(c, tag, EBADF); return; }
	/*
	 * A fid that has been opened may not be walked -- the spec is explicit
	 * about it, and the reason is that a walk would move the file a read
	 * offset already refers to.
	 */
	if (f->f_omode >= 0) { rerror(c, tag, EINVAL); return; }

	if (newfid == fid) nf = f;
	else if (fidof(c, newfid)) { rerror(c, tag, EEXIST); return; }
	else if ((nf = fidnew(c, newfid)) == 0) { rerror(c, tag, EMFILE); return; }

	ip = f->f_ip;
	pino = f->f_pino;		/* inherited by a clone; see f_pino */
	walked = 0;
	for (i = 0; i < (int)nwname; i++) {
		/*
		 * A WALK ELEMENT IS ONE PATH COMPONENT, and nothing enforced
		 * it.  kwalk hands the name to namei, which splits on `/' and
		 * RESTARTS AT rootdir for a leading one (nami.c:65-71) -- so
		 * "sub/deep" traversed two components and reported one qid, and
		 * "/hello" from a fid on /sub escaped the directory it was
		 * walked from.  Not a containment hole, because namei cannot
		 * leave the served image, but the Rwalk qid count stops
		 * describing the traversal, which is the thing a client uses to
		 * tell how much of its path exists.  The empty name is refused
		 * for the same reason: it is not a component.
		 */
		if (names[i][0] == '\0' || strchr(names[i], '/') != 0) {
			u.u_error = ENOENT;
			break;
		}
		if ((ip->i_mode & IFMT) != IFDIR) { u.u_error = ENOTDIR; break; }
		if ((nip = kwalk(ip, names[i])) == NULL) break;
		/*
		 * RECORDED BEFORE ip MOVES, which is the whole trick -- one line
		 * further down and it would be the child's own number.
		 *
		 * `.' AND `..' SET IT TO ZERO rather than to the directory they
		 * were looked up in, because for those two the fid's NAME is not
		 * an entry anybody may remove.  A fid walked to `sub/..' points
		 * at the root and is called `..'; without this a Tremove through
		 * it would ask to unlink an entry named `..'.  Zero is the "no
		 * parent known" value and Tremove refuses it.
		 */
		pino = (strcmp(names[i], ".") == 0 || strcmp(names[i], "..") == 0)
		    ? (ino_t)0 : ip->i_number;
		if (walked) iput(ip);		/* an intermediate we made */
		ip = nip;
		qidof(ip, &qids[i]);
		walked++;
	}

	/*
	 * A WALK THAT FAILS ON THE FIRST NAME IS AN ERROR; one that fails
	 * later is a SHORT Rwalk.  The distinction is the spec's and it is
	 * load-bearing for a client: a short walk means "this much of the path
	 * exists", which is how a client tells `no such file' from `no such
	 * directory on the way to it'.
	 */
	/*
	 * THE THREE OUTCOMES, SEPARATED, because the first draft folded the
	 * clone into the complete-walk arm and that put a reference in two
	 * places.  It did `nf->f_ip = ip' and THEN took the clone's own iget;
	 * if that iget failed, fidfree(nf) released the inode `f' still owned,
	 * and f's own clunk would have released it a second time.  The
	 * assignment was dead on every path that worked, which is why it read
	 * as harmless.
	 */
	if (walked < (int)nwname) {
		/*
		 * SHORT OR FAILED.  9P says a walk that does not complete leaves
		 * newfid unaffected, so the new fid never comes into existence;
		 * the inode the walk reached is ours to release, because `f'
		 * still holds its own.
		 */
		if (nf != f) fidfree(nf);
		if (walked) iput(ip);
		if (walked == 0) {
			rerror(c, tag, u.u_error ? u.u_error : ENOENT);
			return;
		}
	} else if (nwname == 0) {
		/*
		 * A CLONE.  Both fids must own a reference of their own -- a
		 * second POINTER would mean the first clunk released an inode the
		 * other still refers to -- so this is a second iget on the same
		 * (dev, inumber), which iget answers from its hash.  newfid ==
		 * fid is legal and is a no-op.
		 */
		if (nf != f) {
			struct inode *cip = iget(ip->i_dev, ip->i_number, 0);

			if (cip == NULL) {
				fidfree(nf);
				rerror(c, tag, u.u_error ? u.u_error : EIO);
				return;
			}
			cip->i_flag &= ~ILOCK;
			nf->f_ip = cip;
			strcpy(nf->f_name, f->f_name);
			nf->f_pino = f->f_pino;
		}
	} else {
		/*
		 * A COMPLETE WALK.  `ip' carries the reference kwalk took and it
		 * moves to whichever fid the client named.  The newfid == fid arm
		 * is the only place this function releases a reference the CLIENT
		 * owned, and it must, or the old inode leaks.
		 */
		if (nf == f) iput(f->f_ip);
		nf->f_ip = ip;
		strcpy(nf->f_name, names[nwname - 1]);
		nf->f_pino = pino;	/* the directory the last name was found in */
	}

	p9_hdr(&b, txbuf, sizeof txbuf, P9_Rwalk, tag);
	p9_p16(&b, (p9_u32)walked);
	for (i = 0; i < walked; i++) p9_pqid(&b, &qids[i]);
	reply(c, &b);
}

static void
do_open(struct conn *c, p9_u32 tag, struct p9buf *in)
{
	struct p9buf b;
	struct fid *f;
	p9_u32 fid;
	int mode, want;

	fid = p9_g32(in);
	mode = (int)p9_g8(in);
	if (!p9_ok(in)) { rerror(c, tag, EINVAL); return; }
	if ((f = fidof(c, fid)) == 0) { rerror(c, tag, EBADF); return; }
	if (f->f_omode >= 0) { rerror(c, tag, EINVAL); return; }

	/*
	 * PERMISSION IS ASKED OF BELL LABS' access(), not recomputed here.
	 * fio.c's version is the one with the root bypass, the group shift and
	 * the s_ronly arm, and it returns NONZERO for denied -- the opposite
	 * polarity from suser() eleven lines away, which is upstream's own
	 * inconsistency and is reproduced rather than harmonised.
	 */
	switch (mode & 3) {
	case P9_OREAD:	want = IREAD; break;
	case P9_OWRITE:	want = IWRITE; break;
	case P9_ORDWR:	want = IREAD | IWRITE; break;
	default:	want = IEXEC; break;
	}
	/*
	 * ORCLOSE IS REFUSED AND OTRUNC IS NOT, §8a step 5f, and the pair used
	 * to be one EROFS line for both.
	 *
	 * ORCLOSE means "remove this file when the fid is clunked" -- Plan 9's
	 * temporary-file idiom, and there is nothing in a V7 userspace that
	 * asks for it.  Implementing it would put a Tremove inside a clunk,
	 * which is the one place this server must not fail: a clunk is how a
	 * client says goodbye.  EINVAL rather than EROFS, because it is not
	 * about the mount being writable -- a read-only mount and a writable
	 * one refuse it identically, and the client never sets it.
	 */
	if (mode & P9_ORCLOSE) { rerror(c, tag, EINVAL); return; }
	if (mntronly && ((want & IWRITE) || (mode & P9_OTRUNC))) {
		rerror(c, tag, EROFS);
		return;
	}

	u.u_error = 0;
	if (v8k_access(f->f_ip, want)) {
		rerror(c, tag, u.u_error ? u.u_error : EACCES);
		return;
	}

	/*
	 * OTRUNC IS itrunc() AND NOTHING ELSE, AFTER access() AND NOT BEFORE.
	 * open1 at sys/sys2.c:188-200 is the model: the three access() calls,
	 * `if(u.u_error) goto out', and only then `if(trf == 1) itrunc(ip)'.
	 * A truncation performed on a file the caller may not write would be a
	 * destroyed file and a returned error.
	 *
	 * A DRAFT OF THIS ALSO ZEROED i_size AND SET IUPD|ICHG, on the theory
	 * that itrunc only frees blocks.  It does not: iget.c:349 is
	 * `ip->i_size = 0' and the three lines above it are a comment saying
	 * the inode has already been written and the flags already updated.
	 * itrunc writes a ZEROED COPY of the inode synchronously BEFORE it
	 * frees anything -- iget.c:311-319, so a crash mid-free leaves harmless
	 * missing blocks rather than a duplicate claim -- and then clears
	 * IUPD|IACC|ICHG deliberately.  Setting them again would undo exactly
	 * that reasoning.  Read, not recalled.
	 */
	if (mode & P9_OTRUNC)
		itrunc(f->f_ip);
	f->f_omode = mode;

	p9_hdr(&b, txbuf, sizeof txbuf, P9_Ropen, tag);
	{ struct p9qid q; qidof(f->f_ip, &q); p9_pqid(&b, &q); }
	/*
	 * iounit: the largest read or write this server will do in one
	 * message.  Reported rather than left 0 so the client does not have to
	 * rederive it from msize and this header's size.
	 */
	p9_p32(&b, (p9_u32)(c->c_msize - P9_IOHDRSZ));
	reply(c, &b);
}

static void
do_read(struct conn *c, p9_u32 tag, struct p9buf *in)
{
	struct p9buf b;
	struct fid *f;
	p9_u32 fid, count;
	p9_u64 off;
	long n, max;
	int atcur;
	unsigned char *dst;

	fid = p9_g32(in);
	off = p9_g64(in);
	count = p9_g32(in);
	if (!p9_ok(in)) { rerror(c, tag, EINVAL); return; }
	if ((f = fidof(c, fid)) == 0) { rerror(c, tag, EBADF); return; }
	if (f->f_omode < 0) { rerror(c, tag, EBADF); return; }

	/*
	 * THE SENTINEL, AND IT IS RESOLVED HERE SO THAT EVERYTHING BELOW IS
	 * 9P's OWN CODE PATH.  p9.h's extension note has the argument for the
	 * cursor existing at all; what matters at this line is that an
	 * explicit offset must reach the rest of this function unchanged, or
	 * the server stops being one a conforming client can use.
	 */
	atcur = (off == P9_OFFCUR);
	/*
	 * A DIRECTORY HAS NO CURSOR HERE, AND REFUSING IS WHAT TURNS A SILENT
	 * WRONG ANSWER INTO A LOUD ONE.
	 *
	 * The client converts a directory into V7 256-byte records and serves
	 * reads out of its own snapshot (p9cl.c), so the position that matters
	 * is in the record stream and not in this listing -- there is no
	 * sensible cursor for this end to keep.  What made it worth a guard is
	 * what happens without one: a v8fs directory descriptor that leaves the
	 * process that opened it (a plain dup(2) is enough) has no snapshot, so
	 * p9_t_read falls through to a real Tread and the program receives RAW
	 * 9P STAT RECORDS and exit 0.  Measured by an auditor: 222 bytes whose
	 * first two are a stat's size[2], read as a d_ino of 51.
	 *
	 * EISDIR rather than EINVAL because that is the answer the program
	 * needs -- and it restores parity with passthrough, where read(2) on a
	 * host directory descriptor the shim does not know returns -1.
	 */
	if (atcur && (f->f_ip->i_mode & IFMT) == IFDIR) {
		rerror(c, tag, EISDIR);
		return;
	}
	if (atcur) off = f->f_off;

	max = c->c_msize - P9_IOHDRSZ;
	if ((long)count > max) count = (p9_u32)max;

	p9_hdr(&b, txbuf, sizeof txbuf, P9_Rread, tag);
	p9_p32(&b, 0);				/* count, patched below */
	dst = txbuf + p9_len(&b);

	if ((f->f_ip->i_mode & IFMT) == IFDIR) {
		/*
		 * A DIRECTORY READ AT OFFSET 0 IS A REWIND and anything else
		 * must land on an entry boundary -- the spec's rule, and the
		 * reason for it is that a stat cannot be cut in half.  A
		 * client that seeks into the middle of one gets EINVAL rather
		 * than a truncated entry, which would decode as a plausible
		 * short name.
		 */
		if (off == 0 || f->f_dir == 0) {
			if (gendir(f) < 0) {
				rerror(c, tag, u.u_error ? u.u_error : EIO);
				return;
			}
		}
		/*
		 * ONE GUARD, AND IT WAS TWO UNTIL A MUTATION SAID SO.
		 *
		 * The bug was a REMOTE CRASH IN FOUR MESSAGES: `off' is a
		 * p9_u64 off the wire and the test here was
		 * `(long)off > f->f_dirlen', so every offset at or above 2^63
		 * read as negative and passed.  `f_dirlen - off' then exceeded
		 * the buffer and `f_dir + off' pointed BELOW it.  Measured on a
		 * 219-byte listing: 2^64-1 returned 220 bytes starting one byte
		 * before the buffer, -7973 returned 7973 bytes of heap, and
		 * -2^40 was a SIGSEGV that took every other connection with it.
		 * The file arm eight lines down had the guard all along
		 * (`off > 0x7fffffffULL') -- the fix landed on one line and the
		 * line beside it kept the assumption, which is this port's most
		 * repeated shape.  Found by a review subagent, not by a case.
		 *
		 * The first fix was an unsigned range test HERE plus the
		 * boundary walk below, and reverting the range test changed
		 * nothing: the walk rejects a negative offset too, so the line
		 * that looked like the fix was dead.  A mutation that does not
		 * fire is the informative one.  So the range test moved INSIDE
		 * dirboundary, where it happens before any cast to a signed
		 * type, and there is one guard rather than two with one of them
		 * unexercised.
		 *
		 * What it enforces is 9P's own rule, which the comment above
		 * this function claimed and nothing implemented: a directory
		 * offset must be 0 or one a previous read returned.  A read
		 * three bytes into a stat does not return a truncated entry --
		 * it returns a length field taken from the middle of a name.
		 */
		if (!dirboundary(f, off)) { rerror(c, tag, EINVAL); return; }
		n = f->f_dirlen - (long)off;
		if (n > (long)count) {
			/*
			 * Trim to whole entries.  Each begins with its own
			 * size[2], so walking forward from the offset is the
			 * only way to find the boundary -- and it is exactly
			 * what a client decoding the reply will do.
			 */
			long pos = 0, sz;
			unsigned char *p = f->f_dir + off;
			while (pos < (long)count) {
				if (n - pos < 2) break;
				sz = (long)p[pos] | ((long)p[pos + 1] << 8);
				if (pos + 2 + sz > (long)count) break;
				pos += 2 + sz;
			}
			n = pos;
		}
		if (n > 0) memcpy(dst, f->f_dir + off, (size_t)n);
	} else {
		if (off > 0x7fffffffULL) { rerror(c, tag, EINVAL); return; }
		n = kread(f->f_ip, (long)off, (char *)dst, (long)count);
		if (n < 0) { rerror(c, tag, u.u_error ? u.u_error : EIO); return; }
	}

	/*
	 * ADVANCE ONLY WHAT THE SENTINEL ASKED FOR.  `n' is what was actually
	 * produced, not what was requested, so a short read at end of file
	 * leaves the cursor at the end rather than past it -- which is what
	 * makes a second read return 0 instead of EINVAL from the directory
	 * boundary walk.
	 */
	if (atcur) f->f_off = off + (p9_u64)n;

	txbuf[7] = (unsigned char)(n & 0xff);
	txbuf[8] = (unsigned char)((n >> 8) & 0xff);
	txbuf[9] = (unsigned char)((n >> 16) & 0xff);
	txbuf[10] = (unsigned char)((n >> 24) & 0xff);
	b.b_p = dst + n;
	reply(c, &b);
}

/*
 * Twrite -- §8a step 5f, and it is do_read's mirror in three places that are
 * each a trap if they are not mirrored.
 *
 *   THE SENTINEL.  The client has ALWAYS sent P9_OFFCUR on Twrite (p9cl.c's
 *   p9_t_write) while only do_read resolved it, and p9.h has carried the
 *   warning since an auditor read the two files together: a do_write without
 *   these two lines writes every byte at offset 0xFFFFFFFFFFFFFFFF.  That is
 *   not a wild pointer, it is a request to make a file 16 exabytes long, and
 *   what it would actually have done is grow the inode through bmap's
 *   allocating arm until the image filled.
 *
 *   THE ADVANCE IS WHAT WAS TRANSFERRED.  writei decrements u_count and can
 *   stop early -- ENOSPC with some blocks already written is the real case --
 *   so the cursor moves by the count in the Rwrite, not by the count in the
 *   Twrite.  Same sentence as do_read's, for the same reason.
 *
 *   THE UPPER-BOUND GUARD.  do_read's file arm refuses `off > 0x7fffffff'
 *   because u_offset is an off_t the kernel treats as signed and a V7
 *   filesystem cannot address past 2^31 anyway.  A write needs it MORE: a
 *   read past the end returns nothing, while a write past it asks bmap to
 *   allocate.  Note the order -- the sentinel is resolved first, so that the
 *   guard tests the offset that will actually be used.
 */
static void
do_write(struct conn *c, p9_u32 tag, struct p9buf *in)
{
	struct p9buf b;
	struct fid *f;
	p9_u32 fid, count;
	p9_u64 off;
	long n;
	int atcur;
	char *src;

	fid = p9_g32(in);
	off = p9_g64(in);
	count = p9_g32(in);
	src = (char *)p9_gdata(in, count);
	if (!p9_ok(in) || src == 0) { rerror(c, tag, EINVAL); return; }

	/*
	 * READ-ONLY IS TESTED BEFORE THE FID, and the order is a decision.  9P
	 * would have EBADF for a write to an unopened or read-opened fid, and
	 * that is what the two lines below give on a writable mount.  But on a
	 * read-only one the fid's state cannot change the answer -- no fid on
	 * this connection will ever accept a write -- and EROFS says WHY where
	 * EBADF says only that this attempt was malformed.  do_open makes the
	 * same choice one message earlier.
	 *
	 * It is also what keeps p9probe's raw Twrite meaningful: the probe
	 * opens a fid for READ and then writes to it on purpose, which is a
	 * thing no client of ours would do, and the answer it is asking about
	 * is the server's read-only-ness rather than its fid bookkeeping.
	 */
	if (mntronly) { rerror(c, tag, EROFS); return; }
	if ((f = fidof(c, fid)) == 0) { rerror(c, tag, EBADF); return; }
	if (f->f_omode < 0) { rerror(c, tag, EBADF); return; }
	if ((f->f_omode & 3) == P9_OREAD) { rerror(c, tag, EBADF); return; }

	/*
	 * A DIRECTORY IS NOT WRITABLE THROUGH 9P AND MUST NOT BE HERE EITHER.
	 * writei would do it -- the kernel writes directories with exactly this
	 * call -- and the result would be a filesystem whose entries a client
	 * had composed.  V7 says the same thing one layer up, at open1's
	 * `if((ip->i_mode&IFMT) == IFDIR) u.u_error = EISDIR'.
	 */
	if ((f->f_ip->i_mode & IFMT) == IFDIR) { rerror(c, tag, EISDIR); return; }

	atcur = (off == P9_OFFCUR);
	if (atcur) off = f->f_off;
	if (off > 0x7fffffffULL) { rerror(c, tag, EINVAL); return; }

	n = kwrite(f->f_ip, (long)off, src, (long)count);
	if (n < 0) { rerror(c, tag, u.u_error ? u.u_error : EIO); return; }
	if (atcur) f->f_off = off + (p9_u64)n;

	p9_hdr(&b, txbuf, sizeof txbuf, P9_Rwrite, tag);
	p9_p32(&b, (p9_u32)n);
	reply(c, &b);
}

/*
 * Tcreate -- make a name in the directory the fid names, and leave the fid
 * pointing at the new file, opened.  That last clause is 9P's, not V7's, and
 * it is the whole reason this is not just "creat with extra steps": the fid
 * that named the PARENT comes back naming the CHILD, so the parent's inode
 * must be released exactly once and only on success.
 *
 * NAME VALIDATION IS THE SERVER'S JOB BECAUSE THE WIRE PUTS NO LIMIT ON IT.
 * A 9P name is a counted string up to 65535 bytes; a V7 directory entry holds
 * fourteen.  namei would silently truncate -- fsnami compares DIRSIZ bytes --
 * so two different Tcreates could make one entry, and the second would get
 * EEXIST for a name it had never seen.  `.' and `..' are refused for the same
 * class of reason: mkfs put them there and nami.c's dsearch would find them,
 * so a create would return EEXIST rather than corrupt anything, but the errno
 * 9P asks for is EINVAL and the two are worth telling apart.
 */
static void
do_create(struct conn *c, p9_u32 tag, struct p9buf *in)
{
	struct p9buf b;
	struct fid *f;
	struct inode *nip, *parent;
	struct p9qid q;
	char name[P9_NAMELEN];
	p9_u32 fid, perm;
	int mode;

	fid = p9_g32(in);
	if (p9_gstr(in, name, sizeof name) < 0) { rerror(c, tag, EINVAL); return; }
	perm = p9_g32(in);
	mode = (int)p9_g8(in);
	if (!p9_ok(in)) { rerror(c, tag, EINVAL); return; }
	if ((f = fidof(c, fid)) == 0) { rerror(c, tag, EBADF); return; }
	if (f->f_omode >= 0) { rerror(c, tag, EINVAL); return; }
	if ((f->f_ip->i_mode & IFMT) != IFDIR) { rerror(c, tag, ENOTDIR); return; }
	if (mntronly) { rerror(c, tag, EROFS); return; }
	if (name[0] == '\0' || strlen(name) > DIRSIZ ||
	    strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		rerror(c, tag, EINVAL);
		return;
	}
	if (mode & P9_ORCLOSE) { rerror(c, tag, EINVAL); return; }

	/*
	 * THE PERMISSION WORD IS 9P's AND THE LOW NINE BITS ARE V7's, which is
	 * true of every 9P server and is why DMDIR sits in the TOP bit rather
	 * than in IFMT's position.  Everything above 0777 that is not DMDIR --
	 * DMAPPEND, DMEXCL, the setuid bits 9P does not define -- is dropped,
	 * because a bit this server does not implement must not be recorded in
	 * an inode as though it had been.
	 */
	parent = f->f_ip;
	if (perm & P9_DMDIR)
		nip = kmkdir(parent, name, (int)(perm & 0777));
	else
		nip = kcreate(parent, name, (int)(perm & 0777));
	if (nip == NULL) { rerror(c, tag, u.u_error ? u.u_error : EIO); return; }

	/*
	 * OPENED WITHOUT AN access() CALL, and that is V7's rule rather than an
	 * omission -- so there is deliberately no mode-to-IREAD/IWRITE switch
	 * here, and a draft that had one (computed, then thrown away) has been
	 * removed rather than left to read as a check.  The permission that
	 * governs a create is the PARENT's write bit, which kcreate has just
	 * been through in nami.c's `if(access(dp, IWRITE)) goto out'; the file
	 * did not exist a moment ago, so its own mode cannot deny its maker.
	 * creat() says the same thing by calling open1 with trf 2, and open1's
	 * `if(trf != 2)' at sys2.c:188 skips exactly those checks.
	 */
	/*
	 * f_pino IS TAKEN BEFORE THE iput, and it is the create's own parent
	 * rather than whatever the fid was walked from -- the fid named the
	 * DIRECTORY on the way in and names the new FILE on the way out, so its
	 * recorded parent has to move with it.  Without this a Tcreate followed
	 * by a Tremove on the same fid, which is what O_CREAT then unlink is,
	 * would unlink the new name from the wrong directory.
	 */
	f->f_pino = parent->i_number;
	iput(parent);
	f->f_ip = nip;
	f->f_omode = mode;
	f->f_off = 0;
	strncpy(f->f_name, name, sizeof f->f_name - 1);
	f->f_name[sizeof f->f_name - 1] = '\0';

	p9_hdr(&b, txbuf, sizeof txbuf, P9_Rcreate, tag);
	qidof(nip, &q);
	p9_pqid(&b, &q);
	p9_p32(&b, (p9_u32)(c->c_msize - P9_IOHDRSZ));
	reply(c, &b);
}

/*
 * Tremove -- unlink, and 9P defines it as "remove the file AND clunk the fid,
 * whether or not the removal succeeded".  The second half is not optional and
 * is easy to get wrong: a client that sees Rerror must still not send Tclunk,
 * so a server that left the fid alive would leak one per failed remove.
 *
 * THE PARENT COMES FROM THE FID, and f_pino's comment has the argument for
 * why it has to be recorded during the walk rather than recovered here.  The
 * short form: a fid names a FILE, V7's unlink names a DIRECTORY and an ENTRY,
 * and nothing in a struct inode bridges the two -- `..' is an entry, so it
 * exists for a directory and not for a plain file.
 *
 * A ZERO f_pino IS REFUSED RATHER THAN GUESSED, which covers three real cases
 * with one test: the root (a client that attaches and immediately removes), a
 * clone that has never walked, and a fid whose last component was `.' or `..'
 * and whose name is therefore not an entry anybody may unlink.
 */
static void
do_remove(struct conn *c, p9_u32 tag, struct p9buf *in)
{
	struct p9buf b;
	struct fid *f;
	struct inode *parent;
	p9_u32 fid;
	int isdir, err;

	fid = p9_g32(in);
	if (!p9_ok(in)) { rerror(c, tag, EINVAL); return; }
	if ((f = fidof(c, fid)) == 0) { rerror(c, tag, EBADF); return; }

	/*
	 * EVERY EXIT FROM HERE DOWN CLUNKS THE FID.  Written as one fidfree at
	 * the bottom rather than one per arm, because 9P's rule is about the
	 * fid and not about the outcome, and an arm that forgot would be
	 * invisible until a client ran out of fids.
	 */
	if (mntronly) {
		err = EROFS;
	} else if (f->f_pino == 0) {
		err = EINVAL;
	} else if ((parent = iget(f->f_ip->i_dev, f->f_pino, 0)) == NULL) {
		err = u.u_error ? u.u_error : EIO;
	} else {
		parent->i_flag &= ~ILOCK;	/* kwalk's reason exactly */
		isdir = (f->f_ip->i_mode & IFMT) == IFDIR;
		/*
		 * THE TARGET IS RELEASED BEFORE THE REMOVE, and it has to be.
		 * nami.c's NI_DEL arm does its own iget on the entry it found
		 * and then iput()s it -- and it is THAT iput, at i_count 1,
		 * which sees i_nlink drop to 0 and runs itrunc and ifree.  A
		 * fid still holding the inode keeps i_count at 2, so the
		 * blocks would never come back and the image would leak one
		 * file per remove.  Invisible to a reader and loud in icheck,
		 * which is the step-5d lesson about independent checkers.
		 */
		iput(f->f_ip);
		f->f_ip = 0;
		err = kremove(parent, f->f_name, isdir) < 0
		    ? (u.u_error ? u.u_error : EIO) : 0;
		iput(parent);
	}

	fidfree(f);
	if (err) { rerror(c, tag, err); return; }
	p9_hdr(&b, txbuf, sizeof txbuf, P9_Rremove, tag);
	reply(c, &b);
}

/*
 * Twstat -- the one message that carries a whole stat, and 9P's rule for it is
 * that EVERY FIELD WHICH IS "DO NOT TOUCH" IS SENT AS ALL ONES.  So the work
 * here is not applying a stat, it is deciding which of its fields the client
 * actually meant, and a server that applied them all would zero a file's mode
 * every time somebody set its length.
 *
 * FIVE FIELDS ARE HONOURED and each maps onto a V7 syscall this port owes the
 * mount: s_length is truncate, s_mode is chmod, s_uid/s_gid are chown,
 * s_atime and s_mtime are utime.  s_name is a rename and is NOT honoured --
 * V7's rename is unlink-and-link in the shell's own words, there is no syscall
 * for it, and doing it here would mean composing directory entries by hand.
 *
 * s_atime WAS THE FIFTH AND IT ARRIVED WITH ITS CONSUMER, which is this
 * repository's most repeated shape: an unexercised rule cannot be seen to be
 * incomplete.  The reason recorded here for declining it was "nothing in this
 * world sets atime alone", and that sentence is still true and never covered
 * the case that turned up -- mv(1) sets BOTH.  mv.c:129 is
 * `utime(target, &s1.st_atime)', taking the address of one struct stat field
 * and relying on atime/mtime being adjacent time_t's to pass a time_t[2]; and
 * on a mount it is not an unusual path but the ONLY path, because link(2) has
 * no slot and is refused, so mv always falls through to fork, /bin/cp and
 * utime.  Declining an arm because no caller sets a field ALONE is a different
 * claim from no caller setting it at all, and only the second one would have
 * been a reason.
 *
 * A ZERO-LENGTH NAME IS THE "DO NOT TOUCH" FORM for a string, not all ones,
 * which is 9P's own asymmetry and the reason s_name gets a length test rather
 * than a comparison against a sentinel.
 */

/*
 * owner() -- fio.c:215-228, less the namei, and it is here because the
 * COMMENT IN do_wstat CLAIMED THIS RULE WHILE THE CODE TESTED suser() ALONE.
 * Upstream is
 *
 *	owner(follow) { ip = namei(...);
 *		if(u.u_uid == ip->i_uid) return(ip);
 *		if(suser()) return(ip);
 *		iput(ip); return(NULL); }
 *
 * so the rule is ownership OR superuser, and chmod (sys4.c:238) and utime
 * (sys4.c:521) both gate on it.  chown (sys4.c:282) is the one that really is
 * superuser-only -- `if (!suser() || (ip = owner(1)) == NULL) return' -- and
 * its arm below is therefore right as it stands.
 *
 * NOT OBSERVABLE TODAY, which is exactly why the sentence and the line could
 * disagree: u_uid is 0 here and nothing sets it, so suser() is always true and
 * both rules always permit.  The day a uid arrives over the wire the
 * difference between them is the whole of the answer, and the citation the old
 * comment gave -- sys3.c -- was wrong as well; sys3.c is fsmount.
 *
 * suser() HAS A SIDE EFFECT (u_acflag |= ASU) and upstream reaches it only
 * when the uid does not match, so the || has to short-circuit in that order.
 */
static int
wowner(struct inode *ip)
{
	return (u.u_uid == ip->i_uid || suser());
}

/*
 * THE OWNER FIELD IS A NAME IN 9P AND A NUMBER HERE, and atoi() cannot tell
 * the two apart -- which is how every unparseable owner became ROOT.
 *
 * Measured, over a real Twstat, before this existed: "nobody" set i_uid to 0
 * and the server answered Rwstat; so did "--"; "12x" set it to 12.  atoi has
 * no error return, so `atoi("nobody")' and `atoi("0")' are the same call with
 * the same answer, and the answer is the one identity fio.c:193 lets bypass
 * every permission check on the image.
 *
 * IT IS THE SERVER END OF A CONTRACT THE CLIENT END ALREADY KEPT.  p9uid() at
 * p9cl.c:1005-1040 was given exactly this guard by an earlier audit, and its
 * comment states the rule for the whole port -- root maps to root, and
 * non-root NEVER maps to root.  The two ends of one wire, one hour apart, and
 * only the reading end had it.  Nothing could have caught it: do_wstat had no
 * client caller at all until p9_t_chown, which is this step.
 *
 * WHY A FOREIGN CLIENT IS THE REAL CASE.  Plain 9P2000 specifies this field as
 * a NAME, and statof() acknowledges that and sends a number anyway because
 * there are two passwd files here and no principled way to choose between
 * them.  So u9fs or 9pfuse doing a wstat sends "chris", which is not this
 * server's spelling of anything -- and the honest answer to a name is that
 * this server does not take names, not that the file now belongs to root.
 *
 * RANGE IS NOT PARSEABILITY, and only the second is guarded.  "65536" is
 * accepted and truncates to 0, because that is V7's OWN answer: sys4.c:294 is
 * `ip->i_uid = uap->uid', an int assigned into a short, with no check.  A
 * string V7 could have produced keeps V7's behaviour; a string it could not
 * produce is refused.  A leading '-' is accepted for the same reason -- statof
 * renders i_uid with "%d" of a signed short, so a negative owner is a value
 * this server itself emits.
 */
static int
wuid(const char *s, short *out)
{
	long v = 0;
	int i = 0, neg = 0;

	if (s[0] == '-') { neg = 1; i = 1; }
	if (s[i] == '\0') return (-1);		/* "" and "-" are not numbers */
	for (; s[i]; i++) {
		if (s[i] < '0' || s[i] > '9') return (-1);
		v = v * 10 + (s[i] - '0');
		if (v > 2147483647L) return (-1);	/* wider than chown's int */
	}
	*out = (short)(neg ? -v : v);		/* V7's truncation, deliberately */
	return (0);
}
static void
do_wstat(struct conn *c, p9_u32 tag, struct p9buf *in)
{
	struct p9buf b;
	struct fid *f;
	struct p9stat st;
	struct inode *ip;
	p9_u32 fid;
	int touched = 0;

	fid = p9_g32(in);
	(void)p9_g16(in);		/* the outer size[2] wrapper */
	if (p9_gstat(in, &st) < 0 || !p9_ok(in)) { rerror(c, tag, EINVAL); return; }
	if ((f = fidof(c, fid)) == 0) { rerror(c, tag, EBADF); return; }
	if (mntronly) { rerror(c, tag, EROFS); return; }
	if (st.s_name[0] != '\0') { rerror(c, tag, EPERM); return; }
	ip = f->f_ip;

	/*
	 * PERMISSION FOR A wstat IS THE FILE'S OWN WRITE BIT for a truncation
	 * and wowner() for everything else, which is upstream's owner() and is
	 * argued above the function.  Truncate is a write, so it is the one arm
	 * that asks access() rather than ownership -- V7 has no truncate(2) to
	 * copy a rule from, and shortening a file there is creat(2), which
	 * needs write permission and not ownership.
	 */
	if (st.s_length != (p9_u64)~0ULL) {
		if ((ip->i_mode & IFMT) == IFDIR) { rerror(c, tag, EISDIR); return; }
		u.u_error = 0;
		if (v8k_access(ip, IWRITE)) {
			rerror(c, tag, u.u_error ? u.u_error : EACCES);
			return;
		}
		/*
		 * ONLY TRUNCATION TO ZERO, and the refusal is honest rather
		 * than lazy: V7 has no truncate(2) at all -- shortening a file
		 * is `creat' -- so itrunc is the only mechanism here and it
		 * frees every block.  A wstat to a non-zero length would have
		 * to either grow the file with holes or free a suffix, and
		 * neither has a V7 spelling to be faithful to.
		 */
		if (st.s_length != 0) { rerror(c, tag, EINVAL); return; }
		itrunc(ip);
		touched = 1;
	}
	/*
	 * wowner() AND suser() BOTH RETURN 1 FOR PERMITTED, which is the
	 * OPPOSITE polarity from access() six lines above -- upstream's own
	 * inconsistency, reproduced rather than harmonised, and v8fs.c:468-471
	 * records it beside the function.  Getting it the wrong way round here
	 * would make every wstat succeed for every user.
	 *
	 * THE STICKY BIT IS UPSTREAM'S ONE SPECIAL CASE and it is not decoration:
	 * sys4.c:250-251 is `if (u.u_uid) uap->fmode &= ~ISVTX', so a chmod by
	 * anyone but root cannot set ISVTX.  On a VAX that bit kept a program's
	 * text in the swap area, which is a resource a user could otherwise
	 * consume by asking.  It collapses here for the same reason wowner()
	 * does -- u_uid is 0 -- and it is written out for the same reason too.
	 */
	if (st.s_mode != (p9_u32)~0U) {
		unsigned short nm = (unsigned short)(st.s_mode & 07777);

		if (!wowner(ip)) { rerror(c, tag, EPERM); return; }
		if (u.u_uid) nm &= ~ISVTX;
		ip->i_mode = (ip->i_mode & IFMT) | nm;
		ip->i_flag |= ICHG;
		touched = 1;
	}
	/*
	 * uid AND gid ARE DECIMAL STRINGS ON THE WIRE, because statof writes
	 * them that way -- 9P wants a name, a V7 inode stores a number, and
	 * there are two passwd files here with no principled way to choose.
	 * The empty string is what a client sends for "do not touch", since
	 * all-ones has no spelling in a string field.
	 */
	if (st.s_uid[0] != '\0') {
		short v;

		if (!suser()) { rerror(c, tag, EPERM); return; }
		if (wuid(st.s_uid, &v)) { rerror(c, tag, EINVAL); return; }
		ip->i_uid = v;
		ip->i_flag |= ICHG;
		touched = 1;
	}
	if (st.s_gid[0] != '\0') {
		short v;

		if (!suser()) { rerror(c, tag, EPERM); return; }
		if (wuid(st.s_gid, &v)) { rerror(c, tag, EINVAL); return; }
		ip->i_gid = v;
		ip->i_flag |= ICHG;
		touched = 1;
	}
	/*
	 * THE TWO TIMES ARE ONE ARM, because iupdat is one call.
	 *
	 * utime(2) IS THE ONLY WAY TO SET A TIME TO SOMETHING OTHER THAN NOW,
	 * and it must go through iupdat with explicit ta/tm rather than through
	 * the IACC/IUPD flags alone -- a flag means "stamp it with the clock"
	 * and this is the opposite request.  iupdat writes di_atime when IACC
	 * is set and di_mtime when IUPD is set (iget.c:272-275), and there is no
	 * way to ask it for one of the two; so the field the client did NOT send
	 * is re-written with the value already on the disk, which is a no-op
	 * write rather than a silent change.  itimes() is the read half and was
	 * already here for Tstat.
	 *
	 * ICHG GOES WITH THEM, and this arm used not to set it.  sys4.c:536 is
	 * `ip->i_flag |= IACC|IUPD|ICHG' -- all three -- and the comment four
	 * lines above it, "Can't set ICHG", means the CALLER cannot choose a
	 * ctime, not that ctime stays put: iupdat stamps it from the clock.  So
	 * a V7 utime moves ctime and this arm did not.  Invisible over the wire,
	 * because 9P's stat has no third time and p9tostat reports ctime as
	 * mtime, and visible in the image to anything that reads di_ctime.
	 *
	 * waitfor IS 1 HERE AND 0 UPSTREAM (sys4.c:537).  Kept, and the
	 * difference is a write-scheduling hint rather than a semantic: bwrite
	 * puts the block out now, bdwrite marks it delayed and the poll loop's
	 * update() flushes it before the next message is read.  The one thing
	 * bwrite can do that bdwrite cannot is report an I/O error to this
	 * caller, and there is no path here that could act on one.
	 */
	if (st.s_atime != (p9_u32)~0U || st.s_mtime != (p9_u32)~0U) {
		p9_u32 oat, omt;
		time_t at, mt;

		if (!wowner(ip)) { rerror(c, tag, EPERM); return; }
		itimes(ip, &oat, &omt);
		at = (time_t)(st.s_atime != (p9_u32)~0U ? st.s_atime : oat);
		mt = (time_t)(st.s_mtime != (p9_u32)~0U ? st.s_mtime : omt);
		ip->i_flag |= IACC | IUPD | ICHG;
		iupdat(ip, &at, &mt, 1);
		touched = 1;
	}

	/*
	 * A wstat THAT TOUCHED NOTHING IS NOT AN ERROR -- 9P uses an all-ones
	 * stat as a "sync me" request, and this server has nothing per-file to
	 * sync because the poll loop calls update() after every message.  The
	 * variable exists so that the arms above have somewhere to record the
	 * fact and so that adding an arm which forgets is visible; it is read
	 * here rather than being dropped, because a write-only variable is the
	 * unconsumed-component rule applied to a line.
	 */
	if (verbose && !touched)
		fprintf(stderr, "v8fsd: Twstat touched nothing (a sync)\n");

	p9_hdr(&b, txbuf, sizeof txbuf, P9_Rwstat, tag);
	reply(c, &b);
}

/*
 * Taccess -- the second extension, and p9.h has the argument: 9P has no
 * access(2) because Plan 9 has none, and V7 does.  The client used to compute
 * an answer from the mode bits and got it wrong on every file of every image,
 * because the bits were the server's and the identity was the host's.
 *
 * THIS IS FOUR LINES BECAUSE ALL THE WORK IS BELL LABS'.  access() is the
 * function with the root bypass, the group shift and the s_ronly arm; the only
 * thing this adds is the loop saccess() writes out three times, and the
 * observation that a fid already IS the inode a path would have resolved to.
 */
static void
do_access(struct conn *c, p9_u32 tag, struct p9buf *in)
{
	struct p9buf b;
	struct fid *f;
	p9_u32 fid;
	int mode;

	fid = p9_g32(in);
	mode = (int)p9_g8(in);
	if (!p9_ok(in)) { rerror(c, tag, EINVAL); return; }
	if ((f = fidof(c, fid)) == 0) { rerror(c, tag, EBADF); return; }

	u.u_error = 0;
	if (mode & P9_AREAD) (void)v8k_access(f->f_ip, IREAD);
	if (mode & P9_AWRITE) (void)v8k_access(f->f_ip, IWRITE);
	if (mode & P9_AEXEC) (void)v8k_access(f->f_ip, IEXEC);
	if (u.u_error) { rerror(c, tag, u.u_error); return; }

	p9_hdr(&b, txbuf, sizeof txbuf, P9_Raccess, tag);
	reply(c, &b);
}

/*
 * Tseek -- the extension, and p9.h says at length why it exists.  In one line:
 * lseek(2) moves the OPEN FILE DESCRIPTION's offset, this connection is that
 * description, and 9P has no message for it because Plan 9's kernel held it.
 *
 * THE LENGTH FOR SEEK_END IS THE ONE THING THAT IS NOT ARITHMETIC, and it is
 * two different numbers.  For a file it is i_size.  For a DIRECTORY it is the
 * length of the generated 9P listing, not i_size -- the two are unrelated, for
 * exactly the reason dir.c:114 records about the host (a run of records built
 * from entries is not the size the filesystem charges).  So the listing has to
 * exist before the question can be answered, and gendir is what makes it.
 */
static void
do_seek(struct conn *c, p9_u32 tag, struct p9buf *in)
{
	struct p9buf b;
	struct fid *f;
	p9_u32 fid;
	p9_u64 off, base;
	int whence;
	long long np;

	fid = p9_g32(in);
	off = p9_g64(in);
	whence = (int)p9_g8(in);
	if (!p9_ok(in)) { rerror(c, tag, EINVAL); return; }
	if ((f = fidof(c, fid)) == 0) { rerror(c, tag, EBADF); return; }
	if (f->f_omode < 0) { rerror(c, tag, EBADF); return; }

	switch (whence) {
	case P9_SEEKSET:
		base = 0;
		break;
	case P9_SEEKCUR:
		base = f->f_off;
		break;
	case P9_SEEKEND:
		if ((f->f_ip->i_mode & IFMT) == IFDIR) {
			if (f->f_dir == 0 && gendir(f) < 0) {
				rerror(c, tag, u.u_error ? u.u_error : EIO);
				return;
			}
			base = (p9_u64)f->f_dirlen;
		} else
			base = (p9_u64)(unsigned long)f->f_ip->i_size;
		break;
	default:
		rerror(c, tag, EINVAL);
		return;
	}

	/*
	 * `off' IS SIGNED ON THE V7 SIDE AND UNSIGNED ON THE WIRE, which is
	 * the shape that produced this server's one remote crash: a p9_u64 in
	 * a signed comparison.  lseek(fd, -4, 1) is legal and must work, so
	 * the value is reinterpreted rather than range-checked, and the SUM is
	 * what gets tested -- in a signed type wide enough to hold it, before
	 * anything indexes with it.  A negative result is EINVAL, which is
	 * lseek(2)'s own answer.
	 */
	/*
	 * THE CHECK IS BEFORE THE ADDITION, AND THE FIRST VERSION WAS AFTER IT.
	 * `np = base + off; if (np < 0)' reasons correctly about reinterpreting
	 * the unsigned wire offset and then performs the addition in a type
	 * that cannot hold every reachable sum -- signed overflow, which is
	 * undefined and which an ordinary V8 lseek can reach: measured,
	 * lseek(2^62, SEEK_SET) then lseek(2^62, SEEK_CUR) wrapped to
	 * LLONG_MIN and was rejected for the wrong reason.  Found by an
	 * auditor, in the guard written to prevent exactly this class.
	 */
	{
		long long d = (long long)(p9_u64)off;
		long long b0 = (long long)base;

		if (d < 0 ? b0 < -d : d > 0x7fffffffffffffffLL - b0) {
			rerror(c, tag, EINVAL);
			return;
		}
		np = b0 + d;
	}
	f->f_off = (p9_u64)np;

	p9_hdr(&b, txbuf, sizeof txbuf, P9_Rseek, tag);
	p9_p64(&b, f->f_off);
	reply(c, &b);
}

static void
do_clunk(struct conn *c, p9_u32 tag, struct p9buf *in)
{
	struct p9buf b;
	struct fid *f;
	p9_u32 fid = p9_g32(in);

	if (!p9_ok(in)) { rerror(c, tag, EINVAL); return; }
	if ((f = fidof(c, fid)) == 0) { rerror(c, tag, EBADF); return; }
	fidfree(f);
	p9_hdr(&b, txbuf, sizeof txbuf, P9_Rclunk, tag);
	reply(c, &b);
}

static void
do_stat(struct conn *c, p9_u32 tag, struct p9buf *in)
{
	struct p9buf b;
	struct fid *f;
	struct p9stat st;
	p9_u32 fid = p9_g32(in);

	if (!p9_ok(in)) { rerror(c, tag, EINVAL); return; }
	if ((f = fidof(c, fid)) == 0) { rerror(c, tag, EBADF); return; }

	statof(f->f_ip, f->f_name, &st);
	p9_hdr(&b, txbuf, sizeof txbuf, P9_Rstat, tag);
	/*
	 * THE OUTER COUNT is 9P2000's one real wart -- stat[n] whose n bytes
	 * open with the structure's own size[2] -- and it used to be eight
	 * hand-rolled lines here.  It moved into p9_pstatw() when the client
	 * grew a Twstat and would otherwise have written the second copy.
	 */
	p9_pstatw(&b, &st);
	if (!p9_ok(&b)) { rerror(c, tag, EIO); return; }
	reply(c, &b);
}

/*
 * One message, start to finish.  Returns 0 to keep the connection and -1 to
 * drop it.
 */
static int
serve1(struct conn *c)
{
	struct p9buf in;
	long n;
	p9_u32 tag;
	int type;

	n = p9_recv(c->c_fd, rxbuf, (long)sizeof rxbuf);
	if (n == 0) return (-1);		/* the client went away */
	if (n < 0) {
		if (verbose) fprintf(stderr, "v8fsd: bad frame, dropping\n");
		return (-1);
	}
	/*
	 * p9_recv leaves the size in the buffer -- it read it to know how much
	 * to expect -- so the cursor has to step over it here.  (The comment
	 * that used to be on this line said the size was "already consumed by
	 * p9_recv", which would have made this skip an off-by-four.)
	 */
	p9_init(&in, rxbuf, n);
	(void)p9_g32(&in);
	type = (int)p9_g8(&in);
	tag = p9_g16(&in);
	if (!p9_ok(&in)) return (-1);
	if (verbose) fprintf(stderr, "v8fsd: T%d tag %u len %ld\n", type, tag, n);

	/*
	 * THE CLOCK TICK, AND ONCE PER REQUEST IS THE RIGHT GRAIN RATHER THAN A
	 * COMPROMISE.  A V8 kernel advances `time' from a clock interrupt and
	 * every syscall therefore sees a fresh value; here a request IS the
	 * syscall, and nothing can run between two of them.  Sampling it any
	 * more often would be measuring a clock that cannot have moved.
	 *
	 * IT IS ON THE READ PATH TOO, DELIBERATELY, because a read stamps an
	 * atime -- rdwri.c:50 sets IACC and iput runs IUPDAT.  A tick that
	 * covered only the write messages would leave every atime on the
	 * mount reading as the moment mkfs made the image, which is the exact
	 * wrong answer this step exists to find and fix.
	 */
	v8fs_clock();

	switch (type) {
	case P9_Tversion:	do_version(c, tag, &in); break;
	case P9_Tattach:	do_attach(c, tag, &in); break;
	case P9_Twalk:		do_walk(c, tag, &in); break;
	case P9_Topen:		do_open(c, tag, &in); break;
	case P9_Tread:		do_read(c, tag, &in); break;
	case P9_Tclunk:		do_clunk(c, tag, &in); break;
	case P9_Tseek:		do_seek(c, tag, &in); break;
	case P9_Tstat:		do_stat(c, tag, &in); break;

	/*
	 * Tflush has nothing to flush: every request is carried to completion
	 * before the next is read, so by the time a Tflush is seen the message
	 * it refers to has already been answered.  Rflush is the correct reply
	 * and it is not a stub -- it is what a synchronous server means.
	 */
	case P9_Tflush: {
		struct p9buf b;
		(void)p9_g16(&in);
		p9_hdr(&b, txbuf, sizeof txbuf, P9_Rflush, tag);
		reply(c, &b);
		break;
	}

	/* the write half -- §8a step 5f; the header comment says what changed */
	case P9_Twrite:		do_write(c, tag, &in); break;
	case P9_Tcreate:	do_create(c, tag, &in); break;
	case P9_Tremove:	do_remove(c, tag, &in); break;
	case P9_Twstat:		do_wstat(c, tag, &in); break;
	case P9_Taccess:	do_access(c, tag, &in); break;
	case P9_Tauth:
		rerror(c, tag, EPERM);
		break;
	default:
		rerror(c, tag, EINVAL);
		break;
	}
	return (c->c_fd < 0 ? -1 : 0);
}

/* ------------------------------------------------------------------ main */

static void
usage(void)
{
	fprintf(stderr, "usage: v8fsd [-v] [-r] socket image\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	struct sockaddr_un sa;
	struct pollfd pfd[NCONN + 1];
	const char *sockpath, *image;
	int lfd, imgfd, i, np, k;
	dev_t dev;

	while (argc > 1 && argv[1][0] == '-' && argv[1][1] != '\0') {
		if (strcmp(argv[1], "-v") == 0) verbose = 1;
		else if (strcmp(argv[1], "-r") == 0) mntronly = 1;
		else usage();
		argc--; argv++;
	}
	if (argc != 3) usage();
	sockpath = argv[1];
	image = argv[2];

	if (strlen(sockpath) >= sizeof sa.sun_path) {
		fprintf(stderr, "v8fsd: socket path too long (max %d)\n",
		    (int)sizeof sa.sun_path - 1);
		return (2);
	}

	/*
	 * THE OPEN MODE FOLLOWS THE MOUNT FLAG, and O_RDONLY under -r is a
	 * SECOND guarantee rather than the same one twice.  s_ronly stops the
	 * kernel from generating a write; this stops the host from performing
	 * one.  Either alone would do on correct code, and the reason to have
	 * both is that the failure they guard against is different: a bug in
	 * the kernel path defeats s_ronly and cannot defeat O_RDONLY.
	 *
	 * IT IS NOT WHAT WAS PROTECTING THE IMAGE BEFORE 5f, though it looked
	 * like it.  Measured with an instrumented driver: O_RDWR with no
	 * bflush() produced ZERO pwrites across a small read, a 28000-byte read
	 * and twenty more reads -- the atime bdwrite sat in the buffer cache
	 * and nothing ever recycled it.  The old EROFS arm and the old
	 * O_RDONLY were both belt to a brace that was doing all the work by
	 * accident.
	 */
	if ((imgfd = open(image, mntronly ? O_RDONLY : O_RDWR)) < 0) {
		fprintf(stderr, "v8fsd: cannot open %s\n", image);
		return (2);
	}
	if (v8k_imgattach(imgfd) < 0) {
		fprintf(stderr, "v8fsd: cannot register the image driver\n");
		return (2);
	}
	/*
	 * MINOR 0, AND THE MINOR NUMBER IS THE BLOCK SIZE.  param.h defines
	 * BITFS(dev) as `dev & 64', so bit 6 selects a 4096-byte filesystem
	 * over a 1024-byte one.  mkfs writes 1024-byte blocks here, so the
	 * minor must keep that bit clear or every BSIZE/BMASK/itod in the
	 * kernel would describe a different disk.
	 *
	 * AND THE PACKING IS DONE IN imgdev.c, NOT HERE, BECAUSE makedev IN
	 * THIS FILE IS THE HOST'S.  shim/kern/h/param.h defines makedev, major
	 * and minor and warns that they "must not be replaced by the host's
	 * <sys/types.h> versions, which unpack Darwin's 32-bit dev_t at a
	 * different shift" -- and <sys/socket.h> above pulls in <sys/types.h>,
	 * which redefines all three with no -Wmacro-redefined, because the
	 * redefinition is inside a system header.  V8 shifts the major by 8 and
	 * Darwin by 24, and dev_t here is a u_short, so `makedev(i, 0)' in this
	 * translation unit is ZERO for every i.
	 *
	 * Latent rather than live -- imgdev is the first driver registered, so
	 * i is 0 and both packings agree -- and structurally removed rather
	 * than guarded: imgdev.c includes no host header, so it still has V8's
	 * macros, and it is the only place that knows the number anyway.  Found
	 * by a review subagent, and it is exactly the trap param.h claims a
	 * guard for, arriving through the three names it does not claim.
	 */
	dev = v8k_imgdev();
	if (v8k_kinit(dev, mntronly) < 0) {
		fprintf(stderr, "v8fsd: %s is not a V8 filesystem\n", image);
		return (2);
	}

	/*
	 * SIGPIPE would kill the server the first time a client vanished
	 * between its request and our reply, which is an ordinary event -- a
	 * V8 program exiting closes every descriptor it holds.  Ignored, so
	 * the write fails with EPIPE and connclose runs.
	 */
	signal(SIGPIPE, SIG_IGN);

	unlink(sockpath);
	if ((lfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		perror("v8fsd: socket");
		return (2);
	}
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, sockpath);
	if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		perror("v8fsd: bind");
		return (2);
	}
	if (listen(lfd, 16) < 0) {
		perror("v8fsd: listen");
		return (2);
	}
	for (i = 0; i < NCONN; i++) conns[i].c_fd = -1;

	/*
	 * READY, AND SAID SO ON STDOUT.  A test that starts this server has to
	 * know when the socket will accept, and the alternatives are both bad:
	 * sleeping is a race dressed as a delay, and retrying a connect() in a
	 * loop cannot tell "not yet" from "died on the image".  One line, and
	 * then a FLUSH -- which is all it is.  This sentence used to say the
	 * flush closed the stream and released a reader blocked on it; fflush
	 * does no such thing, and a test that read to EOF on the strength of it
	 * would hang.  tests/streams polls with `grep -q', which is what the
	 * flush actually supports.
	 */
	printf("v8fsd ready %s%s\n", sockpath, mntronly ? " ro" : "");
	fflush(stdout);

	for (;;) {
		np = 0;
		pfd[np].fd = lfd;
		pfd[np].events = POLLIN;
		np++;
		for (i = 0; i < NCONN; i++) {
			if (conns[i].c_fd < 0) continue;
			pfd[np].fd = conns[i].c_fd;
			pfd[np].events = POLLIN;
			pfd[np].revents = 0;
			np++;
		}
		if (poll(pfd, (nfds_t)np, -1) < 0) {
			if (errno == EINTR) continue;
			perror("v8fsd: poll");
			break;
		}
		if (pfd[0].revents & POLLIN) {
			int nfd = accept(lfd, NULL, NULL);
			if (nfd >= 0) {
				for (i = 0; i < NCONN; i++)
					if (conns[i].c_fd < 0) break;
				if (i == NCONN) close(nfd);
				else {
					struct timeval tv;

					conns[i].c_fd = nfd;
					conns[i].c_msize = P9_MSIZE;
					for (k = 0; k < NFID; k++)
						conns[i].c_fid[k].f_fid = P9_NOFID;
					/*
					 * A DEADLINE THAT APPLIES ONLY IN THE
					 * MIDDLE OF A MESSAGE, and the two
					 * halves of that are what make it
					 * correct rather than merely present.
					 *
					 * A connection here is an OPEN FILE
					 * and may sit idle for as long as the
					 * program holds it, so it must never
					 * be timed out for saying nothing.
					 * poll() is what protects that: an
					 * idle connection is never read.  Once
					 * poll reports data the first read
					 * cannot block, so this timeout can
					 * only fire part-way through a message
					 * -- which means the peer died between
					 * two writes, and a single-threaded
					 * server that waited for the rest
					 * would be wedged by one dead client.
					 */
					tv.tv_sec = 30;
					tv.tv_usec = 0;
					setsockopt(nfd, SOL_SOCKET, SO_RCVTIMEO,
					    &tv, sizeof tv);
					/*
					 * AND THE SEND SIDE, WHICH THE
					 * PARAGRAPH ABOVE ARGUED AT LENGTH AND
					 * THEN DID NOT DO.  It is the same
					 * hazard in the other direction and it
					 * needs no malice: the socket buffer
					 * here is 8192 bytes and an Rread reply
					 * is up to 8203, so ONE client that
					 * stops reading blocks this server in
					 * write(2) for every other client.
					 * Measured by a review subagent -- a
					 * victim's latency went from 0.0000s
					 * to a 5-second timeout while another
					 * client pipelined reads and drained
					 * nothing, and recovered the instant
					 * it did.
					 */
					setsockopt(nfd, SOL_SOCKET, SO_SNDTIMEO,
					    &tv, sizeof tv);
				}
			}
		}
		for (k = 1; k < np; k++) {
			struct conn *c = 0;

			if (!(pfd[k].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			for (i = 0; i < NCONN; i++)
				if (conns[i].c_fd == pfd[k].fd) { c = &conns[i]; break; }
			if (c == 0) continue;
			if (serve1(c) < 0) connclose(c);
		}

		/*
		 * sync(2), ONCE PER WAKEUP, AND IT IS THE LINE THAT MAKES THE
		 * WHOLE STEP REAL.  writei and iupdat mostly bdwrite: the
		 * buffer is marked B_DELWRI and the write happens when the
		 * cache recycles it, which on a 2000-block image with NBUF
		 * buffers is never.  Measured before this line existed --
		 * O_RDWR, twenty-two reads and a create, ZERO pwrites.
		 *
		 * update() is alloc.c:486, which is the body of sync(2), and
		 * upstream's sched() runs it every thirty seconds.  Here it is
		 * per wakeup rather than on a timer, for two reasons that are
		 * both about this being a server rather than a kernel: there
		 * is no clock interrupt to hang a timer on, and a client that
		 * writes a file and exits must find the bytes on the disk,
		 * because the NEXT thing to open the image is usually fsck.
		 *
		 * AFTER THE CONNECTION LOOP RATHER THAN INSIDE serve1, because
		 * connclose -> fidfree -> iput is where the last IUPDAT of a
		 * file happens -- a flush inside serve1 runs BEFORE that and
		 * leaves the final inode write in the cache.  That is not a
		 * refinement: it is the difference between the measurement
		 * above reading two writes and reading none.
		 *
		 * SKIPPED ENTIRELY ON A READ-ONLY MOUNT, where update() would
		 * be a no-op it still walks NINODE inodes to discover -- and
		 * where s_ronly has already stopped anything from being dirty.
		 */
		if (!mntronly)
			update();
	}
	return (1);
}
