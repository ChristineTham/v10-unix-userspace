/*
 * /lib/f1 -- the second pass of the Fortran compiler.
 *
 * f77's driver runs three programs: f77pass1 writes a binary intermediate,
 * THIS turns it into assembly, and as(1) assembles the two concatenated.  On a
 * VAX /lib/f1 was pcc's own second pass built with -DFORT (pcc1/pcc/makefile's
 * install arm is `mv fort ${DESTDIR}/lib/f1'), matching on shapes and cookies.
 *
 * IT IS NOT THAT HERE, AND THE REASON IS MEASURED RATHER THAN PREFERRED.  This
 * port's back end is pcc2 -- it matches on TYPES, with a cost vector, and its
 * node is {op, goal, type, cst[NCOSTS], ...} against pcc1's {op, rall, type,
 * su, ...}.  Reusing it would mean supplying the fourteen globals its pass 2
 * wants from ITS pass 1 and translating a type word besides.  What f77 actually
 * emits turns out not to need any of that: the intermediate is a POSTFIX
 * STREAM, which is a stack machine, and a stack machine for the operators f77
 * uses is smaller than the adapter would have been.
 *
 * THE FORMAT, decoded from a real file rather than from pcc1's reader.c (which
 * reads the OLDER text format -- putpcc.c's second line is "NEW VERSION USING
 * BINARY POLISH POSTFIX INTERMEDIATE").  Every record starts with one word:
 *
 *	word = op | (var<<8) | (type<<16)		putpcc.c's p2triple()
 *
 * and the word is a `long int', so EIGHT bytes here and four on a VAX.  Both
 * ends are ours, so that is consistent; it is the third place in this port
 * where a `long' turned out to be an ABI decision rather than a type.
 *
 * Operand words follow per opcode: P2ICON takes a value, P2NAME an offset then
 * a name, P2PASS a count of string words.  Strings are four chars per word
 * (p2str); names are the fixed eight-character form, two words, because this
 * build does not define UCBPASS2.
 *
 * THE TYPE WORD IS pcc1's, four bits per level: the base type is the low four
 * bits and PTR (1) and FUNCT (2) stack above it.  So 148 is P2INT under PTR and
 * FUNCT -- a function address.  pcc2 widened that field to five bits, which is
 * the one translation an adapter would have had to do and which this file
 * simply reads in pcc1's terms.
 */

#include <stdio.h>

/* pccdefs, respelled: this is layer 2 and including f77's copy would put its
   whole defs chain on the include path.  The values are checked against
   src/cmd/f77/pccdefs by tests/wavea rather than trusted. */
#define P2NAME		2
#define P2ICON		4
#define P2PLUS		6
#define P2PLUSEQ	7
#define P2STAREQ	12
#define P2MINUS		8
#define P2NEG		10
#define P2STAR		11
#define P2INDIRECT	13
#define P2BITAND	14
#define P2BITOR		17
#define P2BITXOR	19
#define P2QUEST		21
#define P2COLON		22
#define P2ANDAND	23
#define P2OROR		24
#define P2GOTO		37
#define P2LISTOP	56
#define P2ASSIGN	58
#define P2COMOP		59
#define P2SLASH		60
#define P2MOD		62
#define P2LSHIFT	64
#define P2RSHIFT	66
#define P2CALL		70
#define P2CALL0		72
#define P2NOT		76
#define P2BITNOT	77
#define P2EQ		80
#define P2NE		81
#define P2LE		82
#define P2LT		83
#define P2GE		84
#define P2GT		85
#define P2REG		94
#define P2OREG		95
#define P2CONV		104
#define P2FORCE		108
#define P2CBRANCH	109

#define P2PASS		200
#define P2STMT		201
#define P2SWITCH	202
#define P2LBRACKET	203
#define P2RBRACKET	204
#define P2EOF		205
#define P2ARIF		206
#define P2LABEL		207

#define P2CHAR		2
#define P2SHORT		3
#define P2INT		4
#define P2REAL		6
#define P2DREAL		7

#define P2PTR		020
#define P2FUNCT		040
#define BTSHIFT		4
#define BTMASK		017

/*
 * THE FRAME'S ARGUMENT AREA, WHICH IS arm64defs' NUMBER AND NOT THIS FILE'S.
 * prsave() in src/cmd/f77/arm64.c reserves SZADDR*MAXARGSLOT for a procedure's
 * own parameters and SZADDR*(MAXARGSLOT-8) BELOW x29 for the arguments of the
 * calls it makes.  This pass writes that second area, so the two files have to
 * agree about its size; respelled here rather than included, for the reason the
 * opcode numbers are: including f77's copy would put its whole defs chain on
 * this file's include path, and they share no header -- and tests/wavea
 * compares the two spellings, which is what it already does for the opcodes.
 * (Both programs are built by v8cc; see the Makefile.  An earlier draft of
 * this sentence said f1 was clang's, which is the one fact about this file
 * CLAUDE.md tells you to know before editing it, stated backwards.)
 */
#define MAXARGSLOT	64	/* == arm64defs' MAXARGSLOT */
#define ARGSLOT		8	/* == arm64defs' SZADDR */


/* ------------------------------------------------------------ code -------
 *
 * THE STREAM IS POSTFIX, so this is a stack machine rather than a tree matcher.
 * That is the whole reason this file is not an adapter to the port's pcc2 back
 * end: pcc2 wants a TREE with a cost vector attached, and rebuilding one from a
 * postfix stream only to have it flattened again is work in both directions.
 *
 * A value is a CONSTANT, the ADDRESS OF A NAME, or a REGISTER holding a result.
 * Leaving the first two unmaterialised is what lets `do_fio(&L15, v.1, 4)' come
 * out as three adrp/add pairs straight into x0-x2: an argument is not evaluated
 * until CALL knows which register it belongs in.
 *
 * EACH STATEMENT IS A COMPLETE EXPRESSION, which is what makes the operand
 * stack tractable.  P2STMT marks the boundary, so the stack is empty at every
 * one -- and for a call statement the callee is the FIRST value pushed since
 * that mark.  That removes the only genuinely ambiguous decoding question,
 * because LISTOP carries no arity: `a, b, c' arrives as ((a,b),c) and in
 * postfix the three values are simply adjacent, so LISTOP itself does nothing.
 */

/*
 * THE VALUE STACK HAS TO HOLD A WHOLE CALL, and it was a bare 64 -- the same
 * number MAXARGSLOT happens to be, which is a coincidence rather than a
 * relation.  A call occupies one slot per argument plus one for the callee, so
 * the widest call the frame can express needs MAXARGSLOT+1 and 64 was one
 * short: raising the argument bound made THIS the binding limit, and the
 * diagnostic then named the wrong resource -- `expression stack overflow' for a
 * program whose expressions are all one term.
 *
 * Stated as a relation so the two move together.  The headroom is for the
 * expression a call sits inside, which is a separate quantity: `f(...) + g(...)'
 * holds one call's result while building the next.
 */
#define NSTACK	(MAXARGSLOT + 64)

#define V_CON	0		/* a literal */
#define V_ADDR	1		/* the address of a named object */
#define V_REG	2		/* already in a register */
#define V_MEM	3		/* offset(reg), an OREG -- a temporary */
#define V_VAR	4		/* a named variable: its VALUE, not its address */
#define V_CC	5		/* a comparison, waiting for the branch to read it */

struct val {
	int	kind;
	long	con;		/* V_CON: the value.  V_ADDR/V_MEM: an offset.
				   V_CC: the comparison operator */
	char	name[72];	/* V_ADDR, V_VAR */
	int	reg;		/* V_REG: the register.  V_MEM: the base */
	int	vtype;
	int	owned;		/* this value allocated reg and must give it back */
};

static struct val vstack[NSTACK];
static int	vsp;

/*
 * THE LOGICAL STACK, WHICH IS WHAT MAKES A CALL FIND ITS OWN CALLEE.  LISTOP
 * carries no arity, so the first version took the callee to be the first value
 * pushed since P2STMT.  That is true of a call STATEMENT and false of a call in
 * an EXPRESSION: `write(6,*) sq(9)' pushes the result temporary FIRST, so the
 * callee was read out of slot 0, came back a temporary rather than an address,
 * and the refusal said `indirect call not implemented' -- naming a construct
 * the program does not contain.  A wrong answer in a diagnostic is worse than
 * none, because it sends the next reader after a feature instead of a bug.
 *
 * lstack[i] is the vstack index where logical value i begins.  A push starts a
 * new one; an operator's pop ends one; LISTOP merges the top two into a single
 * logical value covering both, which is exactly what it means.  CALL then takes
 * the top logical value as its argument run and the one below it as the callee,
 * needing no arity from the record and no assumption about statements.
 */
static int	lstack[NSTACK];
static int	lsp;

/* THE PROCEDURE NUMBER, FROM LBRACKET, WHICH NAMES THE BASE LABEL AN ASSIGNED
   GOTO MEASURES FROM.  See labelbase() below. */
static int	curproc;

static FILE	*in;
static int	dumping;	/* -d: report the stream instead of compiling */
static long	word;
static int	op, var, type;

/* A word, or 0 at end of file.  The stream is a raw write(2) of long ints, so
   there is no framing to resynchronise on: a short read is the end. */
static int
getword(wp)
	long *wp;
{
	return (fread((char *)wp, sizeof(long), 1, in) == 1);
}

static int
getrec()
{
	if (!getword(&word))
		return (0);
	op = word & 0377;
	var = (word >> 8) & 0377;
	type = (word >> 16) & 0177777;
	return (1);
}

/* p2str packs four characters per word; p2name writes the fixed eight-character
   form in two words.  Both stop at a NUL, and neither is length-prefixed in the
   bytes, so the COUNT in the record header is what says how many words to
   take. */
static void
getstr(buf, words)
	char *buf;
	int words;
{
	long w;
	int i, j, n = 0;

	for (i = 0; i < words; i++) {
		if (!getword(&w))
			break;
		for (j = 0; j < 4; j++)
			buf[n++] = (w >> (8*j)) & 0377;
	}
	buf[n] = '\0';
}

static void
getname(buf)
	char *buf;
{
	long w[2];
	int i;

	if (!getword(&w[0]) || !getword(&w[1])) {
		buf[0] = '\0';
		return;
	}
	for (i = 0; i < 8; i++)
		buf[i] = (w[0] >> (8*i)) & 0377;
	buf[8] = '\0';
}


/* ------------------------------------------------------- the emitters ---- */

