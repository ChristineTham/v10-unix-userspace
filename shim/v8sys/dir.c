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

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "v8sys.h"

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
int
v8sys_diropen(const char *path, int fd)
{
	struct dirshim *s;
	DIR *dp;
	struct dirent *de;
	int i, dupfd;
	long cap, used;
	char *buf;

	for (i = 0; i < MAXDIRFD; i++)
		if (shims[i].fd == -1 || shims[i].fd == 0) { s = &shims[i]; break; }
	if (i == MAXDIRFD) { errno = EMFILE; return (-1); }

	if ((dupfd = dup(fd)) < 0) return (-1);
	if ((dp = fdopendir(dupfd)) == 0) { close(dupfd); return (-1); }

	cap = 64 * sizeof(struct v8_direct);
	if ((buf = malloc(cap)) == 0) { closedir(dp); errno = ENOMEM; return (-1); }
	used = 0;

	while ((de = readdir(dp)) != 0) {
		struct v8_direct rec;

		if (used + (long)sizeof rec > cap) {
			char *nb;
			cap *= 2;
			if ((nb = realloc(buf, cap)) == 0) {
				free(buf); closedir(dp); errno = ENOMEM; return (-1);
			}
			buf = nb;
		}
		rec.d_ino = v8sys_fold_ino((unsigned long long)de->d_ino);
		/* Not strncpy's usual use: the field is NOT null-terminated
		 * when the name fills it, exactly as on a V7 disk. */
		memset(rec.d_name, 0, V8_DIRSIZ);
		strncpy(rec.d_name, de->d_name, V8_DIRSIZ);
		memcpy(buf + used, &rec, sizeof rec);
		used += sizeof rec;
	}
	closedir(dp);		/* also closes dupfd */

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

	if (s == 0) { errno = EBADF; return (-1); }
	avail = s->nbytes - s->pos;
	if (avail <= 0) return (0);
	if (n > avail) n = avail;
	memcpy(buf, s->recs + s->pos, n);
	s->pos += n;
	return (n);
}

/* lseek on a directory descriptor: V8 programs rewind with lseek(fd,0,0). */
long
v8sys_dirseek(int fd, long off, int whence)
{
	struct dirshim *s = find(fd);
	long np;

	if (s == 0) { errno = EBADF; return (-1); }
	switch (whence) {
	case 0:	np = off; break;
	case 1:	np = s->pos + off; break;
	case 2:	np = s->nbytes + off; break;
	default: errno = EINVAL; return (-1);
	}
	if (np < 0) { errno = EINVAL; return (-1); }
	s->pos = np;
	return (np);
}

void
v8sys_dirclose(int fd)
{
	struct dirshim *s = find(fd);

	if (s == 0) return;
	free(s->recs);
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
