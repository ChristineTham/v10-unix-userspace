#include <sys/types.h>
#include <ndir.h>

/*
 * char *spname(name, score)
 *	char name[];
 *	int *score;
 *
 * returns pointer to correctly spelled name,
 * or 0 if no reasonable name is found;
 * uses a static buffer to store correct name,
 * so copy it if you want to call the routine again.
 * score records how good the match was; ignore if NULL return.
 */
char *
spname(name, score)
	register char *name;
	int *score;
{
/*
 * PORT: DIRSIZ 254, not V7's 14, and newname 1024, not 128.
 *
 * The #undef is upstream's and stays: <ndir.h>:38-39 redefines DIRSIZ as the
 * function-like DIRSIZ(dp), so guess[DIRSIZ+1] needs a plain number here.  But
 * 14 was a statement about how long a name could be, and this port raises that
 * to 254 (src/include/sys/dir.h:32), so the number has to move with it.
 *
 * best[] is what forced it.  The copy below is `do; while(*p++ = *q++);' with
 * no bound at all -- correct on a V8 where readdir could not return more than
 * DIRSIZ characters, and a global-buffer-overflow here.  MEASURED under
 * AddressSanitizer: "WRITE of size 1 ... 0 bytes after global variable
 * `spname.best' of size 15".
 *
 * Exactly ONE byte, and the bound is not in the copy -- it is in SPdist, one
 * function away, which gates the copy on a score under 3 and so will not
 * tolerate a name more than one character longer than the guess.  The copy
 * that fills guess *is* bounded, which is why reading this function top to
 * bottom finds a bound and stops looking.
 *
 * ONE BYTE, BUT REACHED FAR MORE EASILY THAN THAT SOUNDS, because the loop
 * walks EVERY component and an exact match scores 0 and copies too.  So the
 * condition is only: some component of the path is >= DIRSIZ characters --
 * which truncates guess to exactly DIRSIZ -- and its directory holds a
 * DIRSIZ+1-character sibling sharing those characters.  Instrumented, the
 * first probe run for this fix tripped on a path nobody had constructed:
 * component `-Users-christie-Repositories-Apps-v10-unix-userspace' truncated
 * to guess `-Users-christi', beside a real directory `-Users-christie', 15
 * characters, SPdist 2.  The target component never came into it.
 *
 * newname is the mv(1) shape: 128 is not independent, it is a sentence about
 * DIRSIZ, spelled `newname[128-DIRSIZ-2]' at :31.  Raising DIRSIZ alone would
 * make that -132, and the >= would be true on the first pass, so spname would
 * return 0 always and cd would silently stop correcting -- mv's guard became
 * -156 and refused every directory move.  1024 is macOS's PATH_MAX, the same
 * number src/cmd/mv/mv.c:32, mkdir.c:49 and rmdir.c:43 use, and it keeps
 * 1024-DIRSIZ-2 = 768 positive and the relationship exact.
 */
#undef	DIRSIZ
#define	DIRSIZ	254
	register char *p, *q, *new;
	register d, nd;
	register DIR *dirf;
	register struct direct *ep;
	static char newname[1024], guess[DIRSIZ+1], best[DIRSIZ+1];

	new = newname;
	*score = 0;
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
		if((dirf=opendir(newname,0)) == NULL)
			return((char *)0);
		d = 3;
		while(ep = readdir(dirf)) {
			nd = SPdist(ep->d_name, guess);
			if (nd>0
			 && (SPeq(".", ep->d_name) || SPeq("..", ep->d_name)))
				continue;
			if(nd<d) {
				p = best;
				q = ep->d_name;
				do; while(*p++ = *q++);
				d = nd;
				if(d == 0)
					break;
			}
		}
		closedir(dirf);
		if(d == 3)
			return((char *)0);
		p = best;
		*score += d;
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

