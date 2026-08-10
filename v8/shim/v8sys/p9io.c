/*
 * The client's half of the 9P transport seam -- shim/p9/p9.h declares these
 * two and says why they are the only thing the shared codec cannot supply.
 *
 * Raw syscalls only, like the rest of shim/v8sys.  Only headers of CONSTANTS
 * are included: <errno.h> for the host's EINTR, which pulls in no symbol as
 * long as nothing here reads `errno' itself (on Darwin that expands to a call
 * to __error()).  syscall.c records the same rule at its top.
 */

#include <errno.h>
#include "rawsys.h"
#include "../p9/p9.h"

/*
 * EINTR IS RETRIED HERE, AND THAT IS V8's BEHAVIOUR RATHER THAN A DEPARTURE
 * FROM IT.  shim/NOTES.md:46 records that this port deliberately does not set
 * SA_RESTART, because a V8 program expects a slow read to fail with EINTR.
 * That is a rule about the read(2) the program called, and this is not it --
 * this is the transport underneath, in the middle of a message.  Abandoning a
 * half-read Rread does not fail one read; it leaves the connection
 * permanently misaligned, and every later message on it decodes as garbage.
 *
 * The authority for retrying is Bell Labs' own, one layer down: bio.c:432 is
 * `sleep((caddr_t)bp, PRIBIO)', and PRIBIO is 20 against PZERO 25 -- below the
 * interruptible boundary, so a signal does not abort a disk read on a VAX
 * either.  bio.c:695 says so in as many words, `can't happen at PRIBIO+1'.
 * A 9P round trip standing in for a bread inherits that, so a V8 program
 * sees the same thing here as it would have seen there.
 */
long
p9_io_read(int fd, void *buf, long n)
{
	long r;

	for (;;) {
		r = rawsys3(SYS_read, fd, (long)buf, n);
		if (r >= 0) return (r);
		if (RAWERR(r) != EINTR) return (-1);
	}
}

long
p9_io_write(int fd, const void *buf, long n)
{
	long r;

	for (;;) {
		r = rawsys3(SYS_write, fd, (long)buf, n);
		if (r >= 0) return (r);
		if (RAWERR(r) != EINTR) return (-1);
	}
}
