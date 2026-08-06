/*
 * Behaviour tests for libv8sys, written in modern C and linked against the
 * shim directly.  These do not go through v8cc: the point is to check that the
 * seam itself behaves the way V8 expects, before any V8 code depends on it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "../../shim/v8sys/v8sys.h"
#include "../../shim/v8sys/vfs.h"

typedef void (*v8handler)();

extern int v8s_open(char *, int, int);
extern long v8s_read(int, char *, long);
extern long v8s_write(int, char *, long);
extern int v8s_close(int);
extern long v8s_lseek(int, long, int);
extern int v8s_stat(char *, struct v8_stat *);
extern int v8s_creat(char *, int);
extern int v8s_unlink(char *);
extern int v8s_mkdir(char *, int);
extern int v8s_rmdir(char *);
extern char *v8s_sbrk(long);
extern int v8s_pipe(int *);
extern int v8s_kill(int, int);
extern int v8s_pause(void);
extern unsigned v8s_alarm(unsigned);
extern v8handler v8s_signal(int, v8handler);
extern int v8sys_signo_to_host(int);
extern int v8sys_signo_from_host(int);
extern int v8sys_errno(int);

/*
 * V8's numbers, spelled out rather than taken from the host's <signal.h>.
 * They agree today -- V8's numbering is BSD's for every signal V8 names -- and
 * a test that reads them from the host could not tell if that stopped being
 * true, which is the one thing the translation table exists for.
 */
#define V8SIG_INT	2
#define V8SIG_ALRM	14

static int pass, fail;

static void
ok(int cond, const char *what)
{
	if (cond) pass++;
	else { fail++; printf("FAIL %s\n", what); }
}

/*
 * ---------------------------------------------------------------------------
 * SIGNAL DELIVERY, EACH CASE IN A FORKED CHILD WITH A DEADLINE.
 *
 * Not caution.  The fault these cases exist for does not produce a wrong
 * answer: a handler installed with a null sa_tramp is entered at pc 0, so the
 * process hangs or dies.  Run inline, the first of them would take the whole
 * suite down with it -- printing nothing, since the counters die too -- or
 * wedge `make test' forever.  In a child it is a failed test with a reason.
 *
 * It is also what makes the mutation check bearable, and these are the
 * mutations each case is for.  Zero sa_tramp in v8s_signal: every one of the
 * five must fail, reporting `hung' or `died' -- which of the two depends on
 * what the kernel finds at pc 0 and neither is a wrong answer.  Drop
 * SA_NODEFER: only the longjmp case, and only at round 2.  Pass &mask to
 * sigsuspend again: only the pause case, and only on a machine whose stack
 * address has the SIGALRM bit set, which is the point of it.  Pass 0 for
 * setitimer's third argument again: only the alarm cases, which are inline.
 * Each has to be watched to fail before it is worth anything.
 *
 * Every body() must leave through _exit, and its code says where it stopped.
 * ---------------------------------------------------------------------------
 */
#define CH_TIMEOUT	(-1)
#define CH_SIGNAL	(-2)
#define CH_NOFORK	(-3)

