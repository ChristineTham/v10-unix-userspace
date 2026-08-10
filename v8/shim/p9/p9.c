/*
 * 9P2000 encode and decode.  p9.h says why this is one file compiled into two
 * very different programs, and why it names no libc function.
 *
 * BYTE AT A TIME, NEVER A CAST, and that is not a style choice.  9P is
 * little-endian by specification, and this host is little-endian, so
 * `*(p9_u32 *)p = v' would produce the right bytes -- by coincidence of the
 * machine, which is exactly the reading src/include/PORTING.md had to unpick
 * for the on-disk records (`int di_size' did not mean "an int", it meant
 * "exactly four bytes, because a VAX wrote four bytes there").  A wire format
 * has an end that is not ours.  It would also be an unaligned store: a message
 * buffer is a byte array and Twrite's count[4] lands at offset 19.
 */

#include "p9.h"

/*
 * No libc, so the two things every codec borrows are here.  They are byte
 * loops on purpose and every rule that compiles this file passes no -O, for
 * exactly this reason: clang's loop-idiom pass rewrites them as calls to the
 * C library, which is the one thing libv8sys may not import.  The Makefile
 * argues it at SHIMFLAGS.
 *
 * MEASURED, BECAUSE THE FIRST VERSION OF THIS COMMENT NAMED THE WRONG LOOP AND
 * THE WRONG SYMBOL.  It said the copy becomes `memcpy'.  Compiling this file
 * at -O2, -O3 and -Os, the symbol that appears is `_strlen', every time, and
 * never memcpy -- `scopy' survives as a byte loop at every level, because
 * clang cannot prove d and s do not alias, and it is `slen', inlined into
 * p9_pstr and p9_pstat, that gets rewritten.  It also said the server is
 * built with -O and the rewrite is harmless there; V8FSFLAGS is SHIMFLAGS
 * plus four -D and -Wno- flags and carries no -O either, so there is no
 * optimised build of this file anywhere.
 *
 * AND THE SAFETY NET IS NOT UNDER THIS FILE YET.  dir.c can afford the same
 * byte loops because tests/kmemu sweeps every Mach-O in the rootfs with nm -u
 * against an empty allowed list, so a leak lands as a red suite.  That sweep
 * sees a name only if some binary PULLS THE MEMBER IN, and nothing references
 * p9_* until the client type exists -- so p9.o is in libv8sys.a and in none of
 * the 98 binaries.  Adding -O to SHIMFLAGS today would put _strlen in the
 * archive and every suite would stay green.  The client closes it.
 */
static long
slen(const char *s)
{
	long n = 0;

	while (s[n]) n++;
	return (n);
}

static void
scopy(unsigned char *d, const unsigned char *s, long n)
{
	long i;

	for (i = 0; i < n; i++) d[i] = s[i];
}

/* ------------------------------------------------------------- the cursor */

void
p9_init(struct p9buf *b, void *base, long len)
{
	b->b_base = (unsigned char *)base;
	b->b_p = b->b_base;
	b->b_end = b->b_base + (len < 0 ? 0 : len);
	b->b_bad = 0;
}

int
p9_ok(struct p9buf *b)
{
	return (b->b_bad == 0);
}

long
p9_len(struct p9buf *b)
{
	return (b->b_p - b->b_base);
}

/*
 * The one place that decides whether n more bytes are available.  Every put
 * and every get is written in terms of it, so "did anyone forget a bounds
 * check" is a question about this function rather than about forty call sites.
 */
static int
room(struct p9buf *b, long n)
{
	if (b->b_bad) return (0);
	if (n < 0 || b->b_end - b->b_p < n) { b->b_bad = 1; return (0); }
	return (1);
}

/* ------------------------------------------------------------------- put */

void
p9_p8(struct p9buf *b, p9_u32 v)
{
	if (!room(b, 1)) return;
	*b->b_p++ = (unsigned char)(v & 0xff);
}

void
p9_p16(struct p9buf *b, p9_u32 v)
{
	if (!room(b, 2)) return;
	*b->b_p++ = (unsigned char)(v & 0xff);
	*b->b_p++ = (unsigned char)((v >> 8) & 0xff);
}

void
p9_p32(struct p9buf *b, p9_u32 v)
{
	if (!room(b, 4)) return;
	*b->b_p++ = (unsigned char)(v & 0xff);
	*b->b_p++ = (unsigned char)((v >> 8) & 0xff);
	*b->b_p++ = (unsigned char)((v >> 16) & 0xff);
	*b->b_p++ = (unsigned char)((v >> 24) & 0xff);
}

void
p9_p64(struct p9buf *b, p9_u64 v)
{
	int i;

	if (!room(b, 8)) return;
	for (i = 0; i < 8; i++)
		*b->b_p++ = (unsigned char)((v >> (i * 8)) & 0xff);
}

