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
 * WHAT THE HOST WILL TELL US IS DECIDED PER FIELD, NOT PER PROCESS, and getting
 * that wrong made ps print an error for a third of the system.
 *
 * The first attempt asked PROC_PIDTASKALLINFO for everything and returned
 * ENOENT when it failed.  Measured: it succeeds for 398 of 614 processes and
 * returns EPERM for the rest -- task information about a process you do not own
 * is privileged, and reasonably so.  So ps printed "/proc ioctl error" 215
 * times and listed two thirds of the system.  A process that EXISTS must not
 * answer ENOENT; that is the shim claiming it is gone.
 *
 * PROC_PIDT_SHORTBSDINFO is the one that is not gated: 614 of 614, and the
 * single miss was a process that exited between listing and asking (errno 0,
 * not EPERM).  It carries identity and state -- pid, ppid, pgid, uid, ruid,
 * status, comm -- which is most of what ps prints.  What it lacks is `nice' and
 * the start time, and those come from the full PROC_PIDTBSDINFO where it is
 * permitted; memory and cpu come from PROC_PIDTASKINFO on the same terms.
 *
 * So: three calls, one required and two optional, and a field is zero exactly
 * when the host declined to say.  That is the privilege boundary macOS actually
 * enforces, reported rather than papered over.
 *
 * The deprecated route was checked, and the conclusion has since been half
 * refuted.  sysctl KERN_PROC_ALL returns a struct kinfo_proc for all 618 in one
 * call, with nice and start time included -- deprecated in favour of exactly
 * the libproc calls above, and those looked sufficient.  They are, for identity
 * and state, everywhere.  For NICE they are not: on a GitHub macos-14 runner
 * pbi_nice does not track renice while sysctl does, measured twice.  So the ps
 * N column can be wrong on such a host; src/cmd/ps/PORTING.md records it, and
 * moving that one field to sysctl means carrying both interfaces.
 */
struct prinfo {
	struct proc_bsdshortinfo si;	/* always */
	struct proc_bsdinfo	 bi;	/* nice, start time -- if permitted */
	struct proc_taskinfo	 ti;	/* memory, cpu -- if permitted */
	int			 havebi, haveti;
};

static int
prgather(int pid, struct prinfo *g)
{
	g->havebi = g->haveti = 0;
	if (proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &g->si,
	    sizeof g->si) < (int)sizeof g->si)
		return (-1);		/* really gone: ENOENT is the truth */
	g->havebi = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &g->bi,
	    sizeof g->bi) >= (int)sizeof g->bi;
	g->haveti = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &g->ti,
	    sizeof g->ti) >= (int)sizeof g->ti;
	return (0);
}

/*
 * Fill one.  What is left zero is left zero deliberately; the notes say which
 * reader would have wanted it.
 */
static int
prgetpr(int pid, struct v8proc *p)
{
	struct prinfo g;
	char *q = (char *)p;
	long i, elapsed;
	double cpu;

	for (i = 0; i < (long)sizeof *p; i++) q[i] = 0;

	if (prgather(pid, &g) < 0) return (-1);

	p->p_pid  = (int)g.si.pbsi_pid;
	p->p_ppid = (int)g.si.pbsi_ppid;
	p->p_pgrp = (int)g.si.pbsi_pgid;
	p->p_uid  = (int)g.si.pbsi_uid;
	p->p_stat = prstat_of(g.si.pbsi_status);

	/* V8's nice runs 0..39 around NZERO; macOS's runs -20..19 around 0.
	 * printp only asks `p_nice > NZERO', so the bias has to be applied or
	 * every renice'd process reads as ordinary.  Where the host will not
	 * say, NZERO is the answer that prints no marker -- which is the same
	 * thing an unrenice'd process prints, and is the honest default. */
	p->p_nice = (char)(g.havebi ? g.bi.pbi_nice + V8_NZERO : V8_NZERO);

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
	p->p_dsize  = g.haveti ? (long)(g.ti.pti_virtual_size / V8_NBPG) : 0;
	p->p_ssize  = 0;
	p->p_rssize = g.haveti ? (long)(g.ti.pti_resident_size / V8_NBPG) : 0;
	p->p_tsize  = 0;

	/*
	 * %cpu, AND IT IS A DIFFERENT STATISTIC FROM V8'S.  p_pctcpu is a
	 * decaying average maintained by schedcpu over recent seconds; this is
	 * cpu time over lifetime, which is what BSD ps falls back to and what
	 * the host can actually answer.  A long-lived idle process reads near
	 * zero either way; a busy one that has been busy for hours reads lower
	 * here than V8 would have shown.  Recorded rather than smoothed over.
	 */
	if (g.haveti && g.havebi) {
		cpu = (double)(g.ti.pti_total_user + g.ti.pti_total_system) /
		    prtickhz();
		elapsed = prnow() - (long)g.bi.pbi_start_tvsec;
		if (elapsed < 1) elapsed = 1;
		p->p_pctcpu = (float)(cpu / (double)elapsed);
		if (p->p_pctcpu > 1.0f) p->p_pctcpu = 1.0f;   /* threads outrun wall */
	}

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

