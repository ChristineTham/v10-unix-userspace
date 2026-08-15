static char *sccsid = "@(#)find.c	4.5 (Berkeley) 3/31/82";
/*	find	COMPILE:	cc -o find -s -O -i find.c -lS	*/
#include <stdio.h>
#include <sys/types.h>
#include <sys/dir.h>
#include <sys/stat.h>
#define A_DAY	86400L /* a day full of seconds */
#define EQ(x, y)	(strcmp(x, y)==0)

int	Randlast;
char	Pathname[1024];	/* PORT: was 200 -- see find.PORTING.md */

struct anode {
	int (*F)();
	struct anode *L, *R;
} Node[100];
int Nn;  /* number of nodes */
char	*Fname;
long	Now;
int	Argc,
	Ai,
	Pi;
char	**Argv;
/* cpio stuff */
int	Cpio;
short	*Buf, *Dbuf, *Wp;
int	Bufsize = 5120;
int	Wct = 2560;

long	Newer;

struct stat Statb;

struct	anode	*exp(),
		*e1(),
		*e2(),
		*e3(),
		*mk();
char	*nxtarg();
char	Home[1024];	/* PORT: was 128 -- see find.PORTING.md */
long	Blocks;
char *rindex();
char *sbrk();
main(argc, argv) char *argv[];
{
	struct anode *exlist;
	int paths;
	register char *cp, *sp = 0;
	FILE *pwd, *popen();

	time(&Now);
	pwd = popen("pwd", "r");
	fgets(Home, 128, pwd);
	pclose(pwd);
	Home[strlen(Home) - 1] = '\0';
	Argc = argc; Argv = argv;
	if(argc<3) {
usage:		fprintf(stderr, "Usage: find path-list predicate-list\n");
		exit(1);
	}
	for(Ai = paths = 1; Ai < (argc-1); ++Ai, ++paths)
		if(*Argv[Ai] == '-' || EQ(Argv[Ai], "(") || EQ(Argv[Ai], "!"))
			break;
	if(paths == 1) /* no path-list */
		goto usage;
	if(!(exlist = exp())) { /* parse and compile the arguments */
		fprintf(stderr, "find: parsing error\n");
		exit(1);
	}
	if(Ai<argc) {
		fprintf(stderr, "find: missing conjunction\n");
		exit(1);
	}
	for(Pi = 1; Pi < paths; ++Pi) {
		sp = 0;
		chdir(Home);
		strcpy(Pathname, Argv[Pi]);
		if(cp = rindex(Pathname, '/')) {
			sp = cp + 1;
			*cp = '\0';
			if(chdir(*Pathname? Pathname: "/") == -1) {
				fprintf(stderr, "find: bad starting directory\n");
				exit(2);
			}
			*cp = '/';
		}
		Fname = sp? sp: Pathname;
		descend(Pathname, Fname, exlist); /* to find files that match  */
	}
	if(Cpio) {
		strcpy(Pathname, "TRAILER!!!");
		Statb.st_size = 0;
		cpio();
		printf("%D blocks\n", Blocks*10);
	}
	exit(0);
}

/* compile time functions:  priority is  exp()<e1()<e2()<e3()  */

struct anode *exp() { /* parse ALTERNATION (-o)  */
	int or();
	register struct anode * p1;

	p1 = e1() /* get left operand */ ;
	if(EQ(nxtarg(), "-o")) {
		Randlast--;
		return(mk(or, p1, exp()));
	}
	else if(Ai <= Argc) --Ai;
	return(p1);
}
struct anode *e1() { /* parse CONCATENATION (formerly -a) */
	int and();
	register struct anode * p1;
	register char *a;

	p1 = e2();
	a = nxtarg();
	if(EQ(a, "-a")) {
And:
		Randlast--;
		return(mk(and, p1, e1()));
	} else if(EQ(a, "(") || EQ(a, "!") || (*a=='-' && !EQ(a, "-o"))) {
		--Ai;
		goto And;
	} else if(Ai <= Argc) --Ai;
	return(p1);
}
struct anode *e2() { /* parse NOT (!) */
	int not();

