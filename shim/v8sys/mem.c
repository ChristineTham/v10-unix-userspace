/*
 * sbrk and brk.
 *
 * V8's malloc is Ritchie's circular first-fit allocator and it grows the arena
 * with sbrk() -- probing with sbrk(0), then extending.  It is the only
 * allocator in the V8 world, so this has to work before anything else does.
 *
 * macOS has no usable brk: the call exists but is deprecated and fails, and
 * there is no contiguous heap to extend anyway.  So a break is simulated over a
 * single large anonymous mapping reserved at first use.  The mapping is
 * reserved PROT_NONE and committed in pages as the break moves, so the address
 * space is claimed up front -- guaranteeing the contiguity malloc assumes --
 * without the resident cost.
 *
 * ARENA SIZE.  1 GB.  V8 programs are small (the whole system fit in a few
 * megabytes of RAM), but troff on a large document and the compiler on a large
 * translation unit are the outliers, and running out here means a malloc
 * failure deep inside ported code rather than a clean error.  The reservation
 * costs nothing until touched.
 */

#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include "v8sys.h"

#define ARENA_SIZE	(1024L * 1024L * 1024L)

static char *arena_base;	/* start of the reservation */
static char *arena_brk;		/* the current break */
static char *arena_end;		/* end of the reservation */
static char *arena_committed;	/* everything below this is readable/writable */

static int
arena_init(void)
{
	void *p;

	if (arena_base) return (0);
	p = mmap(0, ARENA_SIZE, PROT_NONE, MAP_PRIVATE|MAP_ANON, -1, 0);
	if (p == MAP_FAILED) { errno = ENOMEM; return (-1); }
	arena_base = arena_brk = arena_committed = (char *)p;
	arena_end = arena_base + ARENA_SIZE;
	return (0);
}

/* Make everything below `upto` readable and writable. */
static int
commit(char *upto)
{
	long pagesz = sysconf(_SC_PAGESIZE);
	char *want;

	if (upto <= arena_committed) return (0);
	want = (char *)(((unsigned long)upto + pagesz - 1) & ~(unsigned long)(pagesz - 1));
	if (want > arena_end) { errno = ENOMEM; return (-1); }
	if (mprotect(arena_committed, want - arena_committed,
	    PROT_READ|PROT_WRITE) < 0)
		return (-1);
	arena_committed = want;
	return (0);
}

char *
v8s_sbrk(long inc)
{
	char *old;

	if (arena_init() < 0) { v8sys_fail(); return ((char *)-1); }
	old = arena_brk;
	if (inc == 0) return (old);
	if (inc > 0) {
		if (arena_brk + inc > arena_end) {
			v8_errno = V8_ENOMEM;
			return ((char *)-1);
		}
		if (commit(arena_brk + inc) < 0) { v8sys_fail(); return ((char *)-1); }
	} else if (arena_brk + inc < arena_base) {
		v8_errno = V8_EINVAL;
		return ((char *)-1);
	}
	arena_brk += inc;
	return (old);
}

int
v8s_brk(char *addr)
{
	if (arena_init() < 0) return (v8sys_fail());
	if (addr < arena_base || addr > arena_end) {
		v8_errno = V8_ENOMEM;
		return (-1);
	}
	if (commit(addr) < 0) return (v8sys_fail());
	arena_brk = addr;
	return (0);
}

/*
 * `end` is where V8's sbrk stub said the break started -- the symbol the linker
 * places after bss.  V8's malloc reads it to find the bottom of the arena.
 * Ours is the base of the reservation instead.
 */
char *
v8sys_end(void)
{
	arena_init();
	return (arena_base);
}
