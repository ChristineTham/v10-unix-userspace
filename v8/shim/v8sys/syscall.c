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
#include "vfs.h"
#include "rawsys.h"
#include "../v8id.h"		/* v8_foldid -- the narrowing rule, shared */

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
/*
 * THE MOUNT TABLE MOVED.  What used to be `v8dirs[]' and `v8files[]' here is
 * now shim/v8sys/vfs.c's `mounts[]', with a filesystem-type column -- PLAN.md
 * section 8a step 2.  Generalising the existing list rather than adding a
 * second one beside it is the point: two prefix tables that must agree by hand
 * are the standing invitation kmem.c's one-table rule exists to refuse.
 *
 * rootpath() below asks v8fs_typefor() whether any mount claims a path; the
 * reasoning about trailing slashes and exact matches went with the table.
 */

/*
 * Where the V8 world is.
 *
 * $V8ROOT first, then a root compiled in at build time.  The env var alone was
 * a silent fall-through of exactly the kind this port keeps paying for: with it
 * unset, rootpath() quietly returned the HOST path, so a V8 binary run outside
 * the launcher operated on the real filesystem and looked like it was working.
 *
 * V8ROOT_DEFAULT is stamped in by the Makefile (and by `make install`, at the
 * install prefix).  An installed binary therefore knows its own world without
 * anyone having to remember to export anything, and the env var stays as the
 * override that lets a second tree be tested.
 *
 * THE #ifndef IS NOT DECORATION.  tests/v8sys builds this file WITHOUT the -D,
 * and deleting the guard while moving the mount table out compiled cleanly for
 * the whole world and broke only that one link -- which is the right place for
 * it to break, and the reason the suite builds the shim its own way.
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

/*
 * "DOES THE ROOTFS HAVE THIS NAME" IS AN lstat QUESTION, AND IT USED TO BE AN
 * access ONE.
 *
 * access(2) follows the last component.  So a symlink inside the jail whose
 * target cannot be resolved read as ABSENT, the path fell through unresolved,
 * and every operation on it went to the host -- which is the wrong direction
 * for a chroot to fail in.  Found while fixing two syscalls that resolved no
 * path at all; orthogonal to those, because this one resolves correctly except
 * for names access(2) cannot see.
 *
 * MEASURED, the two predicates disagree on exactly four shapes on this host,
 * and all four are "the last component is a symlink whose resolution fails":
 * a dangling absolute link (ENOENT), a dangling relative one (ENOENT), a loop
 * (ELOOP), and a link whose target sits behind an unsearchable directory
 * (EACCES).  Everywhere else they agree -- a file behind chmod 000 fails BOTH,
 * because lstat needs search permission on the prefix too; a name below a
 * dangling link fails both; a trailing slash on a link to a file fails both.
 *
 * AND THE CHANGE IS MONOTONE, which is what bounds it: there is no case where
 * access succeeds and lstat fails, so the union rule can only ever resolve MORE
 * names into the jail and never fewer.  (The candidate counterexample is a
 * trailing slash on a symlink to a DIRECTORY, where lstat is documented to
 * follow; measured, both succeed.)  That is the reasoning for touching the
 * single most load-bearing function in the shim at all.
 *
 * RAW, NOT v8s_lstat -- the same reason the access call had: v8s_lstat runs its
 * argument back through vpath(), which re-enters this function and would
 * overwrite the buf still being built.
 *
 * THE BUFFER IS A BYTE ARRAY because struct hoststat64 is declared a thousand
 * lines below here and nothing is read out of it: only the syscall's success is
 * wanted.  Its size is not a hope -- there is a _Static_assert beside that
 * declaration, where the type is complete.
 */
#define V8_STATBUF	256

static int
rootfs_has(const char *path)
{
	char st[V8_STATBUF];

	return (rawsys2(SYS_lstat64, (long)path, (long)st) == 0);
}

char *
v8sys_rootpath(char *p, int mode)
{
	static char buf[1024];
	char *root;
	int i, n, m;

	/*
	 * THE EMPTY PATH IS THE CURRENT DIRECTORY -- V7 namei()'s rule, and the
	 * comment at the top of this file has the whole story (rmdir(1) stats
	 * its parent with stat("", &cst) and there is no other way it could have
	 * meant).
	 *
	 * It lives HERE rather than in vpath(), and that is a correction: it was
	 * in vpath, so the switch's t_path -- which calls this directly -- lost
	 * it, and `rmdir' stopped removing anything.  It is a namespace rule, so
	 * every filesystem type must get it, not just the two callers that
	 * happened to go through vpath.  tests/waveb caught it on the first run,
	 * which is exactly what PLAN.md section 8a step 2 means by replacing the
	 * floor while the suites stay green.
	 */
	if (p != 0 && *p == '\0') return (".");
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

	/*
	 * A SERVER-BACKED MOUNT IS NOT IN THE ROOTFS, and this line is the
	 * difference between saying so and writing to the Mac.
	 *
	 * v8fs_typefor() is used here as a GATE -- "does any mount claim this
	 * name" -- and every type it could return used to answer out of
	 * $V8ROOT.  v8fs does not: the file is on a disk image in another
	 * process.  Prepending the root would build $V8ROOT/mnt/x, and the
	 * creating syscalls would then bring that file into existence, so a
	 * failed unlink on a mount would leave a real file in the jail named
	 * after a file that is somewhere else entirely.
	 *
	 * Returning the path UNCHANGED is what an unclaimed path gets, and it
	 * is right for the same reason: this function's job is the union with
	 * the rootfs, and a mount is not in the union.  The syscalls that then
	 * pass the bare path to the host are guarded separately -- each in
	 * its own arm now, because "the host probably has no /mnt" is not a
	 * guarantee anyone made.  This used to say "see MOUNTED() below";
	 * §8a step 5g deleted that macro when link took a slot and symlink
	 * moved to EPERM, and the deletion landed on one line while the line
	 * explaining why it mattered kept the assumption -- the exact shape
	 * the deletion's own commit message is about.
	 */
	if (v8fs_mounted(p)) return (p);
	if (v8fs_typefor(p) == 0) return (p);
	if ((root = v8root()) == 0) return (p);

	for (n = 0; root[n] && n < (int)sizeof buf - 2; n++) buf[n] = root[n];
	for (m = 0; p[m] && n < (int)sizeof buf - 1; m++) buf[n++] = p[m];
	buf[n] = '\0';
	/*
	 * rootfs_has() is raw, NOT v8s_lstat: v8s_lstat runs its argument back
	 * through vpath(), which re-enters this function and would overwrite the
	 * buf we are still building.  It happens to be harmless today only
	 * because $V8ROOT does not itself start with a V8 directory prefix --
	 * set V8ROOT=/usr/lib/anything and it would clobber.  Not worth leaving
	 * to luck for a call that only asks whether the name is there.
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
		/*
		 * BOTH MODES CHANGE, AND THE NOTE THAT SAID OTHERWISE WAS A
		 * GUESS.  The reasoning recorded for this fix was that only
		 * V8P_LOOK is affected, "because the parent case is a directory
		 * and cannot be a dangling link".  Nothing stops it being one:
		 * $V8ROOT/etc could be a symlink to nothing, and then access
		 * says absent, the path falls through, and creat("/etc/x")
		 * writes to the MAC's /etc -- the escape direction, in the mode
		 * that exists to stop exactly that.  With lstat the name is the
		 * jail's and the create fails ENOENT, which is failing closed.
		 * Neither answer creates the file; only one of them stays
		 * inside.
		 */
		ok = rootfs_has(buf);
		buf[cut] = '/';
		return (ok ? buf : p);
	}
	if (rootfs_has(buf)) return (buf);
	return (p);
}