/*
 * A 9P string is count[2] followed by that many bytes and NO terminator.  A
 * null pointer encodes as the empty string, because that is what every caller
 * that has nothing to say means, and the alternative is a fault at the seam.
 */
void
p9_pstr(struct p9buf *b, const char *s)
{
	long n = s ? slen(s) : 0;

	if (n > 0xffff) { b->b_bad = 1; return; }
	p9_p16(b, (p9_u32)n);
	if (!room(b, n)) return;
	scopy(b->b_p, (const unsigned char *)s, n);
	b->b_p += n;
}

void
p9_pdata(struct p9buf *b, const void *p, long n)
{
	if (!room(b, n)) return;
	scopy(b->b_p, (const unsigned char *)p, n);
	b->b_p += n;
}

void
p9_pqid(struct p9buf *b, const struct p9qid *q)
{
	p9_p8(b, q->q_type);
	p9_p32(b, q->q_vers);
	p9_p64(b, q->q_path);
}

/*
 * A stat has TWO length prefixes and this is 9P2000's one real wart, so it is
 * spelled out rather than left to be rediscovered: the message field is
 * stat[n], i.e. a 2-byte count and then that many bytes, and the bytes
 * themselves begin with the structure's own size[2], which counts everything
 * after itself.  So the two numbers differ by exactly two, and a reader that
 * conflates them is off by two at the START of the name -- which decodes as a
 * plausible short string rather than as an error.
 *
 * This function writes only the inner one.  The message field's outer count is
 * the caller's, because Twstat has one too and only Rstat's is preceded by
 * nothing else.
 */
void
p9_pstat(struct p9buf *b, const struct p9stat *s)
{
	unsigned char *sz = b->b_p;
	long n;

	p9_p16(b, 0);				/* patched below */
	p9_p16(b, s->s_type);
	p9_p32(b, s->s_dev);
	p9_pqid(b, &s->s_qid);
	p9_p32(b, s->s_mode);
	p9_p32(b, s->s_atime);
	p9_p32(b, s->s_mtime);
	p9_p64(b, s->s_length);
	p9_pstr(b, s->s_name);
	p9_pstr(b, s->s_uid);
	p9_pstr(b, s->s_gid);
	p9_pstr(b, s->s_muid);
	if (b->b_bad) return;
	n = (b->b_p - sz) - 2;
	sz[0] = (unsigned char)(n & 0xff);
	sz[1] = (unsigned char)((n >> 8) & 0xff);
}

/*
 * p9_pstatw -- a stat WITH the outer count in front of it.
 *
 * 9P2000's one real wart: the message field is stat[n], and the n bytes inside
 * it open with the structure's OWN size[2].  The two counts differ by exactly
 * two, and both have to be patched after the fact because neither length is
 * known until the strings are encoded.  Tstat's reply and Twstat carry the
 * wrapped form; a directory read carries bare stats concatenated with no outer
 * count at all, which is why p9_pstat stays exported beside this.
 *
 * IT IS A FUNCTION BECAUSE THERE ARE NOW TWO CALL SITES.  The server's Rstat
 * had these eight lines with a comment explaining the wart, and the client's
 * Twstat would have been a second hand-rolled copy -- which is how the two ends
 * of one protocol come to disagree by two bytes, and the disagreement decodes.
 * The decode side already shows the shape: p9statfid and do_wstat each spend a
 * bare p9_g16 on the outer count with a comment saying which one it is.
 */
void
p9_pstatw(struct p9buf *b, const struct p9stat *s)
{
	unsigned char *outer = b->b_p;
	long n;

	p9_p16(b, 0);
	p9_pstat(b, s);
	if (b->b_bad) return;
	n = (b->b_p - outer) - 2;
	outer[0] = (unsigned char)(n & 0xff);
	outer[1] = (unsigned char)((n >> 8) & 0xff);
}

/*
 * p9_nostat -- the stat in which every field says DO NOT TOUCH.
 *
 * Twstat's rule is that a field the client does not mean to change is sent as
 * all ones, so a wstat is built by starting from this and setting the one or
 * two fields that are the request.  A server given a stat that was merely
 * zeroed would read every field as meant and, as do_wstat's own header comment
 * puts it, zero a file's mode every time somebody set its length.
 *
 * THE STRINGS ARE THE ASYMMETRY and they are why this is a function rather
 * than a memset of 0xff: all-ones has no spelling in a string field, so the
 * "do not touch" form for s_name, s_uid, s_gid and s_muid is the EMPTY string.
 * Filling the struct with 0xff would send four 255-byte names.
 */
