/*
 * Output layer for the ARM64 backend: replaces vax/printx.c and vax/lcatch2.c.
 *
 * Two jobs.
 *
 * 1. printx() -- ccom's own printf, which the machine-independent passes call
 *    for every line of assembly and every diagnostic.  V8's version takes the
 *    address of its first variadic argument and walks forward:
 *
 *	printx(fmt, list) char *fmt; long list; { sprintxl(bufpt, fmt, &list); }
 *
 *    Correct on a VAX, where `calls` pushed all arguments contiguously onto the
 *    stack.  Under AAPCS64 the first eight arrive in registers, so the walk
 *    reads garbage -- this is what segfaults the stage-0 compiler.  Since this
 *    file is new code rather than authentic V8, it simply uses stdarg.
 *
 *    (V8's original printx.c will work again unmodified once v8cc compiles it,
 *    because v8cc's prologue spills x0-x7 into a contiguous block.  That is the
 *    whole point of the ABI choice in PLAN.md S4.  We do not depend on it here.)
 *
 * 2. Function-body buffering.  The VAX needed no explicit frame allocation --
 *    the `calls` instruction built the frame from the callee's entry mask, so
 *    V8 could stream assembly straight out and patch the mask afterwards with a
 *    trailing `.set`.  ARM64 must allocate its own frame in the prologue, and
 *    the frame size is not known until the body has been generated.  A forward
 *    `.set` does not help: the assembler rejects
 *
 *	sub sp, sp, #Lframe0		error: expected ... integer in range [0,4095]
 *
 *    because it needs the value at encoding time.  So the body is captured in
 *    memory, and bfcode/efcode emit prologue and epilogue around it once the
 *    frame layout and callee-saved register usage are known.
 */

# include <stdio.h>
# include <string.h>
# include "mfile2.h"
# include "gencode.h"

/*
 * <stdlib.h> is not here because V8 does not have one -- ANSI C postdates this
 * source by four years -- and this file has to compile under BOTH clang and
 * v8cc, against whichever include tree it is pointed at.  So it declares what
 * it uses, which is exactly what V8's own libc does: src/libc/stdio/flsbuf.c
 * opens with `char *malloc();` for the same reason.
 *
 * An earlier version made the include conditional on __STDC__.  That asks the
 * wrong question -- it tests which COMPILER is running, when what matters is
 * which HEADERS are on the path, and the two come apart the moment clang is
 * aimed at $V8ROOT/usr/include.  Declaring unconditionally has no such seam,
 * and it makes both builds of this file see identical declarations, which the
 * fixpoint comparison in tests/selfhost depends on.
 *
 * char *, not void *: V8's libc returns char *, and both call sites cast.
 * Getting this wrong is not a style question -- an undeclared malloc returns
 * int, which on LP64 truncates the heap pointer, and that is the single most
 * common bug class in this port.
 */
extern char *malloc();
extern char *realloc();

FILE *outfile;			/* set by reader.c/local.c; defaults to stdout */

/*
 * Globals that lived in vax/printx.c.  syncstdio is set from ccom's -X flags
 * (common/scan.c) and forces a flush per line when the output is a terminal;
 * slineno is the statement line number the parser maintains.
 */
int nosharp, syncstdio;
int slineno;

/* ---------------------------------------------------------------- capture */

static char *cap;		/* capture buffer, or 0 when not capturing */
static long caplen, capmax;

void
emitcapture()			/* begin capturing into memory */
{
	if (cap == 0) {
		capmax = 8192;
		cap = (char *)malloc(capmax);
		if (cap == 0) cerror("out of memory buffering a function");
	}
	caplen = 0;
}

char *
emitcaptured(lenp)		/* stop capturing; return the text */
	long *lenp;
{
	if (lenp) *lenp = caplen;
	if (cap) cap[caplen] = '\0';
	return (cap);
}

static void
capput(s, n)
	char *s;
	long n;
{
	while (caplen + n + 1 > capmax) {
		capmax *= 2;
		cap = (char *)realloc(cap, capmax);
		if (cap == 0) cerror("out of memory buffering a function");
	}
	memcpy(cap + caplen, s, n);
	caplen += n;
}

/* True while a function body is being captured. */
int capturing;

/* The single exit for all generated text; printx.c calls this. */
emitout(buf, n)
	char *buf;
	int n;
{
	if (capturing)
		capput(buf, (long)n);
	else
		fwrite(buf, 1, n, outfile ? outfile : stdout);
	return (n);
}

void emitstart() { capturing = 1; emitcapture(); }
void emitstop()  { capturing = 0; }

/* ----------------------------------------------------------------- printx */

/*
 * printx() lives in its own translation unit (printx.c) for a reason -- see the
 * comment there.  Everything here reaches the output through it.
 */

