/*
 * The timezone, for ftime(2).
 *
 * WHY THE SHIM OWNS THIS.  On the VAX the timezone was a kernel variable, set
 * at boot and handed out by ftime(2); V8's localtime() does nothing but ask for
 * it and subtract.  This file is that variable.  macOS has no such variable --
 * the kernel stopped tracking one, and its libc computes the offset per call
 * from the zone database -- so the shim has to read the database itself.
 *
 * WHY NOT JUST CALL THE HOST'S ftime.  That is what was happening, silently,
 * until tests/kmemu started asserting on `nm -u': ftime was missing from this
 * layer, so it resolved out of libSystem and date(1), ls(1) and who(1) were all
 * quietly using host libc.  Nothing was visibly wrong, which is the entire
 * problem -- see the note above v8s_ftime in syscall.c.
 *
 * AND WHY NOT THE gettimeofday SYSCALL'S SECOND ARGUMENT, which is a
 * struct timezone * and looks exactly like the answer.  Measured: the raw
 * syscall writes something else there.  Passing it a zeroed struct came back
 * with 775410594 minutes west, and date(1) then printed a day in the following
 * week.  libc's gettimeofday DOES return -600 -- which is what made this look
 * verified when it was not.  Measuring the wrapper and believing it says
 * something about the syscall is the same mistake as running inv(1) with no
 * stdin: the number was real, it was just not a measurement of this path.
 *
 * READING /etc/localtime IS ALLOWED WHERE READING /var/run/utmpx IS NOT, and
 * the difference is the one PLAN.md section 7 already draws.  TZif is a
 * published, versioned, byte-for-byte specified format (RFC 8536) that every
 * Unix reads with its own code.  utmpx's on-disk layout is private, undocumented
 * and does not match its own documented struct.  One is an interface; the other
 * is a guess that happens to work.
 */

#include "v8sys.h"
#include "rawsys.h"

/*
 * Australia/Sydney is 2190 bytes; the largest zone in the database is a few
 * kilobytes.  A file bigger than this is not a zone file and is refused rather
 * than parsed from a truncated buffer.
 */
#define TZBUFSZ 8192

static unsigned char tzbuf[TZBUFSZ];

static long
be32(const unsigned char *p)			/* signed */
{
	long v = ((long)p[0] << 24) | ((long)p[1] << 16) |
	         ((long)p[2] << 8)  |  (long)p[3];
	return (v & 0x80000000L) ? v - 0x100000000L : v;
}

static long
be64(const unsigned char *p)
{
	long v = 0;
	int i;

	for (i = 0; i < 8; i++) v = (v << 8) | p[i];
	return (v);
}

/*
 * One TZif header: six counts at offset 20, data starting at 44.  Returns the
 * size of the data block that follows, or -1 if the header is not one.
 * `wide' selects the 8-byte transition times and 12-byte leap records of a
 * version 2+ block over the 4-byte and 8-byte ones of the legacy block.
 */
struct tzhdr { long isut, isstd, leap, ntime, ntype, nchar; int version; };

static long
tzheader(const unsigned char *p, long avail, struct tzhdr *h, int wide)
{
	long n;

	if (avail < 44) return (-1);
	if (p[0] != 'T' || p[1] != 'Z' || p[2] != 'i' || p[3] != 'f') return (-1);
	h->version = p[4];
	h->isut  = be32(p + 20);
	h->isstd = be32(p + 24);
	h->leap  = be32(p + 28);
	h->ntime = be32(p + 32);
	h->ntype = be32(p + 36);
	h->nchar = be32(p + 40);
	if (h->ntime < 0 || h->ntype <= 0 || h->nchar < 0 ||
	    h->leap < 0 || h->isut < 0 || h->isstd < 0) return (-1);

	n = h->ntime * (wide ? 8 : 4)		/* transition times */
	  + h->ntime				/* one type index each */
	  + h->ntype * 6			/* utoff, isdst, desigidx */
	  + h->nchar				/* the "AEST\0AEDT\0" strings */
	  + h->leap * (wide ? 12 : 8)
	  + h->isstd + h->isut;
	if (n < 0 || n > avail - 44) return (-1);
	return (n);
}

/*
 * Seconds east of UT at `now', from the type in force at the last transition
 * at or before it.  Returns 0 and leaves *found 0 if anything about the file
 * does not parse -- UT is the only defensible fallback, and a wrong offset is
 * worse than none because every timestamp in the world silently shifts.
 */
static long
tzoffset(long now, int *found)
{
	struct tzhdr h;
	const unsigned char *p, *times, *types, *info;
	long fd, n, blk, i, sel, width;

	*found = 0;
	/*
	 * Raw, and deliberately NOT through vpath(): this is the kernel asking,
	 * and the jail has no zone database.  /etc/ is in the redirect list, so
	 * going the other way would look inside $V8ROOT first.
	 */
	fd = rawsys3(SYS_open, (long)"/etc/localtime", 0, 0);
	if (fd < 0) return (0);
	n = rawsys3(SYS_read, fd, (long)tzbuf, (long)sizeof tzbuf);
	rawsys1(SYS_close, fd);
	if (n < 44) return (0);

	p = tzbuf;
	width = 4;
	if ((blk = tzheader(p, n, &h, 0)) < 0) return (0);
	/*
	 * Version 2 and later repeat everything with 64-bit transition times.
	 * Prefer that block: it is the one that is guaranteed to be populated.
	 * The legacy block is allowed to be empty in a "slim" file, and a zone
	 * whose transitions all live in the second block would otherwise read
	 * as "no transitions" and answer UT with complete confidence.  macOS
	 * ships fat files today; that is not something to depend on.
	 */
	if (h.version >= '2') {
		const unsigned char *q = p + 44 + blk;
		long avail = n - (q - p), blk2;

		if ((blk2 = tzheader(q, avail, &h, 1)) >= 0) {
			p = q;
			blk = blk2;
			width = 8;
		}
	}

	times = p + 44;
	types = times + h.ntime * width;
	info  = types + h.ntime;

	/*
	 * Transitions are sorted, so the last one at or before now is the one
	 * in force.  Before the first (or with none at all), TZif says to use
	 * the first non-DST type, falling back to type 0.
	 */
	sel = -1;
	for (i = 0; i < h.ntime; i++) {
		long t = width == 8 ? be64(times + i * 8) : be32(times + i * 4);
		if (t > now) break;
		sel = types[i];
	}
	if (sel < 0) {
		for (i = 0; i < h.ntype; i++)
			if (info[i * 6 + 4] == 0) { sel = i; break; }
		if (sel < 0) sel = 0;
	}
	if (sel >= h.ntype) return (0);

	*found = 1;
	return (be32(info + sel * 6));
}

/*
 * Cached, and that is authentic rather than lazy: the VAX read one kernel
 * variable, so a program that ran across a daylight-saving change kept using
 * the offset it started with.  ls(1) calls ctime() once per file and would
 * otherwise re-read and re-parse the zone file for every line.
 *
 * The same fact makes ONE offset the whole answer: ftime(2) takes no timestamp,
 * so it cannot say what the offset was in some other month.  A file listed by
 * `ls -l' from the other side of a DST change is shown an hour out -- exactly
 * as it was on the VAX, and for the same reason.
 */
long
v8sys_tzminuteswest(void)
{
	static int done, mw;
	struct { long sec, usec; } tv;
	int found;

	if (!done) {
		tv.sec = 0;
		rawsys2(SYS_gettimeofday, (long)&tv, 0);
		mw = (int)(-tzoffset(tv.sec, &found) / 60);
		done = 1;
	}
	return (mw);
}