	if(Randlast) {
		fprintf(stderr, "find: operand follows operand\n");
		exit(1);
	}
	Randlast++;
	if(EQ(nxtarg(), "!"))
		return(mk(not, e3(), (struct anode *)0));
	else if(Ai <= Argc) --Ai;
	return(e3());
}
struct anode *e3() { /* parse parens and predicates */
	int exeq(), ok(), glob(),  mtime(), atime(), user(),
		group(), size(), perm(), links(), print(),
		type(), ino(), cpio(), newer();
	struct anode *p1;
	int i;
	register char *a, *b, s;

	a = nxtarg();
	if(EQ(a, "(")) {
		Randlast--;
		p1 = exp();
		a = nxtarg();
		if(!EQ(a, ")")) goto err;
		return(p1);
	}
	else if(EQ(a, "-print")) {
		return(mk(print, (struct anode *)0, (struct anode *)0));
	}
	b = nxtarg();
	s = *b;
	if(s=='+') b++;
	if(EQ(a, "-name"))
		return(mk(glob, (struct anode *)b, (struct anode *)0));
	else if(EQ(a, "-mtime"))
		return(mk(mtime, (struct anode *)atoi(b), (struct anode *)s));
	else if(EQ(a, "-atime"))
		return(mk(atime, (struct anode *)atoi(b), (struct anode *)s));
	else if(EQ(a, "-user")) {
		if((i=getunum("/etc/passwd", b)) == -1) {
			if(gmatch(b, "[0-9]*"))
				return mk(user, (struct anode *)atoi(b), (struct anode *)s);
			fprintf(stderr, "find: cannot find -user name\n");
			exit(1);
		}
		return(mk(user, (struct anode *)i, (struct anode *)s));
	}
	else if(EQ(a, "-inum"))
		return(mk(ino, (struct anode *)atoi(b), (struct anode *)s));
	else if(EQ(a, "-group")) {
		if((i=getunum("/etc/group", b)) == -1) {
			if(gmatch(b, "[0-9]*"))
				return mk(group, (struct anode *)atoi(b), (struct anode *)s);
			fprintf(stderr, "find: cannot find -group name\n");
			exit(1);
		}
		return(mk(group, (struct anode *)i, (struct anode *)s));
	} else if(EQ(a, "-size"))
		return(mk(size, (struct anode *)atoi(b), (struct anode *)s));
	else if(EQ(a, "-links"))
		return(mk(links, (struct anode *)atoi(b), (struct anode *)s));
	else if(EQ(a, "-perm")) {
		for(i=0; *b ; ++b) {
			if(*b=='-') continue;
			i <<= 3;
			i = i + (*b - '0');
		}
		return(mk(perm, (struct anode *)i, (struct anode *)s));
	}
	else if(EQ(a, "-type")) {
		i = s=='d' ? S_IFDIR :
		    s=='b' ? S_IFBLK :
		    s=='c' ? S_IFCHR :
		    s=='L' ? S_IFLNK :
		    s=='f' ? 0100000 :
		    0;
		return(mk(type, (struct anode *)i, (struct anode *)0));
	}
	else if (EQ(a, "-exec")) {
		i = Ai - 1;
		while(!EQ(nxtarg(), ";"));
		return(mk(exeq, (struct anode *)i, (struct anode *)0));
	}
	else if (EQ(a, "-ok")) {
		i = Ai - 1;
		while(!EQ(nxtarg(), ";"));
		return(mk(ok, (struct anode *)i, (struct anode *)0));
	}
	else if(EQ(a, "-cpio")) {
		if((Cpio = creat(b, 0666)) < 0) {
			fprintf(stderr, "find: cannot create < %s >\n", s);
			exit(1);
		}
		Buf = (short *)sbrk(512);
		Wp = Dbuf = (short *)sbrk(5120);
		return(mk(cpio, (struct anode *)0, (struct anode *)0));
	}
	else if(EQ(a, "-newer")) {
		if(stat(b, &Statb) < 0) {
			fprintf(stderr, "find: cannot access < %s >\n", b);
			exit(1);
		}
		Newer = Statb.st_mtime;
		return mk(newer, (struct anode *)0, (struct anode *)0);
	}
err:	fprintf(stderr, "find: bad option < %s >\n", a);
	exit(1);
}
struct anode *mk(f, l, r)
int (*f)();
struct anode *l, *r;
{
	Node[Nn].F = f;
	Node[Nn].L = l;
	Node[Nn].R = r;
	return(&(Node[Nn++]));
}

