/*
 * /etc/utmp -- who is logged in.
 *
 * THIS IS THE FILE THE EXCEPTION IS FOR.  <utmpx.h> is host libc, and it is the
 * only thing taken from it here: getutxent(3) and its two bookends.  The record
 * layout, the field widths and the write all go through this port's own code.
 * synth.c has the reasoning and the boundary.
 */

#include "kmemu.h"
#include <utmpx.h>

/*
 * V8's struct utmp, from usr/include/utmp.h:
 *
 *	struct utmp {
 *		char	ut_line[8];	-- tty name
 *		char	ut_name[8];	-- user id
 *		long	ut_time;	-- time on
 *	};
 *
 * Spelled again rather than included, because this file is compiled by clang
 * for the host and that header belongs to the V8 include tree; mixing the two
 * worlds' headers in one translation unit is a hazard with no upside.
 *
 * TWENTY-FOUR BYTES, not the VAX's twenty.  ut_time is a long and this port's
 * long is 64 bits, so the record grew and gained four bytes of padding.  That
 * is only safe because NOTHING ELSE in this world reads or writes utmp -- there
 * is no 1985 file to stay compatible with, only the two ends of this seam,
 * which are this file and a `who' that v8cc compiled against the authentic
 * header.  tests/kmemu asserts they agree, by size and by content, because a
 * disagreement here would print plausible garbage rather than fail.
 */
struct v8utmp {
	char	ut_line[8];
	char	ut_name[8];
	long	ut_time;
};

/*
 * Enough for any plausible login count; the array is static so this costs no
 * allocator.  Sessions past the cap are dropped rather than misreported.
 */
#define MAXUT 128

int
kmemu_utmp(const char *hostpath)
{
	static struct v8utmp rec[MAXUT];
	struct utmpx *u;
	int n = 0;

	/*
	 * USER_PROCESS only.  The host's utmpx also carries BOOT_TIME,
	 * SHUTDOWN_TIME and dead-session records, which V7 had no type field to
	 * express; it marked a vacated line by zeroing ut_name instead, and
	 * who(1) skips those.  Emitting only live logins is the same answer by
	 * the other route.  (BOOT_TIME is what w(1) will want for uptime -- it
	 * belongs in a separate synthetic file, not smuggled into this one.)
	 */
	setutxent();
	while (n < MAXUT && (u = getutxent()) != 0) {
		if (u->ut_type != USER_PROCESS) continue;
		kmemu_field(rec[n].ut_line, (long)sizeof rec[n].ut_line,
		    u->ut_line, (long)sizeof u->ut_line);
		kmemu_field(rec[n].ut_name, (long)sizeof rec[n].ut_name,
		    u->ut_user, (long)sizeof u->ut_user);
		rec[n].ut_time = (long)u->ut_tv.tv_sec;
		n++;
	}
	endutxent();

	/*
	 * ut_host is dropped: V7's utmp has nowhere to put it, so a remote login
	 * reads as a local one on the same tty.  Documented loss, not a bug --
	 * the same shape as dir.c truncating a name to 14 characters.
	 */
	return (kmemu_replace(hostpath, (const char *)rec,
	    (long)n * (long)sizeof rec[0]));
}
