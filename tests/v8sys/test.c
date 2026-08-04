/*
 * Behaviour tests for libv8sys, written in modern C and linked against the
 * shim directly.  These do not go through v8cc: the point is to check that the
 * seam itself behaves the way V8 expects, before any V8 code depends on it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../../shim/v8sys/v8sys.h"

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
extern int v8sys_signo_to_host(int);
extern int v8sys_signo_from_host(int);
extern int v8sys_errno(int);

static int pass, fail;

static void
ok(int cond, const char *what)
{
	if (cond) pass++;
	else { fail++; printf("FAIL %s\n", what); }
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

	/* --------------------------------------------------------- sbrk */
	b0 = v8s_sbrk(0);
	ok(b0 != (char *)-1, "sbrk(0) probes the break");
	b1 = v8s_sbrk(4096);
	ok(b1 == b0, "sbrk returns the OLD break, as malloc expects");
	ok(v8s_sbrk(0) == b0 + 4096, "the break moved");
	memset(b0, 0xa5, 4096);		/* must not fault: it is committed */
	ok(*(unsigned char *)b0 == 0xa5, "memory below the break is writable");
	ok(v8s_sbrk(-4096) == b0 + 4096, "the break can move back");

	/* ------------------------------------------------------- cleanup */
	snprintf(sub, sizeof sub, "%s/a", tmpl); v8s_unlink(sub);
	snprintf(sub, sizeof sub, "%s/bb", tmpl); v8s_unlink(sub);
	snprintf(sub, sizeof sub, "%s/a-very-long-name-indeed", tmpl); v8s_unlink(sub);
	snprintf(sub, sizeof sub, "%s/file", tmpl); v8s_unlink(sub);
	ok(v8s_rmdir(tmpl) == 0, "rmdir");

	printf("v8sys: %d passed, %d failed\n", pass, fail);
	return (fail != 0);
}
