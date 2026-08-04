/*
 * IEEE 754 replacements for the floating-point primitives V8 wrote in VAX
 * assembly: modf, frexp, ldexp and fabs.
 *
 * The originals did bitfield surgery on the VAX's F/D formats -- `extzv $7,$8`
 * to pull an 8-bit excess-128 exponent out of bit 7, `insv` to force it back,
 * `emodd` for modf, and a hand-built `.word 0x7fff,0xffff,0xffff,0xffff` as the
 * D-format maximum "given in hex in order to avoid floating conversions".  None
 * of that means anything in IEEE, where the exponent is 11 bits at bit 52 with
 * a bias of 1023.
 *
 * These are written against the bit layout rather than in terms of each other,
 * so they stay exact: ecvt/fcvt build every printed floating value on modf, and
 * a rounding error here would show up in the last digit of every %f in the
 * system.
 */

/*
 * Written in the C V8's compiler speaks: no `long long` (there is no such type
 * in 1985), no ULL suffixes, K&R parameter declarations.  Under LP64 a plain
 * `long` is already the 64 bits an IEEE double needs.
 */
union dbits {
	double d;
	unsigned long u;
};

#define EXPMASK	0x7ff
#define EXPBIAS	1023
#define EXPSHIFT 52
#define MANTMASK 0x000fffffffffffff
#define SIGNBIT	0x8000000000000000

double
fabs(x)
	double x;
{
	union dbits v;

	v.d = x;
	v.u &= ~SIGNBIT;
	return (v.d);
}

/*
 * modf: split into integer and fraction, both with the sign of x.  Returns the
 * fraction and stores the integer part through iptr.
 *
 * Done by masking rather than by subtraction: (x - floor(x)) loses precision
 * for large x, and ecvt calls this on every value it prints.
 */
double
modf(x, iptr)
	double x;
	double *iptr;
{
	union dbits v;
	int e;
	unsigned long fracmask;

	v.d = x;
	e = (int)((v.u >> EXPSHIFT) & EXPMASK) - EXPBIAS;

	if (e < 0) {
		/* |x| < 1: all fraction, integer part is a signed zero */
		union dbits z;
		z.u = v.u & SIGNBIT;
		*iptr = z.d;
		return (x);
	}
	if (e >= 52) {
		/* no fractional bits left, or an inf/nan */
		*iptr = x;
		v.u &= SIGNBIT;
		return (v.d);
	}

	fracmask = MANTMASK >> e;
	if ((v.u & fracmask) == 0) {	/* already an integer */
		*iptr = x;
		v.u &= SIGNBIT;
		return (v.d);
	}
	{
		union dbits ip;
		ip.u = v.u & ~fracmask;
		*iptr = ip.d;
		return (x - ip.d);
	}
}

/*
 * frexp: return a fraction in [0.5,1) and the binary exponent, so that
 * x == fraction * 2**exponent.
 */
double
frexp(x, eptr)
	double x;
	int *eptr;
{
	union dbits v;
	int e;

	v.d = x;
	e = (int)((v.u >> EXPSHIFT) & EXPMASK);

	if (e == 0) {
		if ((v.u & ~SIGNBIT) == 0) { *eptr = 0; return (x); }
		/*
		 * Subnormal.  Scale it into the normal range by multiplying by
		 * 2**64, then take the exponent back off -- much less code than
		 * counting leading zeros by hand, and exact either way.
		 */
		v.d = x * 18446744073709551616.0;	/* 2**64 */
		e = (int)((v.u >> EXPSHIFT) & EXPMASK) - 64;
	}
	if (e == (int)EXPMASK) { *eptr = 0; return (x); }	/* inf or nan */

	*eptr = e - EXPBIAS + 1;
	v.u = (v.u & ~((unsigned long)EXPMASK << EXPSHIFT)) |
	      ((unsigned long)(EXPBIAS - 1) << EXPSHIFT);
	return (v.d);
}

/*
 * ldexp: x * 2**n.
 *
 * The VAX version set errno to ERANGE (34) inline on overflow and returned its
 * hand-built maximum; that behaviour is kept, since V8 code checks errno here.
 */
extern int errno;
#define V8_ERANGE 34

double
ldexp(x, n)
	double x;
	int n;
{
	union dbits v;
	int e;

	v.d = x;
	if ((v.u & ~SIGNBIT) == 0) return (x);		/* +/-0 */
	e = (int)((v.u >> EXPSHIFT) & EXPMASK);
	if (e == (int)EXPMASK) return (x);		/* inf or nan */

	if (e == 0) {					/* subnormal */
		v.d = x * 18446744073709551616.0;
		e = (int)((v.u >> EXPSHIFT) & EXPMASK) - 64;
	}
	e += n;

	if (e >= (int)EXPMASK) {			/* overflow */
		errno = V8_ERANGE;
		v.u = (v.u & SIGNBIT) | ((unsigned long)(EXPMASK - 1) << EXPSHIFT)
		    | MANTMASK;
		return (v.d);
	}
	if (e <= 0) {					/* underflow to zero */
		v.u &= SIGNBIT;
		return (v.d);
	}
	v.u = (v.u & ~((unsigned long)EXPMASK << EXPSHIFT)) |
	      ((unsigned long)e << EXPSHIFT);
	return (v.d);
}
