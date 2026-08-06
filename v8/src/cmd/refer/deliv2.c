# include "stdio.h"
hash (s)
	char *s;
{
int c, n;
for(n=0; c= *s; s++)
	n += (c*n+ c << (n%4));
return(n>0 ? n : -n);
}
err (s, a)
	char *s;
{
fprintf(stderr, "Error: ");
fprintf(stderr, s, a);
putc('\n', stderr);
exit(1);
}
prefix(t, s)
	char *t, *s;
{
int c, d;
/* refer5.c:93 does prefix(".[", lookat()), and lookat() returns NULL at end of
   input -- so the last citation in a file reaches here with s == 0.  On the VAX
   that read address 0, which was inside the text segment and readable, so the
   comparison simply failed and this returned 0.  macOS keeps page 0 unmapped,
   so it traps.  Returning 0 reproduces the VAX's observable answer.
   See PORTING.md */
if (s == 0) return (0);
while ( (c= *t++) == *s++)
	if (c==0) return(1);
return(c==0 ? 1: 0);
}
char *
mindex(s, c)
	char *s;
{
register char *p;
for( p=s; *p; p++)
	if (*p ==c)
		return(p);
return(0);
}
/* LP64: was `zalloc(m,n)' returning int, with `int t' holding calloc's
   result -- every allocation in refer came back with its top 32 bits gone.
   Its three callers already declare it as returning a pointer.  See PORTING.md */
char *
zalloc(m,n)
{
	char *t;
	extern char *calloc();
# if D1
fprintf(stderr, "calling calloc for %d*%d bytes\n",m,n);
# endif
t = calloc(m,n);
# if D1
fprintf(stderr, "calloc returned %lo\n", (long)t);
# endif
return(t);
}