void
p9_nostat(struct p9stat *s)
{
	long i;
	char *q = (char *)s;

	for (i = 0; i < (long)sizeof *s; i++) q[i] = 0;	/* the strings, and only they, stay */
	s->s_type      = (p9_u16)~0U;
	s->s_dev       = (p9_u32)~0U;
	s->s_qid.q_type = (p9_u8)~0U;
	s->s_qid.q_vers = (p9_u32)~0U;
	s->s_qid.q_path = (p9_u64)~0ULL;
	s->s_mode      = (p9_u32)~0U;
	s->s_atime     = (p9_u32)~0U;
	s->s_mtime     = (p9_u32)~0U;
	s->s_length    = (p9_u64)~0ULL;
}

/* ------------------------------------------------------------------- get */

p9_u32
p9_g8(struct p9buf *b)
{
	if (!room(b, 1)) return (0);
	return (*b->b_p++);
}

p9_u32
p9_g16(struct p9buf *b)
{
	p9_u32 v;

	if (!room(b, 2)) return (0);
	v = b->b_p[0] | ((p9_u32)b->b_p[1] << 8);
	b->b_p += 2;
	return (v);
}

p9_u32
p9_g32(struct p9buf *b)
{
	p9_u32 v;

	if (!room(b, 4)) return (0);
	v = b->b_p[0] | ((p9_u32)b->b_p[1] << 8) |
	    ((p9_u32)b->b_p[2] << 16) | ((p9_u32)b->b_p[3] << 24);
	b->b_p += 4;
	return (v);
}

p9_u64
p9_g64(struct p9buf *b)
{
	p9_u64 v = 0;
	int i;

	if (!room(b, 8)) return (0);
	for (i = 7; i >= 0; i--)
		v = (v << 8) | b->b_p[i];
	b->b_p += 8;
	return (v);
}

/*
 * A counted string into a terminated buffer, and the copy is the whole reason
 * this is a function rather than two lines at each call site.
 *
 * IT REFUSES RATHER THAN TRUNCATES.  A name that does not fit cannot be
 * described truthfully, which is the sentence src/include/PORTING.md arrived
 * at over FSNMLG: truncating a path does not shorten a column, it sends the
 * reader down the arm for a different kind of object.  And returning a pointer
 * into the message instead of copying would hand the caller an UNTERMINATED
 * string -- the %.Ns bug in doprnt.c, one layer up.
 */
long
p9_gstr(struct p9buf *b, char *dst, long max)
{
	long n = (long)p9_g16(b);

	if (b->b_bad) { if (max > 0) dst[0] = '\0'; return (-1); }
	if (n >= max) { b->b_bad = 1; if (max > 0) dst[0] = '\0'; return (-1); }
	if (!room(b, n)) { if (max > 0) dst[0] = '\0'; return (-1); }
	scopy((unsigned char *)dst, b->b_p, n);
	dst[n] = '\0';
	b->b_p += n;
	return (n);
}

/*
 * n bytes in place.  The caller gets a pointer into the message rather than a
 * copy, which is right for Rread's payload -- it is about to be handed to the
 * program's own buffer and copying it twice is the sort of thing that makes a
 * filesystem slow for no reason.  Null on underrun, which every caller must
 * check; there is no useful empty answer for "here is where the data is".
 */
unsigned char *
p9_gdata(struct p9buf *b, long n)
{
	unsigned char *p;

	if (!room(b, n)) return (0);
	p = b->b_p;
	b->b_p += n;
	return (p);
}

void
p9_gqid(struct p9buf *b, struct p9qid *q)
{
	q->q_type = (p9_u8)p9_g8(b);
	q->q_vers = p9_g32(b);
	q->q_path = p9_g64(b);
}

/*
 * The inner size[2] is READ AND HONOURED rather than skipped, because a
 * 9P2000 stat is explicitly extensible: a server may append fields this port
 * does not know, and the size is how a reader steps over them.  Skipping it
 * would work against our own server today and desynchronise the stream
 * against u9fs tomorrow -- and the failure would land on the NEXT message,
 * which is the hardest possible place to diagnose.
 */
int
p9_gstat(struct p9buf *b, struct p9stat *s)
{
	long n;
	unsigned char *end;

	n = (long)p9_g16(b);
	if (b->b_bad) return (-1);
	if (b->b_end - b->b_p < n) { b->b_bad = 1; return (-1); }
	end = b->b_p + n;

	s->s_type = (p9_u16)p9_g16(b);
	s->s_dev = p9_g32(b);
	p9_gqid(b, &s->s_qid);
	s->s_mode = p9_g32(b);
	s->s_atime = p9_g32(b);
	s->s_mtime = p9_g32(b);
	s->s_length = p9_g64(b);
	if (p9_gstr(b, s->s_name, sizeof s->s_name) < 0) return (-1);
	if (p9_gstr(b, s->s_uid, sizeof s->s_uid) < 0) return (-1);
	if (p9_gstr(b, s->s_gid, sizeof s->s_gid) < 0) return (-1);
	if (p9_gstr(b, s->s_muid, sizeof s->s_muid) < 0) return (-1);
	if (b->b_bad) return (-1);
	if (b->b_p > end) { b->b_bad = 1; return (-1); }
	b->b_p = end;				/* step over any extension */
	return (0);
}

