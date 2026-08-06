/*
 * ONE V8 syscall entry point.
 *
 * Compiled once per line of syscalls.def, with -DV8_NAME/-DV8_IMPL/-DV8_TYPE/
 * -DV8_ARGS naming which one.  Sixty-seven tiny objects go into libv8stubs.a,
 * which is how V8's own libc/sys was built -- open.s, mkdir.s, rmdir.s and the
 * rest, one file each -- and for the same reason: a program that defines its
 * own rmdir must get its own, not libc's.  See syscalls.def.
 *
 * V8's libc calls open(), read(), write() and so on.  The implementations live
 * in syscall.c under v8s_ names so the library can also be built against the
 * host libc for testing (tests/v8sys/test.c does that) without every name
 * colliding.  This is the layer that gives them their real names, and it is
 * compiled ONLY into the copy the V8 world links with -nostdlib.  Nothing here
 * may call the host libc: at link time these names ARE libc to the program.
 *
 * EVERY WRAPPER RETURNS long, INCLUDING THE ONES THE MANUAL CALLS int.
 *
 * AAPCS64 says a function returning int need only set w0; the top half of x0 is
 * unspecified, and clang leaves whatever was there.  V8's back end computes at
 * 64-bit width and compares with an x-form `cmp`, so -1 arriving as
 * 0x00000000ffffffff tested as POSITIVE -- and V8 code checks every syscall
 * with `< 0`.  `cat nosuchfile` reported "input nosuchfile is output" because
 * open() failed and the failure did not register.
 *
 * The obvious fix is to sign-extend at the call site, and that was tried.  It
 * breaks worse.  V8 calls malloc without declaring it -- opendir.c has
 * `dirp = (DIR *)malloc(sizeof(DIR));` and nothing declares malloc anywhere in
 * scope -- so K&R types the call `int`, and sign-extending the result from 32
 * bits truncates the pointer.  On the VAX int and pointer were both 32 bits and
 * the omission cost nothing; under LP64 it is fatal.  The compiler cannot tell
 * a declared `int` from an undeclared one: they are the same node.
 *
 * So the seam adapts, which is the shim's job.  Returning long makes clang set
 * all 64 bits, V8 sees a properly extended value, and the compiler narrows only
 * types that must have been declared (char, short, unsigned).  gencall() in
 * compiler/ccom-arm64/gencode.c carries the other half of this note; the two
 * decisions only work together.
 *
 * Anything else in the shim that V8 code calls by name -- isatty() in ioctl.c
 * is the current example -- has to follow the same rule.
 */

#include "v8sys.h"

/*
 * Declared with the IMPLEMENTATION's own return type, not the wrapper's.
 *
 * v8s_open returns int, so AAPCS64 only requires it to set w0.  Declaring it
 * `extern long` claimed the top half of x0 was already meaningful; the wrapper
 * copied it straight through, open() returned 0x00000000ffffffff, and fopen's
 * `if (f < 0)` was false -- so fopen succeeded on a file that does not exist
 * and cmp reported "EOF on nosuchfile" instead of "cannot open".  Naming the
 * real type makes the conversion below a genuine sign-extension.
 */
extern V8_IMPLTYPE V8_IMPL();

/* Keep v8_errno and errno the same storage as far as the world can tell. */
extern int errno;

static void
sync_errno(void)
{
	errno = v8_errno;
}

#if V8_ARGS
V8_TYPE
V8_NAME(a, b, c, d, e, f)
	long a, b, c, d, e, f;
{
	V8_TYPE r = (V8_TYPE)V8_IMPL(a, b, c, d, e, f);

	sync_errno();
	return (r);
}
#else
V8_TYPE
V8_NAME()
{
	V8_TYPE r = (V8_TYPE)V8_IMPL();

	sync_errno();
	return (r);
}
#endif
