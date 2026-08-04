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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "v8sys.h"

int v8_errno;

extern void v8sys_dirinit(void);
extern long v8sys_dirseek(int, long, int);
extern v8_ino_t v8sys_fold_ino(unsigned long long);

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

int
v8sys_fail(void)
{
	v8_errno = v8sys_errno(errno);
	return (-1);
}

/* ------------------------------------------------------- PASSTHROUGH */

int v8s_close(int fd)
{
	if (v8sys_isdirfd(fd)) v8sys_dirclose(fd);
	return (close(fd) < 0 ? v8sys_fail() : 0);
}

long v8s_write(int fd, char *b, long n)
{ long r = write(fd, b, (size_t)n); return (r < 0 ? v8sys_fail() : r); }

int v8s_link(char *a, char *b)   { return (link(a,b) < 0 ? v8sys_fail() : 0); }
int v8s_unlink(char *p)          { return (unlink(p) < 0 ? v8sys_fail() : 0); }
int v8s_chdir(char *p)           { return (chdir(p) < 0 ? v8sys_fail() : 0); }
int v8s_chmod(char *p, int m)    { return (chmod(p,m) < 0 ? v8sys_fail() : 0); }
int v8s_chown(char *p,int u,int g){ return (chown(p,u,g) < 0 ? v8sys_fail() : 0); }
int v8s_fchmod(int f, int m)     { return (fchmod(f,m) < 0 ? v8sys_fail() : 0); }
int v8s_fchown(int f,int u,int g){ return (fchown(f,u,g) < 0 ? v8sys_fail() : 0); }
int v8s_access(char *p, int m)   { return (access(p,m) < 0 ? v8sys_fail() : 0); }
int v8s_mkdir(char *p, int m)    { return (mkdir(p,m) < 0 ? v8sys_fail() : 0); }
int v8s_rmdir(char *p)           { return (rmdir(p) < 0 ? v8sys_fail() : 0); }
int v8s_symlink(char *a,char *b) { return (symlink(a,b) < 0 ? v8sys_fail() : 0); }
int v8s_dup(int f)               { int r = dup(f); return (r < 0 ? v8sys_fail() : r); }
int v8s_getpid(void)             { return ((int)getpid()); }
int v8s_getppid(void)            { return ((int)getppid()); }
int v8s_getuid(void)             { return ((int)getuid()); }
int v8s_geteuid(void)            { return ((int)geteuid()); }
int v8s_getgid(void)             { return ((int)getgid()); }
int v8s_getegid(void)            { return ((int)getegid()); }
int v8s_setuid(int u)            { return (setuid(u) < 0 ? v8sys_fail() : 0); }
int v8s_setgid(int g)            { return (setgid(g) < 0 ? v8sys_fail() : 0); }
int v8s_umask(int m)             { return ((int)umask(m)); }
int v8s_sync(void)               { sync(); return (0); }
int v8s_fork(void)               { int r = fork(); return (r < 0 ? v8sys_fail() : r); }
int v8s_vfork(void)              { int r = fork(); return (r < 0 ? v8sys_fail() : r); }
unsigned v8s_alarm(unsigned s)   { return (alarm(s)); }
int v8s_pause(void)              { pause(); return (v8sys_fail()); }

long v8s_readlink(char *p, char *b, long n)
{ long r = readlink(p,b,(size_t)n); return (r < 0 ? v8sys_fail() : r); }

int v8s_execve(char *p, char **a, char **e)
{ execve(p,a,e); return (v8sys_fail()); }

int v8s_pipe(int fd[2])          { return (pipe(fd) < 0 ? v8sys_fail() : 0); }
int v8s_dup2(int a, int b)       { return (dup2(a,b) < 0 ? v8sys_fail() : b); }
int v8s_nice(int n)              { errno = 0; if (nice(n) == -1 && errno) return v8sys_fail(); return 0; }

/* V8's exit(2).  The libc-level exit() above this flushes stdio first. */
void v8s_exit(int c)             { _exit(c); }

/* ---------------------------------------------------------- TRANSLATE */

/*
 * open() is where directories are noticed.  Everything else about directory
 * reading follows from registering the shim here -- see dir.c.
 */
int
v8s_open(char *path, int flags, int mode)
{
	int fd;
	struct stat st;

	v8sys_dirinit();
	/*
	 * V8's flags are the V7 originals: 0 read, 1 write, 2 read/write, and
	 * O_CREAT/O_TRUNC/O_APPEND came later with the same values macOS uses.
	 * The low two bits therefore need no translation.
	 */
	if ((fd = open(path, flags, mode)) < 0) return (v8sys_fail());
	if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
		if (v8sys_diropen(path, fd) < 0) {
			int e = errno;
			close(fd);
			errno = e;
			return (v8sys_fail());
		}
	}
	return (fd);
}

int
v8s_creat(char *path, int mode)
{
	int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, mode);
	return (fd < 0 ? v8sys_fail() : fd);
}

