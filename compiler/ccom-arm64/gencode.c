/*
 * ARM64 code generator for V8 ccom -- replaces vax/gencode.c + vax/genaux.c.
 *
 * Same architecture as the original: a recursive walk over one statement tree,
 * where each node produces its value where the caller asked for it.  V8 threw
 * away pcc's table-driven pass 2 for exactly this, and the whole contract with
 * pass 1 is gencode(p), called once per statement from common/pjw.c.
 *
 * TWO THINGS THE TREE HAS ALREADY BEEN THROUGH, which shape everything here:
 *
 * 1. condit() -- common/reader.c:378 -- has lowered all control flow.  By the
 *    time we run there is no CBRANCH, QUEST, COLON, ANDAND, OROR or NOT left;
 *    they have become CMP / GENBR / GENUBR / GENLAB / COMOP chains, with the
 *    relational opcode parked in bn.lop and the target in bn.label.  That is
 *    why this file has no cases for them.  It also filled stn.argsize on every
 *    call node.
 *
 * 2. p2tree() -- common/trees.c:2166 -- has rewritten in.type from pass 1's
 *    symbolic types (INT, CHAR, PTR|...) into the T* bit codes of mfile2.h
 *    (TINT, TCHAR, TPOINT, ...), and has resolved in.name for every leaf that
 *    needs a symbol.  So the type predicates below switch on T* codes, and
 *    names come from in.name rather than from the symbol table.
 *
 * WHY THIS IS SHORTER THAN ITS VAX ANCESTOR: the VAX was a CISC with memory
 * operands, so most of vax/gencode.c is addressing-mode recognition -- deciding
 * whether a subtree could be folded into an operand string instead of being
 * materialised.  AArch64 is load/store, so that whole dimension disappears:
 * values live in registers, memory is touched only by ldr/str.
 */

# include <stdio.h>
# include <string.h>
# include "mfile2.h"
# include "gencode.h"

extern char *rnames[];
extern int nrnames;
extern char *exname();

int acnt, Pflag, bbcnt;
ret nodest;

static ret gen();
static ret gencall();

/*
 * Type tracing, enabled by setting V8DBG in the environment.
 *
 * Every wrong-width load this back end has produced came from a node carrying
 * an unexpected T* type, and the types are invisible in the emitted assembly --
 * a 4-byte load looks perfectly reasonable until you know the node was a
 * pointer.  Printing them octal makes them directly comparable with mfile2.h,
 * where TINT is 04 and TPOINT is 04000.
 */
#define V8DBG(...) do { if (getenv("V8DBG")) fprintf(stderr, __VA_ARGS__); } while (0)

/* ------------------------------------------------------- register pool */

static char inuse[REGVAR];

/*
 * Floating-point registers are a separate file on AArch64, so they get their
 * own pool rather than competing with x9-x15.  d16-d23 are caller-saved and
 * are not argument registers, which mirrors the choice of x9-x15 for integers.
 */
#define NFREG 8
#define FREG0 16		/* d16 */

static char inusef[NFREG];


static int
regalloc()
{
	int r;

	for (r = 0; r < REGVAR; r++)
		if (!inuse[r]) { inuse[r] = 1; return (r); }
	/*
	 * The VAX generator answered exhaustion by failing back up the
	 * recursion (FAIL/FAILX), rewriting the tree to spill an operand, and
	 * restarting the statement through longjmp.  With seven scratch
	 * registers instead of six -- and no memory operands competing for
	 * them -- running dry needs a pathological expression, so this reports
	 * it rather than silently generating wrong code.
	 */
	cerror("expression too complicated: out of scratch registers");
	return (0);
}

static void
regfree(r)
	int r;
{
	if (r >= 0 && r < REGVAR) inuse[r] = 0;
}

static void
regreset()
{
	int r;
	for (r = 0; r < REGVAR; r++) inuse[r] = 0;
	for (r = 0; r < NFREG; r++) inusef[r] = 0;
}

static char *
xreg(r)
	int r;
{
	if (r < 0 || r >= nrnames) cerror("bad register number %d", r);
	return (rnames[r]);
}

static int
fregalloc()
{
	int r;

	for (r = 0; r < NFREG; r++)
		if (!inusef[r]) { inusef[r] = 1; return (r); }
	cerror("expression too complicated: out of floating registers");
	return (0);
}

static void
fregfree(r)
	int r;
{
	if (r >= 0 && r < NFREG) inusef[r] = 0;
}

/* d-form (double) and s-form (float) names for FP register r. */
static char *
dreg(r)
	int r;
{
	static char buf[6][8];
	static int i;

	i = (i + 1) % 6;
	snprintf(buf[i], sizeof buf[i], "d%d", FREG0 + r);
	return (buf[i]);
}

static char *
sreg(r)
	int r;
{
	static char buf[6][8];
	static int i;

	i = (i + 1) % 6;
	snprintf(buf[i], sizeof buf[i], "s%d", FREG0 + r);
	return (buf[i]);
}

static char *
wreg(r)
	int r;
{
	static char buf[6][8];
	static int i;

	i = (i + 1) % 6;
	buf[i][0] = 'w';
	strcpy(buf[i] + 1, xreg(r) + 1);	/* x9 -> w9 */
	return (buf[i]);
}

/* --------------------------------------------------------- type coding */

/*
 * These take the T* bit codes installed by p2tree, not pass 1's symbolic
 * types.  Getting this wrong is silent: TCHAR is 01 and CHAR is 2, so a switch
 * written against the wrong set still compiles and still matches *something*.
 */

static int
tybytes(t)
	TWORD t;
{
	switch (t) {
	case TCHAR: case TUCHAR:	return 1;
	case TSHORT: case TUSHORT:	return 2;
	case TFLOAT:			return 4;
	case TDOUBLE:			return 8;
	}
	if (t & (TPOINT | TPOINT2))	return SZPOINT / SZCHAR;
	if (t & (TLONG | TULONG))	return SZLONG / SZCHAR;
	if (t & (TINT | TUNSIGNED))	return SZINT / SZCHAR;
	if (t & TSTRUCT)		return SZPOINT / SZCHAR;
	return SZLONG / SZCHAR;
}

static int
tyunsigned(t)
	TWORD t;
{
	if (t & (TPOINT | TPOINT2)) return 1;
	return ((t & (TUCHAR | TUSHORT | TUNSIGNED | TULONG)) != 0);
}

static int
tyfloat(t)
	TWORD t;
{
	return ((t & (TFLOAT | TDOUBLE)) != 0);
}

static char *
ldinsn(t)
	TWORD t;
{
	switch (tybytes(t)) {
	case 1:	return tyunsigned(t) ? "ldrb" : "ldrsb";
	case 2:	return tyunsigned(t) ? "ldrh" : "ldrsh";
	case 4:	return tyunsigned(t) ? "ldr"  : "ldrsw";
	}
	return "ldr";
}

static char *
stinsn(t)
	TWORD t;
{
	switch (tybytes(t)) {
	case 1:	return "strb";
	case 2:	return "strh";
	}
	return "str";
}

/*
 * Destination register spelling for a load.  ldrb/ldrh into a w register
 * zero-extend into the full x register, which is what we want for unsigned;
 * ldrsb/ldrsh/ldrsw name an x register and sign-extend.  A plain 4-byte
 * unsigned load is a w-form ldr, which also zeroes the top half.
 */
static char *
ldreg(t, r)
	TWORD t;
	int r;
{
	if (tybytes(t) == 8) return xreg(r);
	if (tyunsigned(t)) return wreg(r);
	return xreg(r);
}

static char *
streg(t, r)
	TWORD t;
	int r;
{
	return (tybytes(t) == 8) ? xreg(r) : wreg(r);
}

