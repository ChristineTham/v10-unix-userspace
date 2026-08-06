/*
 * /proc -- Killian's process filesystem, manufactured.
 *
 * V8's own is sys/sys/proca.c, 716 lines, and it is fstyp 2 in the kernel's
 * `struct fstypsw' table.  This is not that file, and the decision not to
 * import it was made by reading it rather than by looking at its function
 * names.  PLAN.md section 8a step 3 records the measurement; the short version:
 *
 *   About 500 of proca.c's lines are portable as C -- prioctl alone is 131 with
 *   no VAX reference at all.  But its operations are written against the V8
 *   KERNEL'S INTERNALS: `t_read(ip)' takes an inode and no length, because the
 *   length is u.u_count.  Standing it up needs struct inode, the u-area,
 *   iomove, proc[], pfind, iget, namei, open1, tsleep/wakeup, psignal, setrq --
 *   about twenty-five substrate functions, which is most of the kernel.
 *   stream.c needed nine names; this needs a kernel.
 *
 * So the CONVENTIONS come from proca.c, exactly, and the implementation answers
 * from the host.  That is libkmemu's existing bargain -- who(1) reads an
 * /etc/utmp nothing else writes -- one level up: ps(1) reads a /proc nothing
 * else mounts.  Every constant below is cited to the line it came from.
 */

#include "kmemu.h"
#include "../v8sys/vfs.h"
#include "../v8sys/v8sys.h"
#include "../v8sys/rawsys.h"
#include <sys/types.h>
#include <libproc.h>

extern int v8_errno;
struct v8fstyp *kmemu_procfs(void);

/*
 * FROM proca.c AND ITS HEADERS, cited so the next reader can check them.
 *
 *   ROOTINO   2    h/param.h:73 -- "i number of all roots".  /proc reuses it
 *                  for its own directory (proca.c:51,67,109,116,159,181).
 *   PRMAGIC   64   proca.c:41 -- the inode number IS the pid plus this, and
 *                  because 64 > ROOTINO there is no collision with the
 *                  directory itself (proca.c:133 going out, :53,:74,:145
 *                  coming back).
 *   names          five digits, zero-padded, written back to front
 *                  (proca.c:134-135).  d_name[5..] stays NUL.
 *   empty slot     d_ino = 0 (proca.c:136-137), which is V7's own rule for an
 *                  unused entry and is what every reader already skips.
 *   size           (nproc + 2) records: "." and ".." plus one per SLOT, holes
 *                  included (proca.c:71).  A fixed-size directory.
 */
#define PR_ROOTINO	2
#define PRMAGIC		64

/*
 * THE TABLE SIZE, and there is no authentic number to inherit.
 *
 * V8's is `NPROC (20 + 8 * MAXUSERS)' (sys/param.c:26), and MAXUSERS came from
 * the per-machine config file that mkconf(8) generated -- which is not in the
 * vendored tree, for any of research, forbes or alice.  So this is a choice,
 * and it is recorded as one.
 *
 * 1024 covers this Mac comfortably (about 620 processes as measured) and keeps
 * the directory a stable, fixed-size thing the way V8's was.  If the host ever
 * has more, the excess is REPORTED rather than dropped: prslots() says so on
 * stderr.  A silent cap here would make ps(1) quietly omit processes, which is
 * the one thing a process lister must not do.
 */
#define PR_NPROC	1024

/* A V7 directory record, at THIS PORT's DIRSIZ.  See src/include/dir.h: 254,
 * not 14, so the record is 256 bytes.  /proc must speak the same dialect as
 * every other directory here or ps(1) reads it at the wrong stride. */
#define PR_DIRSIZ	254
struct prdirect {
	unsigned short	d_ino;
	char		d_name[PR_DIRSIZ];
};

#define PR_SDSIZ	((long)sizeof(struct prdirect))

/* One row per slot.  pid 0 means the slot is free, exactly as V8's p_pid does
 * (proca.c:132 tests `if (n = proc[i].p_pid)'). */
static int	slots[PR_NPROC];
static int	nslots;			/* always PR_NPROC; the table is fixed */
static int	loaded;

/*
 * Per-descriptor state.  A V8 program holds an ordinary fd, so /proc needs a
 * real descriptor number to hand back; it comes from opening /dev/null, which
 * costs one host fd and makes close, dup and inheritance work with no special
 * cases anywhere else in the shim.
 */
#define PR_NFD	64
static struct prfile {
	int	fd;			/* -1 when free */
	int	pid;			/* 0 for the directory itself */
	long	off;
} prfiles[PR_NFD];

static struct prfile *
prfind(int fd)
{
	int i;

	for (i = 0; i < PR_NFD; i++)
		if (prfiles[i].fd == fd) return (&prfiles[i]);
	return (0);
}

