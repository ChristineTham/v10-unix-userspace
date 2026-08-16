#define ASSERT(P,R)	{if (!(P)) {fprintf(stderr,"failed assertion in routine R: P\n"); abort();}}

/*
 * PORT: `#define VERT long', was `int' -- and the define MOVED ABOVE the
 * extern block, because `graph' is declared in terms of it and cpp is ordered.
 *
 * A VERT cell is not only a vertex number.  While a Fortran label is still
 * unresolved, 1.hash.c threads a FIXUP CHAIN through the very cells that will
 * later hold the vertex: addref() stores `ptr' -- the address of a graph cell
 * -- into chain[], each cell holds the address of the next, and fixvalue()
 * walks the chain overwriting every one with the real vertex number.  So one
 * cell means a vertex number OR a pointer, chosen by whether the label has
 * been seen yet, and 1.hash.c:152 (`*ptr = chain[index]') and :162
 * (`chain[index] = ptr') are the two arms written four lines apart.
 *
 * On a VAX both fit in the same four bytes and the pun is exact.  Here a
 * pointer needs eight, so the CELL has to be pointer-sized -- which is the
 * declaration, not any statement.  Widening VERT is therefore the whole fix
 * and `graph' follows it, because graph is an array of arrays of VERT.
 *
 * `long' rather than `char *' for the reason recorded in tbl's tm.c and qed's
 * vars.h: the cells are arithmetic (`v >= 0' is DEFINED(v), UNDEFINED is -1)
 * far more often than they are addresses, and a pointer type would need those
 * edits everywhere while a long needs none.  Same one-word fix as yacc's
 * `#define YYSTYPE long'.
 */
#define VERT long

extern int routnum, routerr;
extern long rtnbeg;		/* number of chars up to beginnine of curernt routing */
extern VERT **graph;
extern int nodenum;
extern int stopflg;		/* turns off generation of stop statements */

#define TRUE 1
#define FALSE 0
#define LOGICAL int
#define DEFINED(v)	(v >= 0)
#define UNDEFINED	-1

/* node types */
#define STLNVX		0
#define IFVX		1
#define DOVX		2
#define IOVX		3
#define FMTVX		4
#define COMPVX		5
#define ASVX		6
#define ASGOVX		7
#define LOOPVX		8
#define WHIVX		9
#define UNTVX		10
#define ITERVX		11
#define THENVX		12
#define STOPVX		13
#define RETVX		14
#define DUMVX		15
#define GOVX		16
#define BRKVX		17
#define NXTVX		18
#define SWCHVX		19
#define ACASVX		20
#define ICASVX		21

#define TYPENUM	22


extern int hascom[TYPENUM];		/* FALSE for types with no comments, 2 otherwise */
extern int nonarcs[TYPENUM];		/* number of wds per node other than arcs */
extern VERT *arc(), *lchild();
/*
 * PORT: `VERT *', all six -- and THIS IS THE LINE BESIDE IT, literally.  The
 * line directly above declares `arc()' and `lchild()' as VERT *; these six
 * were `int *'.  Every one of the eight returns `&graph[v][...]', i.e. the
 * address of a graph cell, so upstream simply spelled the same type two ways
 * on two adjacent lines and a VAX could not tell them apart.
 *
 * With VERT widened they differ, and the consequence is not a warning but
 * SILENT HALF-WIDTH ACCESS: `#define BEGCODE(v) *vxpart(v,...)' dereferences
 * an `int *' into an 8-byte cell, so `BEGCODE(num) = stcode' (1.recog.c:109)
 * stores only the LOW HALF of a string pointer and reading it back yields a
 * truncated address.  Measured: SIGSEGV inside _doprnt on `%s', at an address
 * that CHANGED BETWEEN RUNS (0x4b4686c, then 0x1de0686c) -- ASLR varying,
 * which is what says it is a real heap pointer with its top half gone rather
 * than a constant.
 *
 * TWO THINGS MADE THIS HARD TO SEE.  v8cc warns on a pointer mismatch in an
 * assignment (`illegal pointer combination, op =') and NOT in a `return', so
 * six functions returning VERT * from an int * declaration were silent -- the
 * whole-module warning diff named exactly one new site, and it was elsewhere.
 * And tests/trunc-sweep.awk reads CALL SITES, so it cannot see a callee
 * narrowing its own result either; it reported zero hits over this binary
 * throughout, correctly.  Neither instrument covers the return direction.
 */
extern VERT *vxpart(), *negpart(), *predic(), *expres(), *level(), *stlfmt();
/* node parts */
#define FIXED 4		/* number of wds needed in every node */
#define NTYPE(v)	graph[v][0]
#define BEGCOM(v)	graph[v][1]
#define RSIB(v)	graph[v][2]
#define REACH(v)	graph[v][3]
#define LCHILD(v,i)	*lchild(v,i)
#define CHILDNUM(v)	childper[NTYPE(v)]
#define ARC(v,i)	*arc(v,i)
#define ARCNUM(v)	*((arcsper[NTYPE(v)] >= 0) ? &arcsper[NTYPE(v)]: &graph[v][-arcsper[NTYPE(v)]])

