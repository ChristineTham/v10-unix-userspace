/*
 * ARM64 machine-dependent pass-1 code -- replaces vax/local.c.
 *
 * FRAME LAYOUT
 *
 * The VAX needed no prologue arithmetic: `calls` built the frame from the
 * callee's entry mask, so V8 emitted a placeholder and patched it afterwards
 * with a trailing `.set`.  ARM64 must build its own frame, and it must know the
 * size up front -- the assembler rejects a forward symbol in `sub sp, sp, #N`
 * because it needs the value to encode the instruction.  So emit.c captures the
 * function body in memory and efcode() emits prologue and epilogue around it,
 * by which point the frame size and register usage are known.
 *
 * The layout, high addresses at the top:
 *
 *	...caller's stack arguments (9th onward)...	<- [x29,#80] upward
 *	x7  spill					   [x29,#72]
 *	...						      ...
 *	x0  spill					   [x29,#16]
 *	saved x30 (LR)					   [x29,#8]
 *	saved x29 (FP)					<- x29
 *	callee-saved x19..x28 (only those used)
 *	locals and temporaries				<- negative offsets
 *	...						<- sp
 *
 * The spill block sits *above* the saved frame pointer, which is the whole
 * trick: the prologue allocates it before pushing x29/x30, so the eight spilled
 * argument registers land immediately below the caller's stack arguments and
 * the two run together as one contiguous block.  That is what makes V8's
 * varargs.h -- take the address of the last named parameter and walk forward --
 * work unmodified, and it is why SZARG is 64 (see macdefs.h): pass 1 then lays
 * parameters out on the same 8-byte stride the spills use.
 *
 * Parameters are addressed at [x29, #16+off/8] with off counting up from
 * ARGINIT=0; automatics at [x29, #off/8] with off already negative, since
 * oalloc() under BACKAUTO stores `off = -noff` (pftn.c:1486).
 */

# include <stdio.h>
# include "mfile1.h"

extern int Pflag, bbcnt;

/*
 * exname() returns char *.  Without this declaration K&R rules make it
 * implicitly int-returning, and under LP64 that truncates the pointer to 32
 * bits -- the symbol name then prints as garbage.  The VAX original omits the
 * declaration and gets away with it because there int and char * are both 32
 * bits wide.  Same reasoning for the others.
 */
extern char *exname();
extern char *emitcaptured();

int minrvar = RVARLAST_P1;	/* lowest register-variable number used */
int wloop_level = LL_BOT;
int floop_level = LL_BOT;
int maxboff;
int maxtemp;

/* Kept for the same reason the VAX version keeps it: reader.c/pass 2 expects
 * the symbol, but the pcc2 table machinery it belongs to is not linked. */
codgen(p)
	NODE *p;
{
	extern int bothdebug;
	if (bothdebug)
		tfree(p);
}

main(argc, argv)
	char *argv[];
{
	int r;
	static char errbuf[BUFSIZ];

	setbuf(stderr, errbuf);
	r = mainp1(argc, argv);
	flushx();
	return (r);
}

beg_file()
{
	/* first thing the parser calls, for machine-dependent setup */
	regvar = minrvar = RVARLAST_P1;
}

NODE *
treecpy(p)			/* first pass version of tcopy() */
	register NODE *p;
{
	register NODE *q;

	q = talloc();
	*q = *p;
	switch (optype(q->in.op)) {
	case BITYPE:
		q->in.right = treecpy(p->in.right);
	case UTYPE:
		q->in.left = treecpy(p->in.left);
	}
	return (q);
}

NODE *
clocal(p)
	NODE *p;
{
	register NODE *l, *ll, *r;

	/*
	 * The VAX version reordered  *(a + b*n)  so the scaled term sits on the
	 * left, to expose its index addressing mode.  ARM64 has the same shape
	 * of addressing mode (ldr xd,[xn,xm,lsl #k]), so the rewrite is kept.
	 * It is an optimisation, not a correctness requirement.
	 *
	 * The VAX file's other block, distributing assignments over ?:, sat
	 * under #ifdef DASSOVCOL, which macdefs.h never defined -- dead code,
	 * not reproduced.
	 */
	if (p->in.op == STAR) {
		l = p->in.left;
		if (l->in.op == PLUS) {
			ll = l->in.left;
			if (ll->in.op != MUL && ll->in.op != UNARY AND) {
				if ((l->in.right)->in.op == MUL) {
					r = l->in.right;
					l->in.right = l->in.left;
					l->in.left = r;
				}
			}
		}
	}


	return (p);
}

