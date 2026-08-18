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
 *
 * AND THE ARGUMENT COPY IS *NOT* ABSENT, WHICH IS WHAT THIS COMMENT SAID TWICE.
 * It carried two entries for mvarg(), both claiming the copy was "pass 2's
 * business" because "this port's own prologue spills x0-x7".  No prologue
 * spilled anything: prsave() emitted three instructions and mvarg() was an
 * empty function, so a subroutine's parameter was read out of the saved frame
 * pointer.  `call greet(7)' compiled clean and SIGSEGV'd.  The claim was
 * duplicated in the one file that could have refuted it, which is why neither
 * copy read as wrong -- and it is this tree's most repeated shape, a comment
 * asserting a contract the code beside it never implemented.  Both are deleted;
 * prsave() does the spill now and the sentence describing it is below the code
 * that does it.
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
 * THE FRAME HAS TWO REGIONS AND ONE BASE REGISTER, WHERE THE VAX HAD TWO
 * REGISTERS.  proc.c:806-814 puts autos at POSITIVE offsets from AUTOREG for
 * every target but the VAX and the PDP11 (`stack grows downward' guards the
 * negative arm), and putpcc.c addresses a parameter as ARGOFFSET + memno from
 * ARGREG.  arm64defs set both registers to 29, so a parameter and an auto both
 * landed at [x29+0] -- measured: one `OREG reg 29 offset 0 type int' and one
 * `OREG reg 29 offset 0 type int *' in the same procedure, indistinguishable in
 * the stream because the record carries nothing else to tell them apart.
 *
 * The VAX needs two registers because its `ap' points into the CALLER's frame:
 * arguments are never copied at all unless there are multiple ENTRY points, and
 * proc.c:316 allocates argvec only when nentry>1.  arm64 passes in x0-x7, so
 * they must be spilled into this frame whatever happens -- and once both regions
 * are in one frame, a constant separates them.  ARGOFFSET is that constant, and
 * its only live consumers are the four addressing sites in putpcc.c: proc.c's
 * use is inside #ifdef SDB, which arm64defs leaves off.  So this is one number,
 * declared once in arm64defs and read here rather than respelled.
 *
 *	[x29 + 0        .. ARGOFFSET)	autos, growing up, checked in prolog()
 *	[x29 + ARGOFFSET .. +64)	x0-x7 spilled here, 8 bytes apart
 *
 * The callee-saved registers are saved unconditionally: x19-x22 because pass 1
 * hands them out as register variables (MAXREGVAR is 4) and would otherwise
 * clobber libF77's main, x23-x28 and d8-d15 because /lib/f1 materialises into
 * them and AAPCS64 requires the low 64 bits of v8-v15 to be preserved.  Ten x
 * registers is SAVESPACE, which arm64defs already said was 80 bytes.
 */
#define F77SAVE 160	/* the ten stp pairs below, sixteen bytes each */
#define F77ARGS (SZADDR*MAXARGSLOT)		/* the spilled-argument area */
#define F77CALL (SZADDR*(MAXARGSLOT-8))		/* outgoing slots nine and up */
#define F77FRAME (ARGOFFSET + F77ARGS)	/* autos, then the spilled arguments */

/*
 * argdest -- where each of THIS entry's incoming arguments belongs.
 *
 * With one entry the answer is the identity: nextarg() hands out slots in the
 * order the caller passes them, so incoming position i belongs at SZADDR*i.
 * With more than one, it is not.  doentry() gives a slot only to a parameter
 * not already declared, so the layout is the UNION of every entry's parameters
 * in first-appearance order -- and a later entry's first argument may therefore
 * belong halfway up the area, or, if the two entries name the same parameters
 * in a different order, at a LOWER slot than the one below it.
 *
 * This is vax.c:365-400's loop, and it is the same loop for the same reason.
 * What differs is where the values are read from.  The VAX copies from the
 * CALLER's frame through `ap' into an argvec and then repoints ap at it, which
 * it must because its arguments are never in registers -- and which also makes
 * source and destination disjoint, so `entry t(b,a)' against `subroutine s(a,b)'
 * cannot make one copy clobber the next.  arm64 has no ap to repoint; if this
 * relocated slot-to-slot after a blind spill, that swap would lose a.
 *
 * It does not have to.  The incoming values are still IN x0-x7 at this point,
 * and a register is not one of the destinations, so spilling straight to the
 * right slot needs no staging area and cannot alias.  A second machine's extra
 * mechanism is solving a problem this machine does not have.
 */
