#ifndef	NSIG
#define NSIG	32

#define	SIGHUP	1	/* hangup */
#define	SIGINT	2	/* interrupt */
#define	SIGQUIT	3	/* quit */
#define	SIGILL	4	/* illegal instruction (not reset when caught) */
#define	SIGTRAP	5	/* trace trap (not reset when caught) */
#define	SIGIOT	6	/* IOT instruction */
#define	SIGEMT	7	/* EMT instruction */
#define	SIGFPE	8	/* floating point exception */
#define		K_INTOVF 1	/* integer overflow */
#define		K_INTDIV 2	/* integer divide by zero */
#define		K_FLTOVF 3	/* floating overflow */
#define		K_FLTDIV 4	/* floating/decimal divide by zero */
#define		K_FLTUND 5	/* floating underflow */
#define		K_DECOVF 6	/* decimal overflow */
#define		K_SUBRNG 7	/* subscript out of range */
#define	SIGKILL	9	/* kill (cannot be caught or ignored) */
#define	SIGKIL	9
#define	SIGBUS	10	/* bus error */
#define	SIGSEGV	11	/* segmentation violation */
#define	SIGSYS	12	/* bad argument to system call */
#define	SIGPIPE	13	/* write on a pipe with no one to read it */
#define	SIGALRM	14	/* alarm clock */
#define	SIGTERM	15	/* software termination signal from kill */

#define	SIGSTOP	17	/* sendable stop signal not from tty */
#define	SIGTSTP	18	/* stop signal from tty */
#define	SIGCONT	19	/* continue a stopped process */
#define	SIGCHLD	20	/* to parent on child stop or exit */
#define	SIGTTIN	21	/* to readers pgrp upon background tty read */
#define	SIGTTOU	22	/* like TTIN for output if (tp->t_local&LTOSTOP) */
#define SIGTINT	23	/* to pgrp on every input character if LINTRUP */
#define	SIGXCPU	24	/* exceeded CPU time limit */
#define	SIGXFSZ	25	/* exceeded file size limit */

#ifndef KERNEL
typedef int	(*SIG_TYP)();
SIG_TYP signal();
#endif

#define	BADSIG		(int (*)())-1
#define	SIG_DFL		(int (*)())0
#define	SIG_IGN		(int (*)())1
#ifdef KERNEL
#define	SIG_CATCH	(int (*)())2
#endif
#define	SIG_HOLD	(int (*)())3

/*
 * PORT: `long' for `int' in all three.  These carry a FUNCTION POINTER through
 * an integer in order to use its low bit as a flag -- the VAX aligns code, so
 * bit 0 is free, and so does arm64.  The TRICK is fine here; the WIDTH is not.
 *
 * Two of the three are live bugs and one is not, which is worth separating
 * rather than fixing as a block:
 *
 *	SIGISDEFER  reads bit 0 and yields a boolean.  Bit 0 SURVIVES a
 *	            truncation to int, so this one was accidentally correct --
 *	            the same reasoning that makes `~x' safe for signed values
 *	            in the int-truncation table.  Changed anyway, because
 *	            leaving one of three spelled the old way is how the line
 *	            beside it keeps the assumption.
 *	SIGUNDEFER  RECONSTRUCTS a pointer from the integer.  Truncates.
 *	DEFERSIG    RECONSTRUCTS a pointer from the integer.  Truncates.
 *
 * Reached by libjobs' sigset.c, which is written entirely on these -- so
 * before this every deferred handler csh installed was a heap-or-text address
 * with its top half gone.  This header had never been imported, which is the
 * sys/fblk.h shape: a header nobody patched silently stays 1985's.
 */
#define	SIGISDEFER(x)	(((long)(x) & 1) != 0)
#define	SIGUNDEFER(x)	(int (*)())((long)(x) &~ 1)
#define	DEFERSIG(x)	(int (*)())((long)(x) | 1)

#define	SIGNUMMASK	0377		/* to extract pure signal number */
#define	SIGDOPAUSE	0400		/* do pause after setting action */
#define	SIGDORTI	01000		/* do ret+rti after setting action */
#endif
