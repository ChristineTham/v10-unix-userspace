/*
 * V7 directory emulation.
 *
 * THE PROBLEM.  In V8 a directory is an ordinary file, and programs read it
 * with read(2), expecting a stream of 16-byte records: a 2-byte inode number
 * followed by a 14-byte name.  44 of the commands in the tree do this directly,
 * and V8's own readdir() is itself a shim over the same format (its source even
 * says so: "read an old stlye directory entry and present it as a new one").
 * macOS refuses read(2) on a directory descriptor outright.
 *
 * THE FIX.  open() notices it was handed a directory and registers a shim
 * entry: the host directory is snapshotted through fdopendir/readdir into a
 * buffer of synthetic V7 records, and subsequent read() calls on that
 * descriptor are served from the buffer.  Nothing above the seam changes -- not
 * V8's readdir(), not the 44 raw readers.
 *
 * WHAT IS LOST, deliberately and documented:
 *
 *   Names longer than 14 bytes are truncated, because the record has nowhere
 *   to put them.  That is the authentic V8 limit, not an artefact: a V8 program
 *   could not have seen a longer name either.  Truncation can alias two entries
 *   onto one name; that is the same collision a real V7 filesystem would have
 *   had, and it is why the V8 world should be given short filenames.
 *
 *   Inode numbers are 16 bits, so a 64-bit host inode has to be mapped down.
 *   That map is v8sys_fold_ino() below, and it is injective for the first
 *   65535 inodes a process sees -- which it has to be, because V7 reads an
 *   inode number as an identity and not as a label.  Never 0, which V7 uses
 *   to mean "empty slot" and which every reader skips.
 *
 * The snapshot is taken at open() time, so a directory changing underneath a
 * scan is not seen.  V7 had no such guarantee either, but for the opposite
 * reason: it saw writes immediately.  In practice V8 programs open, scan and
 * close in one breath.
 */

#include "v8sys.h"
#include "rawsys.h"

/* No libc: see rawsys.h.  These are the few pieces we would have borrowed. */
extern char *v8sys_alloc(long);
extern void  v8sys_free(char *);
extern unsigned long long v8sys_host_ino_at(int, const char *);

static void
bcopy_(char *d, char *s, long n)
{ while (n-- > 0) *d++ = *s++; }

static void
bzero_(char *d, long n)
{ while (n-- > 0) *d++ = 0; }

static long
slen_(char *s)
{ char *p = s; while (*p) p++; return (p - s); }

#define MAXDIRFD 256

struct dirshim {
	int		fd;		/* -1 when the slot is free */
	char		*recs;		/* synthetic V7 records */
	long		nbytes;
	long		pos;
};

static struct dirshim shims[MAXDIRFD];

void v8sys_dirinit(void);

static struct dirshim *
find(int fd)
{
	int i;

	/*
	 * Initialise here as well as from open().
	 *
	 * The free-slot sentinel is -1 because fd 0 is a perfectly good
	 * descriptor, and the table only got that sentinel written into it by
	 * v8sys_dirinit(), which open() calls.  A program that never opens
	 * anything -- `cat` with no arguments, reading stdin -- therefore saw a
	 * table still zeroed from bss, where slot 0 reads as fd 0.  isdirfd(0)
	 * came back true, every read from stdin was served from an empty
	 * directory snapshot, and cat printed nothing.
	 */
	v8sys_dirinit();
	for (i = 0; i < MAXDIRFD; i++)
		if (shims[i].fd == fd) return (&shims[i]);
	return (0);
}

int
v8sys_isdirfd(int fd)
{
	return (find(fd) != 0);
}

/*
 * How many bytes read(2) will produce for this directory, which is NOT what the
 * host says its size is.
 *
 * The snapshot is a run of 256-byte V7 records built from variable-length host
 * entries, so the two numbers are unrelated: /etc has nine entries, 2304 bytes
 * of records, and an APFS st_size of 288.  Every reader that loops until EOF
 * never noticed.  ps(1) does not loop -- getdir() sizes its array as
 * st_size/sizeof(struct direct) and then insists read(fd, dp, st_size) return
 * exactly st_size, so it read 288 bytes of a 2304-byte directory and called
 * that the answer.
 *
 * -1 for a descriptor that is not a directory, so the caller can tell "no
 * opinion" from "genuinely empty".
 */
long
v8sys_dirsize(int fd)
{
	struct dirshim *s = find(fd);

	return (s == 0 ? -1L : s->nbytes);
}

