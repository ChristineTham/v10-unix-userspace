/* PORT: `long' here is FORTRAN INTEGER, and V8's own compiler made C's long 32
   bits -- `# define NOLONG', which its own comment glosses "map longs to ints", at
   src/cmd/ccom/vax/macdefs.h:20.  Under v8cc it is 64, so every one of these
   spellings widened the Fortran ABI by a factor of two.  f77's machdefs pins
   SZLONG at 4 twice over: typesize[TYREAL] is SZLONG and libF77's r_nint takes
   `float *', and lengtype() (proc.c:951) hardcodes INTEGER*4 -> TYLONG and
   REAL*4 -> TYREAL.  So `int' is what this source always meant.  Same
   reasoning, and the same remedy, as narrowing daddr_t in src/include.
   src/libF77/PORTING.md and task #12. */
double pow_di(ap, bp)
double *ap;
int *bp;
{
double pow, x;
int n;

pow = 1;
x = *ap;
n = *bp;

if(n != 0)
	{
	if(n < 0)
		{
		if(x == 0)
			{
			return(pow);
			}
		n = -n;
		x = 1/x;
		}
	for( ; ; )
		{
		if(n & 01)
			pow *= x;
		if(n >>= 1)
			x *= x;
		else
			break;
		}
	}
return(pow);
}