cisreg(t)			/* may an auto of type t live in a register? */
	TWORD t;
{
	if (t == INT || t == UNSIGNED || ISPTR(t) || t == CHAR || t == UCHAR
	    || t == SHORT || t == USHORT || t == LONG || t == ULONG) {
		if (regvar >= RVARFIRST_P1) {
			nextrvar = regvar--;
			if (regvar < minrvar) minrvar = regvar;
			return (1);
		}
	}
	return (0);
}

opbigsz(op)
{
	/*
	 * The size below which pass 1 may NOT shrink an operation.
	 *
	 * The VAX answered SZCHAR for the bitwise and additive operators,
	 * because it had byte-wide forms of all of them and, crucially, because
	 * a pointer was 32 bits there -- exactly as wide as an int -- so
	 * narrowing an expression to int width never lost one.
	 *
	 * Under LP64 it does.  V8's malloc hides a busy flag in the low bit of
	 * a pointer:
	 *
	 *	#define clearbusy(p) (union store *)((INT)(p)&~BUSY)
	 *
	 * With SZCHAR here, pass 1 decided that AND could be done narrow and
	 * emitted a 4-byte load for the pointer --
	 *
	 *	ldrsw x9, [x9]      instead of      ldr x9, [x9]
	 *
	 * -- silently truncating it.  malloc then walked its free list through
	 * a half pointer and faulted in ialloc.
	 *
	 * Returning SZLONG disables the narrowing entirely.  It costs a little
	 * code size on byte and short arithmetic, where AArch64 would have been
	 * happy with a w-form; correctness first, and the host assembler still
	 * picks the narrow encoding where the operands allow it.
	 */
	return (SZLONG);
}

branch(n)			/* branch to label n, or return */
	int n;
{
	if (!reached)		/* `return <expr>; }` reaches here twice */
		return;
	genubr(n);
}

/* ------------------------------------------------------------- switches */

/*
 * Linear compare-and-branch for every switch.
 *
 * The VAX version chose between a direct jump table (casel), a binary-search
 * heap, and linear tests, driven by DSWTEST/HEAPTEST.  This emits the linear
 * form unconditionally: correct for every input, and simple enough to be
 * obviously correct while the backend is young.  Dense switches in troff and
 * the shell will want the jump table for speed; that is an optimisation to add
 * once the compiler is trustworthy, and it changes no observable behaviour.
 */
genswitch(p, n)
	register struct sw *p;
	int n;
{
	register int i;

	for (i = 1; i <= n; ++i) {
		printx("\t// case %ld\n", (long)p[i].sval);
		genswcase(p[i].sval, p[i].slab);
	}
	if (p[0].slab >= 0)
		genubr(p[0].slab);
	else
		genubr(brklab);
}

/* ------------------------------------------------------- initialisers */

static long word;		/* word being built from bit fields */
static int inwd;		/* current bit offset within that word */

zecode(n)			/* n words of zeros */
	int n;
{
	if (n <= 0) return;
	printx("\t.space\t%d\n", 4 * n);
	inoff += n * SZINT;
}

vfdzero(n)			/* n bits of zeros in a vfd */
{
	sz_incode((CONSZ)0, n);
}

incode(p, sz)
	NODE *p;
{
	sz_incode(p->tn.lval, sz);
}

sz_incode(val, sz)		/* a constant of width sz into an initialiser */
	CONSZ val;
	int sz;
{
	if ((sz + inwd) > SZLONG) cerror("incode: field > long");

	/*
	 * The VAX original cast through `unsigned`, which is 32 bits.  That was
	 * exactly as wide as its SZLONG, so it was lossless there; here SZLONG
	 * is 64 and the same cast would truncate every wide field.  The file
	 * even carries a comment predicting this: "this code will have to be
	 * replaced if the size of a long on the target machine differs from
	 * that on the host machine".
	 */
# ifdef RTOLBYTES
	word |= ((unsigned long)(val << (SZLONG - sz))) >> (SZLONG - sz - inwd);
# else
	word |= ((unsigned long)(val << (SZLONG - sz))) >> inwd;
# endif
	inwd += sz;
	inoff += sz;

	if (inwd == SZSHORT && SZSHORT >= ALINIT) {
		genshort((short)word);
		word = inwd = 0;
	} else if (inwd == SZINT) {
		genword(word);
		word = inwd = 0;
	} else if (inwd == SZLONG) {
		genlong(word);
		word = inwd = 0;
	}
}

