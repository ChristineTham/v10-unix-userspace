/*      @(#)hash.h      1.1     */
#define HASHWIDTH 27
#define HASHSIZE 134217689L     /*prime under 2^HASHWIDTH*/
#ifdef pdp11
#define INDEXWIDTH 9
#else
#define INDEXWIDTH 10
#endif
#define INDEXSIZE (1<<INDEXWIDTH)
#define NI (INDEXSIZE+1)
#define BYTE 8

extern unsigned *table;
extern unsigned short hindex[];  /*into dif table based on hi hash bits*/ /* PORT: renamed, see hashlook.c */

/*
 * PORT: HALFWORD -- an unsigned is half a long, as on the PDP-11.
 *
 * hashlook.c picks its bit-fetch macro between exactly two cases: the PDP-11,
 * where sizeof(unsigned) == sizeof(long)/2, and everything else, where the two
 * are equal.  LP64 is the PDP-11's case again (4 and 8), so the "pdp11" macro
 * is the correct one here -- it assembles a long out of words half its width,
 * which is precisely what is needed.
 *
 * hashlook.c says so itself, at runtime:
 *
 *	if(sizeof(long) > sizeof(unsigned))
 *		abort();	/\*wrong fetch macro*\/
 *
 * and that abort fired on the first run.  Selecting on the target macro rather
 * than on sizeof because the choice has to be made by the preprocessor, which
 * is also how the file already did it.
 */
#ifdef arm64
#define HALFWORD 1
#endif

extern long hash();
