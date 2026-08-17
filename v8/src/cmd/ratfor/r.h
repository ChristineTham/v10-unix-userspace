#include <stdio.h>
#include <ctype.h>
#include "y.tab.h"

#
#define	putbak(c)	*ip++ = c
/*	#define	getchr()	(ip>ibuf?*--ip: getc(infile[infptr]))	*/

#define	LET	1
#define	DIG	2
#define	COMMENT	'#'
#define	QUOTE	'"'

extern int	transfer;

#define	INDENT	3	/* indent delta */
#define	CONTFLD	6	/* default position of continuation character */
extern	int	contfld;	/* column for continuation char */
extern	int	contchar;
extern	int	dbg;
/*
 * PORT: upstream declares all three of these `int'.  yacc DEFINES yyval and
 * yylval -- src/cmd/yacc/y2.c emits `YYSTYPE yylval, yyval;' preceded by
 * `#define YYSTYPE long' for an untyped grammar (the ntypes==0 arm) -- so the
 * objects are eight bytes and these declarations described four.  A
 * DECLARATION THAT LIES ABOUT A TYPE, the same class as awk's `extern int
 * yylval' and pic's `extern float atof()'.
 *
 * yylval is not merely a width.  rlex.c's yylex() stores the address of the
 * token buffer into it, and the grammar hands that straight to outcode(xp)
 * char *xp, which walks it as a string.  The token that carries it is GOK --
 * ratfor's catch-all for ordinary Fortran text -- so this is the main path
 * through the program, not an edge case.  Measured: revert this one word to
 * `int' and ratfor dies of SIGSEGV on its first line of input.
 *
 * rlex.c IS BYTE-IDENTICAL TO UPSTREAM, and its `yylval = (int) str' is left
 * exactly as Bell Labs wrote it.  That cast looks like a second truncation and
 * measurably is not: v8cc emits `adrp/add/str x10,[x9]' -- a full 64-bit store
 * with no narrowing instruction -- for the (int) and (long) spellings alike,
 * so the two objects are byte-identical and the change would not be forced by
 * the target.  Same finding as awk's maketab.c, and settled the same way, by
 * diffing `cc -S' output rather than by reading the C.
 *
 * yyval carries only label numbers from genlab(), which are small.  It is
 * still wrong, and the two lies COOPERATE: yaccpar does `yyval = yylval', so
 * once a GOK token has been read yyval's upper half holds the upper half of
 * that pointer, and a subsequent `yyval = genlab(3)' written through an `int'
 * declaration replaces only the lower half.  Fixing either alone leaves the
 * other corrupting it.
 *
 * yypv is deliberately UNCHANGED.  It is `register YYSTYPE *yypv' -- a LOCAL
 * inside yyparse() -- so no such object exists at file scope and this
 * declaration names nothing.  It survives only because nothing references it;
 * a single use would fail the link with an undefined _yypv.  Widening it would
 * invent a claim rather than repair one, so it stays as upstream wrote it.
 */
extern	long	yyval;
extern	int	*yypv;
extern	long	yylval;
extern	int	errorflag;

extern	char	comment[];	/* save input comments here */
extern	int	comptr;	/* next free slot in comment */
extern	int	printcom;	/* print comments, etc., if on */
extern	int	indent;	/* level of nesting for indenting */

extern	char	ibuf[];
extern	char	*ip;

extern	FILE	*outfil;	/* output file id */
extern	FILE	*infile[];
extern	char	*curfile[];
extern	int	infptr;
extern	int	linect[];

extern	char	fcname[];

extern	int	svargc;
extern	char	**svargv;

#define EOS 0
#define	HSHSIZ	101
struct	nlist {
	char	*name;
	char	*def;
	int	ydef;
	struct	nlist *next;
};

struct nlist	*lookup();
char	*install();
char	*malloc();
extern	char	*fcnloc;
extern	char	*FCN1loc;

