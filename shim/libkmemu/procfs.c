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
#include <sys/sysctl.h>
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

/* --------------------------------------------------------- PIOCGETPR */

/*
 * struct proc, AND ITS SHAPE IS THE ABI.
 *
 * prioctl answers PIOCGETPR with `iomove((char *)p, sizeof(struct proc),
 * B_READ)' (proca.c:323) -- the kernel's own proc slot, copied out verbatim,
 * no marshalling anywhere.  So there is no layer that could absorb a
 * disagreement between what this file writes and what ps(1) compiled against:
 * a field in the wrong place yields plausible numbers rather than an error.
 *
 * Spelled again here for utmp.c's reason -- this file is clang-compiled for the
 * host, and <sys/proc.h> belongs to the V8 include tree v8cc reads.  Kernel
 * struct pointers become `char *' because nothing here can dereference them,
 * and V8's typedefs resolve as sys/types.h says: size_t and swblk_t are long,
 * caddr_t is char *, u_short is unsigned short.
 *
 * Upstream, for checking against (v8/usr/include/sys/proc.h):
 *
 *	struct proc *p_link, *p_rlink;  struct pte *p_addr;
 *	char p_usrpri, p_pri, p_cpu, p_stat, p_time, p_nice, p_slptime, p_cursig;
 *	long p_sig, p_siga0, p_siga1;   int p_flag;
 *	int p_uid, p_pgrp, p_pid, p_ppid;   short p_poip, p_szpt;
 *	size_t p_tsize, p_dsize, p_ssize, p_rssize, p_maxrss, p_swrss;
 *	swblk_t p_swaddr;  caddr_t p_wchan;  struct text *p_textp;
 *	u_short p_clktim, p_tsleep;  struct pte *p_p0br;  struct proc *p_xlink;
 *	short p_cpticks;  float p_pctcpu;  short p_ndx, p_idhash;
 *	struct proc *p_pptr;  struct inode *p_trace;
 *
 * TWO GUARDS, because one cannot cover this.  The _Static_asserts below fail
 * the clang build the moment this declaration drifts from the offsets it claims
 * -- but they can say nothing about whether v8cc agrees, since they only ever
 * see one compiler.  tests/kmemu measures the same numbers from the V8 side and
 * runs ps's own ioctl end to end, which is the half a static assertion is blind
 * to.  Both, and they fail for different reasons.
 *
 * ...and the four pid-shaped fields are int rather than upstream's short, which
 * is this port's one change to the header.  A macOS pid does not fit in 16 bits
 * and truncates NEGATIVE; src/include/PORTING.md has the measurement.  That is
 * why sizeof is 208 and not the 200 upstream's declaration gives.
 */
struct v8proc {
	char	*p_link, *p_rlink;
	char	*p_addr;
	char	 p_usrpri, p_pri, p_cpu, p_stat;
	char	 p_time, p_nice, p_slptime, p_cursig;
	long	 p_sig, p_siga0, p_siga1;
	int	 p_flag;
	int	 p_uid, p_pgrp, p_pid, p_ppid;
	short	 p_poip, p_szpt;
	long	 p_tsize, p_dsize, p_ssize, p_rssize, p_maxrss, p_swrss;
	long	 p_swaddr;
	char	*p_wchan;
	char	*p_textp;
	unsigned short p_clktim, p_tsleep;
	char	*p_p0br;
	char	*p_xlink;
	short	 p_cpticks;
	float	 p_pctcpu;
	short	 p_ndx, p_idhash;
	char	*p_pptr;
	char	*p_trace;
};

#define AT(f, n)	_Static_assert(__builtin_offsetof(struct v8proc, f) == (n), \
			    "struct proc: " #f " moved")
_Static_assert(sizeof(struct v8proc) == 208, "struct proc is 208 bytes on LP64");
AT(p_stat, 27);   AT(p_time, 28);    AT(p_nice, 29);    AT(p_flag, 56);
AT(p_uid, 60);    AT(p_pgrp, 64);    AT(p_pid, 68);     AT(p_ppid, 72);
AT(p_tsize, 80);  AT(p_dsize, 88);   AT(p_ssize, 96);   AT(p_rssize, 104);
AT(p_swaddr, 128); AT(p_wchan, 136); AT(p_textp, 144);  AT(p_clktim, 152);
AT(p_pctcpu, 180);
#undef AT

