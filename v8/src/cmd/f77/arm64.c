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
 *	[x29 - f77calls().. x29)		outgoing slots nine and up, for a call
 *	[x29 + 0         .. ARGOFFSET)	autos, growing up, checked in prolog()
 *	[x29 + ARGOFFSET .. +f77args())	every parameter, 8 bytes apart: x0-x7
 *					spilled, then slots nine and up copied
 *					down from above by prsave()
 *	[x29 + f77frame() + F77SAVE ..)	the caller's stack arguments, where it
 *					left them -- this is the entry sp
 *
 * The middle region was 64 bytes and held x0-x7 alone until a ninth argument
 * had somewhere to go; the reason it says f77args() rather than a number is
 * that believing it ended at +64 is what produced that defect.  It is a CALL
 * rather than a constant because the fix for that then went the other way and
 * gave every procedure the widest area MAXARGSLOT allows -- see f77args().
 *
 * The callee-saved registers are saved unconditionally: x19-x22 because pass 1
 * hands them out as register variables (MAXREGVAR is 4) and would otherwise
 * clobber libF77's main, x23-x28 and d8-d15 because /lib/f1 materialises into
 * them and AAPCS64 requires the low 64 bits of v8-v15 to be preserved.  Ten x
 * registers is SAVESPACE, which arm64defs already said was 80 bytes.
 */
#define F77SAVE 160	/* the ten stp pairs below, sixteen bytes each */
#define F77ARGSMAX (SZADDR*MAXARGSLOT)		/* the widest spilled-argument area */

/*
 * f77args, f77frame -- the spilled-argument area and the frame, sized to THIS
 * procedure rather than to the widest one MAXARGSLOT allows.
 *
 * THESE WERE CONSTANTS, AND A CONSTANT HERE IS A WORST CASE THAT EVERY
 * PROCEDURE PAYS FOR.  At SZADDR*MAXARGSLOT the area was 512 bytes under
 * `subroutine nop' -- no arguments, no calls, an empty body.  Measured on an
 * 8176 KiB stack, recursion reached 3899 frames against 6699 before the
 * ninth-argument work, and 6699/3899 is 2144/1248 to four figures, which is
 * what says the frame is the whole of the difference rather than a part of it.
 *
 * lastargslot IS FINAL HERE, and that is a property of the call order rather
 * than an assumption: proc.c:46-47 calls epicode() and then procode(), both
 * after the entire procedure body has been parsed, and procode() is what
 * reaches prolog() and prsave().  nextarg() has stopped growing it by then.
 *
 * THE FLOOR IS EIGHT SLOTS BECAUSE prsave() SPILLS x0-x7 UNCONDITIONALLY --
 * four `stp' pairs with no test on lastargslot, since the registers are live
 * whatever the procedure declared.  The ceiling is the clamp prsave() already
 * applies and for the same reason: prolog() refuses past MAXARGSLOT but calls
 * prsave() before it does.
 *
 * AND IT IS ROUNDED UP TO SIXTEEN, WHICH IS NOT COSMETIC.  AAPCS64 requires sp
 * 16-byte aligned at every instruction boundary and ARGOFFSET is a multiple
 * of 16 already, so the two terms that can break it are this one and
 * f77calls(), which rounds for the same reason -- and both are built from a
 * slot count times SZADDR, which is EIGHT.  A nine-slot procedure would
 * otherwise emit `sub sp, sp, #1544' and misalign every frame beneath it.
 *
 * The three sites that spend it -- prsave's `sub sp' and `add x10', goret's
 * `add sp' -- all read f77frame(), so they cannot disagree about it; the
 * entry sp stays f77frame()+F77SAVE above x29 by construction at any size.
 */
f77args()
{
int n;

n = lastargslot;
if(n < SZADDR*8)
	n = SZADDR*8;
if(n > F77ARGSMAX)
	n = F77ARGSMAX;
return( (n + 15) & ~15 );
}

f77frame()
{
return( ARGOFFSET + f77args() );
}

