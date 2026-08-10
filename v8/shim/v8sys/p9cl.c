/*
 * The v8fs CLIENT -- the fourth filesystem type, and the half of §8a step 5e
 * that makes the server something other than a thing a probe talks to.
 *
 * A V8 program's open("/mnt/hello") lands here, this file connects to
 * shim/v8fsd/v8fsd.c over a Unix socket, speaks 9P2000, and hands the program
 * back a descriptor it can read.  Bell Labs' namei, iget, bmap and readi are
 * what answer, in another process, over a disk image.
 *
 * ------------------------------------------------------- THE SHAPE, AND WHY
 *
 * ONE CONNECTION PER open(2).  Not one per process, and the difference is the
 * whole design rather than a tuning choice.
 *
 * A file offset in this process's memory is wrong three ways at once, all of
 * them ordinary Unix -- dup(2) shares one offset between two descriptors,
 * fork(2) shares it between two processes, and a program replacing itself
 * keeps it while every table in its address space is destroyed.  What the
 * three have in common is that the offset belongs to the OPEN FILE
 * DESCRIPTION, the `struct file', which is exactly the object a connection is
 * here: a socket is shared by dup, shared by fork, and survives the image
 * being replaced.  So the offset lives on the far side (p9.h's extension
 * note), the fid is a CONSTANT because a connection carries exactly one open
 * file, and THIS FILE HOLDS NO PER-DESCRIPTOR STATE AT ALL for regular files.
 *
 * The consequence worth stating plainly: an inherited v8fs descriptor is fully
 * usable by a program that knows nothing about it.  `sh' opening a file on a
 * mount, dup2'ing it onto 0 and running `cat' works, and works because there
 * is nothing to inherit -- not because anything was arranged to be inherited.
 *
 * WHAT IT COSTS.  A round trip for Tversion and one for Tattach on every open,
 * plus a host socket on each side per open file.  Measured against the
 * alternative rather than assumed: a shared connection would need a fid
 * allocator, a tag allocator, and a way for a program that has just replaced
 * its own image to discover which fid its descriptor refers to -- and there is
 * no such way, because the only thing that crossed is the descriptor itself.
 *
 * A DIRECTORY IS THE ONE EXCEPTION and it is snapshotted client-side, through
 * dir.c's existing machinery -- see v8sys_diradopt there for why that inherits
 * a limit this shim has had since it was written rather than inventing one.
 *
 * Raw syscalls only, like the rest of shim/v8sys.  The headers included below
 * are headers of constants and layouts -- AF_UNIX, SOCK_STREAM, EINTR and
 * `struct sockaddr_un' -- and name no symbol; rawsys.h says why that rule
 * exists.
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#include "v8sys.h"
#include "vfs.h"
#include "rawsys.h"
#include "../p9/p9.h"
#include "../v8id.h"		/* v8_foldid -- the narrowing rule, shared */

extern int v8_errno;
extern char *v8sys_getenv(const char *name);
extern char *v8sys_alloc(long);
extern void  v8sys_free(char *);
extern int   v8sys_isdirfd(int fd);
extern long  v8sys_dirsize(int fd);
extern long  v8sys_dirread(int fd, void *buf, long n);
extern long  v8sys_dirseek(int fd, long off, int whence);
extern void  v8sys_dirclose(int fd);
extern int   v8sys_diradopt(int fd, char *recs, long nbytes);

/* ------------------------------------------------------------ the mount */

/*
 * WHERE A MOUNT IS CONFIGURED, and the answer is forced rather than chosen.
 *
 * The jail is PER-BINARY -- shim/v8sys/syscall.c's rootpath() is a chroot in
 * process memory, not in the kernel -- so there is no system-wide namespace to
 * record a mount in.  A `mount' command would put a row in its own address
 * space and then exit, leaving nothing behind.  What DOES cross a program
 * boundary is the environment, and the server is already the thing that has to
 * exist for any of this to work, so the environment names it and the server is
 * the registry.
 *
 *	V8MOUNT=/mnt=/tmp/v8fs.sock
 *
 * One mount today.  A second would be a colon-separated list and one more row
 * in the table below; it is not written because nothing has two servers, and
 * an unexercised rule cannot be seen to be incomplete.
 *
 * THE SOCKET PATH IS NOT FREE, which is a constraint the server found first:
 * sun_path is 104 bytes on Darwin and a Mac's $TMPDIR is around 50 of them
 * before anything is appended.  A path that will not fit is refused HERE, at
 * parse time, with the mount simply not existing -- rather than at connect
 * time on every open, where it would present as ENOENT on the file.
 */
/*
 * A MOUNT POINT IS A PATH, so this is a path-shaped number and not a name-
 * shaped one.  It was 64 and that was a guess; it became load-bearing the
 * moment a test wanted to mount somewhere the HOST also has a directory, to
 * prove the guards below contain rather than merely fail -- and $TMPDIR alone
 * is around 50 characters on a Mac, so 64 refused the only mount point that
 * could make that case mean anything.  Cheap: one static struct per process.
 */
#define P9MNT_PFX	256

static struct p9mnt {
	int	m_state;		/* 0 unread, 1 valid, -1 none */
	char	m_pfx[P9MNT_PFX];	/* "/mnt", never with a trailing slash */
	int	m_pfxlen;
	char	m_sock[sizeof(((struct sockaddr_un *)0)->sun_path)];
} mnt;

static struct p9mnt *
p9mount(void)
{
	char *e;
	int i, j, eq;

	if (mnt.m_state) return (mnt.m_state > 0 ? &mnt : 0);
	mnt.m_state = -1;

	if ((e = v8sys_getenv("V8MOUNT")) == 0) return (0);

	/* prefix, up to the '=' */
	for (i = 0; e[i] && e[i] != '='; i++) {
		if (i >= P9MNT_PFX - 1) return (0);
		mnt.m_pfx[i] = e[i];
	}
	if (e[i] != '=' || i == 0 || mnt.m_pfx[0] != '/') return (0);
	/*
	 * A BARE "/" IS REFUSED, and vfs.c used to carry a comment saying it
	 * would "shadow /bin and the whole world with it".  Measured: it does
	 * not.  p9rel below requires p[pfxlen] == '/' for anything longer than
	 * the prefix, and with pfxlen 1 that character is already the first
	 * letter of the name -- so mounting on the root claims exactly the one
	 * path "/" and nothing beneath it.  The result is an incoherent
	 * half-mount, "/" from the server and "/bin" from the jail, which is
	 * worse than the foot-gun the comment warned about and was described
	 * nowhere.  Refusing it is one line; making it work is a namespace.
	 */
	if (i == 1) return (0);
	/*
	 * A TRAILING SLASH IS STRIPPED so that the prefix is one spelling.
	 * vfs.c's static table carries the slash and pays for it in a special
	 * case at the bottom of its match loop -- "/etc" and "/etc/" naming two
	 * different worlds is a bug that file records having had.  Storing the
	 * bare name and testing the boundary explicitly below says it once.
	 */
	eq = i;					/* where the '=' actually is */
	while (i > 1 && mnt.m_pfx[i - 1] == '/') i--;
	mnt.m_pfx[i] = '\0';
	mnt.m_pfxlen = i;
	/*
	 * `eq' AND NOT `i', BECAUSE THE STRIP LOOP MOVED i.  The scan below
	 * used to resume at i+1, which is the '=' only when nothing was
	 * stripped -- so V8MOUNT=/mnt/=sock took the socket path as "=sock" and
	 * V8MOUNT=/mnt//=sock as "/=sock".  Verified by prediction: a server
	 * bound to a socket literally named `=sock' was reachable through the
	 * first spelling.  The two-slash case is the one a person hits, and it
	 * fails as a mount that silently does not exist -- which is exactly the
	 * mode the comment above says was moved to parse time to avoid.  Found
	 * by the lp64-auditor, in code written the same hour.
	 */

	for (j = 0, i = eq + 1; e[i]; i++, j++) {
		if (j >= (int)sizeof mnt.m_sock - 1) return (0);
		mnt.m_sock[j] = e[i];
	}
	mnt.m_sock[j] = '\0';
	if (j == 0) return (0);

	mnt.m_state = 1;
	return (&mnt);
}

/*
 * Does this path fall inside the mount?  Returns the mount-relative remainder
 * -- "sub/deep/hello", with no leading slash, and "" for the mount point
 * itself -- or 0.
 *
 * THE BOUNDARY IS A CHARACTER AND NOT A LENGTH.  "/mnt" and "/mnt/x" are in;
 * "/mntfoo" is not, and testing only the prefix length would claim it.  That
 * is the same trap vfs.c's table avoids by carrying a trailing slash, arriving
 * here where the slash has been stripped.
 */