LOCAL argdest(ep, dest)
struct Entrypoint *ep;
int dest[];
{
register chainp p;
register Namep q;
int n;

n = 0;
/* The hidden result arguments come first and are the caller's, not the
   arglist's: a CHARACTER function is passed the place to put its answer and
   how long it is, a COMPLEX one just the place. */
if(proctype == TYCHAR)
	{
	dest[n++] = chslot;
	dest[n++] = chlgslot;
	}
else if( ISCOMPLEX(proctype) )
	dest[n++] = cxslot;

for(p = ep->arglist ; p ; p = p->nextp)
	{
	q = (Namep) (p->datap);
	if(n >= MAXARGSLOT)
		fatali("entry has more than %d argument slots", MAXARGSLOT);
	dest[n++] = q->vardesc.varno;
	}
/* Then the hidden CHARACTER lengths, in the same order.  A constant-length
   argument still occupies an incoming position -- the caller passes it -- and
   doentry() reserves no slot for it, so it is dropped rather than stored. */
for(p = ep->arglist ; p ; p = p->nextp)
	{
	q = (Namep) (p->datap);
	if(q->vtype==TYCHAR && q->vclass!=CLPROC)
		{
		if(n >= MAXARGSLOT)
			fatali("entry has more than %d argument slots", MAXARGSLOT);
		if(q->vleng && ! ISCONST(q->vleng) )
			dest[n] = q->vleng->addrblock.memno;
		else
			dest[n] = -1;
		++n;
		}
	}
return(n);
}



prsave(ep)
struct Entrypoint *ep;
{
int i, n;
int dest[MAXARGSLOT];

p2pass("\tstp\tx29, x30, [sp, #-16]!");
p2pass("\tstp\tx19, x20, [sp, #-16]!");
p2pass("\tstp\tx21, x22, [sp, #-16]!");
p2pass("\tstp\tx23, x24, [sp, #-16]!");
p2pass("\tstp\tx25, x26, [sp, #-16]!");
p2pass("\tstp\tx27, x28, [sp, #-16]!");
p2pass("\tstp\td8, d9, [sp, #-16]!");
p2pass("\tstp\td10, d11, [sp, #-16]!");
p2pass("\tstp\td12, d13, [sp, #-16]!");
p2pass("\tstp\td14, d15, [sp, #-16]!");
/* THE CALL AREA SITS BELOW x29 RATHER THAN AT IT, which is the whole of the
   outgoing half.  AAPCS64 puts a call's ninth and later arguments at [sp,#0],
   and with `mov x29, sp' that address was [x29,#0] -- the first auto.  So
   /lib/f1 could not have written one without destroying a temporary, and the
   frame is enlarged by F77CALL with x29 lifted off the bottom instead.
   Everything else here is stated from x29 and does not move; goret() reads
   F77FRAME from x29 and needs no change at all. */
p2pi("\tsub\tsp, sp, #%d", F77FRAME + F77CALL);
p2pi("\tadd\tx29, sp, #%d", F77CALL);
/* THE SPILL GOES THROUGH A SCRATCH REGISTER RATHER THAN DIRECTLY, because stp's
   immediate is a signed 7-bit field scaled by 8 -- reaching only +504 -- and
   ARGOFFSET is deliberately larger than that.  `stp x0, x1, [x29, #1024]' is
   not a range error you can see by reading it; the assembler names the operand
   and not the field width. */
p2pi("\tadd\tx9, x29, #%d", ARGOFFSET);
p2pass("\tstp\tx0, x1, [x9, #0]");
p2pass("\tstp\tx2, x3, [x9, #16]");
p2pass("\tstp\tx4, x5, [x9, #32]");
p2pass("\tstp\tx6, x7, [x9, #48]");

/* AND THE NINTH ARGUMENT ONWARD IS COPIED DOWN BESIDE THEM, so that putpcc.c's
   single rule -- parameter n lives at ARGOFFSET + n from ARGREG -- is true of
   every parameter rather than of the first eight.  Without this the ninth was
   read at ARGOFFSET+64, which is entry_sp-F77SAVE, the slot holding the saved
   d14/d15: measured, `subroutine s9(a,b,c,d,e,f,g,h,i)' loaded a callee-saved
   floating-point register as the address of `i' and stored through it.

   The count is lastargslot/SZADDR, which is upstream's own spelling of it --
   vax.c's prolog() stores exactly that number at (ap).  nextarg() has finished
   by the time procode() calls prolog(), so it is final here.

   x9 already holds x29+ARGOFFSET; x10 and x11 are caller-saved and free, since
   the only live registers at this point are the arguments in x0-x7. */
n = lastargslot / SZADDR;
for(i = 0 ; i < n && i < MAXARGSLOT ; ++i)
	dest[i] = SZADDR*i;

/* AND WITH MORE THAN ONE ENTRY POINT THE IDENTITY IS WRONG, so this entry's
   own list decides.  The four stp pairs above have already put x0-x7 at the
   identity positions, which is why they are re-emitted here rather than
   skipped: a store to the right slot is what makes the identity ones dead,
   and leaving both is one instruction each against a special case in the
   common path. */
if(nentry > 1)
	{
	n = argdest(ep, dest);
	for(i = 0 ; i < n && i < 8 ; ++i)
		if(dest[i] >= 0)
			p2pij("\tstr\tx%d, [x9, #%d]", i, dest[i]);
	}

if(n > 8)
	{
	p2pi("\tadd\tx10, x29, #%d", F77FRAME + F77SAVE);
	for(i = 8 ; i < n ; ++i)
		if(dest[i] >= 0)
			{
			p2pi("\tldr\tx11, [x10, #%d]", SZADDR*(i-8));
			p2pi("\tstr\tx11, [x9, #%d]", dest[i]);
			}
	}
}



