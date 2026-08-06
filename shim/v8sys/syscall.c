/*
 * The syscall stubs themselves: what `chmk` used to reach.
 *
 * One function per V8 libc stub, named exactly as the assembly stub exported it
 * (open, read, write, ...) so V8's libc links against these unchanged.  The
 * survey in PLAN.md enumerates all 63; they fall into four groups, and each
 * function below says which one it is in:
 *
 *	PASSTHROUGH	semantics identical; call the host and map errno
 *	TRANSLATE	a struct or value changes shape at the boundary
 *	EMULATE		no host equivalent; built here
 *	ENOSYS		belongs to a kernel we are not porting
 */

/*
 * Only headers of CONSTANTS are included -- <errno.h> for the host's E* values
 * and <sys/syscall.h> (through rawsys.h) for the numbers.  No header that
 * declares a function we might accidentally call: see rawsys.h.
 */
#include <errno.h>
#include "v8sys.h"
#include "rawsys.h"

int v8_errno;

extern void v8sys_dirinit(void);
extern long v8sys_dirseek(int, long, int);
extern v8_ino_t v8sys_fold_ino(unsigned long long);
int v8s_fstat();
int v8s_stat();
int v8s_lstat();

/*
 * getenv, without libc.  crt0.s publishes environ; the shim may not call the
 * host's getenv (see rawsys.h), and V8's own libc getenv is above this seam.
 */
extern char **environ;

char *
v8sys_getenv(const char *name)
{
	char **e;
	int i;

	if (environ == 0) return (0);
	for (e = environ; *e; e++) {
		for (i = 0; name[i] && (*e)[i] == name[i]; i++)
			;
		if (name[i] == '\0' && (*e)[i] == '=')
			return (*e) + i + 1;
	}
	return (0);
}

/* ------------------------------------------------------ errno mapping */

/*
 * Host errno -> V8 errno.  The first 32 numbers were common Unix heritage and
 * mostly agree, but agreeing "mostly" is how a program ends up printing the
 * wrong message, so the table is explicit.  Anything with no V8 counterpart
 * becomes EIO: V8 had no vocabulary for it, and a program that prints
 * strerror() should say something true rather than index off the end of
 * sys_errlist.
 */
int
v8sys_errno(int e)
{
	switch (e) {
	case 0:			return 0;
	case EPERM:		return V8_EPERM;
	case ENOENT:		return V8_ENOENT;
	case ESRCH:		return V8_ESRCH;
	case EINTR:		return V8_EINTR;
	case EIO:		return V8_EIO;
	case ENXIO:		return V8_ENXIO;
	case E2BIG:		return V8_E2BIG;
	case ENOEXEC:		return V8_ENOEXEC;
	case EBADF:		return V8_EBADF;
	case ECHILD:		return V8_ECHILD;
	case EDEADLK:		return V8_EAGAIN;
	case ENOMEM:		return V8_ENOMEM;
	case EACCES:		return V8_EACCES;
	case EFAULT:		return V8_EFAULT;
#ifdef ENOTBLK
	case ENOTBLK:		return V8_ENOTBLK;
#endif
	case EBUSY:		return V8_EBUSY;
	case EEXIST:		return V8_EEXIST;
	case EXDEV:		return V8_EXDEV;
	case ENODEV:		return V8_ENODEV;
	case ENOTDIR:		return V8_ENOTDIR;
	case EISDIR:		return V8_EISDIR;
	case EINVAL:		return V8_EINVAL;
	case ENFILE:		return V8_ENFILE;
	case EMFILE:		return V8_EMFILE;
	case ENOTTY:		return V8_ENOTTY;
	case ETXTBSY:		return V8_ETXTBSY;
	case EFBIG:		return V8_EFBIG;
	case ENOSPC:		return V8_ENOSPC;
	case ESPIPE:		return V8_ESPIPE;
	case EROFS:		return V8_EROFS;
	case EMLINK:		return V8_EMLINK;
	case EPIPE:		return V8_EPIPE;
	case EDOM:		return V8_EDOM;
	case ERANGE:		return V8_ERANGE;
	case ELOOP:		return V8_ELOOP;
	case EAGAIN:		return V8_EAGAIN;
	case ENAMETOOLONG:	return V8_ENOENT;   /* V8 had no such error */
	case ENOSYS:		return V8_EINVAL;
	}
	return V8_EIO;
}

/*
 * Retained for callers that still have a host errno in hand.  With raw
 * syscalls nothing sets the host errno any more -- the error arrives in the
 * return value -- so RET() and RAWERR() are the normal path and this is only
 * used where a value was captured explicitly.
 */
int
v8sys_faile(int hosterr)
{
	v8_errno = v8sys_errno(hosterr);
	return (-1);
}

/*
 * An EMPTY PATH MEANS THE CURRENT DIRECTORY.
 *
 * V7's namei() resolved "" to the working directory, and V8 code uses that:
 * rmdir(1) stats the parent with
 *
 *	if (stat("", &cst) < 0)
 *
 * and there is no other way it could have meant.  POSIX made the empty path an
 * error (ENOENT), so on macOS that branch always fired and rmdir refused to
 * remove anything -- with a garbled message, because the line beneath it has a
 * quoting bug of its own that is genuinely upstream's:
 *
 *	fprintf(stderr, "%s: cannot stat \", cmdname\"");
 *
 * Translating here rather than patching each caller: it is a kernel behaviour,
 * and standing in for the kernel is what this file does.  Same class as V7
 * reading address 0 as zero -- an assumption the tree makes silently.
 */
/*
 * V8 SYSTEM PATHS RESOLVE INSIDE $V8ROOT.
 *
 * V8 programs name their data by absolute path -- nroff opens
 * "/usr/lib/term/tab.37", troff "/usr/lib/font/...", and eqn, tbl, refer and
 * man all reach for "/usr/lib/tmac/..." the same way.  Those directories cannot
 * be created on macOS: /usr is protected by SIP.
 *
 * The rootfs is exactly the V8-shaped tree those paths describe (PLAN.md S3),
 * so a path under one of the V8 data directories is looked for there first and
 * used if it exists.  Anything else, and anything the rootfs does not have, is
 * passed through untouched -- so /usr/bin/whatever still means the host's.
 *
 * Doing it here rather than per program is the point: it is one rule, it leaves
 * every source file unmodified, and it is what having a rootfs is for.
 */
