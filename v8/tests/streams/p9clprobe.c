/*
 * p9clprobe -- the v8fs CLIENT, driven from the syscall layer.
 *
 * tests/streams' §8a step 5e section drives shim/v8sys/p9cl.c through the
 * SHIPPED BINARIES -- cat, ls, tail, wc, sh, chmod -- which is the right
 * headline claim ("a 1985 program reads a 1985 filesystem over a socket") and
 * is the reason those cases exist.  It leaves three paths with no case at all,
 * because no V8 program in this tree performs them:
 *
 *   1. fstat ON A DIRECTORY DESCRIPTOR.  p9_t_fstat overrides the server's
 *      i_size with the snapshot's length, and deleting that line was mutated
 *      and left the suite green.  The only V8 idiom that reads the field is
 *      ps(1)'s getdir(), and ps reads /proc rather than a mount.
 *   2. lseek IN ALL THREE WHENCES.  tail(1) reaches Tseek end to end, but only
 *      SEEK_END; nothing sends a negative SEEK_CUR, and nothing reaches
 *      do_seek's overflow guard.
 *   3. dup SHARING ONE OFFSET.  That is the CENTRAL CLAIM of the design -- the
 *      connection is the open file description -- and it follows structurally
 *      from the offset living on the server rather than being asserted.  A
 *      shell construct cannot express it: stdio makes two readers of one
 *      descriptor read in block-sized gulps, so the second sees nothing.
 *
 * This is a HOST binary compiled against the shim SOURCES, the shape
 * tests/v8sys/test.c already has.  It is not a V8 program and does not pretend
 * to be one: it calls v8s_open/v8s_read/v8s_lseek directly, which is exactly
 * the seam the three paths above live at.
 *
 * IT MUST NOT LINK libv8kern.  §8a step 5e costed that and found 56 symbol
 * collisions over 29 programs, 25 of them silent -- `char buf[4096]' in a
 * program against `struct buf *buf' in the kernel.  Nothing here needs the
 * kernel: the kernel is in the other process, which is the whole point.
 *
 * IT FOUND FOUR THINGS BEYOND THE THREE IT WAS WRITTEN FOR, all recorded in
 * shim/kern/NOTES.md: the pair of numbers in p9_t_fstat's own comment described
 * no directory that exists; p9walk answered ENOENT where V7 answers ENOTDIR,
 * because a short Rwalk carries no errno; a dead server raised SIGPIPE and
 * KILLED a V7 program where a V8 disk gives EIO; and do_seek's overflow guard
 * turned out to be unreachable by any behavioural test, which is what the
 * sanitized server in the Makefile exists for.
 *
 * Output is `key value' lines for run.sh's q() to read, the convention
 * p9probe.c and fsprobe.c already use.  Two modes, because struct p9mnt is one
 * static and a process therefore has one mount: `main' against the image the
 * section above built, `uid' against the purpose-built one with a 0600 file
 * and an owner V8 cannot represent.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
/*
 * The probe is a HOST binary, so it may use the host's socket calls directly --
 * the per-file raw-syscall rule is about shim/v8sys, not about a test harness.
 * deadpeer() needs shutdown(2) and getsockopt(2) to put a live descriptor into
 * the state a dead server would.
 */
#include <sys/socket.h>

#include "../../shim/v8sys/v8sys.h"
#include "../../shim/v8sys/vfs.h"

extern int  v8s_open(char *, int, int);
extern long v8s_read(int, char *, long);
extern long v8s_write(int, char *, long);
extern int  v8s_close(int);
extern long v8s_lseek(int, long, int);
extern int  v8s_stat(char *, struct v8_stat *);
extern int  v8s_fstat(int, struct v8_stat *);
extern int  v8s_dup(int);
extern int  v8s_dup2(int, int);
extern int  v8s_link(char *, char *);
extern int  v8s_symlink(char *, char *);
extern int  v8s_unlink(char *);