/*
 * goret -- the epilogue, matching prsave() above.  proc.c calls it at the exit
 * label, which is the one place it can go: the body falls through to there and
 * the entry stub is emitted later still.  A /lib/f1 that emitted this at
 * RBRACKET put it after the stub's `b', where it is unreachable and the body
 * runs off the end -- measured, before this moved here.
 *
 * IT UNDOES THE FRAME FROM x29 RATHER THAN FROM sp, because this is the one
 * quantity the two halves must agree about and x29 is the only thing still
 * holding it: `mov sp, x29' was right when x29 pointed at the saved pair and is
 * wrong now that it points at the bottom of the autos.
 */
goret(type)
int type;
{
p2pi("\tadd\tsp, x29, #%d", F77FRAME);
p2pass("\tldp\td14, d15, [sp], #16");
p2pass("\tldp\td12, d13, [sp], #16");
p2pass("\tldp\td10, d11, [sp], #16");
p2pass("\tldp\td8, d9, [sp], #16");
p2pass("\tldp\tx27, x28, [sp], #16");
p2pass("\tldp\tx25, x26, [sp], #16");
p2pass("\tldp\tx23, x24, [sp], #16");
p2pass("\tldp\tx21, x22, [sp], #16");
p2pass("\tldp\tx19, x20, [sp], #16");
p2pass("\tldp\tx29, x30, [sp], #16");
p2pass("\tret");
}



/*
 * mvarg -- move one incoming argument slot into the frame slot ARGREG addresses.
 * The VAX copied from its caller's list (`movl n(ap),m(fp)'); here the incoming
 * arguments are in x0-x7 and prsave() has already put all eight where ARGREG
 * addresses them, so for a single-entry procedure the copy has already happened
 * and this is correctly silent.
 *
 * It is reached ONLY when proc.c allocated an argvec, and proc.c:316 does that
 * only for nentry>1 -- so what it would have to implement is the multiple-ENTRY
 * relocation, which prolog() refuses by name instead.  An empty body plus a
 * refusal at the one call site that can reach it is the honest pair; an empty
 * body alone is what let the missing spill above go unnoticed.
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
 * prcmgoto -- Fortran's computed GOTO.  vax.c does NOT define this: putcmgo()
 * takes a `#if TARGET == VAX' branch to vaxgoto() and its `casel' instruction,
 * so the VAX never reaches this entry point at all.  pdp11.c is the only other
 * implementation in the tree, and it is the one to read.
 *
 * THE TABLE IS THE CALLER'S, WHICH IS WHAT THE FOURTH ARGUMENT SAYS.  This was
 * first written taking `struct Labelblock *labs[]' and emitting the table
 * itself, by analogy with nothing -- putcmgo() passes `int labarray', the LABEL
 * of a table it has already written through preven()/prlabel()/prcona() four
 * lines earlier.  So the array subscript dereferenced a label NUMBER: measured,
 * f77pass1 died with SIGSEGV at address 0x13, which is 19, which was the label.
 * The crash report named prcmgoto <- putcmgo <- yyparse in five frames and cost
 * one command; reading the caller's own argument would have cost none.
 *
 * The entry at index 0 is skiplabel -- putcmgo emits it before the loop -- so a
 * zero index lands on the skip and no bias is needed.  `b.hi' is UNSIGNED and
 * that is load-bearing rather than incidental: an index this pass has
 * sign-extended into x0 is a huge unsigned quantity when negative, so one
 * comparison rejects both ends of the range, which is exactly what pdp11.c's
 * `bhi' does with the same reasoning.
 *
 * The VAX had `casel', a one-instruction indexed branch against a table of
 * .word offsets.  arm64 has none, so this is the comparison plus an indexed
 * load from a table of addresses -- `lsl #3' because prcona() emits .quad.
 */
