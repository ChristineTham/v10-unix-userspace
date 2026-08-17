/* PORT: `long' here is FORTRAN INTEGER, and V8's own compiler made C's long 32
   bits -- `# define NOLONG', which its own comment glosses "map longs to ints", at
   src/cmd/ccom/vax/macdefs.h:20.  Under v8cc it is 64, so every one of these
   spellings widened the Fortran ABI by a factor of two.  f77's machdefs pins
   SZLONG at 4 twice over: typesize[TYREAL] is SZLONG and libF77's r_nint takes
   `float *', and lengtype() (proc.c:951) hardcodes INTEGER*4 -> TYLONG and
   REAL*4 -> TYREAL.  So `int' is what this source always meant.  Same
   reasoning, and the same remedy, as narrowing daddr_t in src/include.
   src/libF77/PORTING.md and task #12. */
s_cat(lp, rpp, rnp, np, ll)
char *lp, *rpp[];
int rnp[], *np, ll;
{
int i, n, nc;
char *rp;

n = *np;
for(i = 0 ; i < n ; ++i)
	{
	nc = ll;
	if(rnp[i] < nc)
		nc = rnp[i];
	ll -= nc;
	rp = rpp[i];
	while(--nc >= 0)
		*lp++ = *rp++;
	}
while(--ll >= 0)
	*lp++ = ' ';
}
