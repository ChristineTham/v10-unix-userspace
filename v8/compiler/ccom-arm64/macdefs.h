/*	ARM64 (AArch64)	*/

/*
 * Target model for V8 ccom on ARM64, replacing vax/macdefs.h.
 *
 * The headline difference from the VAX is LP64.  V8 defined NOLONG ("map longs
 * to ints") because on a VAX int, long and pointer were all 32 bits.  macOS has
 * no ILP32 process model, so pointers must be 64 bits, and a pointer that does
 * not fit in a long breaks more V8 code than a long that is wider than an int.
 *
 * Leaving NOLONG *undefined* is what makes LP64 expressible at all: see
 * common/mfile2.h, where TINT is (urTINT|TLONG) under NOLONG but plain urTINT
 * without it, and common/pftn.c:1904, which stops collapsing LONG into INT.
 * Pass 1 then carries LONG as a genuinely distinct type all the way down.
 *
 * Constants are safe at this width for free: manifest.h has `typedef long
 * CONSZ`, and ccom is itself compiled LP64, so its internal constant arithmetic
 * is already 64-bit.
 */

	/* Offsets, in BITS, of the first argument and the first automatic.
	 * Arguments live in the spill block the prologue writes (see below), so
	 * the first one sits at its base; autos start at the frame pointer. */
# define ARGINIT 0
# define AUTOINIT 0

# define SZCHAR 8
# define SZSHORT 16
# define SZINT 32
# define SZLONG 64		/* LP64: NOT SZINT -- see note above */
# define SZFLOAT 32
# define SZDOUBLE 64
# define SZPOINT 64		/* LP64 */

# define ALCHAR 8
# define ALSHORT 16
# define ALINT 32
# define ALLONG 64
# define ALFLOAT 32
# define ALDOUBLE 64
# define ALPOINT 64
# define ALSTRUCT 8
# define ALSTACK 64		/* AAPCS64 stack slots are 8 bytes.  The 16-byte
				 * SP alignment requirement is enforced in the
				 * prologue, not here. */
# define ALINIT 32

	/*
	 * The arithmetic type a pointer converts to.
	 *
	 * mfile1.h defaults this to INT behind an #ifndef, precisely so a target
	 * can override it, and on the VAX INT was right because a pointer was
	 * 32 bits.  Under LP64 it must be LONG, or optim.c's pvconvert() --
	 * which does `makety(l, PTRTYPE, ...)` whenever a non-pointer expression
	 * is cast to a pointer -- narrows the value to 32 bits and truncates it.
	 *
	 * This is what broke V8's malloc:
	 *
	 *	#define clearbusy(p) (union store *)((INT)(p)&~BUSY)
	 *
	 * The cast back to `union store *` sent the whole expression through
	 * pvconvert, which retyped it INT; the back end then emitted a 4-byte
	 * ldrsw for the pointer load, faithfully compiling the tree it was
	 * given.  With the same expression returning `long` instead of a
	 * pointer, the types came out correctly -- which is what finally
	 * identified pvconvert as the site.
	 */
# define PTRTYPE LONG

	/* Argument slot size: every parameter is widened to this, giving the
	 * argument block a uniform 8-byte stride.  The VAX used SZINT (the
	 * default in manifest.h) because it pushed 32-bit words; we need 64 so
	 * the block matches the eight 8-byte registers the prologue spills into
	 * it, which is what makes &arg walking -- varargs.h -- come out right. */
# define SZARG SZLONG

	/* format for labels */
# define LABFMT "L%d"

/* automatics and temporaries are on a negative growing stack */
# define BACKAUTO
# define BACKTEMP

	/* little-endian, as the VAX was */
# define RTOLBYTES
	/* characters are signed -- matches the VAX, and Apple ARM64 agrees
	 * natively.  Linux ARM64 defaults to unsigned, so the stage-0 build
	 * passes -fsigned-char; v8cc itself always emits signed-char code. */
