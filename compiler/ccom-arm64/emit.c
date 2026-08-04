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
# include <stdlib.h>
# include <string.h>
# include "mfile2.h"
# include "gencode.h"

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

char *locnames[] = {
	/* location counters: PROG, DATA, ADATA, ISTRNG, STRNG */
	"\t.text\n",
	"\t.data\n",
	"\t.data\n",
	"\t.section\t__TEXT,__cstring\n",
	"\t.section\t__TEXT,__cstring\n",
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
	 * cmp accepts a 12-bit unsigned immediate (optionally shifted by 12).
	 * Anything else has to be materialised into a scratch register first.
	 */
	if (val >= 0 && val < 4096)
		printx("\tcmp\tx9, #%ld\n", val);
	else {
		printx("\tmov\tx10, #%ld\n", val);
		printx("\tcmp\tx9, x10\n");
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

void
arm64_beginfunction()
{
	emitstart();		/* capture the body; prologue comes later */
}

/* sp adjustment, honouring the 12-bit immediate limit on add/sub. */
static void
spadjust(op, n)
	char *op;
	long n;
{
	if (n == 0) return;
	if (n < 4096)
		printx("\t%s\tsp, sp, #%ld\n", op, n);
	else {
		/* x16 (IP0) is reserved for exactly this kind of scratch use */
		printx("\tmov\tx16, #%ld\n", n);
		printx("\t%s\tsp, sp, x16\n", op);
	}
}

void
arm64_endfunction(framebytes, minrv)
	long framebytes;
	int minrv;
{
	char *body;
	long bodylen;
	long frame;
	int r, nsaved;

	body = emitcaptured(&bodylen);
	emitstop();

	/* Round the frame to 16, which AAPCS64 requires of sp at all times. */
	frame = (framebytes + 15) & ~15L;

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

	for (r = 0; r < nsaved; r++)
		printx("\tstr\t%s, [sp, #-16]!\n", rnames[minrv + 1 + r]);

	spadjust("sub", frame);

	/* ---- body ---- */
	if (body && bodylen > 0)
		printbuf(body, (int)bodylen);

	/* ---- epilogue (control arrives here via the retlab in genret) ---- */
	spadjust("add", frame);

	for (r = nsaved - 1; r >= 0; r--)
		printx("\tldr\t%s, [sp], #16\n", rnames[minrv + 1 + r]);

	printx("\tldp\tx29, x30, [sp], #16\n");
	printx("\tadd\tsp, sp, #%d\n", ARGSPILL);
	printx("\tret\n");
}
