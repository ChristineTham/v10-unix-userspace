/*
 * The filesystem switch.  PLAN.md section 8a step 2.
 *
 * WHY THIS SHAPE.  V8 already had one.  sys/h/conf.h defines
 *
 *	struct fstypsw {
 *		int (*t_put)(); struct inode *(*t_get)(); int (*t_free)();
 *		int (*t_updat)(); int (*t_read)(); int (*t_write)();
 *		int (*t_trunc)(); int (*t_stat)(); int (*t_nami)();
 *		int (*t_mount)(); int (*t_ioctl)();
 *	};
 *
 * and sys/dev/conf.c fills it with four entries -- the ordinary filesystem, the
 * network filesystem, Killian's /proc, and mpx -- with iget.c, nami.c, rdwri.c
 * and ioctl.c all dispatching through it.  V8 had a VFS four years before Sun's
 * paper.  So the interface below answers to that one rather than inventing a
 * rival, and the correspondence is written next to each entry.
 *
 * WHERE IT DIFFERS, AND WHY IT HAS TO.  V8's operations take a `struct inode *'
 * and find their buffer in the u-area: `t_read(ip)' has no length argument
 * because the length is `u.u_count'.  This shim has neither an inode table nor
 * a u-area -- it hands out host descriptors -- so the operations here are
 * descriptor-shaped.  Building the inode/u-area substrate is real work, shared
 * with step 5 (which wants iget.c anyway), and doing it before there is a
 * second filesystem to justify it would be inventing a customer.
 *
 * ONE ENTRY TODAY.  Passthrough, reproducing the previous behaviour exactly.
 * That is the whole point of this step: the floor is replaced while the suites
 * stay green, so that when /proc arrives as the second entry a failure means
 * /proc rather than the switch.
 */

#ifndef V8SYS_VFS_H
#define V8SYS_VFS_H

struct v8_stat;

/*
 * Path resolution has THREE modes, and the distinctions are load-bearing --
 * see shim/NOTES.md.  V8P_LOOK asks whether the rootfs has the path, which is
 * right for a reader and unanswerable for a name being created; V8P_MAKE asks
 * about the parent instead.
 *
 * V8P_ENTRY IS §8a step 5i's, AND IT SAYS THE SAME THING ABOUT A DIFFERENT
 * QUESTION.  Once the shim tracks a working directory it must fold `..'
 * lexically -- there is no kernel here to fix up a walk that crosses a mount
 * upward, so `cd /mnt; cd ..' would otherwise leave a program inside the image
 * (p9.h and syscall.c's v8s_chdir both record the measurement).  But folding
 * the LAST component destroys the one thing 5h's Tunlink exists to name: `..'
 * as an ENTRY, which is what rmdir(1)'s three unlinks and mv(1)'s reparenting
 * are made of.  unlink("/mnt/d/..") folded whole is unlink("/mnt").
 *
 * So the mode is threaded rather than inferred, and the split is exactly the
 * one dotlink()'s two callers already draw:
 *
 *	V8P_LOOK	reach an object.  Fold the path whole.
 *	V8P_ENTRY	name an entry in a directory.  Fold all but the last.
 *	V8P_MAKE	name an entry that does not exist yet -- ENTRY's fold,
 *			plus the parent rule for the rootfs union.
 *
 * "Fold all but the last EVERYWHERE" was considered and is wrong: it makes
 * opendir("..") at a mount root reach the image root while chdir("..") reaches
 * the jail root, and getwd(3) does both in one loop (getwd.c:41-45) and
 * requires them to agree.
 */
#define V8P_LOOK	0
#define V8P_MAKE	1
#define V8P_ENTRY	2

struct v8fstyp {
	const char *t_name;

	/*
	 * t_path -- V8's t_nami, reduced.  Turn a V8 path into whatever this
	 * filesystem's other operations want.  For passthrough that is a host
	 * path; for a server-backed type it would be a handle.
	 */
	char *(*t_path)(char *p, int mode);

	int   (*t_open)(char *rp, int flags, int mode);	/* t_get  */
	int   (*t_close)(int fd);			/* t_put  */
	long  (*t_read)(int fd, char *b, long n);	/* t_read */
	long  (*t_write)(int fd, char *b, long n);	/* t_write */
	long  (*t_seek)(int fd, long off, int whence);
	int   (*t_stat)(char *rp, struct v8_stat *st, int follow);   /* t_stat */
	int   (*t_fstat)(int fd, struct v8_stat *st);