/* ---- the type word ------------------------------------------------------
 *
 * pcc1 stacks PTR and FUNCT four bits at a time above the base type, so a
 * nonzero high field means "this is an address" whatever the base says.  Every
 * width decision below asks these three and nothing else: getting a pointer's
 * width from its BASE type is how `ldr w2, [x29, #1024]' came to load half of a
 * char * and hand it to do_lio.
 */
static int
isptr(t)
	int t;
{
	return ((t >> BTSHIFT) != 0);
}

static int
isflt(t)
	int t;
{
	return (!isptr(t) &&
	    ((t & BTMASK) == P2REAL || (t & BTMASK) == P2DREAL));
}

static int
isdbl(t)
	int t;
{
	return (!isptr(t) && (t & BTMASK) == P2DREAL);
}


/* ---- the registers ------------------------------------------------------
 *
 * TWO POOLS, AND THE CALLEE-SAVED ONE IS NOT AN OPTIMISATION.  `r = g(p) + g(q)'
 * puts the first call's result on the stack and then makes a second call, so a
 * materialised value has to survive a `bl'.  x9-x15 do not; x23-x27 do, and
 * prsave() in src/cmd/f77/arm64.c saves them.  Pass 1 hands out x19-x22 as
 * register variables (MAXREGVAR is 4 in arm64defs), so the pools start above
 * them -- and the two files agree about that boundary by the same argument that
 * makes AUTOREG and regnum[] agree, which is that arm64defs states it once.
 *
 * d8-d15 are the float half, and AAPCS64 requires their low 64 bits to be
 * preserved across a call, which is the same property for the same reason.
 * SCRATCH is for within one emission only and is never live across anything.
 */
/* TWO within-emission scratch registers, live for the length of one
   instruction pair and never across anything.  There were five: SCRATCHA and
   two float ones went unused the moment doassign() started materialising its
   value AS the destination's type rather than into a fixed register, and an
   unconsumed definition is the same claim as an unconsumed function -- it says
   this pass needs a register it does not need.  Deleted rather than kept
   against a future caller. */
#define SCRATCHB	13
#define SCRATCHC	14

/*
 * THE INTEGER POOL IS NOT A CONSTANT, BECAUSE PASS 1 STATES ITS OWN CLAIM IN
 * THE RECORD.  src/cmd/f77/arm64.c's regnum[] hands out x19, x20, x21, x22 as
 * register variables and arm64defs caps that at MAXREGVAR 4 -- but the
 * LBRACKET record opens each procedure with the number it ACTUALLY took, and
 * measured over every program in the corpus that number is 0.  So four
 * callee-saved registers were sitting idle in every procedure this pass has
 * ever compiled, while it refused expressions for want of a fifth.
 *
 * x23-x28 are unconditional: prsave() saves x19-x28, regnum[] never reaches
 * past x24 (and cannot, being capped at four entries), and nothing else here
 * names x28.  The four above are taken in regnum[] order, so listing them
 * BACKWARDS makes the count a subtraction rather than a search.
 *
 * Character concatenation is what found this -- `c = a(1:3) // b(1:3)' needs
 * six live values at once -- and it is a spill that does not have to be a
 * spill: this pass owns no frame slots (pass 1 allocated the autos and told us
 * only how many), so the alternative was refusing, and the registers were
 * already there and already saved.
 */
#define IPOOLFIXED	6	/* x23-x28, always ours */
#define IPOOLREGVAR	4	/* x22-x19, ours unless pass 1 took them */
static int ipool[] = { 23, 24, 25, 26, 27, 28, 22, 21, 20, 19 };
static int fpool[] = { 8, 9, 10, 11, 12, 13, 14, 15 };
static char ibusy[sizeof ipool / sizeof ipool[0]];
static char fbusy[sizeof fpool / sizeof fpool[0]];
static int ipooln = IPOOLFIXED + IPOOLREGVAR;

static int
ralloc(flt)
	int flt;
{
	int i, n;

	n = flt ? sizeof fpool / sizeof fpool[0] : ipooln;
	for (i = 0; i < n; i++)
		if (flt ? !fbusy[i] : !ibusy[i]) {
			if (flt)
				fbusy[i] = 1;
			else
				ibusy[i] = 1;
			return (flt ? fpool[i] : ipool[i]);
		}
	/* REFUSED BY NAME rather than spilled.  A spill needs frame slots this
	   pass does not own -- pass 1 allocated the autos and told us only how
	   many -- so running out is a diagnostic.  Measured against real input
	   rather than assumed sufficient: see the note in PORTING.md. */
	fprintf(stderr,
	    "f1: expression needs more %s registers than this pass allocates\n",
	    flt ? "floating-point" : "integer");
	exit(2);
	/*NOTREACHED*/
	return (0);
}

static void
rfree(flt, r)
	int flt, r;
{
	int i, n;

	n = flt ? sizeof fpool / sizeof fpool[0] : ipooln;
	for (i = 0; i < n; i++)
		if ((flt ? fpool[i] : ipool[i]) == r) {
			if (flt)
				fbusy[i] = 0;
			else
				ibusy[i] = 0;
			return;
		}
}

/* Give back whatever a value was holding.  Called as each operand is consumed,
   so an expression's registers are recycled down its own depth rather than
   across the whole statement. */
static void
vfree(v)
	struct val *v;
{
	if (v->owned) {
		/* THE POOL COMES FROM THE KIND, NOT THE TYPE, and the two agree
		   for every kind but one.  A V_MEM's `reg' is its BASE -- an
		   ADDRESS, so always from the integer pool, whatever it points
		   at -- while `vtype' is the type of the object AT that address.
		   P2INDIRECT builds exactly that mismatch: it keeps the integer
		   base it materialised and takes the loaded type.  Deriving the
		   pool from vtype then sent a float-typed V_MEM's integer base
		   to rfree(1, ...), which searches fpool, does not find it (the
		   pools are disjoint by register number) and returns silently,
		   because rfree has no not-found arm.  One register leaked per
		   subscripted REAL reference: measured, `real a(20)' summed over
		   ten elements exhausted the ten-entry integer pool and f1
		   refused a legal program.  The integer control never fails. */
		rfree(v->kind == V_MEM ? 0 : isflt(v->vtype), v->reg);
		v->owned = 0;
	}
}

static void
regreset()
{
	int i;

	for (i = 0; i < sizeof ibusy / sizeof ibusy[0]; i++)
		ibusy[i] = 0;
	for (i = 0; i < sizeof fbusy / sizeof fbusy[0]; i++)
		fbusy[i] = 0;
}


static void
vpush(v)
	struct val *v;
{
	if (vsp >= NSTACK || lsp >= NSTACK) {
		fprintf(stderr, "f1: expression stack overflow\n");
		exit(2);
	}
	lstack[lsp++] = vsp;
	vstack[vsp++] = *v;
}

static struct val
vpop()
{
	if (vsp <= 0 || lsp <= 0) {
		fprintf(stderr, "f1: expression stack underflow\n");
		exit(2);
	}
	lsp--;
	return (vstack[--vsp]);
}

static void
vreset()
{
	vsp = 0;
	lsp = 0;
	regreset();
}

/*
 * THE LOAD AND STORE MNEMONICS COME FROM THE TYPE, and this is the whole of the
 * width discipline.  A load is signed where the base type is signed, because
 * this port keeps an int sign-extended in its x register -- the invariant
 * arm64_trunc() in compiler/ccom-arm64/gencode.c exists to maintain -- and a
 * comparison reads all 64 bits.  A POINTER is eight bytes whatever its base type
 * says, which is the case the first version got wrong for every parameter of
 * every subroutine.
 */
static char *
ldop(t)
	int t;
{
	if (isptr(t))
		return ("ldr");			/* x: a whole address */
	switch (t & BTMASK) {
	case P2CHAR:	return ("ldrsb");
	case P2SHORT:	return ("ldrsh");
	default:	return ("ldrsw");	/* int/long: SZLONG is 4 here */
	}
}

static char *
stop(t)
	int t;
{
	if (isptr(t))
		return ("str");
	switch (t & BTMASK) {
	case P2CHAR:	return ("strb");
	case P2SHORT:	return ("strh");
	default:	return ("str");
	}
}

/* The register letter a store reads from: an address is written whole out of x,
   everything narrower out of the w half of the same register. */
static int
stwide(t)
	int t;
{
	return (isptr(t));
}


/*
 * Put a value's ADDRESS in integer register r.  Only a named object or an
 * offset(reg) has one; a constant and a register do not, which is what makes
 * `assignment to a constant' a real diagnostic rather than a defensive one.
 */
static void
addrinto(v, r)
	struct val *v;
	int r;
{
	switch (v->kind) {
	case V_ADDR:
	case V_VAR:
		printf("\tadrp\tx%d, %s@PAGE\n", r, v->name);
		printf("\tadd\tx%d, x%d, %s@PAGEOFF\n", r, r, v->name);
		/* AND THE OFFSET CAN BE NEGATIVE, which `add' cannot take.  A
		   Fortran array subscript is one-based, so f77 addresses a(i) as
		   (&a - elementsize) + i*elementsize and the base arrives as
		   `ICON -4 type int * name "v.2"'.  `add x12, x12, #-4' is not an
		   instruction, and the assembler names the operand rather than
		   the sign. */
		if (v->con > 0)
			printf("\tadd\tx%d, x%d, #%ld\n", r, r, v->con);
		else if (v->con < 0)
			printf("\tsub\tx%d, x%d, #%ld\n", r, r, -v->con);
		break;
	case V_MEM:
		if (v->con > 0)
			printf("\tadd\tx%d, x%d, #%ld\n", r, v->reg, v->con);
		else if (v->con < 0)
			printf("\tsub\tx%d, x%d, #%ld\n", r, v->reg, -v->con);
		else if (v->reg != r)
			printf("\tmov\tx%d, x%d\n", r, v->reg);
		break;
	default:
		fprintf(stderr, "f1: the address of a value that has none\n");
		exit(2);
	}
}

/*
 * Materialise a value into integer register r.  V_ADDR becomes the adrp/add pair
 * Mach-O needs for a page-relative address; a V_CON small enough goes in one
 * mov and anything else through a literal pool.
 */