/* FP register spelling: doubles are d-form, floats s-form. */
static char *
freg(t, r)
	TWORD t;
	int r;
{
	return (t & TDOUBLE) ? dreg(r) : sreg(r);
}

/*
 * Re-establish the invariant that a value narrower than a register is held
 * sign- (or zero-) extended to the full 64 bits.
 *
 * The whole back end computes at 64-bit width and relies on that invariant, so
 * every comparison is an x-form `cmp`.  Our own code maintains it -- loads use
 * ldrsw, SCONV re-extends after a narrowing conversion -- but AAPCS64 does NOT.
 * A function returning `int` is only required to set w0; the top half of x0 is
 * explicitly unspecified, and clang leaves whatever was there.
 *
 * So a `register int fi = open(...)` holding -1 arrived as 0x00000000ffffffff,
 * and `fi < 0` -- an x-form compare -- was false.  Every syscall error check in
 * every program was silently broken, because the entire shim is clang-compiled
 * and V8 code checks syscalls with `< 0`.  It never showed in a self-contained
 * test: our own callees return a properly extended x0, so V8-to-V8 calls agree
 * with each other and only the foreign seam disagrees.
 *
 * Both directions of that seam need it, so it lives in one place: after a call
 * returns (below, in gencall) and on narrow parameters at function entry
 * (arm64_extendarg, used by the prologue for signal handlers and the like).
 */
void
arm64_widen(t, r)
	TWORD t;
	int r;
{
	if (ISPTR(t) || ISARY(t) || tyfloat(t))
		return;
	switch (tybytes(t)) {
	case 1:
		printx("\t%s\t%s, %s\n", tyunsigned(t) ? "uxtb" : "sxtb",
		    xreg(r), wreg(r));
		break;
	case 2:
		printx("\t%s\t%s, %s\n", tyunsigned(t) ? "uxth" : "sxth",
		    xreg(r), wreg(r));
		break;
	case 4:
		if (tyunsigned(t))
			printx("\tmov\t%s, %s\n", wreg(r), wreg(r));
		else
			printx("\tsxtw\t%s, %s\n", xreg(r), wreg(r));
		break;
	}
}

/* ------------------------------------------------------------ addresses */

static void
genconst(v, r)
	long v;
	int r;
{
	unsigned long u = (unsigned long)v;

	if (v >= 0 && v <= 65535) {
		printx("\tmov\t%s, #%ld\n", xreg(r), v);
		return;
	}
	if (v < 0 && v >= -65536) {
		printx("\tmov\t%s, #%ld\n", xreg(r), v);
		return;
	}
	printx("\tmovz\t%s, #%lu\n", xreg(r), u & 0xffff);
	if ((u >> 16) & 0xffff)
		printx("\tmovk\t%s, #%lu, lsl #16\n", xreg(r), (u >> 16) & 0xffff);
	if ((u >> 32) & 0xffff)
		printx("\tmovk\t%s, #%lu, lsl #32\n", xreg(r), (u >> 32) & 0xffff);
	if ((u >> 48) & 0xffff)
		printx("\tmovk\t%s, #%lu, lsl #48\n", xreg(r), (u >> 48) & 0xffff);
}

/*
 * r += off, for an offset of any size.
 *
 * ARM64's add and sub take a 12-bit immediate, optionally shifted left by 12 --
 * so any offset below 2^24 fits in at most two instructions, and anything
 * larger has to be materialised in a register first.  Emitting the offset raw
 * works until a program has a large static table, and then fails in the
 * ASSEMBLER rather than the compiler:
 *
 *	add	x10, x10, #99872
 *		           ^ expected ... integer in range [0, 4095]
 *
 * Thirteen commands died that way -- cc, ld, find, du, stty among them -- all
 * of them addressing something well past the first 4KB of a file-scope object.
 */
static void
addconst(r, off)
	int r;
	long off;
{
	char *op = "add";
	unsigned long u;

	if (off == 0)
		return;
	if (off < 0) { op = "sub"; u = (unsigned long)(-off); }
	else u = (unsigned long)off;

	if (u < 4096) {
		printx("\t%s\t%s, %s, #%lu\n", op, xreg(r), xreg(r), u);
		return;
	}
	if (u < (1UL << 24)) {
		if (u & 0xfff)
			printx("\t%s\t%s, %s, #%lu\n", op, xreg(r), xreg(r),
			    u & 0xfff);
		printx("\t%s\t%s, %s, #%lu, lsl #12\n", op, xreg(r), xreg(r),
		    u >> 12);
		return;
	}
	{
		int t = regalloc();

		genconst(off, t);		/* signed: add is correct either way */
		printx("\tadd\t%s, %s, %s\n", xreg(r), xreg(r), xreg(t));
		regfree(t);
	}
}

/*
 * Address of a leaf into register r.
 *
 * Globals go through adrp/add: the only PC-relative form that reaches the whole
 * address space, and required anyway since macOS links everything PIE.
 * @PAGE/@PAGEOFF is the Mach-O spelling; ELF uses :lo12:.
 */
static void
genaddr(p, r)
	NODE *p;
	int r;
{
	char *nm;
	long off;

	switch (p->in.op) {
	case NAME:
	case ICON:
		nm = p->in.name;
		if (nm == 0 || *nm == '\0') {	/* a bare constant address */
			genconst((long)p->tn.lval, r);
			return;
		}
#ifdef ELF_TARGET
		printx("\tadrp\t%s, %s\n", xreg(r), nm);
		printx("\tadd\t%s, %s, :lo12:%s\n", xreg(r), xreg(r), nm);
#else
		printx("\tadrp\t%s, %s@PAGE\n", xreg(r), nm);
		printx("\tadd\t%s, %s, %s@PAGEOFF\n", xreg(r), xreg(r), nm);
#endif
		addconst(r, (long)p->tn.lval);
		return;

	case VAUTO:
		/* lval is already negative: oalloc stores off = -noff */
		printx("\tmov\t%s, x29\n", xreg(r));
		addconst(r, (long)p->tn.lval);
		return;

	case VPARAM:
		/* arguments start 16 bytes above x29, past the saved x29/x30 */
		printx("\tmov\t%s, x29\n", xreg(r));
		addconst(r, (long)(16 + p->tn.lval));
		return;

	case REG:
		printx("\tmov\t%s, %s\n", xreg(r), xreg(p->tn.rval));
		return;
	}
	cerror("genaddr: cannot take the address of op %d", p->in.op);
}

/* Frame-relative operands can be loaded without computing an address first. */
static int
isdirect(p)
	NODE *p;
{
	return (p->in.op == VAUTO || p->in.op == VPARAM);
}

static long
directoff(p)
	NODE *p;
{
	return (p->in.op == VPARAM) ? (long)p->tn.lval + 16 : (long)p->tn.lval;
}

/*
 * Can a frame offset be encoded directly in a load/store?
 *
 * AArch64 offers an unsigned scaled immediate (0 .. 4095*size, a multiple of
 * size) and a signed unscaled one, ldur/stur, limited to -256 .. 255.
 * Automatics live at NEGATIVE offsets from x29 under BACKAUTO, so any function
 * with more than 256 bytes of locals runs off the end of the unscaled form --
 * which the assembler reports as
 *
 *	error: index must be an integer in range [-256, 255].
 *
 * When that happens the address is materialised into a register instead.  This
 * is not rare: one 50-element int array is enough.
 */
static int
offfits(off, size)
	long off;
	int size;
{
	if (off >= -256 && off <= 255) return 1;
	return (off >= 0 && off <= 4095L * size && (off % size) == 0);
}

/* Put x29+off into a scratch register and return it. */
static int
framereg(off)
	long off;
{
	int t = regalloc();

	printx("\tmov\t%s, x29\n", xreg(t));
	addconst(t, off);		/* frames larger than 4KB are common */
	return (t);
}