char *nxtarg() { /* get next arg from command line */
	static strikes = 0;

	if(strikes==3) {
		fprintf(stderr, "find: incomplete statement\n");
		exit(1);
	}
	if(Ai>=Argc) {
		strikes++;
		Ai = Argc + 1;
		return("");
	}
	return(Argv[Ai++]);
}

/* execution time functions */
and(p)
register struct anode *p;
{
	return(((*p->L->F)(p->L)) && ((*p->R->F)(p->R))?1:0);
}
or(p)
register struct anode *p;
{
	 return(((*p->L->F)(p->L)) || ((*p->R->F)(p->R))?1:0);
}
not(p)
register struct anode *p;
{
	return( !((*p->L->F)(p->L)));
}
/*
 * PORT (LP64): THESE ACCESSORS PUN `struct anode' AS INTS, and on LP64 that
 * reads the WRONG FIELD.  anode is `{ int (*F)(); struct anode *L, *R; }' --
 * three POINTER-sized members here, four-byte members on a VAX -- so a pun of
 * `{ int f, t, s; }' finds `t' at offset 4, which is the upper half of the
 * function pointer, and `s' at 8, which is L.
 *
 * Every predicate that reads its second field was therefore silently false:
 * -type, -perm, -links, -size, -user, -group, -atime, -mtime, -ctime, -exec
 * and -ok.  `find /usr/lib -type f' printed nothing while `-print' alone found
 * 83 entries, which is the shape of the whole class -- a wrong answer with no
 * diagnostic anywhere.
 *
 * -name is the exception and it is an ACCIDENT worth naming: its pun is
 * `{ int f; char *pat; }', and the pointer's own alignment pushes `pat' to
 * offset 8, where L really is.  So the one predicate everybody tries first is
 * the one that works, which is why this survived being built and run.
 *
 * `long' rather than a rewrite to use struct anode directly: this is upstream's
 * idiom in eleven places, the widths are what the target changed, and the
 * fidelity contract asks for the minimum edit that the machine forces.
 */
glob(p)
register struct { long f; char *pat; } *p; 
{
	return(gmatch(Fname, p->pat));
}
print()
{
	puts(Pathname);
	return(1);
}
mtime(p)
register struct { long f, t, s; } *p; 
{
	return(scomp((int)((Now - Statb.st_mtime) / A_DAY), p->t, p->s));
}
atime(p)
register struct { long f, t, s; } *p; 
{
	return(scomp((int)((Now - Statb.st_atime) / A_DAY), p->t, p->s));
}
user(p)
register struct { long f, u, s; } *p; 
{
	return(scomp(Statb.st_uid, p->u, p->s));
}
ino(p)
register struct { long f, u, s; } *p;
{
	return(scomp((int)Statb.st_ino, p->u, p->s));
}
group(p)
register struct { long f, u; } *p; 
{
	return(p->u == Statb.st_gid);
}
links(p)
register struct { long f, link, s; } *p; 
{
	return(scomp(Statb.st_nlink, p->link, p->s));
}
size(p)
register struct { long f, sz, s; } *p; 
{
	return(scomp((int)((Statb.st_size+511)>>9), p->sz, p->s));
}
perm(p)
register struct { long f, per, s; } *p; 
{
	register i;
	i = (p->s=='-') ? p->per : 07777; /* '-' means only arg bits */
	return((Statb.st_mode & i & 07777) == p->per);
}
type(p)
register struct { long f, per, s; } *p;
{
	return((Statb.st_mode&S_IFMT)==p->per);
}
exeq(p)
register struct { long f, com; } *p;
{
	fflush(stdout); /* to flush possible `-print' */
	return(doex(p->com));
}
ok(p)
struct { int f, com; } *p;
{
	char c;  int yes;
	yes = 0;
	fflush(stdout); /* to flush possible `-print' */
	fprintf(stderr, "< %s ... %s > ?   ", Argv[p->com], Pathname);
	fflush(stderr);
	if((c=getchar())=='y') yes = 1;
	while(c!='\n') c = getchar();
	if(yes) return(doex(p->com));
	return(0);
}

