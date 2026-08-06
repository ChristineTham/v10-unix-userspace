/*
 * atof, replacing the 319 lines of VAX assembly in libc/gen/atof.s.
 *
 * The original is a D-format routine and unportable to its bones: it builds the
 * result with `muld2`/`addd2` against a table of powers held as `.double`
 * constants, tests the VAX's own overflow trap, and returns in r0/r1 as a
 * 64-bit D float with an 8-bit excess-128 exponent.  None of that survives the
 * move to IEEE, so this joins doprnt.c and ieeefp.c on the short list of files
 * that replace VAX assembly rather than porting it.
 *
 * Written in the C V8's compiler speaks: K&R parameters, no `long long`.
 *
 * WHY IT IS HERE AT ALL, since everything worked without it.  seq(1) declares
 * `extern double atof()' and nothing in libv8c defined one, so the link quietly
 * took Apple's -- the bug class CLAUDE.md names, found this time by tests/kmemu
 * asserting that no V8 binary imports anything from libc.  It gave right
 * answers, which is exactly why it survived 156 Wave A programs unnoticed.
 *
 * ACCURACY, and where the line is drawn.  The mantissa is accumulated as an
 * integer and scaled once, rather than multiplied into a double digit by digit,
 * so error does not compound across the string.  When the integer is exact in a
 * double and the exponent is small, ONE multiply or divide by an exact power of
 * ten is correctly rounded, and that covers essentially everything a program in
 * this tree parses -- 0.5, -1.25, 1.5e-5, 42.75 all come back bit-identical to
 * Apple's atof.  Outside that window (more than 17 significant digits, or an
 * exponent past 22) the powers are combined by binary exponentiation and the
 * result can be an ulp or so out.  That is better than the VAX routine managed
 * and short of a correctly-rounded parser; a port whose printf is V8's own does
 * not need one, and writing one would be inventing something this library never
 * had.
 *
 * What is NOT allowed to be approximate is overflow.  An earlier version here
 * clamped the exponent to 308 to bound the loop, which turned atof("1e400")
 * into 1.0000000000000007e+308 -- a finite, plausible, wrong number where the
 * answer is infinity.  Measured against the host's atof, which is the only
 * reason it was caught.  The cap is now past the table's reach, so an exponent
 * that should overflow does overflow, and one that should underflow reaches 0.
 */

/* Exact in IEEE double up to 1e22; past that a power of ten is not
 * representable and the fast path below must not be taken. */
static double pow10[] = {
	1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
	1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
	1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};
#define MAXEXACT 22

/* For binary exponentiation: 1e1, 1e2, 1e4, ... 1e256. */
static double bit10[] = {
	1e1, 1e2, 1e4, 1e8, 1e16, 1e32, 1e64, 1e128, 1e256
};

static double
scale10(v, e)
	double v;
	register int e;
{
	register int i;
	int neg = 0;

	if (e < 0) { neg = 1; e = -e; }
	/*
	 * 511 is everything bit10[] can express, so anything larger has already
	 * overflowed (or underflowed) by the time it is reached; clamping here
	 * bounds the loop without changing any answer.
	 */
	if (e > 511) e = 511;
	for (i = 0; e; i++, e >>= 1)
		if (e & 1) {
			if (neg) v /= bit10[i];
			else v *= bit10[i];
		}
	return (v);
}

double
atof(p)
	register char *p;
{
	double v;
	long mant;
	int ndig, dexp, eexp, sign, esign, any;

	while (*p == ' ' || *p == '\t' || *p == '\n') p++;
	sign = 0;
	if (*p == '-') { sign = 1; p++; }
	else if (*p == '+') p++;

	/*
	 * Digits go into a long until it would overflow; after that they still
	 * count towards the exponent but are dropped.  18 is the most a signed
	 * 64-bit integer holds for certain, and a double carries fewer than 16
	 * significant decimal digits anyway.
	 */
	mant = 0;
	ndig = 0;
	dexp = 0;
	any = 0;
	for (; *p >= '0' && *p <= '9'; p++) {
		any = 1;
		if (ndig < 18) { mant = mant * 10 + (*p - '0'); ndig++; }
		else dexp++;
	}
	if (*p == '.') {
		p++;
		for (; *p >= '0' && *p <= '9'; p++) {
			any = 1;
			if (ndig < 18) {
				mant = mant * 10 + (*p - '0');
				ndig++;
				dexp--;
			}
		}
	}
	if (!any) return (0.0);			/* no digits: not a number */

	if (*p == 'e' || *p == 'E') {
		register char *q = p + 1;

		esign = 0;
		if (*q == '-') { esign = 1; q++; }
		else if (*q == '+') q++;
		if (*q >= '0' && *q <= '9') {
			eexp = 0;
			for (; *q >= '0' && *q <= '9'; q++) {
				if (eexp < 30000) eexp = eexp * 10 + (*q - '0');
			}
			dexp += esign ? -eexp : eexp;
			p = q;
		}
		/* An `e' with no digits after it is not part of the number;
		 * leaving p where it was is what strtod would do, and atof has
		 * no way to report where it stopped anyway. */
	}

	/*
	 * The exact case: 18 decimal digits can exceed the 53 bits a double
	 * holds, so `ndig < 18' is not enough -- the integer itself has to be
	 * back-convertible.  When it is, and the scale is an exact power of
	 * ten, the single operation below is correctly rounded and this agrees
	 * with the host's atof bit for bit.
	 */
	v = (double)mant;
	if ((long)v == mant && dexp <= MAXEXACT && dexp >= -MAXEXACT)
		v = dexp >= 0 ? v * pow10[dexp] : v / pow10[-dexp];
	else
		v = scale10(v, dexp);
	return (sign ? -v : v);
}
