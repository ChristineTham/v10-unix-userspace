 /* tm.c: split numerical fields */
# include "t..c"
char *maknew(str)
	char *str;
{
	/* make two numerical fields */
	extern char *chspace();
/*
 * PORT: `long dpoint', was `int', AND the cast at :39 with it.
 *
 * dpoint is upstream's flag-and-pointer: it is tested as a boolean four times
 * (`dpoint==0', `!dpoint', `if (dpoint)') and it holds the ADDRESS of the
 * decimal point when one is found.  On a VAX an int and a char * are both four
 * bytes and that is exact; here `(int)str' keeps the low 32 bits and
 * `(char *)dpoint' hands the truncated half back, so maknew dereferences it
 * and dies.  Measured: EXC_BAD_ACCESS at 0x1e6058a9 -- a 32-bit value, which
 * is the signature of this whole class.
 *
 * BOTH LINES HAVE TO MOVE.  Widening the declaration alone changes nothing,
 * because `(int)str' truncates BEFORE the value is widened into the long --
 * the explicit cast is the truncation, not the storage.
 *
 * `long' rather than `char *' because the boolean uses are upstream's idiom
 * and a pointer type would need three more edits to keep them; this is the
 * same one-word fix, for the same reason, as yacc's `#define YYSTYPE long' and
 * qed's five signal-handler variables.
 *
 * IT IS THE `n' COLUMN, WHICH IS TBL'S CHARACTERISTIC FEATURE -- numeric
 * alignment on the decimal point -- and maknew is only called for one
 * (t5.c:72, t9.c:49).  Twenty bytes reproduce it:
 *
 *	.TS
 *	n.
 *	3.5
 *	.TE
 *
 * `l', `c', `r' and `a' columns are all clean, which is why this survived: it
 * was found by Bell Labs' OWN test suite (src/cmd/tbl/samples.a, 16 of its 54
 * cases died), which had been deleted from git by a .gitignore rule and never
 * run.  tests/wavec exercises tbl with hand-written tables and never used an
 * `n' column once.
 */
	long dpoint;
	int c;
	char *p, *q, *ba;
	p = str;
	for (ba= 0; c = *str; str++)
		if (c == '\\' && *(str+1)== '&')
			ba=str;
	str=p;
	if (ba==0)
		{
		for (dpoint=0; *str; str++)
			{
			if (*str=='.' && !ineqn(str,p) &&
				(str>p && digit(*(str-1)) ||
				digit(*(str+1))))
					dpoint=(long)str;	/* PORT: (int) */
			}
		if (dpoint==0)
			for(; str>p; str--)
			{
			if (digit( * (str-1) ) && !ineqn(str, p))
				break;
			}
		if (!dpoint && p==str) /* not numerical, don't split */
			return(0);
		if (dpoint) str=(char *)dpoint;
		}
	else
		str = ba;
	p =str;
	if (exstore ==0 || exstore >exlim)
		{
		exstore = exspace = chspace();
		exlim= exstore+MAXCHS;
		}
	q = exstore;
	while (*exstore++ = *str++);
	*p = 0;
	return(q);
	}
ineqn (s, p)
	char *s, *p;
{
/* true if s is in a eqn within p */
int ineq = 0, c;
while (c = *p)
	{
	if (s == p)
		return(ineq);
	p++;
	if ((ineq == 0) && (c == delim1))
		ineq = 1;
	else
	if ((ineq == 1) && (c == delim2))
		ineq = 0;
	}
return(0);
}