/*
 * THE DISPATCH.  Which filesystem answers for this path, and which for this
 * descriptor.  Both fall back to passthrough, so with one type in the table
 * every call below behaves exactly as it did before the switch existed -- which
 * is what PLAN.md section 8a step 2 asks for: replace the floor, change
 * nothing, and let the suites say so.
 */
#define FSFOR(p)	fs_or_pass(v8fs_typefor(p))
#define FDFS(fd)	v8fs_fdtype(fd)

static struct v8fstyp *
fs_or_pass(struct v8fstyp *t)
{
	/*
	 * No mount claims a path outside the V8 directories -- /tmp, a relative
	 * name, the build tree.  Those are the host's and always were, so the
	 * passthrough type is the honest answer rather than an error: it is the
	 * type whose t_path leaves such a name alone.
	 */
	return (t ? t : &v8fs_pass);
}

/* The reader's path: today's rule, unchanged.  (The empty-path rule moved into
 * v8sys_rootpath, so that every filesystem type gets it -- see there.) */
static char *
vpath(char *p)
{
	return v8sys_rootpath(p, V8P_LOOK);
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

	q = v8sys_rootpath(p, V8P_LOOK);
	if (q != p) return (q);
	return v8sys_rootpath(p, V8P_MAKE);
}

/*
 * FOURTEEN SYSCALLS HAD NO SLOT IN struct v8fstyp AND WERE PASSTHROUGH BY
 * CONSTRUCTION.  Nine of the fourteen have slots now; this is the standing
 * account of where the other five went, and the count has been wrong twice, so
 * it is written as a classification rather than as a number.
 *
 * WHY IT MATTERS AT ALL.  A slotless mutating call resolves a mounted path
 * through rootpath(), which leaves such a path alone and says why -- so it
 * reaches the host VERBATIM, and `rm /mnt/x' asks the Mac to unlink /mnt/x.
 * That was containable while every type answered out of $V8ROOT: a chmod on a
 * /proc path was wrong, but it was wrong about a file in the jail.  With a
 * mount in another process it is not.  There is no /mnt on this machine, which
 * is luck and not a design -- and a case that leaned on it passed for the
 * wrong reason until it was rewritten over a directory the host really has.
 *
 * ONE STILL REFUSES -- symlink -- AND THE MACRO BELOW HAS NO CALLERS LEFT.
 *
 * This paragraph used to say TWO, link and symlink, "three MOUNTED() calls
 * because link guards both of its names", and then: "Neither is deferred
 * work: 9P2000 has no message for either."  §8a step 5g took that apart and
 * every clause of it was doing damage:
 *
 *   THE REASON WAS THE PORT'S OWN ARGUMENT FOR THE OPPOSITE.  "9P2000 has no
 *   message for it" is the exact situation that produced Tseek/Rseek and
 *   Taccess/Raccess.  Twice the answer to a missing message was to add one and
 *   write down why; the third time the same fact was recorded as grounds for
 *   refusing.  A reason that has already been overruled twice in the same file
 *   is not a reason.
 *
 *   AND THE PAIR WAS NOT A PAIR.  A V7 filesystem cannot REPRESENT a symlink
 *   -- no i_mode for it, which is the same fact readlink() answers EINVAL on
 *   -- so that refusal is permanent.  A V7 filesystem IS BUILT ON hard links:
 *   i_nlink is a field in the inode, sys2.c:458's link() is `ip->i_nlink++'
 *   plus a namei with NI_LINK, and nami.c:484's NI_LINK arm was ALREADY IN THE
 *   IMPORTED TREE, unreachable only because nothing sent it a request.  link
 *   was chdir's shape -- a real gap -- filed under symlink's.
 *
 *   AND IT READ AS COSMETIC BECAUSE THE LOUDEST CONSUMER DEGRADES QUIETLY.
 *   mv of a FILE falls back to fork-and-cp and exits 0, so the gap looked like
 *   a slow path.  mv of a DIRECTORY does not: mv.c's mvdir() at :204 has no
 *   fallback at all, so `mv /mnt/d /mnt/d2' printed "mv: cannot link" and left
 *   the directory where it was.  Measured, on a server that had accepted two
 *   writes seconds earlier.
 *
 * symlink keeps its refusal and loses its EROFS -- see the note there for why
 * EPERM is the true word, which is v8s_mknod's distinction applied one line
 * further along.  It spells v8fs_mounted() directly; there is no macro.
 *
 * AND THE MOUNTED() MACRO IS GONE WITH IT, WHICH THIS COMMENT ARGUED AGAINST
 * FOR ABOUT AN HOUR.  The draft kept it, dead, "so that the next slotless
 * syscall has the spelling to hand" -- which is word for word the excuse this
 * repository rejects.  §8a step 5f-b had just deleted four declarations under
 * a paragraph reading "a declaration with no call site is an unconsumed
 * component", and recorded that the fix for one is not a better version of it.
 * A macro with no expansion is the same shape, and keeping it would have left
 * a spelling of EROFS lying about for the next author to reach for at exactly
 * the moment they should be asking whether EROFS is true.  Measured before
 * deleting: zero call sites, every other occurrence in prose.
 *
 * EIGHT ANSWER THROUGH A SLOT, and mknod is a ninth with one arm in each camp.
 * §8a step 5f gave access, unlink, mkdir and rmdir one, along with mknod's
 * DIRECTORY arm; §8a step 5f-b adds chmod, chown and utime, which are one
 * Twstat between them; §8a step 5g adds link.  The rule the sequence obeys is
 * that a slot is a CLAIM the operation is implemented -- so EROFS was the
 * truth while the server refused every write, and became a lie the day it
 * stopped, which is why these arrived in three steps rather than being stubbed
 * early.
 *
 * THREE ANSWER WITHOUT A SLOT, because EROFS would be a lie about a question
 * with a real answer: readlink() is EINVAL, which is what readlink(2) says
 * about a file that is not a symbolic link, and a V7 image contains no other
 * kind; chdir() refuses with EACCES, and its comment says why that is a
 * refusal rather than a gap; and mknod's DEVICE arm is EPERM, which 5f-b
 * changed from EROFS so that the two worlds give one answer -- see the note
 * there for why EPERM is the true one.  (This paragraph used to call chdir
 * "ENOTDIR-or-worse", which was the wrong half of its own sentence: nothing
 * errored at all, and an auditor walked straight out of the jail through it.)
 *
 * AND TWO ARE THEIR OWN QUESTIONS, both missed by the first enumeration:
 * v8s_chroot, which passes its path completely unresolved whether or not it is
 * mounted, and v8s_execve.
 */
