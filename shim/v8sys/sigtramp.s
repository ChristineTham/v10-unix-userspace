/*
 * The signal trampoline -- the piece without which no V8 program can catch a
 * signal at all.  Companion to v8sys_sigcall() in signal.c.
 *
 * WHAT IT IS FOR.  The kernel does not jump to a handler.  It jumps to a
 * TRAMPOLINE whose address the process supplied in sa_tramp when it installed
 * the handler, and hands the handler's address along as an argument; the
 * trampoline calls the handler and then asks the kernel to restore the
 * interrupted context with sigreturn(2).  Userland owns that last step, so a
 * process that supplies no trampoline has nowhere to be entered: this shim
 * passed the SYSCALL a userland `struct sigaction', which has no sa_tramp
 * field, so every handler in this port was installed with sa_tramp = 0 and
 * delivery jumped to address 0.  sigaction returned 0 and nothing looked wrong
 * until a signal actually arrived.  shim/NOTES.md has the measurements.
 *
 * ON ENTRY, from XNU's sendsig() for arm64 (bsd/dev/arm/unix_signal.c):
 *
 *	x0	the handler
 *	x1	infostyle -- UC_TRAD or UC_FLAVOR; opaque to us, and sigreturn
 *		wants it back
 *	x2	the signal number, in the HOST's numbering
 *	x3	siginfo_t *
 *	x4	ucontext_t *, the saved context sigreturn restores from
 *	x5	the sigreturn token: a cookie the kernel checks, so that a wild
 *		branch into sigreturn cannot be used to install a chosen
 *		register set.  Pass it back untouched.  Kernels older than the
 *		check ignore the third argument, so forwarding it is right on
 *		both.
 *
 * WHY THIS FILE IS THREE INSTRUCTIONS.  That register order is exactly AAPCS64
 * argument order, so v8sys_sigcall() declares its parameters in the kernel's
 * order and the shuffle disappears -- which is the point.  Everything that
 * needs judgement (mapping the signal number back into V8's numbering, calling
 * the handler, issuing sigreturn) is C in signal.c, where it can be read and
 * where the syscall number comes from <sys/syscall.h> rather than from memory.
 * Hand-written assembly is the precedent here (compiler/crt0.s, setjmp.s), not
 * the ambition.
 *
 * The frame pointer and link register are zeroed first, for the reason crt0.s
 * gives: the kernel enters us with neither meaning anything, and a handler that
 * dies should give the crash reporter a chain that terminates rather than one
 * that wanders.  v8sys_sigcall() never returns -- it ends in sigreturn, or in a
 * trap if that fails -- so the tail branch loses nothing by discarding x30.
 *
 * Plain arm64, not arm64e: everything this port builds is a third-party
 * executable, so sa_tramp is an ordinary unsigned pointer and needs no signing.
 */

	.text
	.p2align 2
	.globl _v8sys_sigtramp
_v8sys_sigtramp:
	mov	x29, #0
	mov	x30, #0
	b	_v8sys_sigcall
