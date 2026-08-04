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
int v8s_link(char *a, char *b)           { RET(rawsys2(SYS_link, (long)a, (long)b)); }
int v8s_unlink(char *p)                  { RET(rawsys1(SYS_unlink, (long)p)); }
int v8s_chdir(char *p)                   { RET(rawsys1(SYS_chdir, (long)p)); }
int v8s_chmod(char *p, int m)            { RET(rawsys2(SYS_chmod, (long)p, m)); }
int v8s_chown(char *p, int u, int g)     { RET(rawsys3(SYS_chown, (long)p, u, g)); }
int v8s_fchmod(int f, int m)             { RET(rawsys2(SYS_fchmod, f, m)); }
int v8s_fchown(int f, int u, int g)      { RET(rawsys3(SYS_fchown, f, u, g)); }
int v8s_access(char *p, int m)           { RET(rawsys2(SYS_access, (long)p, m)); }
int v8s_mkdir(char *p, int m)            { RET(rawsys2(SYS_mkdir, (long)p, m)); }
int v8s_rmdir(char *p)                   { RET(rawsys1(SYS_rmdir, (long)p)); }
int v8s_symlink(char *a, char *b)        { RET(rawsys2(SYS_symlink, (long)a, (long)b)); }
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
int v8s_execve(char *p, char **a, char **e)
{ RET(rawsys3(SYS_execve, (long)p, (long)a, (long)e)); }

long v8s_readlink(char *p, char *b, long n)
{ RET(rawsys3(SYS_readlink, (long)p, (long)b, n)); }

/*
 * fork.  V8's vfork is the BSD one, which shared the address space until exec;
 * plain fork is always a correct substitute and never a dangerous one, so both
 * names map here.
 */
int v8s_fork(void)                       { RET(rawsys0(SYS_fork)); }
int v8s_vfork(void)                      { RET(rawsys0(SYS_fork)); }

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

/* alarm and pause are setitimer/sigsuspend underneath on modern kernels. */
unsigned
v8s_alarm(unsigned sec)
{
	struct { long sec, usec; } it[2];

	it[0].sec = 0; it[0].usec = 0;		/* interval: one shot */
	it[1].sec = sec; it[1].usec = 0;	/* value */
	rawsys3(SYS_setitimer, 0 /*ITIMER_REAL*/, (long)it, 0);
	return (0);
}

int
v8s_pause(void)
{
	/* sigsuspend with an empty mask: wait for anything at all */
	long mask = 0;
	rawsys1(SYS_sigsuspend, (long)&mask);
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
 * open() is where directories are noticed.  Everything else about directory
 * reading follows from registering the shim here -- see dir.c.
 */
int
v8s_open(char *path, int flags, int mode)
{
	long fd;
	struct v8_stat st;

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

int
v8s_creat(char *path, int mode)
{
	RET(rawsys3(SYS_open, (long)path,
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

int v8s_stat(char *p, struct v8_stat *vs)
{
	struct hoststat64 hs;
	long r = rawsys2(SYS_stat64, (long)p, (long)&hs);
	if (r < 0) { v8_errno = v8sys_errno(RAWERR(r)); return (-1); }
	stat_translate(&hs, vs);
	return (0);
}

int v8s_lstat(char *p, struct v8_stat *vs)
{
	struct hoststat64 hs;
	long r = rawsys2(SYS_lstat64, (long)p, (long)&hs);
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