/*
 * `hello' is 27 bytes -- "hello from a V8 filesystem\n" -- written by the
 * section above through mkfs's proto.  Spelled here as a STRING rather than as
 * a length, so a case that reads at an offset asserts the bytes it landed on
 * and not merely that some bytes arrived.
 */
static const char HELLO[] = "hello from a V8 filesystem\n";

static char buf[8192];

/*
 * Print a run of bytes as a key's value.
 *
 * A NEWLINE RENDERS AS `.' AND A SPACE AS `_', so that no expected value in
 * run.sh is made of whitespace.  `hello ' and `hello' differ by a character
 * an editor strips and a reviewer cannot see -- which would make a case that
 * reads correctly assert something else, and the shell's $( ) keeps trailing
 * spaces so it would have passed here and failed after any reformatting.
 */
static void
pbytes(const char *key, const char *p, long n)
{
	long i;

	printf("%s ", key);
	for (i = 0; i < n; i++)
		putchar(p[i] == '\n' ? '.' : p[i] == ' ' ? '_' : p[i]);
	putchar('\n');
}

/*
 * 1. A DIRECTORY'S TWO SIZES, WHICH DELIBERATELY DISAGREE.
 *
 * dir.c:114's rule, in its second filesystem: what read(2) produces for a
 * directory descriptor is a run of V7 records built in THIS process, and the
 * number the thing underneath charges for the same directory is unrelated.
 * p9_t_stat reports the server's (the image's i_size); p9_t_fstat overrides it
 * with the snapshot's length.
 *
 * THE OBSERVABLE IS THAT THE TWO DIFFER, and nothing had ever asked both.  That
 * is why deleting the override left the suite green -- it collapses fstat onto
 * stat, and every existing reader loops to EOF and never looks.  Asserted as a
 * RELATION (fstat == bytes actually readable, and fstat != stat) rather than
 * against transcribed numbers, so the case does not encode this image's shape.
 */
static void
dirsizes(const char *tag, const char *dir)
{
	struct v8_stat sst, fst;
	long total, n;
	int fd;

	if (v8s_stat((char *)dir, &sst) < 0) {
		printf("%s-stat-failed 1\n", tag);
		return;
	}
	if ((fd = v8s_open((char *)dir, 0, 0)) < 0) {
		printf("%s-open-failed 1\n", tag);
		return;
	}
	if (v8s_fstat(fd, &fst) < 0) {
		printf("%s-fstat-failed 1\n", tag);
		v8s_close(fd);
		return;
	}

	total = 0;
	while ((n = v8s_read(fd, buf, (long)sizeof buf)) > 0)
		total += n;
	printf("%s-read-err %d\n", tag, n < 0 ? 1 : 0);
	v8s_close(fd);

	/*
	 * A MOUNT NEEDS ITS OWN DEVICE NUMBER.  st_ino here is the server's raw
	 * i_number and everything else in this world is a folded host inode, so
	 * with st_dev zero on both sides the two number spaces are one -- and a
	 * pipe is (0, 100), measured.  cat(1) refuses when an input's (dev, ino)
	 * equals its output's, so a file whose i_number happened to be 100 made
	 * `cat f' fail whenever stdout was a $( ).  This is the deterministic
	 * half of that guard: the value cases cannot see it, because whether the
	 * pair collides depends on which inode the image happened to allocate.
	 */
	printf("%s-stat-dev %ld\n", tag, (long)sst.st_dev);
	printf("%s-stat-size %ld\n", tag, (long)sst.st_size);
	printf("%s-fstat-size %ld\n", tag, (long)fst.st_size);
	printf("%s-read-total %ld\n", tag, total);
	printf("%s-fstat-is-readable-bytes %d\n", tag,
	    (long)fst.st_size == total ? 1 : 0);
	printf("%s-two-sizes-differ %d\n", tag,
	    (long)fst.st_size != (long)sst.st_size ? 1 : 0);
	/*
	 * ...and the readable total is a whole number of V7 records, which is
	 * what says the snapshot is a run of them rather than some other number
	 * that happens to differ.  V8_DIRSIZ is 254 here, so a record is 256.
	 */
	printf("%s-total-is-whole-records %d\n", tag,
	    total > 0 && total % (V8_DIRSIZ + 2) == 0 ? 1 : 0);
	/*
	 * THE TWO SIZES ARE THE SAME COUNT IN DIFFERENT UNITS, and saying so is
	 * what stops this being two transcribed numbers.  A V7 directory entry
	 * on the image is 16 bytes (2 for d_ino + DIRSIZ 14); a record read out
	 * of the snapshot is V8_DIRSIZ + 2.  So entries = stat/16 = total/256,
	 * and a case can assert the RATIO without knowing how many files the
	 * image happens to hold.
	 */
	printf("%s-entry-counts-agree %d\n", tag,
	    sst.st_size > 0 &&
	    (long)sst.st_size / 16 == total / (V8_DIRSIZ + 2) ? 1 : 0);
	printf("%s-entries %ld\n", tag, (long)sst.st_size / 16);
}

