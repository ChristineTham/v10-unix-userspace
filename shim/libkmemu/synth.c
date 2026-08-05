/*
 * libkmemu -- the system-facts half of the shim.
 *
 * WHAT IT IS FOR.  Four V8 commands do not compute anything; they report what
 * the kernel knows.  `who' reads /etc/utmp, `df' reads /etc/mtab and then the
 * superblock of each device, `w' and `load' grovel /dev/kmem through a
 * namelist.  None of those exist here, because nothing here is a VAX kernel --
 * so without something standing in for one, this port has four commands that
 * build, link, run, and print nothing.
 *
 * THE SANCTIONED EXCEPTION, AND WHERE ITS EDGE IS.  This library, and only this
 * library, may link the host's libc, to call the documented interfaces that
 * answer what is running and who is logged in: getutxent(3), getfsstat(2),
 * proc_listpids/proc_pidinfo, sysctl(3).  PLAN.md section 7 records the
 * decision and CLAUDE.md carries it on the same list as the as/ld exception.
 *
 * The reason it is justified where the obvious alternative is not: the syscall
 * interface is stable and documented, and /var/run/utmpx's on-disk layout is
 * neither -- measured here as 628-byte records behind a "utmpx-1.00" signature,
 * a size matching no documented struct.  Reverse-engineering that by arithmetic
 * is the shape that cost four wrong attempts on spell's huff format, and a
 * wrong guess yields a `who' that looks right and lies.  Reaching for libc here
 * NARROWS what this port depends on.
 *
 * The exception is for READING FACTS ABOUT THE RUNNING SYSTEM and nothing else.
 * It is drawn per-file, not per-library: everything in shim/v8sys/ stays
 * raw-syscall-only, and so does everything below in THIS file.  Turning those
 * facts into a file is ordinary I/O, which rawsys.h already covers, so it goes
 * through rawsys like the rest of the shim.  Keeping the line inside libkmemu
 * rather than at its edge is what stops the exception from quietly widening
 * into "libkmemu may use libc" -- which is how an exception list stops meaning
 * anything.  utmp.c is where libc actually appears, and it says so at its top.
 *
 * WHY A FILE AND NOT A FUNCTION CALL.  who(1) does fopen("/etc/utmp"); so does
 * w(1).  Manufacturing the file they already read costs those programs no
 * source change at all, where a libkmemu call would cost one deviation per
 * program, each recorded and each a small lie about what the authentic source
 * does.  It is also what the real system did: /etc/utmp was an ordinary file
 * kept current by init and login, both of which live on the kernel's side of
 * this seam.  The shim is that side.  We just do the bookkeeping lazily, when
 * a reader opens the file, instead of eagerly when a session begins.
 */

#include "kmemu.h"
#include "../v8sys/rawsys.h"

static long
kmlen(const char *s)
{
	long n = 0;

	while (s[n]) n++;
	return (n);
}

static int
kmsame(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (*a == *b);
}

/*
 * Copy one V7 fixed-width field.  V7 does NOT require a terminator: a name that
 * exactly fills the field has no NUL after it, and every reader in the tree
 * uses %-8.8s or strncmp with sizeof, so it must not get one.  Short names are
 * zero-filled, which is what `who' tests for an empty slot.
 *
 * Anything longer is truncated, and that is a real loss with a real precedent:
 * dir.c does the same to names over 14 characters.  It is also the authentic
 * limit rather than an artefact of this port -- a V8 program could not have
 * seen a longer login or tty name either.
 */
void
kmemu_field(char *dst, long dlen, const char *src, long slen)
{
	long i, n = slen < dlen ? slen : dlen;

	for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
	for (; i < dlen; i++) dst[i] = '\0';
}

/*
 * Write `hostpath' with n bytes, atomically.
 *
 * Through a temporary and rename(2), because the reader that triggered this is
 * about to open the same path: a half-written file would be read as a short
 * record stream, which is indistinguishable from "nobody is logged in".  A
 * silent wrong answer, and the exact failure mode this library exists to avoid.
 */
int
kmemu_replace(const char *hostpath, const char *buf, long n)
{
	char tmp[1024];
	long i, len = kmlen(hostpath);
	long fd, w;

	if (len + 5 >= (long)sizeof tmp) return (-1);
	for (i = 0; i < len; i++) tmp[i] = hostpath[i];
	tmp[i++] = '.';
	tmp[i++] = 'n';
	tmp[i++] = 'e';
	tmp[i++] = 'w';
	tmp[i] = '\0';

	fd = rawsys3(SYS_open, (long)tmp,
	    0x0001 /*O_WRONLY*/ | 0x0200 /*O_CREAT*/ | 0x0400 /*O_TRUNC*/, 0644);
	if (fd < 0) return (-1);
	for (i = 0; i < n; i += w) {
		w = rawsys3(SYS_write, fd, (long)(buf + i), n - i);
		if (w <= 0) { rawsys1(SYS_close, fd); rawsys1(SYS_unlink, (long)tmp); return (-1); }
	}
	rawsys1(SYS_close, fd);
	if (rawsys2(SYS_rename, (long)tmp, (long)hostpath) < 0) {
		rawsys1(SYS_unlink, (long)tmp);
		return (-1);
	}
	return (0);
}

/*
 * The table of files this library manufactures.  df's /etc/mtab is the next
 * one; it lands here beside utmp rather than as a second hook in the shim.
 */
static struct {
	const char *path;
	int (*make)(const char *);
} synth[] = {
	{ "/etc/utmp", kmemu_utmp },
	{ 0, 0 }
};

int
kmemu_synth(const char *v8path, const char *root)
{
	char host[1024];
	long i, n = 0;
	int k;

	if (v8path == 0 || root == 0 || *root == '\0') return (0);
	for (k = 0; synth[k].path; k++)
		if (kmsame(v8path, synth[k].path)) break;
	if (synth[k].path == 0) return (0);

	for (i = 0; root[i] && n < (long)sizeof host - 1; i++) host[n++] = root[i];
	for (i = 0; v8path[i] && n < (long)sizeof host - 1; i++) host[n++] = v8path[i];
	host[n] = '\0';

	/*
	 * A failure here is deliberately quiet.  The rootfs can be read-only,
	 * or /etc may not exist yet in a half-built tree, and neither is a
	 * reason for open(2) to fail -- the caller then sees exactly what it
	 * would have seen without this library, which is a missing file.
	 */
	return (synth[k].make(host) == 0);
}