static void
loaddirect(p, r)
	NODE *p;
	int r;
{
	long off = directoff(p);
	int sz = tybytes(p->in.type);
	int base;

	if (!offfits(off, sz)) {
		base = framereg(off);
		if (tyfloat(p->in.type))
			printx("\tldr\t%s, [%s]\n", freg(p->in.type, r), xreg(base));
		else
			printx("\t%s\t%s, [%s]\n", ldinsn(p->in.type),
			    ldreg(p->in.type, r), xreg(base));
		regfree(base);
		return;
	}
	if (tyfloat(p->in.type)) {
		printx("\tldr\t%s, [x29, #%ld]\n", freg(p->in.type, r), off);
		return;
	}
	printx("\t%s\t%s, [x29, #%ld]\n", ldinsn(p->in.type),
	    ldreg(p->in.type, r), off);
}

static void
storedirect(p, r)
	NODE *p;
	int r;
{
	long off = directoff(p);
	int sz = tybytes(p->in.type);
	int base;

	if (!offfits(off, sz)) {
		base = framereg(off);
		if (tyfloat(p->in.type))
			printx("\tstr\t%s, [%s]\n", freg(p->in.type, r), xreg(base));
		else
			printx("\t%s\t%s, [%s]\n", stinsn(p->in.type),
			    streg(p->in.type, r), xreg(base));
		regfree(base);
		return;
	}
	if (tyfloat(p->in.type)) {
		printx("\tstr\t%s, [x29, #%ld]\n", freg(p->in.type, r), off);
		return;
	}
	printx("\t%s\t%s, [x29, #%ld]\n", stinsn(p->in.type),
	    streg(p->in.type, r), off);
}

/* --------------------------------------------------------- opcode maps */

static char *
ccsuffix(op)
	int op;
{
	switch (op) {
	case EQ:	return "eq";
	case NE:	return "ne";
	case LE:	return "le";
	case LT:	return "lt";
	case GE:	return "ge";
	case GT:	return "gt";
	case ULE:	return "ls";
	case ULT:	return "lo";
	case UGE:	return "hs";
	case UGT:	return "hi";
	}
	cerror("not a relational: %d", op);
	return "al";
}

static char *
arithop(op, t)
	int op;
	TWORD t;
{
	switch (op) {
	case PLUS:	return "add";
	case MINUS:	return "sub";
	case MUL:	return "mul";
	case DIV:	return tyunsigned(t) ? "udiv" : "sdiv";
	case AND:	return "and";
	case OR:	return "orr";
	case ER:	return "eor";
	case LS:	return "lsl";
	case RS:	return tyunsigned(t) ? "lsr" : "asr";
	}
	return 0;
}

/* ---------------------------------------------------------- lvalues */

/* Store register `src` through the lvalue described by `p`. */
static void
storeto(p, src)
	NODE *p;
	int src;
{
	ret a;
	int reg;

	if (isdirect(p)) {
		storedirect(p, src);
		return;
	}
	switch (p->in.op) {
	case NAME:
		reg = regalloc();
		genaddr(p, reg);
		if (tyfloat(p->in.type))
			printx("\tstr\t%s, [%s]\n", freg(p->in.type, src),
			    xreg(reg));
		else
			printx("\t%s\t%s, [%s]\n", stinsn(p->in.type),
			    streg(p->in.type, src), xreg(reg));
		regfree(reg);
		return;

	case STAR:
		a = gen(p->in.left, WVALUE);
		if (tyfloat(p->in.type))
			printx("\tstr\t%s, [%s]\n", freg(p->in.type, src),
			    xreg(a.reg));
		else
			printx("\t%s\t%s, [%s]\n", stinsn(p->in.type),
			    streg(p->in.type, src), xreg(a.reg));
		regfree(a.reg);
		return;

	case REG:
		printx("\tmov\t%s, %s\n", xreg(p->tn.rval), xreg(src));
		return;

	case FLD: {
		/*
		 * Bit-field assignment.  Read the containing word, splice the
		 * new bits in with bfi, write it back.  Geometry is packed into
		 * tn.rval exactly as for the read case: rval/64 is the bit
		 * offset, rval%64 the width.
		 *
		 * The VAX had insv for this in one instruction; bfi is its
		 * direct equivalent.
		 */
		int boff = p->tn.rval / 64;
		int bsiz = p->tn.rval % 64;
		NODE *cont = p->in.left;
		ret c;

		if (bsiz == 0) cerror("zero-width bit field");
		c = gen(cont, WVALUE);
		printx("\tbfi\t%s, %s, #%d, #%d\n", xreg(c.reg), xreg(src),
		    boff, bsiz);
		storeto(cont, c.reg);
		regfree(c.reg);
		return;
	}

	/*
	 * The pseudo-registers condit()/optim() use to funnel values:
	 * RNODE is a function's return value, SNODE the switch subject,
	 * QNODE the rendezvous for the two arms of a lowered ?:.
	 * All three live in x0 by the callreg() convention.
	 */
	case RNODE:
	case SNODE:
	case QNODE:
		if (tyfloat(p->in.type))
			printx("\tfmov\t%s, %s\n",
			    (p->in.type & TDOUBLE) ? "d0" : "s0",
			    freg(p->in.type, src));
		else
			printx("\tmov\tx0, %s\n", xreg(src));
		return;
	}
	cerror("assignment to unsupported lvalue op %d", p->in.op);
}

/* ------------------------------------------------------- the generator */


/*
 * Make sure a value is in an FP register.
 *
 * A floating operator can be handed an integer operand -- `arg *= 10` in
 * ecvt's digit loop is the canonical case -- and pass 1 does not always insert
 * a CONV for it.  Spelling an integer register number as `d<n>` then names a
 * completely unrelated FP register, which is silent and produces plausible
 * wrong digits rather than a crash: fcvt(3.14159, 6) came out 3.146509.
 */
static ret
tofp(r, t)
	ret r;
	TWORD t;
{
	int fr;

	if (r.flag & R_FREG) return (r);
	if (!(r.flag & R_REG)) cerror("floating operand is nowhere");
	fr = fregalloc();
	printx("\tscvtf\t%s, %s\n", freg(t, fr), xreg(r.reg));
	regfree(r.reg);
	r.reg = fr;
	r.flag = R_FREG;
	return (r);
}


/*
 * An lvalue's address, computed ONCE.
 *
 * Read-modify-write operators -- `x op= y`, `x++`, `--x` -- have to read the
 * lvalue and then store back to it.  Generating the lvalue twice, once for each
 * half, re-runs any side effect inside it.  For `++*--p1`, which is exactly
 * what V8's ecvt() rounding loop does, that decrements p1 twice and increments
 * through it twice:
 *
 *	"123" with p1 at [2]   ->   expected "133", p1 at [1]
 *	                            got      "323", p1 at [0]
 *
 * So the address is materialised once here and both halves work through it.
 * `dir` comes back non-zero for a frame-relative operand, which needs no
 * register at all; `reg` is otherwise the register holding the address.
 */
struct lval {
	NODE *node;	/* the lvalue node, for its type and direct offset */
	int reg;	/* register holding the address, or -1 */
	int direct;	/* frame-relative: use the node's own offset */
	int isreg;	/* the lvalue IS a register (REG/RNODE/SNODE/QNODE) */
	int rval;	/* which register, when isreg */
	int fldoff;	/* bit field: offset and width, -1 when not a field */
	int fldsiz;
	struct lval *cont;	/* bit field: the containing word */
};

static void lvaddr();
static ret lvload();
static void lvstore();

static void
lvaddr(p, lv)
	NODE *p;
	struct lval *lv;
{
	ret a;