/*
 * 2. lseek, ALL THREE WHENCES, AND THE OVERFLOW GUARD.
 *
 * tail(1) sends Tseek (measured: two T128 messages) but only ever SEEK_END with
 * a non-positive offset.  A negative SEEK_CUR is legal lseek(2) and is the case
 * v8fsd's do_seek comment says produced its one remote crash -- a p9_u64 in a
 * signed comparison -- so it is the one to have.
 *
 * THE OVERFLOW GUARD IS THE INTERESTING HALF.  An auditor found do_seek
 * computing base + off and THEN testing for negative, which is signed overflow
 * -- undefined, and reachable from an ordinary lseek: 2^62 twice.  The fixed
 * form tests before adding.  A case for it has to distinguish "refused" from
 * "wrapped and then refused for the wrong reason", so the SEEK_SET that sets
 * the position up is asserted to SUCCEED first.
 */
static void
seeks(const char *file)
{
	long r;
	int fd;

	if ((fd = v8s_open((char *)file, 0, 0)) < 0) {
		printf("seek-open-failed 1\n");
		return;
	}

	/* SEEK_SET into the middle, and read what is there. */
	printf("seek-set-6 %ld\n", v8s_lseek(fd, 6, 0));
	r = v8s_read(fd, buf, 4);
	pbytes("seek-set-bytes", buf, r < 0 ? 0 : r);

	/* SEEK_CUR forward from where that read left the cursor (10). */
	printf("seek-cur-fwd %ld\n", v8s_lseek(fd, 2, 1));
	r = v8s_read(fd, buf, 1);
	pbytes("seek-cur-fwd-byte", buf, r < 0 ? 0 : r);

	/*
	 * SEEK_CUR BACKWARDS -- the arm nothing else in the tree reaches.  The
	 * read above left the cursor at 13; -13 is the start of the file.
	 */
	printf("seek-cur-back %ld\n", v8s_lseek(fd, -13, 1));
	r = v8s_read(fd, buf, 5);
	pbytes("seek-cur-back-bytes", buf, r < 0 ? 0 : r);

	/* SEEK_END, both forms. */
	printf("seek-end-0 %ld\n", v8s_lseek(fd, 0, 2));
	printf("seek-end-neg %ld\n", v8s_lseek(fd, -1, 2));
	r = v8s_read(fd, buf, 4);
	pbytes("seek-end-neg-byte", buf, r < 0 ? 0 : r);
	/* ...and a read at end of file is 0 rather than an error. */
	v8s_lseek(fd, 0, 2);
	printf("seek-read-at-eof %ld\n", v8s_read(fd, buf, 4));

	/* Past the end is legal and reads nothing; V7 files have holes. */
	printf("seek-past-end %ld\n", v8s_lseek(fd, 1000, 0));
	printf("seek-read-past-end %ld\n", v8s_read(fd, buf, 4));

	/* A negative RESULT is EINVAL, which is lseek(2)'s own answer. */
	v8s_lseek(fd, 0, 0);
	v8_errno = 0;
	printf("seek-negative %ld\n", v8s_lseek(fd, -1, 0));
	printf("seek-negative-errno %d\n", v8_errno);

	/* An unknown whence never leaves this process. */
	v8_errno = 0;
	printf("seek-bad-whence %ld\n", v8s_lseek(fd, 0, 7));
	printf("seek-bad-whence-errno %d\n", v8_errno);

	/*
	 * THE OVERFLOW GUARD.  2^62 twice: the first must succeed (it is a
	 * legal, if absurd, position) and the second must be refused, because
	 * 2^63 is not representable.  A guard written after the addition
	 * rejects it too -- for the wrong reason, having already executed
	 * undefined behaviour -- so the pair is what makes the case meaningful:
	 * the SET succeeds, the CUR is refused.
	 */
	v8_errno = 0;
	printf("seek-huge-set %ld\n", v8s_lseek(fd, 1L << 62, 0));
	v8_errno = 0;
	printf("seek-huge-cur %ld\n", v8s_lseek(fd, 1L << 62, 1));
	printf("seek-huge-cur-errno %d\n", v8_errno);
	/*
	 * ...and the fid is still usable afterwards, which says the guard
	 * refused the operation rather than leaving the offset somewhere
	 * unspeakable.  (do_seek assigns f_off only after the check.)
	 */
	printf("seek-after-refusal %ld\n", v8s_lseek(fd, 0, 0));
	r = v8s_read(fd, buf, 5);
	pbytes("seek-after-refusal-bytes", buf, r < 0 ? 0 : r);

	v8s_close(fd);
}