/*
 * f77call, f77calls -- the OUTGOING call area, sized to the widest call this
 * procedure makes.  f77args() above is the incoming half; this is the other
 * 448 bytes, and every procedure carried them, including a leaf that makes no
 * call at all.  It is a region with no VAX counterpart whatever -- a VAX
 * pushes its arguments -- so both the region and its size are target-forced.
 *
 * THE COUNT COMES FROM PASS 1 BECAUSE THAT IS THE ONLY PLACE IT EXISTS.
 * putpcc.c's putcall() has the outgoing slot count as `n' and already branches
 * on it to choose P2CALL against P2CALL0, so recording it is one line there.
 * /lib/f1 recomputes the same number as `vsp - base' when it places the
 * arguments -- and by then this prologue has been written and cannot change.
 *
 * ONE PRODUCER, VERIFIED RATHER THAN RECALLED, because a missed one undersizes
 * the area and corrupts the callee's stack silently.  P2CALL is emitted at
 * putpcc.c:1368 and nowhere else that this program builds: putdmr.c and
 * putscj.c are the dmr and scj back ends and are not in the Makefile's
 * F77P1_NAME, and put.c's ops2[] -- which does hold P2CALL, twice, at the
 * OPCALL and OPCCALL positions -- is a translation table read by putop()'s
 * generic tail, which an OPCALL node never reaches: all four routes to one
 * (putpcc.c:283, :450, :716, :883) go to putcall() first.  The guard in
 * tests/wavea reads the emitted `str x11, [sp, #K]' back rather than trusting
 * that paragraph.
 *
 * IT RESETS ITSELF ON procno RATHER THAN IN procinit(), which is what keeps
 * the change to authentic source down to the single call site.  init.c:182
 * bumps procno once per procedure and BOTH functions test it -- f77call so a
 * new procedure starts from zero, f77calls so a procedure that makes no call
 * at all reads zero rather than the previous procedure's maximum.  Only the
 * second is invisible in ordinary running, and it is the one that decides a
 * leaf: without it a leaf following a wide call inherits its area.
 *
 * AND THE RESET MUST NOT BE IN prsave(), WHICH IS WHERE IT FIRST WENT.
 * proc.c:333-334 calls prolog() once per ENTRY point and prolog() calls
 * prsave(), so a reset there gives the second entry a different frame from the
 * first -- and both prologues branch into ONE body, which stores its outgoing
 * arguments at one set of sp-relative addresses.  f77args() is safe from this
 * only because lastargslot is a single per-procedure global.
 */
LOCAL int callproc = -1;	/* the procno the maximum below describes */
LOCAL int callmax;		/* outgoing argument slots, widest call in it */

f77call(n)
int n;
{
if(callproc != procno)
	{
	callproc = procno;
	callmax = 0;
	}
if(n > callmax)
	callmax = n;
}

f77calls()
{
int n;

if(callproc != procno)		/* this procedure makes no call at all */
	return(0);
n = callmax;
if(n > MAXARGSLOT)		/* prolog() and /lib/f1 both refuse past here */
	n = MAXARGSLOT;
if(n <= 8)			/* x0-x7 take the first eight; nothing spills */
	return(0);
return( ((n - 8) * SZADDR + 15) & ~15 );
}

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
int n, etype;

n = 0;
/* The hidden result arguments come first and are the caller's, not the
   arglist's: a CHARACTER function is passed the place to put its answer and
   how long it is, a COMPLEX one just the place.

   PORT: THE TYPE IS THIS ENTRY'S, NOT THE PROCEDURE'S, and the two are the
   same only when there is one entry.  `proctype' is set once from the FIRST
   entry (proc.c:173) while doentry() -- which allocated the slots this
   function is mapping onto -- reads `np->vtype' per entry (proc.c:367).  So
   keying off the global crosses two operands in a procedure whose entries
   differ in COMPLEX-ness: measured, `real function f(x)' with a `complex'
   `entry g(y)' emitted `str x0, [x9, #16]', putting the hidden result
   pointer into y's slot while cxslot kept &y from the identity spill and
   the real &y was dropped -- printing (3.587324069e-43, 1.401298464e-45)
   for (4.0, 6.0), exit 0, silent.  vax.c:369,375 has the same latent
   defect; this file is not in PROVENANCE, so it is not bound to copy it.

   Only the COMPLEX arm can reach it.  proc.c:372-380 refuses a CHARACTER
   entry of a non-CHARACTER function outright, so when proctype is TYCHAR
   every entry is TYCHAR and the two spellings agree identically -- which is
   why the TYCHAR arm below is a correctness no-op and is changed anyway,
   because a reader must not have to re-derive that argument. */
etype = ep->enamep->vtype;
if(etype == TYCHAR)
	{
	dest[n++] = chslot;
	dest[n++] = chlgslot;
	}
else if( ISCOMPLEX(etype) )
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
   frame is enlarged by f77calls() with x29 lifted off the bottom instead.
   Everything else here is stated from x29 and does not move; goret() reads
   f77frame() from x29 and needs no change at all -- which is why sizing this
   region per procedure touches these two lines and nothing else. */