int v8s_getuid(void);
int v8s_getgid(void);

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
	int r = FDFS(fd)->t_close(fd);
	v8fs_unbind(fd);
	return (r);
}

long v8s_write(int fd, char *b, long n)  { return FDFS(fd)->t_write(fd, b, n); }
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

	/*
	 * A null path is not a dot link -- it is a call that has to reach the
	 * kernel and come back EFAULT.
	 *
	 * This shim's rule is that a null path is the kernel's to reject:
	 * rootpath() returns it unchanged for exactly that reason.  dotlink()
	 * broke the rule because it INSPECTS the string before the syscall
	 * runs, so unlink(0) and link(a, 0) faulted in our own code instead.
	 *
	 * Reached from yacc, not from a synthetic case.  `yacc -o' with -o
	 * last leaves the output name null, openup() fails to create it, and
	 * error() runs cleantmp(), whose two ZAPFILEs are unlink() of temp
	 * names that setup() had not yet assigned.  The crash was in the shim
	 * and looked like a crash in yacc.
	 */
	if (b == 0)
		return (0);
	for (p = base = b; *p; p++)
		if (*p == '/') base = p + 1;
	return (base[0] == '.' &&
	    (base[1] == '\0' || (base[1] == '.' && base[2] == '\0')));
}

int v8s_link(char *a, char *b)
{
	struct v8fstyp *ta, *tb;
	char old[1024];
	char *q;
	int i;

	/*
	 * Same rule as dotlink() below: a null path belongs to the kernel.
	 * vpath() hands one straight back, and the copy loop further down then
	 * dereferences it -- so this has to be settled before either name is
	 * touched.  Handing both to the kernel gets the EFAULT the caller is
	 * owed; the surviving name is never used, because the call fails.
	 */
	if (a == 0 || b == 0)
		RET(rawsys2(SYS_link, (long)a, (long)b));

	/*
	 * THE DOT-LINK ARM MOVED ABOVE THE GUARD IN §8a step 5f, and the
	 * reasoning it already had is what carried it there unchanged.
	 *
	 * V7 has no mkdir(2): mkdir(1) is `mknod(d, IFDIR|mode, 0)' and then
	 * two link()s, one for `.' and one for `..', which is why it was setuid
	 * root.  Neither macOS nor a v8fs mount works that way -- the host's
	 * mkdir(2) writes both entries, and so does Bell Labs' own NI_MKDIR
	 * arm through the eleven lines of sys2.c:246-256 that v8fsd's kmkdir
	 * transcribes.  So on BOTH the entries already exist by the time the
	 * links arrive, and succeeding-and-doing-nothing is the truth rather
	 * than a fiction.
	 *
	 * It has to be before MOUNTED() because it is the same statement about
	 * a different filesystem, and v8s_stat below already dispatches.  With
	 * it after the guard, `mkdir /mnt/d' created the directory correctly
	 * and then reported `cannot link /mnt/d/.' and exited 1 -- a failure
	 * message about a completed operation, which is the worst of the three
	 * possible answers.  Measured, not predicted.
	 */
	if (dotlink(b)) {
		struct v8_stat st;

		/* only if the directory really is there */
		if (v8s_stat(a, &st) == 0 &&
		    (st.st_mode & V8_S_IFMT) == V8_S_IFDIR)
			return (0);
	}

	/*
	 * A LINK CROSSES TWO NAMES, SO IT CROSSES TWO TYPES, and a mismatched
	 * pair is EXDEV -- which is an ANSWER rather than a refusal.  §8a step
	 * 5g; two MOUNTED() calls stood here and gave EROFS, on a filesystem
	 * that had taken writes since 5f.  Measured against a server that had
	 * just accepted `echo > /mnt/f': `ln /mnt/f /mnt/g' said "Read-only
	 * file system".
	 *
	 * EXDEV IS WHAT THE KERNEL BELOW WOULD SAY ANYWAY.  nami.c:487's
	 * NI_LINK arm is `if(dp->i_dev != flagp->idev) u.u_error = EXDEV', and
	 * the host's link(2) says the same across two host filesystems.  So
	 * this line is not inventing a rule; it is applying the one both ends
	 * already have, at the only place that can see both types.
	 *
	 * BOTH names were unresolved until §8a step 5f, so `ln /bin/cat x'
	 * linked the MAC's /bin/cat -- read the same name with open(2) and you
	 * got the jail's.  The existing name takes the reader's rule and the
	 * new name the creator's, which is the same split rename(2) would want.
	 *
	 * COPIED, and that is the aliasing trap CLAUDE.md names: t_path returns
	 * a pointer into a static buffer, so holding two results at once
	 * silently gives you the same string twice.  Here that would have been
	 * link(new, new).  This is the ONLY slot in the switch that takes two
	 * paths, so it is the only one where the trap is unavoidable rather
	 * than merely available -- vfs.h says so beside t_link.
	 */
	ta = FSFOR(a);
	tb = FSFOR(b);
	if (ta != tb) { v8_errno = V8_EXDEV; return (-1); }

	q = ta->t_path(a, V8P_LOOK);
	for (i = 0; q[i] && i < (int)sizeof old - 1; i++) old[i] = q[i];
	old[i] = '\0';
	return (ta->t_link(old, tb->t_path(b, V8P_MAKE)));
}