	lv->node = p;
	lv->reg = -1;
	lv->direct = 0;
	lv->isreg = 0;
	lv->rval = 0;
	lv->fldoff = -1;
	lv->fldsiz = 0;
	lv->cont = 0;

	if (p->in.op == FLD) {
		/*
		 * A bit field is addressed through its containing word, which
		 * is itself an lvalue and must likewise be generated once.
		 * Geometry is packed into tn.rval: rval/64 is the bit offset,
		 * rval%64 the width.
		 */
		static struct lval contbuf[8];
		static int contdepth;

		lv->fldoff = p->tn.rval / 64;
		lv->fldsiz = p->tn.rval % 64;
		if (lv->fldsiz == 0) cerror("zero-width bit field");
		if (contdepth >= 8) cerror("bit fields nested too deeply");
		lv->cont = &contbuf[contdepth++];
		lvaddr(p->in.left, lv->cont);
		contdepth--;
		return;
	}

	if (isdirect(p)) { lv->direct = 1; return; }

	switch (p->in.op) {
	case REG:
		lv->isreg = 1; lv->rval = p->tn.rval; return;
	case RNODE:
	case SNODE:
	case QNODE:
		lv->isreg = 1; lv->rval = -1; return;	/* x0 */
	case NAME:
		lv->reg = regalloc();
		genaddr(p, lv->reg);
		return;
	case STAR:
		a = gen(p->in.left, WVALUE);	/* evaluated exactly once */
		lv->reg = a.reg;
		return;
	}
	cerror("assignment to unsupported lvalue op %d", p->in.op);
}

/* Load the lvalue's current value into a fresh register. */
static ret
lvload(lv)
	struct lval *lv;
{
	ret r;
	TWORD t = lv->node->in.type;
	int reg;

	r.reg = -1; r.flag = R_NONE;
	if (lv->fldoff >= 0) {
		r = lvload(lv->cont);
		printx("\t%s\t%s, %s, #%d, #%d\n",
		    tyunsigned(t) ? "ubfx" : "sbfx",
		    xreg(r.reg), xreg(r.reg), lv->fldoff, lv->fldsiz);
		return (r);
	}
	if (lv->isreg) {
		reg = regalloc();
		printx("\tmov\t%s, %s\n", xreg(reg),
		    lv->rval < 0 ? "x0" : xreg(lv->rval));
		r.reg = reg; r.flag = R_REG;
		return (r);
	}
	if (lv->direct) {
		reg = tyfloat(t) ? fregalloc() : regalloc();
		loaddirect(lv->node, reg);
		r.reg = reg; r.flag = tyfloat(t) ? R_FREG : R_REG;
		return (r);
	}
	if (tyfloat(t)) {
		reg = fregalloc();
		printx("\tldr\t%s, [%s]\n", freg(t, reg), xreg(lv->reg));
		r.reg = reg; r.flag = R_FREG;
		return (r);
	}
	reg = regalloc();
	printx("\t%s\t%s, [%s]\n", ldinsn(t), ldreg(t, reg), xreg(lv->reg));
	r.reg = reg; r.flag = R_REG;
	return (r);
}

/* Store a value back through an already-computed lvalue. */
static void
lvstore(lv, src)
	struct lval *lv;
	int src;
{
	TWORD t = lv->node->in.type;

	if (lv->fldoff >= 0) {
		/* read the containing word, splice the bits in, write it back */
		ret c = lvload(lv->cont);
		printx("\tbfi\t%s, %s, #%d, #%d\n", xreg(c.reg), xreg(src),
		    lv->fldoff, lv->fldsiz);
		lvstore(lv->cont, c.reg);
		regfree(c.reg);
		return;
	}
	if (lv->isreg) {
		if (lv->rval < 0) {
			/* RNODE/SNODE/QNODE: x0, or d0/s0 for a floating value */
			if (tyfloat(t))
				printx("\tfmov\t%s, %s\n",
				    (t & TDOUBLE) ? "d0" : "s0", freg(t, src));
			else
				printx("\tmov\tx0, %s\n", xreg(src));
		} else {
			printx("\tmov\t%s, %s\n", xreg(lv->rval), xreg(src));
		}
		return;
	}
	if (lv->direct) { storedirect(lv->node, src); return; }
	if (tyfloat(t))
		printx("\tstr\t%s, [%s]\n", freg(t, src), xreg(lv->reg));
	else
		printx("\t%s\t%s, [%s]\n", stinsn(t), streg(t, src),
		    xreg(lv->reg));
}

static void
lvfree(lv)
	struct lval *lv;
{
	if (lv->cont) lvfree(lv->cont);
	if (lv->reg >= 0) regfree(lv->reg);
}