p2pi("\tsub\tsp, sp, #%d", f77frame() + f77calls());
p2pi("\tadd\tx29, sp, #%d", f77calls());
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
/* PORT: CLAMPED, BECAUSE THE BOUND THAT WOULD STOP IT RUNS AFTER THIS
   FUNCTION.  prolog() refuses lastargslot > SZADDR*MAXARGSLOT, and calls
   prsave() before it does -- so without this, `dest' is indexed past its
   end by the copy loop below on the way to a diagnostic that does fire.
   Measured on a 100-argument subroutine: `str x11, [x9, #1795217402]'.
   The trigger is not exotic; 33 `character*(*)' arguments give 66 slots,
   because each hidden length occupies one. */
if(n > MAXARGSLOT)
	n = MAXARGSLOT;
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
	p2pi("\tadd\tx10, x29, #%d", f77frame() + F77SAVE);
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
p2pi("\tadd\tsp, x29, #%d", f77frame());
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
 * IT HAS NO CALLER ON THIS TARGET AT ALL, which is what the entry above it in
 * the multiple-ENTRY note means in practice.  vax.c and pdp11.c call mvarg from
 * their own prolog(); this file's prolog() does not, because prsave() performs
 * the relocation AT the spill, out of registers that are still live, and needs
 * no staging area to move things out of.  So the sequence is not "empty body
 * plus a refusal", which is what this comment used to claim and what the
 * deleted `multiple ENTRY points' fatal() used to make true -- it is an unused
 * entry point kept because putpcc.c's machine-dependent interface names it.
 *
 * Anything looking for where an entry's arguments are relocated wants
 * argdest() and prsave(), not this.
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
register chainp p;
register struct Dimblock *dp;
int i;
struct Constblock *mkaddcon();	/* returns a POINTER; defs does not declare it */

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

/* THE VAX WALKED ep->arglist HERE FOR TWO UNRELATED REASONS, AND SAYING ONE OF
   THEM WAS DONE WAS READ AS SAYING BOTH WERE.  It copied each argument slot
   into the frame -- that is prsave()'s job here, for all eight registers
   unconditionally, and that much this file said correctly.  It ALSO evaluated
   each dummy array's run-time dimension expressions, which is a different job
   with a different owner, and this file simply had no such loop.  It is below;
   see the PORT note above it.

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
/* AND EACH DUMMY ARRAY'S RUN-TIME DIMENSIONS MUST BE EVALUATED HERE, which is
   pdp11.c:378-387 and vax.c:404-426 and was absent from this file entirely.
   proc.c:1120 allocates a temporary per adjustable dimension
   (`dims[i].dimsize = autovar(1, tyint, PNULL)') and leaves the machine file
   to store the expression into it; nothing else in the tree does, and grep
   finds the assignment in exactly two places, both of them the other two
   targets.  So the temporary was read and never written.

   Measured before this loop: `subroutine show(a,m,n)' with `integer a(m,n)'
   compiled clean -- no diagnostic from f77pass1 or from /lib/f1 -- and
   SIGSEGV'd, because the subscript arithmetic is `ldrsw x24, [x29, #0]' and
   nothing in the procedure ever stores to [x29,#0].  Four shapes faulted;
   a variable LOWER bound is the worse half, since the missing baseoffset
   store gives exit 0 and a plausible wrong number (`22 33' for `11 22'),
   and is right by coincidence for some bounds -- so a single-value case
   cannot see it.

   It is one dimension that needs it: a 1-D adjustable `a(n)' is correct
   without this loop, because nothing multiplies by the first extent.  That
   is why the gap survived a twenty-program corpus.  The VAX's zero-base
   subscript fudge around `checksubs' is deliberately not copied -- it is an
   addressing-mode optimisation for a machine with one, and pdp11.c omits it
   too. */

for(p = ep->arglist ; p ; p = p->nextp)
	if( dp = ( (Namep) (p->datap) )->vdim )
		{
		for(i = 0 ; i < dp->ndim ; ++i)
			if(dp->dims[i].dimexpr)
				puteq( fixtype(cpexpr(dp->dims[i].dimsize)),
					fixtype(cpexpr(dp->dims[i].dimexpr)) );
		if(dp->basexpr)
			puteq( cpexpr(fixtype(dp->baseoffset)),
				cpexpr(fixtype(dp->basexpr)) );
		}

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