/*
 * 3. dup SHARES ONE OFFSET, WHICH IS THE DESIGN.
 *
 * p9cl.c's header comment is one sentence -- the connection IS the open file
 * description -- and the three consequences it names are dup, fork and a
 * program replacing its image.  The last of the three has an end-to-end case
 * (sh redirecting into cat); the first two follow from the offset living on the
 * SERVER, and nothing asserted it.
 *
 * A shell cannot express this cleanly.  `{ head -c6; head -c4; } < /mnt/hello'
 * makes each program a separate stdio consumer that reads a whole block, so the
 * second sees end of file no matter where the offset really is.  Two raw
 * read(2)s can just ask.
 *
 * THE CLOSE IS THE OTHER HALF, and it is the bug this file already made once: a
 * Tclunk in t_close destroys a fid every dup shares, so `cat < /mnt/hello'
 * printed nothing.  The right number of clunks is zero and the kernel drops the
 * connection at the LAST close.  Asserted by closing one and reading the other.
 */
static void
dups(const char *file)
{
	long r;
	int fd, fd2, fd3;

	if ((fd = v8s_open((char *)file, 0, 0)) < 0) {
		printf("dup-open-failed 1\n");
		return;
	}

	r = v8s_read(fd, buf, 6);
	pbytes("dup-first-read", buf, r < 0 ? 0 : r);

	if ((fd2 = v8s_dup(fd)) < 0) {
		printf("dup-failed 1\n");
		v8s_close(fd);
		return;
	}
	printf("dup-is-a-new-fd %d\n", fd2 != fd ? 1 : 0);

	/* THE CLAIM: the dup continues where the original stopped. */
	r = v8s_read(fd2, buf, 4);
	pbytes("dup-continues", buf, r < 0 ? 0 : r);

	/* ...and it runs the other way too -- the original sees the dup's move. */
	r = v8s_read(fd, buf, 2);
	pbytes("dup-original-sees-it", buf, r < 0 ? 0 : r);

	/* An lseek on one is an lseek on both, for the same reason. */
	v8s_lseek(fd2, 0, 0);
	r = v8s_read(fd, buf, 5);
	pbytes("dup-seek-is-shared", buf, r < 0 ? 0 : r);

	/*
	 * CLOSING ONE MUST NOT CLUNK THE FILE.  This is the case for the bug
	 * that made shell redirection print nothing, stated directly rather
	 * than through sh.
	 */
	v8s_close(fd);
	r = v8s_read(fd2, buf, 4);
	pbytes("dup-survives-sibling-close", buf, r < 0 ? 0 : r);

	/* dup2 carries the type across as well -- v8s_dup2 binds it. */
	fd3 = 20;
	if (v8s_dup2(fd2, fd3) < 0) {
		printf("dup2-failed 1\n");
		v8s_close(fd2);
		return;
	}
	v8s_lseek(fd3, 6, 0);
	r = v8s_read(fd2, buf, 4);
	pbytes("dup2-shares-offset-too", buf, r < 0 ? 0 : r);

	v8s_close(fd3);
	v8s_close(fd2);
}

