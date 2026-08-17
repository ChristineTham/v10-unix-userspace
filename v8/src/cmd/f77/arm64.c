/*
 * arm64.c -- vax.c's counterpart: the machine-dependent half of f77pass1.
 *
 * Sixteen entry points and four data objects, derived from nm rather than
 * guessed: the fourteen machine-independent objects need 45 external names, and
 * subtracting libc leaves exactly what vax.c provides.  Most are three-line
 * assembler-directive printers and are ported literally; the ones that are not
 * are marked, because those are the port.
 *
 * WHAT IS DELIBERATELY ABSENT, and it is the largest difference from vax.c:
 *
 *	.word LWM<procno>	the VAX register-save mask, read by calls/callg.
 *				arm64 has none, so prolog() does not emit one and
 *				fixlwm() has nothing to patch.
 *	movl n(ap),m(fp)	mvarg()'s argument copy.  AAPCS64 passes in x0-x7
 *				and this port's own prologue spills them, so the
 *				copy is pass 2's business.
 *	movl n(ap),m(fp)	mvarg()'s argument copy -- AAPCS64 has no ap.
 *
 * The mask is a HANDSHAKE on the VAX, not one line: pass 1 emits `.word LWM<n>'
 * as a forward reference and fixlwm() later emits `.set LWM<n>,0x<mask>' once
 * register allocation has settled highregvar.  arm64 has no mask to
 * communicate, so the contract is removed rather than reimplemented -- and
 * removing a handshake is safer than translating one, because the failure mode
 * of a mismatched prologue is a binary that links and corrupts its own frame.
 *
 * BUT THE FRAME ITSELF STAYS HERE, which is the opposite of what this comment
 * first said.  prsave() and goret() emit the prologue and epilogue as literal
 * text, exactly as the VAX's did with `subl2' and `ret'.  The intermediate
 * settled it: the entry stub is written AFTER the body -- putbracket() rewrites
 * the header in place -- so a second pass emitting the prologue at LBRACKET
 * would put it before the label it belongs to, and an epilogue at RBRACKET
 * lands after the stub's branch where it is unreachable.  Both were measured
 * that way round before moving here.
 *
 * SDB IS OFF (arm64defs leaves it undefined), so the 120 lines of stab
 * machinery in vax.c -- prstab, stabline, prstleng, stabtype, prcomssym -- are
 * stubs here.  f77's own README spends its first half apologising for that
 * interface and it is a.out's; a debugger for V8 Fortran is a separate project.
 */

#include "defs"
#include "pccdefs"

/*
	ARM64 (AAPCS64, Mach-O) - SPECIFIC ROUTINES
*/

/*
 * The register variables.  v8cc's own callee-saved set is x19-x28 (see
 * compiler/ccom-arm64/macdefs.h), and MAXREGVAR in arm64defs is 4, so four of
 * them are offered.  NOT YET EXERCISED: every consumer of regnum[] is in the
 * register-variable path, which needs pass 2 agreement to be meaningful, so it
 * is stated at the width the tables require and marked rather than trusted.
 */
int maxregvar = MAXREGVAR;
int regnum[] =  { 19, 20, 21, 22, 23, 24 } ;

/*
 * intcon[] is upstream's, unchanged: it is the answer to Fortran's integer
 * inquiry intrinsics (radix, digits, huge) and none of it is machine-dependent
 * once the widths are fixed.  SZLONG is 4 here, as it was on the VAX, so the
 * table is identical -- see the note in arm64defs about why SZLONG is pinned.
 */
ftnint intcon[14] =
	{ 2, 2, 2, 2,
	  15, 31, 24, 56,
	  -128, -128, 127, 127,
	  32767, 2147483647 };

/*
 * realcon[] is NOT upstream's, and this is the one data table that had to
 * change: vax.c's is VAX D-format written in octal, and arm64 is IEEE 754.
 * The six values are TINY(real), TINY(double), HUGE(real), HUGE(double),
 * EPSILON(real), EPSILON(double), in that order -- the same order upstream's
 * comment implies and the same one init.c reads them in.  Written as doubles
 * rather than as bit patterns because IEEE decimal literals are exact enough
 * here and a hex pattern would have to be endian-correct by hand.
 */
