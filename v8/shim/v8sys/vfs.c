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
extern char *v8sys_getenv(const char *name);
extern char *v8sys_rootpath(char *p, int mode);
extern int v8sys_rootjailed(const char *q);
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

/* ------------------------------------------------ the logical working directory */

/*
 * §8a step 5i.  A path is folded before anyone asks who owns it, because a
 * relative name under a mount is not distinguishable from any other relative
 * name -- and until this existed, v8s_chdir had to REFUSE a mounted directory,
 * since a program that got inside one would find every relative name resolving
 * against the host.
 *
 * WHY LEXICALLY, WHICH IS THE PART THAT LOOKS LIKE A SHORTCUT AND IS NOT.
 * `..' at a mount root does not escape and the server cannot make it: measured
 * against a real v8fsd, `ls /mnt/..' lists THE IMAGE ROOT AGAIN, which is V7
 * being right -- a filesystem root's `..' points at itself, and on a real Unix
 * it is namei's mount table that fixes the walk up when it crosses a mount.
 * There is no kernel here and the image does not know it is mounted, so the
 * client is the only thing that can do it.  Plan 9 -- whose protocol this is --
 * folds `..' textually for the same reason, because a bind makes the kernel's
 * answer meaningless too.
 *
 * WHAT IT COSTS, SAID OUT LOUD.  A lexical `..' disagrees with the kernel's
 * when a component is a symlink to a directory somewhere else: A/B/.. is A
 * here and B's target's parent there.  Two things bound it.  It applies only
 * in a process with V8MOUNT set -- v8fs_logical is the identity otherwise, so
 * every other suite in this tree runs on exactly the code it ran on before.
 * And inside the jail there is nothing to disagree about: measured with
 * `find rootfs -type l', the rootfs contains ZERO symlinks.
 *
 * THE cwd IS A STRING IN THE ENVIRONMENT, not a descriptor.  A descriptor
 * would take a number sh(1) redirects onto and that /dev/fd/3 means the
 * terminal by, and it could not name a directory on the image at all -- there
 * is no host object to hold open.
 */
static char cwdbuf[V8_CWDMAX];
static int  cwdstate;			/* 0 unread, 1 have one, -1 none */

const char *
v8fs_cwd(void)
{
	char *e;
	int i;

	if (cwdstate == 0) {
		cwdstate = -1;
		if ((e = v8sys_getenv("V8CWD")) != 0 && *e == '/') {
			for (i = 0; e[i] && i < V8_CWDMAX - 1; i++)
				cwdbuf[i] = e[i];
			if (e[i] == '\0') { cwdbuf[i] = '\0'; cwdstate = 1; }
		}
	}
	return (cwdstate > 0 ? cwdbuf : 0);
}

int
v8fs_setcwd(const char *p)
{
	int i;

	/*
	 * NO MOUNT, NO LOGICAL cwd, and this line is what keeps §8a step 5i out
	 * of every run that did not ask for it.  Without it v8s_chdir would
	 * record one for any absolute path, v8s_execve would splice V8CWD into
	 * the environment of EVERY V8 program in the world, and a mechanism that
	 * changes no behaviour would still be visible in `env'.  The kernel's
	 * own cwd is the whole answer in that world and it already moved.
	 */
	if (!v8fs_p9any()) return (0);
	if (p == 0 || *p != '/') return (-1);
	for (i = 0; p[i]; i++)
		if (i >= V8_CWDMAX - 1) return (-1);
	for (i = 0; (cwdbuf[i] = p[i]) != '\0'; i++)
		;
	cwdstate = 1;
	return (0);
}

/*
 * Where the basename starts, or -1 for "there is no component to keep" -- the
 * root, and any path made only of slashes.  Trailing slashes are ignored when
 * LOOKING for it and kept when COPYING it, so "/mnt/d/" folds to "/mnt/d/" and
 * not to "/mnt/d": a trailing slash is the caller's assertion that the name is
 * a directory, and dropping it would answer a question nobody asked.
 */
static int
basestart(const char *s)
{
	int i, n, last = -1;

	for (n = 0; s[n]; n++)
		;
	while (n > 1 && s[n - 1] == '/') n--;
	for (i = 0; i < n; i++)
		if (s[i] == '/') last = i;
	if (last < 0 || last + 1 >= n) return (-1);
	return (last + 1);
}

/*
 * Fold in place, up to `lim'; anything from `keep' on is copied verbatim.
 *
 * IN PLACE IS SAFE AND IT IS WORTH SAYING WHY, because the loop looks like it
 * could outrun itself.  Every component is written as "/" plus its own bytes,
 * which is exactly what was read to reach it (a separator was consumed first),
 * and `..' writes nothing at all -- so the write index never passes the read
 * index.  The tail is the same argument one step on: folding "/a/b/" leaves
 * r = 4 against keep = 5, and r can never REACH keep because the separator at
 * keep-1 is consumed and not written.
 */