printbuf(p, n)			/* emit n raw bytes, no formatting */
	char *p;
	int n;
{
	emitout(p, n);
	return (n);
}

flushx()
{
	if (!capturing)
		fflush(outfile ? outfile : stdout);
	return (0);
}

/* ------------------------------------------------- assembler-level output */

/*
 * Register names, keyed to ccom's internal register numbers.
 * See the mapping rationale in gencode.h.
 */
char *rnames[] = {
	/* 0..6 scratch */
	"x9", "x10", "x11", "x12", "x13", "x14", "x15",
	/* 7..16 register variables */
	"x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28",
};
int nrnames = sizeof rnames / sizeof rnames[0];

char *frameptr = "x29";
char *argptr   = "x29";

eobl2()				/* end of function stuff */
{
}

/*
 * ENDJOB: last thing emitted for a translation unit (see macdefs.h).
 *
 * Mach-O has no .size directive, so ld derives a symbol's extent from where the
 * next atom starts -- and only knows atoms exist if the object says so.  With
 * the file's last datum unbounded, ld reported sys_nerr's definition as size 0
 * and warned on every link.  ELF carries sizes explicitly and has no equivalent.
 */
arm64_endjob(status)
	int status;
{
#ifndef ELF_TARGET
	if (status == 0)
		printx("\n.subsections_via_symbols\n");
#endif
	flushx();
	return (0);
}

char *
exname(ix)			/* make a name look external on this machine */
	char *ix;
{
	static char text[256];

	if (ix == NULL) cerror("no name in exname");
#ifdef ELF_TARGET
	/* ELF: no decoration */
	strncpy(text, ix, sizeof text - 1);
	text[sizeof text - 1] = '\0';
#else
	/*
	 * Mach-O prefixes C symbols with an underscore -- which is exactly the
	 * convention PDP-11 and VAX Unix used, so V8's assumption is right here
	 * by accident rather than by porting effort.
	 */
	text[0] = '_';
	strncpy(text + 1, ix, sizeof text - 2);
	text[sizeof text - 1] = '\0';
#endif
	return (text);
}

lineid(l, fn)			/* identify line l of file fn */
	int l;
	char *fn;
{
	printx("// line %d, file %s\n", l, fn);
}

deflab(n)
	int n;
{
	printx("L%d:\n", n);
}

genubr(n)			/* unconditional branch to label n */
	int n;
{
	printx("\tb\tL%d\n", n);
}

/*
 * genret is only the *entry* to the return sequence: it lands on the common
 * return label.  The actual epilogue (frame teardown, callee-saved restores,
 * `ret`) is emitted by efcode in local.c, which is the only place that knows
 * the final frame layout.
 */
genret(s, l, n)
	int s, l, n;
{
	deflab(n);
	if (s) {
		/*
		 * Structure return.  STATSRET: the value lives in a static
		 * area, and we hand back its address.
		 */
		printx("\tadrp\tx0, L%d@PAGE\n", l);
		printx("\tadd\tx0, x0, L%d@PAGEOFF\n", l);
	}
	/* epilogue follows, emitted by efcode */
}

defalign(n)			/* alignment to a multiple of n bits */
	int n;
{
	int p2;

	if (n % SZCHAR) { cerror("funny alignment: %d", n); return; }
	n /= SZCHAR;
	if (n <= 1) return;
	for (p2 = 0; (1 << p2) < n; p2++)
		;
	if ((1 << p2) != n) { cerror("funny alignment: %d", n); return; }
	/* .p2align takes a log2 count on both Mach-O and ELF, unlike .align. */
	printx("\t.p2align\t%d\n", p2);
}

/*
 * String literals go in WRITABLE data, which is not a modern convention but is
 * the 1985 one, and the tree depends on it.
 *
 * V8's own VAX back end puts ISTRNG and STRNG in `.data 2` and `.data 1`
 * (src/cmd/ccom/vax/lcatch2.c) -- numbered sub-segments of writable data, not
 * text.  K&R C had no `const`, and programs of the period wrote to literals
 * freely.  tr(1) is the example that found this: nextc() ends with
 *
 *	if(c==0) *--s->p = 0;
 *
 * pushing the NUL back after reading past it, and `tr -d b` supplies only one
 * string so the other is the literal "".  Emitting into __TEXT,__cstring made
 * that a SIGBUS -- and only for the one-argument forms, since with two
 * arguments both strings are argv and writable.
 *
 * They must be SEPARATE sections, not just writable ones.  ccom switches the
 * location counter mid-datum: for
 *
 *	static char *sccsid = "@(#)cmp.c ...";
 *
 * it emits the label, switches to STRNG for the bytes, then switches back to
 * emit the .quad pointing at them.  With STRNG collapsed onto .data the string
 * landed BETWEEN _sccsid and its own initialiser, and ld rejected cmp.o with
 * "pointer not aligned in '_sccsid'+0x21".  On the VAX the two `.data N`
 * sub-segments kept them apart and the linker concatenated them afterwards;
 * __DATA,__v8str1/__v8str2 do the same job here.
 *
 * Losing the literal coalescing that __TEXT,__cstring gave is correct, not a
 * cost: two identical literals were distinct objects in 1985, and a program
 * that writes to one must not disturb the other.
 */