double realcon[6] =
	{
	1.17549435e-38,			/* FLT_MIN */
	2.2250738585072014e-308,	/* DBL_MIN */
	3.40282347e+38,			/* FLT_MAX */
	1.7976931348623157e+308,	/* DBL_MAX */
	1.19209290e-7,			/* FLT_EPSILON */
	2.2204460492503131e-16		/* DBL_EPSILON */
	};



/*
 * prsave -- the frame, and it is emitted HERE rather than by /lib/f1, which is
 * the opposite of what this file first assumed.  The intermediate settled it:
 * the entry stub arrives AFTER the body, because putbracket() rewrites the
 * header in place, so a second pass that emitted the prologue at LBRACKET would
 * put it before the label it belongs to.  prsave() is called from prolog(), at
 * exactly the right point, and the VAX did the same thing here with `subl2'.
 *
 * A FIXED 256-byte frame.  Nothing this pass emits spills, and computing one
 * would mean adopting arm64_endfunction()'s three-region layout -- the contract
 * this design avoids needing.  256 covers the stack argument area AAPCS64 wants
 * beyond x0-x7 and costs one instruction.
 */
prsave(proflab)
int proflab;
{
p2pass("\tstp\tx29, x30, [sp, #-16]!");
p2pass("\tmov\tx29, sp");
p2pass("\tsub\tsp, sp, #256");
}



/*
 * goret -- the epilogue, matching prsave() above.  proc.c calls it at the exit
 * label, which is the one place it can go: the body falls through to there and
 * the entry stub is emitted later still.  A /lib/f1 that emitted this at
 * RBRACKET put it after the stub's `b', where it is unreachable and the body
 * runs off the end -- measured, before this moved here.
 */
goret(type)
int type;
{
p2pass("\tmov\tsp, x29");
p2pass("\tldp\tx29, x30, [sp], #16");
p2pass("\tret");
}



/*
 * mvarg -- the VAX copied argument slots from the ap frame to the fp frame.
 * AAPCS64 has no ap and this port's prologue spills x0-x7 itself, so the copy
 * is pass 2's.  Silent for the same reason goret() is.
 */
mvarg(type, arg1, arg2)
int type, arg1, arg2;
{
}



prlabel(fp, k)
FILEP fp;
int k;
{
fprintf(fp, "L%d:\n", k);
}



/*
 * prconi -- .short for a 2-byte integer, NOT .word.  On the VAX `.word' is two
 * bytes; clang's arm64 assembler makes it FOUR, so porting the directive
 * literally would silently double every INTEGER*2 initialiser.  A directive
 * whose name means different widths on two machines is worse than one that does
 * not exist.
 */
prconi(fp, type, n)
FILEP fp;
int type;
ftnint n;
{
fprintf(fp, "\t%s\t%ld\n", (type==TYSHORT ? ".short" : ".long"), (long) n);
}



/*
 * prcona -- .quad, not .long, for the same reason praddr uses it: this emits
 * the ADDRESS of a label into initialised data, and an address is eight bytes
 * here.  Measured before it was: ld reported `32-bit pointer in 64-bit arch:
 * r_length=2', which names the relocation and not the directive that made it.
 */
prcona(fp, a)
FILEP fp;
ftnint a;
{
fprintf(fp, "\t.quad\tL%ld\n", (long) a);
}



/*
 * prconr -- the bit-pattern form, which is vax.c's own second variant (the file
 * carries two prconr definitions, the first for a `float' argument and the
 * second commented "non-portable cheat to preserve bit patterns").  The cheat
 * is the portable one in practice: `.float 0f%e' asks the assembler to re-parse
 * a decimal rendering, and clang's arm64 assembler has no 0f syntax at all.
 * Emitting the bytes says exactly what was meant.
 */