fincode(d, sz)			/* initialise sz bits to the value d */
	double d;
	int sz;
{
	/*
	 * Emitted as raw bits rather than a decimal literal so the value is
	 * exactly what the compiler computed -- no round-trip through the
	 * assembler's decimal parser.  IEEE 754 on both ends, unlike the VAX's
	 * F/D formats.
	 */
	union { float f; unsigned int i; } fu;
	union { double d; unsigned long l; } du;
	char note[64];

	/*
	 * The human-readable comment is formatted here, not by printx.  printx
	 * takes fixed long parameters (see printx.c) and so cannot receive a
	 * double at all: AAPCS64 passes floating arguments in the FP registers,
	 * where a long parameter never looks.  Passing one anyway silently ate
	 * the rest of the format string, including the newline, and welded the
	 * next directive onto the comment.
	 */
	/*
	 * sprintf, not snprintf: snprintf is C99, absent from libv8c, and
	 * variadic -- so under v8cc it resolved from -lSystem and disagreed
	 * about where its arguments were.  note[64] against a %.17g double,
	 * whose widest output is "-1.2345678901234567e-308" at 24 characters,
	 * so the bound was never the thing doing the work here.
	 */
	if (sz == SZDOUBLE) {
		du.d = d;
		sprintf(note, "%.17g", d);
		printx("\t.quad\t0x%lx\t// %s\n", du.l, note);
	} else {
		fu.f = (float)d;
		sprintf(note, "%.9g", (double)fu.f);
		printx("\t.long\t0x%x\t// %s\n", (long)fu.i, note);
	}
	inoff += sz;
}

/* --------------------------------------------------- function prologue */

int ftlab1, ftlab2;
int proflag;

efcode()
{
	/* code for the end of a function */
	long spoff;

	genret(strftn, strftn, retlab);

	spoff = maxboff;
	if (spoff >= BITOOR(AUTOINIT)) spoff -= BITOOR(AUTOINIT);
	spoff += maxtemp;
	spoff /= SZCHAR;

	/* Body is complete: emit prologue, body, epilogue. */
	arm64_endfunction(spoff, minrvar);

	regvar = minrvar = RVARLAST_P1;
}

bfcode(a, n)
	int a[], n;
{
	/*
	 * Nothing is emitted here.  The prologue cannot be written until the
	 * frame size is known, so this only opens the capture buffer; efcode()
	 * closes it and emits prologue + body + epilogue in order.
	 */
	retlab = getlab();
	arm64_beginfunction();
}

defnam(psym)			/* define this location as psym's name */
	register struct symtab *psym;
{
	if (psym->sclass == EXTDEF)
		printx("\t.globl\t%s\n", exname(psym->sname));
	printx("%s:\n", exname(psym->sname));
}

commdec(id)			/* a .comm or .lcomm from stab index id */
	int id;
{
	register struct symtab *psym;
	OFFSZ n;

	psym = &stab[id];
	psym->sflags |= SBSS;
	n = tsize(psym->stype, psym->dimoff, psym->sizoff) / SZCHAR;
	if (psym->sclass == STATIC) {
		if (psym->slevel)
			printx("\t.lcomm\tL%d,%ld\n", psym->offset, (long)n);
		else
			printx("\t.lcomm\t%s,%ld\n", exname(psym->sname), (long)n);
	} else if (psym->sclass == EXTERN) {
		printx("\t.comm\t%s,%ld\n", exname(psym->sname), (long)n);
	} else {
		cerror("Non-static/external in common");
	}
}

myfcon(p)			/* narrow a double constant to float if exact */
	NODE *p;
{
	union { double d; unsigned long l; } u;

	u.d = p->fpn.dval;
	/*
	 * The VAX version tested the second 32-bit half for zero, which
	 * happened to identify VAX F-format-representable values.  For IEEE the
	 * honest test is whether the round trip through float is exact.
	 */
	if ((double)(float)u.d == u.d) {
		p->fn.type = FLOAT;
		p->fn.csiz = FLOAT;
	}
}