static const char *
p9rel(struct p9mnt *m, const char *p)
{
	int i;

	for (i = 0; i < m->m_pfxlen; i++)
		if (p[i] != m->m_pfx[i]) return (0);
	if (p[i] == '\0') return (p + i);		/* the mount point */
	if (p[i] != '/') return (0);			/* "/mntfoo" */
	while (p[i] == '/') i++;			/* "/mnt//x" */
	return (p + i);
}

/* vfs.c asks this; it is the only thing outside this file that needs it. */
struct v8fstyp *
v8fs_p9for(const char *p)
{
	struct p9mnt *m = p9mount();

	if (m == 0 || p == 0 || *p != '/') return (0);
	return (p9rel(m, p) ? &v8fs_p9 : 0);
}

/*
 * The same question, for the syscalls that have no slot in struct v8fstyp.
 * A separate name rather than a null test on the one above, because the two
 * callers want different things and conflating them is how a guard ends up
 * being read as a dispatch.
 */
int
v8fs_mounted(const char *p)
{
	return (v8fs_p9for(p) != 0);
}

/* -------------------------------------------------------- the transaction */

/*
 * ONE BUFFER, NOT TWO.  A request is sent in full before its reply is read, so
 * the two never overlap, and this object is in the bss of every V8 binary in
 * the world -- dir.c's hostbuf makes the same trade and records the reason a
 * stack buffer is not available (an 8 KB frame makes clang emit a call to
 * ___chkstk_darwin, which is a libc symbol).
 *
 * IT IS NOT RE-ENTRANT, and that is inherited rather than new: a signal
 * handler that reads a directory already re-enters dir.c's buffer the same
 * way.  Said out loud because a v8fs read is a round trip and therefore a much
 * wider window than a getdirentries.
 */
static unsigned char msgbuf[P9_MSIZE];

#define P9CL_TAG	1

/*
 * The fids.  Constants, because a connection carries exactly one open file --
 * see the header comment.  ROOT is what Tattach returns and FILE is the clone
 * the walk lands on; both are known to any process holding the descriptor,
 * which is what makes an inherited one usable with no inherited state.
 */
#define P9CL_ROOTFID	0
#define P9CL_FILEFID	1

/*
 * "ENOENT" off the wire, back to a V8 errno.  v8fsd.c's errnames[] is the
 * other half and its comment argues for the symbolic name over strerror
 * prose; this is the table that makes it "exactly reversible by the client".
 *
 * EIO for anything unrecognised, which is the documented behaviour for a
 * FOREIGN server sending its own text -- an unrecognised error is still an
 * error, and that is the direction to fail in.
 */
static const struct { const char *n; int e; } enames[] = {
	{ "EPERM", V8_EPERM },		{ "ENOENT", V8_ENOENT },
	{ "EIO", V8_EIO },		{ "ENXIO", V8_ENXIO },
	{ "EBADF", V8_EBADF },		{ "EACCES", V8_EACCES },
	{ "EEXIST", V8_EEXIST },	{ "ENOTDIR", V8_ENOTDIR },
	{ "EISDIR", V8_EISDIR },	{ "EINVAL", V8_EINVAL },
	{ "ENFILE", V8_ENFILE },	{ "EMFILE", V8_EMFILE },
	{ "EFBIG", V8_EFBIG },		{ "ENOSPC", V8_ENOSPC },
	{ "EROFS", V8_EROFS },		{ "EMLINK", V8_EMLINK },
	{ "ENOMEM", V8_ENOMEM },	{ "ENAMETOOLONG", V8_ENOENT },
	{ "ENOTEMPTY", V8_EEXIST },
	{ 0, 0 }
};

static int
enumber(const char *s)
{
	int i, k;

	for (i = 0; enames[i].n; i++) {
		for (k = 0; enames[i].n[k] && s[k] == enames[i].n[k]; k++)
			;
		if (enames[i].n[k] == '\0' && s[k] == '\0') return (enames[i].e);
	}
	return (V8_EIO);
}

/*
 * Send what has been built in `b', read the reply into the same buffer, and
 * leave `out' positioned at the first field after the header.
 *
 * A REPLY OF THE WRONG TYPE IS EIO AND NOT A PARSE, which matters because the
 * only ways to get one are a desynchronised connection or a server that is not
 * this one.  Reading fields out of it would be reading the previous message's
 * bytes under this message's names, which is exactly the short-read lesson
 * ttyprobe records: a case has to be a pure function of what it was sent.
 */
static int
xacttag(int fd, struct p9buf *b, int want, struct p9buf *out, p9_u32 tag)
{
	char en[64];
	long n;
	p9_u32 type;

	if (p9_send(fd, b) < 0) { v8_errno = V8_EIO; return (-1); }
	if ((n = p9_recv(fd, msgbuf, (long)sizeof msgbuf)) <= 0) {
		v8_errno = V8_EIO;
		return (-1);
	}
	p9_init(out, msgbuf, n);
	p9_g32(out);				/* size, already honoured */
	type = p9_g8(out);
	/*
	 * The tag is a PARAMETER because Tversion's is P9_NOTAG and every other
	 * message here uses P9CL_TAG.  It was hardcoded, so switching Tversion
	 * to the tag the spec asks for made the reply unmatchable -- a
	 * three-line change that would have failed at the first mount.
	 */
	if (p9_g16(out) != tag) { v8_errno = V8_EIO; return (-1); }
	if (type == (p9_u32)want) return (0);
	if (type == P9_Rerror) {
		if (p9_gstr(out, en, (long)sizeof en) < 0 || !p9_ok(out))
			v8_errno = V8_EIO;
		else
			v8_errno = enumber(en);
		return (-1);
	}
	v8_errno = V8_EIO;
	return (-1);
}

static int
xact(int fd, struct p9buf *b, int want, struct p9buf *out)
{
	return (xacttag(fd, b, want, out, P9CL_TAG));
}

static void
begin(struct p9buf *b, int type)
{
	p9_hdr(b, msgbuf, (long)sizeof msgbuf, type, P9CL_TAG);
}

/* ------------------------------------------------------ connect and attach */

/*
 * getpeername(2) IS THE IDENTIFICATION, AND IT IS THE REASON AN INHERITED
 * DESCRIPTOR WORKS.
 *
 * vfs.c's fdtyp[] is process memory: it dies when a program replaces its own
 * image, and v8fs_fdtype() then answers "passthrough" for a descriptor that is
 * a 9P socket.  A raw read(2) on one of those does not return garbage -- it
 * BLOCKS FOREVER, because the server sends nothing unsolicited -- so the
 * failure is a hang rather than a wrong answer, which is worse to diagnose and
 * no better to have.
 *
 * The kernel still knows what the descriptor is, so it is the kernel that gets
 * asked.  Measured before this was written: a CONNECTED client reports the
 * server's bound path with len 106; an accept()ed descriptor and a socketpair
 * both report an empty path with len 16.  So the answer is positive
 * identification of our own socket and not merely "this is a Unix socket".
 *
 * The path is compared as well as the family, so a V8 program holding some
 * other Unix socket -- there are none today, and that is exactly the kind of
 * thing that stops being true quietly -- is not mistaken for a mount.
 */
static int
ispeer(int fd, struct p9mnt *m)
{
	struct sockaddr_un sa;
	int len = (int)sizeof sa, i;

	/*
	 * ZEROED FIRST, because getpeername writes only as much of sun_path as
	 * the address needs.  Measured on a socketpair: it returns len 16 and
	 * leaves 90 of the 104 bytes untouched.  Not reachable today -- the
	 * kernel does zero sun_path[0..13] and m_sock[0] is never NUL, so the
	 * compare always stops at index 0 -- and comparing against uninitialised
	 * stack for a security decision is not a thing to leave standing on the
	 * strength of that.
	 */
	for (i = 0; i < (int)sizeof sa; i++) ((char *)&sa)[i] = 0;
	if (rawsys3(SYS_getpeername, fd, (long)&sa, (long)&len) < 0)
		return (0);
	if (len <= (int)(sizeof sa - sizeof sa.sun_path)) return (0);
	if (sa.sun_family != AF_UNIX) return (0);
	for (i = 0; i < (int)sizeof sa.sun_path; i++) {
		if (m->m_sock[i] != sa.sun_path[i]) return (0);
		if (m->m_sock[i] == '\0') return (1);
	}
	return (0);
}

/*
 * v8fs_fdtype()'s fallback, called only when the process table has no opinion.
 * vfs.c has the placement argument.
 */
struct v8fstyp *
v8fs_p9adopt(int fd)
{
	struct p9mnt *m = p9mount();

	if (m == 0 || fd < 0) return (0);
	return (ispeer(fd, m) ? &v8fs_p9 : 0);
}