static int
runchild(void (*body)(void), int limit_ms)
{
	pid_t pid;
	int status, waited;

	fflush(stdout);			/* else the child inherits our buffer */
	if ((pid = fork()) < 0) return (CH_NOFORK);
	if (pid == 0) { body(); _exit(99); }

	for (waited = 0; waited < limit_ms; waited += 10) {
		if (waitpid(pid, &status, WNOHANG) == pid)
			return (WIFSIGNALED(status) ? CH_SIGNAL
						    : WEXITSTATUS(status));
		usleep(10000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	return (CH_TIMEOUT);
}

static void
okchild(void (*body)(void), int limit_ms, const char *what)
{
	int rc = runchild(body, limit_ms);

	if (rc == 0) { pass++; return; }
	fail++;
	printf("FAIL %s\n", what);
	if (rc == CH_TIMEOUT)
		printf("     the child hung -- the null-trampoline symptom\n");
	else if (rc == CH_SIGNAL)
		printf("     the child died on a signal\n");
	else if (rc == CH_NOFORK)
		printf("     fork failed\n");
	else
		printf("     the child stopped at _exit(%d)\n", rc);
}

static volatile sig_atomic_t hits, seen;

static void
onsig(int sig)
{
	hits++;
	seen = sig;
}

/*
 * The handler runs at all -- and control comes BACK, which is the half that
 * needs sigreturn.  kill(2) delivers before it returns, so by the line after
 * it the handler has run and the interrupted context has been restored; a
 * trampoline that called the handler and then failed to sigreturn would never
 * reach the test below.
 */
static void
c_delivery(void)
{
	hits = 0; seen = -1;
	if (v8s_signal(V8SIG_INT, onsig) == (v8handler)-1) _exit(2);
	if (v8s_kill(getpid(), V8SIG_INT) < 0) _exit(3);
	if (hits != 1) _exit(4);
	if (seen != V8SIG_INT) _exit(5);
	_exit(0);
}

/*
 * V7 reset-on-delivery: the handler is SIG_DFL again by the time it returns,
 * and V8 code is written to re-arm inside itself.  Read back through
 * v8s_signal's own return value, which reports what was installed.
 */
static void
c_reset(void)
{
	hits = 0;
	if (v8s_signal(V8SIG_INT, onsig) == (v8handler)-1) _exit(2);
	if (v8s_kill(getpid(), V8SIG_INT) < 0) _exit(3);
	if (hits != 1) _exit(4);
	if (v8s_signal(V8SIG_INT, (v8handler)SIG_IGN) != (v8handler)SIG_DFL)
		_exit(5);
	_exit(0);
}

/*
 * A slow read is INTERRUPTED, not restarted: no SA_RESTART.  V8 programs check
 * for EINTR and would wait forever if the kernel restarted the call for them.
 * The read is on an empty pipe with no writer, so nothing but the alarm can
 * end it.
 */
static void
c_eintr(void)
{
	int fd[2];
	char b;

	hits = 0;
	if (v8s_pipe(fd) < 0) _exit(2);
	if (v8s_signal(V8SIG_ALRM, onsig) == (v8handler)-1) _exit(3);
	if (v8s_alarm(1) != 0) _exit(4);
	if (v8s_read(fd[0], &b, 1) != -1) _exit(5);
	if (v8_errno != V8_EINTR) _exit(6);
	if (hits != 1) _exit(7);
	_exit(0);
}

/*
 * pause() WAITS, and wakes on delivery.  This is what V8's sleep(3) spends its
 * time in -- `for(;;) pause()' -- and it decides the shape of sigsuspend's
 * argument, which the kernel takes BY VALUE where POSIX's wrapper takes a
 * pointer.  Both ways of being wrong are caught here and they fail
 * differently: a pointer read as a mask blocks a pseudo-random set of signals,
 * so if it happens to include SIGALRM the child hangs; a mask read as a
 * pointer faults straight back out, so pause() returns with no signal
 * delivered and hits is 0.
 *
 * One pause() call, deliberately, not a loop: a loop cannot tell waiting from
 * spinning.
 */
static void
c_pause(void)
{
	time_t t0;

	hits = 0;
	if (v8s_signal(V8SIG_ALRM, onsig) == (v8handler)-1) _exit(2);
	if (v8s_alarm(1) != 0) _exit(3);
	t0 = time(0);
	if (v8s_pause() != -1) _exit(4);	/* pause always "fails" with EINTR */
	if (hits != 1) _exit(5);		/* came back without a signal */
	if (time(0) - t0 < 1) _exit(6);		/* ...or came back too early */
	_exit(0);
}

/*
 * THE SECOND SIGNAL, after a handler that longjmps out.
 *
 * This is the case that would catch a missing SA_NODEFER, and only the second
 * round can: sigaction blocks the signal for the duration of the handler and
 * sigreturn unblocks it, so a handler that jumps out leaves it blocked
 * forever.  Round one passes either way; round two never gets its signal.
 *
 * _setjmp/_longjmp, NOT setjmp/longjmp.  Darwin's setjmp saves the signal mask
 * and its longjmp restores it, which would paper over exactly this -- V8's
 * setjmp.s saves registers and nothing else, as the VAX original did, so
 * _setjmp is the contract V8 code actually has.
 */
static jmp_buf jb;

static void
onjmp(int sig)
{
	hits++;
	_longjmp(jb, 1);
}

static void
c_longjmp(void)
{
	/* volatile: a local live across a longjmp, and this one is read after */
	volatile int round;

	hits = 0;
	for (round = 1; round <= 3; round++) {
		if (v8s_signal(V8SIG_INT, onjmp) == (v8handler)-1) _exit(2);
		if (_setjmp(jb) == 0) {
			v8s_kill(getpid(), V8SIG_INT);
			_exit(round + 10);	/* signal never arrived */
		}
	}
	if (hits != 3) _exit(3);
	_exit(0);
}

int
main(void)
{
	char tmpl[] = "/tmp/v8systestXXXXXX";
	char path[512], sub[512];
	struct v8_stat st;
	struct v8_direct rec;
	int fd, n, found_a, found_b, found_long;
	char buf[4096];
	char *b0, *b1;

	if (mkdtemp(tmpl) == 0) { perror("mkdtemp"); return 1; }

	/* --------------------------------------------- files: the basics */
	snprintf(path, sizeof path, "%s/file", tmpl);
	fd = v8s_creat(path, 0644);
	ok(fd >= 0, "creat");
	ok(v8s_write(fd, "hello", 5) == 5, "write");
	ok(v8s_close(fd) == 0, "close");

	fd = v8s_open(path, 0, 0);
	ok(fd >= 0, "open for reading");
	ok(v8s_read(fd, buf, sizeof buf) == 5, "read returns what was written");
	ok(memcmp(buf, "hello", 5) == 0, "read returns the right bytes");
	ok(v8s_lseek(fd, 0, 0) == 0, "lseek to start");
	ok(v8s_read(fd, buf, 2) == 2, "short read after seek");
	v8s_close(fd);

	/* ------------------------------------------------ struct stat */
	ok(v8s_stat(path, &st) == 0, "stat");
	ok(st.st_size == 5, "st_size");
	ok((st.st_mode & V8_S_IFMT) == V8_S_IFREG, "st_mode says regular file");
	ok(st.st_ino != 0, "st_ino is never 0 (V7 uses 0 for an empty slot)");
	/*
	 * 48 bytes, not the VAX's 32.  st_size, st_atime, st_mtime and st_ctime
	 * are off_t and time_t, which are `long` -- 4 bytes on the VAX and 8
	 * under LP64.  V8 code compiled by v8cc sees the same widths through its
	 * own <sys/stat.h>, so the two agree; what would be wrong is this struct
	 * disagreeing with the one the V8 world builds, not with the one a VAX
	 * would have built.
	 */
	ok(sizeof(struct v8_stat) == 48,
	    "struct v8_stat matches the LP64 layout V8 code will see");

	/* ------------------------------- directories read as V7 records */
	/*
	 * The load-bearing test: 44 V8 commands read directories with read(2)
	 * and expect fixed-size records.  macOS refuses read() on a directory at
	 * all, so this is entirely synthesised by the shim.
	 *
	 * 256 bytes, not the V7 16: DIRSIZ is 254 here, because a 14-character
	 * name cannot hold most of a real macOS path component and pwd(1) failed
	 * in any directory with a long one above it.  See src/include/dir.h for
	 * the reasoning; what matters at this seam is only that the shim and the
	 * header agree, which is what this asserts.
	 */
	ok(sizeof(struct v8_direct) == 2 + V8_DIRSIZ,
	    "a v8_direct record is 2 + DIRSIZ bytes");
	ok(sizeof(struct v8_direct) == 256, "struct v8_direct is 256 bytes");

	snprintf(sub, sizeof sub, "%s/a", tmpl);
	close(creat(sub, 0644));
	snprintf(sub, sizeof sub, "%s/bb", tmpl);
	close(creat(sub, 0644));
	snprintf(sub, sizeof sub, "%s/a-very-long-name-indeed", tmpl);
	close(creat(sub, 0644));

	fd = v8s_open(tmpl, 0, 0);
	ok(fd >= 0, "open a directory");

	found_a = found_b = found_long = 0;
	while ((n = v8s_read(fd, (char *)&rec, sizeof rec)) == sizeof rec) {
		char nm[V8_DIRSIZ + 1];
		memcpy(nm, rec.d_name, V8_DIRSIZ);
		nm[V8_DIRSIZ] = '\0';
		if (strcmp(nm, "a") == 0) found_a = 1;
		if (strcmp(nm, "bb") == 0) found_b = 1;
		/*
		 * The whole name, not the 14 bytes a V7 disk would have held.
		 * That is the point of DIRSIZ being 254 here: truncation made
		 * most of a real filesystem unnameable.
		 */
		if (strcmp(nm, "a-very-long-name-indeed") == 0) found_long = 1;
		ok(rec.d_ino != 0, "directory entry has a non-zero inode");
	}
	ok(n == 0, "read on a directory ends cleanly at EOF");
	ok(found_a, "found entry 'a'");
	ok(found_b, "found entry 'bb'");
	ok(found_long, "a 23-character name survives whole");

	ok(v8s_lseek(fd, 0, 0) == 0, "rewind a directory with lseek");
	ok(v8s_read(fd, (char *)&rec, sizeof rec) == sizeof rec,
	    "read again after rewind");
	v8s_close(fd);

	/* stat of a directory */
	ok(v8s_stat(tmpl, &st) == 0, "stat a directory");
	ok((st.st_mode & V8_S_IFMT) == V8_S_IFDIR, "st_mode says directory");

	/* ------------------------------------------------------- errno */
	snprintf(path, sizeof path, "%s/nonexistent", tmpl);
	ok(v8s_open(path, 0, 0) == -1, "open of a missing file fails");
	ok(v8_errno == V8_ENOENT, "and sets V8's ENOENT (2)");

	ok(v8sys_errno(0) == 0, "errno 0 maps to 0");
	ok(v8sys_errno(9999) == V8_EIO,
	    "an error V8 has no name for becomes EIO, not garbage");

	/* ------------------------------------------------------ signals */
	ok(v8sys_signo_to_host(1) == 1, "SIGHUP maps through");
	ok(v8sys_signo_to_host(9) == 9, "SIGKILL maps through");
	ok(v8sys_signo_to_host(16) == -1,
	    "signal 16 is unused in V8 and is rejected");
	ok(v8sys_signo_from_host(15) == 15, "SIGTERM maps back");
	/* the flags V8 packs into the signal number must not confuse it */
	ok(v8sys_signo_to_host(2 | 0400) == 2, "SIGDOPAUSE flag is stripped");

	/* ---------------------------------------------- signal DELIVERY */
	/*
	 * Everything above is numbering, and numbering is all this suite
	 * checked for a long time -- which is how a shim where no handler
	 * could ever run passed 44 of 44.  The kernel takes a different struct
	 * from the one libc's sigaction() takes, with a signal-trampoline
	 * pointer where the userland one keeps sa_mask, so every handler was
	 * installed with a null trampoline and delivery jumped to address 0.
	 * Nothing at the seam could see it: sigaction returned 0.
	 */
	okchild(c_delivery, 5000,
	    "a handler installed through v8s_signal runs, and control returns");
	okchild(c_reset, 5000,
	    "V7 reset-on-delivery: SIG_DFL is what is installed afterwards");
	okchild(c_eintr, 5000,
	    "a slow read is interrupted with EINTR rather than restarted");
	okchild(c_pause, 5000,
	    "pause() blocks until a signal is delivered, then returns EINTR");
	okchild(c_longjmp, 5000,
	    "a second signal arrives after a handler that longjmped out");

	/* ------------------------------------------------------- alarm */
	/*
	 * alarm(2) owes the caller the time left on the previous alarm; this
	 * returned 0 unconditionally, having passed setitimer's old-value
	 * argument as 0.  V8's sleep(3) saves and restores its caller's alarm
	 * across the sleep with exactly this, so a constant 0 threw it away.
	 */
	ok(v8s_alarm(100) == 0, "alarm with nothing pending reports 0");
	{
		unsigned prev = v8s_alarm(0);
		ok(prev != 0, "alarm reports the previous alarm's remaining time");
		ok(prev == 100, "...rounding the part-second up, as alarm(2) does");
	}
	ok(v8s_alarm(0) == 0, "and once cancelled there is nothing left to report");

	/* --------------------------------------------------------- sbrk */
	b0 = v8s_sbrk(0);
	ok(b0 != (char *)-1, "sbrk(0) probes the break");
	b1 = v8s_sbrk(4096);
	ok(b1 == b0, "sbrk returns the OLD break, as malloc expects");
	ok(v8s_sbrk(0) == b0 + 4096, "the break moved");
	memset(b0, 0xa5, 4096);		/* must not fault: it is committed */
	ok(*(unsigned char *)b0 == 0xa5, "memory below the break is writable");
	ok(v8s_sbrk(-4096) == b0 + 4096, "the break can move back");

	/* ------------------------------------ the filesystem switch (S8a.2) */
	/*
	 * The mount table is the single source of truth for which filesystem
	 * answers for a path.  It used to be `v8dirs[]' in syscall.c, consulted
	 * only by rootpath(); generalising THAT list rather than adding a second
	 * beside it is what these cases pin -- two prefix tables that must agree
	 * by hand is the standing invitation kmem.c's one-table rule refuses.
	 */
	ok(v8fs_typefor("/bin/cat")   == &v8fs_pass, "a V8 directory is claimed by a mount");
	ok(v8fs_typefor("/etc/group") == &v8fs_pass, "...and so is /etc");
	ok(v8fs_typefor("/etc")       == &v8fs_pass, "...and the directory itself, with no slash");
	ok(v8fs_typefor("/usr/lib/tmac/x") == &v8fs_pass, "...and anything beneath one");

	/*
	 * The negative half, which is the half that matters.  A prefix table
	 * that claimed everything would pass every case above.
	 */
	ok(v8fs_typefor("/tmp/x")   == 0, "an unclaimed path has no mount");
	ok(v8fs_typefor("relative") == 0, "nor does a relative one");
	ok(v8fs_typefor("/binary")  == 0, "/binary is not /bin/, despite the prefix");
	ok(v8fs_typefor("/unix")    == &v8fs_pass, "/unix is claimed exactly");
	ok(v8fs_typefor("/unixfoo") == 0, "...and /unixfoo is not, which is why m_exact exists");

	/*
	 * Descriptor ownership defaults to passthrough for every descriptor,
	 * including ones out of the table's range, so nothing needs initialising
	 * and an inherited descriptor works by construction.
	 */
	ok(v8fs_fdtype(0) == &v8fs_pass,     "descriptor 0 is passthrough by default");
	ok(v8fs_fdtype(9999) == &v8fs_pass,  "...and so is one past the end of the table");
	ok(v8fs_fdtype(-1) == &v8fs_pass,    "...and a negative one, rather than faulting");

	/*
	 * THE EMPTY PATH IS THE CURRENT DIRECTORY, and this case exists because
	 * the switch broke it.  V7's namei() resolved "" to the working
	 * directory and rmdir(1) depends on it (`stat("", &cst)').  The rule was
	 * inside vpath(); the switch's t_path calls the resolver directly, so it
	 * was bypassed and rmdir silently stopped removing anything.  tests/waveb
	 * caught it, which is what "the suites stay green" is for -- but a rule
	 * this load-bearing should have a case of its own rather than a
	 * consequence three layers up.
	 */
	{
		struct v8_stat es, ds;
		ok(v8s_stat("", &es) == 0, "stat(\"\") succeeds, as V7 namei made it");
		ok(v8s_stat(".", &ds) == 0, "and so does stat(\".\")");
		ok(es.st_ino == ds.st_ino, "...naming the same directory");
	}

	/* ------------------------------------------------------- cleanup */
	snprintf(sub, sizeof sub, "%s/a", tmpl); v8s_unlink(sub);
	snprintf(sub, sizeof sub, "%s/bb", tmpl); v8s_unlink(sub);
	snprintf(sub, sizeof sub, "%s/a-very-long-name-indeed", tmpl); v8s_unlink(sub);
	snprintf(sub, sizeof sub, "%s/file", tmpl); v8s_unlink(sub);
	ok(v8s_rmdir(tmpl) == 0, "rmdir");

	printf("v8sys: %d passed, %d failed\n", pass, fail);
	return (fail != 0);
}