/*
 * /bin/ and /usr/bin/ are on this list, which makes the rootfs a chroot rather
 * than just a data directory: a V8 program that execs /bin/sh gets V8's sh, and
 * the shell it starts finds V8's cp, rm and cc.  That is what lets V8's make
 * drive a build in which every command is V8 code.
 *
 * chroot(2) itself is not available to this port.  Every V8 binary here is a
 * Mach-O linked against /usr/lib/libSystem.B.dylib, so a real chroot would need
 * dyld and the dyld shared cache inside the jail, and that cache is protected
 * by SIP; chroot also needs root, and building as root is worse than the
 * problem.  But this file IS the kernel as far as V8 code is concerned, and
 * chroot is a kernel service, so it belongs here.
 */
static const char *v8dirs[] = {
	"/usr/lib/", "/usr/share/", "/usr/dict/", "/lib/", "/usr/pub/",
	"/bin/", "/usr/bin/", "/etc/", "/usr/man/", "/usr/spool/",
	/*
	 * /dev/ is here for the grovelers: load(1) opens /dev/kmem, which
	 * libkmemu manufactures.  It does NOT capture /dev/null or /dev/tty,
	 * by the same mechanism that protects every other entry -- a path whose
	 * rootfs copy does not exist falls through to the host, and the rootfs
	 * has no null or tty.  Worth knowing rather than assuming: create
	 * rootfs/dev/tty and the V8 world would stop seeing the real terminal.
	 */
	"/dev/", 0
};

/*
 * EXACT matches, not prefixes.  /unix is a file at the root, and there is no
 * way to spell that in the list above: an entry of "/unix" without a trailing
 * slash matches by prefix, so it would also claim /unixfoo.  The distinction is
 * cheap to make and the alternative is a rule that is subtly wrong for names
 * nobody has created yet.
 *
 * /unix is the kernel namelist libkmemu writes -- see kmem.c.  nlist(3) reads
 * it to find _avenrun before load(1) seeks to that address in /dev/kmem.
 */
static const char *v8files[] = {
	"/unix", 0
};

/*
 * Where the V8 world is.
 *
 * $V8ROOT first, then a root compiled in at build time.  The env var alone was
 * a silent fall-through of exactly the kind this port keeps paying for: with it
 * unset, rootpath() quietly returned the HOST path, so a V8 binary run outside
 * the launcher operated on the real filesystem and looked like it was working.
 * That is the same shape as the variadic libc gaps -- a missing thing filled in
 * by the host, discovered only by its consequences.
 *
 * V8ROOT_DEFAULT is stamped in by the Makefile (and by `make install`, at the
 * install prefix).  An installed binary therefore knows its own world without
 * anyone having to remember to export anything, and the env var stays as the
 * override that lets a second tree be tested.
 */
#ifndef V8ROOT_DEFAULT
#define V8ROOT_DEFAULT ""
#endif

static char *
v8root(void)
{
	static const char dflt[] = V8ROOT_DEFAULT;
	char *r = v8sys_getenv("V8ROOT");

	if (r != 0 && *r != '\0') return (r);
	if (dflt[0] != '\0') return ((char *)dflt);
	return (0);
}

/*
 * ROOTPATH HAS TWO MODES, AND FOR A LONG TIME IT HAD ONLY THE FIRST.
 *
 * V8P_LOOK is the union rule and is what every reader wants: a path under one
 * of the directories above resolves into $V8ROOT *if the rootfs copy exists*,
 * and otherwise falls through to the host, so the Mac's /usr/lib is still
 * visible through the gaps in ours.
 *
 * That rule cannot resolve a path that DOES NOT EXIST YET, which means it
 * cannot resolve anything being created -- and creation is half of what a
 * filesystem is for.  Measured: creat("/etc/anything") inside the jail went to
 * the HOST's /etc, and on macOS was refused with EACCES.  So a V8 program could
 * read /etc/group and not write it, and the failure looked like a permission
 * problem rather than a missing jail.  On a host directory that happened to be
 * writable it would not have failed at all; it would have written outside.
 *
 * V8P_MAKE keys on the PARENT instead: if $V8ROOT/etc exists, then /etc/newfile
 * resolves inside the jail whether or not it is there yet.  Reads are untouched
 * because readers still pass V8P_LOOK.
 */
#define V8P_LOOK	0		/* redirect if the path itself is there */
#define V8P_MAKE	1		/* redirect if its PARENT is there */

static char *
rootpath(char *p, int mode)
{
	static char buf[1024];
	char *root;
	int i, n, m;

	if (p == 0 || *p != '/') return (p);

	/*
	 * "/" IS THE JAIL ROOT.  The most basic thing a chroot does, and it was
	 * missing: every other path was resolved inside $V8ROOT while the root
	 * itself still meant the host's.
	 *
	 * It is what makes pwd tell the truth.  getwd() in src/libc/gen/getwd.c
	 * does not ask the kernel for a path -- there is no such syscall in V8 --
	 * it stats "/" to learn the root's dev/ino and then walks ".." upwards
	 * until it matches, assembling the name from directory entries.  With "/"
	 * meaning the host's root, that walk ran past $V8ROOT and printed
	 * /Users/.../rootfs/bin for a directory the V8 world calls /bin.
	 *
	 * No existence check here, unlike below: $V8ROOT is the root by
	 * definition, and if it does not exist nothing else works either.
	 */
	if (p[1] == '\0') {
		if ((root = v8root()) == 0) return (p);
		for (n = 0; root[n] && n < (int)sizeof buf - 1; n++) buf[n] = root[n];
		buf[n] = '\0';
		return (buf);
	}

	for (i = 0; v8dirs[i]; i++) {
		const char *d = v8dirs[i];
		int k;
		for (k = 0; d[k] && p[k] == d[k]; k++)
			;
		if (d[k] == '\0') break;		/* matched a V8 directory */
		/*
		 * ...and the directory ITSELF, spelled without the trailing
		 * slash.  The entries in v8dirs carry one so that "/binary" is
		 * not mistaken for "/bin/", but that also meant "/etc" and
		 * "/bin" matched nothing, so `ls /etc` listed the Mac's while
		 * `cat /etc/group` read V8's -- the same path naming two
		 * different worlds depending on a trailing character.
		 */
		if (d[k] == '/' && d[k + 1] == '\0' && p[k] == '\0') break;
	}
	if (v8dirs[i] == 0) {
		/* No directory claimed it; try the exact-match list. */
		for (i = 0; v8files[i]; i++) {
			const char *f = v8files[i];
			int k;
			for (k = 0; f[k] && p[k] == f[k]; k++)
				;
			if (f[k] == '\0' && p[k] == '\0') break;
		}
		if (v8files[i] == 0) return (p);
	}
	if ((root = v8root()) == 0) return (p);

	for (n = 0; root[n] && n < (int)sizeof buf - 2; n++) buf[n] = root[n];
	for (m = 0; p[m] && n < (int)sizeof buf - 1; m++) buf[n++] = p[m];
	buf[n] = '\0';
	/*
	 * access(2) raw, NOT v8s_stat: v8s_stat runs its argument back through
	 * vpath(), which re-enters this function and would overwrite the buf we
	 * are still building.  It happens to be harmless today only because
	 * $V8ROOT does not itself start with a V8 directory prefix -- set
	 * V8ROOT=/usr/lib/anything and it would clobber.  Not worth leaving to
	 * luck for a call that only asks whether the file exists.
	 */
	if (mode == V8P_MAKE) {
		/*
		 * Ask about the parent, by cutting the buffer at the last slash
		 * and putting it back.  n is the length built above, and the
		 * root's own slash is always at or before buf[strlen(root)], so
		 * there is one to find.
		 */
		int cut = -1, ok;
		for (i = 0; i < n; i++)
			if (buf[i] == '/') cut = i;
		if (cut <= 0) return (p);
		buf[cut] = '\0';
		ok = rawsys2(SYS_access, (long)buf, 0) == 0;
		buf[cut] = '/';
		return (ok ? buf : p);
	}
	if (rawsys2(SYS_access, (long)buf, 0) == 0) return (buf);
	return (p);
}