/*
 * IDENTITY, NOT JUST WIDTH.
 *
 * 64 bits do not fit in 16, so some map is needed; the question this function
 * answers is which one.  It used to be a pure fold --
 *
 *	ino ^ ino>>16 ^ ino>>32 ^ ino>>48
 *
 * -- which is stable, cheap, and wrong, because V7's idioms read an inode
 * number as an identity rather than as a label.  getwd(3) walks to the root by
 * looking for the entry in `..' whose d_ino equals stat(".")'s st_ino
 * (getwd.c:62), so two entries of one directory sharing a fold make pwd(1)
 * print another directory's path and exit 0.  Measured on a months-old
 * $TMPDIR: 121 of 1545 directories sat in a collision group, and inside those
 * groups pwd was right 32 times in 60, against 60 of 60 outside.  ttyname(3)
 * has the same idiom in its careful form -- pre-filter on d_ino, then confirm
 * with stat() -- and is defeated identically, because the confirming stat
 * returns the folded number too.  Nothing above the seam can separate two
 * files the shim has already mapped onto one (dev, ino).
 *
 * SO THE MAP HAS TO BE INJECTIVE, AND NO PURE FUNCTION OF THE INODE IS.  What
 * replaces it is a process-local table: the fold proposes a number, and if
 * that number is already spoken for by a different host inode the next free
 * one is taken instead.  Three properties, and the third is what it costs:
 *
 *   Stable.  The table is append-only -- an assignment is never revised and
 *   never evicted -- so `ls -i' twice in one process agrees, and so does the
 *   stat(".") that getwd takes BEFORE it opens `..'.  That ordering is what
 *   killed the cheaper fix of disambiguating inside the directory snapshot:
 *   a snapshot cannot perturb a value its caller already holds.
 *
 *   Unchanged where it can be.  A host inode whose fold is free gets exactly
 *   the fold, so entries that never collide -- 96.5% of $TMPDIR here -- print
 *   the number they printed before this existed, and print it in every
 *   process.
 *
 *   Process-local for the rest.  Which of two colliding inodes gets the fold
 *   depends on which was seen first, so two processes can disagree about a
 *   colliding inode (215 of 6031 entries here).  That is a real departure and
 *   it is the price of the fix: the alternative is a rule over the set of
 *   inodes in a directory, and stat(2) does not know the directory.
 *
 * WHEN THE TABLE CANNOT ANSWER -- 65535 distinct inodes seen, or mmap refused
 * -- the fold is returned unrecorded.  That is exactly the old behaviour, and
 * it is still stable, because the fold is a pure function.  Degradation is
 * graceful rather than incoherent.
 *
 * AND IT IS NO LONGER A PURE FUNCTION, WHICH COSTS ASYNC-SIGNAL SAFETY.  The
 * old fold could be called from anywhere; this one has a read-modify-write
 * window from the inofind() below to the assignment at the end of it.  A
 * signal arriving inside that window whose handler reaches stat(2) or
 * opendir(3) re-enters, and the outer call's assignment is then either written
 * into a table inogrow() has already orphaned or overwritten by the handler's
 * -- and the affected inode answers differently next time, which is precisely
 * the property this exists to guarantee.  There is no live caller: every
 * signal() with a real handler in src/cmd was read, and the closest are
 * sort.c's `term' and cc.c's `idexit' (unlink only), fsck.c's `catch'
 * (ckfini), dumpoptr.c's `alarmcatch' (prints) and sh/fault.c:83 (sets a
 * flag).  None stats or reads a directory.  So this is a property change with
 * no known trigger rather than a bug, and it is written down because the
 * paragraphs above argue the stability property at length and this is the one
 * way it can be lost.
 *
 * NOT KEYED ON THE DEVICE, deliberately.  The old fold was not either, and the
 * caller in this file has no device to offer: getdirentries64 reports d_ino
 * and no d_dev.  Two files with the same host inode on different devices
 * therefore still share a v7 inode, which is correct -- V7 identity is the
 * pair, and stat_translate reports st_dev separately.  The synthetic
 * filesystems assign their own numbers outside this map for the same reason
 * (/proc's ROOTINO 2 at libkmemu/procfs.c:887, /dev/fd's minor+1 at vfs.c).
 */
#define INOSPACE	65536		/* the whole u_short range */
#define INOWORDS	(INOSPACE / 32)

struct inoslot {
	unsigned long long	host;
	unsigned short		v7;	/* 0 means the slot is empty */
};

static unsigned int	*inoclaim;	/* bit v set: v7 inode v is taken */
static long		 inonclaim;
static struct inoslot	*inomap;	/* host inode -> v7 inode */
static long		 inosize;	/* a power of two; 0 until allocated */
static long		 inoused;