/* Connect, negotiate, attach.  Returns the descriptor, or -1 with v8_errno. */
static int
p9dial(struct p9mnt *m)
{
	struct sockaddr_un sa;
	struct p9buf b, r;
	char ver[32];
	long fd;
	int i;

	fd = rawsys3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) { v8_errno = v8sys_errno(RAWERR(fd)); return (-1); }

	for (i = 0; i < (int)sizeof sa; i++) ((char *)&sa)[i] = 0;
	sa.sun_family = AF_UNIX;
	for (i = 0; m->m_sock[i]; i++) sa.sun_path[i] = m->m_sock[i];
	if (rawsys3(SYS_connect, fd, (long)&sa, (long)sizeof sa) < 0) {
		/*
		 * A SERVER THAT IS NOT RUNNING IS ENOENT ON THE FILE, not
		 * ECONNREFUSED, because the caller asked about a file and not
		 * about a socket.  The distinction is visible in the message a
		 * V8 program prints and there is no V8 errno for the other one
		 * -- V8's errno.h stops at ELOOP.
		 */
		rawsys1(SYS_close, fd);
		v8_errno = V8_ENOENT;
		return (-1);
	}

	/*
	 * A DEAD SERVER MUST BE AN I/O ERROR AND NOT A SIGNAL, and without this
	 * it was a signal.
	 *
	 * The transport is a socket and the caller is a V7 program that has no
	 * idea it is one.  Writing a request to a peer that has gone away raises
	 * SIGPIPE, whose default disposition is to terminate -- so a v8fsd that
	 * died mid-conversation killed `cat' with signal 13 rather than giving
	 * it a failed read.  Measured: exit 141, from tests/streams' sanitized
	 * server aborting on a deliberately broken guard.  That is the pipe's
	 * semantics leaking through a filesystem, and on a real V8 a disk that
	 * stopped answering is EIO.
	 *
	 * SO_NOSIGPIPE IS PER SOCKET, WHICH IS THE WHOLE REASON TO USE IT rather
	 * than signal(SIGPIPE, SIG_IGN).  Ignoring the signal would change the
	 * program's own disposition, and a V8 program in a pipeline MUST still
	 * die when its reader goes away -- that is how `yes | head' terminates.
	 * Here, write(2) on this socket returns EPIPE and everything else about
	 * the process is untouched.
	 *
	 * PER SOCKET, NOT PER DESCRIPTOR, and this comment said descriptor until
	 * an auditor pointed out that the difference is the file's whole thesis.
	 * The flag lives in so_flags on the struct socket -- the OPEN FILE
	 * DESCRIPTION -- so every dup, every fork and an image replacement all
	 * share it. That is the better reading rather than a weaker one: the
	 * protection inherits for exactly the reason the offset does.
	 *
	 * Failure is not fatal: an older kernel without the option leaves the
	 * older behaviour, which is worse but not wrong enough to refuse a mount
	 * over.  Darwin has had it since 10.2.
	 */
	{
		int on = 1;

		(void)rawsys5(SYS_setsockopt, fd, SOL_SOCKET, SO_NOSIGPIPE,
		    (long)&on, (long)sizeof on);
	}

	/*
	 * TAG IS P9_NOTAG, WHICH THE SPEC ASKS FOR AND THE FIRST DRAFT DID NOT
	 * DO -- it sent tag 1 like every other message, and P9_NOTAG was
	 * defined in p9.h and used nowhere.  Harmless against this server,
	 * which does not check, and exactly the sort of thing a foreign server
	 * is entitled to refuse.
	 */
	p9_hdr(&b, msgbuf, (long)sizeof msgbuf, P9_Tversion, P9_NOTAG);
	p9_p32(&b, P9_MSIZE);
	p9_pstr(&b, P9_VERSION_V8);
	if (xacttag((int)fd, &b, P9_Rversion, &r, P9_NOTAG) < 0) goto fail;
	/*
	 * THE NEGOTIATED msize IS CHECKED RATHER THAN DISCARDED, and it used to
	 * be read into nothing.  This client has no per-connection state to
	 * keep it in -- that is the whole design -- and it clamps its reads to
	 * its OWN P9_MSIZE, so a server that negotiates smaller would be sent
	 * messages larger than it agreed to.  Refusing is the answer available
	 * to a stateless client; against v8fsd it never fires, because the
	 * server clamps to the client's proposal and ours is the maximum.
	 */
	if (p9_g32(&r) < (p9_u32)P9_MSIZE) { v8_errno = V8_EIO; goto fail; }
	if (p9_gstr(&r, ver, (long)sizeof ver) < 0 || !p9_ok(&r)) {
		v8_errno = V8_EIO;
		goto fail;
	}
	/*
	 * "unknown" is 9P's own way for a server to refuse a version, and it
	 * is a successful Rversion rather than an Rerror -- so a client that
	 * only checked the message type would go on to Tattach against a
	 * server that has agreed to nothing.
	 *
	 * AND THE STRING MUST BE THE EXTENDED ONE, WHICH IS THE FIX FOR A
	 * SILENT EMPTY FILE.  This client sends P9_OFFCUR on every read; a
	 * CONFORMING 9P2000 server has no cursor, so it read zero bytes from a
	 * file whose Rstat reported a length, and cat printed nothing and
	 * exited 0.  Measured against a deliberately conforming server.  9P's
	 * own version negotiation is the right place to catch it: a conforming
	 * server answers "9P2000" to our "9P2000.v8" offer, and that answer is
	 * this test failing.
	 */
	for (i = 0; P9_VERSION_V8[i]; i++)
		if (ver[i] != P9_VERSION_V8[i]) { v8_errno = V8_EIO; goto fail; }
	if (ver[i] != '\0') { v8_errno = V8_EIO; goto fail; }

	begin(&b, P9_Tattach);
	p9_p32(&b, P9CL_ROOTFID);
	p9_p32(&b, P9_NOFID);			/* no authentication */
	p9_pstr(&b, "v8");			/* uname */
	p9_pstr(&b, "");			/* aname: the one tree served */
	if (xact((int)fd, &b, P9_Rattach, &r) < 0) goto fail;

	return ((int)fd);
fail:
	rawsys1(SYS_close, fd);
	return (-1);
}

/*
 * Walk from the root fid to `rel', cloning onto P9CL_FILEFID.
 *
 * MORE THAN P9_MAXWELEM COMPONENTS NEEDS MORE THAN ONE MESSAGE, and 16 is the
 * spec's limit rather than this server's.  The continuation walks FILEFID onto
 * itself, which the spec allows for an unopened fid and v8fsd.c implements
 * (`if (newfid == fid) nf = f').
 *
 * A SHORT Rwalk IS A FAILED OPEN HERE.  The server distinguishes "the first
 * name does not exist" (an error) from "the path stopped part way" (a short
 * reply), and that distinction is for a client that is resolving a path a piece
 * at a time.  open(2) is not: it asked for the whole name, and a partial answer
 * means the name is not there.
 *
 * ...BUT WHICH FAILURE IT IS HAS TO BE RECONSTRUCTED, AND THE ABOVE USED TO
 * ANSWER ENOENT FOR BOTH.  V7's namei has two answers, one line apart, and so
 * does this server -- nami.c's "not a directory" arm is do_walk's
 * `if ((ip->i_mode & IFMT) != IFDIR) u.u_error = ENOTDIR' at v8fsd.c:699.  A
 * short Rwalk CARRIES NO ERRNO, so that answer is lost on the wire, and
 * `open("/mnt/hello/beyond")' reported ENOENT where a V7 kernel reports
 * ENOTDIR.  Measured with tests/streams' client probe, which is the first thing
 * in this port to ask.
 *
 * The information is in the reply, in the qids this function used to discard:
 * a short walk means the components before `got' succeeded and the one AT `got'
 * did not, so the last qid returned describes what the failed component was
 * looked up in.  If that is not a directory, the reason is ENOTDIR; otherwise
 * the name really is absent.  Only the short arm needs this -- a walk that
 * fails on its FIRST name is an Rerror and already carries the server's own
 * errno through enumber().
 */