/* The reader's path: today's rule, unchanged. */
static char *
vpath(char *p)
{
	if (p && *p == '\0') return ".";
	return rootpath(p, V8P_LOOK);
}

/*
 * The creator's path.  V8P_LOOK first, so that an existing rootfs file still
 * wins and nothing about the union's read behaviour changes; only when the
 * rootfs does not already have the path does the parent rule decide.  Every
 * syscall that can bring a name into existence uses this -- open with O_CREAT,
 * creat, mkdir, mknod, and the NEW name of link and symlink.
 */
static char *
mkpath(char *p)
{
	char *q;

	if (p && *p == '\0') return ".";
	q = rootpath(p, V8P_LOOK);
	if (q != p) return (q);
	return rootpath(p, V8P_MAKE);
}

/* ------------------------------------------------------- PASSTHROUGH */

/*
 * Every one of these goes straight to the kernel.  See rawsys.h for why the
 * shim may not call the host's libc: it would bind to its own definition of the
 * same name and recurse.
 */

#define RET(r)	do { long _r = (r); \
		     if (_r < 0) { v8_errno = v8sys_errno(RAWERR(_r)); return (-1); } \
		     return (_r); } while (0)

int v8s_close(int fd)
{
	if (v8sys_isdirfd(fd)) v8sys_dirclose(fd);
	RET(rawsys1(SYS_close, fd));
}

long v8s_write(int fd, char *b, long n)  { RET(rawsys3(SYS_write, fd, (long)b, n)); }
/*
 * link.  One special case, for V7 directory semantics.
 *
 * V7 had no mkdir(2).  mkdir(1) made a directory with mknod(2) and then linked
 * its own "." and ".." into place by hand -- which is why it was setuid root.
 * V8's mkdir.c still does exactly that, and the host will not: macOS refuses to
 * hard-link a directory at all, and its mkdir(2) has already created both
 * entries anyway.
 *
 * So a link whose target basename is "." or ".." inside a directory succeeds
 * and does nothing.  It is not a fiction: the entry the caller is asking for
 * already exists, with the meaning it wanted.  Anything else is a real link.
 */
static int
dotlink(const char *b)
{
	const char *p, *base;

	for (p = base = b; *p; p++)
		if (*p == '/') base = p + 1;
	return (base[0] == '.' &&
	    (base[1] == '\0' || (base[1] == '.' && base[2] == '\0')));
}

int v8s_link(char *a, char *b)
{
	char old[1024];
	char *q;
	int i;

	if (dotlink(b)) {
		struct v8_stat st;

		/* only if the directory really is there */
		if (v8s_stat(a, &st) == 0 &&
		    (st.st_mode & V8_S_IFMT) == V8_S_IFDIR)
			return (0);
	}
	/*
	 * BOTH names were unresolved until now, so `ln /bin/cat x' linked the
	 * MAC's /bin/cat -- read the same name with open(2) and you got the
	 * jail's.  The existing name takes the reader's rule and the new name
	 * the creator's, which is the same split rename(2) would want.
	 *
	 * COPIED, and that is the aliasing trap CLAUDE.md names: rootpath()
	 * returns a pointer into its own static buffer, so holding two results
	 * at once silently gives you the same string twice.  Here that would
	 * have been link(new, new).
	 */
	q = vpath(a);
	for (i = 0; q[i] && i < (int)sizeof old - 1; i++) old[i] = q[i];
	old[i] = '\0';
	RET(rawsys2(SYS_link, (long)old, (long)mkpath(b)));
}

/*
 * mknod.  V7's way of creating a directory, and the reason mkdir(1) was setuid.
 *
 * Nothing else in the tree makes a device node that we can honour -- the host
 * would refuse, and a V8 program has no business creating one on a Mac -- so
 * the only mode that does anything here is S_IFDIR, which becomes mkdir(2).
 * Without this, mknod fell through to libSystem's, which needs root for a
 * directory, and mkdir(1) reported "cannot make directory".
 */
int v8s_mknod(char *p, int mode, int dev)
{
	if ((mode & 0170000) == 0040000)
		RET(rawsys2(SYS_mkdir, (long)p, mode & 07777));
	v8_errno = v8sys_errno(EPERM);
	return (-1);
}
/*
 * unlink.  The mirror of mknod/link above, completing V7's directory story.
 *
 * V7 had no rmdir(2) either.  rmdir(1) takes a directory apart by hand:
 *
 *	unlink("d/..");  unlink("d/.");  unlink("d");
 *
 * which is why it, like mkdir(1), was setuid root.  macOS will do none of it --
 * unlink refuses a directory outright, and the dot entries are not separately
 * removable.
 *
 * So the two dot entries succeed and do nothing (the host removes them with the
 * directory), and unlinking the directory itself becomes rmdir(2).  A plain
 * file is still a plain unlink.
 */
