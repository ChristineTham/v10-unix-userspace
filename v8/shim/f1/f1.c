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
#define P2MINUS		8
#define P2NEG		10
#define P2STAR		11
#define P2INDIRECT	13
#define P2GOTO		37
#define P2LISTOP	56
#define P2ASSIGN	58
#define P2COMOP		59
#define P2SLASH		60
#define P2MOD		62
#define P2CALL		70
#define P2CALL0		72
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

#define P2PTR		020
#define P2FUNCT		040
#define BTSHIFT		4
#define BTMASK		017


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

#define NSTACK	64

#define V_CON	0		/* a literal */
#define V_ADDR	1		/* the address of a named object */
#define V_REG	2		/* already in a register */

struct val {
	int	kind;
	long	con;		/* V_CON: the value.  V_ADDR: an offset */
	char	name[72];	/* V_ADDR */
	int	reg;		/* V_REG */
	int	vtype;
};

static struct val vstack[NSTACK];
static int	vsp;
static int	stmtbase;	/* vsp at the last P2STMT: the expression's floor */
static int	infunc;

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

static void
vpush(v)
	struct val *v;
{
	if (vsp >= NSTACK) {
		fprintf(stderr, "f1: expression stack overflow\n");
		exit(2);
	}
	vstack[vsp++] = *v;
}

static struct val
vpop()
{
	if (vsp <= 0) {
		fprintf(stderr, "f1: expression stack underflow\n");
		exit(2);
	}
	return (vstack[--vsp]);
}

/*
 * Materialise a value into a named register.  V_ADDR becomes the adrp/add pair
 * Mach-O needs for a page-relative address; a V_CON small enough goes in one
 * mov and anything else through a literal pool.
 */
static void
into(v, r)
	struct val *v;
	int r;
{
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
		printf("\tadrp\tx%d, %s@PAGE\n", r, v->name);
		printf("\tadd\tx%d, x%d, %s@PAGEOFF\n", r, r, v->name);
		if (v->con)
			printf("\tadd\tx%d, x%d, #%ld\n", r, r, v->con);
		break;
	case V_REG:
		if (v->reg != r)
			printf("\tmov\tx%d, x%d\n", r, v->reg);
		break;
	}
}

/*
 * The prologue and epilogue.  A FIXED 256-byte frame, stated rather than
 * computed: LBRACKET does carry the auto size, but nothing here spills, and a
 * computed frame is what arm64_endfunction() does for C -- adopting it would
 * mean adopting its three-region layout too, which is the contract this design
 * exists to avoid needing.  256 covers the stack argument area AAPCS64 wants
 * beyond x0-x7 and costs one instruction.
 */
static void
prologue()
{
	printf("\tstp\tx29, x30, [sp, #-16]!\n");
	printf("\tmov\tx29, sp\n");
	printf("\tsub\tsp, sp, #256\n");
}

static void
epilogue()
{
	printf("\tmov\tsp, x29\n");
	printf("\tldp\tx29, x30, [sp], #16\n");
	printf("\tret\n");
}

/*
 * CALL and CALL0.  Everything above stmtbase is this statement's expression, so
 * the callee is vstack[stmtbase] and the arguments are what follows it -- which
 * is exact, and needs no arity from the record.  AAPCS64 puts the first eight
 * in x0-x7; more than eight would need the stack area the prologue reserves,
 * and f77 does not emit one for the calls it makes, so that is a refusal rather
 * than a silent truncation.
 */
static void
docall(hasargs)
	int hasargs;
{
	struct val f, r;
	int n, i;

	if (vsp <= stmtbase) {
		fprintf(stderr, "f1: call with no callee\n");
		exit(2);
	}
	f = vstack[stmtbase];
	n = hasargs ? vsp - stmtbase - 1 : 0;
	if (n > 8) {
		fprintf(stderr, "f1: %d arguments, more than AAPCS64 puts in registers\n", n);
		exit(2);
	}
	for (i = 0; i < n; i++)
		into(&vstack[stmtbase + 1 + i], i);
	if (f.kind != V_ADDR) {
		fprintf(stderr, "f1: indirect call not implemented\n");
		exit(2);
	}
	printf("\tbl\t%s\n", f.name);

	vsp = stmtbase;
	r.kind = V_REG; r.reg = 0; r.con = 0; r.vtype = 0; r.name[0] = '\0';
	vpush(&r);
}

/*
 * ASSIGN.  The left operand is the object being stored into, which arrives as
 * a NAME -- an ADDRESS -- and the right is the value.  The store width comes
 * from the type word: an INTEGER is four bytes here, which is SZLONG in f77's
 * arm64defs, pinned there by typesize[TYREAL] and by lengtype()'s hardcoded
 * INTEGER*4.  Getting this from the type rather than assuming eight is what
 * keeps a Fortran INTEGER four bytes all the way to the store.
 */
static void
doassign()
{
	struct val rhs, lhs;
	int r;

	rhs = vpop();
	lhs = vpop();
	r = 10;
	into(&rhs, r);
	if (lhs.kind != V_ADDR) {
		fprintf(stderr, "f1: assignment to something that is not a name\n");
		exit(2);
	}
	printf("\tadrp\tx11, %s@PAGE\n", lhs.name);
	printf("\tadd\tx11, x11, %s@PAGEOFF\n", lhs.name);
	if ((lhs.vtype & BTMASK) == 3)
		printf("\tstrh\tw%d, [x11]\n", r);
	else if ((lhs.vtype & BTMASK) == 2)
		printf("\tstrb\tw%d, [x11]\n", r);
	else
		printf("\tstr\tw%d, [x11]\n", r);
	vsp = stmtbase;
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
	case P2MINUS:	return ("MINUS");
	case P2STAR:	return ("STAR");
	case P2SLASH:	return ("SLASH");
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
	default:	return ("?");
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
	case P2GOTO:
		printf(" L%d", type);
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
		getname(buf);
		printf(" \"%s\" type %s", buf, typename(type));
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
		vsp = 0;
		stmtbase = 0;
		break;

	case P2LBRACKET:
		if (!getword(&w))
			w = 0;
		break;

	case P2RBRACKET:
		break;

	case P2LABEL:
		printf("L%d:\n", type);
		vsp = 0; stmtbase = 0;
		break;

	case P2GOTO:
		printf("\tb\tL%d\n", type);
		vsp = 0; stmtbase = 0;
		break;

	case P2NAME:
		getname(buf);
		v.kind = V_ADDR; v.con = 0; v.reg = -1; v.vtype = type;
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
		v.con = w; v.reg = -1; v.vtype = type;
		vpush(&v);
		break;

	/* LISTOP is structural: in postfix its two operands are already adjacent
	   on the stack, so joining them is a no-op and the run of arguments is
	   simply everything above the callee. */
	case P2LISTOP:
		break;

	case P2ASSIGN:
		doassign();
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