static void
into(v, r)
	struct val *v;
	int r;
{
	if (isflt(v->vtype) && v->kind != V_CON) {
		/* A FLOAT BY VALUE IN AN INTEGER REGISTER IS v8cc's CONVENTION,
		   not AAPCS64's, and it is the right one here because everything
		   f1 calls was compiled by v8cc: measured, `double twice(x)'
		   reads its parameter from where x0 was spilled and returns it in
		   d0.  The pair is asymmetric and neither half is a guess.
		   Nothing in Fortran reaches this today -- every libF77 entry
		   point takes pointers, because Fortran passes by reference -- so
		   it is refused by name rather than written and never exercised. */
		fprintf(stderr,
		    "f1: a floating-point value passed by value is not implemented\n");
		exit(2);
	}
	switch (v->kind) {
	case V_CON:
		if (v->con >= 0 && v->con < 65536)
			printf("\tmov\tx%d, #%ld\n", r, v->con);
		else if (v->con < 0 && v->con > -65536)
			printf("\tmov\tx%d, #-%ld\n", r, -v->con);
		else
			printf("\tldr\tx%d, =%ld\n", r, v->con);
		break;
	case V_ADDR:
		addrinto(v, r);
		break;
	case V_REG:
		if (v->reg != r)
			printf("\tmov\tx%d, x%d\n", r, v->reg);
		break;
	case V_MEM:
		/* An OREG is offset(reg) -- pass 1's temporaries and, above
		   ARGOFFSET, its spilled parameters.  The register is AUTOREG
		   from arm64defs, x29. */
		printf("\t%s\tx%d, [x%d, #%ld]\n",
		    ldop(v->vtype), r, v->reg, v->con);
		break;
	case V_VAR:
		/* A NAME IS A VALUE AND AN ICON IS AN ADDRESS, and that is the
		   whole distinction the stream draws.  `i + j' arrives as
		   NAME/NAME/PLUS, and an argument passed by reference arrives as
		   an ICON whose type carries PTR.  Treating both as addresses
		   made `i + j' ADD THE TWO ADDRESSES -- which produced Fortran's
		   field-overflow marker rather than a wrong number, because the
		   sum did not fit i2.  Measured in the emitted code: two
		   adrp/add pairs and then `add w12, w12, w13'. */
		addrinto(v, r);
		printf("\t%s\tx%d, [x%d]\n", ldop(v->vtype), r, r);
		break;
	case V_CC:
		fprintf(stderr, "f1: a comparison used as a value\n");
		exit(2);
	}
}

/* The same, into float register r.  The letter follows the type: a REAL is four
   bytes and a DOUBLE PRECISION eight, which arm64defs pins through
   typesize[TYREAL] being SZLONG and libF77's r_nint taking a float *. */
static void
finto(v, r)
	struct val *v;
	int r;
{
	int c = isdbl(v->vtype) ? 'd' : 's';

	if (!isflt(v->vtype)) {
		fprintf(stderr, "f1: an integer value where a float was wanted\n");
		exit(2);
	}
	switch (v->kind) {
	case V_REG:
		if (v->reg != r)
			printf("\tfmov\t%c%d, %c%d\n", c, r, c, v->reg);
		break;
	case V_MEM:
		printf("\tldr\t%c%d, [x%d, #%ld]\n", c, r, v->reg, v->con);
		break;
	case V_VAR:
		addrinto(v, SCRATCHC);
		printf("\tldr\t%c%d, [x%d]\n", c, r, SCRATCHC);
		break;
	default:
		fprintf(stderr, "f1: a floating-point constant with no storage\n");
		exit(2);
	}
}

/*
 * The condition a comparison stands for, in its TRUE sense.  CBRANCH wants the
 * inverse and asks for it separately, because the two readings are different
 * claims and writing one as `not the other' is how the sense got inverted twice
 * while this pass was being written.
 *
 * The float comparisons use the same names, which is exact for every value
 * Fortran 77 can produce and wrong only for a NaN: after `fcmp' an unordered
 * result sets C and V, so `lt' -- which is N!=V -- reads true where IEEE says
 * false.  1977 Fortran has no way to write a NaN and no intrinsic that returns
 * one, so the distinction has no source-level spelling to be wrong about.
 */
static char *
ccname(o)
	int o;
{
	switch (o) {
	case P2EQ: return ("eq");
	case P2NE: return ("ne");
	case P2LE: return ("le");
	case P2LT: return ("lt");
	case P2GE: return ("ge");
	case P2GT: return ("gt");
	}
	fprintf(stderr, "f1: a branch on a value that is not a comparison\n");
	exit(2);
	/*NOTREACHED*/
	return ("");
}

/*
 * Materialise into a FRESH register of the right class and return a value that
 * owns it.  This is what every operator uses for its operands, so an operand
 * that is already in a register costs nothing and one that is a name costs the
 * load it was always going to need.
 */
static struct val
mater(v)
	struct val *v;
{
	struct val r;
	int flt;

	if (v->kind == V_REG && v->owned)
		return (*v);
	/* A COMPARISON BECOMES A 0/1 VALUE HERE, and this is the only place it
	   can: everywhere else it lives in the flags, which is what the hardware
	   wants for the common `cmp' followed by `b.cc'.  See flushcc(). */
	if (v->kind == V_CC) {
		r.kind = V_REG;
		r.con = 0;
		r.name[0] = '\0';
		r.vtype = P2INT;
		r.reg = ralloc(0);
		r.owned = 1;
		printf("\tcset\tw%d, %s\n", r.reg, ccname((int) v->con));
		return (r);
	}
	flt = isflt(v->vtype);
	r.kind = V_REG;
	r.con = 0;
	r.name[0] = '\0';
	r.vtype = v->vtype;
	r.reg = ralloc(flt);
	r.owned = 1;
	if (flt)
		finto(v, r.reg);
	else
		into(v, r.reg);
	vfree(v);
	return (r);
}

/*
 * TURN EVERY PENDING COMPARISON INTO A VALUE, because the flags are a single
 * global register and the next `cmp' destroys them.
 *
 * `if (i .gt. 3 .and. j .lt. 4)' emits GT, then LT, then ANDAND -- so the
 * second comparison overwrites the first one's flags before anything has read
 * them.  Called at the top of the comparison arms, which is the only thing that
 * writes flags, so a comparison stays lazy in the common case where CBRANCH
 * reads it immediately and is spilled to a register exactly when a second one
 * is about to arrive.  This was wrong before ANDAND existed; nothing had
 * produced two comparisons in one expression, so nothing could show it.
 */
static void
flushcc()
{
	struct val t;
	int i;

	for (i = 0; i < vsp; i++)
		if (vstack[i].kind == V_CC) {
			t = mater(&vstack[i]);
			vstack[i] = t;
		}
}


/*
 * Materialise a value AS a given type, which is not the same as materialising
 * it: an operator's type in pcc is the type of its RESULT, and its operands are
 * allowed to be narrower.  f77 leans on that for the float widths and not for
 * the integer ones -- measured, `d = d + r' over a DOUBLE PRECISION and a REAL
 * arrives as
 *
 *	NAME "v.1" type double / NAME "v.2" type real / PLUS type double
 *
 * with no CONV between them, while `d = d * i' over an INTEGER gets an explicit
 * `CONV type double'.  So half the widening is stated and half is implied, and
 * a pass that honours only the stated half reads a single-precision bit pattern
 * with `fadd d' and gets a plausible number: 2.5 + 1.5 came out 2.5, and the
 * program printed 7.5 where it should have printed 12.  Nothing refused,
 * because every operator involved was one this pass knows.
 */
static struct val
materas(v, t)
	struct val *v;
	int t;
{
	struct val r, s;

	if (isflt(t) && isflt(v->vtype)) {
		s = mater(v);
		if (isdbl(t) == isdbl(s.vtype))
			return (s);
		r.kind = V_REG; r.con = 0; r.name[0] = '\0';
		r.vtype = t; r.reg = ralloc(1); r.owned = 1;
		printf("\tfcvt\t%c%d, %c%d\n",
		    isdbl(t) ? 'd' : 's', r.reg,
		    isdbl(s.vtype) ? 'd' : 's', s.reg);
		vfree(&s);
		return (r);
	}
	if (isflt(t) && !isflt(v->vtype)) {
		s = mater(v);
		r.kind = V_REG; r.con = 0; r.name[0] = '\0';
		r.vtype = t; r.reg = ralloc(1); r.owned = 1;
		printf("\tscvtf\t%c%d, w%d\n",
		    isdbl(t) ? 'd' : 's', r.reg, s.reg);
		vfree(&s);
		return (r);
	}
	if (!isflt(t) && isflt(v->vtype)) {
		/* The other direction, which `integer i; real r; i = r' takes.
		   Without it mater() hands back a FLOAT register and the store
		   names an integer one of the same number -- so `i = r' would
		   have stored x8's contents, silently. */
		s = mater(v);
		r.kind = V_REG; r.con = 0; r.name[0] = '\0';
		r.vtype = t; r.reg = ralloc(0); r.owned = 1;
		printf("\tfcvtzs\tw%d, %c%d\n",
		    r.reg, isdbl(s.vtype) ? 'd' : 's', s.reg);
		printf("\tsxtw\tx%d, w%d\n", r.reg, r.reg);
		vfree(&s);
		return (r);
	}
	return (mater(v));
}

/*
 * CALL and CALL0.  The callee and its arguments are the top two LOGICAL values
 * -- see the note on lstack above -- so this needs no arity from the record and
 * no assumption that a call is a whole statement.  AAPCS64 puts the first eight
 * arguments in x0-x7 and the rest at [sp,#0] upward, which is the call area
 * prsave() reserves below x29; past MAXARGSLOT the frame has no room and this
 * refuses rather than truncating.
 */
