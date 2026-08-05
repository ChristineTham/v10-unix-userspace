#include <sys/types.h>
#include <stdio.h>
#include <ctype.h>
#include <signal.h>
/*
 * V8 included <dir.h> here for DIRSIZ, the 14-character limit on a V7
 * directory entry, used only by getsuf() when it looks for a `.c` suffix.
 *
 * The driver runs on HOST paths, which are not limited to 14 characters, so
 * inheriting that number would silently refuse to recognise the suffix of any
 * file with a longer name.  The V7 limit still applies inside the V8 world --
 * libv8sys truncates directory entries to 14 bytes, as PLAN.md S6 describes --
 * but it does not apply out here.
 *
 * The #undef is required, not decorative.  <sys/types.h> above reaches
 * <sys/param.h>, which defines DIRSIZ as 14.  While the driver was compiled by
 * clang against the HOST headers nothing defined it and a bare #define was
 * enough; compiled by v8cc against V8's own include tree -- which is what
 * happens now that the driver is a V8 binary -- it is a redefinition, and the
 * value that wins decides whether getsuf() can see the `.c` on a path with a
 * component longer than 14 characters.  Most of this repo's own paths are.
 */
#undef	DIRSIZ
#define	DIRSIZ	255

/*
 * Pass programs.  V8 hard-coded absolute paths; here they are resolved at
 * startup from $V8ROOT so the world can live anywhere (see setpaths below).
 *
 * as and ld are the HOST toolchain, reached through clang -- porting V8's VAX
 * assembler and link editor would buy no authenticity, and their a.out output
 * is meaningless to XNU.  See PLAN.md S1.
 */
char	*cpp = "/lib/cpp";
char	*ccom = "/lib/ccom";
char	*c2 = "/lib/c2";
char	*as = "/usr/bin/clang";
char	*ld = "/usr/bin/clang";
char	*crt0 = "/lib/crt0.o";
char	*instrcnt = "/lib/instrcnt";
#ifndef V8ROOT_DEFAULT
#define V8ROOT_DEFAULT ""
#endif

char	*v8root;
char	*incdir;		/* "-I$V8ROOT/usr/include", built by setpaths */
char	*libdir;
char	*libc_a     = "/lib/libv8c.a";
char	*libstubs_a = "/lib/libv8stubs.a";
char	*libsys_a   = "/lib/libv8sys.a";

char	tmp0[30];		/* big enough for /tmp/ctm%05.5d */
char	*tmp1, *tmp2, *tmp3, *tmp4, *tmp5;
char	*outfile;
char	*savestr(), *strspl(), *setsuf();
void	idexit();
char	**av, **clist, **llist, **plist;
int	cflag, eflag, gflag, oflag, pflag, sflag, wflag, Rflag, exflag, proflag;
int	vflag, Cflag;
char	*dflag;
int	exfail;
char	*chpass;
char	*npassname;
extern	int	optind;
extern	int	opterr;
extern	char	*optarg;
extern	int	optopt;

int	nc, nl, np, nxo, na;

#define	cunlink(s)	if (s) unlink(s)

/*
 * Resolve the pass programs relative to $V8ROOT.
 *
 * V8 hard-coded /lib/cpp and /lib/ccom because the world was the root
 * filesystem.  Ours is a directory, so the paths are built at startup; without
 * $V8ROOT the original absolute paths remain, which is what an installed system
 * would want.
 *
 * strspl(), not snprintf().  snprintf is C99 and does not exist in V8's libc,
 * so in a driver compiled by v8cc it resolved from -lSystem -- and it is
 * VARIADIC, which is the one shape where that silent fallback is not merely
 * inauthentic but wrong: v8cc passes every argument positionally in x0-x7 and
 * Apple's ARM64 ABI passes variadic arguments on the stack, so the two
 * disagree about where `r` even is.  The same mistake produced garbage from
 * scanf, printf and execl earlier in this port.  strspl() is this file's own
 * concatenate-and-save helper, three K&R lines at the bottom, and it was
 * always the right answer here.
 */
