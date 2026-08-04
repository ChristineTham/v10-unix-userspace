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
 *   Inode numbers are folded to 16 bits (see stat.c) and can therefore collide.
 *   Programs that compare inode numbers for identity -- find(1) hunting for
 *   hard links -- may report false matches.  Never folded to 0, which V7 uses
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

static struct dirshim *
find(int fd)
{
	int i;

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
 * Fold a host inode number into 16 bits without ever producing 0, which V7
 * reserves for "this slot is empty" and which every directory reader skips.
 */
v8_ino_t
v8sys_fold_ino(unsigned long long ino)
{
	unsigned short v;

	v = (unsigned short)(ino ^ (ino >> 16) ^ (ino >> 32) ^ (ino >> 48));
	return (v ? v : (v8_ino_t)1);
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