int v8s_unlink(char *p)
{
	struct v8_stat st;

	p = vpath(p);
	if (dotlink(p)) {
		/*
		 * "d/." or "d/.." -- succeed only if the directory is really
		 * there, so a genuine mistake still reports ENOENT.
		 */
		char parent[1024];
		int i, base = 0;

		for (i = 0; p[i] && i < (int)sizeof parent - 1; i++)
			if (p[i] == '/') base = i;
		for (i = 0; i < base; i++) parent[i] = p[i];
		parent[base] = '\0';
		if (base == 0) return (0);		/* bare "." or ".." */
		if (v8s_stat(parent, &st) == 0 &&
		    (st.st_mode & V8_S_IFMT) == V8_S_IFDIR)
			return (0);
	}
	if (v8s_lstat(p, &st) == 0 && (st.st_mode & V8_S_IFMT) == V8_S_IFDIR)
		RET(rawsys1(SYS_rmdir, (long)p));
	RET(rawsys1(SYS_unlink, (long)p));
}
int v8s_chdir(char *p)                   { RET(rawsys1(SYS_chdir, (long)vpath(p))); }
int v8s_chmod(char *p, int m)            { RET(rawsys2(SYS_chmod, (long)vpath(p), m)); }
int v8s_chown(char *p, int u, int g)     { RET(rawsys3(SYS_chown, (long)vpath(p), u, g)); }
int v8s_fchmod(int f, int m)             { RET(rawsys2(SYS_fchmod, f, m)); }
int v8s_fchown(int f, int u, int g)      { RET(rawsys3(SYS_fchown, f, u, g)); }
int v8s_access(char *p, int m)           { RET(rawsys2(SYS_access, (long)vpath(p), m)); }
int v8s_mkdir(char *p, int m)            { RET(rawsys2(SYS_mkdir, (long)mkpath(p), m)); }
int v8s_rmdir(char *p)                   { RET(rawsys1(SYS_rmdir, (long)vpath(p))); }
/* a is the link TEXT and is stored verbatim -- resolving it would bake this
 * machine's rootfs path into a symlink the jail is supposed to interpret for
 * itself.  Only the new name is resolved. */
int v8s_symlink(char *a, char *b)        { RET(rawsys2(SYS_symlink, (long)a, (long)mkpath(b))); }
int v8s_dup(int f)                       { RET(rawsys1(SYS_dup, f)); }
int v8s_dup2(int a, int b)               { RET(rawsys2(SYS_dup2, a, b)); }
int v8s_getpid(void)                     { return ((int)rawsys0(SYS_getpid)); }
int v8s_getppid(void)                    { return ((int)rawsys0(SYS_getppid)); }
int v8s_getuid(void)                     { return ((int)rawsys0(SYS_getuid)); }
int v8s_geteuid(void)                    { return ((int)rawsys0(SYS_geteuid)); }
int v8s_getgid(void)                     { return ((int)rawsys0(SYS_getgid)); }
int v8s_getegid(void)                    { return ((int)rawsys0(SYS_getegid)); }
int v8s_setuid(int u)                    { RET(rawsys1(SYS_setuid, u)); }
int v8s_setgid(int g)                    { RET(rawsys1(SYS_setgid, g)); }
int v8s_umask(int m)                     { return ((int)rawsys1(SYS_umask, m)); }
int v8s_sync(void)                       { rawsys0(SYS_sync); return (0); }
int v8s_chroot(char *p)                  { RET(rawsys1(SYS_chroot, (long)p)); }
/*
 * execve is where the jail is either real or theatre.  It used to pass its path
 * straight to the kernel with no translation at all, so no matter what the
 * rootfs contained, /bin/sh meant the host's shell and /usr/bin/cc meant the
 * host's compiler.  Routing it through vpath() is what makes the rootfs a root
 * filesystem rather than a data directory.
 *
 * WHEN A PATH FALLS THROUGH.  rootpath() returns the original when the rootfs
 * does not have the file, so an unported tool still runs the host's.  That
 * keeps the port usable while /bin is incomplete, but it is exactly the shape
 * of the bug that has already cost this port three debugging rounds: a gap
 * filled silently by the host, discovered only by its consequences.  scanf,
 * printf and execl each did that at the libc layer.
 *
 * So the fall-through is reported.  V8JAIL=warn names each host binary reached;
 * V8JAIL=strict refuses to exec it at all, which is what the build should run
 * under once /bin is complete.  Unset stays quiet, for a tree that is still
 * being ported.
 *
 * THREE THINGS, NOT TWO.  rootpath() leaving a path alone is not by itself an
 * escape, and treating it as one made the report useless the moment the C
 * driver became a V8 binary:
 *
 *   1. A path already INSIDE the rootfs.  cc(1) resolves its own passes from
 *      $V8ROOT at startup, so it execs the absolute /path/to/rootfs/lib/cpp --
 *      which rootpath() has no reason to touch, since it is already there.
 *      `cc -c` under V8JAIL=warn duly reported cpp and ccom as escaping into
 *      the jail they were sitting in.  injail() is that check.
 *
 *   2. A host tool on the EXCEPTION LIST.  as, ld, ar, strip and nm are the
 *      host's deliberately -- the object format is Mach-O, and porting V8's
 *      a.out assembler and link editor would buy no authenticity (PLAN.md S1).
 *      cc reaches them through clang.  That crossing is real, so warn names it;
 *      but it is sanctioned, so strict permits it.  Without this the compiler
 *      simply cannot link inside its own jail.
 *
 *   3. Anything else.  A gap filled silently by the host -- the shape of bug
 *      this port has already paid for three times.  strict refuses it.
 *
 * Keeping 2 and 3 apart is the whole value of the report.  A jail that cannot
 * say which crossings it meant to allow reports either everything or nothing.
 */
static int
injail(char *p)
{
	char *root = v8root();
	int i;

	if (root == 0 || *root == '\0') return (0);
	for (i = 0; root[i]; i++)
		if (p[i] != root[i]) return (0);
	return (p[i] == '/');		/* $V8ROOTsomething is not inside it */
}

/*
 * The exception list, spelled as the paths cc(1) actually execs.  Short on
 * purpose: every entry here is a hole in the jail.  tests/jail asserts that a
 * host tool which is NOT on this list is still refused under strict.
 */
static const char *hosttools[] = { "/usr/bin/clang", 0 };

static int
sanctioned(char *p)
{
	int i, k;

	for (i = 0; hosttools[i]; i++) {
		for (k = 0; hosttools[i][k] && p[k] == hosttools[i][k]; k++)
			;
		if (hosttools[i][k] == '\0' && p[k] == '\0') return (1);
	}
	return (0);
}

static void
jailsay(const char *pre, int prelen, char *p, const char *post, int postlen)
{
	int n;

	rawsys3(SYS_write, 2, (long)pre, (long)prelen);
	for (n = 0; p[n]; n++)
		;
	rawsys3(SYS_write, 2, (long)p, (long)n);
	rawsys3(SYS_write, 2, (long)post, (long)postlen);
}

