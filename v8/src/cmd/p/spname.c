#include <sys/types.h>
#include <sys/dir.h>
/*
 * char *spname(name)
 *	char name[];
 *
 * returns pointer to correctly spelled name,
 * or 0 if no reasonable name is found;
 * uses a static buffer to store correct name,
 * so copy it if you want to call the routine again.
 */
char *
spname(name)
	register char *name;
{
/*
 * PORT: newname is 1024, not 80, and the loop is bounded.
 *
 * THIS IS THE THIRD COPY OF spname IN THE TREE and it is not the one already
 * fixed.  sh's (src/cmd/sh/spname.c) is the later rewrite: <ndir.h>, opendir,
 * readdir, a `score' out-parameter, newname[128] AND a bound test.  This one is
 * the original -- <sys/dir.h> and a raw read(2) of struct direct -- and the
 * difference that matters is that it has NO bound test at all.
 *
 * 80 is a sentence about DIRSIZ.  A component copied out of best[] can be
 * DIRSIZ characters, so upstream's 80 held about five components of 14; this
 * port raises DIRSIZ to 254 (src/include/sys/dir.h:32), so ONE component can
 * overrun it by 175 bytes and an ordinary /usr/local/share/... path overruns it
 * on names of any length.  Exactly mv(1)'s MAXN-DIRSIZ-2: the constant encodes
 * a relationship with a number this port changed.
 *
 * THE ABSENCE OF THE GUARD IS WHAT HIDES IT, which is the inverse of how the
 * same change was found in sh.  There, raising DIRSIZ made `newname[128-DIRSIZ-2]'
 * evaluate to -132 and spname returned 0 on the first pass, so cd stopped
 * correcting LOUDLY.  Here there is nothing to go negative, so the identical
 * change to DIRSIZ makes this copy silently worse instead of visibly broken.
 * 1024 is macOS's PATH_MAX, the same number sh, mv, mkdir and rmdir use here.
 *
 * best[] does NOT need sh's fix, and for a reason worth recording: sh's copy
 * carries upstream's `#undef DIRSIZ / #define DIRSIZ 14', because <ndir.h>
 * redefines DIRSIZ as the function-like DIRSIZ(dp) -- so best[] stayed 15 bytes
 * while readdir returned up to 254 characters, which is the measured one-byte
 * overflow recorded there.  This copy includes <sys/dir.h>, has no #undef, and
 * therefore sizes best[] at the real 255.  The two files needed opposite halves
 * of the same fix.
 *
 * spname is reachable from the ordinary use of the program: spopen() calls it
 * whenever fopen fails, which is what `p mistypedname' does.
 */
	register char *p, *q, *new;
	register d, nd, dir;
	static char newname[1024], guess[DIRSIZ+1], best[DIRSIZ+1];
	static struct{
		ino_t ino;
		char name[DIRSIZ+1];
	} nbuf;

	new = newname;
	nbuf.name[DIRSIZ] = '\0';
	for(;;){
		if (new >= &newname[1024-DIRSIZ-2])
			return((char *)0);
		while(*name == '/')
			*new++ = *name++;
		*new = '\0';
		if(*name == '\0')
			return(newname);
		p = guess;
		while(*name!='/' && *name!='\0'){
			if(p != guess+DIRSIZ)
				*p++ = *name;
			name++;
		}
		*p = '\0';
		if((dir=open(newname,0)) < 0)
			return((char *)0);
		d = 3;
		while(read(dir, &nbuf, sizeof (struct direct)) == sizeof (struct direct))
			if(nbuf.ino){
				nd=SPdist(nbuf.name, guess);
				if(nd<=d && nd!=3) {	/* <= to avoid "." */
					p = best;
					q = nbuf.name;
					do; while(*p++ = *q++);
					d = nd;
					if(d == 0)
						break;
				}
			}
		close(dir);
		if(d == 3)
			return(0);
		p = best;
		do; while(*new++ = *p++);
		--new;
	}
}
/*
 * very rough spelling metric
 * 0 if the strings are identical
 * 1 if two chars are interchanged
 * 2 if one char wrong, added or deleted
 * 3 otherwise
 */
SPdist(s, t)
	register char *s, *t;
{
	while(*s++ == *t)
		if(*t++ == '\0')
			return(0);
	if(*--s){
		if(*t){
			if(s[1] && t[1] && *s==t[1] && *t==s[1] && SPeq(s+2,t+2))
				return(1);
			if(SPeq(s+1, t+1))
				return(2);
		}
		if(SPeq(s+1, t))
			return(2);
	}
	if(*t && SPeq(s, t+1))
		return(2);
	return(3);
}
SPeq(s, t)
	register char *s, *t;
{
	while(*s++ == *t)
		if(*t++ == '\0')
			return(1);
	return(0);
}
