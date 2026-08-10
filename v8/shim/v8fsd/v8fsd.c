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
 * READ ONLY, IN THIS STEP.  Twrite, Tcreate, Tremove and Twstat answer
 * Rerror; the kernel underneath them is written and tested (§8a step 5d did
 * writei, the allocating arm of bmap, alloc/free, ialloc/ifree, itrunc and
 * namei with NI_CREAT/NI_DEL), so this is a boundary in the server rather than
 * a gap in the port.  Splitting it the same way step 5c and 5d were split
 * keeps the unit reviewable and keeps a failure attributable.
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
 */
int		readi(), iput(), schar();
int		iinit(), binit(), bhinit(), ihinit(), update(), brelse();
int		v8k_access();
int		v8k_kinit(dev_t dev);

/*
 * rootdev is systm.h's -- a K&R tentative definition, so every object that
 * includes that header emits a common and the linker merges them.  Declared
 * extern here rather than including systm.h, which would bring in the three
 * warnings CLAUDE.md records for it (a caddr_t calloc() against the builtin,
 * and two tentative arrays) for the sake of one dev_t.
 */
extern dev_t	rootdev;

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
};

struct conn {
	int		c_fd;		/* -1 when the slot is free */
	long		c_msize;
	struct fid	c_fid[NFID];
};

static struct conn	conns[NCONN];
static unsigned char	rxbuf[P9_MSIZE], txbuf[P9_MSIZE];
static int		verbose;

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
	if (mode & (P9_OTRUNC | P9_ORCLOSE)) { rerror(c, tag, EROFS); return; }
	if (want & IWRITE) { rerror(c, tag, EROFS); return; }

	u.u_error = 0;
	if (v8k_access(f->f_ip, want)) {
		rerror(c, tag, u.u_error ? u.u_error : EACCES);
		return;
	}
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
	unsigned char *outer;
	long n;

	if (!p9_ok(in)) { rerror(c, tag, EINVAL); return; }
	if ((f = fidof(c, fid)) == 0) { rerror(c, tag, EBADF); return; }

	statof(f->f_ip, f->f_name, &st);
	p9_hdr(&b, txbuf, sizeof txbuf, P9_Rstat, tag);
	/*
	 * THE OUTER COUNT, which is 9P2000's one real wart: the message field
	 * is stat[n] and the bytes inside it start with the structure's own
	 * size[2].  The two differ by exactly two.  p9_pstat writes the inner
	 * one; this writes the outer, and it is patched afterwards for the
	 * same reason the inner one is.
	 */
	outer = b.b_p;
	p9_p16(&b, 0);
	p9_pstat(&b, &st);
	if (!p9_ok(&b)) { rerror(c, tag, EIO); return; }
	n = (b.b_p - outer) - 2;
	outer[0] = (unsigned char)(n & 0xff);
	outer[1] = (unsigned char)((n >> 8) & 0xff);
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

	/* the write half -- §8a step 5f; the header comment says why */
	case P9_Twrite: case P9_Tcreate: case P9_Tremove: case P9_Twstat:
		rerror(c, tag, EROFS);
		break;
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
	fprintf(stderr, "usage: v8fsd [-v] socket image\n");
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

	if ((imgfd = open(image, O_RDONLY)) < 0) {
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
	if (v8k_kinit(dev) < 0) {
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
	printf("v8fsd ready %s\n", sockpath);
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
	}
	return (1);
}
