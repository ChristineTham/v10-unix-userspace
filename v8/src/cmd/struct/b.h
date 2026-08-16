/*
 * PORT: `char *malloc()', which none of b.h's four includers declared.
 * beautify builds its whole parse tree out of undeclared malloc: tree.c:12
 * `p = malloc(sizeof(*p))' is every NODE, tree.c:16 and :75,:89,:95 are every
 * literal string, and beauty.y:304 is the output buffer.  Each came back
 * through an implicit int, so each was a heap address with its top half gone.
 *
 * Declared HERE rather than per file because upstream's own makefile already
 * makes b.h the shared dependency of exactly the files that need it
 * (`lextab.o tree.o beauty.y: b.h'), which is the same argument def.h carries
 * for structure's allocators.
 */
char *malloc();

extern int xxindent, xxval, newflag, xxmaxchars, xxbpertab;
extern int xxlineno;		/* # of lines already output */
#define xxtop	100		/* max size of xxstack */
extern int xxstind, xxstack[xxtop], xxlablast, xxt;
struct node
	{int op;
	char *lit;
	struct node *left;
	struct node *right;
	};