#define MKSHORT(v, lv) {U.l=1L;if(U.c[0]) U.l=lv, v[0]=U.s[1], v[1]=U.s[0]; else U.l=lv, v[0]=U.s[0], v[1]=U.s[1];}
union { long l; short s[2]; char c[4]; } U;
long mklong(v)
short v[];
{
	U.l = 1;
	if(U.c[0] /* VAX */)
		U.s[0] = v[1], U.s[1] = v[0];
	else
		U.s[0] = v[0], U.s[1] = v[1];
	return U.l;
}
cpio()
{
#define MAGIC 070707
	struct header {
		short	h_magic,
			h_dev,
			h_ino,
			h_mode,
			h_uid,
			h_gid,
			h_nlink,
			h_rdev;
		short	h_mtime[2];
		short	h_namesize;
		short	h_filesize[2];
		char	h_name[256];
	} hdr;
	register ifile, ct;
	static long fsz;
	register i;

	hdr.h_magic = MAGIC;
	/*
	 * PORT: BOUNDED, because h_name is an ARCHIVE FORMAT field and cannot
	 * grow with Pathname.  Upstream's strcpy was safe by construction --
	 * Pathname[200] fits in h_name[256] -- and raising Pathname to hold a
	 * host component broke that relationship, which is `mv''s MAXN-DIRSIZ-2
	 * lesson: a constant can encode a SENTENCE about two other constants.
	 * A name that will not fit is REPORTED AND SKIPPED rather than
	 * truncated, for mtab.c's reason -- an entry that cannot be described
	 * truthfully must not be described at all, and a short name in a cpio
	 * archive names a different file.
	 */
	{
		char *hn = !strncmp(Pathname, "./", 2)? Pathname+2: Pathname;

		if(strlen(hn) >= sizeof hdr.h_name) {
			fprintf(stderr, "find: name too long for cpio: %s\n", hn);
			return;
		}
		strcpy(hdr.h_name, hn);
	}
	hdr.h_namesize = strlen(hdr.h_name) + 1;
	hdr.h_uid = Statb.st_uid;
	hdr.h_gid = Statb.st_gid;
	hdr.h_dev = Statb.st_dev;
	hdr.h_ino = Statb.st_ino;
	hdr.h_mode = Statb.st_mode;
	MKSHORT(hdr.h_mtime, Statb.st_mtime);
	hdr.h_nlink = Statb.st_nlink;
	fsz = hdr.h_mode & S_IFREG? Statb.st_size: 0L;
	MKSHORT(hdr.h_filesize, fsz);
	hdr.h_rdev = Statb.st_rdev;
	if(EQ(hdr.h_name, "TRAILER!!!")) {
		bwrite((short *)&hdr, (sizeof hdr-256)+hdr.h_namesize);
		for(i = 0; i < 10; ++i)
			bwrite(Buf, 512);
		return;
	}
	if(!mklong(hdr.h_filesize))
		return;
	if((ifile = open(Fname, 0)) < 0) {
cerror:
		fprintf(stderr, "find: cannot copy < %s >\n", hdr.h_name);
		return;
	}
	bwrite((short *)&hdr, (sizeof hdr-256)+hdr.h_namesize);
	for(fsz = mklong(hdr.h_filesize); fsz > 0; fsz -= 512) {
		ct = fsz>512? 512: fsz;
		if(read(ifile, (char *)Buf, ct) < 0)
			goto cerror;
		bwrite(Buf, ct);
	}
	close(ifile);
	return;
}
newer()
{
	return Statb.st_mtime > Newer;
}

