/*
 * The filesystem switch: the mount table, and the one type behind it.
 *
 * vfs.h says why the interface has the shape it has -- V8 already had a VFS,
 * and this answers to `struct fstypsw' rather than inventing a rival.  This
 * file is the table and the passthrough implementation.
 *
 * Raw syscalls only, like the rest of shim/v8sys.
 */

#include "v8sys.h"
#include "vfs.h"
#include "rawsys.h"

extern int v8_errno;
extern char *v8sys_rootpath(char *p, int mode);
extern void v8sys_dirinit(void);
extern int v8sys_diropen(const char *path, int fd);
extern int v8sys_pt_fstat(int fd, struct v8_stat *st);
extern long v8sys_dirseek(int fd, long off, int whence);  /* dir.c; not in v8sys.h */

#define RET(r)	do { long _r = (r); \
		     if (_r < 0) { v8_errno = v8sys_errno(RAWERR(_r)); return (-1); } \
		     return (_r); } while (0)

/* ------------------------------------------------------- the mount table */

/*
 * THIS IS THE LIST THAT USED TO BE `v8dirs[]' IN syscall.c, with a type column.
 *
 * Generalising it rather than adding a second table beside it is the point.
 * Two prefix lists that have to agree by hand are exactly the standing
 * invitation that kmem.c's one-table rule exists to refuse: get them out of
 * step and a path is jailed for resolution and unjailed for dispatch, or the
 * reverse, and nothing says so.  A /proc entry is one more row here.
 *
 * m_exact distinguishes a FILE at the root from a directory prefix.  There is
 * no way to spell /unix in a prefix list -- an entry without a trailing slash
 * would also claim /unixfoo -- and the alternative is a rule that is wrong only
 * for names nobody has created yet.
 */
static struct v8mount {
	const char	*m_pfx;
	int		 m_exact;
	struct v8fstyp	*m_typ;
} mounts[] = {
	{ "/usr/lib/",	 0, &v8fs_pass },
	{ "/usr/share/", 0, &v8fs_pass },
	{ "/usr/dict/",	 0, &v8fs_pass },
	{ "/lib/",	 0, &v8fs_pass },
	{ "/usr/pub/",	 0, &v8fs_pass },
	{ "/bin/",	 0, &v8fs_pass },
	{ "/usr/bin/",	 0, &v8fs_pass },
	{ "/etc/",	 0, &v8fs_pass },
	{ "/usr/man/",	 0, &v8fs_pass },
	{ "/usr/spool/", 0, &v8fs_pass },
	/*
	 * /dev/ is here for the grovelers: load(1) opens /dev/kmem, which
	 * libkmemu manufactures.  It does NOT capture /dev/null or /dev/tty,
	 * by the same mechanism that protects every other entry -- a path whose
	 * rootfs copy does not exist falls through to the host, and the rootfs
	 * has neither.  Worth knowing rather than assuming: create
	 * rootfs/dev/tty and the V8 world would stop seeing the real terminal.
	 */
	{ "/dev/",	 0, &v8fs_pass },
	/* /unix is the kernel namelist libkmemu writes -- see kmem.c. */
	{ "/unix",	 1, &v8fs_pass },
	/*
	 * /proc -- Killian's process filesystem, and the SECOND TYPE.  Its row
	 * carries no pointer because the type lives in libkmemu: it answers from
	 * proc_listpids and proc_pidinfo, which are libc, and putting it here
	 * would make every V8 binary import libSystem for a filesystem it never
	 * opens.  m_typ is filled from kmemu_procfs() at lookup time, and the
	 * do-nothing version in nokmemu.c returns null -- so in a binary without
	 * libkmemu nothing claims /proc and it falls through to the host, where
	 * macOS has none, which is the truth.
	 */
	{ "/proc/",	 0, 0 },
	{ "/proc",	 1, 0 },
	{ 0, 0, 0 }
};

extern struct v8fstyp *kmemu_procfs(void);

/*
 * A row's type, resolved late for the ones that have none of their own.  Only
 * /proc is in that position today; see its rows above.
 */
static struct v8fstyp *
typ(int i)
{
	return (mounts[i].m_typ ? mounts[i].m_typ : kmemu_procfs());
}

struct v8fstyp *
v8fs_typefor(const char *p)
{
	int i, k;

	if (p == 0 || *p != '/') return (0);
	for (i = 0; mounts[i].m_pfx; i++) {
		const char *d = mounts[i].m_pfx;
		for (k = 0; d[k] && p[k] == d[k]; k++)
			;
		if (mounts[i].m_exact) {
			if (d[k] == '\0' && p[k] == '\0') return (typ(i));
			continue;
		}
		if (d[k] == '\0') return (typ(i));
		/*
		 * ...and the directory ITSELF, spelled without the trailing
		 * slash.  The prefixes carry one so that "/binary" is not
		 * mistaken for "/bin/", but that also meant "/etc" and "/bin"
		 * matched nothing, so `ls /etc' listed the Mac's while
		 * `cat /etc/group' read V8's -- the same path naming two
		 * different worlds depending on a trailing character.
		 */
		if (d[k] == '/' && d[k + 1] == '\0' && p[k] == '\0')
			return (typ(i));
	}
	return (0);
}