static unsigned short
inofold(unsigned long long ino)
{
	unsigned short v;

	v = (unsigned short)(ino ^ (ino >> 16) ^ (ino >> 32) ^ (ino >> 48));
	return (v ? v : (unsigned short)1);
}

/*
 * Murmur3's 64-bit finaliser.  The fold cannot double as the table's hash --
 * it is the value being handed out, and the whole problem is that it clusters
 * -- so the table needs its own, and the probe stride below needs a second,
 * independent opinion about the same key.
 */
static unsigned long long
inohash(unsigned long long x)
{
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33;
	x *= 0xc4ceb9fe1a85ec53ULL;
	x ^= x >> 33;
	return (x);
}

/* The slot holding `host', or the one it would go in.  0 if the table is full. */
static struct inoslot *
inofind(unsigned long long host)
{
	long mask = inosize - 1;
	long i = (long)(inohash(host) & (unsigned long long)mask);
	long n;

	for (n = 0; n < inosize; n++) {
		if (inomap[i].v7 == 0 || inomap[i].host == host)
			return (&inomap[i]);
		i = (i + 1) & mask;
	}
	return (0);
}

/*
 * Double the table.  The v7 values do not move, so every assignment already
 * made survives -- which is the stability property, and the reason growth is
 * safe to do in the middle of a directory scan.  The old table is leaked into
 * the pool, because v8sys_free() is a no-op; the total leak is bounded by the
 * final size and the pool goes back to the kernel at exit.
 */
static int
inogrow(void)
{
	long newsize = inosize ? inosize * 2 : 512;
	struct inoslot *old = inomap, *nw;
	long oldsize = inosize, i;

	nw = (struct inoslot *)v8sys_alloc(newsize * (long)sizeof *nw);
	if (nw == 0) return (-1);
	bzero_((char *)nw, newsize * (long)sizeof *nw);
	inomap = nw;
	inosize = newsize;
	for (i = 0; i < oldsize; i++)
		if (old[i].v7) {
			struct inoslot *s = inofind(old[i].host);
			/*
			 * Cannot be null: the new table is twice the old and
			 * takes at most oldsize entries.  Tested anyway, because
			 * that proof is a property of the growth FACTOR and
			 * nothing else here states it -- change the 2, or add
			 * tombstones, and this becomes a null store whose only
			 * other failure path is the allocation above.
			 *
			 * MEASURED UNREACHABLE, not assumed: deleting this line
			 * leaves every suite green (mutation M6), which is the
			 * evidence for the paragraph above rather than a gap in
			 * the tests.  Nothing can reach it until the factor
			 * changes, and then it is the difference between a
			 * refused rehash and a null store.
			 */
			if (s == 0) continue;
			s->host = old[i].host;
			s->v7   = old[i].v7;
		}
	v8sys_free((char *)old);
	return (0);
}

static int
inoinit(void)
{
	long n = INOWORDS * (long)sizeof *inoclaim;

	if ((inoclaim = (unsigned int *)v8sys_alloc(n)) == 0)
		return (-1);
	bzero_((char *)inoclaim, n);
	inoclaim[0] = 1;	/* value 0 is V7's empty slot: never handed out */
	inonclaim = 1;
	return (0);
}

#define TAKEN(v)	(inoclaim[(v) >> 5] & (1U << ((v) & 31)))

v8_ino_t
v8sys_fold_ino(unsigned long long ino)
{
	unsigned short v = inofold(ino);
	struct inoslot *s;
	unsigned long long step;
	long n;

	if (inoclaim == 0 && inoinit() < 0)
		return ((v8_ino_t)v);		/* no table: the old answer */
	if (inoused * 2 >= inosize)
		(void)inogrow();		/* if it fails we run hotter */
	if ((s = inofind(ino)) == 0)
		return ((v8_ino_t)v);		/* full and could not grow */
	if (s->v7)
		return ((v8_ino_t)s->v7);	/* seen before: the same answer */
	if (inonclaim >= INOSPACE)
		return ((v8_ino_t)v);		/* every value is spoken for */

	if (TAKEN(v)) {
		/*
		 * The stride is odd and the space is a power of two, so
		 * v + k*step visits all 65536 values; bit 0 is claimed at init
		 * so 0 is never among the free ones; and the count above says
		 * at least one value IS free.  The loop therefore always finds
		 * one, and the test after it cannot fire -- it is here so that
		 * being wrong about that costs a duplicate rather than a
		 * silent aliasing of two files onto one identity.
		 *
		 * THE COUNT CHECK AND THAT TEST ARE MUTUALLY REDUNDANT ON
		 * CORRECTNESS, and a mutation says so: deleting the
		 * `inonclaim >= INOSPACE' guard above leaves every suite green
		 * (M5), because a full table then walks all 65536 values,
		 * finds nothing, and falls out through the test below to the
		 * same answer.  Neither is dead -- each catches the case the
		 * other would miss -- but only the count guard bounds the
		 * WORK, which is the thing no value-checking case can see:
		 * without it every call after exhaustion costs 65536
		 * iterations.  Do not delete one because a mutation of it
		 * stayed green.
		 */
		step = (inohash(ino) | 1ULL) & 0xffffULL;
		for (n = 0; n < INOSPACE; n++) {
			v = (unsigned short)((v + step) & 0xffff);
			if (!TAKEN(v)) break;
		}
		if (TAKEN(v)) return ((v8_ino_t)inofold(ino));
	}
	inoclaim[v >> 5] |= 1U << (v & 31);
	inonclaim++;
	s->host = ino;
	s->v7   = v;
	inoused++;
	return ((v8_ino_t)v);
}

