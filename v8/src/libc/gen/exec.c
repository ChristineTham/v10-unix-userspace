/*
 * execl, execle and execv.
 *
 * V8 kept these in libc/sys as hand-written VAX assembly (exec.s), alongside
 * the raw syscall stubs.  The shim replaces libc/sys, so they are written here
 * in C -- but they are still V8's, in the sense that they are the same three
 * one-liners over execve that the assembly was.
 *
 * They matter more than their size suggests.  execl is VARIADIC, and until this
 * file existed it was left undefined in libv8c.a and resolved from -lSystem at
 * run time.  That does not fail the link and it does not fail at startup; it
 * fails when it is called.  v8cc passes every argument in one positional
 * sequence in x0-x7, while Apple's ARM64 ABI passes the variadic arguments of a
 * call on the stack, so the host's execl saw no arguments at all:
 *
 *	system("echo hello")
 *		-> execl("/bin/sh", "sh", "-c", "echo hello", 0)
 *		-> /bin/sh with NO -c, which is an INTERACTIVE shell
 *
 * and refer, which shells out, sat at an `sh-3.2$` prompt looking exactly like
 * a hang.  system() even returned 0, because the shell it started exited fine.
 *
 * That is the third time a gap in libc has been filled silently by a host
 * variadic function (scanf and doscan were the first two).  tests/libv8c
 * now scans the built binaries for the shape rather than waiting to trip over
 * it again.
 *
 * The `&args` idiom is V8's own, and is why these cannot be written portably:
 * take the address of the last named parameter and walk forward.  It works
 * because the prologue v8cc emits spills x0-x7 contiguously below the caller's
 * stack arguments, so the arguments really are one array in memory.
 */

extern char **environ;

execl(f, args)
	char *f;
	char *args;
{

	return (execv(f, &args));
}

execv(f, argv)
	char *f;
	char **argv;
{

	return (execve(f, argv, environ));
}

/*
 * execle(file, arg0, ..., argn, 0, envp) -- the environment comes AFTER the
 * null that ends the argument list, so walk to the null and take the next slot.
 */
execle(f, args)
	char *f;
	char *args;
{
	register char **p;

	for (p = &args; *p != 0; p++)
		;
	return (execve(f, &args, (char **)p[1]));
}