/*
 * mknod.  V7's way of creating a directory, and the reason mkdir(1) was setuid.
 *
 * Nothing else in the tree makes a device node that we can honour -- the host
 * would refuse, and a V8 program has no business creating one on a Mac -- so
 * the only mode that does anything here is S_IFDIR, which becomes mkdir(2).
 * Without this, mknod fell through to libSystem's, which needs root for a
 * directory, and mkdir(1) reported "cannot make directory".
 *
 * mkpath(), AND IT WAS MISSING -- the last hole left by the creation fix.
 * When V8P_MAKE arrived, creat, link, mkdir and unlink were all converted and
 * this one was not, because nothing called it: mkdir(1) is the ONLY user of
 * mknod in the whole tree, and mkdir(1) was among the eleven commands that had
 * been imported and never built.  An unreachable syscall cannot be seen to be
 * wrong, so building mkdir(1) is what exposed it.
 *
 * The shape is the one CLAUDE.md warns about, and mkdir(1) shows both halves in
 * one program: its `access(pname, 02)' goes through v8s_access -> vpath() and
 * asks the JAIL, then its mknod asked the HOST.  Checked one filesystem, wrote
 * to another.  It failed closed only because every jailed prefix happens to be
 * SIP-protected on this Mac -- on a writable one it would have created the
 * directory outside the jail and reported success.
 */
int v8s_mknod(char *p, int mode, int dev)
{
	/*
	 * AND ON A MOUNT IT IS t_mkdir, §8a step 5f -- so mkdir(1) works
	 * inside an image without knowing there is one.  The DEVICE arm is
	 * still EPERM and that is not a gap being deferred: a V7 image is
	 * bytes on a disk that no kernel will mount, so a device node in it
	 * would name a driver on a machine that does not exist.  9P2000 has no
	 * message for one either -- the .u extension added it, and PLAN.md §8a
	 * rules .u out.  The refusal is the same one the host arm gives.
	 *
	 * AND THAT LAST SENTENCE WAS FALSE UNTIL §8a step 5f-b, which is what
	 * made it true rather than correcting it.  A MOUNTED(p) stood here, so
	 * the mounted answer was EROFS and the host answer EPERM -- two words
	 * for one refusal, and the wrong one of the two: EROFS says the
	 * filesystem will not take writes, which stopped being true in 5f,
	 * where EPERM says the operation is meaningless, which is the actual
	 * reason and is true of both worlds.  The macro could go because this
	 * arm never touches the path: it sets errno and returns, so there was
	 * nothing for the guard to contain.  procfs.c's three new slots record
	 * the same distinction from the other side.
	 */
	if ((mode & 0170000) == 0040000) {
		struct v8fstyp *t = FSFOR(p);

		return (t->t_mkdir(t->t_path(p, V8P_MAKE), mode & 07777));
	}
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

	/*
	 * THE DOT ENTRIES FIRST, FOR BOTH FILESYSTEMS -- §8a step 5f, and it is
	 * v8s_link's move made in the mirror direction.  rmdir(1) is
	 * `unlink("d/.."); unlink("d/."); unlink("d")', and on a v8fs mount the
	 * first two would reach the server, which refuses them: a fid whose
	 * last component is `.' or `..' has no recorded parent, on purpose,
	 * because its NAME is not an entry anybody may unlink.  rmdir(1)
	 * ignores those two return values (rmdir.c:105,108 check nothing) so
	 * the refusal costs nothing today -- which is exactly why it should be
	 * settled here rather than left to be discovered by the one caller that
	 * does look.
	 *
	 * `d' RESOLVED BELOW USES vpath, AND THE MOUNT MUST NOT, so the parent
	 * test is done against the ORIGINAL path and the mount dispatch happens
	 * between the two halves.
	 */
	if (dotlink(p)) {
		char par[1024];
		int i, b = 0;

		for (i = 0; p[i] && i < (int)sizeof par - 1; i++)
			if (p[i] == '/') b = i;
		for (i = 0; i < b; i++) par[i] = p[i];
		par[b] = '\0';
		if (b == 0) return (0);			/* bare "." or ".." */
		/*
		 * ...BUT NOT ON A MOUNT, AND §8a step 5g's OWN AUDIT IS WHY.
		 *
		 * Succeeding-and-doing-nothing is the truth for the caller this
		 * arm was written for -- mkdir(1) and rmdir(1), where `.' and
		 * `..' are made and destroyed with the directory and the entry
		 * really does already exist with the meaning the caller wanted.
		 * It is a LIE for mv(1), whose entire purpose in these two
		 * calls is to CHANGE what `..' means, and the predicate looks
		 * only at the basename so it cannot tell them apart.
		 *
		 * Before link had a slot this never fired: mvdir's first step
		 * was refused, loudly, and mv stopped.  5g made the whole
		 * sequence reachable and it then ran to completion returning 0
		 * with `..' still pointing at the OLD parent.  Measured --
		 * a=102, b=101, `mv /mnt/a/d /mnt/b/d' exits 0 and `b/d/..'
		 * reads 102, so `ls /mnt/b/d/..' lists an empty `a'.  And
		 * NOTHING SEES IT: icheck and dcheck are silent, because the
		 * link counts stay perfectly consistent with the wrong `..'.
		 * A silently wrong filesystem, where a refusal stood before.
		 *
		 * REFUSING THE UNLINK IS WHAT MAKES mv ROLL BACK, and that is
		 * the whole reason the guard goes here rather than on the link
		 * beside it.  mv.c:216 is the FIRST of the two `..' calls, and
		 * its failure arm at :218-219 relinks the old name and unlinks
		 * the new -- so the directory goes back exactly where it was
		 * and `..' is never touched.  Failing the link at :222 instead
		 * would leave the move half done.
		 *
		 * AND IT COSTS THE OTHER TWO CALLERS NOTHING, which is measured
		 * rather than hoped: rmdir(1) IGNORES both dot unlinks
		 * (rmdir.c:105 and :108 are bare `unlink(name);', only the
		 * third is checked), and mkdir(1) uses link(2) for both of its
		 * and never comes here at all.
		 *
		 * EINVAL rather than EPERM or EROFS: the filesystem takes
		 * writes and the caller has the right to ask, but "remove the
		 * `..' of a live directory" is not a request this server can
		 * be given -- there is no Tunlink that names it.
		 */
		if (v8fs_mounted(p)) {
			v8_errno = V8_EINVAL;
			return (-1);
		}
		if (v8s_stat(par, &st) == 0 &&
		    (st.st_mode & V8_S_IFMT) == V8_S_IFDIR)
			return (0);
	}

	/*
	 * AND THEN IT DISPATCHES LIKE EVERY OTHER PATH-TAKING CALL, which it
	 * was the LAST one not to do.  It used to test v8fs_mounted() and reach
	 * into v8fs_p9 by name, then fall through to a raw SYS_rmdir/SYS_unlink
	 * on a vpath()-resolved name -- so /proc and /dev/fd never saw an
	 * unlink at all, and pt_remove's unlink arm had no caller anywhere
	 * (v8s_rmdir always passes isdir 1).  That is the v8s_creat shape this
	 * file already records: path resolution without dispatch, so no second
	 * type can ever see the operation, and the slot is a claim nothing can
	 * check.  Nothing misbehaved, because pt_path(p, V8P_LOOK) IS vpath(p)
	 * and the two roads met -- which is exactly why an auditor found it and
	 * no test did.
	 *
	 * isdir IS -1, "no opinion", because that is precisely what unlink(2)
	 * has: V7's unlink removes an entry of any kind if the caller is
	 * privileged enough.  The macOS reasoning that used to be here -- its
	 * unlink refuses a directory, so the choice V7 declines to make must be
	 * made anyway -- moved into pt_remove unchanged, because it is a fact
	 * about the host filesystem and that is what the passthrough type is.
	 * A mount needs none of it: the SERVER's suser() decides, and Bell
	 * Labs' nami.c NI_DEL arm performs it.
	 */
	{
		struct v8fstyp *t = FSFOR(p);

		return (t->t_remove(t->t_path(p, V8P_LOOK), -1));
	}
}
/*
 * chdir INTO A MOUNT IS REFUSED, and until it was, it was a JAIL ESCAPE.
 *
 * rootpath() now returns a mounted path unchanged -- it must, because the file
 * is on an image in another process -- so an unguarded chdir hands the bare
 * name to the host.  With a mount whose prefix the Mac also has, that is not a
 * failure but a success into the wrong world: measured with V8MOUNT=/etc=sock,
 * `cd /etc' returned 0, pwd said /private/etc, and `cat passwd' read the Mac's
 * password database.  Before the rootpath line, /etc resolved into $V8ROOT.
 * /mnt is safe only because this machine has no /mnt.
 *
 * EACCES rather than EROFS, because chdir is not a write and the refusal is
 * not about the medium: nothing in this shim tracks a working directory, so
 * v8fs_typefor refuses a relative path and a program that got inside a mount
 * would find every relative name resolving against the host.  That is the one
 * gap the client cannot close on its own -- PLAN.md §8a step 5f -- and until
 * it is closed the honest answer is no.
 *
 * AND THE COSTING FOR CLOSING IT WAS TOO SMALL, RE-MEASURED.  The first
 * estimate was "one function plus chdir": a v8fs_cwd[] that is empty whenever
 * the process sits in a real host directory, consulted by v8fs_typefor for a
 * relative path only.  Three things say it is bigger, and the first two were
 * measured rather than reasoned:
 *
 *   `..' AT A MOUNT POINT DOES NOT ESCAPE, AND THE SERVER CANNOT MAKE IT.
 *   Measured against a real v8fsd: `ls /mnt/sub/..' correctly lists the mount
 *   root, and `ls /mnt/..' lists THE IMAGE ROOT AGAIN rather than the jail's /
 *   (bin dev etc lib unix usr).  That is V7 being right -- a filesystem root's
 *   `..' points at itself -- and on a real Unix it is namei's mount table that
 *   fixes it up when a walk crosses a mount upward.  There is no kernel here to
 *   do that, and the image does not know it is mounted anywhere, so `cd /mnt &&
 *   cd ..' would leave a program stuck inside the image.  The client has to
 *   resolve `..' at the mount point LEXICALLY, before the walk.
 *
 *   getwd(3) IS THE HARD CONSUMER AND IT WRITES.  src/libc/gen/getwd.c does not
 *   merely read its way up: it opens `..', reads it, `chdir("..")'s, repeats,
 *   and chdirs back at the end -- so every level of a pwd(1) inside a mount is
 *   another chdir to intercept, and its central loop matches a directory
 *   entry's d_ino against stat(".")  , which puts the folded-inode machinery
 *   and the mount's qid paths on the same comparison.
 *
 *   AND THE cwd MUST SURVIVE exec, which is vfs.c:167's recorded lesson about
 *   the descriptor table: a table in process memory dies when a program
 *   replaces its image.  V8MOUNT survives because it is in the ENVIRONMENT; a
 *   tracked cwd would have to be too, written on every chdir and carried by
 *   v8s_execve -- which is a second thing in the environment that has to agree
 *   with the first.
 */