char *locnames[] = {
	/* location counters: PROG, DATA, ADATA, ISTRNG, STRNG */
	"\t.text\n",
	"\t.data\n",
	"\t.data\n",
#ifdef ELF_TARGET
	"\t.section\t.v8str2,\"aw\",@progbits\n",
	"\t.section\t.v8str1,\"aw\",@progbits\n",
#else
	"\t.section\t__DATA,__v8str2\n",
	"\t.section\t__DATA,__v8str1\n",
#endif
};

bycode(t, i)			/* accumulate bytes of a string constant */
	int t, i;
{
	if (t < 0) {
		printx("\n");
	} else {
		if ((i & 7) == 0) printx("\n\t.byte\t");
		else printx(",");
		printx("0x%x", t);
	}
}

genshort(s)			/* a 16-bit initialiser */
	short s;
{
	printx("\t.short\t%d\n", (int)(short)s);
}

/*
 * A "long" initialiser.  On the VAX this was .long, four bytes, because long
 * was 32 bits.  Under LP64 it is eight, so this must be .quad -- emitting
 * .long here would silently truncate every pointer and long initialiser.
 */
genlong(l)
	long l;
{
	printx("\t.quad\t0x%lx\n", l);
}

genword(l)			/* an explicitly 32-bit initialiser */
	long l;
{
	printx("\t.long\t0x%lx\n", (unsigned long)(l & 0xffffffffL));
}

genswcase(val, lab)		/* one linear switch test */
	long val;
	int lab;
{
	/*
	 * The switch subject is in x0, not a scratch register: cgram.y assigns
	 * it to the SNODE pseudo-register, and SNODE -- like RNODE and QNODE --
	 * lives in x0 by the callreg() convention (mfile2.h).  Comparing a
	 * scratch register here instead made every case fall to the default,
	 * which showed up as printf printing "int=d" for "int=%d": _doprnt's
	 * conversion switch never matched and its default echoed the letter.
	 *
	 * cmp takes a 12-bit unsigned immediate; anything wider has to be
	 * materialised first.
	 */
	if (val >= 0 && val < 4096)
		printx("\tcmp\tx0, #%ld\n", val);
	else {
		printx("\tmov\tx9, #%ld\n", val);
		printx("\tcmp\tx0, x9\n");
	}
	printx("\tb.eq\tL%d\n", lab);
}

/* ------------------------------------------------ prologue and epilogue */

/*
 * ARM64 frame construction.  See the layout diagram in local.c.
 *
 * The order in the prologue matters: the argument spill block is allocated
 * BEFORE x29/x30 are pushed, so the eight spilled registers end up immediately
 * below the caller's stack arguments and the two form one contiguous block for
 * varargs.h to walk.
 */

#define ARGSPILL 64		/* x0..x7, eight 8-byte slots */

extern void arm64_resetcalls();
extern long arm64_callarea();

void
arm64_beginfunction()
{
	arm64_resetcalls();
	emitstart();		/* capture the body; prologue comes later */
}

/*
 * sp adjustment, honouring TWO immediate limits, not one.
 *
 * add/sub take a 12-bit immediate, so anything from 4096 up has to go through a
 * register.  That much was here from the start.  What was missing is that `mov'
 * has a limit of its own: it assembles to MOVZ, which carries a 16-bit
 * immediate shifted by 0, 16, 32 or 48 -- so it can name 65535 and it cannot
 * name 65536.  A frame between those did not produce bad code; it produced code
 * the ASSEMBLER refused, which is the good direction to fail in but only
 * because clang happens to be strict:
 *
 *	mov x16, #65984
 *	    ^ error: expected compatible register or logical immediate
 *
 * Found by writing a test program with a 64 KB buffer on the stack, which is
 * the first thing in this port to want one.  MOVK supplies the upper half.
 */
