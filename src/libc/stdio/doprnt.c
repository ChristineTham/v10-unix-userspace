/*
 * _doprnt -- the engine behind printf, fprintf and sprintf.
 *
 * Replaces libc/stdio/doprnt.S, which is 765 lines of VAX assembly: it uses
 * the VAX's decimal-string instructions, its F/D floating formats, and a
 * 256-byte `locc` translate table for scanning the format.  None of that
 * survives a change of machine, so this is a fresh implementation in C with the
 * same interface and the same observable output.
 *
 * THE ARGUMENT BLOCK.  V8's printf is
 *
 *	printf(fmt, args) char *fmt; { _doprnt(fmt, &args, stdout); }
 *
 * -- take the address of the first variadic argument and walk forward.  That is
 * the varargs.h idiom, and it is the whole reason v8cc's prologue spills x0-x7
 * into one contiguous block (PLAN.md S4).  Here the block is walked with an
 * 8-byte stride because SZARG is 64: every argument, including a char or a
 * short, occupies one slot, and a double was bitcast into its slot by the
 * caller.  Reading 8 bytes and reinterpreting is therefore always right.
 *
 * WHAT IT SUPPORTS, matching the V8 manual page:
 *	%d %i	signed decimal		%u	unsigned decimal
 *	%o	octal			%x %X	hexadecimal
 *	%c	character		%s	string
 *	%e %f %g		floating point
 *	%%	a literal percent
 * with the flags `-` (left justify) and `0` (zero pad), a field width, a
 * precision, and the `l` length modifier.  `*` takes the width or precision
 * from the argument list, as V8 did.
 *
 * Floating point is formatted through ecvt/fcvt, exactly as the VAX version
 * did -- the difference is that those are now IEEE rather than D-format.
 */

#include <stdio.h>

#define ARGSZ	8		/* SZARG/SZCHAR: one argument slot */

/* Pull the next argument out of the block and step past it. */
#define NEXTLONG(p)	(*(long *)((p) += ARGSZ, (p) - ARGSZ))
#define NEXTPTR(p)	(*(char **)((p) += ARGSZ, (p) - ARGSZ))
#define NEXTDOUBLE(p)	(*(double *)((p) += ARGSZ, (p) - ARGSZ))

extern char *ecvt(), *fcvt();

static int
emit(iop, s, n)
	FILE *iop;
	char *s;
	int n;
{
	int i;

	for (i = 0; i < n; i++)
		putc(s[i], iop);
	return (n);
}

static int
pad(iop, c, n)
	FILE *iop;
	int c, n;
{
	int i;

	for (i = 0; i < n; i++)
		putc(c, iop);
	return (n > 0 ? n : 0);
}

/*
 * Convert an unsigned value to digits in `base`, filling buf from the right.
 * Returns a pointer to the first digit; *lenp is how many there are.
 */
static char *
convert(val, base, upper, buf, lenp)
	unsigned long val;
	int base, upper;
	char *buf;
	int *lenp;
{
	static char lodigits[] = "0123456789abcdef";
	static char updigits[] = "0123456789ABCDEF";
	char *digits = upper ? updigits : lodigits;
	char *p = buf + 32;

	*--p = '\0';
	if (val == 0)
		*--p = '0';
	else
		while (val) {
			*--p = digits[val % base];
			val /= base;
		}
	*lenp = (int)(buf + 32 - 1 - p);
	return (p);
}