	/*
	 * t_ioctl -- V8's own, and the slot this header used to say was
	 * deliberately absent "until a type needs it".  That type has arrived:
	 * PIOCGETPR is how ps(1) asks /proc for a struct proc, and proca.c
	 * answers it from prioctl (proca.c:290) through exactly this vector.
	 *
	 * Note what the arrival changed and what it did not.  The sgtty/termios
	 * translation in ioctl.c is not per-filesystem and did not move; it
	 * became the PASSTHROUGH type's implementation of this operation, which
	 * is what it always was in fact.  Only the dispatch is new.
	 */
	int   (*t_ioctl)(int fd, int cmd, char *arg);	/* t_ioctl */

	/*
	 * THE THREE §8a STEP 5f ADDED, AND THEY ARE THE FIRST OF THE FOURTEEN
	 * SLOTLESS SYSCALLS TO ARRIVE.  syscall.c's MOUNTED() macro exists to
	 * refuse the ones that have no slot, because without a slot a mutating
	 * call resolves a mounted path through rootpath() and acts on the HOST.
	 * That refusal was the truth while the server answered EROFS to every
	 * write; from 5f it is a lie about a writable filesystem, so the calls
	 * that can now be honoured are honoured and the rest still refuse.
	 *
	 * t_access IS NOT A MUTATOR and is here for a different reason: it is
	 * the one READER whose answer the client cannot compute.  See p9cl.c.
	 *
	 * t_remove TAKES isdir FROM THE CALLER rather than stat-ing, because
	 * unlink(2) on a directory and rmdir(2) on a file are different errors
	 * and only the caller knows which syscall was made.  -1 means "do not
	 * check", which nothing passes today and which exists so that a caller
	 * with no opinion has a spelling other than a wrong one.
	 *
	 * WHAT STILL HAS NO SLOT AFTER THIS, and why, so that the next step
	 * inherits a list rather than a survey: symlink, because a V7 image
	 * cannot represent one; mknod for a device, meaningless on an image no
	 * kernel will mount; chroot and execve, the two the enumeration in
	 * syscall.c had missed, which are their own questions.
	 *
	 * chdir WAS ON THAT LIST AND CAME OFF IT IN §8a step 5i -- and it never
	 * took a slot, which is the point.  It needed a working directory this
	 * shim did not track; with one, `cd' into a mount is answered by
	 * v8fs_logical and v8s_chdir rather than by a per-type operation,
	 * because where a process is standing is a fact about the NAMESPACE and
	 * not about any one filesystem in it.  A slot would have made each type
	 * answer separately and then have to agree.
	 *
	 * THIS LIST USED TO OPEN "link and symlink have no 9P2000 message at
	 * all", pairing them, and §8a step 5g is the step that took the pair
	 * apart.  The missing message was never the reason -- it is the exact
	 * situation that produced Tseek and Taccess -- and the two halves are
	 * not alike: a V7 filesystem cannot represent a symlink at any price,
	 * and it is BUILT on hard links.  p9.h has the argument and the
	 * measurement that made it matter (mv of a directory failed outright,
	 * because mvdir has no fork-and-cp fallback).
	 */
	int   (*t_access)(char *rp, int mode);
	int   (*t_remove)(char *rp, int isdir);
	int   (*t_mkdir)(char *rp, int mode);

	/*
	 * THE THREE §8a STEP 5f-b ADDED, and they are ONE MESSAGE on the wire
	 * even though they are three syscalls here.  9P's Twstat carries a whole
	 * stat and the server applies whichever fields are not "do not touch",
	 * so chmod is a wstat setting s_mode, chown one setting s_uid/s_gid, and
	 * utime one setting s_atime/s_mtime.  The line above used to say they
	 * were "one Twstat away and deferred only to keep this step reviewable";
	 * this is that step.
	 *
	 * t_utime TAKES V7's OWN ARGUMENT, a time_t[2] of {atime, mtime} or a
	 * null pointer, rather than something host-shaped.  That is deliberate:
	 * mv.c:129 passes &st.st_atime and relies on the two fields being
	 * adjacent, so the shape the caller uses is part of what has to be
	 * reproduced, and each type converts for itself.
	 */
	int   (*t_chmod)(char *rp, int mode);
	int   (*t_chown)(char *rp, int uid, int gid);
	int   (*t_utime)(char *rp, long *tv);

	/*
	 * THE ONE §8a step 5g ADDED, and it is the only slot here that takes
	 * TWO resolved names.  That shape is forced by the operation rather
	 * than chosen: a link joins two points in one filesystem, so both have
	 * to be resolved before the type can be asked, and both by the SAME
	 * type -- v8s_link refuses a mismatched pair with EXDEV, which is the
	 * true answer (two filesystems) rather than a refusal.
	 *
	 * IT IS ALSO THE SLOT THAT MAKES THE ALIASING TRAP UNAVOIDABLE RATHER
	 * THAN AVOIDABLE.  t_path returns a pointer into a static buffer, so a
	 * caller holding two of its results at once has the same string twice.
	 * Every other operation here takes one path and never notices; this one
	 * cannot be written correctly without copying the first, which is why
	 * v8s_link has carried that copy since before there was a switch.
	 */
	int   (*t_link)(char *rold, char *rnew);
};