static void
docall(hasargs)
	int hasargs;
{
	struct val f, r;
	int n, i, base, fbase;

	/* A CALL WRITES THE FLAGS TOO, AND flushcc()'s SURVEY SAID ONLY
	 * COMPARISONS DID.  That was a true and complete account of the code
	 * that existed when it was written; a `bl' arrived later.  So
	 *
	 *	if (i .lt. 3 .and. f(j) .gt. 1)
	 *
	 * emitted `cmp' for the first test, `bl _f_' -- which destroys NZCV --
	 * and only then, at the second comparison, did flushcc() run `cset' on
	 * flags the callee had overwritten.  Measured: with a callee that
	 * leaves LT set on the way out, .FALSE. .AND. .TRUE. came out TRUE.
	 * Nothing refused and nothing crashed, which is what makes it worse
	 * than the four refusals this pass answered with.
	 *
	 * Here rather than after the arguments, because in postfix everything
	 * between the comparison and this point is address pushes, which write
	 * no flags -- and a nested call flushes before its own `bl', so the
	 * induction closes.  The pool is callee-saved (x23-x27), so a value
	 * materialised now survives the call it is being protected from.
	 */
	flushcc();

	if (lsp < (hasargs ? 2 : 1)) {
		fprintf(stderr, "f1: call with no callee\n");
		exit(2);
	}
	if (hasargs) {
		base = lstack[lsp - 1];		/* the argument run */
		fbase = lstack[lsp - 2];	/* the callee */
		n = vsp - base;
	} else {
		base = fbase = lstack[lsp - 1];
		n = 0;
	}
	/* AN INDIRECT CALL IS `blr', AND IT IS WHAT PASSING A PROCEDURE MEANS.
	 * `external sq / call apply(sq, x)' spills sq's address into apply's
	 * parameter area, so at the call site the callee arrives as an OREG --
	 * a VALUE -- rather than as the ICON-with-a-name every direct call
	 * gives.  The refusal here named it "a call through a value rather
	 * than a name", which was an accurate description of a construct
	 * Fortran has had since 1966.
	 *
	 * Materialised BEFORE the arguments are placed, and into the pool,
	 * which is callee-saved: x0-x7 are about to be written, and a callee
	 * address sitting in one of them would be overwritten by the argument
	 * that belongs there.  Done through the stack slot rather than through
	 * the local copy, so the cleanup loop below frees the register exactly
	 * once.
	 */
	if (vstack[fbase].kind != V_ADDR)
		vstack[fbase] = mater(&vstack[fbase]);
	f = vstack[fbase];
	if (n > MAXARGSLOT) {
		fprintf(stderr,
		    "f1: %d arguments, more than the frame holds\n", n);
		exit(2);
	}

	/* A FLOAT PASSED BY VALUE PROMOTES TO DOUBLE, WHICH IS K&R's RULE AND
	 * NOT A SPECIAL CASE FOR ANY PARTICULAR CALLEE.  Fortran passes
	 * everything by reference, so an ordinary call hands over ICONs --
	 * addresses, whose type carries PTR and for which isflt() is false.  A
	 * float VALUE reaches here only from intr.c:381-396's callbyvalue[],
	 * thirteen C math functions f77 calls directly as OPCCALL rather than
	 * through libF77's by-reference wrapper, and `double sqrt(double)' is
	 * what src/libc/math/sqrt.c declares.  The promotion is what a K&R C
	 * compiler would have emitted at that call.
	 */
	for (i = 0; i < n; i++) {
		/* THE NINTH ARGUMENT ONWARD GOES TO MEMORY, and it is staged
		   through a scratch register rather than placed directly
		   because there is no register to place it in -- that is the
		   whole of what "beyond x0-x7" means.  SCRATCHB is caller-saved
		   and dead here: the values being placed live in the pool
		   (x19-x28) or in x29-relative memory, and nothing this loop
		   writes is read by a later iteration.

		   sp is the base rather than x29 because AAPCS64 states the
		   outgoing area from sp and the CALLEE has no x29 of its own
		   yet.  sp does not move inside a procedure -- prsave() sets it
		   once -- so the two ends meet at exactly one address. */
		int r = i < 8 ? i : SCRATCHB;

		if (isflt(vstack[base + i].vtype)) {
			struct val d;

			d = materas(&vstack[base + i], P2DREAL);
			/* v8cc passes a double in an X register -- its own
			   convention, not AAPCS64's, and the half of the pair
			   that differs from the return.  The note in into()
			   has the measurement. */
			printf("\tfmov\tx%d, d%d\n", r, d.reg);
			vstack[base + i] = d;	/* freed by the loop below */
		} else
			into(&vstack[base + i], r);
		if (i >= 8)
			printf("\tstr\tx%d, [sp, #%d]\n",
			    SCRATCHB, ARGSLOT * (i - 8));
	}
	if (f.kind == V_ADDR)
		printf("\tbl\t%s\n", f.name);
	else
		printf("\tblr\tx%d\n", f.reg);

	/* Give back everything the arguments were holding, then take the result
	   OUT of x0 immediately.  A second call in the same expression -- which
	   `g(p) + g(q)' is -- would otherwise overwrite it, and that is why the
	   pool is callee-saved. */
	for (i = 0; i < n; i++)
		vfree(&vstack[base + i]);
	vfree(&vstack[fbase]);
	vsp = fbase;
	lsp -= hasargs ? 2 : 1;

	r.kind = V_REG;
	r.con = 0;
	r.name[0] = '\0';
	r.vtype = type;
	r.owned = 1;
	if (isflt(type)) {
		/*
		 * A FLOATING RESULT IS ALWAYS A DOUBLE IN d0, WHATEVER THE
		 * STREAM CALLS THE CALL.  K&R C has no float return, so every
		 * callee this pass can reach widens: measured, not one of the
		 * 66 typed functions in src/libF77, src/libI77 or src/libc/math
		 * returns `float' -- they are all `double'.  And a FORTRAN
		 * function is the same, because putpcc.c:551-552 forces a
		 * TYREAL result as P2DREAL; see the note at P2FORCE.
		 *
		 * f77's own table calls these results real anyway --
		 * `{ TYREAL, TYREAL, 1, "r_sqrt", 1 }' against
		 * `double r_sqrt(float *x)' -- and ON A VAX THAT COST NOTHING,
		 * for the reason wrt_E's bug cost nothing until it met IEEE:
		 * D_floating's leading 32 bits have F_floating's exact layout,
		 * so reading a returned double's first word as a float is the
		 * same number.  Here d0's low half is the low mantissa.  This
		 * is the second instance of that coincidence, in a second
		 * component -- and it is why `x ** 0.5' printed 3.01e+23 for
		 * the square root of two: pow_dd returned a double and `fmov
		 * s8, s0' read the bottom of it.
		 */
		r.reg = ralloc(1);
		if (isdbl(type))
			printf("\tfmov\td%d, d0\n", r.reg);
		else
			printf("\tfcvt\ts%d, d0\n", r.reg);
	} else {
		r.reg = ralloc(0);
		printf("\tmov\tx%d, x0\n", r.reg);
	}
	vpush(&r);
}

/*
 * ASSIGN.  The left operand is the object being stored into and the right is the
 * value.  The store width comes from the type word: an INTEGER is four bytes
 * here, which is SZLONG in f77's arm64defs, pinned there by typesize[TYREAL] and
 * by lengtype()'s hardcoded INTEGER*4.  Getting this from the type rather than
 * assuming eight is what keeps a Fortran INTEGER four bytes all the way to the
 * store -- and what keeps a POINTER eight.
 */
/*
 * `ASSIGN 20 TO lbl' STORES A CODE ADDRESS IN A FOUR-BYTE INTEGER, AND THE WAY
 * TO MAKE THAT FIT IS TO STORE AN OFFSET RATHER THAN AN ADDRESS.
 *
 * exec.c:554 requires the ASSIGN variable to be an integer, INTEGER is four
 * bytes here (SZLONG, pinned by typesize[TYREAL] and by lengtype), and Mach-O
 * loads text above 4GB -- measured, `str w23, [x13]' drops the 1 in bit 32 of
 * 0x10001f140.  On a VAX an address and an INTEGER were both four bytes and it
 * fit exactly, which is why upstream needs no mechanism here at all.
 *
 * So the four bytes hold `target - Lf1b<proc>', the distance from a label this
 * pass emits at the head of each procedure, and the branch adds it back.  Two
 * properties make that exact rather than approximate: the difference of two
 * addresses in one image fits in 32 bits by construction (a Mach-O image is far
 * below 2GB), and BOTH ENDS COMPUTE IT AT RUN TIME from adrp/add pairs -- so it
 * does not depend on the two labels sharing a section, which f77pass1's
 * interleaved constant pool would otherwise make a question.
 *
 * The alternative was an index into a table of `.quad L13', which is what the
 * computed GOTO does.  It needs per-procedure state (the set of labels ASSIGNed
 * to, collected until RBRACKET) and a second relocation-bearing table in
 * __DATA,__const.  This needs neither, and no state beyond the procedure number.
 */
static void
labelbase(r)
	int r;
{
	printf("\tadrp\tx%d, Lf1b%d@PAGE\n", r, curproc);
	printf("\tadd\tx%d, x%d, Lf1b%d@PAGEOFF\n", r, r, curproc);
}