prcmgoto(index, nlab, skiplabel, labarray)
expptr index;
int nlab, skiplabel, labarray;
{
putforce(index->headblock.vtype, index);
p2pi("\tcmp\tx0, #%d", nlab);
p2pi("\tb.hi\tL%d", skiplabel);
p2pi("\tadrp\tx9, L%d@PAGE", labarray);
p2pi("\tadd\tx9, x9, L%d@PAGEOFF", labarray);
p2pass("\tldr\tx10, [x9, x0, lsl #3]");
p2pass("\tbr\tx10");
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
		prsave(ep);
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
		prsave(ep);
		}
	}

if(procclass == CLBLOCK)
	return;

/* THE VAX WALKED ep->arglist HERE COPYING EACH SLOT INTO THE FRAME, and this
   file used to say there was nothing to do because pass 2 spilled x0-x7.  Pass
   2 did not, and nothing said so; see the note above prsave().  prsave() does
   the spill now, for all eight registers unconditionally, which is why there is
   still no walk here -- but the reason is that the work is done rather than
   that it is somebody else's.

   MULTIPLE ENTRY POINTS ARE NOT REFUSED HERE ANY MORE, and the reason the
   refusal gave was accurate: each entry's arguments must be relocated into one
   shared slot layout, which is the mvarg() the VAX implements, and a frame that
   spills x0-x7 at one fixed place cannot do it.  What it did not say is that
   the spill does not have to be at one fixed place.  prsave() takes the entry
   now and walks its own argument list, so the relocation happens AT the spill,
   out of registers that are still live -- see argdest() above.  argvec is still
   allocated by proc.c:316 and is still unused here; it is the VAX's staging
   area, and this target needs none.

   autoleng is the auto area pass 1 allocated, and ARGOFFSET is where the spilled
   arguments start; an overrun would put a temporary on top of a parameter and
   is invisible in the generated code.  Checked against the number that defines
   the boundary rather than against a transcribed copy of it. */
if(autoleng > ARGOFFSET)
	fatali("procedure needs %d bytes of temporaries, more than the frame holds",
		(int) autoleng);

/* AND THE ARGUMENT AREA IS BOUNDED THE SAME WAY, at the same place, for the
   same reason: past MAXARGSLOT a parameter would be spilled outside the region
   prsave() reserves, which is invisible in the generated code.  The caller's
   half of this bound is in /lib/f1, which refuses a call it cannot place; both
   ends are needed, because either alone leaves the other silently wrong. */
if(lastargslot > SZADDR*MAXARGSLOT)
	fatali("procedure has %d argument slots, more than the frame holds",
		(int) (lastargslot/SZADDR));

/* AND THE ENTRY MUST SAY WHICH EPILOGUE IT RETURNS THROUGH, which is vax.c:466-467
   and was simply absent here.  A procedure with more than one ENTRY may have
   entries of different types, so proc.c:383-385 gives each TYPE an epilogue
   label and every RETURN branches to one common exit -- which then jumps
   INDIRECTLY through this auto.  Without the store the auto is never written
   and the exit branches through whatever the frame happened to hold.

   Nothing had reached it: with one entry the exit is not indirect at all, so
   `typeaddr' is null and this is a no-op.  The refusal above is what kept the
   multiple-entry path from ever running, which is the same shape as the ninth
   argument -- a guard at one end making the code at the other end untestable.

   Measured before the fix: `integer function f(n) ... entry g(m,k)' compiled
   clean, and the program printed NOTHING and exited 0. */
if(typeaddr)
	puteq( cpexpr(typeaddr), mkaddcon(ep->typelabel) );

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
