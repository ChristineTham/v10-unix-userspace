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

/* ------------------------------------------------------- register pool */

static char inuse[REGVAR];

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
}

static char *
xreg(r)
	int r;
{
	if (r < 0 || r >= nrnames) cerror("bad register number %d", r);
	return (rnames[r]);
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
		if (p->tn.lval)
			printx("\tadd\t%s, %s, #%ld\n", xreg(r), xreg(r),
			    (long)p->tn.lval);
		return;

	case VAUTO:
		/* lval is already negative: oalloc stores off = -noff */
		off = (long)p->tn.lval;
		if (off < 0)
			printx("\tsub\t%s, x29, #%ld\n", xreg(r), -off);
		else
			printx("\tadd\t%s, x29, #%ld\n", xreg(r), off);
		return;

	case VPARAM:
		/* arguments start 16 bytes above x29, past the saved x29/x30 */
		printx("\tadd\t%s, x29, #%ld\n", xreg(r),
		    (long)(16 + p->tn.lval));
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

static void
loaddirect(p, r)
	NODE *p;
	int r;
{
	printx("\t%s\t%s, [x29, #%ld]\n", ldinsn(p->in.type),
	    ldreg(p->in.type, r), directoff(p));
}

static void
storedirect(p, r)
	NODE *p;
	int r;
{
	printx("\t%s\t%s, [x29, #%ld]\n", stinsn(p->in.type),
	    streg(p->in.type, r), directoff(p));
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
		printx("\t%s\t%s, [%s]\n", stinsn(p->in.type),
		    streg(p->in.type, src), xreg(reg));
		regfree(reg);
		return;

	case STAR:
		a = gen(p->in.left, WVALUE);
		printx("\t%s\t%s, [%s]\n", stinsn(p->in.type),
		    streg(p->in.type, src), xreg(a.reg));
		regfree(a.reg);
		return;

	case REG:
		printx("\tmov\t%s, %s\n", xreg(p->tn.rval), xreg(src));
		return;

	/*
	 * The pseudo-registers condit()/optim() use to funnel values:
	 * RNODE is a function's return value, SNODE the switch subject,
	 * QNODE the rendezvous for the two arms of a lowered ?:.
	 * All three live in x0 by the callreg() convention.
	 */
	case RNODE:
	case SNODE:
	case QNODE:
		printx("\tmov\tx0, %s\n", xreg(src));
		return;
	}
	cerror("assignment to unsupported lvalue op %d", p->in.op);
}