/* ------------------------------------------------------------- the framing */

void
p9_hdr(struct p9buf *b, void *base, long max, int type, p9_u32 tag)
{
	p9_init(b, base, max);
	p9_p32(b, 0);				/* size, patched by p9_fin */
	p9_p8(b, (p9_u32)type);
	p9_p16(b, tag);
}

long
p9_fin(struct p9buf *b)
{
	long n = p9_len(b);

	if (b->b_bad || n < P9_HDRSZ) return (-1);
	b->b_base[0] = (unsigned char)(n & 0xff);
	b->b_base[1] = (unsigned char)((n >> 8) & 0xff);
	b->b_base[2] = (unsigned char)((n >> 16) & 0xff);
	b->b_base[3] = (unsigned char)((n >> 24) & 0xff);
	return (n);
}

/*
 * SHORT TRANSFERS ARE THE WHOLE JOB OF THESE TWO, and it is why the loop is
 * not written twice.  A stream socket may hand back any prefix of what was
 * asked for, and on a message-framed protocol a caller that treats one read as
 * one message works perfectly until the day a message spans a segment -- after
 * which every subsequent message on that connection is misaligned, and the
 * symptom is a garbage message type rather than a short read.
 *
 * EINTR is deliberately NOT handled here: it needs the errno, which does not
 * fit through a `long' return, and the two implementations of p9_io_read have
 * two different ways of seeing it (a negated errno from rawsys, and the libc
 * global).  So each side retries in its own p9_io_read, and this loop's
 * contract is only about counts.
 */
long
p9_send(int fd, struct p9buf *b)
{
	long n = p9_fin(b), off = 0, k;

	if (n < 0) return (-1);
	while (off < n) {
		k = p9_io_write(fd, b->b_base + off, n - off);
		if (k <= 0) return (-1);
		off += k;
	}
	return (n);
}

/*
 * One whole message into buf.  Returns its length, 0 if the peer closed before
 * sending anything, or -1.
 *
 * A CLEAN EOF AND A TRUNCATED MESSAGE ARE DIFFERENT ANSWERS.  The server exits
 * when a client goes away, and it must not report that as a protocol error;
 * but a peer that sends three bytes of a header and vanishes has broken the
 * stream, and continuing to read the connection would resynchronise on
 * whatever came next.  So end of file is only clean at offset 0.
 */
long
p9_recv(int fd, void *buf, long max)
{
	unsigned char *p = (unsigned char *)buf;
	long off = 0, k, n;

	/*
	 * THE CALLER'S BOUND IS CHECKED BEFORE THE FIRST READ, AND THE FIRST
	 * VERSION OF THIS FUNCTION DID NOT DO THAT.  The size field has to be
	 * in the buffer before it can be parsed, so the header read below is
	 * issued against p + off -- and `max' was not consulted until the test
	 * twenty lines down, by which time four bytes were already written.
	 * With max 2 that put two bytes past the end of a two-byte buffer;
	 * with max 0, four.  The return value was a correct -1 either way, so
	 * a caller checking it learned nothing.
	 *
	 * It was latent rather than live -- every caller today passes a whole
	 * message buffer -- and it becomes live the first time a program's own
	 * count reaches here, which is what a V8 `read(fd, buf, 1)' on a
	 * server-backed descriptor is.  Found by the lp64-auditor, which is
	 * the third time that subagent has found its bug in the shim code
	 * written that hour rather than in the 1985 half.
	 */
	if (max < P9_HDRSZ) return (-1);

	while (off < 4) {
		k = p9_io_read(fd, p + off, 4 - off);
		if (k == 0) return (off == 0 ? 0 : -1);
		if (k < 0) return (-1);
		off += k;
	}
	n = (long)p[0] | ((long)p[1] << 8) | ((long)p[2] << 16) |
	    ((long)p[3] << 24);
	/*
	 * Both bounds matter and they fail differently.  Under P9_HDRSZ there
	 * is no type byte, so the loop below would return a message the
	 * dispatcher reads fields out of that were never sent; over max it
	 * would write past the caller's buffer, which is the one that is not
	 * merely a wrong answer.
	 */
	if (n < P9_HDRSZ || n > max) return (-1);
	while (off < n) {
		k = p9_io_read(fd, p + off, n - off);
		if (k <= 0) return (-1);
		off += k;
	}
	return (n);
}