/* ----------------------------------------------------------- the u-area */

/*
 * struct user, the OTHER half of the /proc ABI, and ps reads it by seeking to
 * a virtual address:
 *
 *	Sread(fd, UBASE, up)   ==   lseek(fd, UBASE, 0); read(fd, up, 4016)
 *
 * because /proc/<pid> as a byte stream IS the process's address space --
 * proca.c serves it through prusrio -- and the u-area sits at the top of it,
 * at 0x80000000 - UPAGES*NBPG = 0x7fffec00.  So this is not a second file
 * format; it is a region of the one file, and pr_read hands it out when the
 * offset lands inside it.
 *
 * DECLARED BY OFFSET, unlike struct proc, and the difference is deliberate.
 * struct proc is 208 bytes of fields this file could honestly spell.  struct
 * user is 4016 bytes containing the VAX process control block, four disk maps,
 * a label_t, u_signal[NSIG] and the kernel stack -- spelling all of that to
 * reach the twelve fields ps reads would be a page of declaration for nobody,
 * and every line of it a chance to get padding wrong.  So the pads are
 * explicit and the _Static_asserts check them: a pad of the wrong length moves
 * the next field and the build fails.  Same two-guard arrangement as above --
 * tests/kmemu measures the same offsets from the V8 side.
 */
#define U_SIZE		4016
#define UPAGES		10		/* sys/param.h:69 */
#define UBASE		(0x80000000L - (long)UPAGES * V8_NBPG)

struct v8user {
	char	 pad0[282];
	short	 u_uid;			/* 282 */
	char	 pad1[2];		/* u_gid */
	short	 u_ruid;		/* 286 */
	char	 pad2[8];		/* u_rgid, then alignment */
	char	*u_procp;		/* 296 */
	char	 pad3[40];
	char	*u_cdir;		/* 344 */
	char	*u_rdir;		/* 352 */
	char	 pad4[528];
	char	*u_ofile[16];		/* 888 -- NOFILE */
	char	 pad5[1434];
	unsigned short u_ttydev;	/* 2450 */
	unsigned short u_ttyino;	/* 2452 */
	char	 pad6[34];
	char	 u_comm[254];		/* 2488 -- DIRSIZ */
	char	 pad7[2];
	long	 u_start;		/* 2744 */
	char	 pad8[8];		/* u_acflag, u_fpflag, u_cmask */
	long	 u_tsize;		/* 2760 */
	long	 u_dsize;		/* 2768 */
	long	 u_ssize;		/* 2776 */
	int	 vm_utime;		/* 2784 -- u_vm.vm_utime, 60ths */
	int	 vm_stime;		/* 2788 */
	char	 pad9[1224];
};

#define AT(f, n)	_Static_assert(__builtin_offsetof(struct v8user, f) == (n), \
			    "struct user: " #f " moved")
_Static_assert(sizeof(struct v8user) == U_SIZE, "struct user is 4016 bytes");
AT(u_uid, 282);   AT(u_ruid, 286);   AT(u_procp, 296);  AT(u_cdir, 344);
AT(u_rdir, 352);  AT(u_ofile, 888);  AT(u_ttydev, 2450); AT(u_ttyino, 2452);
AT(u_comm, 2488); AT(u_start, 2744); AT(u_tsize, 2760); AT(u_dsize, 2768);
AT(u_ssize, 2776); AT(vm_utime, 2784); AT(vm_stime, 2788);
#undef AT

/*
 * V8's HZ is 60 and u_vm is in sixtieths (sys/vtimes.h says so in the comment
 * on the field).  printp divides the sum by 60 to get seconds.
 */
#define V8_HZ	60

/*
 * ps looks at exactly twelve fields.  What is filled, and what is not:
 *
 *   u_uid, u_ruid   doselect's -r test.  Left 16 bits, unlike the proc
 *                   struct's p_uid: there the wider field was free (alignment
 *                   padding paid for it), here it would shift 4016 bytes of
 *                   layout, and the highest uid measured on this host is 501.
 *                   A uid that did not fit would truncate to a value with no
 *                   passwd entry, so getuname prints "?" -- a visible gap
 *                   rather than a wrong name.  Recorded as measured-safe.
 *   u_procp         ps sets it to 0 before the read and then treats non-zero
 *                   as "u-area already loaded" (doselect.c:19, getuarea:2).
 *                   Nothing dereferences it.  SYSADR, so that it is non-zero
 *                   AND shaped like the kernel pointer it claims to be -- and
 *                   so that Kread, which demands that bit, would fail on the
 *                   read rather than believe a small integer.
 *   u_comm          the command name, and the one that matters: it is what
 *                   getargs falls back to printing when it cannot read the
 *                   stack, which here is always.
 *   u_ssize         see below -- a behavioural choice, not a measurement.
 *   u_start         -T prints ctime of it.
 *   u_vm.vm_*       the TIME column.
 *   u_tsize/u_dsize the file's own st_size, per proca.c:88.
 *
 *   u_ttyino        LEFT ZERO, and it is a /dev question rather than a /proc
 *                   one.  gettty() looks the number up in the directory
 *                   records of /dev, /dev/dk and /dev/pt; inside the jail /dev
 *                   holds one entry, `kmem', so the lookup fails and ps prints
 *                   "?" whatever this field says.  Filling it correctly is a
 *                   stat of /dev/ttys<minor> folded through v8sys_fold_ino --
 *                   e_tdev's minor does map to the name, measured -- and it
 *                   buys exactly nothing until the jail's /dev carries tty
 *                   nodes.  So: a decision, not a discovery.
 *   u_cdir, u_rdir  followed by ps -F through /dev/kmemr, which needs the
 *   u_ofile         kernel's inode table.  Out of scope; see PLAN.md.
 */