/* STLNVX, FMTVX parts */
#define BEGCODE(v)	*stlfmt(v,0)		/* 1st char of line on disk or address of string */
#define ONDISK(v)	*stlfmt(v,1)		/* FALSE if in core,# of lines on disk otherwise */
#define CODELINES(v)		*vxpart(v,STLNVX,2)		/* # of statements stored in node */

/* IOVX parts */
#define FMTREF(v)	*vxpart(v,IOVX,0)	/* FMTVX associated with i/o statememt */
#define PRERW(v)	*vxpart(v,IOVX,1)	/* string occurring in i/o statement before parts with labels */
#define POSTRW(v)	*vxpart(v,IOVX,2)	/* string occurring in i/o statement after parts wih labels */
#define ENDEQ	1		/* arc number associated with endeq */
#define ERREQ	2		/* arc number associated wth erreq */

/* ITERVX parts */
#define NXT(v)	*vxpart(v,ITERVX,0)		/* THENVX containing condition for iteration for WHILE or UNTIL */
#define FATH(v) *vxpart(v,ITERVX,1)		/* father of v */
#define LPRED(v) *vxpart(v,ITERVX,2)		/* loop predicate for WHILE, UNTIL */

/*DOVX parts */
#define INC(v)	*vxpart(v,DOVX,0)		/* string for iteration condition of DO */

/* IFVX,THENVX parts */
#define PRED(v)		*predic(v)	/* string containing predicate */
#define NEG(v)			*negpart(v)		/* TRUE if predicate negated */
#define THEN	0		/* arc number of true branch */
#define ELSE 1		/* arc number of false branch */

/* miscellaneous parts */
#define EXP(v)	*expres(v)		/* expression - ASVX, COMPVX, ASGOVX, SWCHVX, ICASVX */
#define LABREF(v)	*vxpart(v,ASVX,1)		/* node referred to by label in ASSIGN statement */


/* BRKVX, NXTVX parts */
#define LEVEL(v)	*level(v)

/* also COMPVX, ASGOVX, SWCHVX, and DUMVX contain wd for number of arcs */
/* location of this wd specified by negative entry in arcsper */
/*
 * PORT: `VERT arcsper[]', was `int' -- forced by ARCNUM above, which is a
 * ternary yielding EITHER `&arcsper[t]' or `&graph[v][-arcsper[t]]'.  The two
 * arms are interchangeable by construction: a non-negative entry IS the arc
 * count for that node type, a negative one is the OFFSET of the count inside
 * the node's own storage.  So an arcsper cell and a graph cell are the same
 * kind of thing and must be the same width, or the ternary has no type.
 */
extern VERT arcsper[TYPENUM];

/* also nodes contain wds for children as specified by childper */
extern childper[TYPENUM];


/* switches */
extern int intcase, arbcase, whiloop, invelse, exitsize, maxnode,
	maxhash, progress, labinit, labinc, inputform, debug,levbrk,levnxt,mkunt;

/* arrays */
/*
 * PORT: `VERT *after', was `int *' -- and this is UPSTREAM'S OWN LATENT
 * CONTRADICTION, not a consequence of widening VERT.  One array is declared
 * in two headers at two types: `extern VERT *after' (2.def.h:2) and
 * `extern int *after' (here).  It is DEFINED `VERT *after' at 2.main.c:5,
 * so 2.def.h is the correct one and this line was always the wrong one.
 *
 * It cost nothing for forty years because `#define VERT int' made the two
 * spellings the same type, so no compiler had cause to speak.  Widening VERT
 * is what made them disagree -- seven translation units refused at once.
 * The same shape as sys/fblk.h measuring 716 by coincidence: right by an
 * accident of two types coinciding, and invisible until one of them moved.
 */
extern VERT *after;
extern char *typename[];

struct list {
	VERT elt;
	struct list *nxtlist;
	};
struct list *append(), *consl();
extern VERT retvert, stopvert;	/* specifies unique return and stop vertices */
extern VERT START;
extern int progtype;		/* type of program - main or sub or blockdata */
#define sub	1
#define blockdata	2

extern FILE *infd, *debfd, *outfd;

/*
 * PORT: the allocators, declared so every caller knows they return POINTERS.
 * 0.alloc.c defines them; without these each caller assumed `int' and
 * truncated a heap address to 32 bits.  See the note at the top of 0.alloc.c
 * for the measurement.  This header is the right place because upstream's own
 * makefile already says `main.o $(0FILES.o) $(1FILES.o) ...: def.h'.
 */
char *challoc();
int *balloc(), *talloc();
VERT *galloc();			/* PORT: was `int *' -- allocates graph rows */
struct coreblk *morespace();