/*
 * Fill the slot table from the host.  proc_listpids(2) is on PLAN.md section
 * 7's sanctioned list -- documented, stable, and answering "what is running",
 * which is the whole reason libkmemu may reach for libc at all.
 */
static void
prslots(void)
{
	static int pidbuf[PR_NPROC * 2];
	int n, i;

	for (i = 0; i < PR_NPROC; i++) slots[i] = 0;
	nslots = PR_NPROC;
	loaded = 1;

	n = proc_listpids(PROC_ALL_PIDS, 0, pidbuf, (int)sizeof pidbuf);
	if (n <= 0) return;
	n /= (int)sizeof(int);

	for (i = 0; i < n && i < PR_NPROC; i++)
		if (pidbuf[i] > 0) slots[i] = pidbuf[i];

	if (n > PR_NPROC) {
		/* NOT silent.  See the note on PR_NPROC. */
		static const char msg[] =
		    "/proc: more processes than table slots; some are not listed\n";
		rawsys3(SYS_write, 2, (long)msg, (long)sizeof msg - 1);
	}
}

/* The directory, as a byte stream.  Record i is: 0 ".", 1 "..", then slot i-2. */
static void
prrecord(long i, struct prdirect *d)
{
	int j, n;

	for (j = 0; j < PR_DIRSIZ; j++) d->d_name[j] = 0;

	if (i == 0 || i == 1) {
		/* Both point at ROOTINO: /proc's parent is itself (proca.c:109-110). */
		d->d_ino = PR_ROOTINO;
		d->d_name[0] = '.';
		if (i == 1) d->d_name[1] = '.';
		return;
	}
	n = slots[i - 2];
	if (n == 0) { d->d_ino = 0; return; }	/* free slot (proca.c:136-137) */
	/*
	 * d_ino IS 16 BITS AND A MACOS PID IS NOT.  V8's pids were `short', so
	 * pid + PRMAGIC always fit and proca.c never had to think about it; here
	 * pids run to 99998 (kern.maxproc) and the sum wraps -- measured, pid
	 * 67757 lands on 2285.  Wrapping is harmless in itself, because nothing
	 * maps the number back to a pid; the NAME does that, and prnami parses
	 * the name.
	 *
	 * Except for one value.  A pid of 65472 wraps to exactly 0, and 0 is
	 * V7's "this slot is unused" -- so that one process would vanish from
	 * every directory reader in the tree, with nothing to say so.  Folded to
	 * 1 instead, which collides with nothing that matters for the same
	 * reason the wrap does not.  Same defence, and the same reasoning, as
	 * v8sys_fold_ino() in dir.c never producing 0.
	 */
	d->d_ino = (unsigned short)(n + PRMAGIC);
	if (d->d_ino == 0) d->d_ino = 1;
	for (j = 4; j >= 0; j--) { d->d_name[j] = (char)(n % 10 + '0'); n /= 10; }
}

static long
prdirsize(void)
{
	return ((long)(PR_NPROC + 2) * PR_SDSIZ);
}

/* ------------------------------------------------------ the operations */

static char *
pr_path(char *p, int mode)
{
	(void)mode;
	return (p);		/* /proc names nothing on the host to resolve */
}

/* Parse "/proc/01234" -> 1234.  0 for "/proc" itself, -1 for anything else.
 * Strictly all-digits after the slash, as prnami is (proca.c:232-236). */
static int
prpid(const char *p)
{
	int n = 0, any = 0;

	if (p[0]!='/'||p[1]!='p'||p[2]!='r'||p[3]!='o'||p[4]!='c') return (-1);
	if (p[5] == '\0') return (0);
	if (p[5] != '/') return (-1);
	if (p[6] == '\0') return (0);		/* "/proc/" is the directory too */
	for (p += 6; *p; p++) {
		if (*p < '0' || *p > '9') return (-1);
		n = n * 10 + (*p - '0');
		any = 1;
	}
	/*
	 * PID 0 IS NOT THE DIRECTORY, and returning 0 for it made /proc/00000
	 * open /proc.  V8 gets the same answer by a different route: prnami
	 * parses the digits, iget calls prget, prget calls pfind(0), and pfind
	 * walks the hash chain until it reaches proc[0] -- which is the
	 * SENTINEL, the swapper, never on a chain -- so it returns null and the
	 * open is ENOENT (subr.c:229-239).  Same outcome, stated directly.
	 */
	if (!any || n == 0) return (-1);
	return (n);
}

static int
prlive(int pid)
{
	int i;

	if (!loaded) prslots();
	for (i = 0; i < PR_NPROC; i++)
		if (slots[i] == pid) return (1);
	return (0);
}