static void
spadjust(op, n)
	char *op;
	long n;
{
	if (n == 0) return;
	if (n < 4096) {
		printx("\t%s\tsp, sp, #%ld\n", op, n);
		return;
	}
	/* x16 (IP0) is reserved for exactly this kind of scratch use */
	printx("\tmov\tx16, #%ld\n", n & 0xffffL);
	if (n > 0xffffL)
		printx("\tmovk\tx16, #%ld, lsl #16\n", (n >> 16) & 0xffffL);
	if (n > 0xffffffffL) {
		printx("\tmovk\tx16, #%ld, lsl #32\n", (n >> 32) & 0xffffL);
		printx("\tmovk\tx16, #%ld, lsl #48\n", (n >> 48) & 0xffffL);
	}
	printx("\t%s\tsp, sp, x16\n", op);
}

void
arm64_endfunction(framebytes, minrv)
	long framebytes;
	int minrv;
{
	char *body;
	long bodylen;
	long locals, callarea;
	int r, nsaved;

	body = emitcaptured(&bodylen);
	emitstop();

	/*
	 * The frame carries the locals AND the call areas -- the outgoing
	 * arguments and the caller-save blocks that gencall() addresses at
	 * fixed offsets from sp.  Reserving them here is what lets sp stay put
	 * for the whole body; see the note above gencall() for why moving it
	 * per call was wrong.
	 *
	 * Capturing the body first is what makes this possible: by now every
	 * call in the function has been generated and the deepest nesting is
	 * known.
	 *
	 * The two areas are rounded SEPARATELY, because the callee-saved
	 * registers go between them -- see the ORDER note below -- so each has
	 * to leave sp 16-aligned on its own.
	 */
	locals   = (framebytes + 15) & ~15L;
	callarea = (arm64_callarea() + 15) & ~15L;

	/*
	 * Callee-saved registers actually used.  cisreg() hands out register
	 * variables counting down from RVARLAST_P1, and minrvar records how far
	 * it got, so everything from minrv+1 through RVARLAST_P1 is live.
	 */
	nsaved = RVARLAST_P1 - minrv;
	if (nsaved < 0) nsaved = 0;

	/* ---- prologue ---- */
	printx("\tsub\tsp, sp, #%d\n", ARGSPILL);
	printx("\tstp\tx0, x1, [sp, #0]\n");
	printx("\tstp\tx2, x3, [sp, #16]\n");
	printx("\tstp\tx4, x5, [sp, #32]\n");
	printx("\tstp\tx6, x7, [sp, #48]\n");
	printx("\tstp\tx29, x30, [sp, #-16]!\n");
	printx("\tmov\tx29, sp\n");

	/*
	 * ORDER, AND IT HAS THREE PARTS RATHER THAN TWO.  Top to bottom:
	 *
	 *	x29 ->	saved x29/x30
	 *		locals			[x29, #-16] downward
	 *		callee-saved registers
	 *	sp  ->	call area		[sp, #0] upward
	 *
	 * The saves cannot go IMMEDIATELY BELOW x29, because pass 1's oalloc()
	 * hands out automatics as negative offsets from the frame pointer under
	 * BACKAUTO, so every save would land on a local.  That was the first
	 * version of this code, and the first real libc function (fputs) died
	 * on it.
	 *
	 * They cannot go AT THE BOTTOM either, which is what the fix for that
	 * did, and the second collision took much longer to find because
	 * nothing in the tree provoked it.  AAPCS64 puts the ninth and later
	 * arguments of a call at [sp, #0] -- gencode.c's argslot() says exactly
	 * that -- and [sp, #0] is where the LAST save lands.  So a function that
	 * both used register variables and called something with more than eight
	 * arguments overwrote its own saved register with an outgoing argument,
	 * and handed the corrupted value back to its CALLER on return.
	 *
	 * Nothing hit it for 156 Wave A programs plus all of Wave B and C,
	 * because no call in any of them has nine arguments.  printp() in ps(1)
	 * does: sprintf with a format and seven values.  The symptom was ps
	 * walking off the end of its /proc directory array, because main()'s
	 * `dp' was the register that came back holding a char *.
	 *
	 * So: locals, then the saves, then the call area beneath them.
	 */
	spadjust("sub", locals);

	for (r = 0; r < nsaved; r++)
		printx("\tstr\t%s, [sp, #-16]!\n", rnames[minrv + 1 + r]);

	spadjust("sub", callarea);

	/* ---- body ---- */
	if (body && bodylen > 0)
		printbuf(body, (int)bodylen);

	/* ---- epilogue (control arrives here via the retlab in genret) ---- */
	/* mirror the prologue exactly, in reverse */
	spadjust("add", callarea);

	for (r = nsaved - 1; r >= 0; r--)
		printx("\tldr\t%s, [sp], #16\n", rnames[minrv + 1 + r]);

	spadjust("add", locals);

	printx("\tldp\tx29, x30, [sp], #16\n");
	printx("\tadd\tsp, sp, #%d\n", ARGSPILL);
	printx("\tret\n");
}