static void
foldpath(char *s, int lim, int keep)
{
	int r = 0, i = 0, j;

	while (i < lim) {
		while (i < lim && s[i] == '/') i++;
		if (i >= lim) break;
		for (j = i; j < lim && s[j] != '/'; j++)
			;
		if (j - i == 1 && s[i] == '.') {
			;				/* "." names where we are */
		} else if (j - i == 2 && s[i] == '.' && s[i + 1] == '.') {
			while (r > 0 && s[r - 1] != '/') r--;
			if (r > 0) r--;			/* and the slash itself */
		} else {
			s[r++] = '/';
			while (i < j) s[r++] = s[i++];
		}
		i = j;
	}
	if (keep >= 0) {
		s[r++] = '/';
		for (i = keep; s[i]; i++) s[r++] = s[i];
	}
	s[r] = '\0';
	if (r == 0) { s[0] = '/'; s[1] = '\0'; }	/* "/.." is "/" */
}

/*
 * FOLD AN ABSOLUTE PATH IN PLACE, unconditionally -- the half of v8fs_logical
 * that has no scope test, exported because p9cl.c needs it before there is a
 * mount to be in scope of.
 *
 * A FOLD INTRODUCES A NORMAL FORM, AND EVERYTHING COMPARED AGAINST A FOLDED
 * PATH HAS TO BE IN IT.  vfs.c's static table is normalised by construction --
 * it is written out by hand -- but V8MOUNT's prefix is user input, and this is
 * how it joins.  Measured the hard way: $TMPDIR ends in a slash on a Mac, so
 * tests/streams builds `$TMPDIR/streams.N/...' with a DOUBLE slash in it, and
 * the first version of the fold normalised the path while leaving the prefix
 * alone.  p9rel compares byte for byte, so the mount stopped claiming its own
 * files and `cat' read the host directory the mount was covering -- silently,
 * and in the one case written to prove containment.
 */
void
v8fs_foldabs(char *s, int mode)
{
	int n, keep;

	if (s == 0 || *s != '/') return;
	for (n = 0; s[n]; n++)
		;
	keep = (mode == V8P_LOOK) ? -1 : basestart(s);
	foldpath(s, keep < 0 ? n : keep, keep);
}

/*
 * CALLING THIS ON ITS OWN ANSWER IS SAFE, AND IT HAPPENS: v8sys_rootpath folds
 * once and then hands the result to v8fs_mounted and v8fs_typefor, which fold
 * again.  Two properties make that an identity rather than a hazard.  The copy
 * loop with p == buf is buf[i] = buf[i], because a folded path is absolute and
 * the cwd branch is not taken; and folding an already-folded path removes
 * nothing, since there is no `.', no `..' outside the kept tail and no
 * duplicate slash left to remove.  Said out loud because this file's own
 * lessons are about things that are safe by accident.
 */
char *
v8fs_logical(char *p, int mode)
{
	static char buf[V8_CWDMAX];
	const char *cwd;
	int n = 0, i, keep;

	if (p == 0) return (p);
	/*
	 * NO MOUNT, NO FOLD.  This is the scope of the whole mechanism and it
	 * is one branch: with V8MOUNT unset there is nothing a lexical reading
	 * of `..' could be more correct about, and every path-taking syscall in
	 * this shim goes through here.
	 */
	if (!v8fs_p9any()) return (p);

	if (*p != '/') {
		/*
		 * A RELATIVE NAME WITH NO LOGICAL cwd IS THE HOST'S BUSINESS,
		 * and handing it back unchanged is what keeps the two agreeing:
		 * v8s_chdir really chdirs whenever the target is a host path,
		 * so until something enters a mount the kernel's own cwd is the
		 * only answer and it is the right one.
		 */
		if ((cwd = v8fs_cwd()) == 0) return (p);
		for (i = 0; cwd[i]; i++) {
			if (n >= V8_CWDMAX - 2) return (p);
			buf[n++] = cwd[i];
		}
		if (n == 0 || buf[n - 1] != '/') buf[n++] = '/';
	}
	for (i = 0; p[i]; i++) {
		if (n >= V8_CWDMAX - 1) return (p);
		buf[n++] = p[i];
	}
	buf[n] = '\0';

	keep = (mode == V8P_LOOK) ? -1 : basestart(buf);
	foldpath(buf, keep < 0 ? n : keep, keep);
	return (buf);
}