/* sizeof works here because the literals are substituted textually. */
#define	SAY(pre, p, post) \
	jailsay(pre, (int)sizeof (pre) - 1, p, post, (int)sizeof (post) - 1)

/*
 * A #! SCRIPT IS RUN BY THE SHIM, NOT BY THE HOST KERNEL.
 *
 * The kernel resolves a shebang before this port gets a say, and against the
 * REAL filesystem: /usr/bin/man opens `#!/bin/sh`, so XNU ran the Mac's shell,
 * which looked for the Mac's /usr/man and ran the Mac's commands, never calling
 * rootpath() once.  man printed "cat not found" for a page in the rootfs, and
 * every shell script in the world had been leaving the jail the same way --
 * invisibly, because a script that works looks like a script that works.
 *
 * So the interpreter line is read here and the exec rewritten through vpath(),
 * which is what a kernel does; the shim is this port's kernel.  `#!/bin/sh`
 * then means V8's sh.
 *
 * THE SCRIPT PATH IS COPIED, and that is not defensive tidiness -- it is the
 * bug that broke the first version of this.  rootpath() returns a pointer into
 * its OWN STATIC BUFFER, so calling vpath() a second time to resolve the
 * interpreter overwrites the buffer the argv is still holding as the script
 * name.  argv[1] and the interpreter path became the same string, and V8's sh
 * was handed itself to interpret.
 *
 * Minimal, matching V7: interpreter plus at most one argument, no recursion
 * into a second script, and any failure falls through to the kernel unchanged
 * rather than inventing an error.
 */
