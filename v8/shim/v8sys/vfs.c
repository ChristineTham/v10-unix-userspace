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
	 * /usr/src/ is where V8 kept its own sources, and it is here so that
	 * Bell Labs' top-level build description can run in place.  Admin/Mk
	 * opens with `cd /usr/src/cmd' -- an absolute path, and the only one in
	 * the script -- so without this row the V8 world can rebuild a program
	 * from its own makefile (rung 5) but cannot rebuild one that HAS no
	 * makefile, which is more than half of cmd/.  See tests/jail.
	 */
	{ "/usr/src/",	 0, &v8fs_pass },
	/*
	 * /dev/fd -- THE THIRD TYPE, and the five rows below it are one device.
	 *
	 * The DIRECTORY is an ordinary directory and stays passthrough: V8's
	 * /dev/fd holds 128 real nodes and `ls /dev/fd' reads them out of the
	 * filesystem like any other name.  Its ENTRIES are not files at all.
	 * Spelling that as an exact row before the prefix row is the mechanism
	 * doing what it was built for -- v8fs_typefor returns the first match,
	 * and without it the "directory itself" rule at the bottom of the prefix
	 * arm would hand /dev/fd to the descriptor type.
	 */
	{ "/dev/fd",	 1, &v8fs_pass },
	{ "/dev/fd/",	 0, &v8fs_fdfs },
	/*
	 * ...and the four names V8 hard-links into it, minors 0-3.  proto-dev
	 * shows the link count: /dev/stdin, /dev/stdout, /dev/stderr and
	 * /dev/tty are `2', and so are /dev/fd/0, 1, 2 and 3; every other fd
	 * node is `1'.  See v8fs_fdfs below for why /dev/tty is here rather
	 * than under a stream driver.
	 */
	{ "/dev/tty",	 1, &v8fs_fdfs },
	{ "/dev/stdin",	 1, &v8fs_fdfs },
	{ "/dev/stdout", 1, &v8fs_fdfs },
	{ "/dev/stderr", 1, &v8fs_fdfs },
	/*
	 * /dev/ is here for the grovelers: load(1) opens /dev/kmem, which
	 * libkmemu manufactures.  It does NOT capture /dev/null, which has no
	 * rootfs copy and therefore falls through to the host's.
	 *
	 * IT USED TO SAY THAT ABOUT /dev/tty TOO, and warned that creating
	 * rootfs/dev/tty "would stop the V8 world seeing the real terminal".
	 * The warning was right and the conclusion was backwards: the real
	 * terminal is not what V8 puts there.  The five rows above are what it
	 * puts there, and rootfs/dev/tty now exists so that the NAME is real --
	 * it is never opened, because the row above claims the path first.
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

/*
 * THE CREATOR'S RULE BELONGS TO THE TYPE, and it was in syscall.c instead.
 *
 * `mkpath()' there is V8P_LOOK-then-V8P_MAKE, and its comment claims "every
 * syscall that can bring a name into existence uses this -- open with O_CREAT,
 * creat, mkdir, mknod...".  v8s_open never did: it passes V8P_MAKE straight
 * through this function, and routing v8s_creat through the switch made that two
 * callers rather than one.  So the rule moves here, where every filesystem type
 * that resolves a path gets it, and mkpath stays for the syscalls that do not
 * go through the switch at all (link, symlink, mkdir, mknod).
 *
 * It is a no-op for passthrough TODAY and that is worth saying rather than
 * discovering: V8P_MAKE keys on the parent, and a file that exists always has a
 * parent that exists, so LOOK and MAKE agree on every name LOOK can resolve.
 * The order matters for a type where they need not -- and for the reader of
 * this code, who should not have to derive that they agree.
 *
 * Two calls, never three, because v8sys_rootpath returns a pointer into its own
 * static buffer: the second call overwrites the first's answer, so the result
 * of LOOK has to be returned before MAKE runs.  That is the aliasing trap
 * v8s_link records.
 */