/* support functions */
scomp(a, b, s) /* funny signed compare */
register a, b;
register char s;
{
	if(s == '+')
		return(a > b);
	if(s == '-')
		return(a < (b * -1));
	return(a == b);
}

doex(com)
{
	register np;
	register char *na;
	static char *nargv[50];
	static ccode;

	ccode = np = 0;
	while (na=Argv[com++]) {
		if(strcmp(na, ";")==0) break;
		if(strcmp(na, "{}")==0) nargv[np++] = Pathname;
		else nargv[np++] = na;
	}
	nargv[np] = 0;
	if (np==0) return(9);
	if(fork()) /*parent*/ {
#include <signal.h>
		int (*old)() = signal(SIGINT, SIG_IGN);
		int (*oldq)() = signal(SIGQUIT, SIG_IGN);
		wait(&ccode);
		signal(SIGINT, old);
		signal(SIGQUIT, oldq);
	} else { /*child*/
		chdir(Home);
		execvp(nargv[0], nargv, np);
		exit(1);
	}
	return(ccode ? 0:1);
}

getunum(f, s) char *f, *s; { /* find user/group name and return number */
	register i;
	register char *sp;
	register c;
	char str[20];
	FILE *pin;

	i = -1;
	pin = fopen(f, "r");
	c = '\n'; /* prime with a CR */
	do {
		if(c=='\n') {
			sp = str;
			while((c = *sp++ = getc(pin)) != ':')
				if(c == EOF) goto RET;
			*--sp = '\0';
			if(EQ(str, s)) {
				while((c=getc(pin)) != ':')
					if(c == EOF) goto RET;
				sp = str;
				while((*sp = getc(pin)) != ':') sp++;
				*sp = '\0';
				i = atoi(str);
				goto RET;
			}
		}
	} while((c = getc(pin)) != EOF);
 RET:
	fclose(pin);
	return(i);
}

descend(name, fname, exlist)
struct anode *exlist;
char *name, *fname;
{
	int	dir = 0, /* open directory */
		offset,
		dsize,
		entries;
	struct direct dentry[BUFSIZ / sizeof (struct direct)];
	register struct direct	*dp;
	register char *c1, *c2;
	int i;
	int rv = 0;
	char *endofname;

	if(lstat(fname, &Statb)<0) {
		fprintf(stderr, "find: bad status < %s >\n", name);
		return(0);
	}
	(*exlist->F)(exlist);
	if((Statb.st_mode&S_IFMT)!=S_IFDIR)
		return(1);

	for(c1 = name; *c1; ++c1);
	if(*(c1-1) == '/')
		--c1;
	endofname = c1;
	if(chdir(fname) == -1)
		return(0);
	/*
	 * PORT: DRIVEN BY read(), NOT BY st_size.  Upstream bounds this loop with
	 * `offset < dirsize' where dirsize is the lstat above -- exact on a V7
	 * filesystem, where a directory's size IS its records.  It is not exact
	 * here: this shim builds V7 records out of the host's variable-length
	 * entries, so the two numbers are unrelated (nine entries: 2304 bytes of
	 * records, APFS says 288), and only FSTAT reports the snapshot length --
	 * stat and lstat give the host's, deliberately, because doing otherwise
	 * would put a getdirentries loop inside every `ls -l'.  So find read one
	 * block, computed one entry from 288/256, skipped it as `.', and reported
	 * an empty filesystem.
	 *
	 * Ending on a short read needs neither number and cannot disagree with
	 * either.  It also removes the lseek arithmetic's dependence on a fixed
	 * block size, which is why `offset' now advances by what was actually
	 * read.
	 */
	for(offset=0 ; ; offset += dsize) { /* each block */
		if(!dir) {
			if((dir=open(".", 0))<0) {
				fprintf(stderr, "find: cannot open < %s >\n",
					name);
				rv = 0;
				goto ret;
			}
			if(offset) lseek(dir, (long)offset, 0);
		}
		if((dsize=read(dir, (char *)dentry, sizeof dentry))<0) {
			fprintf(stderr, "find: cannot read < %s >\n", name);
			rv = 0;
			goto ret;
		}
		if(dsize == 0)
			break;
		if(dir > 10) {
			close(dir);
			dir = 0;
		}
		/* PORT: was `dsize>>4' -- 16 is sizeof(struct direct) at DIRSIZ 14 */
		for(dp=dentry, entries=dsize/sizeof(struct direct); entries; --entries, ++dp) { /* each directory entry */
			if(dp->d_ino==0
			|| (dp->d_name[0]=='.' && dp->d_name[1]=='\0')
			|| (dp->d_name[0]=='.' && dp->d_name[1]=='.' && dp->d_name[2]=='\0'))
				continue;
			c1 = endofname;
			*c1++ = '/';
			c2 = dp->d_name;
			for(i=0; i<DIRSIZ; ++i)	/* PORT: was 14 */
				if(*c2)
					*c1++ = *c2++;
				else
					break;
			*c1 = '\0';
			if(c1 == endofname) { /* ?? */
				rv = 0;
				goto ret;
			}
			Fname = endofname+1;
			if(!descend(name, Fname, exlist)) {
				*endofname = '\0';
				chdir(Home);
				if(chdir(Pathname) == -1) {
					fprintf(stderr, "find: bad directory tree\n");
					exit(1);
				}
			}
		}
	}
	rv = 1;
ret:
	if(dir)
		close(dir);
	if(chdir("..") == -1) {
		*endofname = '\0';
		fprintf(stderr, "find: bad directory <%s>\n", name);
		rv = 1;
	}
	return(rv);
}

