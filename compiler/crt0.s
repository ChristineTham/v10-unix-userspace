/*
 * crt0 for ARM64 -- replaces libc/csu/crt0.s.
 *
 * V8's version walked the VAX's argument block to find argc, argv and envp,
 * published environ, called main, and passed the result to exit.  The job is
 * the same here; only the stack layout differs.
 *
 * On entry, with -nostdlib and -e _v8start, the kernel has laid the stack out:
 *
 *	[sp,  #0]	argc
 *	[sp,  #8]	argv[0]
 *	...		argv[argc-1]
 *	[...]		NULL
 *	[...]		envp[0] ...
 *
 * so argv is just sp+8, and envp follows the NULL that terminates argv.
 *
 * The VAX original had a heuristic here -- it walked to the end of argv and
 * then checked whether an envp vector really followed, backing up if not.  That
 * guarded against kernels which did not supply one.  XNU and Linux both always
 * do, so this walks the NULL terminator and takes what follows.
 *
 * No stdio initialisation and no atexit machinery, exactly as V8 had it:
 * exit() calls _cleanup() itself before the exit syscall (libc/sys/exit.s).
 */

	.text
	.p2align 2
	.globl _v8start
_v8start:
	/*
	 * Establish a frame.  Not needed to run, but it gives lldb and the
	 * crash reporter something to unwind, which matters when a ported
	 * program dies somewhere deep in V8 code.
	 */
	mov	x29, #0
	mov	x30, #0

	/*
	 * macOS hands argc, argv and envp in REGISTERS, not on the stack.
	 *
	 * Modern Mach-O executables carry LC_MAIN rather than LC_UNIXTHREAD, and
	 * dyld calls the entry point with the same signature as main:
	 *
	 *	x0 = argc, x1 = argv, x2 = envp, x3 = apple
	 *
	 * The VAX crt0 walked the stack for these, and so did the first version
	 * of this file -- which linked, ran, and died writing through a garbage
	 * pointer inside the first write(2).  Nothing to walk here: the values
	 * are already where main wants them.
	 */
	adrp	x3, _environ@PAGE
	add	x3, x3, _environ@PAGEOFF
	str	x2, [x3]		/* publish environ */

	bl	_main

	/* main's return value becomes exit's argument */
	bl	_exit

	/*
	 * exit() does not return.  If it somehow does, leave through the
	 * kernel directly rather than falling into whatever follows.
	 */
	mov	x0, #1
	bl	__exit
	brk	#0

	.data
	.p2align 3
	.globl _environ
_environ:
	.quad	0
