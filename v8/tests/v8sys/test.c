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
#include <sys/socket.h>
#include <dirent.h>
#include "../../shim/v8sys/v8sys.h"
#include "../../shim/v8sys/vfs.h"
#include "../../shim/p9/p9.h"
#include "../../shim/v8id.h"

typedef void (*v8handler)();

extern int v8s_open(char *, int, int);
extern long v8s_read(int, char *, long);
extern long v8s_write(int, char *, long);
extern int v8s_close(int);
extern long v8s_lseek(int, long, int);
extern int v8s_stat(char *, struct v8_stat *);
extern int v8s_fstat(int, struct v8_stat *);
extern int v8s_dup(int);
extern int v8s_dup2(int, int);
extern int v8s_creat(char *, int);
extern int v8s_unlink(char *);
extern int v8s_link(char *, char *);
extern int v8s_mkdir(char *, int);
extern int v8s_rmdir(char *);
extern int v8s_symlink(char *, char *);
extern long v8s_readlink(char *, char *, long);
extern int v8s_utime(char *, long *);
extern char *v8s_sbrk(long);
extern int v8s_pipe(int *);
extern int v8s_kill(int, int);
extern int v8s_pause(void);
extern unsigned v8s_alarm(unsigned);
extern v8handler v8s_signal(int, v8handler);
extern char *v8s_sigsys(int, char *);	/* 4.1BSD call 48, libjobs' primitive */
extern int v8sys_signo_to_host(int);
extern int v8sys_signo_from_host(int);
extern int v8sys_errno(int);
extern v8_ino_t v8sys_fold_ino(unsigned long long);
/* rootpath is asked DIRECTLY below: the union rule's answer is the
 * behaviour under test, and a create cannot show it on a host that has
 * no writable jailed prefix.  V8P_LOOK/V8P_MAKE come from vfs.h. */
extern char *v8sys_rootpath(char *p, int mode);

/*
 * The map v8sys_fold_ino() used to be, written out here because it is the
 * SPECIFICATION of one of the properties being tested and not a copy of the
 * implementation: a host inode whose folded value nobody else wants must still
 * get exactly that value, so that `ls -i' prints what it printed before the
 * table existed and prints it in every process.
 */
static v8_ino_t
classicfold(unsigned long long ino)
{
	unsigned short v;

	v = (unsigned short)(ino ^ (ino >> 16) ^ (ino >> 32) ^ (ino >> 48));
	return (v ? v : (v8_ino_t)1);
}

/*
 * V8's numbers, spelled out rather than taken from the host's <signal.h>.
 * They agree today -- V8's numbering is BSD's for every signal V8 names -- and
 * a test that reads them from the host could not tell if that stopped being
 * true, which is the one thing the translation table exists for.
 */
#define V8SIG_INT	2
#define V8SIG_ALRM	14

/*
 * Reach a path inside the jail WITHOUT the shim, by putting $V8ROOT in front of
 * it by hand.
 *
 * hostrm() existed for the one thing the jail could not remove itself: a
 * dangling symlink was invisible to rootpath()'s access(2) existence test, so
 * v8s_unlink could not see it, and a case asserting that limit would leave the
 * link behind and break the NEXT run -- which is exactly what it did.  ("Would
 * this still pass on a tree that has never been used?" has a mirror: would it
 * still pass on a tree that has?)
 *
 * The predicate is an lstat now and the jail can remove its own links, so
 * hostrm is a belt-and-braces cleanup rather than the only way out.  hostexists
 * is the half that earns its keep: it is the only way to tell "the shim removed
 * the jail's link" from "the shim removed a link it made on the host at the
 * bare path", and before the fix the second is what would have happened.
 */
static int
hostpath(char *out, long n, const char *jailpath)
{
	const char *root = getenv("V8ROOT");

	if (root == 0 || *root == '\0') return (0);
	snprintf(out, (size_t)n, "%s%s", root, jailpath);
	return (1);
}

static void
hostrm(const char *jailpath)
{
	char h[1024];

	if (hostpath(h, (long)sizeof h, jailpath)) unlink(h);
}

/* lstat, not access -- this helper has to see exactly what the fix is about. */
static int
hostexists(const char *jailpath)
{
	char h[1024];
	struct stat sb;

	if (!hostpath(h, (long)sizeof h, jailpath)) return (0);
	return (lstat(h, &sb) == 0);
}

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
 * ...AND THE OTHER INTERFACE RESTARTS IT, which is the other half of the pair
 * above and is upstream's rule rather than a preference.  V8 decides in one
 * flag: ssig() (sys4.c:318-320) sets SNUSIG when the action is SIGISDEFER(f)
 * -- which is exactly what sigset() passes -- and read() then opens with
 * `if ((p_flag&SNUSIG) && setjmp(u_qsav)) if (u_count == uap->count)
 * u_eosys = RESTARTSYS' (sys2.c:55), which trap.c:184 turns into backing the
 * PC over the chmk.  So V7's signal(2) yields EINTR and the reliable interface
 * restarts, in the SAME kernel.
 *
 * THE HANDLER IS THE WRITER, which is what makes this deterministic and
 * unhangable: with the restart the read is interrupted, the handler puts a
 * byte in the pipe, and the restarted read returns it; without the restart the
 * read returns -1/EINTR and the case fails at once.  No fork, no second
 * process, no timing.
 *
 * It is worth a case rather than a comment because the defect it guards is a
 * silent WRONG ANSWER at a fraction of a percent: csh's backeval reads the
 * backquote pipe with `if (icnt <= 0) { c = -1; break; }', so an interrupted
 * read is indistinguishable from end-of-file and `set v = `echo x`' quietly
 * became the empty string.  Measured before the fix at 2 in 480 under eight-way
 * contention -- far too rare for a behavioural case, which is why this asserts
 * the PROPERTY instead.
 */
static int restartfd;

static void
onsig_write(int sig)
{
	char c = 'x';

	hits++;
	seen = sig;
	(void)v8s_write(restartfd, &c, 1);
}

static void
c_sigsys_restart(void)
{
	int fd[2];
	char b;

	hits = 0; seen = -1; b = 0;
	if (v8s_pipe(fd) < 0) _exit(2);
	restartfd = fd[1];
	/* DEFERSIG: the low bit is the defer flag, and it is what sets SNUSIG */
	if (v8s_sigsys(V8SIG_ALRM, (char *)((long)onsig_write | 1)) ==
	    (char *)-1) _exit(3);
	if (v8s_alarm(1) != 0) _exit(4);
	if (v8s_read(fd[0], &b, 1) != 1) _exit(5);	/* -1 == not restarted */
	if (hits != 1) _exit(6);
	if (b != 'x') _exit(7);
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

/*
 * 65535 numbers is all a u_short has, so a process that sees more distinct
 * inodes than that must run out.  What it must not do is hang in the probe
 * loop, revise an assignment it has already made, or start handing out 0.
 * Runs in a child because filling the table is not undoable, and under a
 * deadline because the failure this guards against is a hang rather than a
 * wrong answer.
 *
 * Feeding 1..65535 is deliberate: for a value under 65536 the old fold is the
 * identity, so *most* iterations claim their own number and go straight in.
 * Not all -- the parent has already taken a handful, and an iteration landing
 * on one of those probes, and can displace the next.  (An earlier comment here
 * claimed nothing probes at all, which was wrong for exactly that reason.)
 * The assertions do not depend on the count either way.
 *
 * The bitmap is the injectivity assertion at scale, and it is sharper than
 * counting: EVERY answer must be new until the space fills, and after that a
 * repeat is legal only if it is the plain fold.  A repeat that is anything
 * else means two host inodes were aliased onto one identity, which is the bug
 * this whole change exists to remove.
 *
 * `late' is what proves the space really filled, and it is sharp rather than
 * decorative: 0x4000000000000002 folds to 16386, which iteration 16386 has
 * certainly claimed, so a table with room left would have probed it somewhere
 * else.  Getting the fold back can only mean the fallback ran.
 */