# define CHSIGN
	/* structures are returned in a static location.
	 * Kept from the VAX deliberately (PLAN.md S4): it is authentic V8
	 * behaviour, it is the simplest thing for the generator, and nothing
	 * crosses the host ABI boundary as a struct -- the libv8sys shim passes
	 * only scalars and pointers -- so AAPCS64's own struct-return rules
	 * never come into play. */
# define STATSRET

	/* Number of register units a type occupies.
	 * On the VAX this was 2 for double (two 32-bit registers) and 1 for
	 * everything else.  ARM64 registers are 64 bits and doubles live in the
	 * FP file, so every type is a single unit. */
# define szty(t) 1

	/* number of scratch registers -- x9..x15, the AAPCS64 caller-saved
	 * temporaries that are neither argument nor callee-saved registers.
	 * Must agree with REGVAR/REGMASK in gencode.h. */
# define NRGS 7

	/* Register-variable numbering, needed in pass 1 (cisreg) as well as
	 * pass 2, so it lives here rather than in gencode.h which pass 1 does
	 * not include.  Internal 7..16 are the callee-saved x19..x28; cisreg()
	 * hands them out counting DOWN from RVARLAST_P1. */
# define RVARFIRST_P1 7
# define RVARLAST_P1 16

	/* use clocal() -- machine-dependent tree rewriting in pass 1 */
# define CLOCAL

	/*
	 * A pointer converted to int keeps all its bits, rather than being
	 * truncated to SZINT.  Needed because SZPOINT is 64 and SZINT is 32; on
	 * the VAX they were equal and the question never arose.  See the note in
	 * common/optim.c, in sconvert().
	 */
# define PTRCONVFULL

	/*
	 * A conversion that changes only signedness is kept over `/`, `%` and
	 * `>>`, rather than being painted onto them.  Those three read their own
	 * node's signedness to pick udiv/sdiv and lsr/asr, so repainting one
	 * silently changes what it computes.  See the note in common/optim.c,
	 * in sconvert().
	 */
# define SIGNCONVKEEP

	/* .comm and .lcomm are available */
#define ALLCOMM

/* asm markers */
#define	ASM_COMMENT	"//ASM"
#define	ASM_END		"//ASMEND"

	/* decide what ops can be shortened */
# define OPBIGSZ

	/*
	 * End-of-translation-unit hook, called by scan.c's main().  Mach-O
	 * objects need `.subsections_via_symbols` to tell ld that each symbol
	 * begins its own atom; without it ld cannot work out where one datum
	 * ends and the next begins, and every link of a program using perror
	 * warned that sys_nerr's "real definition" had size 0.  clang emits it
	 * on every Mach-O object it produces.  ENDJOB is ccom's own hook for
	 * exactly this, so no original file needs changing.
	 */
# define ENDJOB arm64_endjob

	/*
	 * Static-initialiser hook, called by pftn.c's doinit() for every datum
	 * of at least SZINT bits.  Without it the generic arm is
	 * `ecode(optim(p)); inoff += sz;' -- which sends the INIT node through
	 * pass 2 and lets the back end RE-DERIVE the width from the node's
	 * type.  Pass 1 has already computed that width, already advanced inoff
	 * by it and already laid the enclosing object out with it, so a second
	 * derivation is one that can disagree; and for an enum it does.
	 *
	 * pftn.c:920-921 sizes every enum as SZINT.  econvert() picks the
	 * underlying type from dimtab[csiz] (trees.c:1209-1212), and an INIT
	 * node carries the TYPE CODE in csiz rather than a dimtab index -- so
	 * the lookup reads an unallocated slot, matches none of SZCHAR/SZINT/
	 * SZSHORT, and falls to `else ty = LONG'.  On a VAX that fallback was
	 * invisible: SZLONG and SZINT were both 32, so LONG and INT emitted the
	 * same four bytes.  Under LP64 it emits eight, the datum overruns its
	 * own slot, and every pointer after it in the aggregate is misaligned
	 * -- `ld: pointer not aligned'.
	 *
	 * Taking sz from pass 1 makes the two agree by construction instead.
	 */
# define MYINIT arm64_myinit
