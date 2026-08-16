/* t..c : external declarations */

# include "stdio.h"
# include "ctype.h"

# define MAXLIN 250
# define MAXHEAD 44
# define MAXCOL 30
 /* Do NOT make MAXCOL bigger with adjusting nregs[] in tr.c */
# define MAXCHS 2000
# define MAXRPT 100
# define CLLEN 10
# define SHORTLINE 4
extern int nlin, ncol, iline, nclin, nslin;

extern int (*style)[MAXHEAD];
extern char (*font)[MAXHEAD][2];
extern char (*csize)[MAXHEAD][4];
extern char (*vsize)[MAXHEAD][4];
extern char (*cll)[CLLEN];
extern int (*flags)[MAXHEAD];
# define ZEROW 001
# define HALFUP 002
# define CTOP 004
# define CDOWN 010
extern int stynum[];
extern int qcol;
extern int *doubled, *acase, *topat;
extern int F1, F2;
extern int (*lefline)[MAXHEAD];
extern int fullbot[];
extern char *instead[];
extern int expflg;
extern int ctrflg;
extern int evenflg;
extern int *evenup;
extern int boxflg;
extern int dboxflg;
extern int linsize;
extern int tab;
extern int pr1403;
extern int linsize, delim1, delim2;
extern int allflg;
extern int textflg;
extern int left1flg;
extern int rightl;
struct colstr {char *col, *rcol;};
extern struct colstr *table[];
extern char *cspace, *cstore;
extern char *exstore, *exlim, *exspace;
extern int *sep;
extern int *used, *lused, *rused;
extern int linestop[];
/*
 * PORT: `long leftover', was `int'.  The tm.c defect a second time, in the
 * same program and with the same flag-and-pointer idiom -- t5.c:25 is
 * `leftover=(int)cstore' and t5.c:15 is `leftover=0', so one variable is both
 * a boolean (t7.c:18 `if (leftover)') and the address of the line that did not
 * fit.  t9.c:14 then passes it to `domore(dataln) char *dataln;' which hands
 * it to prefix(), and prefix dereferences it.  Measured: EXC_BAD_ACCESS at
 * 0x4ac8141, a 32-bit value, in prefix.
 *
 * BOTH DECLARATIONS MOVE, and this one is in an #include'd non-header, so
 * every object in tbl sees it -- which is what makes the coupling real here
 * where qed's savint had none.  The cast at t5.c:25 has to move too: an
 * explicit (int) truncates before the value is widened into the long.
 */
extern long leftover;
extern char *last, *ifile;
extern int texname;
extern int texct, texmax;
extern char texstr[];
extern int linstart;


extern FILE *tabin, *tabout;
# define CRIGHT 2
# define CLEFT 0
# define CMID 1
# define S1 31
# define S2 32
# define S3 33
# define TMP 38
#define S9 39
# define SF 35
# define SL 34
# define LSIZE 33
# define SIND 37
# define SVS 36
/* this refers to the relative position of lines */
# define LEFT 1
# define RIGHT 2
# define THRU 3
# define TOP 1
# define BOT 2