/*
 * Snapshot the directory behind `fd` into V7 records.  `fd` stays open and
 * owned by the caller; a duplicate is handed to fdopendir, which consumes it.
 */
/*
 * Snapshot the directory behind `fd` into V7 records.
 *
 * The host entries come from getdirentries64, the raw syscall behind
 * readdir(3).  Its records are variable-length: a 64-bit inode, a 64-bit
 * offset, a 16-bit record length, a 16-bit name length, a type byte, then the
 * name.  Stepping by d_reclen is what walks them.
 */
struct hostdirent64 {
	unsigned long long d_ino;
	unsigned long long d_seekoff;
	unsigned short	   d_reclen;
	unsigned short	   d_namlen;
	unsigned char	   d_type;
	char		   d_name[1];
};

int
v8sys_diropen(const char *path, int fd)
{
	struct dirshim *s = 0;
	/*
	 * static, not automatic: an 8 KB stack frame makes clang emit a call to
	 * ___chkstk_darwin, which is a libc symbol and would defeat the whole
	 * point of reaching the kernel directly.  Only one directory is being
	 * snapshotted at a time, so sharing the buffer costs nothing.
	 */
	static char hostbuf[8192];
	long got, off, base;
	int i, dupfd;
	long cap, used;
	char *buf;

	for (i = 0; i < MAXDIRFD; i++)
		if (shims[i].fd == -1) { s = &shims[i]; break; }
	if (s == 0) { v8_errno = V8_EMFILE; return (-1); }

	/*
	 * Read through a duplicate so the caller's descriptor keeps its own
	 * file position -- a V8 program may lseek it, and dirseek() below
	 * expects the offset it manages to be the only one that matters.
	 */
	if ((dupfd = (int)rawsys1(SYS_dup, fd)) < 0) return (-1);

	cap = 64 * sizeof(struct v8_direct);
	if ((buf = v8sys_alloc(cap)) == 0) {
		rawsys1(SYS_close, dupfd);
		v8_errno = V8_ENOMEM;
		return (-1);
	}
	used = 0;
	base = 0;

	for (;;) {
		got = rawsys4(SYS_getdirentries64, dupfd, (long)hostbuf,
		    (long)sizeof hostbuf, (long)&base);
		if (got <= 0) break;

		for (off = 0; off < got; ) {
			struct hostdirent64 *hd =
			    (struct hostdirent64 *)(hostbuf + off);
			struct v8_direct rec;
			long nl;

			if (hd->d_reclen == 0) break;	/* malformed; stop */
			off += hd->d_reclen;

			if (used + (long)sizeof rec > cap) {
				char *nb = v8sys_alloc(cap * 2);
				if (nb == 0) {
					v8sys_free(buf);
					rawsys1(SYS_close, dupfd);
					v8_errno = V8_ENOMEM;
					return (-1);
				}
				bcopy_(nb, buf, used);
				v8sys_free(buf);
				buf = nb;
				cap *= 2;
			}

			/*
			 * Reconcile the entry's inode with what stat() would
			 * report for the same name.  In a V7 filesystem they
			 * are the same number by construction; across an APFS
			 * firmlink they are not, and pwd(1) fails because
			 * getwd(3) matches directory entries against stat(".").
			 * See v8sys_host_ino_at() in syscall.c for the detail.
			 *
			 * Only for directories (DT_DIR is 4): firmlinks and
			 * mount points are always directories, and this costs a
			 * syscall per entry, so paying it for every regular file
			 * would slow every ls in the system to fix nothing.
			 * If the stat fails -- a directory we cannot search --
			 * the raw number stands, which is what we had before.
			 */
			if (hd->d_type == 4) {
				unsigned long long real =
				    v8sys_host_ino_at(dupfd, hd->d_name);
				rec.d_ino = v8sys_fold_ino(real ? real : hd->d_ino);
			} else
				rec.d_ino = v8sys_fold_ino(hd->d_ino);
			bzero_(rec.d_name, V8_DIRSIZ);
			nl = hd->d_namlen;
			if (nl > V8_DIRSIZ) nl = V8_DIRSIZ;	/* the V7 limit */
			bcopy_(rec.d_name, hd->d_name, nl);
			bcopy_((char *)(buf + used), (char *)&rec, sizeof rec);
			used += sizeof rec;
		}
	}
	rawsys1(SYS_close, dupfd);

	s->fd = fd;
	s->recs = buf;
	s->nbytes = used;
	s->pos = 0;
	return (0);
}