static int
p9walk(int fd, const char *rel)
{
	struct p9buf b, r;
	const char *p = rel;
	p9_u32 nw, got;
	int first = 1, i;
	long lenpos;

	do {
		begin(&b, P9_Twalk);
		p9_p32(&b, first ? (p9_u32)P9CL_ROOTFID : (p9_u32)P9CL_FILEFID);
		p9_p32(&b, P9CL_FILEFID);
		/*
		 * nwname is not known until the components have been packed,
		 * so its two bytes are reserved and patched.  p9_len() is
		 * relative to the message base, which is what makes the patch
		 * an index rather than a saved pointer -- and a saved pointer
		 * would be the aliasing trap v8s_link records, one layer down.
		 */
		lenpos = p9_len(&b);
		p9_p16(&b, 0);
		nw = 0;
		while (*p && nw < P9_MAXWELEM) {
			char nm[P9_NAMELEN];

			for (i = 0; *p && *p != '/'; p++) {
				if (i >= (int)sizeof nm - 1) {
					v8_errno = V8_ENOENT;
					return (-1);
				}
				nm[i++] = *p;
			}
			nm[i] = '\0';
			while (*p == '/') p++;
			if (i == 0) continue;		/* "a//b", trailing '/' */
			p9_pstr(&b, nm);
			nw++;
		}
		msgbuf[lenpos] = (unsigned char)(nw & 0xff);
		msgbuf[lenpos + 1] = (unsigned char)((nw >> 8) & 0xff);

		if (xact(fd, &b, P9_Rwalk, &r) < 0) return (-1);
		got = p9_g16(&r);
		/*
		 * THESE TWO ARE PROTOCOL FAULTS AND NOT "NO SUCH FILE", and
		 * this line said ENOENT until an auditor read it.  It used to
		 * be `got != nw', which ALSO covered the legitimate short walk
		 * -- and for that case ENOENT was the right answer.  Moving the
		 * legitimate case into the branch below left its errno behind
		 * on the two that remain: a reply too short to hold nwqid[2],
		 * and a server returning more qids than names were asked for.
		 * Neither is a statement about the file.  xacttag() in this
		 * same file already says the rule -- "A REPLY OF THE WRONG TYPE
		 * IS EIO AND NOT A PARSE" -- and :613 four lines below calls
		 * this very class EIO.  The fix landed on one line and the line
		 * beside it kept the assumption, which is this port's most
		 * repeated shape and is no less easy to do while writing the
		 * thing that documents it.
		 *
		 * Measured against a nonconforming server: an Rwalk with no
		 * nwqid field, and got = 65535 for a two-name walk, both
		 * reported ENOENT.  A caller that reads ENOENT as "then create
		 * it" takes the wrong branch on a wedged mount.
		 */
		if (!p9_ok(&r) || got > nw) { v8_errno = V8_EIO; return (-1); }
		if (got != nw) {
			struct p9qid q;
			p9_u32 k;
			int isdir = 1;

			/*
			 * `got' is 0 only from a server that answered a failed
			 * FIRST name with a short reply instead of an Rerror.
			 * There is then nothing IN THIS MESSAGE to read a reason
			 * out of, so the loop does not run and ENOENT stands.
			 *
			 * On a CONTINUATION message the reason would in fact be
			 * available -- it is the last qid of the previous Rwalk,
			 * which this loop discards each time round.  Carrying it
			 * across iterations is deliberately not done: it needs a
			 * path of more than P9_MAXWELEM components AND a server
			 * that breaks the spec, since a real one answers a
			 * zero-length walk with an Rerror (v8fsd.c:732-735), so
			 * the state would exist for a case nothing can reach.
			 *
			 * p9_gqid returns void and reports an underrun through
			 * p9_ok, which is checked once after the loop: a buffer
			 * that ran out stays short for every later get, so one
			 * test at the end sees any failure in it.
			 */
			for (k = 0; k < got; k++) {
				p9_gqid(&r, &q);
				isdir = (q.q_type & P9_QTDIR) != 0;
			}
			if (!p9_ok(&r)) { v8_errno = V8_EIO; return (-1); }
			v8_errno = isdir ? V8_ENOENT : V8_ENOTDIR;
			return (-1);
		}
		first = 0;
	} while (*p);

	return (0);
}

/*
 * ---------------------------------------------------------- §8a step 5f ---
 *
 * THE WRITE SIDE NEEDS THE PARENT, AND 9P IS WHY.  Tcreate names a directory
 * fid and a single component; so does Tremove's useful form.  V7's syscalls
 * name a whole path.  So every mutating operation here is "walk to all but the
 * last component, then act on the last one" -- which is exactly what namei
 * does on the far side, split across the wire.
 *
 * p9parent LEAVES FILEFID ON THE PARENT and copies the last component out.
 * Trailing slashes are dropped first, because `rmdir /mnt/sub/' must name
 * `sub' and not the empty string -- and the empty string would walk nowhere
 * and then create a nameless entry.  A path with no parent component (a bare
 * name directly under the mount) walks zero components, which is a legal
 * Twalk and leaves FILEFID cloned onto the root.
 */
static int
p9parent(int fd, const char *rel, char *base, long bmax)
{
	const char *end, *slash, *p;
	char dir[1024];
	long n;

	end = rel;
	while (*end) end++;
	while (end > rel && end[-1] == '/') end--;	/* "sub/" -> "sub" */
	if (end == rel) { v8_errno = V8_EINVAL; return (-1); }

	slash = end;
	while (slash > rel && slash[-1] != '/') slash--;
	n = (long)(end - slash);
	if (n >= bmax) { v8_errno = V8_ENOENT; return (-1); }
	for (p = slash; p < end; p++) base[p - slash] = *p;
	base[n] = '\0';
	if (base[0] == '.' && (base[1] == '\0' ||
	    (base[1] == '.' && base[2] == '\0'))) {
		v8_errno = V8_EINVAL;
		return (-1);
	}

	n = (long)(slash - rel);
	if (n >= (long)sizeof dir) { v8_errno = V8_ENOENT; return (-1); }
	for (p = rel; p < slash; p++) dir[p - rel] = *p;
	dir[n] = '\0';
	return (p9walk(fd, dir));
}

/*
 * Tcreate on the fid p9parent left, and the two things it takes are 9P's
 * permission word and 9P's open mode -- not V7's.  DMDIR is the top bit and
 * the low nine are the mode; the server maps them back.
 */
static int
p9create(int fd, const char *name, p9_u32 perm, int om)
{
	struct p9buf b, r;
	struct p9qid q;

	begin(&b, P9_Tcreate);
	p9_p32(&b, P9CL_FILEFID);
	p9_pstr(&b, name);
	p9_p32(&b, perm);
	p9_p8(&b, (p9_u32)om);
	if (xact(fd, &b, P9_Rcreate, &r) < 0) return (-1);
	p9_gqid(&r, &q);
	if (!p9_ok(&r)) { v8_errno = V8_EIO; return (-1); }
	return (0);
}

/* --------------------------------------------------------------- the type */

/*
 * t_path: identity.  There is no host path to resolve -- running the name
 * through rootpath() would ask whether the JAIL has a file of that name, which
 * is a different question with a different answer, and for a name under a
 * mount the honest answer is that only the server knows.
 *
 * V8P_MAKE is accepted and ignored for the same reason the /dev/fd type
 * ignores it: the parent-exists rule it expresses is the host filesystem's
 * way of answering a question about a name that does not exist yet, and here
 * that question is Twalk's to answer.
 */
static char *
p9_t_path(char *p, int mode)
{
	(void)mode;
	return (p);
}