/*
 * 4. THE ERRNO SEAM, from the client's side.
 *
 * v8fsd's errnames[] turns an errno into a symbolic name and p9cl.c's enames[]
 * turns it back.  run.sh compares the two TABLES as text; these are the values
 * that actually cross the wire, which is the half a table comparison cannot see
 * -- a name both tables hold is still wrong if the server never sends it for
 * this condition or the client stores it in the wrong place.
 */
static void
errnos(const char *dir, const char *file)
{
	char path[256];
	int fd;

	/*
	 * THE PAIR THAT V7 DISTINGUISHES AND A SHORT Rwalk DOES NOT.  namei
	 * answers ENOENT for a name that is absent from a real directory and
	 * ENOTDIR for one looked up in something that is not a directory --
	 * v8fsd.c:1136 is the second arm.  A short Rwalk carries no errno, so
	 * the client reconstructs it from the last qid; both halves are here
	 * because a client that simply always said ENOTDIR would pass the
	 * second case alone.
	 */
	snprintf(path, sizeof path, "%s/no-such-file", dir);
	v8_errno = 0;
	printf("err-open-missing %d\n", v8s_open(path, 0, 0));
	printf("err-open-missing-errno %d\n", v8_errno);

	/* ...absent from a real SUBdirectory, which is a short walk and not an
	 * Rerror -- the first name succeeds.  This is the arm that shares its
	 * code path with the ENOTDIR one below. */
	snprintf(path, sizeof path, "%s/sub/no-such-file", dir);
	v8_errno = 0;
	printf("err-open-missing-deep %d\n", v8s_open(path, 0, 0));
	printf("err-open-missing-deep-errno %d\n", v8_errno);

	/* ...and through a plain file, which is ENOTDIR. */
	snprintf(path, sizeof path, "%s/beyond", file);
	v8_errno = 0;
	printf("err-walk-thru-file %d\n", v8s_open(path, 0, 0));
	printf("err-walk-thru-file-errno %d\n", v8_errno);

	/*
	 * WRITING, WHICH THE SERVER REFUSES SOMEWHERE.  p9_t_write SENDS the
	 * Twrite rather than refusing locally, so the errno is the server's
	 * answer about the file and not this process's guess -- which is what
	 * §8a step 5f will change without touching the client.  Today the
	 * refusal comes at the OPEN, so which of the two spoke is reported
	 * rather than assumed: `-1' for the open means it never got that far.
	 */
	v8_errno = 0;
	if ((fd = v8s_open((char *)file, 1, 0)) >= 0) {
		printf("err-open-for-write 0\n");
		v8_errno = 0;
		printf("err-write %ld\n", v8s_write(fd, (char *)"x", 1L));
		printf("err-write-errno %d\n", v8_errno);
		v8s_close(fd);
	} else {
		printf("err-open-for-write %d\n", v8_errno);
		printf("err-write -1\n");
		printf("err-write-errno %d\n", v8_errno);
	}

	/* EISDIR, and this one never leaves the process: a dirfd is not writable. */
	if ((fd = v8s_open((char *)dir, 0, 0)) >= 0) {
		v8_errno = 0;
		printf("err-write-dir %ld\n", v8s_write(fd, (char *)"x", 1L));
		printf("err-write-dir-errno %d\n", v8_errno);
		v8s_close(fd);
	}
}