/* sys/h/pioctl.h.  Only PIOCGETPR is answered; see pr_ioctl. */
#define PIOC		('p'<<8)
#define PIOCGETPR	(PIOC|1)

/* sys/proc.h stat codes and the one flag bit that matters here. */
#define V8_SSLEEP	1
#define V8_SWAIT	2
#define V8_SRUN		3
#define V8_SIDL		4
#define V8_SZOMB	5
#define V8_SSTOP	6
#define V8_SLOAD	0x00000001

#define V8_NZERO	20	/* sys/param.h:40 -- V8's nice is 0..39, not -20..19 */
#define V8_NBPG		512	/* sys/param.h:65 -- a click, and ps prints clicks */

/*
 * THE STAT CODES DISAGREE ON EVERY VALUE BUT ONE, and both are small integers
 * in the same range, so a straight copy compiles, runs, stays in bounds and
 * prints the wrong letter for every process:
 *
 *	              macOS   V8
 *	SIDL            1      4       ps prints "?swRLZT?"[p_stat]
 *	SRUN            2      3       so a running process would read 'w',
 *	SSLEEP          3      1       a sleeping one 'R', a stopped one 'L'.
 *	SSTOP           4      6
 *	SZOMB           5      5       <- the only one that agrees
 *
 * That is the shape of failure this whole file is trying to avoid: output that
 * looks like output.  Hence a table rather than an assignment.
 */
static char
prstat_of(unsigned int mac)
{
	switch (mac) {
	case 1:  return (V8_SIDL);	/* SIDL */
	case 2:  return (V8_SRUN);	/* SRUN */
	case 3:  return (V8_SSLEEP);	/* SSLEEP */
	case 4:  return (V8_SSTOP);	/* SSTOP */
	case 5:  return (V8_SZOMB);	/* SZOMB */
	}
	return (V8_SWAIT);		/* V8's own abandoned state; prints 'w' */
}

/*
 * How many mach ticks make a second.
 *
 * pti_total_user AND pti_total_system ARE NOT NANOSECONDS.  <sys/proc_info.h>
 * says only "total time", and they are mach absolute-time units -- measured
 * here against CLOCK_PROCESS_CPUTIME_ID: a 0.3096 s burn reported 7560618,
 * which is 0.0076 s read as nanoseconds and 0.3150 s read as ticks.  On Intel
 * the timebase is 1/1 and the two coincide exactly, so this is a bug that an
 * x86 CI runner cannot see and an Apple Silicon one is wrong by 41.67x.  The
 * same reason .github/workflows/ci.yml pins macos-14.
 *
 * hw.tbfrequency is the tick rate and comes through sysctl, which is on PLAN.md
 * section 7's sanctioned list; mach_timebase_info() would be a second way to
 * ask and is not.  It is a quad in the kernel, but reading it into a zeroed
 * 64-bit variable is right whichever width it answers in, little-endian.
 */
static double
prtickhz(void)
{
	static double hz;
	unsigned long long f = 0;
	size_t n = sizeof f;

	if (hz == 0) {
		if (sysctlbyname("hw.tbfrequency", &f, &n, (void *)0, 0) < 0 ||
		    f == 0)
			f = 1000000000ULL;	/* nanoseconds; the Intel case */
		hz = (double)f;
	}
	return (hz);
}

static long
prnow(void)
{
	struct { long sec, usec; } tv;

	/* rawsys, not libc: gettimeofday is not a system fact this library was
	 * given an exception for, and rawsys.h already covers it.  Same rule
	 * that keeps synth.c writing files through raw syscalls. */
	if (rawsys2(SYS_gettimeofday, (long)&tv, 0) < 0) return (0);
	return (tv.sec);
}

/*
 * Fill one.  What is left zero is left zero deliberately; the notes say which
 * reader would have wanted it.
 */
