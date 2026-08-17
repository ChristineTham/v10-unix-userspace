/*
 * f77probe -- the consumer libF77 and libI77 do not have yet.
 *
 * There is no Fortran compiler in this tree, so the runtime has no caller, and
 * CLAUDE.md's rule for that is not "wait": an unconsumed component invents a
 * difference nobody can see, and when the gap is a missing CONSUMER rather
 * than a missing case, a probe is the instrument.  tests/wavea/tgotoprobe.c is
 * the precedent -- libtermcap's largest member had no consumer either, and
 * deleting four -D flags from it failed nothing at all until a probe existed.
 *
 * WHAT THIS EXISTS TO PROVE, in order of importance:
 *
 * 1. THAT IT LINKS.  V8's shipped /usr/lib/libI77.a has two symbols that
 *    resolve in no archive Bell Labs shipped -- setvbuf and _bufendtab,
 *    measured across every .a in the distribution -- so no Fortran program on
 *    a real V8 could reach ld's exit.  A C program that supplies MAIN__ and
 *    links -lF77 -lI77 is the smallest thing that can say the port closed it.
 *
 * 2. THAT _bufend WORKS.  wrtfmt.o and wsfe.o are built with
 *    -D_bufend='(unsigned char *)v8_bufend', and those two objects are the
 *    formatted-write path -- so any WRITE with a format goes through the
 *    rename.  Both sites are reached only when the T/TL edit descriptors or
 *    record padding move the cursor BACKWARD, which is why the format below
 *    uses T explicitly rather than trusting a plain write to get there.
 *
 * 3. THAT THE FORTRAN CALLING CONVENTION SURVIVES, AT THE RIGHT WIDTH.  Every
 *    argument is by reference and every character argument carries a hidden
 *    trailing length, so s_cmp(a, b, la, lb) is four arguments for two strings.
 *    The lengths are ftnlen, and this file used to pass them as `2L' because
 *    fio.h spelled the typedef `long' -- EIGHT bytes under v8cc where a VAX
 *    gave four.  f77's machdefs pins SZLONG at 4 (typesize[TYREAL] is SZLONG,
 *    and lengtype() at proc.c:951 hardcodes INTEGER*4 -> TYLONG), so the
 *    library was narrowed to match and the lengths here are plain ints.  The
 *    width is PRINTED at the bottom and the suite checks it against SZLENG in
 *    the generated arm64defs, which is a third thing that is neither end.
 *
 * WHY IT PROVIDES MAIN__ RATHER THAN main.  libF77/main.c defines main(), sets
 * up five signal handlers, and calls f_init(), MAIN__(), f_exit() in that
 * order.  So the probe is shaped like a Fortran program -- the library owns
 * the entry point -- which also means f_init() runs before anything here does,
 * exercising err.c's setvbuf calls against the shim without asking for them.
 *
 * Output is one token per check on a single line, so the suite can compare the
 * whole thing at once and a wrong answer names itself.
 */

#include <stdio.h>

/* int, not long, and the probe is the only thing that can check it.  f77's
   machdefs pins SZLONG at 4 -- typesize[TYREAL] is SZLONG and libF77's r_nint
   takes `float *', and lengtype() (proc.c:951) hardcodes INTEGER*4 -> TYLONG --
   so a Fortran INTEGER and a hidden character length are four bytes.  V8's own
   compiler agreed: `# define NOLONG' at ccom/vax/macdefs.h:20 made C's long 32
   bits, which is why libI77's fio.h spells these `long' and was right to.
   Task #12.  The width case at the bottom asserts the two ends still agree. */
typedef int ftnint;
typedef ftnint flag;
typedef int ftnlen;

/* fio.h's cilist, respelled rather than included: including "fio.h" would need
   -I into libI77's source directory, and that is the one thing the build of
   this library refuses to do -- libI77 ships a System V stdio.h that disagrees
   with V8's about the layout of FILE.  The struct is five fields and copying
   it is cheaper than reopening that decision for a test program. */
typedef struct {
	flag	cierr;
	ftnint	ciunit;
	flag	ciend;
	char	*cifmt;
	ftnint	cirec;
} cilist;

extern int s_wsfe(), do_fio(), e_wsfe();
extern int s_cmp(), i_indx();
extern int pow_ii(), i_nint(), i_mod();	/* PORT: int, per task #12 */
extern double pow_dd(), d_mod(), d_nint();

int	nfail;

/* int, not long, and getting this wrong produced the one failure the narrowing
   caused.  A Fortran INTEGER is four bytes now, so an intrinsic returns one in
   w0 -- and any write to a w register zeroes bits 63:32 architecturally, so a
   negative result comes back ZERO-extended.  Widening it to a long here read
   i_nint(-2.5) as 4294967293.  A Fortran caller never sees that, because it
   assigns the result into a four-byte INTEGER; only a C caller that widens
   does.  So the probe compares at the width the convention actually has. */
