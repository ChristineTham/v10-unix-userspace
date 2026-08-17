signal_(sigp, procp)
int *sigp, (**procp)();
{
int sig;
/* PORT: was `int sig, proc;'.  procp already says what it points at -- a
   function pointer -- and storing that in an int is exact on a VAX and loses
   the top half here, so the handler was installed at a truncated address and
   the first delivery faulted.  The declaration is the whole fix; both uses
   below are unchanged.

   The RETURN direction is still narrow and is not fixable here: signal_ has an
   implicit int return, signal() hands back the previous handler, and a Fortran
   caller receives it in an INTEGER.  Widening the return would not help, since
   the value has to fit whatever f77 calls an INTEGER at the other end.  Every
   caller in this tree uses CALL SIGNAL and discards it. */
int (*proc)();
sig = *sigp;
proc = *procp;

return( signal(sig, proc) );
}