setpaths()
{
	char *r, *getenv();

	/*
	 * $V8ROOT, then the root compiled in at build time -- the same decision
	 * v8root() makes in shim/v8sys/syscall.c, and the driver needs its own
	 * copy of it because it hands cpp an explicit -I and cpp is still a host
	 * binary that never sees rootpath().
	 */
	if ((r = getenv("V8ROOT")) == 0 || *r == 0)
		r = V8ROOT_DEFAULT;
	if (r == 0 || *r == 0)
		return;
	v8root = r;
	cpp        = strspl(r, "/lib/cpp");
	ccom       = strspl(r, "/lib/ccom");
	crt0       = strspl(r, "/lib/crt0.o");
	libc_a     = strspl(r, "/lib/libv8c.a");
	libstubs_a = strspl(r, "/lib/libv8stubs.a");
	libsys_a   = strspl(r, "/lib/libv8sys.a");
	incdir     = strspl("-I", strspl(r, "/usr/include"));
}

main(argc, argv)
	char **argv;
{
	char *t;
	char *assource;
	int i, j, c;
	char errbuf[BUFSIZ];
	setbuf(stderr, errbuf);

	setpaths();

	/*
	 * The link step adds 10 of its own: -nostdlib, -e, _v8start, -o, the
	 * output name, crt0, three archives and -lSystem, plus the terminating
	 * null.  20 is room to spare.
	 */
	av = (char **)calloc(argc+20, sizeof (char **));
	clist = (char **)calloc(argc, sizeof (char **));
	llist = (char **)calloc(argc, sizeof (char **));
	plist = (char **)calloc(argc, sizeof (char **));
	opterr = 0;
	while (optind<argc) switch (c = getopt(argc, argv, "vsSno:RiOPgwEpPcD:I:U:C:t:B:l:d:")) {
	case 'i':
		Cflag++;
		continue;
	case 'v':
		vflag++;
		continue;
	case 'S':
		sflag++;
		cflag++;
		continue;
	case 'l':
		llist[nl++] = strspl("-l", optarg);
		continue;
	case 'o':
		outfile = optarg;
		/*
		 * V8 refused any -o naming a .c or .o file, to stop
		 * `cc -o foo.o foo.c` quietly destroying a source file.  The
		 * .o half of that is too strict for `cc -c -o foo.o foo.c`,
		 * which is ordinary usage, so it now applies only when we are
		 * actually linking.  Overwriting a .c is still refused.
		 */
		switch (getsuf(outfile)) {

		case 'c':
			error("-o would overwrite %s", outfile);
			exit(8);
		case 'o':
			if (!cflag) {
				error("-o would overwrite %s", outfile);
				exit(8);
			}
		}
		continue;
	case 'R':
		Rflag++;
		continue;
	case 'n':
		/*
		 * -n asked the VAX link editor for a shared-text a.out.  There
		 * is no Mach-O equivalent and nothing to translate it to, so it
		 * is accepted and ignored -- exactly as -O is, and for the same
		 * reason: V8's own makefiles pass it, and the alternative is
		 * forwarding a flag clang does not know.  src/cmd/sed/Makefile
		 * is the one that found this: `cc -o sed -n *.o`.
		 */
		continue;
	case 'O':
		/*
		 * c2, the peephole optimiser, read and wrote VAX assembler text
		 * and is not retargetable; PLAN.md S5 drops it.  The flag is
		 * still accepted so V8 makefiles keep working, and the host
		 * assembler does the remaining tidying.
		 */
		continue;
	case 'p':
		proflag++;
		continue;
	case 'g':
		gflag++;
		continue;
	case 'w':
		wflag++;
		continue;
	case 'E':
		exflag++;
	case 'P':
		pflag++;
		t = strspl("-", "x");
		t[1] = optopt;
		plist[np++] = t;
	case 'c':
		cflag++;
		continue;
	case 'D':
	case 'I':
	case 'U':
	case 'C':
		plist[np] = strspl("-X", optarg);
		plist[np++][1] = c;
		continue;
	case 't':
		if (chpass)
			error("-t overwrites earlier option", 0);
		chpass = optarg;
		if (chpass[0]==0)
			chpass = "012p";
		continue;
	case 'B':
		if (npassname)
			error("-B overwrites earlier option", 0);
		npassname = optarg;
		if (npassname[0]==0)
			npassname = "/usr/c/o";
		continue;
	case 'd':
		dflag = strspl("-d", optarg);
		continue;

	case '?':
	case 's':
		t = strspl("-", "x");
		t[1] = optopt;
		llist[nl++] = t;
		continue;

	case EOF:
		t = argv[optind];
		optind++;
		c = getsuf(t);
		if (c=='c' || c=='s' || c=='i' || exflag) {
			clist[nc++] = t;
			t = setsuf(t, 'o');
		}
		if (nodup(llist, t)) {
			llist[nl++] = t;
			if (getsuf(t)=='o')
				nxo++;
		}
	}
	if (gflag) {
		if (oflag)
			fprintf(stderr, "cc: warning: -g disables -O\n");
		oflag = 0;
	}
	if (npassname && chpass ==0)
		chpass = "012p";
	if (chpass && npassname==0)
		npassname = "/usr/new";
	if (chpass)
	for (t=chpass; *t; t++) {
		switch (*t) {

		case '0':
			ccom = strspl(npassname, "ccom");
			continue;
		case '2':
			c2 = strspl(npassname, "c2");
			continue;
		case 'p':
			cpp = strspl(npassname, "cpp");
			continue;
		}
	}
	if (proflag)
		crt0 = "/lib/mcrt0.o";
	if (nc==0)
		goto nocom;
	if (signal(SIGINT, SIG_IGN) != SIG_IGN)
		signal(SIGINT, idexit);
	if (signal(SIGTERM, SIG_IGN) != SIG_IGN)
		signal(SIGTERM, idexit);
	if (pflag==0)
		sprintf(tmp0, "/tmp/ctm%05.5d", getpid());
	tmp1 = strspl(tmp0, "1");
	tmp2 = strspl(tmp0, "2");
	tmp3 = strspl(tmp0, "3");
	if (pflag==0)
		tmp4 = strspl(tmp0, "4");
	if (oflag || Cflag)
		tmp5 = strspl(tmp0, "5");
	for (i=0; i<nc; i++) {
		int suffix = getsuf(clist[i]);
		if (nc > 1) {
			printf("%s:\n", clist[i]);
			fflush(stdout);
		}
		if (suffix == 's') {
			assource = clist[i];
			goto assemble;
		} else
			assource = tmp3;
		if (suffix == 'i')
			goto compile;
		if (pflag)
			tmp4 = setsuf(clist[i], 'i');
		av[0] = "cpp";
		av[1] = clist[i];
		av[2] = exflag ? "-" : tmp4;
		na = 3;
		for (j = 0; j < np; j++)
			av[na++] = plist[j];
		/*
		 * The world's headers.  V8 needed no such flag because
		 * /usr/include WAS the system's; ours lives under $V8ROOT, and
		 * cpp has no built-in notion of where that is.  Appended after
		 * the user's -I options so they still take precedence.
		 */
		if (incdir)
			av[na++] = incdir;
		av[na++] = 0;
		switch (callsys(cpp, av)) {
			case 0:
				break;
#define CLASS 27
			case CLASS:
				if (callsys("/lib/cpre",av) == 0)
					break;
			default:
				exfail++;
				eflag++;
		}
		if (pflag || exfail) {
			cflag++;
			continue;
		}
compile:
		if (sflag)
			assource = tmp3 = setsuf(clist[i], 's');
		av[0] = "ccom";
		av[1] = suffix=='i'? clist[i]: tmp4;
		av[2] = (oflag || Cflag)? tmp5:tmp3;
		na = 3;
		if (proflag)
			av[na++] = "-Xp";
		/*
		 * -g is NOT passed on to ccom.
		 *
		 * On the VAX it asked pass 1 to emit stabs into the assembly for
		 * the a.out symbol table.  The ARM64 back end emits no debug
		 * records at all -- debugging here is done on the generated
		 * assembly with the host's tools, which is the same decision as
		 * as and ld being the host's (PLAN.md S1) -- so ccom rejects the
		 * flag outright: "bad option: g".
		 *
		 * Accepting it in the driver and dropping it here is what lets
		 * V8's own makefiles run unmodified; src/cmd/sh/makefile says
		 * `CFLAGS = -gd2` and is the one that found this.  The flag
		 * still reaches the link, where clang does understand it.
		 */
		if (wflag)
			av[na++] = "-w";
		av[na] = 0;
		if (callsys(ccom, av)) {
			cflag++;
			eflag++;
			continue;
		}
		if (oflag || Cflag) {
			av[0] = Cflag? "instrcnt": "c2";
			av[1] = tmp5; av[2] = tmp3; av[3] = 0;
			if (callsys(Cflag? instrcnt: c2, av)) {
				unlink(tmp3);
				tmp3 = assource = tmp5;
			} else
				unlink(tmp5);
		}
		if (sflag)
			continue;
	assemble:
		cunlink(tmp1); cunlink(tmp2); cunlink(tmp4);
		/*
		 * The host assembler, via clang -c.  -R and -d were VAX-as
		 * flags with no counterpart and are dropped; the driver still
		 * accepts them so old makefiles do not break.
		 */
		av[0] = "clang"; av[1] = "-c";
		/*
		 * -x assembler is required: the driver's temporary files have
		 * no .s suffix (V8 named them /tmp/ctmNNNNN), and clang decides
		 * what an input is from its extension.  Without this it treats
		 * the assembly as a linker input and silently produces nothing.
		 */
		av[2] = "-x"; av[3] = "assembler";
		av[4] = "-o";
		/*
		 * V8's driver used -o only when linking, so `cc -c -o foo.o`
		 * silently wrote foo.o next to the source and ignored the name
		 * given.  Modern makefiles rely on it, and honouring it changes
		 * nothing V8 itself did -- with several inputs it stays
		 * per-source, since one -o cannot name them all.
		 */
		av[5] = (cflag && outfile && nc == 1) ? outfile
						     : setsuf(clist[i], 'o');
		na = 6;
		av[na++] = assource;
		av[na] = 0;
		if (callsys(as, av)) {
			cflag++;
			eflag++;
			continue;
		}
	}
nocom:
	if (cflag==0 && nl!=0) {
		i = 0;
		/*
		 * The host link editor, via clang, but linking the V8 world:
		 * V8's crt0, V8's libc, and the shim.  NOT clang's startup and
		 * NOT the host libc.
		 *
		 * Linking the host libc looks as though it works -- the program
		 * runs, and most of it behaves -- and then printf prints
		 * rubbish.  The reason is variadic calls.  v8cc passes every
		 * argument the ordinary way, in x0-x7 and then on the stack;
		 * Apple's ARM64 ABI requires the VARIADIC arguments of a call
		 * to go on the stack instead.  So a v8cc-compiled caller and
		 * the host printf disagree about where the arguments are, and
		 * `printf("%d", 42)` printed 1839618368.
		 *
		 * V8's own printf is not variadic in that sense -- it is
		 * printf(fmt, args) taking &args and walking forward -- so it
		 * wants exactly what v8cc emits.  The two only agree if we link
		 * V8's.
		 *
		 * -lSystem still comes last: the shim is written against the
		 * host syscall layer, and that is the one place the two worlds
		 * are meant to meet.
		 */
		av[0] = "clang"; na = 1;
		av[na++] = "-nostdlib";
		av[na++] = "-e";
		av[na++] = "_v8start";
		if (outfile) {
			av[na++] = "-o";
			av[na++] = outfile;
		}
		av[na++] = crt0;
		while (i < nl)
			av[na++] = llist[i++];
		av[na++] = libc_a;
		av[na++] = libstubs_a;
		av[na++] = libsys_a;
		av[na++] = "-lSystem";
		av[na++] = 0;
		eflag |= callsys(ld, av);
		if (nc==1 && nxo==1 && eflag==0)
			unlink(setsuf(clist[0], 'o'));
	}
	dexit();
}