int v8s_chdir(char *p)
{
	if (v8fs_mounted(p)) { v8_errno = V8_EACCES; return (-1); }
	RET(rawsys1(SYS_chdir, (long)vpath(p)));
}
/*
 * chmod AND chown ARE SLOTS NOW, §8a step 5f-b, and so is utime a thousand
 * lines below.  All three were MOUNTED() -- a flat EROFS on any mounted path --
 * and that refusal was honest for exactly as long as the server refused every
 * write.  From 5f it was a lie about a writable filesystem; this is the step
 * that stops telling it.
 *
 * ONE Twstat ANSWERS ALL THREE, so the three slots below are three spellings
 * of one message rather than three protocol additions.  p9cl.c has the shape.
 *
 * V8P_LOOK FOR ALL OF THEM, because every one names a file that must already
 * exist -- there is no creating form of chmod.  v8s_mkdir is the contrast, and
 * it is four lines up.
 */
int v8s_chmod(char *p, int m)
{
	struct v8fstyp *t = FSFOR(p);

	return (t->t_chmod(t->t_path(p, V8P_LOOK), m));
}
int v8s_chown(char *p, int u, int g)
{
	struct v8fstyp *t = FSFOR(p);

	return (t->t_chown(t->t_path(p, V8P_LOOK), u, g));
}
int v8s_fchmod(int f, int m)             { RET(rawsys2(SYS_fchmod, f, m)); }
int v8s_fchown(int f, int u, int g)      { RET(rawsys3(SYS_fchown, f, u, g)); }
/*
 * access() IS A SLOT NOW, §8a step 5f, AND IT TOOK THREE ANSWERS TO GET HERE.
 * Both earlier ones were left standing above this function, one contradicting
 * the other, which is the stale-comment class arriving as a PAIR -- the
 * rewrite added its reasoning and did not remove the reasoning it replaced.
 *
 *   1. RECOMPUTED LOCALLY, from the image's mode bits against THE HOST'S uid.
 *      Wrong on every file of every image: v8fsd runs Bell Labs' access() with
 *      u.u_uid, which nothing sets and which is therefore 0, so fio.c's root
 *      bypass applies and the open succeeds whatever the mode says.  Measured
 *      on a 0600 root-owned file -- `test -r' said no and `cat' printed it.
 *      The bits were the server's and the IDENTITY was the host's, and
 *      identity is the half that decides.
 *
 *   2. REPORTED WHAT WOULD HAPPEN: R_OK from a stat, W_OK always EROFS, X_OK
 *      always EACCES.  Right while the server refused every write, and its own
 *      comment said so -- "when §8a step 5f gives the server a writable image
 *      this becomes a question worth asking over the wire".  That day is this
 *      one, and a fixed EROFS is now a wrong answer rather than a blunt one.
 *
 *   3. ASKED.  Taccess carries a fid and V7's three mode bits, and Bell Labs'
 *      access() answers with the server's identity on both sides.  p9.h has
 *      the argument for the extension; the short form is that 9P has no
 *      access(2) for the same reason it has no seek, and this port has to
 *      supply what Plan 9 put in its kernel.
 *
 * DISPATCHED RATHER THAN BRANCHED, which is what makes the other two types
 * answer too: passthrough is the host syscall it always was, /proc gets a real
 * answer instead of a stat this function used to do on its behalf, and /dev/fd
 * inherits passthrough because a dup'd descriptor IS a host descriptor.
 */