/*
 * 5. A DEAD SERVER IS AN I/O ERROR, NOT A SIGNAL.
 *
 * The transport is a socket and the caller is a V7 program that does not know
 * it.  Writing a request to a peer that has gone away raises SIGPIPE, whose
 * default disposition is to terminate -- so before p9dial set SO_NOSIGPIPE, a
 * v8fsd that died mid-conversation killed the program with signal 13 instead of
 * failing its read.  Found here: the sanitized server aborts on a deliberately
 * broken guard, and the probe came back 141 = 128 + SIGPIPE.
 *
 * THE CONDITION IS PRODUCED WITHOUT KILLING ANYTHING, which is what makes this
 * a case rather than an anecdote.  shutdown(2) on the client's OWN socket puts
 * it in exactly the state a dead peer would: the next write gets EPIPE, and
 * whether that is also a signal is the whole question.  No second server, no
 * timing, no process to kill.
 *
 * Both halves are reported.  The option being set is the mechanism; the read
 * returning rather than the process dying is the property -- and the property
 * is what run.sh really checks, because the probe's exit status is 0 only if it
 * reached the end.
 */
static void
deadpeer(const char *file)
{
	int fd, on = -1;
	socklen_t len = sizeof on;
	long r;

	if ((fd = v8s_open((char *)file, 0, 0)) < 0) {
		printf("pipe-open-failed 1\n");
		return;
	}
	if (getsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, &len) < 0) on = -2;
	printf("pipe-nosigpipe-set %d\n", on);

	/*
	 * A read first, to show the descriptor was working -- otherwise a
	 * failure below is consistent with the open having produced something
	 * unusable.
	 */
	r = v8s_read(fd, buf, 5);
	pbytes("pipe-read-before", buf, r < 0 ? 0 : r);

	shutdown(fd, SHUT_RDWR);
	v8_errno = 0;
	r = v8s_read(fd, buf, 5);
	printf("pipe-read-after %ld\n", r);
	printf("pipe-read-after-errno %d\n", v8_errno);
	/* Reaching this line at all is the assertion; 141 never gets here. */
	printf("pipe-survived 1\n");
	v8s_close(fd);
}

/*
 * 6. THE OWNER CONTRACT, DIRECTLY.
 *
 * CLAUDE.md's rule for every 16-bit narrowing in this port is two properties
 * rather than a formula: root maps to root, and non-root NEVER maps to root.
 * The section above asserts both through `ls -l', which reads a login name out
 * of /etc/passwd -- so what it really tests is uid 0 versus not-0.  Here the
 * number itself is available.
 *
 * P9UID_BAD is (short)-1, i.e. 65535 unsigned: not a uid any V8 system issues,
 * and it prints as itself.  di_uid is v8_i16 and SIGNED, so an image built with
 * owner 40000 loads as -25536 and v8fsd renders it "-25536"; the '-' is a
 * non-digit and the parser must produce the sentinel, not 0.
 */
