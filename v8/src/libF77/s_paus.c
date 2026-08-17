/* PORT: `long' here is FORTRAN INTEGER, and V8's own compiler made C's long 32
   bits -- `# define NOLONG', which its own comment glosses "map longs to ints", at
   src/cmd/ccom/vax/macdefs.h:20.  Under v8cc it is 64, so every one of these
   spellings widened the Fortran ABI by a factor of two.  f77's machdefs pins
   SZLONG at 4 twice over: typesize[TYREAL] is SZLONG and libF77's r_nint takes
   `float *', and lengtype() (proc.c:951) hardcodes INTEGER*4 -> TYLONG and
   REAL*4 -> TYREAL.  So `int' is what this source always meant.  Same
   reasoning, and the same remedy, as narrowing daddr_t in src/include.
   src/libF77/PORTING.md and task #12. */
#include <stdio.h>
#define PAUSESIG 15


s_paus(s, n)
char *s;
int n;
{
int i;
int waitpause();

fprintf(stderr, "PAUSE ");
if(n > 0)
	for(i = 0; i<n ; ++i)
		putc(*s++, stderr);
fprintf(stderr, " statement executed\n");
if( isatty(fileno(stdin)) )
	{
	fprintf(stderr, "To resume execution, type go.  Any other input will terminate job.\n");
	if( getchar()!='g' || getchar()!='o' || getchar()!='\n' )
		{
		fprintf(stderr, "STOP\n");
		f_exit();
		exit(0);
		}
	}
else
	{
	fprintf(stderr, "To resume execution, execute a   kill -%d %d   command\n",
		PAUSESIG, getpid() );
	signal(PAUSESIG, waitpause);
	pause();
	}
fprintf(stderr, "Execution resumes after PAUSE.\n");
}





static waitpause()
{
return;
}