static ret
gen(p, want)
	NODE *p;
	int want;
{
	ret l, r, res;
	int reg, sz;
	char *op;
	long v;

	res.reg = -1;
	res.flag = R_NONE;
	if (p == 0) return (res);

	switch (p->in.op) {

	/* -------------------------------------------------------- leaves */
	case ICON:
		if (want == WEFFECT) return (res);
		reg = regalloc();
		if (p->in.name && *p->in.name)
			genaddr(p, reg);	/* address constant */
		else
			genconst((long)p->tn.lval, reg);
		res.reg = reg; res.flag = R_REG;
		return (res);

	case REG:
		/*
		 * A REGISTER VARIABLE, not a scratch register.  It must be
		 * copied before it is handed out, because almost every operator
		 * here reuses its operand's register as the destination -- fine
		 * for a scratch register, fatal for a variable.
		 *
		 * The symptom, in fputs():
		 *	ldrsw x27, [x27]   ; load iop->_cnt into x27 -- which IS iop
		 *	str   w27, [x27]   ; then store through the count as an address
		 *
		 * The 62 back-end tests never caught this because they use
		 * `register` nowhere; V8's libc uses it on almost every
		 * parameter, so the very first real function died on it.
		 */
		if (want == WEFFECT) return (res);
		reg = tyfloat(p->in.type) ? fregalloc() : regalloc();
		if (tyfloat(p->in.type)) {
			printx("\tfmov\t%s, %s\n", freg(p->in.type, reg),
			    freg(p->in.type, p->tn.rval));
			res.reg = reg; res.flag = R_FREG;
		} else {
			printx("\tmov\t%s, %s\n", xreg(reg), xreg(p->tn.rval));
			res.reg = reg; res.flag = R_REG;
		}
		return (res);

	case RNODE:
	case SNODE:
	case QNODE:
		if (want == WEFFECT) return (res);
		reg = regalloc();
		printx("\tmov\t%s, x0\n", xreg(reg));
		res.reg = reg; res.flag = R_REG;
		return (res);

	case NAME:
		if (want == WEFFECT) return (res);
		reg = regalloc();
		genaddr(p, reg);
		if (tyfloat(p->in.type)) {
			/* float constants arrive here: prtdcon() has already
			 * turned every FCON into a .data label plus a NAME */
			int fr = fregalloc();
			printx("\tldr\t%s, [%s]\n", freg(p->in.type, fr), xreg(reg));
			regfree(reg);
			res.reg = fr; res.flag = R_FREG;
			return (res);
		}
		printx("\t%s\t%s, [%s]\n", ldinsn(p->in.type),
		    ldreg(p->in.type, reg), xreg(reg));
		res.reg = reg; res.flag = R_REG;
		return (res);

	case VAUTO:
	case VPARAM:
		if (want == WEFFECT) return (res);
		if (tyfloat(p->in.type)) {
			reg = fregalloc();
			loaddirect(p, reg);
			res.reg = reg; res.flag = R_FREG;
			return (res);
		}
		reg = regalloc();
		loaddirect(p, reg);
		res.reg = reg; res.flag = R_REG;
		return (res);

	/* ----------------------------------------------------- unary ops */
	case UNARY AND:			/* address-of */
		if (want == WEFFECT) return (gen(p->in.left, WEFFECT));
		reg = regalloc();
		genaddr(p->in.left, reg);
		res.reg = reg; res.flag = R_REG;
		return (res);

	case STAR:			/* UNARY MUL -- indirection */
		V8DBG("STAR type=%o bytes=%d unsigned=%d\n",
		    (unsigned)p->in.type, tybytes(p->in.type),
		    tyunsigned(p->in.type));
		l = gen(p->in.left, WVALUE);
		if (want == WEFFECT) { regfree(l.reg); return (res); }
		if (tyfloat(p->in.type)) {
			int fr = fregalloc();
			printx("\tldr\t%s, [%s]\n", freg(p->in.type, fr), xreg(l.reg));
			regfree(l.reg);
			res.reg = fr; res.flag = R_FREG;
			return (res);
		}
		printx("\t%s\t%s, [%s]\n", ldinsn(p->in.type),
		    ldreg(p->in.type, l.reg), xreg(l.reg));
		return (l);

	case UNARY MINUS:
		l = gen(p->in.left, WVALUE);
		if (l.flag & R_FREG) {
			printx("\tfneg\t%s, %s\n", freg(p->in.type, l.reg),
			    freg(p->in.type, l.reg));
			return (l);
		}
		printx("\tneg\t%s, %s\n", xreg(l.reg), xreg(l.reg));
		return (l);

	case COMPL:
		l = gen(p->in.left, WVALUE);
		printx("\tmvn\t%s, %s\n", xreg(l.reg), xreg(l.reg));
		return (l);

	case CONV: {
		V8DBG("CONV type=%o from=%o\n", (unsigned)p->in.type,
		    (unsigned)p->in.left->in.type);
		TWORD st = p->in.left->in.type;
		TWORD dt = p->in.type;

		l = gen(p->in.left, want);
		if (want == WEFFECT) return (l);

		if (tyfloat(dt) && !tyfloat(st)) {
			/* integer -> floating */
			int fr = fregalloc();
			printx("\t%s\t%s, %s\n",
			    tyunsigned(st) ? "ucvtf" : "scvtf",
			    freg(dt, fr),
			    tybytes(st) == 8 ? xreg(l.reg) : wreg(l.reg));
			regfree(l.reg);
			res.reg = fr; res.flag = R_FREG;
			return (res);
		}
		if (!tyfloat(dt) && tyfloat(st)) {
			/* floating -> integer, truncating toward zero as C says */
			int ir = regalloc();
			printx("\t%s\t%s, %s\n",
			    tyunsigned(dt) ? "fcvtzu" : "fcvtzs",
			    tybytes(dt) == 8 ? xreg(ir) : wreg(ir),
			    freg(st, l.reg));
			fregfree(l.reg);
			res.reg = ir; res.flag = R_REG;
			return (res);
		}
		if (tyfloat(dt) && tyfloat(st)) {
			if (dt != st)
				printx("\tfcvt\t%s, %s\n", freg(dt, l.reg),
				    freg(st, l.reg));
			return (l);
		}

		if (!(l.flag & R_REG)) return (l);
		arm64_widen(p->in.type, l.reg);
		return (l);
	}

	/* ---------------------------------------------------- binary ops */
	case PLUS: case MINUS: case MUL: case DIV:
	case AND: case OR: case ER: case LS: case RS:
		if (tyfloat(p->in.type)) {
			switch (p->in.op) {
			case PLUS:  op = "fadd"; break;
			case MINUS: op = "fsub"; break;
			case MUL:   op = "fmul"; break;
			case DIV:   op = "fdiv"; break;
			default:
				cerror("operator %d is not defined on floating operands",
				    p->in.op);
				op = "fadd";
			}
			l = tofp(gen(p->in.left, WVALUE), p->in.type);
			r = tofp(gen(p->in.right, WVALUE), p->in.type);
			printx("\t%s\t%s, %s, %s\n", op, freg(p->in.type, l.reg),
			    freg(p->in.type, l.reg), freg(p->in.type, r.reg));
			fregfree(r.reg);
			return (l);
		}
		V8DBG("BINOP %d type=%o L=%o R=%o\n", p->in.op,
		    (unsigned)p->in.type, (unsigned)p->in.left->in.type,
		    (unsigned)p->in.right->in.type);
		op = arithop(p->in.op, p->in.type);
		l = gen(p->in.left, WVALUE);
		/* fold a small constant right operand into the immediate form */
		if ((p->in.op == PLUS || p->in.op == MINUS) &&
		    p->in.right->in.op == ICON &&
		    (p->in.right->in.name == 0 || *p->in.right->in.name == 0) &&
		    (v = (long)p->in.right->tn.lval) >= 0 && v < 4096) {
			printx("\t%s\t%s, %s, #%ld\n", op, xreg(l.reg),
			    xreg(l.reg), v);
			return (l);
		}
		r = gen(p->in.right, WVALUE);
		printx("\t%s\t%s, %s, %s\n", op, xreg(l.reg), xreg(l.reg),
		    xreg(r.reg));
		regfree(r.reg);
		return (l);

	case MOD:
		/* AArch64 has no remainder: q = a/b; rem = a - q*b */
		l = gen(p->in.left, WVALUE);
		r = gen(p->in.right, WVALUE);
		reg = regalloc();
		printx("\t%s\t%s, %s, %s\n",
		    tyunsigned(p->in.type) ? "udiv" : "sdiv",
		    xreg(reg), xreg(l.reg), xreg(r.reg));
		printx("\tmsub\t%s, %s, %s, %s\n", xreg(l.reg), xreg(reg),
		    xreg(r.reg), xreg(l.reg));
		regfree(reg);
		regfree(r.reg);
		return (l);

	/* ---------------------------------------------------- assignment */
	case ASSIGN: {
		struct lval lv;

		/*
		 * The lvalue is generated ONCE here too.  A plain assignment
		 * looks like it only needs the address, but the address itself
		 * can carry a side effect -- stdio's putc macro is
		 *
		 *	*(p)->_ptr++ = (x)
		 *
		 * and evaluating that twice advanced _ptr twice per character,
		 * so putchar wrote one good byte and then garbage.
		 */
		lvaddr(p->in.left, &lv);
		r = gen(p->in.right, WVALUE);
		if (tyfloat(p->in.left->in.type)) r = tofp(r, p->in.left->in.type);
		lvstore(&lv, r.reg);
		lvfree(&lv);
		if (want == WEFFECT) {
			if (r.flag & R_FREG) fregfree(r.reg); else regfree(r.reg);
			return (res);
		}
		return (r);
	}

	/*
	 * Assignment operators.  ASG X is X+1 (manifest.h: "# define ASG 1+"),
	 * so the underlying operator is recovered by subtracting ASG 0.
	 */
	case ASG PLUS: case ASG MINUS: case ASG MUL: case ASG DIV:
	case ASG MOD: case ASG AND: case ASG OR: case ASG ER:
	case ASG LS: case ASG RS: {
		struct lval lv;
		int base = p->in.op - (ASG 0);

		/* the lvalue is generated ONCE -- see lvaddr() */
		lvaddr(p->in.left, &lv);
		l = lvload(&lv);

		if (tyfloat(p->in.type)) {
			char *fop;
			switch (base) {
			case PLUS:  fop = "fadd"; break;
			case MINUS: fop = "fsub"; break;
			case MUL:   fop = "fmul"; break;
			case DIV:   fop = "fdiv"; break;
			default:
				cerror("operator %d is not defined on floating operands",
				    base);
				fop = "fadd";
			}
			l = tofp(l, p->in.type);
			r = tofp(gen(p->in.right, WVALUE), p->in.type);
			printx("\t%s\t%s, %s, %s\n", fop, freg(p->in.type, l.reg),
			    freg(p->in.type, l.reg), freg(p->in.type, r.reg));
			fregfree(r.reg);
			lvstore(&lv, l.reg);
			lvfree(&lv);
			if (want == WEFFECT) { fregfree(l.reg); return (res); }
			return (l);
		}

		if (base == MOD) {
			r = gen(p->in.right, WVALUE);
			reg = regalloc();
			printx("\t%s\t%s, %s, %s\n",
			    tyunsigned(p->in.type) ? "udiv" : "sdiv",
			    xreg(reg), xreg(l.reg), xreg(r.reg));
			printx("\tmsub\t%s, %s, %s, %s\n", xreg(l.reg),
			    xreg(reg), xreg(r.reg), xreg(l.reg));
			regfree(reg);
			regfree(r.reg);
		} else {
			op = arithop(base, p->in.type);
			if (op == 0) cerror("bad assignment operator %d", p->in.op);
			r = gen(p->in.right, WVALUE);
			printx("\t%s\t%s, %s, %s\n", op, xreg(l.reg),
			    xreg(l.reg), xreg(r.reg));
			regfree(r.reg);
		}
		lvstore(&lv, l.reg);
		lvfree(&lv);
		if (want == WEFFECT) { regfree(l.reg); return (res); }
		return (l);
	}

	case INCR:
	case DECR: {
		struct lval lv;
		int reg2;

		/* the lvalue is generated ONCE -- see lvaddr() */
		lvaddr(p->in.left, &lv);
		l = lvload(&lv);

		if (want != WEFFECT) {
			/* postfix: hand back the value from before the change */
			reg2 = regalloc();
			printx("\tmov\t%s, %s\n", xreg(reg2), xreg(l.reg));
			r = gen(p->in.right, WVALUE);
			printx("\t%s\t%s, %s, %s\n",
			    p->in.op == INCR ? "add" : "sub",
			    xreg(l.reg), xreg(l.reg), xreg(r.reg));
			regfree(r.reg);
			lvstore(&lv, l.reg);
			lvfree(&lv);
			regfree(l.reg);
			res.reg = reg2; res.flag = R_REG;
			return (res);
		}
		r = gen(p->in.right, WVALUE);
		printx("\t%s\t%s, %s, %s\n", p->in.op == INCR ? "add" : "sub",
		    xreg(l.reg), xreg(l.reg), xreg(r.reg));
		regfree(r.reg);
		lvstore(&lv, l.reg);
		lvfree(&lv);
		regfree(l.reg);
		return (res);
	}

	/* --------------------------------------------------- comparisons */
	/*
	 * condit() turns every relational into a CMP feeding a GENBR, so CMP
	 * produces flags and nothing else.  A relational used for its *value*
	 * has already been rewritten into a branch chain by the time we run.
	 */
	case CMP:
		if (tyfloat(p->in.left->in.type)) {
			l = tofp(gen(p->in.left, WVALUE), p->in.left->in.type);
			r = tofp(gen(p->in.right, WVALUE), p->in.left->in.type);
			printx("\tfcmp\t%s, %s\n",
			    freg(p->in.left->in.type, l.reg),
			    freg(p->in.left->in.type, r.reg));
			fregfree(l.reg);
			fregfree(r.reg);
			res.flag = R_CC;
			return (res);
		}
		l = gen(p->in.left, WVALUE);
		if (p->in.right->in.op == ICON &&
		    (p->in.right->in.name == 0 || *p->in.right->in.name == 0) &&
		    (v = (long)p->in.right->tn.lval) >= 0 && v < 4096) {
			printx("\tcmp\t%s, #%ld\n", xreg(l.reg), v);
		} else {
			r = gen(p->in.right, WVALUE);
			printx("\tcmp\t%s, %s\n", xreg(l.reg), xreg(r.reg));
			regfree(r.reg);
		}
		regfree(l.reg);
		res.flag = R_CC;
		return (res);

	case EQ: case NE: case LE: case LT: case GE: case GT:
	case ULE: case ULT: case UGE: case UGT:
		/* A bare relational reaching here wants a 0/1 value. */
		l = gen(p->in.left, WVALUE);
		r = gen(p->in.right, WVALUE);
		printx("\tcmp\t%s, %s\n", xreg(l.reg), xreg(r.reg));
		regfree(r.reg);
		printx("\tcset\t%s, %s\n", xreg(l.reg), ccsuffix(p->in.op));
		return (l);

	/* -------------------------------------------------- control flow */
	case GENBR:
		/* left produces the flags; bn.lop is the relational; bn.label
		 * the destination */
		l = gen(p->in.left, WCC);
		if (l.flag & R_CC) {
			printx("\tb.%s\tL%d\n", ccsuffix(p->bn.lop),
			    p->bn.label);
		} else if (l.flag & R_REG) {
			/*
			 * A truth test rather than a relational -- `if (x & 1)`
			 * and friends.  The polarity still comes from bn.lop,
			 * which condit() set to the sense it wants: branching
			 * unconditionally on non-zero here inverts every such
			 * test, which is subtle enough that the symptom was a
			 * loop summing the odd numbers instead of the even.
			 */
			printx("\tcmp\t%s, #0\n", xreg(l.reg));
			regfree(l.reg);
			printx("\tb.%s\tL%d\n", ccsuffix(p->bn.lop),
			    p->bn.label);
		} else {
			cerror("GENBR with no condition");
		}
		return (res);

	case GENUBR:
		if (p->in.left) gen(p->in.left, WEFFECT);
		genubr(p->bn.label);
		return (res);

	case GENLAB:
		/*
		 * The join of a lowered conditional.
		 *
		 * condit() turns `a ? b : c` into branches whose arms each
		 * deliver their value through the QNODE pseudo-register -- x0,
		 * by the callreg() convention.  So after the label the value is
		 * in x0, and ONLY in x0.
		 *
		 * Returning whichever register the left arm happened to use is
		 * wrong, because the other arm never wrote it.  In getc that
		 * produced
		 *
		 *	bl  __filbuf
		 *	mov x9, x0        ; this arm's value
		 *	mov x0, x9        ; ...into the rendezvous register
		 *	L26:              ; join
		 *	str w9, [x29,#-4] ; c = x9  -- but the other arm set only x0
		 *
		 * so a character read through the fast path came back as the
		 * buffer pointer.
		 */
		if (p->in.left) res = gen(p->in.left, want);
		deflab(p->bn.label);
		if (want != WEFFECT) {
			reg = regalloc();
			printx("\tmov\t%s, x0\n", xreg(reg));
			if (res.flag & R_REG) regfree(res.reg);
			res.reg = reg;
			res.flag = R_REG;
		}
		return (res);

	case GOTO:
		if (p->in.left->in.op == ICON)
			genubr((int)p->in.left->tn.lval);
		else {
			l = gen(p->in.left, WVALUE);
			printx("\tbr\t%s\n", xreg(l.reg));
			regfree(l.reg);
		}
		return (res);

	/* --------------------------------------------------------- calls */
	case CALL:
	case UNARY CALL:
	case STCALL:
	case UNARY STCALL:
		return (gencall(p, want));

	case FUNARG:
		cerror("FUNARG outside a call");
		return (res);

	/* ------------------------------------------------------ sequence */
	case COMOP:
		gen(p->in.left, WEFFECT);
		return (gen(p->in.right, want));

	/* ------------------------------------------------------ structures */
	case STASG: {
		/*
		 * Structure assignment.  stn.stsize is the size IN BITS, set by
		 * p2tree from tsize().  The VAX did this with movc3, a single
		 * block-move instruction; AArch64 has no such thing, so small
		 * structures are copied with a few ldp/str pairs and larger ones
		 * with a counted loop.
		 *
		 * The tree shape is STASG(dest, src) where both subtrees yield
		 * ADDRESSES, not values.
		 */
		int nbytes = p->stn.stsize / SZCHAR;
		int dst, src, tmp, off;

		l = gen(p->in.left, WVALUE);	/* destination address */
		r = gen(p->in.right, WVALUE);	/* source address */
		dst = l.reg; src = r.reg;

		if (nbytes <= 256) {
			tmp = regalloc();
			for (off = 0; off + 8 <= nbytes; off += 8) {
				printx("\tldr\t%s, [%s, #%d]\n", xreg(tmp), xreg(src), off);
				printx("\tstr\t%s, [%s, #%d]\n", xreg(tmp), xreg(dst), off);
			}
			for (; off + 4 <= nbytes; off += 4) {
				printx("\tldr\t%s, [%s, #%d]\n", wreg(tmp), xreg(src), off);
				printx("\tstr\t%s, [%s, #%d]\n", wreg(tmp), xreg(dst), off);
			}
			for (; off < nbytes; off++) {
				printx("\tldrb\t%s, [%s, #%d]\n", wreg(tmp), xreg(src), off);
				printx("\tstrb\t%s, [%s, #%d]\n", wreg(tmp), xreg(dst), off);
			}
			regfree(tmp);
		} else {
			int cnt = regalloc(), tmp2 = regalloc(), lab = getlab();

			genconst((long)nbytes, cnt);
			deflab(lab);
			printx("\tldrb\t%s, [%s], #1\n", wreg(tmp2), xreg(src));
			printx("\tstrb\t%s, [%s], #1\n", wreg(tmp2), xreg(dst));
			printx("\tsubs\t%s, %s, #1\n", xreg(cnt), xreg(cnt));
			printx("\tb.ne\tL%d\n", lab);
			regfree(cnt); regfree(tmp2);
		}
		regfree(src);
		if (want == WEFFECT) { regfree(dst); return (res); }
		res.reg = dst; res.flag = R_REG;	/* value is the destination */
		return (res);
	}

	/* ------------------------------------------------------- bitfields */
	case FLD: {
		/*
		 * Bit-field extraction.  pass 1 packs the geometry into tn.rval:
		 * rval/64 is the bit offset and rval%64 the width (see
		 * common/pjw.c:186 and the VAX generator's FLD case).  ubfx/sbfx
		 * do in one instruction what the VAX needed extzv/extv for.
		 */
		int boff = p->tn.rval / 64;
		int bsiz = p->tn.rval % 64;

		l = gen(p->in.left, WVALUE);
		if (bsiz == 0) cerror("zero-width bit field");
		printx("\t%s\t%s, %s, #%d, #%d\n",
		    tyunsigned(p->in.type) ? "ubfx" : "sbfx",
		    xreg(l.reg), xreg(l.reg), boff, bsiz);
		return (l);
	}

	case INIT:
		/*
		 * A static initialiser, NOT an expression: this runs with the
		 * location counter in .data, so emitting code here silently
		 * plants instructions in the data segment (`mov x9, #123` where
		 * `.long 123` belonged).
		 *
		 * The VAX emitted `.long <operand>` unconditionally because
		 * every initialiser datum was one 32-bit word.  Under LP64 the
		 * width has to follow the type, or a pointer initialiser loses
		 * its top half.
		 */
		{
			NODE *q = p->in.left;
			char *dir;

			switch (tybytes(p->in.type)) {
			case 1:  dir = ".byte";  break;
			case 2:  dir = ".short"; break;
			case 4:  dir = ".long";  break;
			default: dir = ".quad";  break;
			}
			if (q->in.name && *q->in.name) {
				/* address of a symbol, optionally offset */
				if (q->tn.lval)
					printx("\t%s\t%s+%ld\n", dir, q->in.name,
					    (long)q->tn.lval);
				else
					printx("\t%s\t%s\n", dir, q->in.name);
			} else {
				printx("\t%s\t%ld\n", dir, (long)q->tn.lval);
			}
		}
		return (res);

	case FREE:
		return (res);
	}

	cerror("gencode: unimplemented operator %d (%s)", p->in.op,
	    (p->in.op >= 0 && p->in.op < DSIZE && opst[p->in.op]) ?
		opst[p->in.op] : "?");
	return (res);
}

