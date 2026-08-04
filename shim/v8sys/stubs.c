/*
 * The V8-named entry points.
 *
 * V8's libc calls open(), read(), write() and so on -- the names its assembly
 * stubs exported.  The implementations live in syscall.c under v8s_ names so
 * that this library can also be built against the host libc for testing
 * (tests/v8sys/test.c does exactly that) without every name colliding.
 *
 * This file is the thin layer that gives them their real names, and it is
 * compiled ONLY into the copy of the library that the V8 world links against
 * with -nostdlib.  Nothing here may call the host libc: at link time these
 * names ARE libc as far as the program is concerned.
 */

#include "v8sys.h"

extern int v8s_open(), v8s_creat(), v8s_close(), v8s_link(), v8s_unlink();
extern int v8s_chdir(), v8s_chmod(), v8s_chown(), v8s_fchmod(), v8s_fchown();
extern int v8s_access(), v8s_mkdir(), v8s_rmdir(), v8s_symlink(), v8s_dup();
extern int v8s_dup2(), v8s_pipe(), v8s_getpid(), v8s_getppid(), v8s_getuid();
extern int v8s_geteuid(), v8s_getgid(), v8s_getegid(), v8s_setuid();
extern int v8s_setgid(), v8s_umask(), v8s_sync(), v8s_fork(), v8s_vfork();
extern int v8s_pause(), v8s_execve(), v8s_nice(), v8s_kill(), v8s_stat();
extern int v8s_lstat(), v8s_fstat(), v8s_utime(), v8s_wait(), v8s_wait3();
extern int v8s_ioctl(), v8s_stime(), v8s_chroot(), v8s_nap(), v8s_syscall();
extern int v8s_mount(), v8s_umount(), v8s_gmount(), v8s_swapon(), v8s_reboot();
extern int v8s_acct(), v8s_settod(), v8s_vadvise(), v8s_vlimit(), v8s_vtimes();
extern int v8s_profil(), v8s_ptrace(), v8s_mpx(), v8s_brk();
extern int v8s_killpg(), v8s_setpgrp();
extern long v8s_read(), v8s_write(), v8s_lseek(), v8s_readlink();
extern long v8s_time(), v8s_times();
extern unsigned v8s_alarm();
extern char *v8s_sbrk();
extern void v8s_exit();

/*
 * errno.  V8 declares `extern int errno` and every program reads it directly,
 * so it has to be a real object with that name -- not a macro, and not
 * thread-local, both of which a modern libc would make it.
 */
int errno;

/* Keep v8_errno and errno the same storage as far as the world can tell. */
static void
sync_errno(void)
{
	errno = v8_errno;
}

#define WRAP0(name, impl, type) \
	type name() { type r = impl(); sync_errno(); return r; }
#define WRAP(name, impl, type) \
	type name(a,b,c,d,e,f) long a,b,c,d,e,f; \
	{ type r = impl(a,b,c,d,e,f); sync_errno(); return r; }

WRAP(open, v8s_open, int)
WRAP(creat, v8s_creat, int)
WRAP(close, v8s_close, int)
WRAP(read, v8s_read, long)
WRAP(write, v8s_write, long)
WRAP(lseek, v8s_lseek, long)
WRAP(link, v8s_link, int)
WRAP(unlink, v8s_unlink, int)
WRAP(chdir, v8s_chdir, int)
WRAP(chmod, v8s_chmod, int)
WRAP(chown, v8s_chown, int)
WRAP(fchmod, v8s_fchmod, int)
WRAP(fchown, v8s_fchown, int)
WRAP(access, v8s_access, int)
WRAP(mkdir, v8s_mkdir, int)
WRAP(rmdir, v8s_rmdir, int)
WRAP(symlink, v8s_symlink, int)
WRAP(readlink, v8s_readlink, long)
WRAP(dup, v8s_dup, int)
WRAP(dup2, v8s_dup2, int)
WRAP(pipe, v8s_pipe, int)
WRAP0(getpid, v8s_getpid, int)
WRAP0(getppid, v8s_getppid, int)
WRAP0(getuid, v8s_getuid, int)
WRAP0(geteuid, v8s_geteuid, int)
WRAP0(getgid, v8s_getgid, int)
WRAP0(getegid, v8s_getegid, int)
WRAP(setuid, v8s_setuid, int)
WRAP(setgid, v8s_setgid, int)
WRAP(umask, v8s_umask, int)
WRAP0(sync, v8s_sync, int)
WRAP0(fork, v8s_fork, int)
WRAP0(vfork, v8s_vfork, int)
WRAP0(pause, v8s_pause, int)
WRAP(execve, v8s_execve, int)
WRAP(nice, v8s_nice, int)
WRAP(kill, v8s_kill, int)
WRAP(stat, v8s_stat, int)
WRAP(lstat, v8s_lstat, int)
WRAP(fstat, v8s_fstat, int)
WRAP(utime, v8s_utime, int)
WRAP(wait, v8s_wait, int)
WRAP(wait3, v8s_wait3, int)
WRAP(ioctl, v8s_ioctl, int)
WRAP(alarm, v8s_alarm, unsigned)
WRAP(time, v8s_time, long)
WRAP(times, v8s_times, long)
WRAP(stime, v8s_stime, int)
WRAP(chroot, v8s_chroot, int)
WRAP(nap, v8s_nap, int)
WRAP(sbrk, v8s_sbrk, char *)
WRAP(brk, v8s_brk, int)
WRAP(mount, v8s_mount, int)
WRAP(umount, v8s_umount, int)
WRAP(gmount, v8s_gmount, int)
WRAP(swapon, v8s_swapon, int)
WRAP(reboot, v8s_reboot, int)
WRAP(acct, v8s_acct, int)
WRAP(settod, v8s_settod, int)
WRAP(vadvise, v8s_vadvise, int)
WRAP(vlimit, v8s_vlimit, int)
WRAP(vtimes, v8s_vtimes, int)
WRAP(profil, v8s_profil, int)
WRAP(ptrace, v8s_ptrace, int)
WRAP(mpx, v8s_mpx, int)
WRAP(syscall, v8s_syscall, int)
WRAP(killpg, v8s_killpg, int)
WRAP(setpgrp, v8s_setpgrp, int)

/*
 * _exit is the raw syscall; exit() is libc's, which flushes stdio through
 * _cleanup first.  V8 keeps them in separate files (sys/_exit.s and sys/exit.s)
 * for exactly that reason.
 */
void _exit(code) int code; { v8s_exit(code); }

/*
 * exit().  V8's real one lives in libc and calls _cleanup() to flush stdio
 * first (libc/sys/exit.s); this is the placeholder until that is ported, and
 * crt0 calls it after main returns.
 */
void exit(code) int code; { v8s_exit(code); }

/*
 * signal() takes a function pointer, so it does not fit the WRAP macro, whose
 * arguments are all long.
 */
extern char *v8s_signal();
char *signal(sig, h) int sig; char *h;
{
	char *r = v8s_signal(sig, h);
	sync_errno();
	return (r);
}
