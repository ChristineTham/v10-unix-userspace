/*	@(#)brkincr.h	1.2	*/
/*	3.0 SID #	1.1	*/
/*
 * PORT: scaled by four for a 64-bit machine.  Were 01000 and 04000.
 *
 * These size the shell's own arena, and every structure it puts there grew when
 * pointers did: BYTESPERWORD is 8 rather than 4, `struct blk` doubled, and
 * `struct namnod` -- three pointers and two {char,char *} pairs -- went from
 * about 20 bytes to 64.  The 1985 numbers left proportionally that much less
 * headroom.
 *
 * It matters because the invariant "stakbot stays below brkend" is maintained
 * only by locstak(), and setenv() does not go through it: it calls getstak()
 * directly, which bumps stakbot with no check at all.  On the VAX 3*BRKINCR of
 * initial arena covered the environment comfortably.  Here it did not --
 *
 *	bot/top/end  00000001076b3ff0 00000001076b3ff0 00000001076b1a00
 *
 * stakbot was 9712 bytes PAST brkend -- so staknam() wrote into unmapped
 * memory, the fault handler stdsigs() installed returned, and the faulting
 * instruction retried forever.  The shell hung rather than crashed, in the
 * child of a fork, which is why it looked like a fork or wait problem.
 *
 * Scaling the constant is the intended adjustment: brkincr.h exists to hold it,
 * and blok.c reads it into a variable that locstak() then grows towards BRKMAX.
 */
#define BRKINCR 04000
#define BRKMAX 040000