int v8s_access(char *p, int m)
{
	struct v8fstyp *t = FSFOR(p);

	return (t->t_access(t->t_path(p, V8P_LOOK), m));
}
int v8s_mkdir(char *p, int m)
{
	struct v8fstyp *t = FSFOR(p);

	return (t->t_mkdir(t->t_path(p, V8P_MAKE), m));
}
int v8s_rmdir(char *p)
{
	struct v8fstyp *t = FSFOR(p);

	return (t->t_remove(t->t_path(p, V8P_LOOK), 1));
}
/* a is the link TEXT and is stored verbatim -- resolving it would bake this
 * machine's rootfs path into a symlink the jail is supposed to interpret for
 * itself.  Only the new name is resolved. */
/* Only b is guarded: a is the link TEXT, stored verbatim and never resolved. */
/*
 * symlink -- THE LAST ONE THAT REFUSES, and §8a step 5g changed its WORD
 * without changing its verdict, which is the distinction v8s_mknod's device
 * arm made one step earlier and this line did not inherit.
 *
 * EROFS is a claim about the MEDIUM and it stopped being true at 5f: the
 * server takes writes, and `ln -s' on a mount answering "Read-only file
 * system" is a measurably false statement about a filesystem that had just
 * accepted a create.  EPERM is a claim about the OPERATION, and it is the one
 * that is true here and will stay true: a V7 filesystem has no i_mode for a
 * symbolic link, which is the same fact v8s_readlink answers EINVAL on, and
 * no amount of protocol could change it.  symlink(2) documents exactly this
 * -- EPERM when the filesystem does not support symbolic links.
 *
 * SO THIS IS NOT link's REFUSAL AND THE OLD COMMENT PAIRED THEM.  link got a
 * slot at 5g because a V7 filesystem is BUILT on hard links; symlink cannot
 * have one at any price.  The guard also loses its MOUNTED() for v8s_mknod's
 * reason: the mounted arm never touches the path, so there is nothing for a
 * containment guard to contain.
 */
int v8s_symlink(char *a, char *b)
{
	if (v8fs_mounted(b)) { v8_errno = V8_EPERM; return (-1); }
	RET(rawsys2(SYS_symlink, (long)a, (long)mkpath(b)));
}
/*
 * dup and dup2 -- and BOTH DROPPED THE DESCRIPTOR'S TYPE, which was invisible
 * while the only non-passthrough type was /proc and nothing dup'd one.
 *
 * v8fs_fdtype() is how every read, write, seek, close and ioctl finds its
 * filesystem, and a bare rawsys dup left the new descriptor unbound -- which
 * v8fs_fdtype reads as passthrough.  So dup(fd) on an open /proc file returned
 * a descriptor whose reads went to the host instead of to procfs, silently and
 * with the right-looking fd number.  /dev/fd is what makes it live: its whole
 * implementation is a dup, and its customers are exactly the programs that
 * pass descriptors around.
 *
 * dup2 overwrites the target's row rather than leaving whatever was there --
 * the host closes b when it is open, and a stale row would describe a
 * descriptor that no longer exists.  ONE call does that, because v8fs_bind
 * stores null for the passthrough type, and a v8fs_unbind() in front of it was
 * DEAD: the mutation that removed it changed no test, which is the only reason
 * anyone found out.  The comment that used to sit here acknowledged the two
 * calls collapse and then kept both.
 */
int v8s_dup(int f)
{
	long r = rawsys1(SYS_dup, f);

	if (r < 0) { v8_errno = v8sys_errno(RAWERR(r)); return (-1); }
	v8fs_bind((int)r, v8fs_fdtype(f));
	return ((int)r);
}