static int
prgetpr(int pid, struct v8proc *p)
{
	struct proc_taskallinfo ai;
	char *q = (char *)p;
	long i, elapsed;
	double cpu;

	for (i = 0; i < (long)sizeof *p; i++) q[i] = 0;

	if (proc_pidinfo(pid, PROC_PIDTASKALLINFO, 0, &ai, sizeof ai) <
	    (int)sizeof ai)
		return (-1);

	p->p_pid  = (int)ai.pbsd.pbi_pid;
	p->p_ppid = (int)ai.pbsd.pbi_ppid;
	p->p_pgrp = (int)ai.pbsd.pbi_pgid;
	p->p_uid  = (int)ai.pbsd.pbi_uid;
	p->p_stat = prstat_of(ai.pbsd.pbi_status);

	/* V8's nice runs 0..39 around NZERO; macOS's runs -20..19 around 0.
	 * printp only asks `p_nice > NZERO', so the bias has to be applied or
	 * every renice'd process reads as ordinary. */
	p->p_nice = (char)(ai.pbsd.pbi_nice + V8_NZERO);

	/*
	 * SLOAD IS LOAD-BEARING, not decoration.  ps's getuarea (doselect.c)
	 * reads the u-area from /proc only `if (pp->p_flag & SLOAD)'; without
	 * it, it seeks into /dev/drum instead -- the swap device, which this
	 * world does not have.  Every process here is in core in the only sense
	 * available, so every one gets SLOAD.
	 */
	p->p_flag = V8_SLOAD;

	/*
	 * Sizes in clicks: printp computes (p_dsize+p_ssize)*NBPG/1024.
	 *
	 * macOS has one VM map, not the VAX's three segments, so there is no
	 * honest split to make.  Data carries the whole virtual size and stack
	 * is zero, which keeps the SUM -- the only thing ps prints -- true, and
	 * leaves the one field that would have to lie empty.
	 */
	p->p_dsize  = (long)(ai.ptinfo.pti_virtual_size / V8_NBPG);
	p->p_ssize  = 0;
	p->p_rssize = (long)(ai.ptinfo.pti_resident_size / V8_NBPG);
	p->p_tsize  = 0;

	/*
	 * %cpu, AND IT IS A DIFFERENT STATISTIC FROM V8'S.  p_pctcpu is a
	 * decaying average maintained by schedcpu over recent seconds; this is
	 * cpu time over lifetime, which is what BSD ps falls back to and what
	 * the host can actually answer.  A long-lived idle process reads near
	 * zero either way; a busy one that has been busy for hours reads lower
	 * here than V8 would have shown.  Recorded rather than smoothed over.
	 */
	cpu = (double)(ai.ptinfo.pti_total_user + ai.ptinfo.pti_total_system) /
	    prtickhz();
	elapsed = prnow() - (long)ai.pbsd.pbi_start_tvsec;
	if (elapsed < 1) elapsed = 1;
	p->p_pctcpu = (float)(cpu / (double)elapsed);
	if (p->p_pctcpu > 1.0f) p->p_pctcpu = 1.0f;	/* threads outrun wall time */

	/*
	 * Left zero, and each one is a reader that gets a defensible answer:
	 *
	 *   p_clktim  time to the alarm signal.  printp picks 'I' over 'S' with
	 *             "IS"[p_clktim && p_clktim < 20], so every sleeping
	 *             process reads 'I' -- idle.  macOS reports no per-process
	 *             alarm, and inventing one would invent the distinction.
	 *   p_wchan   the kernel address a process sleeps on; `ps -l' prints it
	 *             masked to 20 bits.  Not exposed by any documented
	 *             interface, so 0 -- which prints as 0.
	 *   p_textp   the text-structure pointer, followed by `ps -F' through
	 *             /dev/kmemr.  That path needs the kernel's inode table and
	 *             is out of scope; see src/cmd/ps/PORTING.md when it lands.
	 *   p_swaddr  where the u-area sits on the drum.  Unreachable while
	 *             SLOAD is set, which it always is.
	 */
	return (0);
}

static int
pr_ioctl(int fd, int cmd, char *arg)
{
	struct prfile *f = prfind(fd);

	if (f == 0) { v8_errno = V8_EBADF; return (-1); }

	/*
	 * V8 reaches ENOENT here the same way it does for /proc/00000: prioctl
	 * calls pfind(i_number - PRMAGIC) FIRST, for every command it knows
	 * (proca.c:312), and on the directory itself that is pfind(ROOTINO -
	 * PRMAGIC) = pfind(-62), which finds nothing.  An unknown command is
	 * EINVAL and is checked second, in that order (proca.c:296).
	 */
	switch (cmd) {
	case PIOCGETPR:
		break;
	default:
		v8_errno = V8_EINVAL;
		return (-1);
	}
	if (f->pid == 0 || prgetpr(f->pid, (struct v8proc *)arg) < 0) {
		v8_errno = V8_ENOENT;
		return (-1);
	}
	return (0);
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
	pr_stat, pr_fstat,
	pr_ioctl
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