static int
p9_t_open(char *p, int flags, int mode)
{
	struct p9mnt *m = p9mount();
	struct p9buf b, r;
	struct p9qid q;
	const char *rel;
	int fd, om;

	if (m == 0 || (rel = p9rel(m, p)) == 0) { v8_errno = V8_ENOENT; return (-1); }
	if ((fd = p9dial(m)) < 0) return (-1);

	/*
	 * V8's open(2) flags ARE 9P's low two bits -- 0 read, 1 write, 2
	 * read/write -- which is not a coincidence: Plan 9 inherited the
	 * numbering from the same place V7 got it.  O_TRUNC is the one other
	 * bit 9P has a name for.
	 *
	 * O_APPEND IS REFUSED RATHER THAN DROPPED, and refusing it costs
	 * nothing because the shell does not use it.  9P has no append OPEN
	 * MODE -- DMAPPEND is an attribute of the file, set at create and
	 * binding on every writer -- so honouring it would mean either seeking
	 * to the end here (wrong the moment a second writer exists, which is
	 * the entire reason O_APPEND is not a seek) or a third extension.
	 * Dropping it would be worse than either: a program would believe it
	 * had append semantics and silently overwrite from offset zero.
	 * Measured before choosing: `>>' in V8's sh is sh/service.c:76,
	 * `lseek(fd, 0L, 2)' after an ordinary open, which this client already
	 * serves through Tseek.  The one O_APPEND in the whole tree is
	 * sh/service.c:598's accounting file, which nothing here enables.
	 */
	if (flags & 0x0008) { v8_errno = V8_EINVAL; goto fail; }
	om = flags & 3;
	if (om == 3) om = P9_ORDWR;		/* V8 has no O_EXEC */
	/*
	 * 0x0400 AND NOT 01000, which is where the first draft of this line
	 * was wrong: 01000 octal is 0x200, which is O_CREAT.  syscall.c spells
	 * both in hex beside each other for exactly this reason, and mixing
	 * the bases in one expression is how a flag test ends up naming the
	 * flag next to the one it meant.
	 */
	if (flags & 0x0400) om |= P9_OTRUNC;

	/*
	 * O_CREAT IS A WALK THEN A Tcreate, IN THAT ORDER, AND THE ORDER IS THE
	 * WHOLE OF IT -- §8a step 5f.
	 *
	 * 9P's Tcreate FAILS if the name exists; V7's O_CREAT tolerates it.
	 * The two are reconciled on this side rather than by loosening the
	 * server, because the server's strictness is what a second client would
	 * rely on.  So: try the walk first, and only if it fails go to the
	 * parent and create.  A create that then loses a race gets the server's
	 * EEXIST, which is the honest answer and not one this client invents.
	 *
	 * THE WALK IS TRIED FIRST RATHER THAN THE CREATE, which costs a round
	 * trip on the create path and buys the common case: almost every
	 * O_CREAT in a 1985 userspace is `> file' on a file that already
	 * exists, and Tcreate-then-fall-back would send a doomed message every
	 * time.  It also keeps O_TRUNC's meaning: truncation belongs to the
	 * OPEN of an existing file, and a freshly created one is empty anyway.
	 */
	if ((flags & 0x0200) != 0) {		/* O_CREAT */
		char base[P9_NAMELEN];

		if (p9walk(fd, rel) < 0) {
			if (p9parent(fd, rel, base, (long)sizeof base) < 0)
				goto fail;
			/*
			 * MODE, NOT PERM, and the difference is one bit that
			 * cannot be set from here: DMDIR.  open(2) cannot make
			 * a directory, so the permission word is the V7 mode
			 * with nothing added -- v8s_mkdir is the caller that
			 * sets DMDIR, through t_mkdir.
			 */
			if (p9create(fd, base, (p9_u32)(mode & 07777), om) < 0)
				goto fail;
			goto opened;
		}
	} else if (p9walk(fd, rel) < 0) {
		goto fail;
	}

	begin(&b, P9_Topen);
	p9_p32(&b, P9CL_FILEFID);
	p9_p8(&b, (p9_u32)om);
	if (xact(fd, &b, P9_Ropen, &r) < 0) goto fail;
	p9_gqid(&r, &q);
	if (!p9_ok(&r)) { v8_errno = V8_EIO; goto fail; }
	goto checked;

opened:
	/*
	 * A Tcreate LEAVES THE FID OPEN, so there is no Topen after it -- 9P
	 * says so and v8fsd's do_create sets f_omode.  What that costs is the
	 * qid, which Rcreate carries and p9create discards: a freshly created
	 * name cannot be a directory when open(2) made it, so the only thing
	 * the qid would have been used for below is a question with one answer.
	 */
	q.q_type = P9_QTFILE;
checked:

	if (q.q_type & P9_QTDIR) {
		extern int v8fs_p9dirsnap(int fd);

		if (v8fs_p9dirsnap(fd) < 0) goto fail;
	}
	/*
	 * THE TYPE BINDS ITSELF, which is the pattern rather than a detail --
	 * v8s_open does not do it, because noticing what kind of thing came
	 * back is the filesystem's business and not open(2)'s.  /dev/fd's
	 * fd_open deliberately does NOT bind, and says why: a dup'd descriptor
	 * IS an ordinary host descriptor.  This one is not.
	 *
	 * The bind is a CACHE and not the record -- v8fs_fdtype can rediscover
	 * this descriptor from the kernel if the table is gone.  Doing it here
	 * anyway saves a getpeername on the first read of every file the
	 * program opened itself, which is most of them.
	 */
	v8fs_bind(fd, &v8fs_p9);
	return (fd);
fail:
	rawsys1(SYS_close, fd);
	return (-1);
}

static int
p9_t_close(int fd)
{
	if (v8sys_isdirfd(fd)) v8sys_dirclose(fd);
	/*
	 * NO Tclunk, AND THE FIRST DRAFT SENT ONE AND BROKE REDIRECTION.
	 *
	 * A clunk destroys the fid, and the fid belongs to the CONNECTION --
	 * which is shared by every dup of this descriptor.  So `sh' doing the
	 * ordinary thing for `cat < /mnt/hello' -- open, dup2 onto 0, close the
	 * original -- clunked the file out from under the descriptor it had
	 * just made, and cat read nothing at all.  close(2) on one of two dups
	 * must not disturb the other, and a clunk is not close(2), it is the
	 * last close.
	 *
	 * Dropping the connection is what releases the fids, and the kernel
	 * does that at the LAST close because it is the thing that knows the
	 * reference count.  connclose() walks the fids on EOF.  So the right
	 * number of Tclunks is zero and the server frees everything anyway.
	 *
	 * Worth recording that the comment which stood here got the mechanism
	 * exactly right -- "dropping the connection is what actually releases
	 * the server's fids" -- and then called the clunk "politeness".  It was
	 * not politeness, it was the bug, in the sentence explaining why it was
	 * unnecessary.  That is this port's most repeated shape and it took an
	 * end-to-end run to see.
	 */
	if (rawsys1(SYS_close, fd) < 0) { v8_errno = V8_EIO; return (-1); }
	return (0);
}

static long
p9_t_read(int fd, char *buf, long n)
{
	struct p9buf b, r;
	unsigned char *d;
	p9_u32 count;

	if (v8sys_isdirfd(fd)) return (v8sys_dirread(fd, buf, n));
	if (n <= 0) return (0);

	/*
	 * ONE MESSAGE, NOT A LOOP TO n.  read(2) is permitted to return less
	 * than it was asked for, every V8 program already handles that (they
	 * were written for a machine where a tty read stopped at a newline),
	 * and a loop here would turn one short read at end of file into two
	 * round trips.  The server clamps to its iounit; the count that comes
	 * back is what the caller gets.
	 */
	begin(&b, P9_Tread);
	p9_p32(&b, P9CL_FILEFID);
	p9_p64(&b, P9_OFFCUR);
	p9_p32(&b, (p9_u32)(n > P9_MSIZE - P9_IOHDRSZ ? P9_MSIZE - P9_IOHDRSZ : n));
	if (xact(fd, &b, P9_Rread, &r) < 0) return (-1);
	count = p9_g32(&r);
	if (!p9_ok(&r)) { v8_errno = V8_EIO; return (-1); }
	if ((long)count > n) { v8_errno = V8_EIO; return (-1); }
	if ((d = p9_gdata(&r, (long)count)) == 0) { v8_errno = V8_EIO; return (-1); }
	{
		long i;
		for (i = 0; i < (long)count; i++) buf[i] = (char)d[i];
	}
	return ((long)count);
}

/*
 * Twrite IS SENT rather than refused locally, and the difference matters.
 * The server answers EROFS today (§8a step 5f is the write half) and the
 * kernel underneath it is written and tested -- so a client that short-circuits
 * here would have to be changed on the day the server stops refusing, and a
 * client that asks would not.  It also means the errno a program sees is the
 * server's own answer about the file rather than this file's guess.
 */
static long
p9_t_write(int fd, char *buf, long n)
{
	struct p9buf b, r;
	p9_u32 count;
	long max = P9_MSIZE - P9_IOHDRSZ;

	if (v8sys_isdirfd(fd)) { v8_errno = V8_EISDIR; return (-1); }
	if (n <= 0) return (0);
	if (n > max) n = max;

	begin(&b, P9_Twrite);
	p9_p32(&b, P9CL_FILEFID);
	p9_p64(&b, P9_OFFCUR);
	p9_p32(&b, (p9_u32)n);
	p9_pdata(&b, buf, n);
	if (xact(fd, &b, P9_Rwrite, &r) < 0) return (-1);
	count = p9_g32(&r);
	if (!p9_ok(&r) || (long)count > n) { v8_errno = V8_EIO; return (-1); }
	return ((long)count);
}

static long
p9_t_seek(int fd, long off, int whence)
{
	struct p9buf b, r;
	p9_u64 np;

	if (v8sys_isdirfd(fd)) return (v8sys_dirseek(fd, off, whence));
	if (whence < 0 || whence > 2) { v8_errno = V8_EINVAL; return (-1); }

	begin(&b, P9_Tseek);
	p9_p32(&b, P9CL_FILEFID);
	p9_p64(&b, (p9_u64)off);	/* signed on this side; see do_seek */
	p9_p8(&b, (p9_u32)whence);
	if (xact(fd, &b, P9_Rseek, &r) < 0) return (-1);
	np = p9_g64(&r);
	if (!p9_ok(&r)) { v8_errno = V8_EIO; return (-1); }
	return ((long)np);
}

/* ----------------------------------------------------------------- stat */

/*
 * A 9P stat, into V8's struct stat.
 *
 * st_ino IS THE QID PATH AND IS NOT FOLDED, which is the one place this type
 * is BETTER off than passthrough.  dir.c's v8sys_fold_ino exists because a
 * 64-bit APFS inode does not fit in V8's 16 bits and the map has to be
 * invented; a v8fs qid path IS a V7 i_number, already 16 bits, already the
 * number the directory entry holds.  So the identity V7's idioms depend on --
 * getwd's `while (dir->d_ino != d.st_ino)' -- is the real one here, with no
 * collision class behind it at all.
 *
 * THE OWNER ARRIVES AS A DECIMAL STRING, which v8fsd.c's statof argues for:
 * a V7 inode holds a number, 9P wants a name, and turning one into the other
 * would mean choosing between two passwd files.  Parsed straight back.
 */