static void
ino_exhaust(void)
{
	static unsigned char seen[65536 / 8];
	unsigned long long i, keep = 0x8000000000000001ULL;
	unsigned long long late = 0x4000000000000002ULL;
	v8_ino_t first, v;

	if ((first = v8sys_fold_ino(keep)) == 0) _exit(2);
	seen[first >> 3] |= 1 << (first & 7);
	for (i = 1; i <= 65535; i++) {
		if ((v = v8sys_fold_ino(i)) == 0) _exit(3);
		if (seen[v >> 3] & (1 << (v & 7))) {
			if (v != classicfold(i)) _exit(7);
		} else
			seen[v >> 3] |= 1 << (v & 7);
	}
	/* full: a host inode never seen before falls back to the plain fold */
	if (v8sys_fold_ino(late) != classicfold(late)) _exit(4);
	/* and it is still stable, because the plain fold is a pure function */
	if (v8sys_fold_ino(late) != classicfold(late)) _exit(5);
	/* an assignment made before it filled is still honoured */
	if (v8sys_fold_ino(keep) != first) _exit(6);
	_exit(0);
}

/*
 * ---------------------------------------------------------------- 9P2000
 *
 * The codec in shim/p9, which is the wire between a V8 program and the v8fs
 * server -- §8a step 5e, and shim/kern/NOTES.md says why there has to be a
 * wire.  It is tested HERE rather than in a suite of its own because this is
 * already the place that links the shim's sources directly and calls them as
 * C, and because the transport half (shim/v8sys/p9io.c) is raw-syscall code
 * that belongs to libv8sys.
 *
 * THE ROUND TRIP IS THE WEAKEST THING THAT COULD BE ASSERTED HERE, so it is
 * not what most of these do.  Encode-then-decode agrees with itself under any
 * self-consistent byte order, including a wrong one -- and a wrong one is
 * invisible until the day something on the far end is not this codec (u9fs,
 * 9pfuse, a Plan 9 client).  So the width cases assert the BYTES.  Same
 * discipline as comparing a generated image against what the clock alone
 * does, rather than against another run of the same program.
 */
static void
p9_widths(void)
{
	unsigned char buf[256];
	struct p9buf b;
	struct p9qid q, q2;
	char s[P9_NAMELEN];

	p9_init(&b, buf, sizeof buf);
	p9_p16(&b, 0x1234);
	ok(p9_len(&b) == 2 && buf[0] == 0x34 && buf[1] == 0x12,
	    "p16 puts the low byte first");

	p9_init(&b, buf, sizeof buf);
	p9_p32(&b, 0x12345678U);
	ok(p9_len(&b) == 4 && buf[0] == 0x78 && buf[1] == 0x56 &&
	    buf[2] == 0x34 && buf[3] == 0x12, "p32 puts the low byte first");

	p9_init(&b, buf, sizeof buf);
	p9_p64(&b, 0x0123456789abcdefULL);
	ok(p9_len(&b) == 8 && buf[0] == 0xef && buf[1] == 0xcd &&
	    buf[2] == 0xab && buf[3] == 0x89 && buf[4] == 0x67 &&
	    buf[5] == 0x45 && buf[6] == 0x23 && buf[7] == 0x01,
	    "p64 puts the low byte first");

	/*
	 * A string is count[2] and then the bytes, with NO terminator.  The
	 * absence is the assertion: a codec that wrote one would round-trip
	 * perfectly against itself and be one byte long everywhere else.
	 */
	p9_init(&b, buf, sizeof buf);
	p9_pstr(&b, "abc");
	ok(p9_len(&b) == 5 && buf[0] == 3 && buf[1] == 0 && buf[2] == 'a' &&
	    buf[3] == 'b' && buf[4] == 'c', "a string is counted, not terminated");

	p9_init(&b, buf, sizeof buf);
	p9_pstr(&b, "");
	ok(p9_len(&b) == 2 && buf[0] == 0 && buf[1] == 0, "the empty string is two bytes");

	p9_init(&b, buf, sizeof buf);
	p9_pstr(&b, (char *)0);
	ok(p9_len(&b) == 2 && buf[0] == 0, "a null string encodes as the empty one");

	/* ...and the far side reads back what was written. */
	p9_init(&b, buf, sizeof buf);
	p9_p8(&b, 0xa5); p9_p16(&b, 0xbeef); p9_p32(&b, 0xdeadbeefU);
	p9_p64(&b, 0xfeedfacecafebabeULL); p9_pstr(&b, "hello");
	ok(p9_ok(&b), "the mixed encode fitted");
	p9_init(&b, buf, sizeof buf);
	ok(p9_g8(&b) == 0xa5 && p9_g16(&b) == 0xbeef &&
	    p9_g32(&b) == 0xdeadbeefU && p9_g64(&b) == 0xfeedfacecafebabeULL &&
	    p9_gstr(&b, s, sizeof s) == 5 && strcmp(s, "hello") == 0 && p9_ok(&b),
	    "every width decodes to what was encoded");

	q.q_type = P9_QTDIR; q.q_vers = 7; q.q_path = 0x1122334455667788ULL;
	p9_init(&b, buf, sizeof buf);
	p9_pqid(&b, &q);
	ok(p9_len(&b) == P9_QIDSZ, "a qid is thirteen bytes");
	p9_init(&b, buf, sizeof buf);
	p9_gqid(&b, &q2);
	ok(q2.q_type == q.q_type && q2.q_vers == q.q_vers &&
	    q2.q_path == q.q_path && p9_ok(&b), "a qid round-trips");
}

/*
 * The stat, and its two length prefixes.  9P2000's one real wart: the message
 * field is stat[n] -- a 2-byte count -- and the bytes inside it begin with the
 * structure's OWN size[2], counting everything after itself.  The two numbers
 * differ by exactly two, and a reader that conflates them lands two bytes into
 * the name, which decodes as a plausible short string rather than as an error.
 * So the difference is asserted rather than described.
 */
static void
p9_stats(void)
{
	unsigned char buf[1024];
	struct p9buf b;
	struct p9stat s, t;
	long inner, outer;

	memset(&s, 0, sizeof s);
	s.s_type = 0; s.s_dev = 0;
	s.s_qid.q_type = P9_QTFILE; s.s_qid.q_vers = 0; s.s_qid.q_path = 42;
	s.s_mode = 0644; s.s_atime = 1000; s.s_mtime = 2000; s.s_length = 28000;
	strcpy(s.s_name, "hello"); strcpy(s.s_uid, "root");
	strcpy(s.s_gid, "root"); strcpy(s.s_muid, "root");

	p9_init(&b, buf, sizeof buf);
	p9_pstat(&b, &s);
	inner = (long)buf[0] | ((long)buf[1] << 8);
	outer = p9_len(&b);
	ok(p9_ok(&b) && inner == outer - 2,
	    "the stat's own size counts everything after itself");

	p9_init(&b, buf, sizeof buf);
	ok(p9_gstat(&b, &t) == 0 && p9_ok(&b), "a stat decodes");
	ok(t.s_qid.q_path == 42 && t.s_mode == 0644 && t.s_length == 28000 &&
	    strcmp(t.s_name, "hello") == 0 && strcmp(t.s_uid, "root") == 0,
	    "...to the fields that went in");
	ok(p9_len(&b) == outer, "...consuming exactly the bytes it was given");

	/*
	 * AND IT STEPS OVER A FIELD IT DOES NOT KNOW.  A 9P2000 stat is
	 * explicitly extensible, so a reader that ignores the inner size works
	 * against our own server and desynchronises against u9fs -- with the
	 * damage landing on the NEXT message, which is the hardest place to
	 * diagnose.  Four extra bytes are appended and the inner count raised
	 * to cover them; the decode must still end where the stat ends.
	 */
	p9_init(&b, buf, sizeof buf);
	p9_pstat(&b, &s);
	p9_p32(&b, 0xffffffffU);
	inner += 4;
	buf[0] = (unsigned char)(inner & 0xff);
	buf[1] = (unsigned char)((inner >> 8) & 0xff);
	outer = p9_len(&b);
	p9_init(&b, buf, sizeof buf);
	ok(p9_gstat(&b, &t) == 0 && strcmp(t.s_name, "hello") == 0 &&
	    p9_len(&b) == outer,
	    "an unknown trailing field is stepped over, not tripped on");
}

/*
 * THE BOUNDS, which is what this codec is really for.  A server reads its
 * messages off a socket: the length in the header and the fields inside it are
 * two claims by the same untrusted party and need not agree.  Every get and
 * put goes through one `room()' and sets a sticky flag, so these cases are
 * about that flag rather than about forty call sites.
 */