int v8s_dup2(int a, int b)
{
	long r;

	/*
	 * A dirfd being dup2'd OVER is closed by the host and must lose its V7
	 * snapshot too, or dir.c keeps serving records for a descriptor that is
	 * now something else entirely.  v8s_close does this through t_close; a
	 * raw dup2 never went near it.
	 */
	if (a != b && v8sys_isdirfd(b)) v8sys_dirclose(b);
	r = rawsys2(SYS_dup2, a, b);
	if (r < 0) { v8_errno = v8sys_errno(RAWERR(r)); return (-1); }
	v8fs_bind((int)r, v8fs_fdtype(a));
	return ((int)r);
}
int v8s_getpid(void)                     { return ((int)rawsys0(SYS_getpid)); }
int v8s_getppid(void)                    { return ((int)rawsys0(SYS_getppid)); }
/*
 * getuid AND ITS THREE SIBLINGS ARE RAW, AND THAT IS A DECISION RATHER THAN
 * THE ABSENCE OF ONE.
 *
 * Every 16-bit id FIELD in this port is folded (shim/v8id.h): st_uid here,
 * u_uid in /proc, u_uid in the v8fs server.  This is not a field, it is a
 * VALUE THAT FLOWS BACK OUT TO THE HOST, and the tree proves it in one line --
 * mv.c:56 is `setuid(getuid())', and v8s_setuid hands its argument straight to
 * the kernel.  A folded id there would try to become a user that does not
 * exist.  mkdir.c:69's `chown(d, getuid(), getgid())' is the same shape: on a
 * passthrough path it reaches the host's chown, which wants the real number.
 *
 * WHAT THAT COSTS, said plainly because it is the honest half.  A V8 program
 * comparing `st_uid == getuid()' -- ls.c:81, ps/doselect.c:30 -- is comparing a
 * folded 16-bit value against a raw 32-bit one, so on a host whose uid exceeds
 * 32767 they disagree.  THIS IS NOT A REGRESSION: the bare `(short)' cast that
 * preceded the fold disagreed with getuid() just as surely, for the same values.
 * Making them agree needs a two-way map -- fold on the way in, unfold on the
 * way back out to setuid and chown -- which is a real design and not this
 * commit.  What this commit fixes is the CONTRACT: non-root must never read as
 * root, because root is a privilege and a colliding non-root id is only a
 * wrong name.
 */
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
 * The exception list.  Short on purpose: every entry here is a hole in the
 * jail.  tests/jail asserts that a host tool which is NOT on this list is
 * still refused under strict.
 *
 * PLAN.md S1 sanctions as, ld, ar, strip and nm.  For a long time only clang
 * was spelled here, on the reasoning that cc(1) reaches all of them THROUGH
 * clang, so clang was the only path anything actually execed.  That was true
 * of every recipe this port had run, and it stopped being true the moment an
 * authentic makefile ran a build helper of its own: sh's `:fix' compiles to
 * assembly, rewrites .data to .text with ed -- the VAX shared-text trick, so
 * every shell process maps one copy of the message tables -- and then invokes
 * `$AS' by name.  V8's make supplies AS=as from its built-in macros.
 *
 * So the entry is not a new exception; it is the documented one, finally
 * reachable.  The same shape as v8s_mknod passing its path unresolved: an
 * unexercised rule cannot be seen to be incomplete.
 *
 * `strip' arrived the same way and for the same reason.  Admin/Mk is upstream's
 * build description for the half of cmd/ with no makefile, and its install()
 * is `strip $1 && cp $1 $2' -- so with strip missing the && short-circuits and
 * NOTHING IS INSTALLED, which reads as a build failure rather than as a jail
 * decision.  Note how it is reached, because it is not a new mechanism: sh
 * searches PATH=/bin:/usr/bin:/etc by execve, /bin/strip is a quiet miss (the
 * Mac has none either), and /usr/bin/ is a union mount whose rootfs half has no
 * strip -- so the host's is what the third probe finds, and this list is the
 * gate on it.  `nm' is still absent and tests/jail asserts it is refused, which
 * is what stops this array drifting into "everything PLAN.md mentions".
 */
static const char *hosttools[] = {
	"/usr/bin/clang", "/usr/bin/as", "/usr/bin/strip", 0
};

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
			} else if (rawsys2(SYS_access, (long)p, 0) != 0) {
				/*
				 * A MISS IS NOT AN ESCAPE, and the difference
				 * only became visible when a command moved.
				 *
				 * V8's sh searches PATH by calling execve on
				 * each directory in turn, so with
				 * PATH=/bin:/usr/bin every /usr/bin command
				 * probes /bin/<name> first.  Nothing is in that
				 * rootfs directory, vpath() leaves the path
				 * alone, and this arm reported an escape -- for
				 * a file the Mac does not have either.  It was
				 * invisible while every tool this port installs
				 * lived in /bin and the first probe always hit.
				 *
				 * So: refuse, quietly, when the host has no such
				 * file.  Nothing can run from a path that does
				 * not exist, which is the only thing V8JAIL is
				 * there to prevent; sh takes the ENOENT and
				 * tries the next directory, exactly as it would
				 * on a real machine.  The loud arms below still
				 * fire for a host binary that IS there, which is
				 * what tests/jail's negative cases use.
				 */
				v8_errno = V8_ENOENT;
				return (-1);
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

/*
 * vpath, AND IT HAD NONE AT ALL.  Every other path-taking syscall in this file
 * resolves; these two passed the V8 path straight to the host, so `ls -l' on a
 * jailed symlink read the Mac's (ls.c:365) and `mv' inside the jail stamped the
 * Mac's file of that name (mv.c:129).  The failure has two directions and the
 * quiet one is worse: loud ENOENT where the host has no such name, silently
 * wrong where it does -- and /etc, /bin, /usr/bin and /usr/lib are all names
 * the Mac also has.
 *
 * THE SHAPE IS THIS FILE'S MOST REPEATED ONE.  v8s_symlink twelve lines below
 * DOES resolve its new name, with mkpath -- so the port could create a jailed
 * symlink and then not read it back.  The fix landed on one line and the line
 * beside it kept the assumption.
 *
 * LOOK rather than MAKE for both: readlink and utime(2) each require the file
 * to exist, and V8P_MAKE keys on the parent, which would resolve a name inside
 * the jail that is not there instead of letting it fall through.
 *
 * Found by the dispatch sweep run before adding a fourth filesystem type, not
 * by anything failing -- and tests/v8sys could not have found it, because that
 * suite had been running with V8ROOT unset and therefore with the jail off.
 */
long v8s_readlink(char *p, char *b, long n)
{
	/*
	 * EINVAL AND NOT EROFS, because readlink is a reader and the question
	 * has a real answer: a V7 image has no symbolic links -- V8 added them
	 * to ITS filesystem, and mkfs(8) writes no IFLNK -- so every name on a
	 * mount is "not a symbolic link", which is exactly what readlink(2)
	 * reports as EINVAL.  Refusing with EROFS would make ls -l print a
	 * different wrong thing.
	 */
	if (v8fs_mounted(p)) { v8_errno = V8_EINVAL; return (-1); }
	RET(rawsys3(SYS_readlink, (long)vpath(p), (long)b, n));
}

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
	struct v8fstyp *t;

	kmemu_synth(path, v8root());
	t = FSFOR(path);
	/*
	 * O_CREAT (0x0200) takes the creator's path, for the reason creat(2)
	 * does -- rootpath cannot resolve a name that does not exist yet, so
	 * without this an open that CREATES a file in a jailed directory writes
	 * to the Mac.  Everything else takes the reader's.  shim/NOTES.md.
	 */
	return t->t_open(t->t_path(path, (flags & 0x0200) ? V8P_MAKE : V8P_LOOK),
	    flags, mode);
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
	/*
	 * AND IT WENT STRAIGHT TO THE HOST SYSCALL UNTIL /dev/fd ARRIVED.  The
	 * line below used to be `RET(rawsys3(SYS_open, (long)mkpath(path), ...))'
	 * -- correct for passthrough, and structurally unable to reach a second
	 * type, because mkpath() resolves a path while FSFOR() chooses who
	 * answers for it and only the first half was here.  Nothing noticed,
	 * because /proc is read-only and passthrough was every other row.
	 *
	 * fd.4 is what makes it live: "Creat(2) is equivalent to open, and mode
	 * is ignored", so creat("/dev/tty", 0666) must dup fd 3 rather than
	 * truncate the node.  The same shape as v8s_mknod passing its path
	 * unresolved -- an unexercised rule cannot be seen to be incomplete.
	 */
	struct v8fstyp *t = FSFOR(path);

	return t->t_open(t->t_path(path, V8P_MAKE),
	    0x0001 /*O_WRONLY*/ | 0x0200 /*O_CREAT*/ | 0x0400 /*O_TRUNC*/, mode);
}