static struct val
doassign()
{
	struct val rhs, lhs;
	int flt, c, r;

	rhs = vpop();
	lhs = vpop();
	flt = isflt(lhs.vtype);
	c = isdbl(lhs.vtype) ? 'd' : 's';

	/* AN ADDRESS GOING INTO A NARROWER INTEGER IS `ASSIGN n TO v', AND IT IS
	   THE ONLY THING IN FORTRAN THAT PRODUCES THIS SHAPE -- every other
	   assignment has a source at least as wide as its destination.  Without
	   this the store below is `str w', which keeps the low half of a text
	   address and discards the half that says which 4GB it was in.  See
	   labelbase() above for why an offset is stored instead.

	   THE PRODUCER IS UNIQUE AND THE CONSUMER IS NOT, which is a separate
	   claim and the sentence above does not make it.  Fortran 77 lets an
	   ASSIGNed variable be a FORMAT specifier as well as a branch target,
	   and io.c:691 -- the arm f77's own front end labels `ASSIGNed label'
	   -- hands its value to libI77 as a char * with nothing adding a base
	   back.
	   That program compiles clean and SIGSEGVs -- and did so before this
	   encoding existed too, by truncation instead, so it is a standing gap
	   rather than one this created.  Closing it means the FRONT END saying
	   which statement an ASSIGN is instead of this pass inferring it from
	   the node shape; src/cmd/f77/PORTING.md carries the costing. */
	if (!flt && isptr(rhs.vtype) && !isptr(lhs.vtype)) {
		struct val o;

		/* AND THE DESTINATION HAS TO BE ABLE TO HOLD THE DISTANCE.
		   exec.c gates ASSIGN on MSKINT, which admits TYSHORT, so
		   `integer*2 lbl' reaches here and the store below is `strh'
		   -- sixteen bits of a thirty-two-bit difference, with the
		   branch reading it back through `ldrsh'.  It is right until
		   the procedure grows: measured, the same source at a distance
		   of 28880 bytes printed the right answer and at 108080 bytes
		   SIGSEGV'd, with no diagnostic at either end.  A refusal is
		   the honest answer, because the alternative is a program that
		   works only while it is small.

		   Spelled as stop()'s own switch rather than as a compare
		   against what stop() returns: <string.h> is not included
		   here and an undeclared strcmp() is the truncated-pointer
		   class this port keeps finding. */
		if ((lhs.vtype & BTMASK) == P2CHAR ||
		    (lhs.vtype & BTMASK) == P2SHORT) {
			fprintf(stderr,
			    "f1: an ASSIGNed label needs a 4-byte INTEGER;\n");
			fprintf(stderr,
			    "    a narrower one cannot hold the distance\n");
			exit(2);
		}

		o.kind = V_REG; o.con = 0; o.name[0] = '\0';
		o.vtype = P2INT; o.owned = 1; o.reg = ralloc(0);
		addrinto(&rhs, o.reg);
		labelbase(SCRATCHC);
		printf("\tsub\tx%d, x%d, x%d\n", o.reg, o.reg, SCRATCHC);
		/* Kept sign-extended, which is what every other int here is and
		   what the `ldrsw' at the branch expects to read back. */
		printf("\tsxtw\tx%d, w%d\n", o.reg, o.reg);
		vfree(&rhs);
		rhs = o;
	}

	/* AS the destination's type, not merely into a register: `d = r' and
	   `i = r' both cross a width here, and the store below names a register
	   NUMBER whose class comes from the destination. */
	rhs = materas(&rhs, lhs.vtype);
	r = rhs.reg;

	switch (lhs.kind) {
	case V_MEM:
		if (flt)
			printf("\tstr\t%c%d, [x%d, #%ld]\n", c, r, lhs.reg, lhs.con);
		else
			printf("\t%s\t%c%d, [x%d, #%ld]\n", stop(lhs.vtype),
			    stwide(lhs.vtype) ? 'x' : 'w', r, lhs.reg, lhs.con);
		break;
	/* A DO loop assigns to its control variable, which pass 1 may have put
	   in a register -- so a REG is a legitimate assignment target and the
	   store is a move.  Kept sign-extended for the reason every other
	   integer here is. */
	case V_REG:
		if (flt)
			printf("\tfmov\t%c%d, %c%d\n", c, lhs.reg, c, r);
		else {
			printf("\tmov\tw%d, w%d\n", lhs.reg, r);
			printf("\tsxtw\tx%d, w%d\n", lhs.reg, lhs.reg);
		}
		break;
	case V_VAR:
	case V_ADDR:
		addrinto(&lhs, SCRATCHB);
		if (flt)
			printf("\tstr\t%c%d, [x%d]\n", c, r, SCRATCHB);
		else
			printf("\t%s\t%c%d, [x%d]\n", stop(lhs.vtype),
			    stwide(lhs.vtype) ? 'x' : 'w', r, SCRATCHB);
		break;
	default:
		fprintf(stderr, "f1: assignment to a %s, which is not an lvalue\n",
			lhs.kind == V_CON ? "constant" : "comparison");
		exit(2);
	}
	/* AND IT HAS A VALUE, because an assignment is an EXPRESSION -- which is
	   pcc's rule and not a convenience.  A Fortran argument that is not a
	   variable has to be given storage to be passed by reference, and f77
	   builds that as `(temp = n-1, &temp)': an ASSIGN with a COMOP over it,
	   in the middle of a call's argument list.  Treating ASSIGN as a
	   statement and clearing the stack after it threw away the callee and
	   the half-built expression underneath, and the next record underflowed.
	   The caller decides what to do with the value; the statement boundary
	   is P2STMT, and that is the only thing that resets.

	   THE VALUE IS THE OBJECT ASSIGNED TO, NOT THE REGISTER THE VALUE PASSED
	   THROUGH, AND THAT IS WHERE THIS PASS'S REGISTER PRESSURE WAS.  Both
	   answer the same number -- after the store, reading the destination
	   yields exactly what was stored -- but the register form pins a pool
	   register until something pops it, and the only two things that do are
	   COMOP and the statement boundary.  A CHARACTER concatenation is one
	   statement containing an ASSIGN PER OPERAND: f77 builds two arrays in
	   the frame, a length and a pointer for each piece, and calls s_cat over
	   them.  So the registers held were 2n and nothing was using them.

	   Measured, distinct pool registers in the emitted code: one-way 1,
	   two-way 6, three-way 8, four-way 10, five-way refused.  Returning the
	   lvalue costs nothing anywhere else -- COMOP frees its left operand
	   either way, and a consumer that reads this as a value re-loads from
	   the destination, which is the same number and, where the destination
	   is narrower, the same TRUNCATION the store just performed.

	   V_ADDR becomes V_VAR because those two differ in exactly this: an ICON
	   destination means `store to this address', so what it names as a VALUE
	   is the object's contents rather than the address.  The other three
	   kinds re-read correctly as they stand.

	   THAT LINE IS DEFENSIVE RATHER THAN LOAD-BEARING, AND IT IS MEASURED
	   RATHER THAN ASSUMED: an instrumented f1 reporting every V_ADDR
	   destination found ZERO over the whole wavea suite and a sixty-program
	   corpus -- f77 spells a destination as OREG, NAME, REG or a dereference
	   and never as an ICON.  Kept because the store switch above already has
	   a `case V_ADDR' and the returned value has to agree with it; a
	   mutation of it therefore fires nothing, and writing a case for it
	   would be manufacturing a vacuous one on purpose.  Same verdict as
	   v8_bufend's NULL arm in shim/libI77/sysv.c. */
	vfree(&rhs);
	if (lhs.kind == V_ADDR)
		lhs.kind = V_VAR;
	return (lhs);
}

static char *
typename(t)
	int t;
{
	static char b[64];
	char *base;
	int mods = t >> BTSHIFT, n = 0;

	switch (t & BTMASK) {
	case 2:  base = "char";   break;
	case 3:  base = "short";  break;
	case 4:  base = "int";    break;
	case 6:  base = "real";   break;
	case 7:  base = "double"; break;
	default: base = "?";      break;
	}
	sprintf(b, "%s", base);
	while (mods && n++ < 4) {
		strcat(b, (mods & 3) == 1 ? " *" : " ()");
		mods >>= 2;
	}
	return (b);
}

static char *
opname(o)
	int o;
{
	switch (o) {
	case P2NAME:	return ("NAME");
	case P2ICON:	return ("ICON");
	case P2PLUS:	return ("PLUS");
	case P2PLUSEQ:	return ("PLUSEQ");
	case P2STAREQ:	return ("STAREQ");
	case P2MINUS:	return ("MINUS");
	case P2NEG:	return ("NEG");
	case P2STAR:	return ("STAR");
	case P2SLASH:	return ("SLASH");
	case P2MOD:	return ("MOD");
	case P2LSHIFT:	return ("LSHIFT");
	case P2RSHIFT:	return ("RSHIFT");
	case P2BITAND:	return ("BITAND");
	case P2BITOR:	return ("BITOR");
	case P2BITXOR:	return ("BITXOR");
	case P2BITNOT:	return ("BITNOT");
	case P2NOT:	return ("NOT");
	case P2ANDAND:	return ("ANDAND");
	case P2OROR:	return ("OROR");
	case P2QUEST:	return ("QUEST");
	case P2COLON:	return ("COLON");
	case P2INDIRECT:return ("INDIRECT");
	case P2ASSIGN:	return ("ASSIGN");
	case P2COMOP:	return ("COMOP");
	case P2LISTOP:	return ("LISTOP");
	case P2CALL:	return ("CALL");
	case P2CALL0:	return ("CALL0");
	case P2GOTO:	return ("GOTO");
	case P2FORCE:	return ("FORCE");
	case P2CBRANCH:	return ("CBRANCH");
	case P2CONV:	return ("CONV");
	case P2REG:	return ("REG");
	case P2OREG:	return ("OREG");
	case P2EQ:	return ("EQ");
	case P2NE:	return ("NE");
	case P2LE:	return ("LE");
	case P2LT:	return ("LT");
	case P2GE:	return ("GE");
	case P2GT:	return ("GT");
	case P2PASS:	return ("PASS");
	case P2STMT:	return ("STMT");
	case P2LBRACKET:return ("LBRACKET");
	case P2RBRACKET:return ("RBRACKET");
	case P2LABEL:	return ("LABEL");
	case P2ARIF:	return ("ARIF");
	case P2SWITCH:	return ("SWITCH");
	case P2EOF:	return ("EOF");
	/* AN UNKNOWN OPERATOR NAMES ITS NUMBER, because `?' is what a survey of
	   the stream cannot use.  Building a histogram over a corpus to find out
	   which operators Fortran actually reaches reported six `?' -- silently
	   merging LSHIFT, MOD and NEG, the three the survey existed to find.  An
	   instrument that hides exactly the thing it is pointed at. */
	default:
		{
		static char b[16];

		sprintf(b, "op%d", o);
		return (b);
		}
	}
}

/*
 * The dump mode, which is what makes this file testable BEFORE it emits a
 * single instruction: it prints the record stream in the terms the format
 * defines, and tests/wavea compares that against a decode done by hand from a
 * hexdump.  A reader that agrees with an independent decode of the same bytes
 * is the only claim worth making at this stage, and it is one that can be made
 * now rather than after the code generator works.
 */
static void
dumprec()
{
	long v;
	char buf[512];

	printf("%-9s", opname(op));
	switch (op) {
	case P2PASS:
		getstr(buf, var);
		printf(" \"%s\"", buf);
		break;
	case P2STMT:
		if (var) {
			getstr(buf, var);
			printf(" file \"%s\"", buf);
		} else
			printf(" line %d", type);
		break;
	case P2LBRACKET:
		if (!getword(&v))
			v = 0;
		printf(" regvars %d proc %d autobits %ld\n", var, type, v);
		return;
	case P2RBRACKET:
		printf(" proc %d", type);
		break;
	case P2LABEL:
		printf(" L%d", type);
		break;
	case P2GOTO:
		/* `var' says whether `type' is a label or a pcc type; see the
		   note in genrec(). */
		if (var)
			printf(" L%d", type);
		else
			printf(" indirect, through the top of the stack");
		break;
	case P2ICON:
		if (!getword(&v))
			v = 0;
		printf(" %ld type %s", v, typename(type));
		if ((type >> BTSHIFT) != 0) {
			getname(buf);
			printf(" name \"%s\"", buf);
		}
		break;
	case P2NAME:
		/* `var' IS A FLAG HERE RATHER THAN A REGISTER, and when it is set
		   an offset word precedes the name.  See the note in genrec(). */
		if (var) {
			if (!getword(&v))
				v = 0;
			getname(buf);
			printf(" \"%s\" offset %ld type %s", buf, v,
			    typename(type));
		} else {
			getname(buf);
			printf(" \"%s\" type %s", buf, typename(type));
		}
		break;
	case P2OREG:
		if (!getword(&v))
			v = 0;
		getname(buf);
		printf(" reg %d offset %ld type %s", var, v, typename(type));
		break;
	case P2REG:
		printf(" reg %d type %s", var, typename(type));
		break;
	default:
		printf(" type %s", typename(type));
		break;
	}
	printf("\n");
}


/*
 * genrec -- one record, compiled.  The twelve operators below are what a whole
 * Fortran program uses; anything else is refused BY NAME rather than ignored,
 * because a code generator that silently skips an operator emits a program that
 * links and computes the wrong thing.  That is the failure mode this port has
 * spent its life finding, so the default arm is an error with the opcode in it.
 */