/* ------------------------------------------------- descriptor ownership */

/*
 * Which filesystem owns a descriptor.  Null means passthrough, so the table
 * costs nothing until a second type exists and needs no initialisation.
 *
 * IT DOES NOT SURVIVE A PROGRAM REPLACING ITSELF, and that is fine today and
 * will not be later.  The table is process memory; a passthrough descriptor
 * needs no entry, so inheritance works by construction.  A server-backed one
 * would need its handle re-attached on the far side, which is a thing 9P has a
 * message for and an in-process table could never have done anyway.
 */
#define V8FS_NFD	256
static struct v8fstyp *fdtyp[V8FS_NFD];

struct v8fstyp *
v8fs_fdtype(int fd)
{
	if (fd < 0 || fd >= V8FS_NFD || fdtyp[fd] == 0) return (&v8fs_pass);
	return (fdtyp[fd]);
}

void
v8fs_bind(int fd, struct v8fstyp *t)
{
	if (fd >= 0 && fd < V8FS_NFD) fdtyp[fd] = (t == &v8fs_pass) ? 0 : t;
}

void
v8fs_unbind(int fd)
{
	if (fd >= 0 && fd < V8FS_NFD) fdtyp[fd] = 0;
}

/* ------------------------------------------------------------ passthrough */

/*
 * The host filesystem, seen through the jail.  Every one of these is the code
 * that was inline in syscall.c before the switch existed, moved rather than
 * rewritten -- this step is meant to change no behaviour at all.
 */

static char *
pt_path(char *p, int mode)
{
	return v8sys_rootpath(p, mode);
}

/*
 * open, and the directory registration that goes with it.  Noticing that a
 * descriptor is a directory -- and snapshotting it into V7 records -- is a
 * property of THIS filesystem, not of open(2); /proc builds its own directory
 * and must not be handed to dir.c.  It moved here from v8s_open when the switch
 * arrived, which is the sort of thing a switch is for.
 */
static int
pt_open(char *rp, int flags, int mode)
{
	struct v8_stat st;
	long fd;

	v8sys_dirinit();
	/*
	 * V8's flags are the V7 originals -- 0 read, 1 write, 2 read/write --
	 * and O_CREAT/O_TRUNC/O_APPEND arrived later with the values macOS
	 * still uses, so nothing needs translating.
	 */
	fd = rawsys3(SYS_open, (long)rp, flags, mode);
	if (fd < 0) { v8_errno = v8sys_errno(RAWERR(fd)); return (-1); }

	if (v8sys_pt_fstat((int)fd, &st) == 0 &&
	    (st.st_mode & V8_S_IFMT) == V8_S_IFDIR) {
		if (v8sys_diropen(rp, (int)fd) < 0) {
			rawsys1(SYS_close, fd);
			return (-1);
		}
	}
	return ((int)fd);
}

static int
pt_close(int fd)
{
	if (v8sys_isdirfd(fd)) v8sys_dirclose(fd);
	RET(rawsys1(SYS_close, fd));
}

/*
 * Directory reads are answered by dir.c, which turns the host's getdirentries
 * into authentic 16-byte V7 records.  That is a filesystem behaviour and it
 * sits here rather than in the caller -- but it is NOT a mount type: it applies
 * to a directory descriptor on any mount, so it is a layer over a type rather
 * than one of them.  Noted because the shape invites the mistake.
 */
static long
pt_read(int fd, char *b, long n)
{
	if (v8sys_isdirfd(fd)) return (v8sys_dirread(fd, b, n));
	RET(rawsys3(SYS_read, fd, (long)b, n));
}

static long
pt_write(int fd, char *b, long n)
{
	RET(rawsys3(SYS_write, fd, (long)b, n));
}

static long
pt_seek(int fd, long off, int whence)
{
	if (v8sys_isdirfd(fd)) return (v8sys_dirseek(fd, off, whence));
	RET(rawsys3(SYS_lseek, fd, off, whence));
}

/*
 * stat and fstat stay in syscall.c, beside stat_translate() and the host
 * struct they convert from -- V8's is a different shape entirely, 16-bit inode
 * numbers among other things.  Two copies of a struct conversion at this seam
 * is how this shim's worst bugs have started, so the table points at the one
 * that exists rather than at a second.  They take an ALREADY-RESOLVED path,
 * which is what makes them safe to call from here: the resolution happened one
 * level up, in the caller that chose this type.
 */
extern int v8sys_pt_stat(char *rp, struct v8_stat *st, int follow);
extern int v8sys_pt_fstat(int fd, struct v8_stat *st);

/*
 * ...and ioctl stays in ioctl.c for the same reason, one step further: the
 * sgtty-over-termios translation is 200 lines of mapping that belongs beside
 * the two flag vocabularies it converts between, and it is not per-filesystem.
 * It is the PASSTHROUGH type's t_ioctl, which is what it has always been in
 * fact; the switch only made that sayable.
 */
extern int v8sys_pt_ioctl(int fd, int cmd, char *arg);

struct v8fstyp v8fs_pass = {
	"pass",
	pt_path,
	pt_open, pt_close,
	pt_read, pt_write, pt_seek,
	v8sys_pt_stat, v8sys_pt_fstat,
	v8sys_pt_ioctl
};