/*
 * The mount table is the prefix list that used to be `v8dirs[]'.  Generalising
 * it rather than adding a second list beside it is deliberate: two prefix
 * tables that have to agree are the standing invitation kmem.c's one-table rule
 * exists to refuse.  A /proc entry is one more row here.
 */
struct v8fstyp *v8fs_typefor(const char *path, int mode);
struct v8fstyp *v8fs_fdtype(int fd);
void            v8fs_bind(int fd, struct v8fstyp *t);
void            v8fs_unbind(int fd);

/*
 * THE LOGICAL WORKING DIRECTORY -- §8a step 5i, and the reason it is here
 * rather than in syscall.c is that it has to run BEFORE dispatch as well as
 * before resolution.  A relative name under a mount belongs to the server, and
 * v8fs_typefor cannot see that in the name itself.
 *
 * v8fs_logical() is the whole of it: it makes a path absolute against the cwd
 * and folds it to the mode's rule.  It is the IDENTITY whenever no v8fs mount
 * is configured, which is every run of every other suite -- so the lexical
 * reading of `..' is confined to processes that have asked for a mount.
 *
 * THE cwd LIVES IN THE ENVIRONMENT, as V8CWD, for vfs.c's own recorded reason:
 * a table in process memory dies when a program replaces its image, and sh(1)
 * runs every command by fork and exec.  v8s_execve splices it in; nothing
 * writes it with putenv, because this shim has no libc.
 */
char           *v8fs_logical(char *path, int mode);
const char     *v8fs_cwd(void);
int             v8fs_setcwd(const char *path);
/*
 * The fold with no scope test, in place, on a path that is already absolute.
 * p9cl.c normalises V8MOUNT's prefix with it, which v8fs_logical cannot do:
 * asking whether a mount exists is what p9cl is in the middle of answering.
 */
void            v8fs_foldabs(char *path, int mode);

/*
 * The longest path the fold can hold.  1024 is past what getwd(3) could report
 * anyway -- getwd.c:14 sizes its own buffer at 512 -- so a path this shim
 * cannot hold is one no V8 program could print.
 */
#define V8_CWDMAX	1024

extern struct v8fstyp v8fs_pass;

/*
 * /dev/fd -- the third type, and the one that answers for /dev/tty.  V8's
 * controlling terminal is not a device: open("/dev/fd/n") is dup(n), and
 * /dev/tty is the hard link at n = 3.  vfs.c has the four citations.
 */
extern struct v8fstyp v8fs_fdfs;

/*
 * /dev/null -- the FIFTH type, two operations, and the one that exists because
 * making a NAME authentic made an OBJECT wrong.  V8 shipped /dev/null
 * (proto-dev:25) so the rootfs has to have the node; the jail's union rule then
 * opened that node, and a write to /dev/null accumulated in it while a read
 * handed the accumulation back.  t_path returns the name unresolved so every
 * inherited operation reaches the HOST's device, whose two behaviours are
 * Bell Labs' own (dev/mem.c:68 and :156).  vfs.c has the measurement.
 */
extern struct v8fstyp v8fs_null;

/*
 * v8fs -- the FOURTH type, and the first that is not in this process.  V8's own
 * namei/iget/bmap/readi answer it, over a 9P socket, in shim/v8fsd.  p9cl.c has
 * the design; the three things a caller here needs to know are:
 *
 *  - it is CONFIGURED AT RUN TIME, from V8MOUNT, so it is not a row in the
 *    static table above.  v8fs_p9for() is the lookup and v8fs_typefor() asks it
 *    FIRST, because shadowing what is underneath is what mounting means.
 *  - a descriptor is identified by asking the KERNEL (getpeername), not by a
 *    table, which is what makes an inherited one work.  v8fs_p9adopt() is that
 *    question and v8fs_fdtype() asks it only when its own table has no opinion.
 *  - v8fs_mounted() is the guard for the syscalls that have no slot in this
 *    struct.  Without it they resolve a mounted path through rootpath() and act
 *    on the HOST -- see its comment in syscall.c.
 */
extern struct v8fstyp v8fs_p9;
struct v8fstyp *v8fs_p9for(const char *path);
struct v8fstyp *v8fs_p9adopt(int fd);
int             v8fs_mounted(const char *path, int mode);
/*
 * "Is any mount configured at all" -- the scope of the fold, and deliberately
 * a different question from v8fs_p9for's "does the mount claim THIS path".
 * v8fs_logical needs the first, because a relative name cannot be tested
 * against a prefix until it has been made absolute.
 */
int             v8fs_p9any(void);

#endif /* V8SYS_VFS_H */
