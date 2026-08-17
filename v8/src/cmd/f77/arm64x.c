/*
 * arm64x.c -- vaxx.c's counterpart, and the whole of the driver's
 * machine-dependent output.
 *
 * Four functions, all of them assembler directives, and they exist as a
 * separate file for the reason upstream separated them: the makefile links the
 * DRIVER as `driver.o vaxx.o', because driver.c is not only a driver.  From
 * line 1039 it holds dodata(), the DATA statement emitter -- it sorts the
 * intermediate file f77pass1 writes, walks it, and lays initialised variables
 * out in the assembly file.  So these four are called by the driver, and
 * pdp11x.c and vaxx.c are each one machine's answer.
 *
 * There is a fifth name in the same family, prskip's neighbour prspace(), and
 * it is NOT here: driver.c defines it itself, above dodata.
 *
 * WHAT DIFFERS FROM vaxx.c, which is three of the four:
 *
 *	prchars   .byte 0%o,0%o     -> .byte 0x%x,0x%x
 *	prskip    .space %ld        -> .space %ld       (unchanged)
 *	pruse     \t%s\n            -> \t%s\n          (unchanged)
 *	prcomblock LABELFMT         -> LABELFMT        (unchanged)
 *
 * prchars is the only real change and it is not a preference: clang's arm64
 * assembler does not accept the VAX assembler's leading-zero octal, so
 * `.byte 0101,0102' is read as decimal 101 -- a plausible wrong answer rather
 * than an error, which is the direction that costs a session.  Measured: 0x is
 * accepted and 0-prefixed octal is silently decimal.
 *
 * prskip's SIZE ARGUMENT IS ftnint, WHICH IS FOUR BYTES ON THIS TARGET, and the
 * %ld is therefore wrong here in a way it was not on a VAX.  See the note in
 * arm64defs: SZLONG is 4 because typesize[TYREAL] pins it, and V8's own compiler
 * made C's `long' 32 bits (`# define NOLONG', ccom/vax/macdefs.h:20) so upstream
 * %ld and upstream ftnint agreed.  Under v8cc they do not.  This file therefore
 * casts at the call rather than trusting the format, which is the narrowest
 * possible statement of the problem and leaves task #12's decision open.
 */

#include <stdio.h>
#include "defines"
#include "machdefs"



prchars(fp, s)
FILEP fp;
int *s;
{

fprintf(fp, ".byte 0x%x,0x%x\n", s[0] & 0377, s[1] & 0377);
}



/*
 * pruse -- a section directive, PLUS AN ALIGNMENT, which the VAX's did not need.
 *
 * dodata() emits `.data' and then the first variable's label, relying on the
 * per-variable doeven() for alignment -- and doeven() emits nothing when the
 * running offset is already 0, which it always is for the first one.  On a VAX
 * that was harmless because a section began aligned enough for anything.  In
 * Mach-O the first atom after a section switch keeps whatever alignment the
 * assembler last had, so a block holding a pointer came out 1-aligned and ld
 * refused it: `alignment (1) of atom v.1 is too small'.
 *
 * Three is log2(8), the widest thing a Fortran data block can hold.  Emitting
 * it here rather than in dodata() keeps the fix on the machine-dependent side,
 * where the reason lives.
 */
pruse(fp, s)
FILEP fp;
char *s;
{
fprintf(fp, "\t%s\n", s);
if(s[0] == '.' && (s[1] == 'd' || s[1] == 't' || s[1] == 's'))
	fprintf(fp, "\t.p2align\t3\n");
}



prskip(fp, k)
FILEP fp;
ftnint k;
{
fprintf(fp, "\t.space\t%ld\n", (long) k);
}





prcomblock(fp, name)
FILEP fp;
char *name;
{
fprintf(fp, LABELFMT, name);
}