static char *
pt_path(char *p, int mode)
{
	char *q;

	if (mode != V8P_MAKE) return v8sys_rootpath(p, V8P_LOOK);
	q = v8sys_rootpath(p, V8P_LOOK);
	if (q != p) return (q);
	return v8sys_rootpath(p, V8P_MAKE);
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

/* ---------------------------------------------------------------- /dev/fd */

/*
 * THE THIRD TYPE, and it is the one this port went looking for in the wrong
 * place.  PLAN.md section 8a step 1b costed a host-fd driver to put underneath
 * /dev/tty, on the reading that V8's controlling terminal was a stream.  It is
 * not.  It is not a stream, and it is not a device: /dev/tty is a hard link to
 * /dev/fd/3, and opening anything in /dev/fd is dup(2).
 *
 * Bell Labs say so in their own man page, usr/man/man4/fd.4:
 *
 *	If file descriptor n is open, these two system calls have the same
 *	effect:   fd = open("/dev/fd/n", mode);   fd = dup(n);
 *	Creat(2) is equivalent to open, and mode is ignored.  As with dup,
 *	subsequent IO on fd fails unless the original file descriptor allows
 *	the read or write operation.
 *	...
 *	Open returns -1 if the related file descriptor is not open.
 *
 * and the kernel agrees, four times over:
 *
 *   proto-dev:91	tty is major 40 minor 3, link count 2 -- and fd/3 is
 *			the other link.  stdin/stdout/stderr are 40,0-2.
 *   conf/devices:55	`device 40  std', with `int stdio_no = 40' on the next
 *			line.  No driver name, no `stream-device' keyword.
 *   dev/conf.c:565	major 40's cdevsw row is nodev, nodev, nodev, nodev,
 *			nodev, nulldev, NULL.  Every entry, and a null
 *			streamtab.  There is nothing to call.
 *   sys/sys2.c:174	open1() special-cases it BEFORE the permission check:
 *			getf(minor), ufalloc(), u_ofile[i] = fp, fp->f_count++
 *			-- which is the body of dup(2), written out.
 *
 * V7's /dev/tty was a real driver (syopen, redirecting through u.u_ttyp).  That
 * file is still in the V8 tree as sys/sys/sys.c and is DEAD: it is absent from
 * conf/files, nothing in conf.c points at it, and it could not compile anyway
 * because u_ttyp and u_ttyd are not in V8's struct user.  Killian replaced the
 * driver with a filesystem convention.
 *
 * WHY fd 3.  Because init put the terminal there.  cmd/init.c:368-382:
 *
 *	while (open(tty, 2) != 0) sleep(10);
 *	ioctl(0, TIOCSPGRP, (char *)0);
 *	while (ioctl(0, FIOPOPLD, (char *)0) >= 0) ;
 *	ioctl(0, FIOPUSHLD, &tty_ld);
 *	dup(0); dup(0); dup(0);
 *
 * -- three dups, to 1, 2 and 3.  "Controlling terminal" is not a kernel fact in
 * V8; it is the userspace convention that fd 3 is one.  The v8 launcher is this
 * port's init and does the same thing, for the same reason, at $(BINDIR_HOST)/v8.
 *
 * WHAT THIS TYPE IMPLEMENTS, AND WHAT IT DELIBERATELY DOES NOT.  Three
 * operations are its own -- t_path, t_open and t_stat.  Everything after open
 * is the passthrough type's, unchanged and not merely equivalent, because after
 * the dup there is nothing left that is special: a dup'd descriptor IS an
 * ordinary host descriptor, and binding it to a type of its own would be
 * inventing a difference the kernel does not have.  That is also why fd_open
 * never calls v8fs_bind().
 */

/*
 * 128 because NOFILE is 128 (h/param.h:19), which is not a coincidence: the
 * node set IS the file table.  A name outside it -- /dev/fd/999, /dev/fd/x --
 * is a name V8's /dev does not contain, so it is ENOENT from namei and not
 * EBADF from getf.  Keeping the two apart is the difference between "no such
 * device" and fd.4's "the related file descriptor is not open".
 */
#define V8FS_NDEVFD	128

/* No libc here -- see rawsys.h.  strcmp is the only piece we would borrow. */
static int
streq_(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (*a == '\0' && *b == '\0');
}

static int
fd_minor(char *p)
{
	int n, i;

	if (p == 0) return (-1);
	/* The four hard links, by name.  proto-dev's minors, in its order. */
	if (streq_(p, "/dev/stdin"))  return (0);
	if (streq_(p, "/dev/stdout")) return (1);
	if (streq_(p, "/dev/stderr")) return (2);
	if (streq_(p, "/dev/tty"))    return (3);

	for (i = 0; "/dev/fd/"[i]; i++)
		if (p[i] != "/dev/fd/"[i]) return (-1);
	if (p[i] == '\0') return (-1);		/* "/dev/fd/" itself */
	/*
	 * Strictly decimal and strictly the whole component.  "01" is not a
	 * node V8 shipped and neither is "3/x", so both are ENOENT rather than
	 * a lenient reading of a name that does not exist.
	 */
	if (p[i] == '0' && p[i + 1] != '\0') return (-1);
	for (n = 0; p[i]; i++) {
		if (p[i] < '0' || p[i] > '9') return (-1);
		n = n * 10 + (p[i] - '0');
		if (n >= V8FS_NDEVFD) return (-1);
	}
	return (n);
}

/*
 * t_path: identity.  There is no host path -- the answer is a descriptor, and
 * running the name through rootpath() would resolve it to the empty rootfs node
 * that exists only so `ls /dev' tells the truth about the namespace.
 */
static char *
fd_path(char *p, int mode)
{
	(void)mode;
	return (p);
}

static int
fd_open(char *p, int flags, int mode)
{
	int n = fd_minor(p);

	/*
	 * flags and mode are DISCARDED, and that is fd.4's sentence rather than
	 * an omission: "Creat(2) is equivalent to open, and mode is ignored.
	 * As with dup, subsequent IO on fd fails unless the original file
	 * descriptor allows the read or write operation."  So open("/dev/fd/0",
	 * 1) on a read-only stdin SUCCEEDS here and fails at the first write.
	 *
	 * MEASURED against the host rather than assumed, because macOS has a
	 * /dev/fd of its own and the first draft of this comment was wrong
	 * about it.  Darwin's is a dup too, so the shared offset -- the classic
	 * fdescfs difference -- is NOT one here: both continue where the other
	 * left off.  Three things do differ, and they are why this is
	 * implemented rather than delegated:
	 *
	 *	open("/dev/fd/3", 1) on a read-only fd   V8 ok, later EIO/EBADF
	 *						 macOS EACCES at open
	 *	open("/dev/fd/999")			 V8 ENOENT (no node)
	 *						 macOS EBADF
	 *	stat("/dev/fd/1")			 V8 crw-rw-rw- 40,1
	 *						 macOS the real object
	 *
	 * and, the one that matters most, macOS's /dev/tty is the controlling
	 * terminal while V8's is fd 3.  Delegating would have imported all four.
	 */
	(void)flags; (void)mode;
	if (n < 0) { v8_errno = V8_ENOENT; return (-1); }
	RET(rawsys1(SYS_dup, n));
}

/*
 * t_stat.  The NODE is a character device whatever the descriptor turns out to
 * point at, because on V8 the thing being stat'd is an inode in /dev and not
 * the open file: `test -c /dev/tty' is true with a pipe on fd 3.  fstat on the
 * descriptor this type hands out is the passthrough one and reports the real
 * object, which is the same asymmetry V8 has and worth having a case for.
 */
static int
fd_stat(char *p, struct v8_stat *st, int follow)
{
	int n = fd_minor(p), i;
	char *q = (char *)st;

	(void)follow;
	if (n < 0) { v8_errno = V8_ENOENT; return (-1); }
	for (i = 0; i < (int)sizeof *st; i++) q[i] = 0;
	st->st_dev   = 0;
	st->st_ino   = (v8_ino_t)(n + 1);	/* never 0: inofold()'s rule,
						   dir.c:212 -- and assigned
						   outside that map on purpose,
						   see its `still open' note */
	st->st_mode  = V8_S_IFCHR | 0666;	/* crw-rw-rw-, proto-dev:91 */
	st->st_nlink = (short)(n <= 3 ? 2 : 1);	/* the four hard links */
	st->st_rdev  = (v8_dev_t)((40 << 8) | n);	/* makedev(40, n) */
	return (0);
}

struct v8fstyp v8fs_fdfs = {
	"fd",
	fd_path,
	fd_open, pt_close,
	pt_read, pt_write, pt_seek,
	fd_stat, v8sys_pt_fstat,
	v8sys_pt_ioctl
};