/* --------------------------------------------------------------- calls */

/*
 * Arguments arrive as a CM-linked list of FUNARG nodes.  The VAX pushed them
 * right to left (no LTORARGS) because that put the leftmost at the lowest
 * address; AAPCS64 wants them in x0..x7 instead, so we walk left to right and
 * assign register numbers in order.
 *
 * Each argument is evaluated into a scratch register first and only moved into
 * its x-register at the end, so that evaluating argument n+1 cannot clobber the
 * already-placed argument n.
 */
#define MAXARGS 32

/* See the note above gencall(): sp never moves inside a function body, so both
 * the outgoing-argument area and the caller-save blocks are fixed offsets. */
#define OUTAREA	256		/* up to 32 stack arguments */
#define SAVEBLK	128		/* 7 scratch + 8 FP registers, rounded */

/* How many arguments does this CM-linked list hold? */
static int
countargs(p)
	NODE *p;
{
	if (p == 0) return 0;
	if (p->in.op == CM) return countargs(p->in.left) + countargs(p->in.right);
	return 1;
}

/*
 * Evaluate each argument and store it straight into its outgoing slot.
 *
 * The obvious implementation -- evaluate every argument into a scratch register
 * and move them all at the end -- runs out of registers at the eighth argument,
 * and V8 has functions with a dozen.  Storing each one as it is produced keeps
 * exactly one live at a time, and incidentally makes nested calls in argument
 * expressions safe: a call inside argument three cannot clobber argument two,
 * because argument two is already in memory.
 *
 * Slot layout, relative to the sp the callee will see:
 *	[sp, (i-8)*8]			arguments 8 and up -- the real
 *					outgoing area, contiguous with the
 *					callee's spill block
 *	[sp, stackbytes + i*8]		arguments 0..7 -- scratch, loaded into
 *					x0-x7 just before the call
 */
