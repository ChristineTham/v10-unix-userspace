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
	int width, prec, haveprec, longflag, convch;
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

		/*
		 * CAPITAL CONVERSION LETTERS.
		 *
		 * V8's doprnt.S does this, at the label `capital`:
		 *
		 *	bisl2 $1<caps,flags	# note that it was capitalized
		 *	xorb2 $'a^'A,r0		# make it small
		 *	jbr L4			# and try again
		 *
		 * -- fold to lowercase, remembering that it was capital.  In
		 * doprnt.S the flag is then read in only two places: the hex
		 * digit table (`%X` prints A-F and the 0X prefix) and the
		 * exponent letter (`%E`, `%G`).  `X`, `E` and `G` keep their own
		 * cases below, so all that is left to fold here is D, O and U.
		 *
		 * They also set longflag, which is a decision doprnt.S never had
		 * to make: on a 32-bit VAX an int and a long were the same four
		 * bytes, so `%D` and `%d` could not be told apart.  Under LP64
		 * they can, and long is the right reading -- these are the V6/V7
		 * long conversions, and grep's `tln` really is a long.  A caller
		 * passing an int is unharmed, since the argument slot holds it
		 * sign-extended to eight bytes.
		 *
		 * Twelve uses across the command tree, grep(1) among them:
		 * `grep -c apple` printed "D" instead of "2".
		 */
		convch = *fmt++;
		switch (convch) {
		case 'D': convch = 'd'; longflag = 1; break;
		case 'O': convch = 'o'; longflag = 1; break;
		case 'U': convch = 'u'; longflag = 1; break;
		}

		sign = 0;
		switch (convch) {

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
			/*
			 * PORT: THE EMPTY STRING, NOT "(null)", BECAUSE THAT IS
			 * WHAT A VAX PRINTED.  This file's own header promises
			 * "the same observable output" as doprnt.S, and "(null)"
			 * was output V8 never produced: doprnt.S has no null
			 * guard at all -- measured, its six occurrences of the
			 * word are comments about NUL bytes in the format scan --
			 * so %s of a null pointer read virtual address 0 and
			 * stopped on the byte it found there.
			 *
			 * AND THAT BYTE IS 0x00, MEASURED ON THE SHIPPED
			 * BINARIES.  V8 a.out is ZMAGIC (magic 0413, and
			 * a.out.h:26-27 makes N_TXTOFF 1024 for it), so the
			 * header is never mapped and virtual 0 is the first byte
			 * of crt0.  bin/cat, bin/ls and usr/bin/egrep all begin
			 * `00 00 c2 08 5e d0 ae 08' there.  A leading NUL is a
			 * zero-length string, so the VAX printed nothing -- and
			 * padded a field width, which this spelling keeps and a
			 * skip of the whole arm would not.
			 *
			 * The "(null)" convention is real but belongs to other
			 * programs' private copies of 4BSD's doprnt, not to
			 * libc: cmd/ex/ovdoprnt.s carries `nulstr: <(null)\0>'
			 * and cmd/csh/doprnt.c has its own.  Neither is this
			 * file, and neither is what printf(3) linked.
			 */
			if (s == 0) s = "";
			/*
			 * The precision has to be in the loop CONDITION, not
			 * in its body.  Written as
			 *
			 *	for (len = 0; s[len]; len++)
			 *		if (haveprec && len >= prec) break;
			 *
			 * it reads s[prec] before deciding to stop -- exactly
			 * one byte past the field -- and %.Ns exists for a
			 * fixed-width field that need NOT be terminated.  V8's
			 * own ncheck prints directory entries with "%.14s"
			 * over a 14-byte d_name that fills its record, so the
			 * byte read is the next entry's d_ino; against a
			 * DIRSIZ field at the end of a mapped page it is a
			 * fault.  Ours to fix: this file is a C rewrite of
			 * doprnt.S, so the bug is the port's, not V8's.
			 */
			for (len = 0; (!haveprec || len < prec) && s[len]; len++)
				;
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
			    plussign, blanksign, alt, fbuf);
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
 * %g's trailing-zero strip, and it is the VAX's own.  doprnt.S:625-631:
 *
 *	g1:	jbs $numsgn,flags,g2	# `#' given: keep them
 *		jbs $dpflag,flags,g2	# dont strip if no decimal point
 *	g3:	cmpb -(r5),$'0		# strip trailing zeroes
 *		jeql g3
 *		cmpb (r5),$'.		# and trailing decimal point
 *		jeql g2
 *		incl r5
 *
 * `dpflag' records whether a decimal point was emitted; scanning the region for
 * one asks the same question, since these two arms write a point only when they
 * write a fraction.  numsgn is `#', so %#g keeps its zeros, which is ANSI's rule
 * and was V8's four years before the standard.
 *
 * Returns the new end of the buffer.
 */
static char *
gstrip(start, o)
	char *start, *o;
{
	register char *p;

	for (p = start; p < o; p++)
		if (*p == '.')
			break;
	if (p >= o)
		return (o);		/* no decimal point: nothing to strip */
	while (o > p + 1 && o[-1] == '0')
		o--;
	if (o == p + 1)
		o--;			/* the whole fraction went: take the '.' */
	return (o);
}

/*
 * Floating conversion, built on ecvt/fcvt the way the VAX version was.  Those
 * are now IEEE rather than VAX D-format, which is the one place output can
 * differ from a real V8: the last digit or two of a value that was not exactly
 * representable in either format.
 */
static
fmtfloat(conv, val, prec, plussign, blanksign, alt, out)
	int conv;
	double val;
	int prec, plussign, blanksign, alt;
	char *out;
{
	char *digits;
	int decpt, sign, nd, i;
	char *o = out;
	int expo;
	int gfmt = 0;

	if (conv == 'g' || conv == 'G') {
		/*
		 * %g: %e if the exponent is below -4 or at least the precision,
		 * %f otherwise, with trailing zeros removed.
		 *
		 * The stripping is what `gfmt' carries down: it belongs to the
		 * MANTISSA and not to the whole buffer, so the %e arm below has
		 * to do it before it writes the `e'.  The VAX splits the same
		 * way -- gfmte calls eedit, jumps back to g1 to strip, and only
		 * then falls into eexp to append the exponent.
		 */
		if (prec == 0) prec = 1;
		digits = ecvt(val, prec, &decpt, &sign);
		expo = decpt - 1;
		gfmt = !alt;
		if (expo < -4 || expo >= prec) {
			conv = (conv == 'G') ? 'E' : 'e';
			/*
			 * %g's precision counts SIGNIFICANT digits and %e's
			 * counts digits after the point, so the e-style form of
			 * %.Pg is %.(P-1)e.  Without this, %g of 1234567 came
			 * out `1.234567e+06' -- seven significant digits from a
			 * conversion that asked for six.  The VAX makes the
			 * same distinction in the other direction: `scien'
			 * (doprnt.S:569) does `incl ndigit' on the way in and
			 * `general' (doprnt.S:639) jumps past it, so ndigit is
			 * P+1 for %e and P for %g.
			 */
			prec = prec - 1;
		} else {
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
		if (gfmt) o = gstrip(out, o);
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
	if (gfmt) o = gstrip(out, o);
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
