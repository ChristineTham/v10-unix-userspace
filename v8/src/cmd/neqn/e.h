#include <stdio.h>

#define	FATAL	1
#define	ROM	'1'
#define	ITAL	'1'
#define	BLD	'1'

#define	VERT(n)	(20 * (n))
#define	EFFPS(p)	((p) >= 6 ? (p) : 6)

extern int	dbg;
extern int	ct;
extern int	lp[];
extern int	used[];	/* available registers */
extern int	ps;	/* dflt init pt size */
extern int	deltaps;	/* default change in ps */
extern int	gsize;	/* global size */
extern int	gfont;	/* global font */
extern int	ft;	/* dflt font */
extern FILE	*curfile;	/* current input file */
extern int	ifile;	/* input file number */
extern int	linect;	/* line number in current file */
extern int	eqline;	/* line where eqn started */
extern int	svargc;
extern char	**svargv;
extern int	eht[];
extern int	ebase[];
extern int	lfont[];
extern int	rfont[];
/*
 * PORT: long, to match YYSTYPE.
 *
 * yacc's value stack holds pointers in this grammar -- text() is handed the
 * char * the lexer just built -- so this port's yacc emits `#define YYSTYPE
 * long' (cmd/yacc/y2.c).  These two declarations have to agree with it or the
 * parser and the rest of neqn disagree about the width of every value on the
 * stack.  cmd/eqn/e.h carries the identical change for the identical reason.
 *
 * yypv IS DELIBERATELY LEFT `int *', AND IT IS NOT A THIRD INSTANCE OF THIS.
 * It looks like one -- a pointer that walks the value stack, which would
 * stride four bytes over eight-byte cells -- but the object it names does not
 * exist: in the yaccpar this port uses it is `register YYSTYPE *yypv' INSIDE
 * yyparse() (yaccpar:26), i.e. a local, and an older yacc's skeleton is where
 * this extern came from.  Measured: the only occurrence of the name in the
 * whole of neqn is this line, so nothing references it and no code is emitted
 * for it.  Widening it would not be forced by the target, which is what S1
 * asks, and would quietly suggest the symbol is real.
 */
extern long	yyval;
extern int	*yypv;
extern long	yylval;
extern int	eqnreg, eqnht;
extern int	lefteq, righteq;
extern int	lastchar;	/* last character read by lex */
extern int	markline;	/* 1 if this EQ/EN contains mark or lineup */

typedef struct s_tbl {
	char	*name;
	char	*defn;
	struct s_tbl *next;
} tbl;