struct v8fstyp *
v8fs_typefor(const char *p, int mode)
{
	struct v8fstyp *t;
	int i, k;

	p = v8fs_logical((char *)p, mode);
	if (p == 0 || *p != '/') return (0);
	/*
	 * A v8fs MOUNT IS ASKED FIRST, and the order is mount semantics rather
	 * than a preference: mounting on a name hides what was under it.  The
	 * table below is static and this one is configured at run time from
	 * V8MOUNT, so it cannot be a row -- p9cl.c argues why the environment
	 * is the registry (the jail is per-binary; a `mount' command would put
	 * a row in its own address space and exit).
	 *
	 * The foot-gun is stated rather than guarded: V8MOUNT=/=... would
	 * shadow /bin and the whole world with it.  That is what mounting on
	 * the root does, and a rule refusing it would be this port deciding
	 * something Unix does not.
	 */
	if ((t = v8fs_p9for(p)) != 0) return (t);
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
 * Which filesystem owns a descriptor.
 *
 * THIS USED TO BE THE ANSWER AND IS NOW A CACHE, and the note it replaces was
 * right about the problem and wrong about how long it had.  It said the table
 * "does not survive a program replacing itself, and that is fine today and
 * will not be later" -- later arrived with v8fs.  A server-backed descriptor
 * that reads as passthrough gets a raw read(2) on a 9P socket, and because the
 * server sends nothing unsolicited that is a HANG rather than a wrong answer.
 * Redirection is the ordinary way it would happen: sh opens the file, dup2s it
 * onto 0 and runs cat.
 *
 * So the authority moved to the KERNEL, which still knows what a descriptor is
 * after the image is replaced -- p9cl.c's v8fs_p9adopt asks getpeername.  This
 * table is what stops that being a syscall on every read.
 *
 * THREE STATES, WHERE THERE USED TO BE TWO.  Null now means UNEXAMINED rather
 * than passthrough, and a descriptor examined and found ordinary stores
 * &v8fs_pass.  Without that distinction the fallback would run on every read
 * of stdin forever, which is the hottest path in the system.
 */
#define V8FS_NFD	256
static struct v8fstyp *fdtyp[V8FS_NFD];

struct v8fstyp *
v8fs_fdtype(int fd)
{
	struct v8fstyp *t;

	if (fd < 0) return (&v8fs_pass);
	if (fd < V8FS_NFD && fdtyp[fd] != 0) return (fdtyp[fd]);
	/*
	 * Unexamined -- or above the table, which is not a limit on
	 * correctness here, only on caching.  v8fs_p9adopt returns 0 without a
	 * syscall when no mount is configured, so a world with no v8fs pays
	 * one predictable branch.
	 */
	if ((t = v8fs_p9adopt(fd)) == 0) t = &v8fs_pass;
	if (fd < V8FS_NFD) fdtyp[fd] = t;
	return (t);
}

void
v8fs_bind(int fd, struct v8fstyp *t)
{
	if (fd >= 0 && fd < V8FS_NFD) fdtyp[fd] = t;
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

	/*
	 * V8P_ENTRY for the first call and not V8P_LOOK -- §8a step 5i, and
	 * mkpath() in syscall.c makes the same correction for the same reason:
	 * what is wanted here is LOOK's UNION rule with MAKE's FOLD, and that
	 * is exactly what V8P_ENTRY is.  Folding one call's path whole and the
	 * other's all-but-the-last would ask "does it exist" about a different
	 * name from the one being created.
	 */
	if (mode != V8P_MAKE) return v8sys_rootpath(p, mode);
	q = v8sys_rootpath(p, V8P_ENTRY);
	if (v8sys_rootjailed(q)) return (q);
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

/*
 * THE THREE §8a STEP 5f ADDED, and for passthrough each is the host syscall it
 * always was -- these are not new behaviour, they are the behaviour syscall.c
 * had inline moved behind the switch so that a second type can have its own.
 *
 * THE PATH ARRIVES ALREADY RESOLVED, exactly as t_stat's does, which is what
 * makes them safe to call from here: the caller chose the type and resolved
 * the path in one step.  Note the ASYMMETRY that is not an oversight --
 * pt_remove's unlink arm takes a LOOK path and its rmdir arm takes one too,
 * while pt_mkdir needs a MAKE path, because the name it is given does not
 * exist yet.  syscall.c is where that choice is made and it is the same
 * choice v8s_unlink and v8s_mkdir were already making inline.
 */
static int
pt_access(char *rp, int mode)
{
	RET(rawsys2(SYS_access, (long)rp, mode));
}

/*
 * isdir < 0 IS "NO OPINION", AND ON THIS HOST IT NEEDS ONE.
 *
 * V7's unlink(2) removes a directory entry of ANY kind if the caller is
 * privileged enough -- that is how rmdir(1) works, and why it was setuid root.
 * macOS refuses a directory outright, so the choice V7 does not make has to be
 * made somewhere, and the somewhere is HERE rather than in v8s_unlink: it is
 * a fact about the host filesystem, which is what this type is.
 *
 * IT MOVED HERE UNCHANGED FROM v8s_unlink, §8a step 5f-b's follow-on, and the
 * move is what gave this function's unlink arm a caller at all.  v8s_rmdir
 * always passes 1 and v8s_unlink did not dispatch, so the arm below was dead
 * -- the recorded v8s_creat shape, "path resolution without dispatch, so no
 * second type could ever see it".  An auditor found it; nothing misbehaved,
 * because pt_path(p, V8P_LOOK) is vpath(p) and the two roads met.
 *
 * lstat AND NOT stat, which is the same choice the code made before the move:
 * unlinking a symlink to a directory removes the LINK, so following it here
 * would pick rmdir and fail with ENOTDIR on a name that unlink handles.
 */
static int
pt_remove(char *rp, int isdir)
{
	struct v8_stat st;

	if (isdir < 0 && v8sys_pt_stat(rp, &st, 0) == 0 &&
	    (st.st_mode & V8_S_IFMT) == V8_S_IFDIR)
		isdir = 1;
	if (isdir > 0) RET(rawsys1(SYS_rmdir, (long)rp));
	RET(rawsys1(SYS_unlink, (long)rp));
}

static int
pt_mkdir(char *rp, int mode)
{
	RET(rawsys2(SYS_mkdir, (long)rp, mode & 07777));
}

static int
pt_chmod(char *rp, int mode)
{
	RET(rawsys2(SYS_chmod, (long)rp, mode));
}
/*
 * BOTH NAMES ARE ALREADY RESOLVED, which is what makes this one line.  The
 * host's own link(2) supplies every refusal that matters here -- EXDEV for two
 * host filesystems, EPERM for a directory, EEXIST, EMLINK -- so there is
 * nothing for this type to decide.  The interesting arm is the cross-TYPE one
 * and it never reaches here: v8s_link answers that with EXDEV before it
 * dispatches.
 */
static int
pt_link(char *rold, char *rnew)
{
	RET(rawsys2(SYS_link, (long)rold, (long)rnew));
}

static int
pt_chown(char *rp, int uid, int gid)
{
	RET(rawsys3(SYS_chown, (long)rp, uid, gid));
}

/*
 * pt_utime.  The conversion from V7's time_t[2] to the host's two timevals is
 * the body of what v8s_utime used to be, moved here unchanged -- it is
 * passthrough's business and not the dispatcher's, which is the same move
 * ioctl.c's sgtty translation made when t_ioctl arrived.
 *
 * A NULL tv IS "NOW", which is macOS's reading of a null timeval pointer and
 * not a VAX's -- see the long note beside p9_t_utime, which reproduces this
 * answer rather than the 1985 one so that the two types agree.
 */
static int
pt_utime(char *rp, long *tv)
{
	struct { long sec, usec; } t[2];

	if (tv == 0) RET(rawsys2(SYS_utimes, (long)rp, 0));
	t[0].sec = tv[0]; t[0].usec = 0;
	t[1].sec = tv[1]; t[1].usec = 0;
	RET(rawsys2(SYS_utimes, (long)rp, (long)t));
}

struct v8fstyp v8fs_pass = {
	"pass",
	pt_path,
	pt_open, pt_close,
	pt_read, pt_write, pt_seek,
	v8sys_pt_stat, v8sys_pt_fstat,
	v8sys_pt_ioctl,
	pt_access, pt_remove, pt_mkdir,
	pt_chmod, pt_chown, pt_utime,
	pt_link
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

/*
 * /dev/fd INHERITS ALL THREE, which is this type's whole argument arriving in
 * three more slots.  A name under /dev/fd is a DESCRIPTOR NUMBER, and the
 * directory it lives in is the host's real /dev/fd -- so `access("/dev/fd/1",
 * R_OK)' is a question the host can answer and `unlink("/dev/fd/1")' is one
 * the host will refuse, both correctly and for the right reason.  Giving this
 * type implementations of its own would be inventing a difference the kernel
 * does not have, which is exactly what fd_open declines to do by not calling
 * v8fs_bind().
 */
struct v8fstyp v8fs_fdfs = {
	"fd",
	fd_path,
	fd_open, pt_close,
	pt_read, pt_write, pt_seek,
	fd_stat, v8sys_pt_fstat,
	v8sys_pt_ioctl,
	pt_access, pt_remove, pt_mkdir,
	pt_chmod, pt_chown, pt_utime,
	pt_link
};
