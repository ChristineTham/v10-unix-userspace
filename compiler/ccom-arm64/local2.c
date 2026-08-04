/*
 * ARM64 local2.c -- the address printers.
 *
 * In V8's build this file is very nearly vestigial.  Its real customer,
 * common/match.c (the pcc table matcher), is not linked; the only live caller
 * is common/t2print.c:89, which uses adrput() when dumping trees for the -X
 * debug flags.  So these need to be correct enough to read, not to assemble.
 *
 * Kept as a separate file rather than folded into emit.c because the machine
 * -independent code declares these names and expects them to exist; if the
 * table matcher is ever revived, this is where it would look.
 */

# include "mfile2.h"
# include "gencode.h"

extern char *rnames[];

/* Conditional-branch mnemonics, indexed o-EQ.  The order of the relationals in
 * manifest.h is EQ NE LE LT GE GT ULE ULT UGE UGT. */
char *ccbranches[] = {
	"b.eq", "b.ne", "b.le", "b.lt", "b.ge", "b.gt",
	"b.ls", "b.lo", "b.hs", "b.hi",
};

cbgen(o, lab, mode)
	int o, lab, mode;
{
	if (o == 0) {
		printx("\tb\tL%d\n", lab);
		return;
	}
	if (o > UGT) cerror("bad conditional branch %d", o);
	printx("\t%s\tL%d\n", ccbranches[o - EQ], lab);
}

acon(p)				/* print an address constant */
	NODE *p;
{
	if (p->in.name == 0 || *p->in.name == '\0')
		printx("%ld", (long)p->tn.lval);
	else if (p->tn.lval == 0)
		printx("%s", p->in.name);
	else
		printx("%s+%ld", p->in.name, (long)p->tn.lval);
}

conput(p)
	NODE *p;
{
	switch (p->in.op) {
	case ICON:
		acon(p);
		return;
	case REG:
		printx("%s", rnames[p->tn.rval]);
		return;
	}
	cerror("conput: not a constant");
}

upput(p)			/* print an address, dereferenced */
	NODE *p;
{
	switch (p->in.op) {
	case NAME:
	case ICON:
		printx("[");
		acon(p);
		printx("]");
		return;
	case REG:
		printx("[%s]", rnames[p->tn.rval]);
		return;
	case VAUTO:
		printx("[x29, #%ld]", (long)p->tn.lval);
		return;
	case VPARAM:
		printx("[x29, #%ld]", (long)p->tn.lval + 16);
		return;
	}
	printx("[?]");
}

adrput(p)			/* print an operand */
	NODE *p;
{
	if (p == 0) { printx("<null>"); return; }

	switch (p->in.op) {
	case FLD:
	case CONV:
		adrput(p->in.left);
		return;
	case NAME:
		acon(p);
		return;
	case ICON:
		printx("#");
		acon(p);
		return;
	case REG:
		printx("%s", rnames[p->tn.rval]);
		return;
	case STAR:
		upput(p->in.left);
		return;
	case VAUTO:
		printx("[x29, #%ld]", (long)p->tn.lval);
		return;
	case VPARAM:
		printx("[x29, #%ld]", (long)p->tn.lval + 16);
		return;
	}
	printx("<op %d>", p->in.op);
}

staradr(p)
	NODE *p;
{
	upput(p);
}

insput(p)
	NODE *p;
{
	cerror("insput");
}

zzzcode(p, ppc, q)
	NODE *p;
	char **ppc;
{
	/* Template escapes for the table matcher, which is not linked. */
	cerror("zzzcode: the table matcher is not part of this compiler");
}

special()
{
	cerror("reached special");
}