/*
 * ...AND A NAME THIS CANNOT PARSE MUST NOT COME BACK AS ROOT.  CLAUDE.md
 * states the contract for every 16-bit narrowing in this port -- "root maps to
 * root, and non-root never maps to root" -- and the first version of this
 * function returned 0 on both failure paths, which is the second half exactly
 * backwards.
 *
 * It was reachable, and the route is worth keeping because nothing about it is
 * exotic: di_uid is v8_i16 and therefore SIGNED (src/include/sys/ino.h:28), so
 * an image whose proto gave a file uid 40000 loads as -25536, v8fsd's statof
 * renders it "%d" as "-25536", and the '-' is a non-digit.  Measured: `ls -l'
 * printed that file as owned by root.
 *
 * P9UID_BAD is 65535 as an unsigned pattern -- (short)-1 -- which is not a uid
 * any V8 system issues and prints as itself, so an owner this port cannot
 * represent reads as one it cannot represent.  Note also that the range test
 * below is DEAD against v8fsd, whose "%d" of a short can never exceed 32767;
 * it is kept for a foreign server, and it is the guard that could not fire
 * while the case that did fire fell into the same return.
 */
#define P9UID_BAD	((short)-1)

static short
p9uid(const char *s)
{
	long v = 0;
	int i;

	if (s[0] == '\0') return (P9UID_BAD);
	for (i = 0; s[i]; i++) {
		if (s[i] < '0' || s[i] > '9') return (P9UID_BAD);
		v = v * 10 + (s[i] - '0');
		if (v > 32767) return (P9UID_BAD);
	}
	return ((short)v);
}

static void
p9tostat(const struct p9stat *s, struct v8_stat *st)
{
	char *q = (char *)st;
	int i;

	for (i = 0; i < (int)sizeof *st; i++) q[i] = 0;
	st->st_dev  = 0;
	/*
	 * The caller has already refused a qid that does not fit -- see
	 * p9statfid.  Said here because the line reads like a bare truncation
	 * and the argument for it being safe is in another function.
	 */
	st->st_ino  = (v8_ino_t)s->s_qid.q_path;
	st->st_mode = (unsigned short)(s->s_mode & 07777);
	st->st_mode |= (s->s_mode & P9_DMDIR) ? V8_S_IFDIR : V8_S_IFREG;
	st->st_nlink = 1;
	st->st_uid = p9uid(s->s_uid);
	st->st_gid = p9uid(s->s_gid);
	st->st_rdev = 0;
	st->st_size = (v8_off_t)s->s_length;
	st->st_atime = (v8_time_t)s->s_atime;
	st->st_mtime = (v8_time_t)s->s_mtime;
	/*
	 * ctime IS mtime, and that is 9P's shape rather than a shortcut: the
	 * stat structure has atime and mtime and no third time at all.  V8's
	 * di_ctime exists on the disk this server is reading and cannot cross
	 * the wire, so the choice is between mtime and zero.  A zero would
	 * make `ls -lc' sort every file into 1970, which is a wrong answer;
	 * mtime is the closest true one and the same thing u9fs does.
	 */
	st->st_ctime = (v8_time_t)s->s_mtime;
}

/* Tstat on an open connection's file fid. */
static int
p9statfid(int fd, struct v8_stat *st)
{
	struct p9buf b, r;
	struct p9stat s;

	begin(&b, P9_Tstat);
	p9_p32(&b, P9CL_FILEFID);
	if (xact(fd, &b, P9_Rstat, &r) < 0) return (-1);
	/*
	 * THE DOUBLE SIZE PREFIX.  Rstat is `size[4] Rstat tag[2] stat[n]',
	 * and the stat itself opens with its own size[2] -- so the message
	 * carries a two-byte count in front of a structure whose first field
	 * is a two-byte count of the same object less two.  p9_gstat consumes
	 * the inner one; this g16 is the outer.  Getting them the wrong way
	 * round decodes a stat shifted by two bytes, which parses.
	 */
	p9_g16(&r);
	if (p9_gstat(&r, &s) < 0 || !p9_ok(&r)) { v8_errno = V8_EIO; return (-1); }
	/*
	 * A QID THAT DOES NOT FIT IS REFUSED HERE TOO, AND IT WAS REFUSED IN
	 * ONLY ONE OF THE TWO PLACES.  v8fs_p9dirsnap below has had this test
	 * since it was written, with a comment saying a truncated inode number
	 * "is the silently-wrong answer that v8sys_fold_ino's whole history is
	 * about" -- and p9tostat did the truncation bare, twelve lines from the
	 * argument.  Measured against a foreign server handing out qid path
	 * 65539: stat reported ino 3, indistinguishable from a real inode 3,
	 * while a directory listing of the same server correctly failed.  Two
	 * answers to one question, from one file.
	 */
	if (s.s_qid.q_path > 65535ULL) { v8_errno = V8_EIO; return (-1); }
	p9tostat(&s, st);
	return (0);
}

/*
 * fstat, AND A DIRECTORY'S st_size IS THE SNAPSHOT'S LENGTH AND NOT THE
 * SERVER'S.  This is dir.c:114's rule arriving in a second filesystem: what
 * read(2) will produce for a directory descriptor is a run of 256-byte V7
 * records, and the number the thing underneath charges for the same directory
 * is unrelated -- for the root of tests/streams' image, 64 bytes against 1024
 * of records.  (This said 768, and 768 belongs to the SUBDIRECTORY, which has
 * three entries where the root has four.  The pair 64/768 describes neither
 * directory: 64 bytes of 16-byte entries is four of them, which is 1024 bytes
 * of records.  Two numbers from two places, written down as a measurement --
 * which is why the probe below prints the ratio and the case asserts THAT.)
 *
 * The port already learned what the difference costs: every reader that loops
 * to EOF never notices, and ps(1)'s getdir() sizes an array from st_size and
 * then insists read() return exactly that.  So the fix that was made once for
 * passthrough (v8sys_pt_fstat, fstat only) has to be made again for each type
 * that snapshots -- which is this file's version of "the fix landed on one
 * line and the line beside it kept the assumption", except the line beside it
 * is a whole second implementation of the same interface.
 *
 * fstat ONLY, like passthrough: nothing can read a directory without opening
 * it, and doing this for stat(2) would mean building the snapshot inside every
 * `ls -l'.
 *
 * NO CASE COVERS THIS LINE, AND SAYING SO IS THE POINT.  Deleting it was
 * mutated and the suite stayed green -- which by this repo's rule means either
 * the code is dead or the guard is missing, and here it is the guard.  The
 * only V8 idiom that reads a directory's fstat size is ps(1)'s getdir(), which
 * sizes an array from st_size and then demands read(2) return exactly that
 * many bytes; ps reads /proc and nothing in the tree fstats a directory on a
 * MOUNT.  So the line is right by the same argument that made v8sys_pt_fstat
 * right, and it is unexercised until something calls it.  A client probe
 * linked against the shim sources -- the shape tests/v8sys/test.c already has
 * -- is what would close it, and it would reach lseek and dup directly too.
 */
static int
p9_t_fstat(int fd, struct v8_stat *st)
{
	long n;

	if (p9statfid(fd, st) < 0) return (-1);
	if ((n = v8sys_dirsize(fd)) >= 0) st->st_size = (v8_off_t)n;
	return (0);
}

/*
 * t_stat: a whole connection for one question.
 *
 * Expensive and correct, in that order.  `ls -l' on a mount is one connect,
 * version, attach, walk, stat and close per entry -- and the cheap version,
 * caching a connection across calls, would need this file to hold state that
 * the header comment spends its length explaining it must not.  Recorded as a
 * cost rather than fixed: nothing measures it yet, and a cache added before
 * there is a measurement is a cache added on a guess.
 *
 * follow IS IGNORED because a V7 filesystem has no symbolic links.  V8 added
 * them to ITS filesystem, but the images this server reads are mkfs(8)'s and
 * IFLNK is not a mode any of them contains -- so lstat and stat cannot differ,
 * and pretending they might would be inventing a difference the far end does
 * not have.
 */
static int
p9_t_stat(char *p, struct v8_stat *st, int follow)
{
	struct p9mnt *m = p9mount();
	const char *rel;
	int fd, r;

	(void)follow;
	if (m == 0 || (rel = p9rel(m, p)) == 0) { v8_errno = V8_ENOENT; return (-1); }
	if ((fd = p9dial(m)) < 0) return (-1);
	if (p9walk(fd, rel) < 0) { rawsys1(SYS_close, fd); return (-1); }
	r = p9statfid(fd, st);
	rawsys1(SYS_close, fd);
	return (r);
}