long
v8s_read(int fd, char *buf, long n)      { return FDFS(fd)->t_read(fd, buf, n); }

long
v8s_lseek(int fd, long off, int whence)  { return FDFS(fd)->t_seek(fd, off, whence); }

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

/*
 * rootfs_has() up at the top of this file hands SYS_lstat64 a bare byte array,
 * because this declaration is a thousand lines below it and nothing is read out
 * of the result.  The size is asserted HERE, where the type is complete, rather
 * than left as a comment claiming it is big enough.
 */
_Static_assert(sizeof(struct hoststat64) <= V8_STATBUF,
    "V8_STATBUF is smaller than the kernel's stat64 -- rootfs_has would smash its stack");

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
	/*
	 * FOLDED, NOT CAST, and this was a bare `(short)' until the sweep that
	 * followed §8a step 5f-b.  st_uid is `short' -- V8's own width -- and a
	 * host id is 32 bits, so the cast mapped every multiple of 65536 onto
	 * ZERO.  Measured: 65536 -> 0 and 131072 -> 0, in the stat(2) path every
	 * `ls -l' goes through, which means a file owned by such a user reads as
	 * owned by ROOT.  shim/v8id.h has the contract and the history; the
	 * short version is that fio.c was given this fix after an auditor found
	 * the identical cast there, and the fix reached one component of three.
	 *
	 * NOTE WHAT IS DELIBERATELY NOT FOLDED: v8s_getuid.  See the note there.
	 */
	vs->st_uid   = v8_foldid((long)hs->st_uid);
	vs->st_gid   = v8_foldid((long)hs->st_gid);
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

/*
 * The passthrough type's stat, on an ALREADY-RESOLVED path.  It lives here
 * rather than in vfs.c because it is bound to stat_translate() and to
 * struct hoststat64 above, and a second copy of a struct conversion at this
 * seam is how this shim's worst bugs have started.  vfs.c's table points at it.
 */
int v8sys_pt_stat(char *rp, struct v8_stat *vs, int follow)
{
	struct hoststat64 hs;
	long r = rawsys2(follow ? SYS_stat64 : SYS_lstat64, (long)rp, (long)&hs);
	if (r < 0) { v8_errno = v8sys_errno(RAWERR(r)); return (-1); }
	stat_translate(&hs, vs);
	return (0);
}

int v8s_stat(char *p, struct v8_stat *vs)
{
	struct v8fstyp *t = FSFOR(p);
	return t->t_stat(t->t_path(p, V8P_LOOK), vs, 1);
}

int v8s_lstat(char *p, struct v8_stat *vs)
{
	struct v8fstyp *t = FSFOR(p);
	return t->t_stat(t->t_path(p, V8P_LOOK), vs, 0);
}

int v8sys_pt_fstat(int f, struct v8_stat *vs)
{
	struct hoststat64 hs;
	long r = rawsys2(SYS_fstat64, f, (long)&hs);
	long dsz;

	if (r < 0) { v8_errno = v8sys_errno(RAWERR(r)); return (-1); }
	stat_translate(&hs, vs);
	/*
	 * A DIRECTORY'S SIZE IS THE SIZE OF WHAT read(2) WILL GIVE, not what
	 * the host filesystem happens to charge for it.  The shim presents a
	 * run of 256-byte V7 records built from variable-length host entries,
	 * so the host's number is unrelated -- /etc is nine entries, 2304 bytes
	 * of records and an APFS st_size of 288.  See v8sys_dirsize() in dir.c.
	 *
	 * fstat only.  stat(2) by path cannot answer without snapshotting the
	 * directory, which would put a getdirentries loop inside every `ls -l'
	 * of a tree; and it does not need to, because nothing can READ a
	 * directory without opening it first.  Recorded rather than hidden.
	 */
	if ((dsz = v8sys_dirsize(f)) >= 0)
		vs->st_size = (v8_off_t)dsz;
	return (0);
}

int v8s_fstat(int f, struct v8_stat *vs)  { return FDFS(f)->t_fstat(f, vs); }

/*
 * ioctl dispatches on the descriptor's filesystem, exactly as V8's does: its
 * sys/ioctl.c hands an inode's fstypsw the request, and a /proc inode answers
 * PIOCGETPR from prioctl while an ordinary one falls through to the device.
 * Here the sgtty translation is the passthrough type's op (ioctl.c) and /proc's
 * is libkmemu's, and a descriptor that belongs to neither cannot reach the
 * wrong one -- v8fs_fdtype() decides, and it was set at open.
 */
int v8s_ioctl(int f, int cmd, char *arg) { return FDFS(f)->t_ioctl(f, cmd, arg); }

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
	struct v8fstyp *t = FSFOR(p);

	/*
	 * A SLOT SINCE §8a step 5f-b -- see v8s_chmod for why the MOUNTED()
	 * that stood here had to go.  What used to be in this function is the
	 * timeval conversion, and it moved into vfs.c's pt_utime unchanged: it
	 * was always passthrough's business rather than the dispatcher's, in
	 * exactly the way ioctl.c's sgtty translation was.
	 *
	 * THE ALIASING TRAP IS GONE WITH IT and is worth recording because it
	 * was the reason this function had a body.  rootpath() returns a
	 * pointer into its own static buffer, so two calls in one expression
	 * silently give you the same string twice -- v8s_link records it.  A
	 * single t_path() call cannot have the problem, which is a property of
	 * the dispatch shape and not a thing anyone has to remember here.
	 */
	return (t->t_utime(t->t_path(p, V8P_LOOK), tv));
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