static void
owners(const char *root)
{
	struct v8_stat st;
	char path[256];

	snprintf(path, sizeof path, "%s/secret", root);
	if (v8s_stat(path, &st) == 0) {
		printf("uid-root-file %d\n", (int)st.st_uid);
		printf("uid-root-mode %o\n", (unsigned)(st.st_mode & 07777));
	} else
		printf("uid-root-file -2\n");

	snprintf(path, sizeof path, "%s/wide", root);
	if (v8s_stat(path, &st) == 0) {
		printf("uid-wide-file %d\n", (int)(unsigned short)st.st_uid);
		printf("uid-wide-is-not-root %d\n", st.st_uid != 0 ? 1 : 0);
	} else
		printf("uid-wide-file -2\n");

	/*
	 * ...and the 0600 file opens, because the server takes fio.c's root
	 * bypass.  That is the fact v8s_access was rewritten to report rather
	 * than to recompute, and here it is asked of open(2) itself.
	 */
	snprintf(path, sizeof path, "%s/secret", root);
	{
		int fd = v8s_open(path, 0, 0);
		long r = fd < 0 ? -1 : v8s_read(fd, buf, 6);

		printf("uid-0600-opens %d\n", fd >= 0 ? 1 : 0);
		pbytes("uid-0600-bytes", buf, r < 0 ? 0 : r);
		if (fd >= 0) v8s_close(fd);
	}
}

/*
 * 7. LINK -- §8a step 5g, AND THE PROBE IS THE ONLY INSTRUMENT FOR MOST OF IT.
 *
 * Three of these cases cannot be reached from any binary in the rootfs.
 * ln(1) REFUSES to hard-link a directory before it ever calls link(2)
 * (ln.c's linkit stats first), which is exactly the case mv(1) depends on;
 * `.' and `..' as a new name are rejected by nothing on the way in; and an
 * errno is what the suite has to assert, where a shell only sees exit 1.
 *
 * THE MOUNT IS WRITABLE, so this mode gets its own image and its own server.
 * Every other mode here runs against the read-only one, and running these
 * there would assert EROFS eight times.
 */
static void
links(const char *m)
{
	char a[256], b[256], c[256];
	struct v8_stat sa, sb;
	int r;

#define P(dst, tail) do { \
		size_t _i = 0, _j = 0; \
		while (m[_i]) { dst[_i] = m[_i]; _i++; } \
		while ((tail)[_j]) dst[_i + _j] = (tail)[_j], _j++; \
		dst[_i + _j] = '\0'; \
	} while (0)

	/* A second name for a plain file, and it is the SAME inode. */
	P(a, "/hello"); P(b, "/hello2");
	v8_errno = 0;
	r = v8s_link(a, b);
	printf("link-file %d\n", r);
	printf("link-file-errno %d\n", v8_errno);
	if (v8s_stat(a, &sa) == 0 && v8s_stat(b, &sb) == 0) {
		printf("link-same-ino %d\n", sa.st_ino == sb.st_ino ? 1 : 0);
		printf("link-nlink %d\n", (int)sb.st_nlink);
	}

	/* An existing name is EEXIST -- nami.c's NI_LINK arm never runs. */
	v8_errno = 0;
	printf("link-eexist %d\n", v8s_link(a, b));
	printf("link-eexist-errno %d\n", v8_errno);

	/*
	 * A DIRECTORY, which is the case mv(1) needs and ln(1) will not make.
	 * Upstream allows it for the superuser only (sys2.c:471) and u_uid is
	 * 0 here, so it must SUCCEED -- a server that refused would leave
	 * mvdir with no way to rename a directory at all.
	 *
	 * `lsrc' IS MADE BY THE SUITE, not reused from the image.  An earlier
	 * draft linked and then unlinked /w/sub, which is the image's own
	 * subdirectory -- and putting it back afterwards is impossible from a
	 * shell, because ln(1) refuses a directory, which is the very fact
	 * this case exists to work around.  A probe that cannot undo what it
	 * did is the shared-artefact hazard this suite has already been bitten
	 * by three times.
	 */
	P(a, "/lsrc"); P(b, "/lsrc2");
	v8_errno = 0;
	printf("link-dir %d\n", v8s_link(a, b));
	printf("link-dir-errno %d\n", v8_errno);

	/*
	 * ...AND UNLINKING ONE OF THE TWO NAMES MUST NOT DESTROY IT.  This is
	 * the whole of the Tunlink argument in one pair: at nlink 3 the server
	 * must use NI_DEL, so `sub' goes and `sub2' still reads.  With the old
	 * inode-sniffing Tremove this was EBUSY, and worse, nami.c:361 had
	 * already decremented the parent's link count on the way to the error.
	 */
	v8_errno = 0;
	printf("unlink-linked-dir %d\n", v8s_unlink(a));
	printf("unlink-linked-dir-errno %d\n", v8_errno);
	P(c, "/lsrc2/inner");
	printf("survivor-readable %d\n", v8s_stat(c, &sa) == 0 ? 1 : 0);

	/* `.' and `..' as the NEW name -- refused before the wire and on it. */
	P(a, "/hello"); P(b, "/lsrc2/..");
	v8_errno = 0;
	printf("link-dotdot %d\n", v8s_link(a, b));
	printf("link-dotdot-errno %d\n", v8_errno);

	/* A new name inside a plain file is ENOTDIR, not ENOENT. */
	P(b, "/hello/under");
	v8_errno = 0;
	printf("link-notdir %d\n", v8s_link(a, b));
	printf("link-notdir-errno %d\n", v8_errno);

	/*
	 * CROSS-TYPE IS EXDEV, and it is an ANSWER rather than a refusal --
	 * two filesystems really are two devices.  /etc is passthrough, so
	 * this pair spans the switch.
	 */
	v8_errno = 0;
	printf("link-xdev %d\n", v8s_link(a, (char *)"/etc/xlink"));
	printf("link-xdev-errno %d\n", v8_errno);

	/*
	 * SYMLINK IS EPERM AND NOT EROFS.  The image has taken four writes by
	 * now, so EROFS would be a measurably false statement about it;
	 * EPERM says the operation is meaningless, which is permanent.
	 */
	P(b, "/slink");
	v8_errno = 0;
	printf("symlink-refused %d\n", v8s_symlink(a, b));
	printf("symlink-errno %d\n", v8_errno);