/* ------------------------------------------------------- the generator */

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
		res.reg = p->tn.rval; res.flag = R_REG;
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
		printx("\t%s\t%s, [%s]\n", ldinsn(p->in.type),
		    ldreg(p->in.type, reg), xreg(reg));
		res.reg = reg; res.flag = R_REG;
		return (res);

	case VAUTO:
	case VPARAM:
		if (want == WEFFECT) return (res);
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
		l = gen(p->in.left, WVALUE);
		if (want == WEFFECT) { regfree(l.reg); return (res); }
		printx("\t%s\t%s, [%s]\n", ldinsn(p->in.type),
		    ldreg(p->in.type, l.reg), xreg(l.reg));
		return (l);

	case UNARY MINUS:
		l = gen(p->in.left, WVALUE);
		printx("\tneg\t%s, %s\n", xreg(l.reg), xreg(l.reg));
		return (l);

	case COMPL:
		l = gen(p->in.left, WVALUE);
		printx("\tmvn\t%s, %s\n", xreg(l.reg), xreg(l.reg));
		return (l);

	case CONV:
		l = gen(p->in.left, want);
		if (want == WEFFECT || !(l.flag & R_REG)) return (l);
		sz = tybytes(p->in.type);
		if (sz == 1)
			printx("\t%s\t%s, %s\n",
			    tyunsigned(p->in.type) ? "uxtb" : "sxtb",
			    xreg(l.reg), wreg(l.reg));
		else if (sz == 2)
			printx("\t%s\t%s, %s\n",
			    tyunsigned(p->in.type) ? "uxth" : "sxth",
			    xreg(l.reg), wreg(l.reg));
		else if (sz == 4) {
			if (tyunsigned(p->in.type))
				printx("\tmov\t%s, %s\n", wreg(l.reg), wreg(l.reg));
			else
				printx("\tsxtw\t%s, %s\n", xreg(l.reg), wreg(l.reg));
		}
		return (l);

	/* ---------------------------------------------------- binary ops */
	case PLUS: case MINUS: case MUL: case DIV:
	case AND: case OR: case ER: case LS: case RS:
		if (tyfloat(p->in.type))
			cerror("floating point arithmetic not yet implemented");
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
	case ASSIGN:
		r = gen(p->in.right, WVALUE);
		storeto(p->in.left, r.reg);
		if (want == WEFFECT) { regfree(r.reg); return (res); }
		return (r);

	/*
	 * Assignment operators.  ASG X is X+1 (manifest.h: "# define ASG 1+"),
	 * so the underlying operator is recovered by subtracting ASG 0.
	 */
	case ASG PLUS: case ASG MINUS: case ASG MUL: case ASG DIV:
	case ASG MOD: case ASG AND: case ASG OR: case ASG ER:
	case ASG LS: case ASG RS: {
		NODE tmp;
		int base = p->in.op - (ASG 0);

		/* value of the left operand */
		l = gen(p->in.left, WVALUE);
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
		storeto(p->in.left, l.reg);
		if (want == WEFFECT) { regfree(l.reg); return (res); }
		return (l);
	}

	case INCR:
	case DECR: {
		int reg2;

		l = gen(p->in.left, WVALUE);
		if (want != WEFFECT) {
			/* postfix: keep the old value */
			reg2 = regalloc();
			printx("\tmov\t%s, %s\n", xreg(reg2), xreg(l.reg));
			r = gen(p->in.right, WVALUE);
			printx("\t%s\t%s, %s, %s\n",
			    p->in.op == INCR ? "add" : "sub",
			    xreg(l.reg), xreg(l.reg), xreg(r.reg));
			regfree(r.reg);
			storeto(p->in.left, l.reg);
			regfree(l.reg);
			res.reg = reg2; res.flag = R_REG;
			return (res);
		}
		r = gen(p->in.right, WVALUE);
		printx("\t%s\t%s, %s, %s\n", p->in.op == INCR ? "add" : "sub",
		    xreg(l.reg), xreg(l.reg), xreg(r.reg));
		regfree(r.reg);
		storeto(p->in.left, l.reg);
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
		if (p->in.left) res = gen(p->in.left, want);
		deflab(p->bn.label);
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
#define MAXARGS 8

static int
collectargs(p, regs, n)
	NODE *p;
	int regs[];
	int n;
{
	ret a;

	if (p == 0) return n;
	if (p->in.op == CM) {
		n = collectargs(p->in.left, regs, n);
		return collectargs(p->in.right, regs, n);
	}
	if (p->in.op == FUNARG) p = p->in.left;
	if (n >= MAXARGS)
		cerror("more than %d arguments not yet implemented", MAXARGS);
	a = gen(p, WVALUE);
	regs[n] = a.reg;
	return (n + 1);
}

static ret
gencall(p, want)
	NODE *p;
	int want;
{
	ret res, f;
	int regs[MAXARGS];
	int saved[REGVAR];
	int n, i, reg, nsave;
	int isunary = (p->in.op == (UNARY CALL) || p->in.op == (UNARY STCALL));

	res.reg = -1; res.flag = R_NONE;

	n = 0;
	if (!isunary)
		n = collectargs(p->in.right, regs, 0);

	for (i = 0; i < n; i++)
		printx("\tmov\tx%d, %s\n", i, xreg(regs[i]));
	for (i = 0; i < n; i++)
		regfree(regs[i]);

	/*
	 * Save any scratch register still holding a live value.
	 *
	 * x9-x15 are caller-saved under AAPCS64, so the callee is free to
	 * destroy them.  Anything computed before the call and still wanted
	 * after it -- the classic case being the left operand of
	 * `fib(n-1) + fib(n-2)` -- has to be preserved here.  Without this,
	 * recursion silently returns garbage: fib(10) came out 0, because every
	 * inner call flattened the partial sum its caller was holding.
	 *
	 * Pairs are pushed so sp keeps its 16-byte alignment.
	 */
	nsave = 0;
	for (i = 0; i < REGVAR; i++)
		if (inuse[i]) saved[nsave++] = i;
	for (i = 0; i + 1 < nsave; i += 2)
		printx("\tstp\t%s, %s, [sp, #-16]!\n", xreg(saved[i]),
		    xreg(saved[i + 1]));
	if (nsave & 1)
		printx("\tstr\t%s, [sp, #-16]!\n", xreg(saved[nsave - 1]));

	if (p->in.left->in.op == ICON && p->in.left->in.name &&
	    *p->in.left->in.name) {
		printx("\tbl\t%s\n", p->in.left->in.name);
	} else if (p->in.left->in.op == NAME && p->in.left->in.name &&
	    *p->in.left->in.name) {
		printx("\tbl\t%s\n", p->in.left->in.name);
	} else {
		f = gen(p->in.left, WVALUE);
		printx("\tblr\t%s\n", xreg(f.reg));
		regfree(f.reg);
	}

	/* restore, in reverse */
	if (nsave & 1)
		printx("\tldr\t%s, [sp], #16\n", xreg(saved[nsave - 1]));
	for (i = (nsave & ~1) - 2; i >= 0; i -= 2)
		printx("\tldp\t%s, %s, [sp], #16\n", xreg(saved[i]),
		    xreg(saved[i + 1]));

	if (want == WEFFECT) return (res);

	reg = regalloc();
	printx("\tmov\t%s, x0\n", xreg(reg));
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
