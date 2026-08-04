/*
 * ARM64 backend definitions for V8 ccom -- replaces vax/gencode.h.
 *
 * REGISTER MODEL
 *
 * ccom refers to registers by small internal numbers, not machine names; the
 * mapping lives in rnames[] in lcatch2.c.  V8's convention (see vax/local.c
 * cisreg(), and reader.c) is:
 *
 *	0 .. REGVAR-1	scratch, allocated by the expression generator
 *	REGVAR ..	register variables, handed out by cisreg() counting
 *			DOWN from minrvar as pass 1 meets `register` declarations
 *
 * Mapped onto AAPCS64:
 *
 *	internal 0..6	-> x9 .. x15	caller-saved temporaries.  Deliberately
 *					not x0-x7 (arguments) and not x16/x17
 *					(linker veneers) or x18 (reserved by
 *					Apple -- touching it corrupts the OS's
 *					platform register).
 *	internal 7..16	-> x19 .. x28	callee-saved, so a register variable
 *					survives a call, which is the entire
 *					point of `register` in V8 code.
 *
 * x29/x30 are frame pointer and link register; sp is the stack pointer.  They
 * are never allocated, so they are not in the internal numbering at all.
 */

/*
 * mfile2.h has no include guard, so it is included by the .c files, once, and
 * never from here.
 *
 * There is no `extern jmp_buf back` here, unlike the VAX header.  That buffer
 * was the landing pad for the original generator's backtracking: when doit()
 * ran out of registers it rewrote the tree to spill an operand and longjmp'd
 * back to restart the whole statement, discarding the assembly it had buffered.
 * This generator does not backtrack -- AArch64 has no memory operands competing
 * for registers, so the register pressure that made backtracking necessary is
 * largely gone -- and it reports exhaustion instead.
 */

extern char *frameptr, *argptr;

/* scratch registers: internal 0..6 == x9..x15 */
#define REGVAR	7		/* first register-variable number */
#define REGMASK	0x7f		/* seven scratch registers */

/* register variables: internal 7..16 == x19..x28 */
#define RVARFIRST 7
#define RVARLAST  16

/* physical register numbers, for readability in the generator */
#define R_SCRATCH0	9	/* x9  == internal 0 */
#define R_RVAR0		19	/* x19 == internal 7 */
#define R_FP		29
#define R_LR		30

/* internal register number -> AArch64 x-register number */
#define PHYSREG(r) ((r) < REGVAR ? (R_SCRATCH0 + (r)) : (R_RVAR0 + (r) - REGVAR))

/*
 * Result descriptor returned by the recursive generator.
 *
 * Unlike the VAX original this is NOT punned with an integer anywhere: V8 got
 * away with passing literal 0 for a `ret` parameter because K&R had no
 * prototypes and the struct was exactly int-sized.  We keep the struct honest
 * and use NODEST for "no destination requested".
 */
typedef struct {
	short reg;		/* internal register number holding the value */
	short flag;		/* the R* bits below */
} ret;

#define R_NONE	0		/* no value produced */
#define R_REG	1		/* value is in ret.reg */
#define R_CC	2		/* value is in the condition flags */
#define R_CON	4		/* value is a known constant (in ret.reg's
				 * place we keep nothing; the caller re-reads
				 * the node) */

/* Request flags, passed down as `flag` -- what the caller wants. */
#define WVALUE	1		/* want the value in a register */
#define WCC	2		/* only the condition flags are needed */
#define WEFFECT	4		/* only side effects; value is discarded */

extern ret nodest;		/* "no destination" -- see gencode.c */
#define NODEST nodest

/* Sizes, in bytes, derived from the target model in macdefs.h. */
#define BYTESZ(t) (tsize(t, 0, 0) / SZCHAR)