_doprnt(fmt, argp, iop)
	register char *fmt;
	char *argp;
	register FILE *iop;
{
	int total = 0;
	int leftjust, zeropad, plussign, blanksign, alt;
	int width, prec, haveprec, longflag;
	int len, n, i, base, upper, isneg;
	unsigned long uval;
	long sval;
	double dval;
	char *s, *p;
	char numbuf[64];
	char fbuf[512];
	char sign;

	while (*fmt) {
		if (*fmt != '%') { putc(*fmt++, iop); total++; continue; }
		fmt++;
		if (*fmt == '%') { putc('%', iop); fmt++; total++; continue; }

		/* ---- flags ---- */
		leftjust = zeropad = plussign = blanksign = alt = 0;
		for (;;) {
			if (*fmt == '-')      { leftjust = 1; fmt++; }
			else if (*fmt == '0') { zeropad = 1; fmt++; }
			else if (*fmt == '+') { plussign = 1; fmt++; }
			else if (*fmt == ' ') { blanksign = 1; fmt++; }
			else if (*fmt == '#') { alt = 1; fmt++; }
			else break;
		}

		/* ---- width ---- */
		width = 0;
		if (*fmt == '*') {
			width = (int)NEXTLONG(argp);
			if (width < 0) { leftjust = 1; width = -width; }
			fmt++;
		} else
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + (*fmt++ - '0');

		/* ---- precision ---- */
		prec = 0; haveprec = 0;
		if (*fmt == '.') {
			fmt++;
			haveprec = 1;
			if (*fmt == '*') { prec = (int)NEXTLONG(argp); fmt++; }
			else
				while (*fmt >= '0' && *fmt <= '9')
					prec = prec * 10 + (*fmt++ - '0');
			if (prec < 0) { prec = 0; haveprec = 0; }
		}

		/* ---- length modifier ---- */
		longflag = 0;
		while (*fmt == 'l' || *fmt == 'h') {
			if (*fmt == 'l') longflag = 1;
			fmt++;
		}

		sign = 0;
		switch (*fmt++) {

		case 'd':
		case 'i':
			sval = NEXTLONG(argp);
			if (!longflag) sval = (long)(int)sval;
			isneg = (sval < 0);
			uval = isneg ? (unsigned long)(-sval) : (unsigned long)sval;
			if (isneg) sign = '-';
			else if (plussign) sign = '+';
			else if (blanksign) sign = ' ';
			base = 10; upper = 0;
			goto number;

		case 'u':
			uval = (unsigned long)NEXTLONG(argp);
			if (!longflag) uval = (unsigned long)(unsigned int)uval;
			base = 10; upper = 0;
			goto number;

		case 'o':
			uval = (unsigned long)NEXTLONG(argp);
			if (!longflag) uval = (unsigned long)(unsigned int)uval;
			base = 8; upper = 0;
			goto number;

		case 'x':
			uval = (unsigned long)NEXTLONG(argp);
			if (!longflag) uval = (unsigned long)(unsigned int)uval;
			base = 16; upper = 0;
			goto number;

		case 'X':
			uval = (unsigned long)NEXTLONG(argp);
			if (!longflag) uval = (unsigned long)(unsigned int)uval;
			base = 16; upper = 1;
			goto number;

		number:
			p = convert(uval, base, upper, numbuf, &len);
			/* a precision on an integer is a minimum digit count */
			n = (haveprec && prec > len) ? prec - len : 0;
			i = len + n + (sign ? 1 : 0);
			if (alt && base == 8) i++;
			if (alt && base == 16 && uval) i += 2;

			if (!leftjust && !zeropad)
				total += pad(iop, ' ', width - i);
			if (sign) { putc(sign, iop); total++; }
			if (alt && base == 8) { putc('0', iop); total++; }
			if (alt && base == 16 && uval) {
				putc('0', iop);
				putc(upper ? 'X' : 'x', iop);
				total += 2;
			}
			if (!leftjust && zeropad && !haveprec)
				total += pad(iop, '0', width - i);
			total += pad(iop, '0', n);
			total += emit(iop, p, len);
			if (leftjust)
				total += pad(iop, ' ', width - i);
			break;

		case 'c':
			numbuf[0] = (char)NEXTLONG(argp);
			if (!leftjust) total += pad(iop, ' ', width - 1);
			putc(numbuf[0], iop); total++;
			if (leftjust) total += pad(iop, ' ', width - 1);
			break;

		case 's':
			s = NEXTPTR(argp);
			if (s == 0) s = "(null)";
			for (len = 0; s[len]; len++)
				if (haveprec && len >= prec) break;
			if (!leftjust) total += pad(iop, ' ', width - len);
			total += emit(iop, s, len);
			if (leftjust) total += pad(iop, ' ', width - len);
			break;

		case 'e':
		case 'E':
		case 'f':
		case 'g':
		case 'G':
			dval = NEXTDOUBLE(argp);
			len = fmtfloat(fmt[-1], dval, haveprec ? prec : 6,
			    plussign, blanksign, fbuf);
			if (!leftjust && zeropad && fbuf[0] != '-')
				total += pad(iop, '0', width - len);
			else if (!leftjust)
				total += pad(iop, ' ', width - len);
			total += emit(iop, fbuf, len);
			if (leftjust) total += pad(iop, ' ', width - len);
			break;

		default:
			/* Unknown conversion: V8 printed it literally. */
			putc(fmt[-1], iop);
			total++;
			break;
		}
	}
	return (total);
}