static void
p9_bounds(void)
{
	unsigned char buf[64];
	struct p9buf b;
	char s[P9_NAMELEN];
	char small[4];

	p9_init(&b, buf, 4);
	p9_p32(&b, 1);
	ok(p9_ok(&b), "four bytes fit in four");
	p9_p8(&b, 1);
	ok(!p9_ok(&b), "the fifth does not");
	ok(p9_fin(&b) == -1, "and a message that did not fit cannot be finished");

	/* A get past the end returns 0 rather than whatever was there. */
	p9_init(&b, buf, sizeof buf);
	p9_p32(&b, 0xdeadbeefU);
	p9_init(&b, buf, 2);
	ok(p9_g32(&b) == 0 && !p9_ok(&b), "a short read decodes as zero, not as junk");

	/* ...and the flag is sticky, so a later get cannot look successful. */
	ok(p9_g8(&b) == 0 && !p9_ok(&b), "the failure is sticky");

	/*
	 * A NAME THAT DOES NOT FIT IS REFUSED, NOT TRUNCATED.  FSNMLG's lesson
	 * from src/include/PORTING.md: a truncated path does not shorten a
	 * column, it sends the reader down the arm for a different kind of
	 * object.  Here the caller would get a valid-looking short name for a
	 * file that is not the one the server named.
	 */
	p9_init(&b, buf, sizeof buf);
	p9_pstr(&b, "abcdefgh");
	p9_init(&b, buf, sizeof buf);
	ok(p9_gstr(&b, small, sizeof small) == -1 && !p9_ok(&b),
	    "a name too long for the caller's buffer is refused");
	ok(small[0] == '\0', "...and the buffer is left empty rather than partial");

	/* A count that runs past the message is the same class, one level up. */
	buf[0] = 40; buf[1] = 0;		/* claims 40 bytes ... */
	p9_init(&b, buf, 10);			/* ... in a 10-byte message */
	ok(p9_gstr(&b, s, sizeof s) == -1 && !p9_ok(&b),
	    "a string longer than the message it is in is refused");

	p9_init(&b, buf, 10);
	ok(p9_gdata(&b, 20) == 0 && !p9_ok(&b),
	    "p9_gdata refuses rather than pointing past the message");
	p9_init(&b, buf, 10);
	ok(p9_gdata(&b, 10) == buf && p9_ok(&b), "...and yields the bytes when they are there");

	/* The header the framing writes, read back field by field. */
	p9_hdr(&b, buf, sizeof buf, P9_Tversion, P9_NOTAG);
	p9_p32(&b, P9_MSIZE);
	p9_pstr(&b, P9_VERSION);
	ok(p9_fin(&b) == 4 + 1 + 2 + 4 + 2 + 6, "Tversion is the length it should be");
	p9_init(&b, buf, sizeof buf);
	ok(p9_g32(&b) == (p9_u32)(4 + 1 + 2 + 4 + 2 + 6) &&
	    p9_g8(&b) == P9_Tversion && p9_g16(&b) == P9_NOTAG &&
	    p9_g32(&b) == P9_MSIZE && p9_gstr(&b, s, sizeof s) == 6 &&
	    strcmp(s, P9_VERSION) == 0,
	    "...and its size, type and tag are where the spec puts them");
}

/*
 * THE FRAMING, over a real socket rather than a fake read.
 *
 * The property is that one read is not one message, and it fails in both
 * directions.  A message can arrive in pieces, and two messages can arrive in
 * one piece -- the second is the deterministic one and is asserted first: both
 * are written with a single write(2), so a p9_recv that returned everything it
 * read would swallow the second and the tag comparison would fail.
 *
 * The short-read half needs the socket buffer to be smaller than the message,
 * which the host may refuse to arrange.  So the relation is MEASURED and the
 * case says "not exercised" rather than passing silently -- tests/kmemu's rule
 * about asserting a property of the machine.
 *
 * EVERY RECEIVING SOCKET HERE CARRIES A DEADLINE, and it is not decoration --
 * it was put there by a mutation.  Breaking p9_recv so it trusts one read
 * makes the FIRST receive swallow both messages and the second block forever:
 * the harness hung, `make test' would have hung with it, and a suite that
 * hangs reports nothing at all.  That is the ttyprobe lesson (the failure mode
 * of the thing under you is a hang) arriving in a socket rather than in a
 * stream.  SO_RCVTIMEO turns it into a -1, so the case goes red.  It is a
 * property of a socket this function created, not of the machine.
 *
 * AND THE DEADLINE ON THE SOCKET WAS NOT ENOUGH, which is worth recording
 * because the second hang looked exactly like the first.  The same mutation
 * leaves the CHILD blocked in write(2) -- it is pushing 8216 bytes into a
 * 512-byte socket that the parent has stopped draining -- so the parent then
 * hung in waitpid instead of in read.  A deadline on one end of a pipe is not
 * a deadline on the pipe.  p9_reap bounds the wait and kills, which is the
 * pattern runchild() above already uses for the signal cases.
 */
#define P9_DEADLINE	5		/* seconds; a correct run never waits */