static void
prgetuarea(struct prinfo *g, struct v8user *u)
{
	char *q = (char *)u;
	long i;
	double cpu;

	for (i = 0; i < (long)sizeof *u; i++) q[i] = 0;

	u->u_uid  = (short)g->si.pbsi_uid;
	u->u_ruid = (short)g->si.pbsi_ruid;
	u->u_procp = (char *)0x80000000L;	/* SYSADR; see above */
	u->u_start = g->havebi ? (long)g->bi.pbi_start_tvsec : 0;

	/* From the SHORT bsdinfo, so a process this user does not own still has
	 * a name -- which is the field getargs falls back to printing. */
	for (i = 0; i < (long)sizeof g->si.pbsi_comm && i < 253; i++)
		u->u_comm[i] = g->si.pbsi_comm[i];

	u->u_tsize = 0;
	u->u_dsize = g->haveti ? (long)(g->ti.pti_virtual_size / V8_NBPG) : 0;

	/*
	 * u_ssize IS A BEHAVIOURAL CHOICE AND NOT A MEASUREMENT, which is why
	 * it is spelled as one.  getargs reads the process's stack image to
	 * recover argv, and this port has no stack image to give it -- so the
	 * read must FAIL, and getargs then takes its own documented fallback
	 * and prints "(u_comm)", exactly as V8 does for a swapped-out process.
	 *
	 * Zero would not do that.  getargs computes nstack = ctob(u_ssize), and
	 * with zero it reads zero bytes, which SUCCEEDS -- and then scans
	 * backwards from stack+0, reading stack[-1] before its own guard can
	 * stop it.  So this is NSTACK's worth: the largest window getargs will
	 * look at, which makes it attempt one read, get a short count, and take
	 * the fallback deterministically.
	 */
	u->u_ssize = 8192L / V8_NBPG;

	if (g->haveti) {
		cpu = (double)(g->ti.pti_total_user) / prtickhz();
		u->vm_utime = (int)(cpu * V8_HZ);
		cpu = (double)(g->ti.pti_total_system) / prtickhz();
		u->vm_stime = (int)(cpu * V8_HZ);
	}
}

/*
 * The size of /proc/<pid>, which proca.c defines rather than leaves to us:
 *
 *	ip->i_size = (int)ptob(p->p_tsize+p->p_dsize+p->p_ssize+UPAGES)
 *	                                                    (proca.c:88, :189)
 *
 * -- the process's whole image plus the u-area, in bytes.  Worth taking from
 * upstream rather than inventing, because it is the only statement anywhere
 * about what this file's extent means.
 */
static long
prfilesize(int pid)
{
	struct prinfo g;

	if (prgather(pid, &g) < 0) return (0);
	/* The u-area is always there; the image size is only knowable where the
	 * host permits it, so an unprivileged reader sees the u-area alone. */
	return ((long)((g.haveti ? g.ti.pti_virtual_size / V8_NBPG : 0) +
	    UPAGES) * V8_NBPG);
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
		 * indexed by virtual address.  One region of it is answerable
		 * here -- the u-area at UBASE -- and everything else reads as
		 * end of file rather than as zeroes that could be believed.
		 *
		 * That EOF is load-bearing, not a gap: getargs seeks below
		 * UBASE for the stack image, and a short read is precisely
		 * what sends it to its own fallback.  See prgetuarea.
		 */
		struct prinfo g;
		struct v8user u;
		long within, take;

		if (f->off < UBASE || f->off >= UBASE + U_SIZE) return (0);
		if (prgather(f->pid, &g) < 0) {
			v8_errno = V8_ENOENT;
			return (-1);
		}
		prgetuarea(&g, &u);
		within = f->off - UBASE;
		take = U_SIZE - within;
		if (take > n) take = n;
		for (done = 0; done < take; done++)
			b[done] = ((char *)&u)[within + done];
		f->off += done;
		return (done);
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
	case 2: base = f->pid ? prfilesize(f->pid) : prdirsize(); break;
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
		st->st_size = (v8_off_t)prfilesize(pid);
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
