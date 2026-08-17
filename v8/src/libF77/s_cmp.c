/* PORT: `long' here is FORTRAN INTEGER, and V8's own compiler made C's long 32
   bits -- `# define NOLONG', which its own comment glosses "map longs to ints", at
   src/cmd/ccom/vax/macdefs.h:20.  Under v8cc it is 64, so every one of these
   spellings widened the Fortran ABI by a factor of two.  f77's machdefs pins
   SZLONG at 4 twice over: typesize[TYREAL] is SZLONG and libF77's r_nint takes
   `float *', and lengtype() (proc.c:951) hardcodes INTEGER*4 -> TYLONG and
   REAL*4 -> TYREAL.  So `int' is what this source always meant.  Same
   reasoning, and the same remedy, as narrowing daddr_t in src/include.
   src/libF77/PORTING.md and task #12. */
int s_cmp(a, b, la, lb)	/* compare two strings */
register char *a, *b;
int la, lb;
{
register char *aend, *bend;
aend = a + la;
bend = b + lb;

if(la <= lb)
	{
	while(a < aend)
		if(*a != *b)
			return( *a - *b );
		else
			{ ++a; ++b; }

	while(b < bend)
		if(*b != ' ')
			return( ' ' - *b );
		else	++b;
	}

else
	{
	while(b < bend)
		if(*a == *b)
			{ ++a; ++b; }
		else
			return( *a - *b );
	while(a < aend)
		if(*a != ' ')
			return(*a - ' ');
		else	++a;
	}
return(0);
}
