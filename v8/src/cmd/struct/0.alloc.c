#include <stdio.h>
#include "def.h"
int routbeg;

extern int debug;
struct coreblk	{struct coreblk *nxtblk;
			int blksize;
			int nxtfree;
			int *blk;
			};

long space;
/*
 * PORT: the five allocators here return POINTERS through an implicit `int'.
 *
 * This is the class CLAUDE.md opens with -- "the tree calls malloc without
 * declaring it and casts the int result to a pointer" -- and struct stacks two
 * of them in challoc: `int i; i = malloc(n); return(i);' truncates once into
 * the local and once again by returning from a function nothing has declared.
 *
 * MEASURED, and it is the program's first instruction of real work:
 * hash_init() at 1.hash.c:92 does `hashtab = challoc(...)' and then
 * `hashtab[i] = -1L', which is EXC_BAD_ACCESS at 0x26e04c90 -- a 32-bit value,
 * this class's signature.  structure(1) could not process one line of Fortran.
 *
 * Exact on a VAX, where an int and a pointer are both four bytes.  The
 * declarations move; not one statement does.  def.h gains the matching
 * declarations so every caller sees them -- it is already the header every one
 * of the 0-4 objects includes (`main.o $(0FILES.o) ...: def.h' in upstream's
 * own makefile), which is what makes one place enough.
 */
char *malloc();

char *
challoc(n)
int n;
	{
	char *i;				/* PORT: int */
	i = malloc(n);
	if(i) { space += n; return(i); }
	fprintf(stderr,"alloc out of space\n");
	fprintf(stderr,"total space alloc'ed = %D\n",space);
	fprintf(stderr,"%d more bytes requested\n",n);
	exit(1);
	}


chfree(p,n)
int *p,n;
	{
	ASSERT(p,chfree);
	space -= n;
	free(p);
	}


struct coreblk *tcore, *gcore;
int tblksize=12, gblksize=300;


int *
balloc(n,p,size)		/* allocate n bytes from coreblk *p */	/* PORT: implicit int */
int n,size;		/* use specifies where called */
struct coreblk **p;
	{
	int i;
	struct coreblk *q;
	n = (n+sizeof(i)-1)/sizeof(i);	/* convert bytes to wds to ensure ints always at wd boundaries */
	for (q = *p; ; q = q->nxtblk)
		{
		if (!q)
			{
			q = morespace(n,p,size);
			break;
			}
		if (q-> blksize - q->nxtfree >= n)  break;
		}
	i = q->nxtfree;
	q ->nxtfree += n;
	return( &(q->blk)[i]);
	}

int *
talloc(n)		/* allocate from line-by-line storage area */	/* PORT: implicit int */
int n;
	{return(balloc(n,&tcore,tblksize)); }

int *
galloc(n)		/* allocate from graph storage area */	/* PORT: implicit int */
int n;
	{
	return(balloc(n,&gcore,gblksize));
	}

reuse(p)		/* set nxtfree so coreblk can be reused */
struct coreblk *p;
	{
	for (; p; p=p->nxtblk)  p->nxtfree = 0;  
	}

bfree(p)		/* free coreblk p */
struct coreblk *p;
	{
	if (!p) return;
	bfree(p->nxtblk);
	p->nxtblk = 0;
	free(p);
	}


struct coreblk *
morespace(n,p,size)	/* get at least n more wds for coreblk *p */	/* PORT: implicit int */
int n,size;
struct coreblk **p;
	{struct coreblk *q;
	int t,i;

	t = n<size?size:n;
	q = malloc(i=t*sizeof(*(q->blk))+sizeof(*q));
	if(!q){
		error(": alloc out of space","","");
		fprintf(stderr,"space = %D\n",space);
		fprintf(stderr,"%d more bytes requested\n",n);
		exit(1);
		}
	space += i;
	q->nxtblk = *p;
	*p = q;
	q -> blksize = t;
	q-> nxtfree = 0;
	q->blk = q + 1;
	return(q);
	}




freegraf()
	{
	bfree(gcore);
	gcore = 0;


	}









error(mess1, mess2, mess3)
char *mess1, *mess2, *mess3;
	{
	static lastbeg;
	if (lastbeg != routbeg)
		{
		fprintf(stderr,"routine beginning on line %d:\n",routbeg);
		lastbeg = routbeg;
		}
	fprintf(stderr,"error %s %s %s\n",mess1, mess2, mess3);
	}


faterr(mess1, mess2, mess3)
char *mess1, *mess2, *mess3;
	{
	error(mess1, mess2, mess3);
	exit(1);
	}


strerr(mess1, mess2, mess3)
char *mess1, *mess2, *mess3;
	{
	error("struct error: ",mess1, mess2);
	}
