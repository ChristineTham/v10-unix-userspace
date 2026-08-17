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
		else {
			fprintf(stderr, "f1: code generation is not written yet\n");
			exit(2);
		}
	}
	exit(0);
}