/*
 * Where argument n of a call at nesting depth d lives, as a fixed offset from
 * sp.  Arguments 8 and up must sit at sp+0 upward, because that is where the
 * callee reads them; 0..7 get scratch slots above the outgoing area and are
 * loaded into x0-x7 immediately before the branch.
 */
static int
argslot(n, depth)
	int n, depth;
{
	if (n >= 8) return ((n - 8) * 8);
	return (OUTAREA + (depth + 1) * SAVEBLK - 64 + n * 8);
}

static int
placeargs(p, n, depth)
	NODE *p;
	int n, depth;
{
	ret a;
	int slot;

	if (p == 0) return n;
	if (p->in.op == CM) {
		n = placeargs(p->in.left, n, depth);
		return placeargs(p->in.right, n, depth);
	}
	if (p->in.op == FUNARG) p = p->in.left;

	a = gen(p, WVALUE);
	slot = argslot(n, depth);

	if (a.flag & R_FREG) {
		/* a float widens to double first, as C and V8 both promote */
		if (!(p->in.type & TDOUBLE))
			printx("\tfcvt\t%s, %s\n", dreg(a.reg), sreg(a.reg));
		printx("\tstr\t%s, [sp, #%d]\n", dreg(a.reg), slot);
		fregfree(a.reg);
	} else {
		printx("\tstr\t%s, [sp, #%d]\n", xreg(a.reg), slot);
		regfree(a.reg);
	}
	return (n + 1);
}

