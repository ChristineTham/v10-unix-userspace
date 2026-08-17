/*	@(#)fmt.h	1.2	*/
/*	3.0 SID #	1.2	*/
/* PORT: p1, p2 and p3 CARRY POINTERS AS WELL AS COUNTS, and on this target an
   int cannot hold one.  fmt.c:129 is `op_gen(APOS,(int)s,0,0)' and fmt.c:122 is
   `op_gen(H,n,(int)(s+1),0)' -- a char * into the format string, cast to int and
   cast back by wrt_AP() and wrt_H().  Exact on a VAX, where sizeof(int) was
   sizeof(char *); here the top half is lost and the format interpreter faults.

   Measured before the fix, with the crash report giving the whole path:
   main -> e_wsfe -> en_fio -> do_fio -> w_ned -> wrt_AP, EXC_BAD_ACCESS at
   0x4feb9e9 -- a truncated address.  `write(6,10)' with `format(1x,i3)' ran
   correctly throughout, because an integer edit descriptor puts a WIDTH in p1
   and never a pointer, which is why the fault looked like a character-handling
   bug rather than a width one.

   Widened rather than unioned: what upstream means by these fields is "a
   machine word", and a width fits in a long as happily as an address does.
   That is struct(1)'s VERT lesson -- when a type is punned, widen the TYPE and
   not the uses.  Safe because struct syl has ONE end: it is the interpreter's
   own state, not a record on disk and not across the shim seam.
   src/libF77/PORTING.md. */
struct syl
{	int op;
	long p1,p2,p3;
};
#define RET 1
#define REVERT 2
#define GOTO 3
#define X 4
#define SLASH 5
#define STACK 6
#define I 7
#define ED 8
#define NED 9
#define IM 10
#define APOS 11
#define H 12
#define TL 13
#define TR 14
#define T 15
#define COLON 16
#define S 17
#define SP 18
#define SS 19
#define P 20
#define BN 21
#define BZ 22
#define F 23
#define E 24
#define EE 25
#define D 26
#define G 27
#define GE 28
#define L 29
#define A 30
#define AW 31
#define O 32
#define NONL 33
extern struct syl syl[];
extern int pc,parenlvl,revloc;
extern int (*doed)(),(*doned)();
extern int (*dorevert)(),(*donewrec)(),(*doend)();
extern flag cblank,cplus,workdone, nonl;
extern int dummy();
extern char *fmtbuf;
extern int scale;
typedef union
{	float pf;
	double pd;
} ufloat;
typedef union
{	short is;
	char ic;
	int il;		/* PORT: the FORTRAN INTEGER arm; see fio.h */
} uint;
#define GET(x) if((x=(*getn)())<0) return(x)
#define VAL(x) (x!='\n'?x:' ')
#define PUT(x) (*putn)(x)
extern int cursor;