prconr(fp, type, x)
FILEP fp;
int type;
double x;
{
union { double xd; int xl[2]; } cheat;
union { float xf; int xi; } single;

if(type == TYREAL)
	{
	single.xf = x;
	fprintf(fp, "\t.long\t0x%x\n", single.xi);
	}
else
	{
	cheat.xd = x;
	fprintf(fp, "\t.long\t0x%x,0x%x\n", cheat.xl[0], cheat.xl[1]);
	}
}



praddr(fp, stg, varno, offset)
FILEP fp;
int stg, varno;
ftnint offset;
{
char *memname();

if(stg == STGNULL)
	fprintf(fp, "\t.quad\t0\n");
else
	{
	/* .quad, not .long: an address is eight bytes here, and SZADDR in
	   arm64defs says so.  This is the only directive in the file whose
	   WIDTH differs from the VAX's rather than its spelling. */
	fprintf(fp, "\t.quad\t%s", memname(stg,varno));
	if(offset)
		fprintf(fp, "+%ld", (long) offset);
	fprintf(fp, "\n");
	}
}



preven(k)
int k;
{
register int lg;

/* PORT: A DATA BLOCK IS NEVER ALIGNED LESS THAN A POINTER HERE, which is why
   this does not simply translate upstream's ladder.  dodata() picks the
   alignment from the FIRST element's type -- typealign[TYLONG] is 4 -- and the
   block may still contain an address further in, as a cilist does at offset 16.
   Mach-O relocations require the pointer to be 8-aligned, and ld says so:
   `alignment (1) of atom v.1 is too small and may result in unaligned
   pointers', then refuses the object.  On a VAX SZADDR was 4 and the first
   element's alignment covered everything after it.
   The cost is padding between blocks and nothing else. */
if(k > 1)
	lg = 3;
else
	return;
fprintf(asmfile, "\t.p2align\t%d\n", lg);
}



/*
 * prcmgoto -- Fortran's computed GOTO.  vax.c spells this vaxgoto() and reaches
 * it through a #define in pccdefs; the name putpcc.c calls is prcmgoto, which is
 * also what pdp11.c defines directly, so this file uses the callers' name.
 *
 * The VAX had `casel', a one-instruction indexed branch against a table of
 * .word offsets.  arm64 has no such instruction, so this emits the comparison
 * and an indexed load from a table of addresses -- which is what a C switch
 * compiles to here.  NOT YET EXERCISED by anything that has run.
 */
prcmgoto(index, nlab, skiplabel, labs)
expptr index;
int nlab, skiplabel;
struct Labelblock *labs[];
{
int i, arrlab;

putforce(TYINT, index);
p2pi("\tcmp\tx0, #%d", nlab);
p2pi("\tb.hi\tL%d", skiplabel);
p2pi("\tadrp\tx9, L%d@PAGE", arrlab = newlabel());
p2pi("\tadd\tx9, x9, L%d@PAGEOFF", arrlab);
p2pass("\tldr\tx10, [x9, x0, lsl #3]");
p2pass("\tbr\tx10");
prlabel(asmfile, arrlab);
for(i = 0; i < nlab; ++i)
	fprintf(asmfile, "\t.quad\tL%d\n", labs[i]->labelno);
}



prarif(p, neg, zer, pos)
expptr p;
int neg, zer, pos;
{
putforce(p->headblock.vtype, p);
if( ISINT(p->headblock.vtype) )
	p2pass("\tcmp\tx0, #0");
else
	p2pass("\tfcmp\td0, #0.0");
p2pi("\tb.lt\tL%d", neg);
p2pi("\tb.eq\tL%d", zer);
p2pi("\tb\tL%d", pos);
}



char *memname(stg, mem)
int stg, mem;
{
static char s[20];

switch(stg)
	{
	case STGCOMMON:
	case STGEXT:
		sprintf(s, "_%s", varstr(XL, extsymtab[mem].extname) );
		break;

	case STGBSS:
	case STGINIT:
		sprintf(s, "v.%d", mem);
		break;

	case STGCONST:
		sprintf(s, "L%d", mem);
		break;

	case STGEQUIV:
		sprintf(s, "q.%d", mem+eqvstart);
		break;

	default:
		badstg("memname", stg);
	}
return(s);
}