/*
 * OUTGOING-ARGUMENT AND SAVE AREAS, RESERVED ONCE IN THE PROLOGUE.
 *
 * The obvious implementation brackets each call with `sub sp,sp,#N` and a
 * matching `add`.  That is correct only for straight-line control flow, and
 * condit() lowers `a ? b : c` into branches -- so a call in one arm puts the
 * sub and the add on different paths and the stack drifts.  Seen directly in
 * `while ((c = getchar()) != EOF)`:
 *
 *	sub sp, sp, #16
 *	b   L21           <- branches away, sp never restored
 *
 * getc and putc are exactly this shape, which is why stdio was garbling while
 * every straight-line test passed.
 *
 * So sp does not move inside a function body at all.  Two regions are reserved
 * in the prologue, which emit.c writes after the body is captured and can
 * therefore size correctly:
 *
 *	[sp, 0 .. OUTAREA)		outgoing stack arguments.  Must be at
 *					sp+0: the callee reads them at [x29,#80]
 *					after its own prologue.
 *	[sp, OUTAREA + d*SAVEBLK ...)	caller-saved registers, one block per
 *					call-nesting depth so f(g(x)) does not
 *					have g clobber f's saves.
 */
int arm64_maxdepth;		/* deepest call nesting seen; read by emit.c */
static int calldepth;

void
arm64_resetcalls()
{
	arm64_maxdepth = -1;
	calldepth = 0;
}

long
arm64_callarea()
{
	if (arm64_maxdepth < 0) return (0);	/* a leaf: nothing needed */
	return (OUTAREA + (long)(arm64_maxdepth + 1) * SAVEBLK);
}

static ret
gencall(p, want)
	NODE *p;
	int want;
{
	ret res, f;
	int saved[REGVAR];
	int fsaved[NFREG];
	int n, i, reg, nsave, nfsave, depth, sbase;
	int isunary = (p->in.op == (UNARY CALL) || p->in.op == (UNARY STCALL));

	res.reg = -1; res.flag = R_NONE;

	depth = calldepth++;
	if (depth > arm64_maxdepth) arm64_maxdepth = depth;
	sbase = OUTAREA + depth * SAVEBLK;

	n = isunary ? 0 : countargs(p->in.right);
	if (n > MAXARGS) cerror("more than %d arguments in one call", MAXARGS);
	if (n > 8 && (n - 8) * 8 > OUTAREA)
		cerror("more than %d stack arguments in one call", OUTAREA / 8);

	/*
	 * Arguments are evaluated and stored one at a time, so only one is live
	 * at a time and a nested call inside an argument cannot clobber an
	 * argument already placed.  Argument evaluation may branch; that is
	 * safe now precisely because no sp adjustment is outstanding.
	 */
	if (!isunary)
		placeargs(p->in.right, 0, depth);

	/*
	 * Save the scratch registers still holding live values.  x9-x15 and
	 * d16-d23 are caller-saved, so anything wanted after the call must
	 * survive it -- without this, recursion returns garbage.
	 */
	nsave = 0;
	for (i = 0; i < REGVAR; i++)
		if (inuse[i]) saved[nsave++] = i;
	for (i = 0; i < nsave; i++)
		printx("\tstr\t%s, [sp, #%d]\n", xreg(saved[i]), sbase + i * 8);

	nfsave = 0;
	for (i = 0; i < NFREG; i++)
		if (inusef[i]) fsaved[nfsave++] = i;
	for (i = 0; i < nfsave; i++)
		printx("\tstr\t%s, [sp, #%d]\n", dreg(fsaved[i]),
		    sbase + (REGVAR + i) * 8);

	/* the first eight arguments move from their slots into x0-x7 */
	for (i = 0; i < n && i < 8; i++)
		printx("\tldr\tx%d, [sp, #%d]\n", i, argslot(i, depth));

	/*
	 * ICON, not NAME.
	 *
	 * Both arrive here carrying a name and pointer type, and the difference
	 * is the whole of it: an ICON with a name IS the address of the callee,
	 * so `bl name` is right; a NAME is a reference to STORAGE that holds an
	 * address, so its value must be loaded first.
	 *
	 *	add(2,3)	CALLEE op=4 name=[_add] type=4000   ICON
	 *	(*fp)(2,3)	CALLEE op=2 name=[_fp]  type=4000   NAME
	 *
	 * Accepting NAME here emitted `bl _fp` -- a branch into the data cell
	 * holding the pointer -- and every indirect call died with SIGBUS.
	 * qsort was the first thing to need one; sh, awk and troff are full of
	 * them.  Set V8DBG to see the trace above.
	 */
	V8DBG("CALLEE op=%d name=[%s] calltype=%o (%d bytes%s)\n",
	    p->in.left->in.op,
	    p->in.left->in.name ? p->in.left->in.name : "",
	    p->in.type, tybytes(p->in.type),
	    tyunsigned(p->in.type) ? ", unsigned" : "");
	if (p->in.left->in.op == ICON &&
	    p->in.left->in.name && *p->in.left->in.name) {
		printx("\tbl\t%s\n", p->in.left->in.name);
	} else {
		f = gen(p->in.left, WVALUE);
		printx("\tblr\t%s\n", xreg(f.reg));
		regfree(f.reg);
	}

	for (i = 0; i < nfsave; i++)
		printx("\tldr\t%s, [sp, #%d]\n", dreg(fsaved[i]),
		    sbase + (REGVAR + i) * 8);
	for (i = 0; i < nsave; i++)
		printx("\tldr\t%s, [sp, #%d]\n", xreg(saved[i]), sbase + i * 8);

	calldepth--;

	if (want == WEFFECT) return (res);

	if (tyfloat(p->in.type)) {
		reg = fregalloc();
		printx("\tfmov\t%s, %s\n", freg(p->in.type, reg),
		    (p->in.type & TDOUBLE) ? "d0" : "s0");
		res.reg = reg; res.flag = R_FREG;
		return (res);
	}
	reg = regalloc();
	printx("\tmov\t%s, x0\n", xreg(reg));
	/*
	 * Re-extend a narrow return -- but NOT a plain signed int.  See
	 * arm64_widen() for why AAPCS64 makes this necessary at all; this is
	 * about which types it may safely be applied to.
	 *
	 * A CALL node typed char, short or unsigned can only have come from an
	 * explicit declaration: K&R's implicit type for an undeclared function
	 * is signed int and nothing else.  So for those, `int` really is what
	 * the callee returns and narrowing is right.
	 *
	 * Signed int is ambiguous, and in this tree it usually means "nobody
	 * declared it".  V8 code calls malloc undeclared -- opendir.c says
	 *
	 *	dirp = (DIR *)malloc(sizeof(DIR));
	 *
	 * with no `char *malloc();` anywhere in scope -- because on the VAX an
	 * int and a pointer were both 32 bits and the fiction cost nothing.
	 * Under LP64 it costs everything: sign-extending malloc's result from
	 * 32 bits truncated the pointer and opendir segfaulted.
	 *
	 * We cannot tell the two apart here; `int` from a declaration and `int`
	 * from a guess are the same node.  So the seam is fixed on the other
	 * side instead: shim/v8sys/stubs.c returns long, which makes clang set
	 * all 64 bits of x0, and no narrowing is needed for it.  See the note
	 * above the WRAP macros there -- the two halves of this decision have
	 * to stay together.
	 */
	if (tybytes(p->in.type) < 4 || tyunsigned(p->in.type))
		arm64_widen(p->in.type, reg);
	res.reg = reg; res.flag = R_REG;
	return (res);
}

/* --------------------------------------------------------------- entry */

gencode(p)
	NODE *p;
{
	regreset();
	gen(p, WEFFECT);
	regreset();
}
