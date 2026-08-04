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

/*
 * EVERY WRAPPER RETURNS long, INCLUDING THE ONES THE MANUAL CALLS int.
 *
 * AAPCS64 says a function returning int need only set w0; the top half of x0 is
 * unspecified, and clang leaves whatever was there.  V8's back end computes at
 * 64-bit width and compares with an x-form `cmp`, so -1 arriving as
 * 0x00000000ffffffff tested as POSITIVE -- and V8 code checks every syscall
 * with `< 0`.  `cat nosuchfile` reported "input nosuchfile is output" because
 * open() failed and the failure did not register.
 *
 * The obvious fix is to sign-extend at the call site, and that was tried.  It
 * breaks worse.  V8 calls malloc without declaring it -- opendir.c has
 * `dirp = (DIR *)malloc(sizeof(DIR));` and nothing declares malloc anywhere in
 * scope -- so K&R types the call `int`, and sign-extending the result from 32
 * bits truncates the pointer.  On the VAX int and pointer were both 32 bits and
 * the omission cost nothing; under LP64 it is fatal.  The compiler cannot tell
 * a declared `int` from an undeclared one: they are the same node.
 *
 * So the seam adapts, which is the shim's job.  Returning long makes clang set
 * all 64 bits, V8 sees a properly extended value, and the compiler narrows only
 * types that must have been declared (char, short, unsigned).  gencall() in
 * compiler/ccom-arm64/gencode.c carries the other half of this note; the two
 * decisions only work together.
 *
 * Anything else in the shim that V8 code calls by name -- isatty() in ioctl.c
 * is the current example -- has to follow the same rule.
 */
#define WRAP0(name, impl, type) \
	type name() { type r = impl(); sync_errno(); return r; }
#define WRAP(name, impl, type) \
	type name(a,b,c,d,e,f) long a,b,c,d,e,f; \
	{ type r = impl(a,b,c,d,e,f); sync_errno(); return r; }

WRAP(open, v8s_open, long)
WRAP(creat, v8s_creat, long)
WRAP(close, v8s_close, long)
WRAP(read, v8s_read, long)
WRAP(write, v8s_write, long)
WRAP(lseek, v8s_lseek, long)
WRAP(link, v8s_link, long)
WRAP(unlink, v8s_unlink, long)
WRAP(chdir, v8s_chdir, long)
WRAP(chmod, v8s_chmod, long)
WRAP(chown, v8s_chown, long)
WRAP(fchmod, v8s_fchmod, long)
WRAP(fchown, v8s_fchown, long)
WRAP(access, v8s_access, long)
WRAP(mkdir, v8s_mkdir, long)
WRAP(rmdir, v8s_rmdir, long)
WRAP(symlink, v8s_symlink, long)
WRAP(readlink, v8s_readlink, long)
WRAP(dup, v8s_dup, long)
WRAP(dup2, v8s_dup2, long)
WRAP(pipe, v8s_pipe, long)
WRAP0(getpid, v8s_getpid, long)
WRAP0(getppid, v8s_getppid, long)
WRAP0(getuid, v8s_getuid, long)
WRAP0(geteuid, v8s_geteuid, long)
WRAP0(getgid, v8s_getgid, long)
WRAP0(getegid, v8s_getegid, long)
WRAP(setuid, v8s_setuid, long)
WRAP(setgid, v8s_setgid, long)
WRAP(umask, v8s_umask, long)
WRAP0(sync, v8s_sync, long)
WRAP0(fork, v8s_fork, long)
WRAP0(vfork, v8s_vfork, long)
WRAP0(pause, v8s_pause, long)
WRAP(execve, v8s_execve, long)
WRAP(nice, v8s_nice, long)
WRAP(kill, v8s_kill, long)
WRAP(stat, v8s_stat, long)
WRAP(lstat, v8s_lstat, long)
WRAP(fstat, v8s_fstat, long)
WRAP(utime, v8s_utime, long)
WRAP(wait, v8s_wait, long)
WRAP(wait3, v8s_wait3, long)
WRAP(ioctl, v8s_ioctl, long)
WRAP(alarm, v8s_alarm, long)
WRAP(time, v8s_time, long)
WRAP(times, v8s_times, long)
WRAP(stime, v8s_stime, long)
WRAP(chroot, v8s_chroot, long)
WRAP(nap, v8s_nap, long)
WRAP(sbrk, v8s_sbrk, char *)
WRAP(brk, v8s_brk, long)
WRAP(mount, v8s_mount, long)
WRAP(umount, v8s_umount, long)
WRAP(gmount, v8s_gmount, long)
WRAP(swapon, v8s_swapon, long)
WRAP(reboot, v8s_reboot, long)
WRAP(acct, v8s_acct, long)
WRAP(settod, v8s_settod, long)
WRAP(vadvise, v8s_vadvise, long)
WRAP(vlimit, v8s_vlimit, long)
WRAP(vtimes, v8s_vtimes, long)
WRAP(profil, v8s_profil, long)
WRAP(ptrace, v8s_ptrace, long)
WRAP(mpx, v8s_mpx, long)
WRAP(syscall, v8s_syscall, long)
WRAP(killpg, v8s_killpg, long)
WRAP(setpgrp, v8s_setpgrp, long)

/*
 * _exit is the raw syscall; exit() is libc's, which flushes stdio through
 * _cleanup first.  V8 keeps them in separate files (sys/_exit.s and sys/exit.s)
 * for exactly that reason.
 */
void _exit(code) int code; { v8s_exit(code); }

/*
 * exit() -- what V8's libc/sys/exit.s does: flush stdio through _cleanup(),
 * then leave through the kernel.
 *
 * Skipping the flush is invisible in a program that writes with write(2) and
 * fatal in one that uses stdio, which is most of them.  `tee` and `yes` worked
 * while `echo`, `wc`, `head`, `rev`, `tr`, `basename` and `sum` all printed
 * nothing -- the buffered output was simply discarded at exit.
 *
 * _cleanup lives in libc's flsbuf.c and walks _iob flushing every open stream.
 * It is weakly referenced so this file still links into a program built
 * without V8 libc (the freestanding tests do exactly that).
 */
extern void _cleanup() __attribute__((weak));

void
exit(code)
	int code;
{
	if (_cleanup) _cleanup();
	v8s_exit(code);
}

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