static void
p9_deadline(int fd)
{
	struct timeval tv;

	tv.tv_sec = P9_DEADLINE;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

static void
p9_reap(pid_t pid)
{
	int status, waited;

	for (waited = 0; waited < P9_DEADLINE * 1000; waited += 10) {
		if (waitpid(pid, &status, WNOHANG) == pid) return;
		usleep(10000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
}

/*
 * v8_foldid's CONTRACT, AND IT HAS TO BE A UNIT TEST BECAUSE THE HOST CANNOT
 * REACH IT.
 *
 * The function narrows a 32-bit host uid into V8's 16-bit field, and the whole
 * point of it is what happens above 32767 -- where this machine's uid (501)
 * and a CI runner's never go.  CLAUDE.md's rule is to assert a RELATION THE
 * PORT CONTROLS rather than a property of the machine, and there is no relation
 * available end to end: no `ls -l' on this host can produce the input that
 * matters.  So the guard is the rule itself, over a table that includes the
 * values that broke it.
 *
 * THE TWO PROPERTIES ARE ASSERTED SEPARATELY because they fail separately.
 * A cast satisfies "root maps to root" perfectly and violates the other one;
 * a fold that forgot the id==0 case would do the reverse.
 *
 * 65536 AND 131072 ARE THE MEASURED FAILURES of the bare `(short)' cast that
 * stood in stat_translate, procfs.c and (once) fio.c.  4294967294 is `nobody'
 * on macOS, which is a real uid a real file can have.
 */
static void
foldid_contract(void)
{
	static const long v[] = {
		0, 1, 2, 501, 32766, 32767,		/* everything that fits */
		32768, 65535, 65536, 65537,		/* the wrap */
		131072, 200000, 999999, 4294967294L,	/* and past it; -2 is nobody */
		-1, -65536				/* not reachable, still guarded */
	};
	int i, n = (int)(sizeof v / sizeof v[0]);
	int exact = 1, noroot = 1, inrange = 1;

	for (i = 0; i < n; i++) {
		short f = v8_foldid(v[i]);

		/* every id that fits is kept EXACTLY -- the common case, and the
		 * reason this change is invisible on any ordinary host */
		if (v[i] >= 0 && v[i] <= 32767 && f != (short)v[i]) exact = 0;
		/* the one with teeth: nothing but 0 may come out as 0 */
		if (v[i] != 0 && f == 0) noroot = 0;
		/* and the result is a plausible V7 uid rather than a negative */
		if (f < 0) inrange = 0;
	}
	ok(v8_foldid(0) == 0, "v8_foldid: root maps to root");
	ok(exact,   "v8_foldid: every id that fits is exact");
	ok(noroot,  "v8_foldid: non-root NEVER maps to root");
	ok(inrange, "v8_foldid: never returns a negative uid");
	/* The two the cast actually got wrong, named so a failure says which. */
	ok(v8_foldid(65536) != 0,  "v8_foldid: 65536 is not root");
	ok(v8_foldid(131072) != 0, "v8_foldid: 131072 is not root");
}

static void
p9_framing(void)
{
	unsigned char out[P9_MSIZE], in[P9_MSIZE];
	struct p9buf b;
	int sv[2], want = 512, got;
	socklen_t glen;
	long n, n2, i;
	pid_t pid;
	int status;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		ok(0, "socketpair for the 9P framing cases");
		return;
	}
	p9_deadline(sv[0]);

	/* two messages, one write */
	p9_hdr(&b, out, sizeof out, P9_Tclunk, 1);
	p9_p32(&b, 11);
	n = p9_fin(&b);
	p9_hdr(&b, out + n, sizeof out - n, P9_Tclunk, 2);
	p9_p32(&b, 22);
	n2 = p9_fin(&b);
	ok(write(sv[1], out, n + n2) == n + n2, "two messages written as one");

	ok(p9_recv(sv[0], in, sizeof in) == n, "p9_recv returns the first message's length");
	p9_init(&b, in, n);
	(void)p9_g32(&b);
	ok(p9_g8(&b) == P9_Tclunk && p9_g16(&b) == 1 && p9_g32(&b) == 11,
	    "...and it is the FIRST message, not both");
	ok(p9_recv(sv[0], in, sizeof in) == n2, "the second is still there");
	p9_init(&b, in, n2);
	(void)p9_g32(&b);
	ok(p9_g8(&b) == P9_Tclunk && p9_g16(&b) == 2 && p9_g32(&b) == 22,
	    "...and it is the second");

	/* a message bigger than the socket buffer, so the reads must be short */
	setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &want, sizeof want);
	setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &want, sizeof want);
	glen = sizeof got;
	if (getsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &got, &glen) < 0) got = 0;

	p9_hdr(&b, out, sizeof out, P9_Rread, 3);
	p9_p32(&b, (p9_u32)(P9_MSIZE - 4 - 1 - 2 - 4));
	for (i = p9_len(&b); i < (long)sizeof out; i++)
		p9_p8(&b, (p9_u32)(i & 0xff));
	n = p9_fin(&b);

	if (got >= n) {
		printf("  (9P short-read case NOT EXERCISED: the host would not "
		    "shrink SO_RCVBUF below %ld, got %d)\n", n, got);
	} else if ((pid = fork()) == 0) {
		for (i = 0; i < n; ) {
			long k = write(sv[1], out + i, n - i);
			if (k <= 0) _exit(1);
			i += k;
		}
		_exit(0);
	} else {
		ok(p9_recv(sv[0], in, sizeof in) == n,
		    "a message larger than the socket buffer arrives whole");
		ok(memcmp(in, out, (size_t)n) == 0, "...byte for byte");
		p9_reap(pid);
	}

	close(sv[0]); close(sv[1]);

	/*
	 * SIZE FIELDS THE RECEIVER MUST REJECT, and the second of these was
	 * VACUOUS in its first form -- which the mutation said and no green run
	 * ever would.  It asserted only that p9_recv returned -1, and with no
	 * body in the socket a p9_recv WITHOUT the bound also returns -1, on the
	 * deadline rather than on the check.  The mutation that deletes `n > max'
	 * changed nothing.
	 *
	 * So the body is supplied, and what is asserted is the thing the bound is
	 * actually for: nothing was written past the caller's buffer.  The canary
	 * is a real array after a short one, and a p9_recv that trusted the
	 * header would put 196 bytes into a hundred-byte buffer and land in it.
	 */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		ok(0, "socketpair for the size-bound cases");
		return;
	}
	p9_deadline(sv[0]);

	out[0] = 3; out[1] = out[2] = out[3] = 0;
	ok(write(sv[1], out, 4) == 4, "a header claiming three bytes is written");
	ok(p9_recv(sv[0], in, sizeof in) == -1, "...and refused: under the header size");

	{
		struct { unsigned char buf[100]; unsigned char guard[128]; } g;
		int clean = 1;

		memset(&g, 0x5a, sizeof g);
		out[0] = 200; out[1] = out[2] = out[3] = 0;
		for (i = 4; i < 200; i++) out[i] = 0xa5;
		ok(write(sv[1], out, 200) == 200,
		    "a 200-byte message is written whole");
		ok(p9_recv(sv[0], g.buf, (long)sizeof g.buf) == -1,
		    "...and refused: over the caller's buffer");
		for (i = 0; i < (long)sizeof g.guard; i++)
			if (g.guard[i] != 0x5a) clean = 0;
		ok(clean, "...with nothing written past the end of it");
	}

	/*
	 * AND THE BOUND HAS TO BE CHECKED BEFORE THE FIRST READ, which the case
	 * above structurally cannot see: it passes 100, so the four header
	 * bytes land inside the buffer whether or not anything checked.  The
	 * size field must be in the buffer before it can be parsed, so the
	 * header read is the one transfer that cannot be bounded by the size --
	 * it has to be bounded by the caller's buffer instead, up front.
	 * p9_recv did not, and with max 2 it put two bytes past a two-byte
	 * buffer while returning a perfectly correct -1.
	 */
	{
		struct { unsigned char buf[2]; unsigned char guard[64]; } g;
		int clean = 1;

		memset(&g, 0x5a, sizeof g);
		out[0] = 0xc8; out[1] = out[2] = out[3] = 0;	/* claims 200 */
		ok(write(sv[1], out, 4) == 4, "a four-byte header is written");
		ok(p9_recv(sv[0], g.buf, (long)sizeof g.buf) == -1,
		    "...and a buffer too small to hold a header is refused");
		for (i = 0; i < (long)sizeof g.guard; i++)
			if (g.guard[i] != 0x5a) clean = 0;
		ok(clean, "...having read nothing into it at all");
	}

	close(sv[0]); close(sv[1]);

	/*
	 * A CLEAN END OF FILE AND A TRUNCATED MESSAGE ARE DIFFERENT ANSWERS,
	 * and the server's main loop turns on the difference: a client that has
	 * gone away is not a protocol error, but a client that sent half a
	 * header has broken the stream and the connection cannot be resumed.
	 */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
		p9_deadline(sv[0]);
		close(sv[1]);
		ok(p9_recv(sv[0], in, sizeof in) == 0, "a clean close reads as end of file");
		close(sv[0]);
	} else
		ok(0, "socketpair for the end-of-file case");

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
		p9_deadline(sv[0]);
		ok(write(sv[1], "\003\000", 2) == 2, "two bytes of a header are written");
		close(sv[1]);
		ok(p9_recv(sv[0], in, sizeof in) == -1,
		    "...and a close after them is an error, not end of file");
		close(sv[0]);
	} else
		ok(0, "socketpair for the truncated-header case");
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

	/*
	 * ------------------------------------------------ inode identity
	 *
	 * FIRST IN main(), AND THE POSITION IS PART OF THE TEST.  The v7 inode
	 * number a host inode gets depends on what the process has already
	 * asked for -- the table is append-only, so the first claimant of a
	 * value keeps it.  Running before anything stats a file is what makes
	 * "the first one gets the old fold" a deterministic assertion, and
	 * that case is therefore also the guard on the position: move this
	 * block down and it goes red rather than going quietly weak.
	 *
	 * The three inodes collide by construction.  The fold XORs the four
	 * 16-bit words together, so flipping the same bit in any two of them
	 * cancels: b flips bit 0 of words 0 and 1, c of words 0 and 2.
	 *
	 * A NOTE ON THE CONSTANTS, BECAUSE THE FIRST SET MADE THE HEADLINE
	 * ASSERTION VACUOUS.  They were 0x0123456789abcdef and its two
	 * variants, whose four 16-bit words XOR to exactly 0 -- so
	 * classicfold() returned its 0-substitute 1, and "the first claimant
	 * gets the old fold" was asserting 1 == 1.  Mutating inofold() to
	 * `v + 1' left all five cases green while every non-colliding inode in
	 * the system changed its number.  0x00ff00ff00ff5a3c folds to 0x5ac3,
	 * which is a value rather than a special case, and the same mutation
	 * now goes red.  The uncontended inode below is the second half of
	 * that repair: it exercises the path where nothing probes at all.
	 */
	{
		unsigned long long a = 0x00ff00ff00ff5a3cULL;	/* folds to 0x5ac3 */
		unsigned long long b = a ^ 0x0000000000010001ULL;
		unsigned long long c = a ^ 0x0000000100000001ULL;
		unsigned long long g = 0x0f0f0f0f0f0f1234ULL;	/* folds to 0x1d3b */
		v8_ino_t fa, fb, fc;

		ok(classicfold(a) != 0 && classicfold(a) != 1,
		    "the synthetic fold is an ordinary value, not the 0 substitute");
		ok(classicfold(a) == classicfold(b) &&
		   classicfold(a) == classicfold(c),
		    "the three synthetic inodes really do collide under the fold");
		ok(classicfold(g) != classicfold(a),
		    "...and the uncontended one does not collide with them");
		fa = v8sys_fold_ino(a);
		fb = v8sys_fold_ino(b);
		fc = v8sys_fold_ino(c);
		ok(fa == classicfold(a),
		    "the first claimant of a value gets exactly the old fold");
		ok(v8sys_fold_ino(g) == classicfold(g),
		    "an inode nobody contends for gets the old fold untouched");
		ok(fa != fb && fa != fc && fb != fc,
		    "colliding host inodes get DIFFERENT v7 numbers");
		ok(fa != 0 && fb != 0 && fc != 0,
		    "no v7 inode is 0, which V7 reads as an empty slot");
		ok(v8sys_fold_ino(a) == fa && v8sys_fold_ino(b) == fb &&
		   v8sys_fold_ino(c) == fc,
		    "an assignment is never revised: ask again, same answer");
		ok(v8sys_fold_ino(b) == fb,
		    "...and again, after the other two have been re-asked");
	}
	okchild(ino_exhaust, 20000,
	    "exhausting the 16-bit space degrades to the fold, without a hang");

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
	okchild(c_sigsys_restart, 5000,
	    "...but a DEFERRED handler restarts it, which is what SNUSIG means");
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
	ok(v8fs_typefor("/bin/cat", V8P_LOOK)   == &v8fs_pass, "a V8 directory is claimed by a mount");
	ok(v8fs_typefor("/etc/group", V8P_LOOK) == &v8fs_pass, "...and so is /etc");
	ok(v8fs_typefor("/etc", V8P_LOOK)       == &v8fs_pass, "...and the directory itself, with no slash");
	ok(v8fs_typefor("/usr/lib/tmac/x", V8P_LOOK) == &v8fs_pass, "...and anything beneath one");

	/*
	 * The negative half, which is the half that matters.  A prefix table
	 * that claimed everything would pass every case above.
	 */
	ok(v8fs_typefor("/tmp/x", V8P_LOOK)   == 0, "an unclaimed path has no mount");
	ok(v8fs_typefor("relative", V8P_LOOK) == 0, "nor does a relative one");
	ok(v8fs_typefor("/binary", V8P_LOOK)  == 0, "/binary is not /bin/, despite the prefix");
	ok(v8fs_typefor("/unix", V8P_LOOK)    == &v8fs_pass, "/unix is claimed exactly");
	ok(v8fs_typefor("/unixfoo", V8P_LOOK) == 0, "...and /unixfoo is not, which is why m_exact exists");

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

	/*
	 * A NULL PATH IS THE KERNEL'S TO REJECT, AND THE SHIM MUST NOT LOOK
	 * FIRST.
	 *
	 * rootpath() passes a null through unchanged for exactly that reason,
	 * so unlink(0) is meant to come back EFAULT.  dotlink(), which decides
	 * whether a name ends in "." or "..", inspected the string BEFORE the
	 * syscall could answer, so it faulted in our own code instead.
	 *
	 * Found through yacc, not by writing this case: `yacc -o' with -o last
	 * leaves the output name null, openup() cannot create it, and error()
	 * runs cleantmp() -- two unlink()s of temp names setup() had not yet
	 * assigned.  The crash looked like a crash in yacc.  tests/wavea has
	 * the program end of it.
	 *
	 * Reaching the call at all is the assertion; the return value is the
	 * host's business.  If dotlink() ever dereferences again this dies on
	 * SIGSEGV and the suite prints nothing after it.
	 */
	{
		v8s_unlink((char *)0);
		ok(1, "unlink(0) returns instead of faulting in the shim");
		v8s_link((char *)0, (char *)0);
		ok(1, "and so does link(0, 0)");
	}

	/*
	 * ---------------------------------------------------------------
	 * /dev/fd -- V8's controlling terminal, which is not a device.
	 *
	 * open("/dev/fd/n") is dup(n) and /dev/tty is the hard link at n = 3.
	 * usr/man/man4/fd.4 is the specification; sys/sys2.c:174 is the code;
	 * cmd/init.c:379-381 is why fd 3 is the terminal.  shim/v8sys/vfs.c has
	 * the argument.
	 *
	 * fd 3 IS ARRANGED HERE rather than inherited.  Whether the harness
	 * leaves one open is a property of the machine, and this suite has been
	 * bitten by that class three times; every case below asserts a relation
	 * between two things this port controls.
	 */
	{
		int t3, a, b, hostfd;
		struct v8_stat s2;
		char c;

		snprintf(sub, sizeof sub, "%s/fdfile", tmpl);
		fd = v8s_creat(sub, 0644);
		v8s_write(fd, "ABCDEFGHIJKLMNOPQRST", 20);
		v8s_close(fd);

		t3 = v8s_open(sub, 0, 0);
		ok(t3 >= 0, "/dev/fd: a file to stand in for the terminal");
		/* Put it on 3, which is where init.c's third dup(0) puts it. */
		ok(v8s_dup2(t3, 3) == 3, "/dev/fd: dup2 onto fd 3, as init does");
		if (t3 != 3) v8s_close(t3);

		/* The four hard links, by minor.  proto-dev:76-78,91. */
		ok(v8s_read(3, buf, 5) == 5 && memcmp(buf, "ABCDE", 5) == 0,
		    "/dev/fd: fd 3 reads the file");
		a = v8s_open("/dev/tty", 0, 0);
		ok(a >= 0, "/dev/tty opens when fd 3 is open");
		/*
		 * THE PROPERTY THAT MAKES IT A dup AND NOT A RE-OPEN: one file
		 * offset behind two descriptors.  A re-open would restart at
		 * byte 0 and print ABCDE here.
		 */
		ok(v8s_read(a, buf, 5) == 5 && memcmp(buf, "FGHIJ", 5) == 0,
		    "/dev/tty shares fd 3's offset -- it is a dup, not a re-open");
		ok(v8s_read(3, buf, 5) == 5 && memcmp(buf, "KLMNO", 5) == 0,
		    "...and the sharing runs both ways");
		v8s_close(a);

		/*
		 * Mode is ignored: "Creat(2) is equivalent to open, and mode is
		 * ignored.  As with dup, subsequent IO on fd fails unless the
		 * original file descriptor allows the read or write operation."
		 * macOS's own /dev/fd refuses this at open with EACCES, which is
		 * why the type is implemented here rather than delegated.
		 */
		a = v8s_open("/dev/tty", 1 /*write*/, 0);
		ok(a >= 0, "/dev/tty ignores the mode and opens (fd.4)");
		ok(v8s_write(a, "x", 1) < 0, "...and the WRITE is what fails");
		v8s_close(a);

		/*
		 * creat.  This case is the one that proves v8s_creat goes
		 * through the filesystem switch: before it did, this truncated
		 * rootfs/dev/tty and returned a descriptor on an empty file, so
		 * the read below would return 0.
		 */
		v8s_lseek(3, 0, 0);
		a = v8s_creat("/dev/tty", 0666);
		ok(a >= 0, "creat(\"/dev/tty\") is open, not a truncation");
		ok(v8s_read(a, buf, 5) == 5 && memcmp(buf, "ABCDE", 5) == 0,
		    "...and the file behind fd 3 still has its bytes");
		v8s_close(a);

		/* The other three names, and the numeric spelling of each. */
		ok(v8s_open("/dev/stdin", 0, 0) >= 0, "/dev/stdin is minor 0");
		ok(v8s_open("/dev/fd/0", 0, 0) >= 0, "/dev/fd/0 is the same node");
		v8s_lseek(3, 5, 0);
		a = v8s_open("/dev/fd/3", 0, 0);
		ok(a >= 0 && v8s_read(a, &c, 1) == 1 && c == 'F',
		    "/dev/fd/3 and /dev/tty are one node");
		v8s_close(a);

		/*
		 * fd.4: "Open returns -1 if the related file descriptor is not
		 * open."  EBADF from getf, and NOT the empty rootfs/dev/tty
		 * node -- which exists, so a row-ordering mistake would read 0
		 * bytes and pass every case above.
		 */
		v8s_close(3);
		a = v8s_open("/dev/tty", 0, 0);
		ok(a < 0 && v8_errno == V8_EBADF,
		    "/dev/tty with fd 3 closed is EBADF, not the empty node");
		ok(v8s_open("/dev/fd/3", 0, 0) < 0, "...and so is /dev/fd/3");

		/*
		 * A NAME THE /dev/fd DIRECTORY DOES NOT CONTAIN IS ENOENT, not
		 * EBADF -- namei never reaches open1.  V8 shipped 0..127 and
		 * NOFILE is 128, so 128 is past the end of the node set.  macOS
		 * answers EBADF here, which is the host's rule and not V8's.
		 */
		ok(v8s_open("/dev/fd/128", 0, 0) < 0 && v8_errno == V8_ENOENT,
		    "/dev/fd/128 is ENOENT: V8 shipped 128 nodes, 0 through 127");
		ok(v8s_open("/dev/fd/999", 0, 0) < 0 && v8_errno == V8_ENOENT,
		    "/dev/fd/999 is ENOENT, where macOS says EBADF");
		ok(v8s_open("/dev/fd/x", 0, 0) < 0 && v8_errno == V8_ENOENT,
		    "/dev/fd/x is ENOENT");
		ok(v8s_open("/dev/fd/01", 0, 0) < 0 && v8_errno == V8_ENOENT,
		    "/dev/fd/01 is ENOENT: the node is named 1, not 01");
		ok(v8s_open("/dev/fd/", 0, 0) < 0 && v8_errno == V8_ENOENT,
		    "/dev/fd/ with no component is ENOENT");

		/*
		 * THE ROW ORDER, ASSERTED DIRECTLY.  /dev/fd the DIRECTORY is an
		 * ordinary directory and must stay passthrough, or `ls /dev/fd'
		 * asks the descriptor type to open a name with no minor in it.
		 * The exact row before the prefix row is what arranges that, and
		 * nothing else in the suite would notice it being dropped.
		 */
		ok(v8fs_typefor("/dev/fd", V8P_LOOK) == &v8fs_pass,
		    "the /dev/fd DIRECTORY is passthrough");
		ok(v8fs_typefor("/dev/fd/3", V8P_LOOK) == &v8fs_fdfs,
		    "...and its entries are not");
		ok(v8fs_typefor("/dev/tty", V8P_LOOK) == &v8fs_fdfs, "/dev/tty is claimed");
		ok(v8fs_typefor("/dev/kmem", V8P_LOOK) == &v8fs_pass,
		    "...and /dev/kmem is still the groveler's");

		/*
		 * stat and fstat DISAGREE, and that is V8 rather than a defect:
		 * stat reads an inode in /dev, which is a character device
		 * whatever the descriptor points at, while fstat follows the
		 * open file to the real object.  `test -c /dev/tty' is true with
		 * a plain file on fd 3.
		 */
		ok(v8s_stat("/dev/fd/1", &s2) == 0, "stat(/dev/fd/1)");
		ok((s2.st_mode & V8_S_IFMT) == V8_S_IFCHR,
		    "...says character device, whatever fd 1 is");
		ok(s2.st_rdev == (v8_dev_t)((40 << 8) | 1),
		    "...with rdev makedev(40, 1) -- conf/devices:55");
		ok(s2.st_nlink == 2, "...and nlink 2, the /dev/stdout link");
		ok(v8s_stat("/dev/fd/50", &s2) == 0 && s2.st_nlink == 1,
		    "an unlinked minor has nlink 1");
		ok(v8s_stat("/dev/fd/999", &s2) < 0, "stat of a missing node fails");

		t3 = v8s_open(sub, 0, 0);
		v8s_dup2(t3, 3);
		if (t3 != 3) v8s_close(t3);
		a = v8s_open("/dev/tty", 0, 0);
		ok(v8s_fstat(a, &s2) == 0 &&
		    (s2.st_mode & V8_S_IFMT) == V8_S_IFREG,
		    "fstat on the descriptor reports the REAL object");
		v8s_close(a);

		/*
		 * dup and dup2 carry the descriptor's filesystem.  Both dropped
		 * it until /dev/fd made dup the point: v8fs_fdtype() is how
		 * every read and ioctl finds its type, and an unbound descriptor
		 * reads as passthrough -- so dup() of a /proc file quietly
		 * returned one whose reads went to the host.  Nothing in the
		 * tree dup'd one, which is why nothing said so.
		 *
		 * Bound by hand here because this binary has no libkmemu and so
		 * no /proc: the mechanism is what is under test, not procfs.
		 */
		hostfd = v8s_open(sub, 0, 0);
		v8fs_bind(hostfd, &v8fs_fdfs);
		b = v8s_dup(hostfd);
		ok(v8fs_fdtype(b) == &v8fs_fdfs, "dup carries the fd's type");
		v8s_close(b);
		b = v8s_open(sub, 0, 0);
		ok(v8fs_fdtype(b) == &v8fs_pass, "a fresh open is passthrough");
		ok(v8s_dup2(hostfd, b) == b && v8fs_fdtype(b) == &v8fs_fdfs,
		    "dup2 replaces the target's type as well as the target");
		/*
		 * ...and it OVERWRITES rather than merges, which is a property
		 * of v8fs_bind storing null for the passthrough type.  This case
		 * had a different name and was VACUOUS: it claimed to guard a
		 * v8fs_unbind() call standing in front of the bind, and the
		 * mutation that deleted that call changed nothing, because bind
		 * already clears the row.  The call is gone; what is left here
		 * is the property that was actually load-bearing.
		 */
		v8fs_unbind(hostfd);
		ok(v8s_dup2(hostfd, b) == b && v8fs_fdtype(b) == &v8fs_pass,
		    "dup2 of a passthrough fd CLEARS the target's stale type");

		/*
		 * A DUP OF A DIRECTORY DESCRIPTOR CANNOT BE READ, and that is a
		 * limit rather than a bug to fix here.  dir.c's V7 snapshot is
		 * keyed by descriptor, so the dup is a bare host directory fd
		 * and macOS refuses read(2) on one -- where V8, sharing the file
		 * struct, would have continued the scan at the shared offset.
		 *
		 * Asserted because /dev/fd is what makes it SPELLABLE:
		 * open("/dev/fd/N") on a directory N reaches it without anyone
		 * writing dup().  It fails loudly, which is the tolerable
		 * direction, and the case exists so a future dir.c that
		 * refcounts a shared snapshot turns this red rather than
		 * silently changing an unexamined behaviour.
		 */
		b = v8s_open(tmpl, 0, 0);
		ok(v8s_read(b, buf, 64) > 0, "a directory descriptor reads V7 records");
		a = v8s_dup(b);
		ok(a >= 0, "...and dup of it succeeds");
		ok(v8s_read(a, buf, 64) < 0,
		    "...but the dup has no snapshot, so the read fails loudly");
		v8s_close(a); v8s_close(b);
		v8s_close(b);
		v8s_close(hostfd);
		v8s_close(3);
		v8s_unlink(sub);
	}

	/*
	 * ---------------------------------------------------------------
	 * /dev/null -- the FIFTH type, and the one that exists because making a
	 * NAME authentic made an OBJECT wrong.
	 *
	 * V8 shipped /dev/null (proto-dev:25, `crw-rw-rw- 1 root man 3, 2'), so
	 * the rootfs has to hold the node or `ls /dev' lists a machine that never
	 * existed.  The jail's union rule then found that node, and four things
	 * were wrong at once: a write appended to it, a read handed the
	 * accumulation back, `test -c' was false and `test -f' true.  The read is
	 * the sharpest -- `prog < /dev/null' is how a program is given EMPTY
	 * input, and it was being given whatever last wrote.
	 *
	 * WHY THE SIZE CASE TRUNCATES THE NODE FIRST, WHICH IT DID NOT AT FIRST
	 * AND WHICH MUTATION TESTING IS WHAT CAUGHT.  The case began as "the size
	 * does not change", a relation rather than a value, for the usual reason:
	 * a checkout that ran the old code has bytes in the node and 0 is a
	 * property of history rather than of the port.  It was still wrong.  Run
	 * under a mutation, the suite WRITES into the node and leaves it there --
	 * so the next mutation's run started from 3 bytes, its truncating creat
	 * landed back on exactly 3, and the one case written for that mutation
	 * PASSED while the escape it names was happening.  Measured: the node
	 * held 3 bytes when the run finished.
	 *
	 * That is the litter shape this tree keeps meeting -- between programs
	 * sharing a directory, between cases sharing a stream, between suite
	 * sections sharing an image -- arriving between two RUNS of one case.  A
	 * relation is not enough when the run that fails is what contaminates the
	 * next one's baseline, so the baseline is now established rather than
	 * observed.
	 */
	{
		struct v8_stat s2;
		struct stat hb;
		char h[1024];
		long before = -1, after = -1;
		int a, haveh;

		/*
		 * Dispatch, IN BOTH MODES.  V8P_MAKE is not decoration: creat is
		 * what made the file in the first place, because it keys on the
		 * parent and $V8ROOT/dev exists.  A row that claimed only the
		 * LOOK path would leave the original escape open.
		 */
		ok(v8fs_typefor("/dev/null", V8P_LOOK) == &v8fs_null,
		    "/dev/null is claimed by its own type");
		ok(v8fs_typefor("/dev/null", V8P_MAKE) == &v8fs_null,
		    "...in V8P_MAKE too, which is the mode that created the file");
		/*
		 * The row is EXACT, and these two are what says so.  A prefix row
		 * would swallow both, and the second would then be a name under a
		 * character device.
		 */
		ok(v8fs_typefor("/dev/nullx", V8P_LOOK) == &v8fs_pass,
		    "...but /dev/nullx is not -- the row is exact");
		ok(v8fs_typefor("/dev/kmem", V8P_LOOK) == &v8fs_pass,
		    "...and /dev/kmem is still the groveler's");

		haveh = hostpath(h, (long)sizeof h, "/dev/null");
		if (haveh) truncate(h, 0);		/* see the note above */
		if (haveh && stat(h, &hb) == 0) before = (long)hb.st_size;
		ok(haveh && before == 0,
		    "the rootfs node exists and is empty, so `ls /dev' is authentic");

		/* The write is ACCEPTED -- dev/mem.c:156 consumes u_count -- ... */
		a = v8s_open("/dev/null", 1 /*write*/, 0);
		ok(a >= 0, "/dev/null opens for writing");
		ok(v8s_write(a, "0123456789", 10) == 10,
		    "...a write to it is accepted whole (dev/mem.c:156)");
		v8s_close(a);
		/* ...and creat is an open, not a truncation of the rootfs node. */
		a = v8s_creat("/dev/null", 0666);
		ok(a >= 0, "creat(\"/dev/null\") succeeds");
		ok(v8s_write(a, "xyz", 3) == 3, "...and it too accepts a write");
		v8s_close(a);

		/* ...and NONE of those 13 bytes reached the rootfs. */
		if (haveh && stat(h, &hb) == 0) after = (long)hb.st_size;
		ok(after == before,
		    "...but 13 bytes written through the jail did not reach the node");

		/*
		 * The read is EOF.  dev/mem.c:68 is `case 2: return;' -- it
		 * transfers nothing, which IS end of file.  Before the type, this
		 * returned the bytes written above.
		 */
		a = v8s_open("/dev/null", 0, 0);
		ok(a >= 0, "/dev/null opens for reading");
		ok(v8s_read(a, buf, 64) == 0,
		    "...and reads EOF, not what was last written (dev/mem.c:68)");
		v8s_close(a);

		/*
		 * stat.  The two machines agree about this device completely and
		 * the shim still got it wrong, because they disagree about how a
		 * major and a minor are PACKED: Darwin's makedev is
		 * (major << 24) | minor, V8's is (major << 8) | minor, and
		 * stat_translate narrows with `& 0xffff'.  A mask cannot preserve
		 * a field at bit 24, so an inherited stat reports major 0 -- which
		 * is `console' in conf/devices -- with the minor right by luck.
		 * Measured on the nodes that still fall through: /dev/zero is 3,3
		 * and the jail says `0, 3'; /dev/random is 17,0 and it says `0, 0'.
		 */
		ok(v8s_stat("/dev/null", &s2) == 0, "stat(/dev/null)");
		ok((s2.st_mode & V8_S_IFMT) == V8_S_IFCHR,
		    "...says character device, so `test -c' is true and `-f' false");
		ok(s2.st_rdev == (v8_dev_t)((3 << 8) | 2),
		    "...with rdev makedev(3, 2) -- proto-dev:25, not the masked 0,2");
	}

	/*
	 * ---------------------------------------------------------------
	 * Host device numbers through stat_translate -- the general half of the
	 * case above, and the reason it is a separate block is that /dev/null
	 * SYNTHESIZES its rdev and therefore cannot test the translation at all.
	 *
	 * The old code masked with & 0xffff, which reads like a narrowing into
	 * V8's 16-bit dev_t and is not one: Darwin packs the major at bit 24 and
	 * V8 at bit 8 (types.h:44), so the mask deleted the major and kept the
	 * minor alone.  That is not a display bug.  fsck.c:435 and df.c:142
	 * compare an st_rdev against an st_dev to decide whether a block device
	 * is the one a filesystem is mounted on, so two devices that translate
	 * to the same number are two devices the caller cannot tell apart.
	 *
	 * ASSERTED AS DISTINCTNESS, NOT AS A VALUE, for the reason the folded
	 * inode case is: which device nodes this machine has is a property of
	 * the machine, and any particular number is a property of the machine.
	 * What the port controls is that it does not MERGE two devices the host
	 * says are different.  Same shape as the $TMPDIR inode sweep, and it
	 * says out loud when the population is too small to mean anything.
	 */
	{
		struct v8_stat vs;
		struct stat hb;
		char path[256];
		v8_dev_t seen[512];
		unsigned long truth[512];
		int nseen = 0, dup = 0, i, j, k;
		DIR *d;
		struct dirent *de;

		if ((d = opendir("/dev")) != 0) {
			while ((de = readdir(d)) != 0 && nseen < 512) {
				snprintf(path, sizeof path, "/dev/%s", de->d_name);
				if (lstat(path, &hb) != 0) continue;
				if (!S_ISCHR(hb.st_mode) && !S_ISBLK(hb.st_mode)) continue;
				/* skip the names the mount table claims -- those
				 * synthesize and do not exercise the translation */
				if (v8fs_typefor(path, V8P_LOOK) != &v8fs_pass) continue;
				if (v8s_stat(path, &vs) != 0) continue;
				/* dedupe on the HOST's truth, so two links to one
				 * device are not counted as a collision */
				for (k = 0; k < nseen; k++)
					if (truth[k] == (unsigned long)hb.st_rdev) break;
				if (k < nseen) continue;
				truth[nseen] = (unsigned long)hb.st_rdev;
				seen[nseen++] = vs.st_rdev;
			}
			closedir(d);
		}
		for (i = 0; i < nseen; i++)
			for (j = i + 1; j < nseen; j++)
				if (seen[i] == seen[j]) dup++;

		if (nseen < 8) {
			printf("  (not exercised: only %d host device nodes reach"
			       " the translation)\n", nseen);
		} else {
			ok(dup == 0,
			    "distinct host devices stay distinct through stat_translate");
			if (dup) printf("  %d colliding pairs out of %d nodes\n", dup, nseen);
			/*
			 * ...and the major SURVIVES, which is the half
			 * distinctness alone would not catch: an encoding that
			 * kept the minor and threw the major away could still
			 * be injective on a host whose minors happen to differ.
			 * Measured against the host's own major, on a node the
			 * loop above already accepted.
			 */
			for (i = 0, k = -1; i < nseen; i++)
				if ((truth[i] >> 24) != 0) { k = i; break; }
			if (k < 0)
				printf("  (not exercised: every host device has major 0)\n");
			else
				ok((unsigned)(seen[k] >> 8) == ((truth[k] >> 24) & 0xff),
				    "...and a non-zero host major survives the translation");
		}
	}

	/*
	 * ------------------------------------- the jail's own path resolution
	 *
	 * TWO SYSCALLS RESOLVED NOTHING AT ALL, and every other path-taking one
	 * in the file resolves.  v8s_readlink and v8s_utime passed the V8 path
	 * straight to the host: `mv' inside the jail stamped the Mac's file of
	 * that name (mv.c:129 is utime(target, ...)), and `ls -l' on a jailed
	 * symlink read the host's (ls.c:365).  Found by the dispatch sweep run
	 * before adding a fourth filesystem type, not by anything failing.
	 *
	 * THE SHAPE IS THIS FILE'S MOST REPEATED ONE.  v8s_symlink DOES resolve
	 * its new name with mkpath -- so the port could create a jailed symlink
	 * and then not read it back.  The pair is asserted together for that
	 * reason.
	 *
	 * /usr/src is the prefix to use: it is on the mount table (Admin/Mk
	 * needs it) and, measured, macOS has no /usr/src at all -- so before
	 * the fix these fail LOUDLY with ENOENT rather than silently stamping
	 * someone else's file.  On a host that does have one, the same cases
	 * would have caught the quiet direction instead.
	 */
	{
		char jf[] = "/usr/src/v8systest-file";
		char jl[] = "/usr/src/v8systest-link";
		long tv[2];
		long n;

		hostrm(jf);
		hostrm(jl);

		fd = v8s_creat(jf, 0644);
		ok(fd >= 0, "a file can be created inside the jail");
		if (fd >= 0) { v8s_write(fd, "x", 1); v8s_close(fd); }

		tv[0] = tv[1] = 1000000000L;
		ok(v8s_utime(jf, tv) == 0, "utime finds the JAIL's file");
		ok(v8s_stat(jf, &st) == 0 && st.st_mtime == 1000000000L,
		    "...and stamps that one");

		/*
		 * THE LINK POINTS AT SOMETHING THAT EXISTS, and the first
		 * version of this case did not -- which found a THIRD instance
		 * of the same root cause and is why the case below exists.
		 */
		ok(v8s_symlink("v8systest-file", jl) == 0,
		    "a symlink can be created inside the jail");
		n = v8s_readlink(jl, buf, (long)sizeof buf);
		ok(n == 14, "...and readlink finds it");
		ok(n == 14 && strncmp(buf, "v8systest-file", 14) == 0,
		    "...and reads back what was written");

		v8s_unlink(jl);

		/*
		 * A DANGLING SYMLINK IS JAILED NOW, AND THESE CASES USED TO
		 * ASSERT THE OPPOSITE.
		 *
		 * rootpath() decided whether the rootfs had a name with
		 * `access(buf, 0)', and access(2) FOLLOWS the last component --
		 * so a symlink whose target does not exist read as ABSENT, the
		 * path fell through unresolved, and every operation on it went
		 * to the host.  It affected every syscall, not the two that
		 * showed it: v8s_unlink could not remove one either, which is
		 * how it was found, because the first run of these cases left
		 * the link behind and broke the next one.
		 *
		 * The predicate is a raw lstat now, which answers the question
		 * the union rule actually asks -- does the rootfs have this
		 * NAME -- rather than "is there a reachable object at the end
		 * of it".  Rewritten on purpose, which is what the old comment
		 * asked a future fix to have to do.
		 */
		ok(v8s_symlink("no-such-target-anywhere", jl) == 0,
		    "a DANGLING symlink can be created inside the jail");
		n = v8s_readlink(jl, buf, (long)sizeof buf);
		ok(n == 23, "...and readlink finds it, inside the jail");
		ok(n == 23 && strncmp(buf, "no-such-target-anywhere", 23) == 0,
		    "...and reads back what was written");
		/*
		 * ...AND IT REALLY IS THE JAIL'S, which the two above cannot
		 * say on their own: had the link been made on the HOST at the
		 * bare path, every call would have agreed with every other and
		 * been consistently wrong.  This reaches $V8ROOT/... directly.
		 */
		ok(hostexists(jl), "...and the link is in the rootfs, not on the host");
		ok(v8s_unlink(jl) == 0, "...and the jail can remove its own link");
		ok(!hostexists(jl), "...and it is really gone");

		/*
		 * THE OTHER SHAPES access(2) COULD NOT SEE.  Measured on this
		 * host, the two predicates disagree on exactly four things and
		 * every one is "the last component is a symlink whose
		 * resolution fails": a dangling absolute link (above), a
		 * dangling RELATIVE one, a LOOP, and a link whose target sits
		 * behind a directory that cannot be searched.  Only the first
		 * was ever tested, and one instance of a class is how the two
		 * syscalls that resolved no path at all came to be fixed one at
		 * a time.
		 *
		 * The errnos differ -- ENOENT, ENOENT, ELOOP, EACCES -- so a
		 * fix keyed on "access said ENOENT" would have covered half of
		 * them.  Keying on the QUESTION covers all four, and that is
		 * what these assert.
		 */
		ok(v8s_symlink(jl, jl) == 0, "a symlink LOOP can be made in the jail");
		ok(v8s_readlink(jl, buf, (long)sizeof buf) > 0,
		    "...and readlink finds it too, where access said ELOOP");
		ok(v8s_unlink(jl) == 0, "...and the loop can be removed");

		/*
		 * A CREATION THROUGH A DANGLING PARENT MUST NOT REACH THE HOST,
		 * which is V8P_MAKE's half and the note recorded for this fix
		 * got it wrong.  It said only V8P_LOOK changes, "because the
		 * parent case is a directory and cannot be a dangling link".
		 * Nothing stops it being one.  With access, the parent read as
		 * absent, the path fell through, and the create landed on the
		 * MAC -- the escape direction, in the mode that exists to
		 * prevent it.  With lstat the name is the jail's and the create
		 * fails.  Neither answer creates the file; only one stays in.
		 */
		{
			char jd[] = "/usr/src/v8systest-dir";
			char jn[] = "/usr/src/v8systest-dir/inside";
			char *rp;
			const char *vr = getenv("V8ROOT");
			int cfd;

			/*
			 * REMOVE IT FIRST.  A previous run that died between
			 * making this and removing it leaves a dangling symlink
			 * behind, and the next run then fails to create it --
			 * which reads as the fix being broken.  That is the
			 * crash-probe lesson arriving inside one suite: a case
			 * has to be a pure function of what it set up.  (It is
			 * also why hostrm exists at the bottom of this block.)
			 */
			hostrm(jd);
			ok(v8s_symlink("no-such-directory", jd) == 0,
			    "a dangling symlink can stand where a directory would");

			/*
			 * THE ASSERTION IS ON THE RESOLUTION, NOT ON THE
			 * CREATE, and the first draft of this block had it the
			 * other way round -- which mutation showed was VACUOUS.
			 * Reverting the predicate left three of its cases green,
			 * because the escape it is about is unobservable on a
			 * host with no writable jailed prefix: this Mac has no
			 * /usr/src at all, so the create fails whichever world
			 * it lands in, and the guard is indistinguishable from
			 * the absence.  That is the same trap the mount cases
			 * fell into with `chmod 777 /mnt'.
			 *
			 * v8sys_rootpath is not static and the answer it gives
			 * IS the behaviour that changed: with access the parent
			 * reads absent and the bare path comes back, with lstat
			 * the name is the jail's.  No host directory required.
			 */
			rp = v8sys_rootpath(jn, V8P_MAKE);
			ok(vr != 0 && *vr != '\0' && rp != 0 &&
			    strncmp(rp, vr, strlen(vr)) == 0,
			    "...and a create through it resolves INSIDE the jail");
			ok(rp != 0 && strcmp(rp, jn) != 0,
			    "...rather than falling through to the bare path");

			/* ...and it then fails, which is failing closed. */
			cfd = v8s_creat(jn, 0644);
			ok(cfd < 0, "...and the create itself fails");
			if (cfd >= 0) v8s_close(cfd);
			ok(!hostexists(jn), "...leaving nothing in the rootfs");
			ok(v8s_unlink(jd) == 0, "...and the stand-in comes away");
		}

		hostrm(jl);
		v8s_unlink(jf);
	}

	/*
	 * ------------------------------------------------------------ 9P
	 *
	 * Last, and the position is not arbitrary: these are the only cases
	 * here that fork, and one of them writes into a socket whose buffer it
	 * has deliberately shrunk.  Running them after the filesystem cases
	 * keeps a failure in either from being read as a failure in the other.
	 */
	p9_widths();
	p9_stats();
	p9_bounds();
	p9_framing();
	foldid_contract();

	/* ------------------------------------------------------- cleanup */
	snprintf(sub, sizeof sub, "%s/a", tmpl); v8s_unlink(sub);
	snprintf(sub, sizeof sub, "%s/bb", tmpl); v8s_unlink(sub);
	snprintf(sub, sizeof sub, "%s/a-very-long-name-indeed", tmpl); v8s_unlink(sub);
	snprintf(sub, sizeof sub, "%s/file", tmpl); v8s_unlink(sub);
	ok(v8s_rmdir(tmpl) == 0, "rmdir");

	printf("v8sys: %d passed, %d failed\n", pass, fail);
	return (fail != 0);
}