char *
ftnname(stg, name)
int stg;
char *name;
{
	static char s[40];

	switch (stg) {
	case STGCOMMON:
	case STGEXT:
		sprintf(s, "_%s", varstr(XL, name) );
		break;
	default:
		badstg("ftnname", stg);
	}
	return (s);
}



/*
 * prlocvar -- with an explicit alignment, which the VAX's two-operand .lcomm did
 * not need.  ld64 warns `alignment (1) of atom v.1 is too small and may result
 * in unaligned pointers' otherwise, and a Fortran COMMON or local can hold a
 * double or an address.  Three is log2(8), the widest thing that can land here.
 */
prlocvar(s, len)
char *s;
ftnint len;
{
fprintf(asmfile, "\t.lcomm\t%s,%ld,3\n", s, (long) len);
}



prext(name, leng, init)
char *name;
ftnint leng;
int init;
{
if(leng == 0)
	fprintf(asmfile, "\t.globl\t_%s\n", name);
else
	fprintf(asmfile, "\t.comm\t_%s,%ld\n", name, (long) leng);
}



prendproc()
{
}



prtail()
{
}



/*
 * prolog -- the entry sequence, and the one function here that is a rewrite
 * rather than a translation.  See the note at the top of the file: everything
 * the VAX version emits beyond the label is a register mask, an ap-relative
 * argument copy, or a frame adjustment, and all three are pass 2's on this
 * target.  What is left is the label itself and the jump to the entry point.
 */
prolog(ep, argvec)
struct Entrypoint *ep;
Addrp argvec;
{
p2pass("\t.p2align\t2");

if(procclass == CLMAIN)
	{
	if(fudgelabel)
		{
		if(ep->entryname)
			p2ps("_%s:", varstr(XL, ep->entryname->extname));
		putlabel(fudgelabel);
		fudgelabel = 0;
		}
	else
		{
		p2pass( "_MAIN__:" );
		/* PORT: prsave() HERE TOO, which vax.c does not do.  Upstream's
		   CLMAIN arm emits only the label and the register mask, because
		   the VAX's `calls' instruction builds the frame from that mask
		   at the CALL site.  arm64 has no such instruction and every
		   function builds its own, so the main program needs the same
		   prologue as any other -- measured: without this the entry stub
		   was a bare `b L11' and the epilogue at L12 restored a frame
		   nobody had made. */
		prsave(0);
		}
	}
else if(ep->entryname)
	{
	if(fudgelabel)
		{
		putlabel(fudgelabel);
		fudgelabel = 0;
		}
	else
		{
		p2ps("_%s:", varstr(XL, ep->entryname->extname));
		prsave(0);
		}
	}

if(procclass == CLBLOCK)
	return;

/* The VAX walked ep->arglist here copying each slot into the frame.  Nothing to
   do: pass 2 spills x0-x7 and addresses the arguments where it put them. */

putgoto(ep->entrylabel);
}



/*
 * fixlwm -- patches the register mask on the VAX.  There is no mask, so there
 * is nothing to patch.  It stays an entry point because prolog() in the shared
 * half of the tree calls it; a silent one is the honest answer.
 */
fixlwm()
{
}



/*
 * prhead -- the LEFT BRACKET record, and it is nearly machine-independent: it
 * tells pass 2 how many register variables and how much automatic space the
 * procedure needs, which is what mainp2()'s BBEG case reads back.  Upstream's
 * ARGREG-highregvar is a VAX register NUMBER arithmetic; here the quantity
 * wanted is simply the count of register variables in use.
 */
prhead(fp)
FILEP fp;
{
#if FAMILY==PCC
	p2triple(P2LBRACKET, highregvar, procno);
	p2word( (long) (BITSPERCHAR*autoleng) );
	p2flush();
#endif
}



/* --- the stab half, all stubs: arm64defs leaves SDB undefined ------------- */

prdbginfo()
{
}



prstab(s, code, type, loc)
char *s, *loc;
int code, type;
{
}



prstleng(np, leng)
Namep np;
ftnint leng;
{
}



stabtype(p)
Namep p;
{
return(0);
}



prcomssym(np, xp)
Namep np;
struct Extsym *xp;
{
}
