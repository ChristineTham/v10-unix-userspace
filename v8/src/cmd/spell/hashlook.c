/*	@(#)hashlook.c	1.4	*/
#include <stdio.h>
#include "hash.h"
#include "huff.h"

unsigned *table;
/*
 * PORT: `index` renamed to `hindex`.
 *
 * spell names a global array `index`, which is also a V7 libc function
 * (index(3), what C89 calls strchr).  On V8 that was harmless: a.out ld pulls
 * an archive member only for a symbol that is still UNDEFINED, and a tentative
 * definition is not undefined, so libc's index.o was simply never loaded.
 *
 * Mach-O's linker resolves COMMON symbols from archives too.  It found _index
 * in libv8c.a and replaced spell's 2050-byte array with the 156-byte function:
 *
 *	ld: warning: tentative definition of '_index' with size 2050 ... is
 *	being replaced by real definition of smaller size 156 from
 *	libv8c.a[17](index.o)
 *
 * -- so every write to index[] would have landed in libc's code.  Nothing in
 * spell or in libc REFERENCES index(); the mere tentative definition triggers
 * the lookup.
 *
 * Renaming here rather than changing how the compiler emits file-scope arrays:
 * V8 code relies throughout on tentative definitions merging across
 * translation units, and emitting real definitions instead would break that.
 */
unsigned short hindex[NI];

#define B (BYTE*sizeof(unsigned))
#define L (BYTE*sizeof(long)-1)
#define MASK (~(1L<<L))

#if defined(pdp11) || defined(HALFWORD)	/*sizeof(unsigned)==sizeof(long)/2 */
#define fetch(wp,bp)\
	(((((long)wp[0]<<B)|wp[1])<<(B-bp))|(wp[2]>>bp))
#else 		/*sizeof(unsigned)==sizeof(long)*/
#define fetch(wp,bp) (bp==B?wp[0]:((wp[0]<<(B-bp))|(wp[1]>>bp)))
#endif

hashlook(s)
char *s;
{
	long h;
	long t;
	register bp;
	register unsigned *wp;
	int i;
	long sum;
	unsigned *tp;

	h = hash(s);
	t = h>>(HASHWIDTH-INDEXWIDTH);
	wp = &table[hindex[t]];
	tp = &table[hindex[t+1]];
	bp = B;
	sum = (long)t<<(HASHWIDTH-INDEXWIDTH);
	for(;;) {
		{/*	this block is equivalent to
			 bp -= decode((fetch(wp,bp)>>1)&MASK, &t);*/
			long y;
			long v;
			y = (fetch(wp,bp)>>1) & MASK;
			if(y < cs) {
				t = y >> (L+1-w);
				bp -= w-1;
			}
			else {
				for(bp-=w,v=v0; y>=qcs; y=(y<<1)&MASK,v+=n)
					bp -= 1;
				t = v + (y>>(L-w));
			}
		}
		while(bp<=0) {
			bp += B;
			wp++;
		}
		if(wp>=tp&&(wp>tp||bp<B))
			return(0);
		sum += t;
		if(sum<h)
			continue;
		return(sum==h);
	}
}


prime(argc,argv)
char **argv;
{
	register FILE *f;
	register fd;
	extern char *malloc();
	if(argc <= 1)
		return(0);
#if !defined(pdp11) && !defined(HALFWORD)
	if(sizeof(long) > sizeof(unsigned))
		abort();	/*wrong fetch macro*/
#endif
#ifdef pdp11	/* because of insufficient address space for buffers*/
	fd = dup(0);
	close(0);
	if(open(argv[1], 0) != 0)
		return(0);
	f = stdin;
	if(rhuff(f)==0
	|| read(fileno(f), (char *)hindex, NI*sizeof(*hindex)) != NI*sizeof(*hindex)
	|| (table = (unsigned*)malloc(hindex[NI-1]*sizeof(*table))) == 0
	|| read(fileno(f), (char*)table, sizeof(*table)*hindex[NI-1])
	   != hindex[NI-1]*sizeof(*table))
		return(0);
	close(0);
	if(dup(fd) != 0)
		return(0);
	close(fd);
#else
	if((f = fopen(argv[1], "ri")) == NULL)
		return(0);
	if(rhuff(f)==0
	|| fread((char*)hindex, sizeof(*hindex),  NI, f) != NI
	|| (table = (unsigned*)malloc(hindex[NI-1]*sizeof(*table))) == 0
	|| fread((char*)table, sizeof(*table), hindex[NI-1], f)
	   != hindex[NI-1])
		return(0);
	fclose(f);
#endif
	hashinit();
	return(1);
}