static int
pr_open(char *rp, int flags, int mode)
{
	int pid = prpid(rp);
	long fd;
	struct prfile *f;

	(void)flags; (void)mode;
	if (pid < 0) { v8_errno = V8_ENOENT; return (-1); }
	prslots();				/* a fresh view per open */
	if (pid > 0 && !prlive(pid)) { v8_errno = V8_ENOENT; return (-1); }
	if ((f = prfind(-1)) == 0) { v8_errno = V8_EMFILE; return (-1); }

	/*
	 * A REAL DESCRIPTOR, from /dev/null.  The V8 program is going to hold
	 * this number, pass it to close, maybe dup it, and inherit it across an
	 * exec -- so it has to be a descriptor the kernel knows about.  One host
	 * fd per open /proc file is the price, and it buys no special cases
	 * anywhere else in the shim.
	 */
	fd = rawsys3(SYS_open, (long)"/dev/null", 0L, 0L);
	if (fd < 0) { v8_errno = v8sys_errno(RAWERR(fd)); return (-1); }

	f->fd = (int)fd; f->pid = pid; f->off = 0;
	v8fs_bind((int)fd, kmemu_procfs());
	return ((int)fd);
}

static int
pr_close(int fd)
{
	struct prfile *f = prfind(fd);

	if (f) f->fd = -1;
	rawsys1(SYS_close, fd);
	return (0);
}

static long
pr_read(int fd, char *b, long n)
{
	struct prfile *f = prfind(fd);
	long done = 0;

	if (f == 0) { v8_errno = V8_EBADF; return (-1); }
	if (f->pid != 0) {
		/*
		 * /proc/<pid> as a byte stream is the process's ADDRESS SPACE,
		 * indexed by virtual address -- ps(1) seeks to UBASE and reads
		 * a struct user.  Answering that means manufacturing a u-area
		 * from proc_pidinfo, which is the next slice of section 8a step
		 * 3.  Until then it reads empty rather than reading a lie.
		 */
		return (0);
	}
	while (done < n) {
		struct prdirect rec;
		long i = f->off / PR_SDSIZ;
		long within = f->off % PR_SDSIZ;
		long take = PR_SDSIZ - within;

		if (i >= (long)PR_NPROC + 2) break;
		if (take > n - done) take = n - done;
		prrecord(i, &rec);
		{
			char *src = (char *)&rec + within;
			long k;
			for (k = 0; k < take; k++) b[done + k] = src[k];
		}
		done += take;
		f->off += take;
	}
	return (done);
}

static long
pr_write(int fd, char *b, long n)
{
	(void)fd; (void)b; (void)n;
	v8_errno = V8_EACCES;		/* prwrite needs the process image */
	return (-1);
}

static long
pr_seek(int fd, long off, int whence)
{
	struct prfile *f = prfind(fd);
	long base;

	if (f == 0) { v8_errno = V8_EBADF; return (-1); }
	switch (whence) {
	case 0: base = 0; break;
	case 1: base = f->off; break;
	case 2: base = f->pid ? 0 : prdirsize(); break;
	default: v8_errno = V8_EINVAL; return (-1);
	}
	f->off = base + off;
	return (f->off);
}

static void
prstat(struct v8_stat *st, int pid)
{
	char *p = (char *)st;
	int i;

	for (i = 0; i < (int)sizeof *st; i++) p[i] = 0;
	st->st_nlink = 1;
	st->st_uid = 0;
	st->st_gid = 0;
	if (pid == 0) {
		st->st_ino = PR_ROOTINO;
		st->st_mode = (unsigned short)(V8_S_IFDIR | 0555);
		st->st_size = (v8_off_t)prdirsize();
	} else {
		st->st_ino = (v8_ino_t)(pid + PRMAGIC);
		st->st_mode = (unsigned short)(V8_S_IFREG | 0600);
		st->st_size = 0;
	}
}

static int
pr_stat(char *rp, struct v8_stat *st, int follow)
{
	int pid = prpid(rp);

	(void)follow;
	if (pid < 0) { v8_errno = V8_ENOENT; return (-1); }
	if (pid > 0 && !prlive(pid)) { v8_errno = V8_ENOENT; return (-1); }
	prstat(st, pid);
	return (0);
}

static int
pr_fstat(int fd, struct v8_stat *st)
{
	struct prfile *f = prfind(fd);

	if (f == 0) { v8_errno = V8_EBADF; return (-1); }
	prstat(st, f->pid);
	return (0);
}

static struct v8fstyp procfs = {
	"proc",
	pr_path,
	pr_open, pr_close,
	pr_read, pr_write, pr_seek,
	pr_stat, pr_fstat
};

/*
 * The hook vfs.c's mount table reaches.  shim/v8sys/noprocfs.c has the
 * do-nothing version that every non-groveler links instead.
 */
struct v8fstyp *
kmemu_procfs(void)
{
	static int init;
	int i;

	if (!init) {
		init = 1;
		for (i = 0; i < PR_NFD; i++) prfiles[i].fd = -1;
	}
	return (&procfs);
}