/*
 * t_ioctl: ENOTTY, always, and that is the right answer rather than a stub.
 * The only ioctls in this world are the sgtty ones, and a file on a disk image
 * is not a terminal.  It is also the answer the passthrough type gives for an
 * ordinary file, so a program cannot tell a mounted file from a jailed one by
 * asking -- which is the property a mount is supposed to have.
 */
static int
p9_t_ioctl(int fd, int cmd, char *arg)
{
	(void)fd; (void)cmd; (void)arg;
	v8_errno = V8_ENOTTY;
	return (-1);
}

/*
 * t_access: ASKED, NOT COMPUTED, and p9.h has the argument for the extension
 * that makes it possible.  In one sentence: the client cannot answer this,
 * because the mode bits are the image's and the identity is the server's, and
 * v8s_access got it wrong in both available directions before this existed --
 * first by recomputing against the host uid (so `test -r' said no on every
 * file of every image), then by reporting a fixed EROFS for write (right while
 * the server refused every write, wrong from §8a step 5f onward).
 */
static int
p9_t_access(char *p, int mode)
{
	struct p9mnt *m = p9mount();
	struct p9buf b, r;
	const char *rel;
	int fd, rc;

	if (m == 0 || (rel = p9rel(m, p)) == 0) { v8_errno = V8_ENOENT; return (-1); }
	if ((fd = p9dial(m)) < 0) return (-1);
	if (p9walk(fd, rel) < 0) { rawsys1(SYS_close, fd); return (-1); }

	begin(&b, P9_Taccess);
	p9_p32(&b, P9CL_FILEFID);
	p9_p8(&b, (p9_u32)(mode & (P9_AREAD | P9_AWRITE | P9_AEXEC)));
	rc = xact(fd, &b, P9_Raccess, &r) < 0 ? -1 : 0;
	rawsys1(SYS_close, fd);
	return (rc);
}

/*
 * t_remove: unlink and rmdir, which are one message.  isdir picks the arm and
 * comes from the CALLER rather than from a stat here, because unlink(2) on a
 * directory and rmdir(2) on a file are different errors and the caller is the
 * one that knows which syscall was made.
 */
static int
p9_t_remove(char *p, int isdir)
{
	struct p9mnt *m = p9mount();
	struct p9buf b, r;
	const char *rel;
	int fd, rc;

	if (m == 0 || (rel = p9rel(m, p)) == 0) { v8_errno = V8_ENOENT; return (-1); }
	if ((fd = p9dial(m)) < 0) return (-1);
	if (p9walk(fd, rel) < 0) { rawsys1(SYS_close, fd); return (-1); }

	/*
	 * THE FID IS GONE WHETHER OR NOT THIS SUCCEEDS -- 9P says a Tremove
	 * clunks the fid even on Rerror -- so there is nothing to clean up and
	 * the connection close below is only the socket.  A client that sent a
	 * Tclunk after a failed Tremove would get EBADF.
	 *
	 * isdir IS SENT BY NOT BEING SENT: 9P's Tremove has no flag, so the
	 * server decides from the inode's own mode and picks NI_DEL or
	 * NI_RMDIR.  What the caller's isdir does here is refuse the mismatch
	 * BEFORE the wire, so that `rmdir /mnt/hello' is ENOTDIR rather than an
	 * unlink the caller did not ask for.
	 */
	if (isdir >= 0) {
		struct v8_stat st;

		if (p9statfid(fd, &st) < 0) { rawsys1(SYS_close, fd); return (-1); }
		if (((st.st_mode & V8_S_IFMT) == V8_S_IFDIR) != (isdir != 0)) {
			v8_errno = isdir ? V8_ENOTDIR : V8_EISDIR;
			rawsys1(SYS_close, fd);
			return (-1);
		}
	}

	begin(&b, P9_Tremove);
	p9_p32(&b, P9CL_FILEFID);
	rc = xact(fd, &b, P9_Rremove, &r) < 0 ? -1 : 0;
	rawsys1(SYS_close, fd);
	return (rc);
}

/*
 * t_mkdir: Tcreate with DMDIR, on the parent.  The mode is masked to 0777
 * here as well as on the server, because the bit above it in 9P's permission
 * word IS DMDIR and a caller passing S_IFDIR in the mode -- which v8s_mknod
 * legitimately does -- must not have it land there.
 */
static int
p9_t_mkdir(char *p, int mode)
{
	struct p9mnt *m = p9mount();
	const char *rel;
	char base[P9_NAMELEN];
	int fd, rc;

	if (m == 0 || (rel = p9rel(m, p)) == 0) { v8_errno = V8_ENOENT; return (-1); }
	if ((fd = p9dial(m)) < 0) return (-1);
	if (p9parent(fd, rel, base, (long)sizeof base) < 0) {
		rawsys1(SYS_close, fd);
		return (-1);
	}
	rc = p9create(fd, base, P9_DMDIR | (p9_u32)(mode & 0777), P9_OREAD);
	rawsys1(SYS_close, fd);
	return (rc);
}

/*
 * ------------------------------------------------------------------ Twstat
 *
 * chmod, chown and utime are ONE MESSAGE, and that is 9P's shape rather than a
 * shortcut here: a wstat carries a whole stat and the server applies whichever
 * of its fields are not the "do not touch" sentinel.  So each of the three
 * below is p9_nostat() plus one or two assignments, and the interesting code is
 * all in deciding what to put in a field rather than in sending it.
 */

/*
 * The inverse of p9uid() above, and it exists because THIS FILE HAS NO libc --
 * dir.c's rule at the top of it, raw syscalls only, so there is no snprintf to
 * borrow.  Twelve bytes is enough for any int with its sign.
 *
 * IT FORMATS THE WHOLE int RANGE, INCLUDING NEGATIVES, and after the fold
 * below no caller passes one -- kept because this is a FORMATTER and its
 * contract is its parameter type, not a policy about what may be formatted.
 * Said out loud so the arm is known to be unexercised rather than assumed
 * live.  -(long)v rather than -v so that INT_MIN does not overflow.
 */