static void
genrec()
{
	struct val v;
	long w;
	char buf[512];

	switch (op) {

	/* P2PASS carries literal assembly through -- prolog()'s label, prarif()'s
	   comparisons, the .text directives.  For a small program it is most of
	   the file, so the copy-through path is the common case rather than an
	   edge one. */
	case P2PASS:
		getstr(buf, var);
		printf("%s\n", buf);
		/* THE FRAME IS NOT THIS PASS'S, and finding that out is what the
		   first working version measured.  The entry stub arrives AFTER
		   the body -- putbracket() rewrites the header in place -- so an
		   epilogue emitted at RBRACKET lands after the stub's branch,
		   unreachable, with the body running off the end.  prsave() and
		   goret() in src/cmd/f77/arm64.c are called at exactly the right
		   points and emit both halves as literal PASS text, which is
		   what the VAX did too. */
		break;

	case P2STMT:
		if (var)
			getstr(buf, var);	/* the filename; consumed, not emitted */
		vreset();
		break;

	case P2LBRACKET:
		if (!getword(&w))
			w = 0;
		/* THE BASE AN ASSIGNED GOTO MEASURES FROM.  Emitted here because
		   this record is the head of the procedure -- the stream opens
		   `PASS ".text"' then LBRACKET -- so the label lands in the text
		   section ahead of the body.  One local label per procedure,
		   whether or not anything ASSIGNs: naming it from the procedure
		   number is what lets the two USE sites agree without either
		   knowing the other exists.  `Lf1b' cannot collide with pass 1,
		   whose labels are L followed by digits. */
		curproc = type;
		printf("Lf1b%d:\n", curproc);
		/* `var' IS THE NUMBER OF REGISTER VARIABLES PASS 1 TOOK, and it
		   opens every procedure, so the pool is sized per procedure
		   rather than at the worst case.  Clamped rather than trusted:
		   a count above MAXREGVAR would mean pass 1 and this pass
		   disagree about regnum[], and shrinking to the six that are
		   ours unconditionally is the safe reading of that. */
		ipooln = IPOOLFIXED + IPOOLREGVAR - var;
		if (ipooln < IPOOLFIXED)
			ipooln = IPOOLFIXED;
		if (ipooln > IPOOLFIXED + IPOOLREGVAR)
			ipooln = IPOOLFIXED + IPOOLREGVAR;
		vreset();
		break;

	case P2RBRACKET:
		break;

	case P2LABEL:
		printf("L%d:\n", type);
		vreset();
		break;

	/*
	 * GOTO, AND `var' IS AGAIN A FLAG RATHER THAN A REGISTER.  Upstream
	 * writes the two forms differently and only one of them carries a
	 * label:
	 *
	 *	putgoto()   putpcc.c:172  p2triple(P2GOTO, 1, label)
	 *	putbranch() putpcc.c:182  p2op(P2GOTO, P2INT)
	 *
	 * -- so with var clear the destination is the VALUE on the stack and
	 * `type' is P2INT, which is 4.  Reading it as a label emitted `b L4'
	 * for every assigned GOTO in the program, and the assembler reported
	 * an undefined local symbol: a diagnostic naming a label the program
	 * never had.
	 *
	 * THE INDIRECT FORM IS IMPLEMENTED BELOW; what an INTEGER holds is not
	 * the address.  See labelbase() and doassign() above for the encoding
	 * and the measurement behind it -- this paragraph used to argue that
	 * the form could not work at all, which was the right diagnosis
	 * (four bytes cannot hold a Mach-O text address) attached to an
	 * inference nobody had stated out loud, that the four bytes had to
	 * hold the address rather than something the branch can turn back
	 * into one.
	 */
	case P2GOTO:
		if (var) {
			printf("\tb\tL%d\n", type);
		} else {
			/* AN INDIRECT GOTO HAS TWO PRODUCERS AND THE TYPE IS
			   WHAT SEPARATES THEM.  Fortran's ASSIGN puts a label
			   in an INTEGER, which is four bytes and cannot hold
			   an address, so doassign() stores the distance from
			   Lf1b<proc> and this adds it back.  f77's OWN use of
			   the same record does not: a procedure with more than
			   one ENTRY returns through `OREG reg 29 offset 8 type
			   int *' -- eight bytes, holding the whole address of
			   the typed epilogue -- and adding a base to that
			   branches into nothing.

			   When this was written the assigned GOTO was the only
			   producer, which made `always add the base' a true and
			   complete account of the code that existed; the second
			   arrived one refusal later.  Measured: without the
			   test, an INTEGER FUNCTION with an ENTRY printed
			   nothing and exited 0.

			   `br' rather than `blr' either way: a GOTO is a jump,
			   not a call. */
			struct val g;

			g = vpop();
			into(&g, SCRATCHB);
			if (!isptr(g.vtype)) {
				labelbase(SCRATCHC);
				printf("\tadd\tx%d, x%d, x%d\n",
				    SCRATCHB, SCRATCHB, SCRATCHC);
			}
			printf("\tbr\tx%d\n", SCRATCHB);
			vfree(&g);
		}
		vreset();
		break;

	/* A NAME'S `var' FIELD IS A FLAG, NOT A REGISTER NUMBER, AND WHEN IT IS
	 * SET AN OFFSET WORD COMES BEFORE THE NAME.  putpcc.c:1232-1235 is
	 *
	 *	p2triple(P2NAME, offset!=0, type2);
	 *	if(offset != 0) p2word(offset);
	 *	p2name(name);
	 *
	 * so a reader that goes straight for the name eats the offset's four
	 * bytes as the head of the string.  Everything after that is read at the
	 * wrong alignment, and the first thing the misalignment produces is an
	 * opcode of 0 -- which is not an operator at all, so the diagnostic
	 * named a construct that was never in the program.  The trigger is any
	 * CONSTANT subscript past the first: a(1) has offset 0 and set no flag,
	 * so `a(1) = 1' worked and `a(2) = 2' did not, in a plain local array.
	 * A VARIABLE subscript computes its address with PLUS/STAR instead and
	 * was never affected, which is why 2-D arrays and DO loops were fine.
	 *
	 * The offset goes in `con', which addrinto() already adds to the page
	 * address -- and already handles a NEGATIVE one, for the separate reason
	 * recorded there.  So nothing downstream needed changing.
	 */
	case P2NAME:
		if (var) {
			if (!getword(&w))
				w = 0;
		} else
			w = 0;
		getname(buf);
		v.kind = V_VAR; v.con = w; v.reg = -1; v.vtype = type;
		v.owned = 0;
		strncpy(v.name, buf, sizeof(v.name)-1);
		v.name[sizeof(v.name)-1] = '\0';
		vpush(&v);
		break;

	/* An ICON is a literal UNLESS its type carries a modifier, in which case
	   it is an address constant and a name follows -- pcc1 stacks PTR and
	   FUNCT four bits at a time above the base type, so a nonzero high field
	   is exactly that test. */
	case P2ICON:
		if (!getword(&w))
			w = 0;
		if ((type >> BTSHIFT) != 0) {
			getname(buf);
			v.kind = V_ADDR;
			strncpy(v.name, buf, sizeof(v.name)-1);
			v.name[sizeof(v.name)-1] = '\0';
		} else {
			v.kind = V_CON;
			v.name[0] = '\0';
		}
		v.con = w; v.reg = -1; v.vtype = type; v.owned = 0;
		vpush(&v);
		break;

	/* LISTOP is structural: in postfix its two operands are already adjacent
	   on the stack, so joining them is a no-op PHYSICALLY -- but it does
	   merge two LOGICAL values into one, which is what tells CALL where its
	   argument run begins.  See the note on lstack. */
	case P2LISTOP:
		if (lsp < 2) {
			fprintf(stderr, "f1: a list of fewer than two things\n");
			exit(2);
		}
		lsp--;
		break;

	/*
	 * THE BINARY ARITHMETIC, AND ITS WIDTH IS THREE CASES RATHER THAN ONE.
	 * The first version emitted `add w12, w12, w13' for every PLUS whatever
	 * the type said, which is right for exactly one of the three:
	 *
	 *   an INTEGER is four bytes -- SZLONG in arm64defs, pinned by
	 *	typesize[TYREAL] and by lengtype()'s hardcoded INTEGER*4 -- so it
	 *	computes in w and is sign-extended after, for the reason
	 *	arm64_trunc() exists in compiler/ccom-arm64/gencode.c;
	 *
	 *   a POINTER is eight, and `PLUS type int *' is how an array subscript
	 *	reaches its element: a(i) is (&a - 4) + (i<<2).  In w that
	 *	truncates the address;
	 *
	 *   a REAL or a DOUBLE is not an integer at all, and `x + 2.25' in w
	 *	adds the two BIT PATTERNS.  Measured: the program compiled, linked,
	 *	ran, and hung inside libI77's formatter on the nonsense that came
	 *	out.  The refusal-by-name design cannot see this class, because
	 *	every operator involved is one this pass knows -- a guard on the
	 *	vocabulary is not a guard on the grammar.
	 */
	case P2PLUS: case P2MINUS: case P2STAR: case P2SLASH:
		{
			struct val a, b, r;
			char *o;
			int c;

			b = vpop(); a = vpop();
			if (isflt(type)) {
				c = isdbl(type) ? 'd' : 's';
				a = materas(&a, type); b = materas(&b, type);
				o = op == P2PLUS  ? "fadd" :
				    op == P2MINUS ? "fsub" :
				    op == P2STAR  ? "fmul" : "fdiv";
				printf("\t%s\t%c%d, %c%d, %c%d\n",
				    o, c, a.reg, c, a.reg, c, b.reg);
				vfree(&b);
				r = a; r.vtype = type;
				vpush(&r);
				break;
			}
			a = mater(&a); b = mater(&b);
			o = op == P2PLUS  ? "add" :
			    op == P2MINUS ? "sub" :
			    op == P2STAR  ? "mul" : "sdiv";
			if (isptr(type))
				printf("\t%s\tx%d, x%d, x%d\n",
				    o, a.reg, a.reg, b.reg);
			else {
				printf("\t%s\tw%d, w%d, w%d\n",
				    o, a.reg, a.reg, b.reg);
				printf("\tsxtw\tx%d, w%d\n", a.reg, a.reg);
			}
			vfree(&b);
			r = a; r.vtype = type;
			vpush(&r);
		}
		break;

	/* MOD has no instruction: arm64 dropped the divide-with-remainder the
	   VAX had, so it is a divide and a multiply-subtract.  The scratch is a
	   third register because both operands are still live at the msub. */
	case P2MOD:
		{
			struct val a, b, r;

			b = vpop(); a = vpop();
			a = mater(&a); b = mater(&b);
			printf("\tsdiv\tw%d, w%d, w%d\n", SCRATCHC, a.reg, b.reg);
			printf("\tmsub\tw%d, w%d, w%d, w%d\n",
			    a.reg, SCRATCHC, b.reg, a.reg);
			printf("\tsxtw\tx%d, w%d\n", a.reg, a.reg);
			vfree(&b);
			r = a; r.vtype = type;
			vpush(&r);
		}
		break;

	/* The shifts, which is how an array subscript is scaled -- a(i) arrives
	   as i<<2 for a four-byte element, and P2LSHIFT was the `operator 64' an
	   array of any kind refused on.  Arithmetic right, because a Fortran
	   INTEGER is signed. */
	case P2LSHIFT: case P2RSHIFT:
		{
			struct val a, b, r;

			b = vpop(); a = vpop();
			a = mater(&a); b = mater(&b);
			printf("\t%s\tw%d, w%d, w%d\n",
			    op == P2LSHIFT ? "lsl" : "asr",
			    a.reg, a.reg, b.reg);
			printf("\tsxtw\tx%d, w%d\n", a.reg, a.reg);
			vfree(&b);
			r = a; r.vtype = type;
			vpush(&r);
		}
		break;

	/*
	 * .AND. and .OR., WHICH DO NOT SHORT-CIRCUIT HERE AND MUST NOT.  The
	 * stream is postfix, so both operands have already been evaluated by the
	 * time this record is read -- there is nothing left to skip.  That is not
	 * a limitation: Fortran 77 explicitly does NOT require short-circuit
	 * evaluation of .AND./.OR. (unlike C's && and ||), so evaluating both is
	 * a conforming implementation, and it is the one f77's own intermediate
	 * commits to by emitting them as operators rather than as branches.
	 *
	 * Both operands are 0 or 1 by the time they get here -- a comparison
	 * through cset, a LOGICAL variable by Fortran's own representation -- so
	 * the bitwise instruction is the logical one.
	 */
	case P2ANDAND: case P2OROR:
		{
			struct val a, b, r;

			b = vpop(); a = vpop();
			a = mater(&a); b = mater(&b);
			printf("\t%s\tw%d, w%d, w%d\n",
			    op == P2ANDAND ? "and" : "orr",
			    a.reg, a.reg, b.reg);
			vfree(&b);
			r = a; r.vtype = type;
			vpush(&r);
		}
		break;

	/*
	 * QUEST AND COLON -- AND IN A POSTFIX STREAM THAT IS `csel', NOT A
	 * BRANCH.  Fortran has no conditional operator, so these arrive only
	 * from three places f77 builds them in: intr.c:672 expands ABS as
	 * `0 <= t ? t : -t', putpcc.c:1395-1396 expands MIN and MAX the same
	 * way, and expr.c:1183-1184 is the -C subscript range check.  That is
	 * why `abs' refused with `operator 22 (COLON)' -- the commonest
	 * intrinsic in Fortran is a conditional expression, not a call.
	 *
	 * THE STREAM DECIDES THE IMPLEMENTATION.  Measured on `j = iabs(i)':
	 *
	 *	NAME "v.1" / ICON 0 / LE	the condition
	 *	NAME "v.1"			the then-arm
	 *	NAME "v.1" / NEG		the else-arm
	 *	COLON / QUEST
	 *
	 * so BOTH arms are already evaluated when COLON arrives.  A postfix
	 * stream cannot express short-circuiting and this one does not try to:
	 * f77 assigns to a temporary first (intr.c:670-671 calls mktemp when the
	 * argument is not addressable) precisely so that both arms are safe to
	 * evaluate.  So COLON is physically nothing -- like LISTOP -- and QUEST
	 * selects between two values that are already in hand.
	 *
	 * AT MOST ONE OF THE THREE CAN OWN THE FLAGS, because flushcc() spills
	 * the earlier comparison whenever a second `cmp' is emitted.  The arms
	 * are materialised FIRST: if an arm owns the flags then the condition is
	 * already a value, and if the CONDITION owns them then materialising an
	 * arm emits only loads and moves, which write nothing.  Doing it the
	 * other way round would read the condition's flags into the arm.
	 */
	case P2COLON:
		break;

	case P2QUEST:
		{
			struct val e, t, c, r;
			char *cc;
			int ch;

			e = vpop(); t = vpop(); c = vpop();
			if (t.kind == V_CC)
				t = mater(&t);
			if (e.kind == V_CC)
				e = mater(&e);
			if (c.kind == V_CC)
				cc = ccname((int) c.con);
			else {
				flushcc();
				c = mater(&c);
				printf("\tcmp\tw%d, #0\n", c.reg);
				vfree(&c);
				cc = "ne";
			}
			if (isflt(type)) {
				ch = isdbl(type) ? 'd' : 's';
				t = materas(&t, type);
				e = materas(&e, type);
				printf("\tfcsel\t%c%d, %c%d, %c%d, %s\n",
				    ch, t.reg, ch, t.reg, ch, e.reg, cc);
			} else {
				t = mater(&t); e = mater(&e);
				if (isptr(type))
					printf("\tcsel\tx%d, x%d, x%d, %s\n",
					    t.reg, t.reg, e.reg, cc);
				else {
					/* AND THE RESULT NEEDS RE-EXTENDING.
					   Any write to a w register zeroes bits
					   63:32, and this back end keeps an int
					   sign-extended -- the same fact that
					   put an sxtw after every arithmetic op
					   and that arm64_trunc() exists for. */
					printf("\tcsel\tw%d, w%d, w%d, %s\n",
					    t.reg, t.reg, e.reg, cc);
					printf("\tsxtw\tx%d, w%d\n",
					    t.reg, t.reg);
				}
			}
			vfree(&e);
			r = t; r.vtype = type;
			vpush(&r);
		}
		break;

	case P2BITAND: case P2BITOR: case P2BITXOR:
		{
			struct val a, b, r;

			b = vpop(); a = vpop();
			a = mater(&a); b = mater(&b);
			printf("\t%s\tw%d, w%d, w%d\n",
			    op == P2BITAND ? "and" :
			    op == P2BITOR  ? "orr" : "eor",
			    a.reg, a.reg, b.reg);
			printf("\tsxtw\tx%d, w%d\n", a.reg, a.reg);
			vfree(&b);
			r = a; r.vtype = type;
			vpush(&r);
		}
		break;

	/* The unary operators.  NEG on a float is a sign flip rather than a
	   subtract from zero, which matters for -0.0 and for a NaN. */
	case P2NEG:
		{
			struct val a, r;

			a = vpop();
			a = mater(&a);
			if (isflt(type))
				printf("\tfneg\t%c%d, %c%d\n",
				    isdbl(type) ? 'd' : 's', a.reg,
				    isdbl(type) ? 'd' : 's', a.reg);
			else {
				printf("\tneg\tw%d, w%d\n", a.reg, a.reg);
				printf("\tsxtw\tx%d, w%d\n", a.reg, a.reg);
			}
			r = a; r.vtype = type;
			vpush(&r);
		}
		break;

	case P2BITNOT:
		{
			struct val a, r;

			a = vpop();
			a = mater(&a);
			printf("\tmvn\tw%d, w%d\n", a.reg, a.reg);
			printf("\tsxtw\tx%d, w%d\n", a.reg, a.reg);
			r = a; r.vtype = type;
			vpush(&r);
		}
		break;

	case P2NOT:
		{
			struct val a, r;

			a = vpop();
			a = mater(&a);
			printf("\tcmp\tw%d, #0\n", a.reg);
			printf("\tcset\tw%d, eq\n", a.reg);
			r = a; r.vtype = type;
			vpush(&r);
		}
		break;

	/*
	 * INDIRECT -- the operator that makes an array element addressable, and
	 * the one a subroutine's own scalar parameter needs too, since a
	 * parameter arrives as a pointer and every use of it is a load through
	 * that pointer.
	 *
	 * A NAMED address stays named, so `*(&v.2 - 4)' keeps its adrp/add at
	 * the point of use rather than burning one of five callee-saved
	 * registers per subscript.  Anything computed is materialised once and
	 * becomes offset(reg), which is the same shape an OREG already has --
	 * so the store path in doassign() needs no new case and an array element
	 * is an lvalue for free.
	 */
	case P2INDIRECT:
		{
			struct val a, r;

			a = vpop();
			if (a.kind == V_ADDR) {
				r = a;
				r.kind = V_VAR;
				r.vtype = type;
				vpush(&r);
				break;
			}
			if (a.kind == V_VAR) {
				/* the value of a pointer variable, then a load
				   through it */
				a = mater(&a);
			} else
				a = mater(&a);
			r.kind = V_MEM;
			r.reg = a.reg;
			r.owned = a.owned;
			r.con = 0;
			r.vtype = type;
			r.name[0] = '\0';
			vpush(&r);
		}
		break;

	/*
	 * CONV -- a change of type, and the record's own type is the DESTINATION.
	 * The source is whatever the operand says, which is the only place in
	 * this file where two type words are in play at once.
	 */
	case P2CONV:
		{
			struct val a, r;
			int st, dt, sc, dc;

			a = vpop();
			st = a.vtype;
			dt = type;
			if (isflt(st) && isflt(dt)) {
				sc = isdbl(st) ? 'd' : 's';
				dc = isdbl(dt) ? 'd' : 's';
				a = mater(&a);
				if (sc != dc) {
					r.reg = ralloc(1);
					printf("\tfcvt\t%c%d, %c%d\n",
					    dc, r.reg, sc, a.reg);
					vfree(&a);
					r.owned = 1;
				} else {
					r.reg = a.reg;
					r.owned = a.owned;
				}
				r.kind = V_REG; r.con = 0; r.name[0] = '\0';
				r.vtype = dt;
				vpush(&r);
				break;
			}
			if (!isflt(st) && isflt(dt)) {
				dc = isdbl(dt) ? 'd' : 's';
				a = mater(&a);
				r.reg = ralloc(1);
				printf("\tscvtf\t%c%d, w%d\n", dc, r.reg, a.reg);
				vfree(&a);
				r.kind = V_REG; r.con = 0; r.name[0] = '\0';
				r.vtype = dt; r.owned = 1;
				vpush(&r);
				break;
			}
			if (isflt(st) && !isflt(dt)) {
				sc = isdbl(st) ? 'd' : 's';
				a = mater(&a);
				r.reg = ralloc(0);
				printf("\tfcvtzs\tw%d, %c%d\n", r.reg, sc, a.reg);
				/* AND RE-EXTEND, which is the sixth arm64_trunc
				   site arriving here: fcvtzs writes a w register,
				   any write to a w register zeroes bits 63:32,
				   and this port keeps a signed int sign-extended.
				   The same defect was found in v8cc itself when
				   the Fortran INTEGER width was narrowed. */
				printf("\tsxtw\tx%d, w%d\n", r.reg, r.reg);
				vfree(&a);
				r.kind = V_REG; r.con = 0; r.name[0] = '\0';
				r.vtype = dt; r.owned = 1;
				vpush(&r);
				break;
			}
			/* integer to integer, or to and from a pointer.  A
			   narrowing is the store's business and a widening is
			   already done, because every integer here is held
			   sign-extended; what changes is only the type carried
			   forward. */
			r = a;
			r.vtype = dt;
			vpush(&r);
		}
		break;

	/* COMOP -- evaluate both, the value is the right one.  A lazy operand
	   has no side effect to lose, and an eager one (a CALL, an ASSIGN) has
	   already emitted its code by the time this record is read. */
	case P2COMOP:
		{
			struct val a, b;

			b = vpop(); a = vpop();
			vfree(&a);
			vpush(&b);
		}
		break;

	/* OREG -- offset(reg), a temporary in pass 1's frame, and above
	   ARGOFFSET one of its spilled parameters.  The record is the header,
	   then the offset, then an empty name (p2name("")), and the empty name
	   has to be CONSUMED or every record after it is misread. */
	case P2OREG:
		if (!getword(&w))
			w = 0;
		getname(buf);
		v.kind = V_MEM; v.con = w; v.reg = var; v.vtype = type;
		v.name[0] = '\0'; v.owned = 0;
		vpush(&v);
		break;

	/* REG -- and the number is ALREADY an arm64 one, which is the point of
	   choosing them in arm64defs: AUTOREG is 29 and regnum[] starts at 19,
	   so pass 1 speaks x-register numbers and no mapping is wanted.  A first
	   draft added 8 to make room for a scratch range and emitted
	   `mov x1, x37' for the frame pointer -- the assembler said "unknown
	   AArch64 fixup kind", which names neither the register nor the
	   mapping. */
	case P2REG:
		v.kind = V_REG; v.reg = var;
		v.con = 0; v.vtype = type; v.name[0] = '\0';
		/* NOT OWNED: this is pass 1's register variable, or x29 itself,
		   and handing it to the allocator would let a later value be
		   given the loop counter. */
		v.owned = 0;
		vpush(&v);
		break;

	/*
	 * FORCE -- put the top of the stack where a result is expected.  It is
	 * how prarif() and prcmgoto() stage their operand, and it is also HOW A
	 * FUNCTION RETURNS: proc.c gives every non-subroutine a retslot auto,
	 * and the exit label is followed by that slot and a FORCE.
	 *
	 * So the destination follows the type, and the two halves of v8cc's
	 * convention part company here: a value PASSED goes in an x register and
	 * a value RETURNED comes back in s0 or d0.  Measured on v8cc's own
	 * output rather than assumed symmetric.
	 *
	 * AND THE VALUE IS CONVERTED TO THE FORCE'S TYPE, WHICH IS THE WHOLE OF
	 * K&R's `no float return' RULE.  putpcc.c:551-552 is
	 *
	 *	p2op(P2FORCE, (t==TYSHORT ? P2SHORT : (t==TYLONG ? P2LONG
	 *						: P2DREAL)));
	 *
	 * so a REAL function's FORCE says DOUBLE -- f77 states K&R's rule that
	 * a floating result is always widened.  finto() picks its s-or-d from
	 * the VALUE's type instead of the requested one, so a REAL FUNCTION
	 * returned `ldr s0' where the stream had asked for d0, and every
	 * caller reading d0 got the low mantissa.  The fix is to honour the
	 * type, which is what materas() is for.
	 */
	case P2FORCE:
		{
			struct val a, r;

			a = vpop();
			if (isflt(type)) {
				a = materas(&a, type);
				finto(&a, 0);
				r.reg = 0;
			} else {
				into(&a, 0);
				r.reg = 0;
			}
			vfree(&a);
			r.kind = V_REG; r.con = 0;
			r.vtype = type; r.name[0] = '\0'; r.owned = 0;
			vpush(&r);
		}
		break;

	/* The compound assignments, which a DO loop's induction uses: `s = s + k'
	   comes through as PLUSEQ rather than as PLUS followed by ASSIGN.  The
	   target is loaded, combined and stored back through the same path
	   doassign() uses, so a register, a temporary and a named variable all
	   work without three copies of the store. */
	case P2PLUSEQ: case P2STAREQ:
		{
			struct val rhs, lhs, cur, sum, keep;
			int c;

			rhs = vpop(); lhs = vpop();
			cur = lhs;
			cur.owned = 0;		/* read it without consuming it */
			if (isflt(type)) {
				c = isdbl(type) ? 'd' : 's';
				sum = mater(&cur);
				rhs = mater(&rhs);
				printf("\t%s\t%c%d, %c%d, %c%d\n",
				    op == P2PLUSEQ ? "fadd" : "fmul",
				    c, sum.reg, c, sum.reg, c, rhs.reg);
			} else {
				sum = mater(&cur);
				rhs = mater(&rhs);
				printf("\t%s\tw%d, w%d, w%d\n",
				    op == P2PLUSEQ ? "add" : "mul",
				    sum.reg, sum.reg, rhs.reg);
				printf("\tsxtw\tx%d, w%d\n", sum.reg, sum.reg);
			}
			vfree(&rhs);
			sum.vtype = type;
			/* THE STORE MUST NOT CONSUME THE REGISTER THE VALUE STAYS
			   IN, so doassign() is handed a copy that owns nothing --
			   `k += 1' is compared against the loop bound in the very
			   next record, and giving the register back here would let
			   the comparison's own operand be allocated on top of it. */
			keep = sum;
			keep.owned = 0;
			vpush(&lhs); vpush(&keep);
			keep = doassign();
			vfree(&keep);	/* the store's own value is not wanted */
			/* AND LEAVES ITS VALUE, because a compound assignment is
			   an EXPRESSION here: a DO loop's `k += 1' is compared
			   against the bound in the very next record.  Resetting
			   the stack after it -- which is right for a plain
			   ASSIGN statement -- made the comparison underflow. */
			vpush(&sum);
		}
		break;

	/* The relationals and the conditional branch, which is how a DO loop
	   tests its bound.  pcc emits the comparison and the branch as separate
	   records, with CBRANCH carrying the label -- so the comparison leaves
	   its OPERATOR on the stack for the branch to read rather than
	   materialising a boolean, which is what the hardware wants anyway.
	   A FLOAT COMPARISON IS fcmp AND NOT cmp, and the operand type is what
	   says which -- the record's own type is the type of the RESULT, which
	   is an integer for every comparison there is. */
	case P2EQ: case P2NE: case P2LE: case P2LT: case P2GE: case P2GT:
		{
			struct val a, b, r;

			b = vpop(); a = vpop();
			/* before this cmp writes the flags, give any earlier
			   comparison a register of its own */
			flushcc();
			if (isflt(a.vtype) || isflt(b.vtype)) {
				int c = (isdbl(a.vtype) || isdbl(b.vtype)) ? 'd' : 's';

				a = mater(&a); b = mater(&b);
				printf("\tfcmp\t%c%d, %c%d\n", c, a.reg, c, b.reg);
			} else {
				a = mater(&a); b = mater(&b);
				if (isptr(a.vtype) || isptr(b.vtype))
					printf("\tcmp\tx%d, x%d\n", a.reg, b.reg);
				else
					printf("\tcmp\tw%d, w%d\n", a.reg, b.reg);
			}
			vfree(&a); vfree(&b);
			r.kind = V_CC; r.reg = -1; r.con = op;
			r.vtype = type; r.name[0] = '\0'; r.owned = 0;
			vpush(&r);
		}
		break;

	case P2CBRANCH:
		{
			struct val c, lab;
			char *cc;
			/* THE TARGET IS AN ICON PUSHED BEFORE THE BRANCH, not a
			   field of the record -- measured from the dump, where
			   `ICON 15' precedes CBRANCH.  Reading it out of var
			   left the label on the stack and underflowed the next
			   statement. */
			lab = vpop();
			c = vpop();
			/* A CONDITION IS NOT ALWAYS A COMPARISON.  `.and.' and
			   `.or.' produce a 0/1 VALUE in a register, and so does a
			   LOGICAL variable used on its own -- the flags belong to
			   whatever compared last, which by then is one of the
			   operands.  Branch on the value, still inverted. */
			if (c.kind != V_CC) {
				c = mater(&c);
				printf("\tcbz\tw%d, L%ld\n", c.reg, lab.con);
				vfree(&c);
				vreset();
				break;
			}
			switch ((int) c.con) {
			/* INVERTED, and this is pcc's rule rather than a choice:
			   CBRANCH jumps when the condition is FALSE, so a DO
			   loop's `k > bound' branches back into the body while
			   k is still <= it.  Emitting the condition directly ran
			   the body exactly once and printed sum = 1 -- a
			   plausible wrong answer, not a crash. */
			case P2EQ: cc = "ne"; break;
			case P2NE: cc = "eq"; break;
			case P2LE: cc = "gt"; break;
			case P2LT: cc = "ge"; break;
			case P2GE: cc = "lt"; break;
			case P2GT: cc = "le"; break;
			default:
				fprintf(stderr, "f1: branch on a value that is not a comparison\n");
				exit(2);
			}
			/* var carries the label, and the sense is INVERTED: pcc's
			   CBRANCH jumps when the condition is FALSE, which is how a
			   loop test falls through into its body. */
			printf("\tb.%s\tL%ld\n", cc, lab.con);
			vreset();
		}
		break;

	case P2ASSIGN:
		v = doassign();
		vpush(&v);
		break;

	case P2CALL:
		docall(1);
		break;

	case P2CALL0:
		docall(0);
		break;

	default:
		fprintf(stderr, "f1: operator %d (%s) is not implemented\n",
			op, opname(op));
		exit(2);
	}
}

main(argc, argv)
	char **argv;
{
	char *file = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-' && argv[i][1] == 'd' && argv[i][2] == '\0')
			dumping = 1;
		else
			file = argv[i];
	}
	if (file == 0) {
		fprintf(stderr, "f1: no intermediate file\n");
		exit(1);
	}
	if ((in = fopen(file, "r")) == NULL) {
		fprintf(stderr, "f1: cannot open %s\n", file);
		exit(1);
	}
	while (getrec()) {
		if (op == P2EOF)
			break;
		if (dumping)
			dumprec();
		else
			genrec();
	}
	exit(0);
}
