/*
 * The host half of the 9P transport seam -- shim/p9/p9.h declares these two
 * and says why the codec cannot supply them.
 *
 * TWO CONSUMERS, WHICH IS WHY IT IS A FILE.  The v8fs server and
 * tests/streams/p9probe.c are both ordinary host binaries and both need
 * exactly this; a copy in each would be the same four-line retry loop twice,
 * differing only in which one somebody forgets to fix.  The client's half is
 * shim/v8sys/p9io.c, which cannot share it: libv8sys may name no libc
 * function, so it goes to the kernel through rawsys.
 *
 * EINTR is retried here for the reason p9io.c argues at length -- abandoning a
 * half-read message does not fail one request, it leaves the connection
 * permanently misaligned -- and because Bell Labs do the same one layer down,
 * at bio.c:432's sleep(bp, PRIBIO), below the interruptible boundary.
 */

#include <errno.h>
#include <unistd.h>
#include "p9.h"

long
p9_io_read(int fd, void *buf, long n)
{
	long r;

	for (;;) {
		r = read(fd, buf, (size_t)n);
		if (r >= 0) return (r);
		if (errno != EINTR) return (-1);
	}
}

long
p9_io_write(int fd, const void *buf, long n)
{
	long r;

	for (;;) {
		r = write(fd, buf, (size_t)n);
		if (r >= 0) return (r);
		if (errno != EINTR) return (-1);
	}
}