static void
p9dec(char *d, long max, int v)
{
	char tmp[12];
	long n = 0, i = 0;
	unsigned long u;

	if (max < 1) return;		/* d[0] would be the terminator's slot */

	if (v < 0) {
		if (i + 1 < max) d[i++] = '-';
		u = (unsigned long)(-(long)v);
	} else
		u = (unsigned long)v;
	do { tmp[n++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
	while (n > 0 && i + 1 < max) d[i++] = tmp[--n];
	d[i] = '\0';
}

static int
p9wstat(int fd, const struct p9stat *s)
{
	struct p9buf b, r;

	begin(&b, P9_Twstat);
	p9_p32(&b, P9CL_FILEFID);
	p9_pstatw(&b, s);			/* the outer count is 9P's wart */
	if (!p9_ok(&b)) { v8_errno = V8_EIO; return (-1); }
	return (xact(fd, &b, P9_Rwstat, &r) < 0 ? -1 : 0);
}

/* Walk to the path, wstat it, drop the connection -- t_stat's shape exactly. */
static int
p9wstatpath(char *p, const struct p9stat *s)
{
	struct p9mnt *m = p9mount();
	const char *rel;
	int fd, rc;

	if (m == 0 || (rel = p9rel(m, p)) == 0) { v8_errno = V8_ENOENT; return (-1); }
	if ((fd = p9dial(m)) < 0) return (-1);
	if (p9walk(fd, rel) < 0) { rawsys1(SYS_close, fd); return (-1); }
	rc = p9wstat(fd, s);
	rawsys1(SYS_close, fd);
	return (rc);
}

/*
 * t_chmod.  Masked to 07777 for p9_t_mkdir's reason inverted: there the bit
 * above 0777 is DMDIR and a mode carrying S_IFDIR must not reach it, and here
 * the whole point is that the file type is not the caller's to change.  V7's
 * chmod is `ip->i_mode &= ~07777; ip->i_mode |= fmode&07777' (sys4.c:249,252),
 * so upstream masks in the same place and the server masks again.
 */
static int
p9_t_chmod(char *p, int mode)
{
	struct p9stat s;

	p9_nostat(&s);
	s.s_mode = (p9_u32)(mode & 07777);
	return (p9wstatpath(p, &s));
}

/*
 * t_chown.  BOTH IDS ARE ALWAYS SENT, because V7's chown always sets both --
 * there is no -1 "leave it alone" convention in sys4.c:292-296, that is POSIX
 * arriving later.  So the empty-string "do not touch" form is not used here at
 * all, and a caller who wanted one field would have to read the other first,
 * which is what a V7 program does.
 *
 * THE ID IS FOLDED BEFORE IT GOES ON THE WIRE, AND THAT IS THE FOURTH
 * NARROWING SITE.  An auditor found the two ends of this field disagreeing:
 * the server truncates with V7's own `ip->i_uid = uap->uid' (an int into a
 * short), and p9uid() at the READING end refuses any string beginning with
 * `-'.  So chown(f, 40000) stored -25536 and stat(2) read it back as
 * P9UID_BAD -- a value that does not round-trip through the port's own two
 * halves, and one that disagrees with what stat reports for the same host id
 * on the passthrough type.
 *
 * Folding here fixes both at once: v8_foldid never returns a negative, so the
 * number the server stores is the number statof renders is the number p9uid
 * parses, AND it is the same number stat_translate would have produced for
 * that host id on a jailed path.  A mount and the jail agree about who owns a
 * file, which is the property worth having.
 *
 * WHAT IT COSTS is V7's answer to chown(f, -1), which truncated to 65535.
 * Nothing in the tree makes that call -- mkdir.c:69 passes getuid()/getgid()
 * -- and the rule this port has settled on is that a HOST id entering a V8
 * 16-bit field is folded.  An image is such a field.  shim/v8id.h.
 */
static int
p9_t_chown(char *p, int uid, int gid)
{
	struct p9stat s;

	p9_nostat(&s);
	p9dec(s.s_uid, (long)sizeof s.s_uid, v8_foldid((long)uid));
	p9dec(s.s_gid, (long)sizeof s.s_gid, v8_foldid((long)gid));
	return (p9wstatpath(p, &s));
}

/*
 * t_utime, and it is the slot mv(1) needs.
 *
 * WHY mv IS THE CONSUMER: on a mount link(2) has no slot and is refused, so
 * mv.c:114 always fails and mv always takes the fallback -- fork, exec
 * /bin/cp, wait, then mv.c:129's `utime(target, &s1.st_atime)' to put the
 * source's timestamps on the copy.  Without this slot the copy and the unlink
 * both succeed and only the times are silently lost.
 *
 * A NULL tv IS "NOW" HERE AND IS NOT WHAT A VAX DID, and the difference is
 * older than this file.  V7's utime copyin's from the address it is handed
 * (sys4.c:533), so utime(f, 0) on a VAX read eight bytes of user address 0 --
 * crt0, since a.out text starts there -- and stamped the file with them.  The
 * shim has always answered "now" instead, because v8s_utime passes a null
 * timeval to SYS_utimes and that is what macOS means by it.  Reproduced rather
 * than widened: the mount gives the same answer the passthrough type already
 * gives, so a program cannot tell the two apart by asking.
 *
 * THE 32-BIT TRUNCATION IS THE DISK'S WIDTH, NOT A LOSS THIS INTRODUCES:
 * di_atime and di_mtime are v8_i32 on the image, so a time_t past 2038 could
 * not be stored whatever this line did.  Worth saying because the passthrough
 * type does NOT truncate -- the host keeps 64 bits -- so the two filesystems
 * genuinely differ up there, and the difference belongs to the format.
 *
 * THE SENTINEL COLLIDES WITH A REAL TIME, at exactly one value.  "Do not
 * touch" is all ones, and a time_t of -1 -- 31 December 1969, which a signed
 * di_atime can hold perfectly well -- encodes to the same 0xffffffff.  There
 * is no room in the encoding for both, so the request is refused rather than
 * silently dropped, which is the difference between a caller that can see what
 * happened and one that cannot.
 */
static int
p9_t_utime(char *p, long *tv)
{
	struct p9stat s;
	struct { long sec, usec; } now;
	long at, mt;

	if (tv == 0) {
		if (rawsys2(SYS_gettimeofday, (long)&now, 0) < 0) {
			v8_errno = V8_EINVAL;
			return (-1);
		}
		at = mt = now.sec;
	} else {
		at = tv[0];
		mt = tv[1];
	}
	if ((p9_u32)at == (p9_u32)~0U || (p9_u32)mt == (p9_u32)~0U) {
		v8_errno = V8_EINVAL;
		return (-1);
	}
	p9_nostat(&s);
	s.s_atime = (p9_u32)at;
	s.s_mtime = (p9_u32)mt;
	return (p9wstatpath(p, &s));
}

struct v8fstyp v8fs_p9 = {
	"v8fs",
	p9_t_path,
	p9_t_open, p9_t_close,
	p9_t_read, p9_t_write, p9_t_seek,
	p9_t_stat, p9_t_fstat,
	p9_t_ioctl,
	p9_t_access, p9_t_remove, p9_t_mkdir,
	p9_t_chmod, p9_t_chown, p9_t_utime
};

/* ------------------------------------------------------- directory reads */

/*
 * A DIRECTORY IS CONVERTED AT OPEN, not read through.  The two record formats
 * have nothing in common -- V7's is a fixed 256 bytes here, 9P's is variable
 * and self-describing -- so a byte offset in one is not a byte offset in the
 * other, and there is no mapping short of a table.  dir.c reached the same
 * conclusion about the host's getdirentries for the same reason and has done
 * it that way since it was written; this hands the result to the same
 * machinery rather than building a second one.
 *
 * THE INODE NUMBER SURVIVES INTACT, which the host path cannot manage.  A qid
 * path here IS a V7 i_number: 16 bits, assigned by the filesystem being
 * served, and the same number the directory entry on that disk holds.  So no
 * fold, no collision class, and getwd's identity comparison is the real one.
 * Asserted rather than assumed -- a qid above 65535 could only come from a
 * server that is not this one, and it is refused rather than truncated,
 * because a truncated inode number is the silently-wrong answer that
 * v8sys_fold_ino's whole history is about.
 */
int
v8fs_p9dirsnap(int fd)
{
	struct p9buf b, r;
	struct p9stat s;
	struct v8_direct rec;
	char *buf;
	long cap, used, i, k;
	p9_u64 off = 0;
	p9_u32 count;
	unsigned char *d;

	cap = 32 * (long)sizeof rec;
	if ((buf = v8sys_alloc(cap)) == 0) { v8_errno = V8_ENOMEM; return (-1); }
	used = 0;

	for (;;) {
		struct p9buf ent;

		begin(&b, P9_Tread);
		p9_p32(&b, P9CL_FILEFID);
		p9_p64(&b, off);	/* explicit: the cursor is not ours */
		p9_p32(&b, (p9_u32)(P9_MSIZE - P9_IOHDRSZ));
		if (xact(fd, &b, P9_Rread, &r) < 0) goto fail;
		count = p9_g32(&r);
		if (!p9_ok(&r)) { v8_errno = V8_EIO; goto fail; }
		if (count == 0) break;
		if ((d = p9_gdata(&r, (long)count)) == 0) {
			v8_errno = V8_EIO;
			goto fail;
		}
		off += count;

		/*
		 * NO OUTER COUNT HERE, AND p9statfid ABOVE HAS ONE -- the
		 * asymmetry is the spec's and getting it wrong cost a debug
		 * round.  Rstat wraps ONE stat in the message's own `stat[n]'
		 * field, which is a 2-byte count in front of a structure whose
		 * first field is a 2-byte count; a directory READ is a plain
		 * sequence of the structures, each with only its own.  So the
		 * extra p9_g16 that belongs there is a bug here: it eats the
		 * first entry's size and every field after it decodes shifted.
		 *
		 * It presented as `ls: /mnt unreadable', because open(2) is
		 * what fails when the snapshot cannot be built.
		 */
		p9_init(&ent, d, (long)count);
		while (p9_len(&ent) < (long)count) {
			if (p9_gstat(&ent, &s) < 0) { v8_errno = V8_EIO; goto fail; }
			if (s.s_qid.q_path > 65535ULL) { v8_errno = V8_EIO; goto fail; }

			if (used + (long)sizeof rec > cap) {
				char *nb;
				long j;

				cap *= 2;
				if ((nb = v8sys_alloc(cap)) == 0) {
					v8_errno = V8_ENOMEM;
					goto fail;
				}
				for (j = 0; j < used; j++) nb[j] = buf[j];
				v8sys_free(buf);
				buf = nb;
			}
			rec.d_ino = (v8_ino_t)s.s_qid.q_path;
			for (k = 0; k < V8_DIRSIZ; k++) rec.d_name[k] = '\0';
			for (k = 0; k < V8_DIRSIZ && s.s_name[k]; k++)
				rec.d_name[k] = s.s_name[k];
			for (i = 0; i < (long)sizeof rec; i++)
				buf[used + i] = ((char *)&rec)[i];
			used += (long)sizeof rec;
		}
		if (!p9_ok(&ent)) { v8_errno = V8_EIO; goto fail; }
	}

	if (v8sys_diradopt(fd, buf, used) < 0) goto fail;
	return (0);
fail:
	v8sys_free(buf);
	return (-1);
}