/*
 * Adopt a snapshot SOMEBODY ELSE BUILT, and take ownership of the buffer.
 *
 * WHY THIS RATHER THAN A SECOND SNAPSHOT IMPLEMENTATION.  A v8fs directory
 * arrives as a run of 9P stat records off a socket, not as host getdirentries,
 * so v8sys_diropen above cannot produce it -- but everything AFTER the
 * conversion is identical, and it is exactly the part with the interesting
 * rules in it: dirsize's "what read(2) will produce is not what the host
 * charges", the -1 free-slot sentinel that fd 0 forced, and a seek that is in
 * record space rather than in the underlying object's.  Two copies of those is
 * one copy that goes stale, which is the one-table rule this shim already
 * applies to the mount list.
 *
 * THE BUFFER MUST COME FROM v8sys_alloc, because v8sys_dirclose frees it with
 * v8sys_free.  Said out loud because the ownership transfer is the whole
 * interface and a caller that kept the pointer would double-free at close.
 *
 * A CLIENT-SIDE SNAPSHOT IS NOT THE COMPROMISE IT LOOKS LIKE.  Everything else
 * about a v8fs file is server-side precisely so that dup, fork and a program
 * replacing itself all work (p9.h's extension note).  A directory is the one
 * thing that is not -- and it inherits a limit this shim has had since
 * v8sys_diropen was written, rather than inventing one, because the host
 * filesystem's directories have been snapshotted per-descriptor all along.
 * Nothing redirects a descriptor from a directory, so the case does not arise.
 */
int
v8sys_diradopt(int fd, char *recs, long nbytes)
{
	struct dirshim *s = 0;
	int i;

	v8sys_dirinit();
	for (i = 0; i < MAXDIRFD; i++)
		if (shims[i].fd == -1) { s = &shims[i]; break; }
	if (s == 0) { v8_errno = V8_EMFILE; return (-1); }

	s->fd = fd;
	s->recs = recs;
	s->nbytes = nbytes;
	s->pos = 0;
	return (0);
}

long
v8sys_dirread(int fd, void *buf, long n)
{
	struct dirshim *s = find(fd);
	long avail;

	if (s == 0) { v8_errno = V8_EBADF; return (-1); }
	avail = s->nbytes - s->pos;
	if (avail <= 0) return (0);
	if (n > avail) n = avail;
	bcopy_((char *)buf, s->recs + s->pos, n);
	s->pos += n;
	return (n);
}

/* lseek on a directory descriptor: V8 programs rewind with lseek(fd,0,0). */
long
v8sys_dirseek(int fd, long off, int whence)
{
	struct dirshim *s = find(fd);
	long np;

	if (s == 0) { v8_errno = V8_EBADF; return (-1); }
	switch (whence) {
	case 0:	np = off; break;
	case 1:	np = s->pos + off; break;
	case 2:	np = s->nbytes + off; break;
	default: v8_errno = V8_EINVAL; return (-1);
	}
	if (np < 0) { v8_errno = V8_EINVAL; return (-1); }
	s->pos = np;
	return (np);
}

void
v8sys_dirclose(int fd)
{
	struct dirshim *s = find(fd);

	if (s == 0) return;
	v8sys_free(s->recs);
	s->recs = 0;
	s->fd = -1;
	s->nbytes = s->pos = 0;
}

/* Slots start life free; fd 0 is a valid descriptor so -1 is the sentinel. */
void
v8sys_dirinit(void)
{
	int i;
	static int done;

	if (done) return;
	done = 1;
	for (i = 0; i < MAXDIRFD; i++) shims[i].fd = -1;
}