static int
shebang(char *p, char *buf, int n, char **interp, char **arg)
{
	int fd, i, k;

	*interp = *arg = 0;
	if ((fd = (int)rawsys3(SYS_open, (long)p, 0, 0)) < 0) return (0);
	k = (int)rawsys3(SYS_read, fd, (long)buf, (long)n - 1);
	rawsys1(SYS_close, fd);
	if (k < 4 || buf[0] != '#' || buf[1] != '!') return (0);
	buf[k] = '\0';
	for (i = 2; buf[i] == ' ' || buf[i] == '\t'; i++)
		;
	if (buf[i] == '\0' || buf[i] == '\n') return (0);
	*interp = buf + i;
	for (; buf[i] && buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\n'; i++)
		;
	if (buf[i] == ' ' || buf[i] == '\t') {
		buf[i++] = '\0';
		while (buf[i] == ' ' || buf[i] == '\t') i++;
		if (buf[i] && buf[i] != '\n') {
			*arg = buf + i;
			for (; buf[i] && buf[i] != '\n'; i++)
				;
		}
	}
	buf[i] = '\0';
	return (1);
}

int v8s_execve(char *p, char **a, char **e)
{
	char *q = vpath(p);
	static char shbuf[512];
	static char script[1024];
	static char *nav[260];
	char *interp, *sharg;

	if (shebang(q, shbuf, (int)sizeof shbuf, &interp, &sharg)) {
		int n = 0, i;

		/* copy BEFORE the second vpath() -- see the note above */
		for (i = 0; q[i] && i < (int)sizeof script - 1; i++)
			script[i] = q[i];
		script[i] = '\0';

		nav[n++] = interp;
		if (sharg) nav[n++] = sharg;
		nav[n++] = script;
		for (i = 1; a != 0 && a[i] != 0 && n < 258; i++)
			nav[n++] = a[i];
		nav[n] = 0;
		p = interp;
		a = nav;
		q = vpath(interp);		/* #!/bin/sh must mean V8's sh */
	}

	if (q == p && p != 0 && *p == '/' && !injail(p)) {
		char *j = v8sys_getenv("V8JAIL");
		if (j != 0 && *j != '\0') {
			if (sanctioned(p)) {
				/*
				 * Named under warn, silent under strict.  strict
				 * output is meant to mean "something is wrong",
				 * and a line per compiled file would drown that.
				 */
				if (j[0] != 's')
					SAY("v8sys: sanctioned host toolchain: ",
					    p, " (as/ld are the host's by design)\n");
			} else if (j[0] == 's') {
				SAY("v8sys: exec leaves the jail: ", p,
				    " (refused: V8JAIL=strict)\n");
				v8_errno = V8_ENOENT;
				return (-1);
			} else {
				SAY("v8sys: exec leaves the jail: ", p,
				    " (V8JAIL=strict would refuse)\n");
			}
		}
	}
	RET(rawsys3(SYS_execve, (long)q, (long)a, (long)e));
}

long v8s_readlink(char *p, char *b, long n)
{ RET(rawsys3(SYS_readlink, (long)p, (long)b, n)); }

/*
 * fork.  V8's vfork is the BSD one, which shared the address space until exec;
 * plain fork is always a correct substitute and never a dangerous one, so both
 * names map here.
 *
 * XNU's fork returns TWO values, like pipe below, and the second one is what
 * tells parent from child.  x0 holds a pid in both processes -- the child's pid
 * in the parent, and the PARENT's pid in the child -- and x1 is 0 in the parent
 * and 1 in the child.  Reading only x0, as rawsys0 does, makes the child
 * believe it is the parent: both carry on as the shell, neither ever execs, and
 * the real parent waits forever.  That is exactly how it presented -- the
 * Bourne shell parsed `echo hello`, forked, and sat in wait(2):
 *
 *	main -> exfile -> execute -> await -> wait -> v8s_wait -> rawsys4
 *
 * so the whole shell was working and only this was wrong.
 */
int
v8s_fork(void)
{
#if defined(__APPLE__)
	register long r0 __asm__("x0");
	register long r1 __asm__("x1");
	register long x16 __asm__("x16") = SYS_fork;
	register long err __asm__("x8");

	__asm__ volatile("svc #0x80\n\tcset x8, cs"
	    : "=r"(r0), "=r"(r1), "=r"(err) : "r"(x16) : "memory", "cc");
	if (err) { v8_errno = v8sys_errno((int)r0); return (-1); }
	return (r1 ? 0 : (int)r0);	/* the child sees 0, as Unix promises */
#else
	RET(rawsys0(SYS_fork));
#endif
}

int v8s_vfork(void)                      { return (v8s_fork()); }

/*
 * pipe.  The macOS pipe syscall returns the two descriptors in x0 and x1 rather
 * than writing through a pointer, so the register pair has to be captured
 * directly -- calling rawsys0 would throw x1 away.
 */
int
v8s_pipe(int fd[2])
{
	register long r0 __asm__("x0");
	register long r1 __asm__("x1");
#if defined(__APPLE__)
	register long x16 __asm__("x16") = SYS_pipe;
	register long err __asm__("x8");

	__asm__ volatile("svc #0x80\n\tcset x8, cs"
	    : "=r"(r0), "=r"(r1), "=r"(err) : "r"(x16) : "memory", "cc");
	if (err) { v8_errno = v8sys_errno((int)r0); return (-1); }
	fd[0] = (int)r0;
	fd[1] = (int)r1;
	return (0);
#else
	/* Linux has no bare pipe on arm64; pipe2 with no flags is the same. */
	RET(rawsys2(SYS_pipe2, (long)fd, 0));
#endif
}

/*
 * alarm and pause are setitimer/sigsuspend underneath on modern kernels.
 *
 * TV_USEC IS 32 BITS.  Darwin's struct timeval is { long tv_sec; int tv_usec; }
 * with four bytes of tail padding -- suseconds_t is __int32_t -- not the two
 * longs the rest of this file writes at it.  As a WRITE that shape is harmless:
 * the extra zeroes land in padding the kernel does not read, which is why it
 * has never mattered here.  Reading one back is where it would, so the struct
 * this call reads through is the real shape.  The class is the dominant one in
 * this port: a field that is not the width it looks like.
 */
struct v8_itimerval {
	struct { long sec; int usec; } interval, value;
};
_Static_assert(sizeof(struct v8_itimerval) == 32,
    "struct itimerval on Darwin/LP64: two 16-byte timevals, usec 32 bits + pad");

unsigned
v8s_alarm(unsigned sec)
{
	struct v8_itimerval nit, oit;
	unsigned rem;

	nit.interval.sec = 0; nit.interval.usec = 0;	/* one shot */
	nit.value.sec = sec;  nit.value.usec = 0;
	oit.value.sec = 0;    oit.value.usec = 0;

	if (rawsys3(SYS_setitimer, 0 /*ITIMER_REAL*/, (long)&nit, (long)&oit) < 0)
		return (0);

	/*
	 * alarm(2) OWES THE CALLER THE TIME LEFT ON THE PREVIOUS ALARM, and
	 * this returned 0 unconditionally -- setitimer's third argument, the
	 * one that reports the old value, was passed as 0 and thrown away.
	 *
	 * V8's sleep(3) is what that breaks: `altime = alarm(1000)' saves
	 * whatever alarm the caller had pending so `alarm(altime)' can put it
	 * back afterwards, and a constant 0 means every sleep() silently
	 * cancels its caller's alarm.
	 *
	 * A part-second rounds UP, as every alarm(3) since 4.2BSD has: an
	 * alarm with 0.3s to run has one second left, not none.  Truncating
	 * would lose up to a second on each save-and-restore.
	 */
	rem = (unsigned)oit.value.sec;
	if (oit.value.usec) rem++;
	return (rem);
}

/*
 * pause: sigsuspend with an empty mask -- wait for anything at all.
 *
 * THE SYSCALL TAKES THE MASK BY VALUE.  POSIX's sigsuspend() takes a
 * `const sigset_t *' and Darwin's does too, but the wrapper is where the
 * dereference happens: syscalls.master marks 111 NO_SYSCALL_STUB precisely
 * because the syscall's own argument is a `sigset_t', 32 bits wide, inherited
 * from 4.3BSD's sigpause(int sigmask).  This passed &mask, so the mask the
 * kernel installed was the low half of a STACK ADDRESS -- an arbitrary set of
 * blocked signals, stable within a build and different across machines.
 *
 * It mattered nowhere until now: no V8 program could catch a signal, so
 * nothing ever waited here and returned.  It matters immediately now, because
 * V8's sleep(3) is `for(;;) pause()' and a mask with bit 13 set blocks the very
 * SIGALRM it is waiting for -- a sleep that hangs on some machines and not
 * others, which is the same bug this change exists to remove, wearing a coin
 * flip.  tests/v8sys asserts that one pause() call blocks until a signal is
 * actually delivered, which is what fails if this is wrong in either direction:
 * a pointer the kernel reads as a mask hangs, and a mask the kernel reads as a
 * pointer faults straight back out without waiting.
 */
int
v8s_pause(void)
{
	rawsys1(SYS_sigsuspend, 0);
	v8_errno = V8_EINTR;
	return (-1);
}

int
v8s_nice(int inc)
{
	/* PRIO_PROCESS, this process */
	long cur = rawsys2(SYS_getpriority, 0, 0);
	RET(rawsys3(SYS_setpriority, 0, 0, cur + inc));
}

void v8s_exit(int c) { rawsys1(SYS_exit, c); for (;;) ; }

/* ---------------------------------------------------------- TRANSLATE */

/*
 * SYNTHETIC KERNEL FILES.  /etc/utmp is a file in every version of Unix and a
 * file here too, but nothing in this world writes one -- init and login are on
 * the kernel's side of this seam, and this file is that side.  libkmemu
 * manufactures it from the host's utmpx when a reader opens it, which is what
 * lets who(1) and w(1) compile unchanged: they fopen the path they always did.
 *
 * SUBSTITUTABLE, and that is the whole design.  libkmemu links the host's libc
 * -- the one sanctioned place in the shim that may -- so binding it into
 * libv8sys would put libc imports in EVERY V8 binary and quietly end the
 * freestanding guarantee that tests/freestanding asserts with `nm -u'.  So the
 * call here is unconditional and the DEFINITION is what varies: nokmemu.c in
 * this same library answers 0 to everything, and a program that links libkmemu
 * gets the real one instead.  nokmemu.c is a separate object for exactly the
 * reason libv8stubs.a is one object per syscall -- the linker takes a member
 * only for a symbol still unresolved, so the real implementation, already on
 * the line, wins and the stub is never pulled in.
 *
 * Not a weak reference, which is the obvious way to write this and does not
 * work: `extern int f() __attribute__((weak))' does emit an undefined weak
 * symbol -- nm confirms it -- and ld64 then refuses the link anyway unless
 * something defines f.  Measured here before this was rewritten.
 *
 * BEFORE vpath(), not after: rootpath() only redirects a path whose rootfs copy
 * already exists, so calling this afterwards would hand libkmemu the host's
 * /etc/utmp -- a path outside the jail, and on macOS not even a file.  The
 * point is to create the rootfs copy so the resolution that follows finds it.
 */
extern int kmemu_synth(const char *, const char *);

/*
 * open() is where directories are noticed.  Everything else about directory
 * reading follows from registering the shim here -- see dir.c.
 */
int
v8s_open(char *path, int flags, int mode)
{
	long fd;
	struct v8_stat st;

	kmemu_synth(path, v8root());
	path = vpath(path);

	v8sys_dirinit();
	/*
	 * V8's flags are the V7 originals -- 0 read, 1 write, 2 read/write --
	 * and O_CREAT/O_TRUNC/O_APPEND arrived later with the values macOS
	 * still uses, so nothing needs translating.
	 */
	fd = rawsys3(SYS_open, (long)path, flags, mode);
	if (fd < 0) { v8_errno = v8sys_errno(RAWERR(fd)); return (-1); }

	if (v8s_fstat((int)fd, &st) == 0 &&
	    (st.st_mode & V8_S_IFMT) == V8_S_IFDIR) {
		if (v8sys_diropen(path, (int)fd) < 0) {
			rawsys1(SYS_close, fd);
			return (-1);
		}
	}
	return ((int)fd);
}

/*
 * creat.  It resolved NOTHING before this, so every creat on a jailed path went
 * to the host: `creat("/etc/x")' inside the jail was refused by the Mac's /etc,
 * while open("/etc/x", 0) two lines later read the jail's copy.  The same name
 * meaning two different worlds depending on which syscall asked -- the same
 * shape as the /etc versus /etc/ bug in rootpath's own comment above.
 */
int
v8s_creat(char *path, int mode)
{
	RET(rawsys3(SYS_open, (long)mkpath(path),
	    0x0001 /*O_WRONLY*/ | 0x0200 /*O_CREAT*/ | 0x0400 /*O_TRUNC*/, mode));
}

long
v8s_read(int fd, char *buf, long n)
{
	if (v8sys_isdirfd(fd)) return (v8sys_dirread(fd, buf, n));
	RET(rawsys3(SYS_read, fd, (long)buf, n));
}

long
v8s_lseek(int fd, long off, int whence)
{
	if (v8sys_isdirfd(fd)) return (v8sys_dirseek(fd, off, whence));
	RET(rawsys3(SYS_lseek, fd, off, whence));
}

/*
 * struct stat.
 *
 * The kernel's layout is not the one <sys/stat.h> shows -- macOS's stat64
 * syscalls fill a fixed structure that the headers then reinterpret.  Rather
 * than duplicate that layout and track it across releases, the fields we need
 * are read out of a raw buffer at known offsets for the arm64 stat64 shape.
 *
 * st_ino is the interesting field: V8's is 16 bits and no modern filesystem's
 * is.  Folding loses uniqueness, so programs comparing inode numbers for
 * identity (find(1) looking for hard links) can see false matches.  Truncating
 * instead would collide far more often and could produce 0, which V7 uses to
 * mean "no entry".
 */
struct hoststat64 {			/* macOS arm64 struct stat64 */
	unsigned int	st_dev;
	unsigned short	st_mode;
	unsigned short	st_nlink;
	unsigned long long st_ino;
	unsigned int	st_uid;
	unsigned int	st_gid;
	unsigned int	st_rdev;
	long		st_atime_sec, st_atime_nsec;
	long		st_mtime_sec, st_mtime_nsec;
	long		st_ctime_sec, st_ctime_nsec;
	long		st_btime_sec, st_btime_nsec;
	long long	st_size;
	long long	st_blocks;
	int		st_blksize;
	unsigned int	st_flags;
	unsigned int	st_gen;
	int		st_lspare;
	long long	st_qspare[2];
};

static void
stat_translate(struct hoststat64 *hs, struct v8_stat *vs)
{
	int i;
	char *p = (char *)vs;

	for (i = 0; i < (int)sizeof *vs; i++) p[i] = 0;
	vs->st_dev   = (v8_dev_t)(hs->st_dev & 0xffff);
	vs->st_ino   = v8sys_fold_ino(hs->st_ino);
	vs->st_mode  = hs->st_mode;
	vs->st_nlink = (short)hs->st_nlink;
	vs->st_uid   = (short)hs->st_uid;
	vs->st_gid   = (short)hs->st_gid;
	vs->st_rdev  = (v8_dev_t)(hs->st_rdev & 0xffff);
	vs->st_size  = (v8_off_t)hs->st_size;
	vs->st_atime = (v8_time_t)hs->st_atime_sec;
	vs->st_mtime = (v8_time_t)hs->st_mtime_sec;
	vs->st_ctime = (v8_time_t)hs->st_ctime_sec;
}

/*
 * The host inode number of `name` inside the directory `dirfd`, or 0 if it
 * cannot be determined.
 *
 * dir.c needs this to keep a V7 invariant that macOS breaks: in a V7 filesystem
 * a directory entry's inode number IS the file's inode number, and half the
 * userspace depends on it -- getwd(3) walks up to the root matching entries
 * against stat("."), find(1) prunes by inode, ncheck(8) is nothing else.
 *
 * On APFS the two disagree at a firmlink.  /private is a firmlink onto the Data
 * volume, so readdir("/") reports the entry's inode on the System volume while
 * stat("/private") reports the target's:
 *
 *	private   readdir_ino=142645531   stat_ino=142646994
 *
 * getwd has a fallback for exactly this shape -- when the parent and the child
 * are on different devices it stats each name instead of trusting d_ino -- but
 * macOS reports the SAME st_dev on both sides of a firmlink (they are one
 * synthesised volume as far as stat is concerned), so the fallback never fires
 * and pwd(1) failed with "getwd: read error in ..".
 *
 * Reconciling here rather than working around it in getwd is the right place:
 * presenting a V7 filesystem is what this shim is for.
 */
unsigned long long
v8sys_host_ino_at(int dirfd, const char *name)
{
	struct hoststat64 hs;
	long r = rawsys4(SYS_fstatat64, dirfd, (long)name, (long)&hs, 0L);

	return (r < 0 ? 0ULL : hs.st_ino);
}

int v8s_stat(char *p, struct v8_stat *vs)
{
	struct hoststat64 hs;
	long r = rawsys2(SYS_stat64, (long)vpath(p), (long)&hs);
	if (r < 0) { v8_errno = v8sys_errno(RAWERR(r)); return (-1); }
	stat_translate(&hs, vs);
	return (0);
}

int v8s_lstat(char *p, struct v8_stat *vs)
{
	struct hoststat64 hs;
	long r = rawsys2(SYS_lstat64, (long)vpath(p), (long)&hs);
	if (r < 0) { v8_errno = v8sys_errno(RAWERR(r)); return (-1); }
	stat_translate(&hs, vs);
	return (0);
}

int v8s_fstat(int f, struct v8_stat *vs)
{
	struct hoststat64 hs;
	long r = rawsys2(SYS_fstat64, f, (long)&hs);
	if (r < 0) { v8_errno = v8sys_errno(RAWERR(r)); return (-1); }
	stat_translate(&hs, vs);
	return (0);
}

long
v8s_time(long *tp)
{
	struct { long sec, usec; } tv;

	if (rawsys2(SYS_gettimeofday, (long)&tv, 0) < 0) tv.sec = 0;
	if (tp) *tp = tv.sec;
	return (tv.sec);
}

int v8s_stime(long *tp) { v8_errno = V8_EPERM; return (-1); }

/*
 * ftime(2) -- syscall 35 in V8 (libc/sys/time.s), and until Phase 4 a hole in
 * this layer.  Nothing failed, which is the point: `ftime' resolved quietly out
 * of libSystem instead, so date(1), ls(1) and localtime(3) were all reaching
 * the host's libc without anything saying so.  Exactly the shape CLAUDE.md
 * warns about -- a missing function does not break the link, it gets filled in
 * -- and it was found by tests/kmemu asserting on `nm -u', not by a wrong
 * answer.  There was no wrong answer to find.
 *
 * struct timeb is <sys/timeb.h> unchanged: time_t (a long, so 8 bytes here),
 * then three shorts.  macOS's own struct has the same shape under LP64, which
 * is why the accidental version worked.
 *
 * The timezone comes from tz.c, which reads the zone database the way the VAX
 * kernel held the variable.  Not from gettimeofday's second argument, which is
 * a struct timezone * and looks exactly right and is not -- tz.c says what that
 * cost to find out.
 *
 * dstflag STAYS 0 on purpose.  The offset tz.c returns is the one in force now,
 * daylight saving already applied, so setting the flag would make V8's
 * localtime() add another hour on top.  What it would add is the US 1974-75
 * rule -- `last Sunday in April', with 1974 and 1975 special-cased in a table --
 * which has not been the answer anywhere for decades.  Letting the zone
 * database say what the offset is, and leaving V8's own DST arithmetic
 * unreached, is the honest arrangement.
 */
struct v8_timeb { long time; unsigned short millitm; short timezone; short dstflag; };

extern long v8sys_tzminuteswest(void);

int
v8s_ftime(struct v8_timeb *tb)
{
	struct { long sec, usec; } tv;

	tv.sec = tv.usec = 0;
	rawsys2(SYS_gettimeofday, (long)&tv, 0);
	tb->time = tv.sec;
	tb->millitm = (unsigned short)(tv.usec / 1000);
	tb->timezone = (short)v8sys_tzminuteswest();
	tb->dstflag = 0;
	return (0);
}

/* V8's times(2) reports in 60ths of a second, the VAX clock tick. */
struct v8_tbuffer { long proc_user, proc_system, child_user, child_system; };

long
v8s_times(struct v8_tbuffer *tb)
{
	struct { long u_sec, u_usec, s_sec, s_usec; long rest[14]; } ru;

	tb->proc_user = tb->proc_system = 0;
	tb->child_user = tb->child_system = 0;
	if (rawsys2(SYS_getrusage, 0 /*RUSAGE_SELF*/, (long)&ru) >= 0) {
		tb->proc_user   = ru.u_sec * 60 + ru.u_usec * 60 / 1000000;
		tb->proc_system = ru.s_sec * 60 + ru.s_usec * 60 / 1000000;
	}
	if (rawsys2(SYS_getrusage, -1 /*RUSAGE_CHILDREN*/, (long)&ru) >= 0) {
		tb->child_user   = ru.u_sec * 60 + ru.u_usec * 60 / 1000000;
		tb->child_system = ru.s_sec * 60 + ru.s_usec * 60 / 1000000;
	}
	return (tb->proc_user + tb->proc_system);
}

int v8s_wait(int *status)
{ RET(rawsys4(SYS_wait4, -1, (long)status, 0, 0)); }

int v8s_wait3(int *status, int options, void *rusage)
{ RET(rawsys4(SYS_wait4, -1, (long)status, options, (long)rusage)); }

int
v8s_utime(char *p, long *tv)
{
	struct { long sec, usec; } t[2];

	if (tv == 0) RET(rawsys2(SYS_utimes, (long)p, 0));
	t[0].sec = tv[0]; t[0].usec = 0;
	t[1].sec = tv[1]; t[1].usec = 0;
	RET(rawsys2(SYS_utimes, (long)p, (long)t));
}

int
v8s_kill(int pid, int sig)
{
	int h = v8sys_signo_to_host(sig);
	if (h < 0) { v8_errno = V8_EINVAL; return (-1); }
	RET(rawsys3(SYS_kill, pid, h, 0));
}

/* --------------------------------------------------------------- ENOSYS */

/*
 * These belong to a kernel we are deliberately not porting: mounting
 * filesystems, rebooting, process accounting, the streams machinery, ptrace.
 * PLAN.md S7 lists the commands that use them, all of which are on the
 * excluded list.  Failing loudly is better than pretending to succeed.
 */
static int nosys(void) { v8_errno = V8_EINVAL; return (-1); }

int v8s_mount(void)    { return nosys(); }
int v8s_umount(void)   { return nosys(); }
int v8s_gmount(void)   { return nosys(); }
int v8s_swapon(void)   { return nosys(); }
int v8s_reboot(void)   { return nosys(); }
int v8s_acct(void)     { return nosys(); }
int v8s_settod(void)   { return nosys(); }
int v8s_vadvise(void)  { return nosys(); }
int v8s_vlimit(void)   { return nosys(); }
int v8s_vtimes(void)   { return nosys(); }
int v8s_profil(void)   { return (0); }	/* silently does nothing */
int v8s_ptrace(void)   { return nosys(); }
int v8s_mpx(void)      { return nosys(); }

/* V8's nap(2): sleep for a number of milliseconds. */
int
v8s_nap(long ms)
{
	/*
	 * macOS exposes no nanosleep syscall -- libc builds it out of Mach
	 * primitives -- so this sleeps the portable way, with a select() that
	 * watches nothing and simply times out.
	 */
	struct { long sec, usec; } tv;

	tv.sec  = ms / 1000;
	tv.usec = (ms % 1000) * 1000L;
	rawsys6(SYS_select, 0, 0, 0, 0, (long)&tv, 0);
	return (0);
}

/*
 * V8's indirect syscall.  The stub took the number as its first argument and
 * shuffled the rest down.  Almost nothing uses it, but adb and a few
 * diagnostics do, and a stub that reports failure is more useful than a link
 * error in the middle of porting something else.
 */
int
v8s_syscall(int number)
{
	v8_errno = V8_EINVAL;
	return (-1);
}