static
check(what, got, want)
char *what;
int got, want;
{
	if (got != want) {
		fprintf(stderr, "f77probe: %s: want %d got %d\n",
			what, want, got);
		nfail++;
	}
}

MAIN__()
{
	cilist io;
	char buf[16];
	int one, two, three, n;	/* PORT: FORTRAN INTEGER is four bytes; see check() */
	double x, y;
	float r;

	/*
	 * libF77: the character intrinsics.  Each takes its strings by
	 * reference with the length passed separately, and s_cmp's answer is
	 * the sign of the comparison after blank-padding the shorter one --
	 * which is why "ab" and "ab  " must compare EQUAL.  A C strcmp would
	 * say otherwise, so this distinguishes the Fortran routine from libc's.
	 */
	check("s_cmp equal",    (int) s_cmp("ab", "ab", 2, 2), 0);
	check("s_cmp padded",   (int) s_cmp("ab", "ab  ", 2, 4), 0);
	check("s_cmp less",     (int) s_cmp("ab", "ac", 2, 2) < 0, 1);
	/* INDEX is 1-based and 0 for absent, not -1. */
	check("i_indx found",   i_indx("hello", "ll", 5, 2), 3);
	check("i_indx absent",  i_indx("hello", "zz", 5, 2), 0);

	/* libF77: arithmetic.  pow_ii is integer**integer by repeated
	   squaring, and its result is a long -- the return-width case. */
	two = 2; three = 3; one = 1;
	check("pow_ii 2**10",   (n = 10, pow_ii(&two, &n)), 1024);
	check("pow_ii x**0",    (n = 0, pow_ii(&three, &n)), 1);
	/* MOD keeps the sign of the first operand, unlike a mathematical
	   modulus; -7 mod 3 is -1 in Fortran. */
	n = -7; check("i_mod sign", i_mod(&n, &three), -1);

	x = 2.0; y = 10.0;
	check("pow_dd 2**10",   (int) pow_dd(&x, &y), 1024);
	x = 7.5; y = 2.0;
	check("d_mod",          (int) (d_mod(&x, &y) * 10.0), 15);
	/* NINT rounds half AWAY from zero, which is not what a cast does. */
	x = 2.5;  check("d_nint 2.5",  (int) d_nint(&x), 3);
	x = -2.5; check("d_nint -2.5", (int) d_nint(&x), -3);
	/* i_nint takes a REAL and d_nint a DOUBLE PRECISION, and the prefix
	   letter is the only thing that says so -- i_nint.c declares `float *x'
	   where d_nint.c declares `double *x'.  Handing i_nint the address of a
	   double reads its low four bytes as a float, which for 2.5 are zero,
	   so it returns 0 and looks like a broken library.  That is how this
	   line was first written and what it measured was the probe. */
	r = 2.5;  check("i_nint 2.5",  i_nint(&r), 3);
	r = -2.5; check("i_nint -2.5", i_nint(&r), -3);

	/*
	 * libI77: a formatted WRITE to unit 6, which is what f77's io.c emits
	 * for `WRITE(6,10)'.  This is the whole point of the probe -- it is the
	 * only path that reaches wsfe.o and wrtfmt.o, hence the only thing that
	 * can say whether -D_bufend=v8_bufend produced a working comparison or
	 * a truncated pointer.
	 *
	 * The format uses T to move the cursor BACKWARD over a record already
	 * written, because _bufend's three call sites are all inside the "can I
	 * skip inside the buffer, or must I fseek?" test and a purely forward
	 * write never reaches them.  Writing 'AB' at columns 1-2, then T1 to
	 * return to column 1 and overwrite with 'X', yields XB.
	 */
	io.cierr = 0; io.ciunit = 6; io.ciend = 0; io.cirec = 0;
	io.cifmt = "(A2,T1,A1)";
	s_wsfe(&io);
	n = 1;
	do_fio(&n, "AB", 2);
	do_fio(&n, "X", 1);
	e_wsfe();

	/* A second record, plain, so the suite can tell a working write from a
	   library that produced nothing at all. */
	io.cifmt = "(A5)";
	s_wsfe(&io);
	n = 1;
	do_fio(&n, "plain", 5);
	e_wsfe();

	buf[0] = 0;
	/* THE WIDTH, printed rather than assumed.  The suite compares it against
	   SZLENG read out of the generated arm64defs, so the two ends of the
	   Fortran ABI are checked against each other and against f77's own
	   machine description -- three things, none of them transcribed. */
	printf("ftnlen %d ftnint %d\n", (int)sizeof(ftnlen), (int)sizeof(ftnint));
	printf("checks %s\n", nfail == 0 ? "ok" : "FAILED");
	fflush(stdout);
}