gmatch(s, p) /* string match as in glob */
register char *s, *p;
{
	if (*s=='.' && *p!='.') return(0);
	return amatch(s, p);
}

amatch(s, p)
register char *s, *p;
{
	register cc;
	int scc, k;
	int c, lc;

	scc = *s;
	lc = 077777;
	switch (c = *p) {

	case '[':
		k = 0;
		while (cc = *++p) {
			switch (cc) {

			case ']':
				if (k)
					return(amatch(++s, ++p));
				else
					return(0);

			case '-':
				k |= lc <= scc & scc <= (cc=p[1]);
			}
			if (scc==(lc=cc)) k++;
		}
		return(0);

	case '?':
	caseq:
		if(scc) return(amatch(++s, ++p));
		return(0);
	case '*':
		return(umatch(s, ++p));
	case 0:
		return(!scc);
	}
	if (c==scc) goto caseq;
	return(0);
}

umatch(s, p)
register char *s, *p;
{
	if(*p==0) return(1);
	while(*s)
		if (amatch(s++, p)) return(1);
	return(0);
}

bwrite(rp, c)
register short *rp;
register c;
{
	register short *wp = Wp;

	c = (c+1) >> 1;
	while(c--) {
		if(!Wct) {
again:
			if(write(Cpio, (char *)Dbuf, Bufsize)<0) {
				Cpio = chgreel(1, Cpio);
				goto again;
			}
			Wct = Bufsize >> 1;
			wp = Dbuf;
			++Blocks;
		}
		*wp++ = *rp++;
		--Wct;
	}
	Wp = wp;
}
chgreel(x, fl)
{
	register f;
	char str[22];
	FILE *devtty;
	struct stat statb;
	extern errno;

	fprintf(stderr, "find: errno: %d, ", errno);
	fprintf(stderr, "find: can't %s\n", x? "write output": "read input");
	fstat(fl, &statb);
	if((statb.st_mode&S_IFMT) != S_IFCHR)
		exit(1);
again:
	fprintf(stderr, "If you want to go on, type device/file name %s\n",
		"when ready");
	devtty = fopen("/dev/tty", "r");
	fgets(str, 20, devtty);
	str[strlen(str) - 1] = '\0';
	if(!*str)
		exit(1);
	close(fl);
	if((f = open(str, x? 1: 0)) < 0) {
		fprintf(stderr, "That didn't work");
		fclose(devtty);
		goto again;
	}
	return f;
}
