/* PORT: `long' here is FORTRAN INTEGER, and V8's own compiler made C's long 32
   bits -- `# define NOLONG', which its own comment glosses "map longs to ints", at
   src/cmd/ccom/vax/macdefs.h:20.  Under v8cc it is 64, so every one of these
   spellings widened the Fortran ABI by a factor of two.  f77's machdefs pins
   SZLONG at 4 twice over: typesize[TYREAL] is SZLONG and libF77's r_nint takes
   `float *', and lengtype() (proc.c:951) hardcodes INTEGER*4 -> TYLONG and
   REAL*4 -> TYREAL.  So `int' is what this source always meant.  Same
   reasoning, and the same remedy, as narrowing daddr_t in src/include.
   src/libF77/PORTING.md and task #12. */
/*
 * subroutine getarg(k, c)
 * returns the kth unix command argument in fortran character
 * variable argument c
*/

getarg_(n, s, ls)
int *n;
register char *s;
int ls;
{
extern int xargc;
extern char **xargv;
register char *t;
register int i;

if(*n>=0 && *n<xargc)
	t = xargv[*n];
else
	t = "";
for(i = 0; i<ls && *t!='\0' ; ++i)
	*s++ = *t++;
for( ; i<ls ; ++i)
	*s++ = ' ';
}