void
idexit(sig)
	int sig;
{

	eflag = 100;
	dexit();
}

dexit()
{

	if (!pflag) {
		cunlink(tmp1);
		cunlink(tmp2);
		if (sflag==0)
			cunlink(tmp3);
		cunlink(tmp4);
		cunlink(tmp5);
	}
	exit(eflag);
}

error(s, x)
	char *s, *x;
{
	FILE *diag = exflag ? stderr : stdout;

	fprintf(diag, "cc: ");
	fprintf(diag, s, x);
	putc('\n', diag);
	fflush(diag);
	exfail++;
	cflag++;
	eflag++;
}

getsuf(as)
char as[];
{
	register int c;
	register char *s;
	register int t;

	s = as;
	c = 0;
	while (t = *s++)
		if (t=='/')
			c = 0;
		else
			c++;
	s -= 3;
	if (c <= DIRSIZ && c > 2 && *s++ == '.')
		return (*s);
	return (0);
}

char *
setsuf(as, ch)
	char *as;
{
	register char *s, *s1;

	s = s1 = savestr(as);
	while (*s)
		if (*s++ == '/')
			s1 = s;
	s[-1] = ch;
	return (s1);
}

callsys(f, v)
	char *f, **v;
{
	int t, status;
	register char **vp;	/* god */

	if(vflag) {	/* god & mjm */
		vp = v;
		fprintf(stderr,"+ ");
		while (*vp)
			fprintf(stderr,"%s ",*vp++);
		fprintf(stderr, "\n");
		fflush(stderr);
	}


	t = vfork();
	if (t == -1) {
		printf("No more processes\n");
		return (100);
	}
	if (t == 0) {
		execv(f, v);
		printf("Can't find %s\n", f);
		fflush(stdout);
		_exit(100);
	}
	while (t != wait(&status))
		;
	if ((t=(status&0377)) != 0 && t!=14) {
		if (t!=2) {
			printf("Fatal error in %s\n", f);
			eflag = 8;
		}
		dexit();
	}
	return ((status>>8) & 0377);
}

nodup(l, os)
	char **l, *os;
{
	register char *t, *s;
	register int c;

	s = os;
	if (getsuf(s) != 'o')
		return (1);
	while (t = *l++) {
		while (c = *s++)
			if (c != *t++)
				break;
		if (*t==0 && c==0)
			return (0);
		s = os;
	}
	return (1);
}

#define	NSAVETAB	1024
char	*savetab;
int	saveleft;

char *
savestr(cp)
	register char *cp;
{
	register int len;

	len = strlen(cp) + 1;
	if (len > saveleft) {
		saveleft = NSAVETAB;
		if (len > saveleft)
			saveleft = len;
		savetab = (char *)malloc(saveleft);
		if (savetab == 0) {
			fprintf(stderr, "ran out of memory (savestr)\n");
			exit(1);
		}
	}
	strncpy(savetab, cp, len);
	cp = savetab;
	savetab += len;
	saveleft -= len;
	return (cp);
}

char *
strspl(left, right)
	char *left, *right;
{
	char buf[BUFSIZ];

	strcpy(buf, left);
	strcat(buf, right);
	return (savestr(buf));
}