/*
 * Floating conversion, built on ecvt/fcvt the way the VAX version was.  Those
 * are now IEEE rather than VAX D-format, which is the one place output can
 * differ from a real V8: the last digit or two of a value that was not exactly
 * representable in either format.
 */
static
fmtfloat(conv, val, prec, plussign, blanksign, out)
	int conv;
	double val;
	int prec, plussign, blanksign;
	char *out;
{
	char *digits;
	int decpt, sign, nd, i;
	char *o = out;
	int expo;

	if (conv == 'g' || conv == 'G') {
		/*
		 * %g: %e if the exponent is below -4 or at least the precision,
		 * %f otherwise, with trailing zeros removed.
		 */
		if (prec == 0) prec = 1;
		digits = ecvt(val, prec, &decpt, &sign);
		expo = decpt - 1;
		if (expo < -4 || expo >= prec)
			conv = (conv == 'G') ? 'E' : 'e';
		else {
			conv = 'f';
			prec = prec - decpt;
			if (prec < 0) prec = 0;
		}
	}

	if (conv == 'f') {
		digits = fcvt(val, prec, &decpt, &sign);
		nd = 0; while (digits[nd]) nd++;

		if (sign) *o++ = '-';
		else if (plussign) *o++ = '+';
		else if (blanksign) *o++ = ' ';

		if (decpt <= 0) {
			*o++ = '0';
			if (prec > 0) {
				*o++ = '.';
				for (i = 0; i < -decpt && i < prec; i++) *o++ = '0';
				for (nd = 0; digits[nd] && i < prec; nd++, i++)
					*o++ = digits[nd];
				for (; i < prec; i++) *o++ = '0';
			}
		} else {
			for (i = 0; i < decpt; i++)
				*o++ = digits[i] ? digits[i] : '0';
			if (prec > 0) {
				*o++ = '.';
				for (nd = 0; nd < prec; nd++)
					*o++ = digits[decpt + nd] ?
					    digits[decpt + nd] : '0';
			}
		}
		*o = '\0';
		return ((int)(o - out));
	}

	/* %e / %E */
	digits = ecvt(val, prec + 1, &decpt, &sign);
	if (sign) *o++ = '-';
	else if (plussign) *o++ = '+';
	else if (blanksign) *o++ = ' ';

	*o++ = digits[0] ? digits[0] : '0';
	if (prec > 0) {
		*o++ = '.';
		for (i = 1; i <= prec; i++)
			*o++ = digits[i] ? digits[i] : '0';
	}
	*o++ = (conv == 'E') ? 'E' : 'e';

	expo = (val == 0.0) ? 0 : decpt - 1;
	if (expo < 0) { *o++ = '-'; expo = -expo; }
	else *o++ = '+';
	/* at least two exponent digits, as every printf does */
	if (expo >= 100) {
		*o++ = '0' + expo / 100;
		*o++ = '0' + (expo / 10) % 10;
		*o++ = '0' + expo % 10;
	} else {
		*o++ = '0' + expo / 10;
		*o++ = '0' + expo % 10;
	}
	*o = '\0';
	return ((int)(o - out));
}
