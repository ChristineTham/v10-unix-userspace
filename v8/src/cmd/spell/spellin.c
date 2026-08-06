/*	@(#)spellin.c	1.1	*/
#include <stdio.h>
#include "hash.h"

#define S (BYTE*sizeof(long))
#define B (BYTE*sizeof(unsigned))
unsigned tabword;
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
unsigned wp;		/* word pointer*/
int bp =B;	/* bit pointer*/
int extra;

/*	usage: hashin N
	where N is number of words in dictionary
	and standard input contains sorted, unique
	hashed words in octal
*/
main(argc,argv)
char **argv;
{
	long h,k,d;
	register i;
	long count;
	long w;
	long x;
	int t,u;
	extern double huff();
	extern long ftell();
	long seekpt;
	double atof();
	double z;
	double nwords;
	k = 0;
	u = 0;
	if(argc!=2) {
		fprintf(stderr,"spellin: arg count\n");
		exit(1);
	}
	nwords = atof(argv[1]);
	z = huff((1L<<HASHWIDTH)/nwords);
	fprintf(stderr, "spellin: expected code widths = %f", z);
	z += sizeof(tabword)*BYTE/2*(double)(1<<INDEXWIDTH)/nwords;
	fprintf(stderr, " +breakage = %f\n", z); /*t half word per bin */
	whuff();
	seekpt = ftell(stdout);
	fwrite((char*)hindex, sizeof(*hindex), NI, stdout); /*dummy data */
	for(count=0; scanf("%lo", &h) == 1; ++count) {
		if((t=h>>(HASHWIDTH-INDEXWIDTH)) != u) {
			if(bp!=B)
				newword();
			bp = B;
			while(u<t)
				hindex[++u] = wp;
			k =  (long)t<<(HASHWIDTH-INDEXWIDTH);
		}
		d = h-k;
		k = h;
		for(;;) {
			for(x=d;;x/=2) {
				i = encode(x,&w);
				if(i>0)
					break;
			}
			if(i>B) {
				append((unsigned)(w>>(i-B)), B);
				append((unsigned)(w<<(B+B-i)), i-B);
			} else
				append((unsigned)(w<<(B-i)), i);
			d -= x;
			if(d>0)
				extra++;
			else
				break;
		}
	}
	if(bp!=B)
		newword();
	while(++u<NI)
		hindex[u] = wp;
	newword();	/* padding allows one out-of-bounds fetch */
	newword();
	newword();
	fseek(stdout, seekpt, 0);	/* overwrite dummy data */
	fwrite((char*)hindex, sizeof(*hindex), NI, stdout);
	fprintf(stderr, "spellin: %ld items, %d extra, %u words occupied\n",
		count,extra,wp);
	fprintf(stderr, "spellin: %f table bits/item, ", 
		((float)BYTE*wp)*sizeof(tabword)/count);
	fprintf(stderr, "%f table+hindex bits\n",
		BYTE*((float)wp*sizeof(tabword) + sizeof(hindex))/count);
	return(0);
}

append(w, i)
register unsigned w;
register i;
{
	for(;;) {
		tabword |= w>>(B-bp);
		i -= bp;
		if(i<0) {
			bp = -i;
			return;
		}
		w <<= bp;
		bp = B;
		newword();
	}
}

newword()
{
	fwrite((char*)&tabword, sizeof(tabword), 1, stdout);
	wp++;
	tabword = 0;
}
