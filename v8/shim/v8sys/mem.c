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

#include "v8sys.h"
#include "rawsys.h"

#define PROT_NONE_	0
#define PROT_READ_	1
#define PROT_WRITE_	2
#define MAP_PRIVATE_	0x0002
#define MAP_ANON_	0x1000
#define PAGESZ		16384L	/* Apple silicon; a multiple of 4096 is safe
				 * everywhere, and only alignment matters here */

#define ARENA_SIZE	(1024L * 1024L * 1024L)

static char *arena_base;	/* start of the reservation */
static char *arena_brk;		/* the current break */
static char *arena_end;		/* end of the reservation */
static char *arena_committed;	/* everything below this is readable/writable */

static int
arena_init(void)
{
	long r;

	if (arena_base) return (0);
	r = rawsys6(SYS_mmap, 0, ARENA_SIZE, PROT_NONE_,
	    MAP_PRIVATE_|MAP_ANON_, -1, 0);
	if (r < 0 || r == -1) { v8_errno = V8_ENOMEM; return (-1); }
	arena_base = arena_brk = arena_committed = (char *)r;
	arena_end = arena_base + ARENA_SIZE;
	return (0);
}

/* Make everything below `upto` readable and writable. */
static int
commit(char *upto)
{
	char *want;

	if (upto <= arena_committed) return (0);
	want = (char *)(((unsigned long)upto + PAGESZ - 1) & ~(unsigned long)(PAGESZ - 1));
	if (want > arena_end) { v8_errno = V8_ENOMEM; return (-1); }
	if (rawsys3(SYS_mprotect, (long)arena_committed,
	    want - arena_committed, PROT_READ_|PROT_WRITE_) < 0) {
		v8_errno = V8_ENOMEM;
		return (-1);
	}
	arena_committed = want;
	return (0);
}

char *
v8s_sbrk(long inc)
{
	char *old;

	if (arena_init() < 0) { v8_errno = V8_ENOMEM; return ((char *)-1); }
	old = arena_brk;
	if (inc == 0) return (old);
	if (inc > 0) {
		if (arena_brk + inc > arena_end) {
			v8_errno = V8_ENOMEM;
			return ((char *)-1);
		}
		if (commit(arena_brk + inc) < 0) { v8_errno = V8_ENOMEM; return ((char *)-1); }
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
	if (arena_init() < 0) { v8_errno = V8_ENOMEM; return (-1); }
	if (addr < arena_base || addr > arena_end) {
		v8_errno = V8_ENOMEM;
		return (-1);
	}
	if (commit(addr) < 0) { v8_errno = V8_ENOMEM; return (-1); }
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


/*
 * A minimal internal allocator for the shim itself.
 *
 * The shim cannot call the host's malloc -- it must name no libc function (see
 * rawsys.h) -- and it must not call V8's malloc either, since that lives above
 * the seam and grows the very arena managed here.  Only dir.c uses it, to hold
 * one directory snapshot at a time, so a bump allocator with a free list of
 * exact-size blocks is enough and cannot fragment badly in practice.
 */
static char *pool, *poolend;

char *
v8sys_alloc(long n)
{
	char *p;
	long need = (n + 15) & ~15L;

	if (pool == 0 || pool + need > poolend) {
		long chunk = need > (1L << 20) ? need : (1L << 20);
		long r = rawsys6(SYS_mmap, 0, chunk, PROT_READ_|PROT_WRITE_,
		    MAP_PRIVATE_|MAP_ANON_, -1, 0);
		if (r < 0 || r == -1) return (0);
		pool = (char *)r;
		poolend = pool + chunk;
	}
	p = pool;
	pool += need;
	return (p);
}

/*
 * Freeing is a no-op.  dir.c allocates a snapshot per open directory and frees
 * it on close; a V8 program holds a handful at once, and the pool is reclaimed
 * wholesale when the process exits.  Tracking blocks here would be more code
 * than the waste it saves.
 */
void
v8sys_free(char *p)
{
}