long
v8s_read(int fd, char *buf, long n)
{
	long r;

	if (v8sys_isdirfd(fd)) return (v8sys_dirread(fd, buf, n));
	r = read(fd, buf, (size_t)n);
	return (r < 0 ? v8sys_fail() : r);
}

long
v8s_lseek(int fd, long off, int whence)
{
	off_t r;

	if (v8sys_isdirfd(fd)) return (v8sys_dirseek(fd, off, whence));
	r = lseek(fd, (off_t)off, whence);
	return (r < 0 ? v8sys_fail() : (long)r);
}

/*
 * struct stat.  The interesting field is st_ino: V8's is 16 bits, and no modern
 * filesystem's is.  Folding loses uniqueness, so programs comparing inode
 * numbers for identity (find(1) looking for hard links) can see false matches.
 * The alternative -- truncating -- would collide far more often and could
 * produce 0, which V7 uses to mean "no entry".
 */
static void
stat_translate(struct stat *hs, struct v8_stat *vs)
{
	memset(vs, 0, sizeof *vs);
	vs->st_dev   = (v8_dev_t)(hs->st_dev & 0xffff);
	vs->st_ino   = v8sys_fold_ino((unsigned long long)hs->st_ino);
	vs->st_mode  = (unsigned short)hs->st_mode;
	vs->st_nlink = (short)hs->st_nlink;
	vs->st_uid   = (short)hs->st_uid;
	vs->st_gid   = (short)hs->st_gid;
	vs->st_rdev  = (v8_dev_t)(hs->st_rdev & 0xffff);
	vs->st_size  = (v8_off_t)hs->st_size;
	/* the host's fields are st_atimespec.tv_sec once the macros are undone */
#ifdef __APPLE__
	vs->st_atime = (v8_time_t)hs->st_atimespec.tv_sec;
	vs->st_mtime = (v8_time_t)hs->st_mtimespec.tv_sec;
	vs->st_ctime = (v8_time_t)hs->st_ctimespec.tv_sec;
#else
	vs->st_atime = (v8_time_t)hs->st_atim.tv_sec;
	vs->st_mtime = (v8_time_t)hs->st_mtim.tv_sec;
	vs->st_ctime = (v8_time_t)hs->st_ctim.tv_sec;
#endif
}

int v8s_stat(char *p, struct v8_stat *vs)
{ struct stat s; if (stat(p,&s) < 0) return v8sys_fail(); stat_translate(&s,vs); return 0; }

int v8s_lstat(char *p, struct v8_stat *vs)
{ struct stat s; if (lstat(p,&s) < 0) return v8sys_fail(); stat_translate(&s,vs); return 0; }

int v8s_fstat(int f, struct v8_stat *vs)
{ struct stat s; if (fstat(f,&s) < 0) return v8sys_fail(); stat_translate(&s,vs); return 0; }

long v8s_time(long *tp)
{ time_t t = time(0); if (tp) *tp = (long)t; return ((long)t); }

int v8s_stime(long *tp) { v8_errno = V8_EPERM; return (-1); }

/* V8's times(2) reports in 60ths of a second, the VAX clock tick. */
struct v8_tbuffer { long proc_user, proc_system, child_user, child_system; };

long
v8s_times(struct v8_tbuffer *tb)
{
	struct tms t;
	clock_t r;
	long hz = sysconf(_SC_CLK_TCK);

	if ((r = times(&t)) == (clock_t)-1) return (v8sys_fail());
	if (hz <= 0) hz = 100;
	tb->proc_user     = (long)t.tms_utime  * 60 / hz;
	tb->proc_system   = (long)t.tms_stime  * 60 / hz;
	tb->child_user    = (long)t.tms_cutime * 60 / hz;
	tb->child_system  = (long)t.tms_cstime * 60 / hz;
	return ((long)r * 60 / hz);
}

int v8s_wait(int *status)
{ int r = wait(status); return (r < 0 ? v8sys_fail() : r); }

int v8s_wait3(int *status, int options, void *rusage)
{ int r = wait3(status, options, 0); return (r < 0 ? v8sys_fail() : r); }

int
v8s_utime(char *p, long *tv)
{
	struct timeval t[2];

	if (tv == 0) { if (utimes(p, 0) < 0) return v8sys_fail(); return 0; }
	t[0].tv_sec = tv[0]; t[0].tv_usec = 0;
	t[1].tv_sec = tv[1]; t[1].tv_usec = 0;
	return (utimes(p, t) < 0 ? v8sys_fail() : 0);
}

int
v8s_kill(int pid, int sig)
{
	int h = v8sys_signo_to_host(sig);
	if (h < 0) { v8_errno = V8_EINVAL; return (-1); }
	return (kill(pid, h) < 0 ? v8sys_fail() : 0);
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

int v8s_chroot(char *p) { return (chroot(p) < 0 ? v8sys_fail() : 0); }

/* V8's nap(2): sleep for a number of milliseconds. */
int
v8s_nap(long ms)
{
	struct timespec ts;

	ts.tv_sec  = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	nanosleep(&ts, 0);
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