#undef P
}

int
main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: p9clprobe main|uid|link\n");
		return (2);
	}

	if (strcmp(argv[1], "main") == 0) {
		/*
		 * FIRST, THAT THIS IS THE IMAGE THE CASES BELOW ASSUME.  Every
		 * offset in seeks() and dups() is an index into HELLO, so a
		 * probe run against a different image would report a wall of
		 * plausible wrong bytes rather than one clear failure.  This is
		 * the same reason fsprobe compares its readback with cmp.
		 */
		{
			int fd = v8s_open((char *)"/mnt/hello", 0, 0);
			long r = fd < 0 ? -1 : v8s_read(fd, buf, (long)sizeof buf);

			printf("hello-len %ld\n", r);
			printf("hello-is-expected %d\n",
			    r == (long)(sizeof HELLO - 1) &&
			    memcmp(buf, HELLO, (size_t)r) == 0 ? 1 : 0);
			if (fd >= 0) v8s_close(fd);
		}
		/*
		 * TWO DIRECTORIES, because one cannot show that the pair of
		 * sizes tracks the directory rather than being two constants.
		 * The root has four entries and sub has three, so both numbers
		 * move and the ratio does not.
		 */
		dirsizes("dir", "/mnt");
		dirsizes("subdir", "/mnt/sub");
		seeks("/mnt/hello");
		dups("/mnt/hello");
		errnos("/mnt", "/mnt/hello");
		deadpeer("/mnt/hello");
	} else if (strcmp(argv[1], "uid") == 0) {
		owners("/m");
	} else if (strcmp(argv[1], "link") == 0) {
		links("/w");
	} else {
		fprintf(stderr, "p9clprobe: unknown mode %s\n", argv[1]);
		return (2);
	}
	return (0);
}
