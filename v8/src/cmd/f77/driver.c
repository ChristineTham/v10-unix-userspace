char *xxxvers[] = "\n@(#) FORTRAN 77 DRIVER, VERSION 2.03.5,   7 NOVEMBER 1980\n";
#include <stdio.h>
#include <ctype.h>
#include "defines"
#include "machdefs"
#include "drivedefs"
#include "ftypes"
#include <signal.h>

static FILEP diagfile	= {stderr} ;
static int pid;
static int sigivalue	= 0;
static int sigqvalue	= 0;
static int sighvalue	= 0;
static int sigtvalue	= 0;

static char *pass1name	= PASS1NAME ;
static char *pass2name	= PASS2NAME ;
static char *asmname	= ASMNAME ;
static char *ldname	= LDNAME ;
static char *footname	= FOOTNAME;
static char *proffoot	= PROFFOOT;
static char *macroname	= "m4";
static char *shellname	= "/bin/sh";
static char *aoutname	= "a.out" ;
static char *temppref	= TEMPPREF;

static char *infname;
static char textfname[44];
static char asmfname[44];
static char asmpass2[44];
static char initfname[44];
static char sortfname[44];
static char prepfname[44];
static char objfdefault[44];
static char optzfname[44];
static char setfname[44];

static char fflags[50]	= "-";
static char cflags[50]	= "-c";
#if TARGET == GCOS
	static char eflags[30]	= "system=gcos ";
#else
	static char eflags[30]	= "system=unix ";
#endif
static char rflags[30]	= "";
static char lflag[3]	= "-x";
static char *fflagp	= fflags+1;
static char *cflagp	= cflags+2;
static char *eflagp	= eflags+12;
static char *rflagp	= rflags;
static char **loadargs;
static char **loadp;

static flag erred	= NO;
static flag loadflag	= YES;
static flag saveasmflag	= NO;
static flag profileflag	= NO;
static flag optimflag	= NO;
static flag debugflag	= NO;
static flag verbose	= NO;
static flag nofloating	= NO;
static flag fortonly	= NO;
static flag macroflag	= NO;
static flag sdbflag	= NO;



main(argc, argv)
int argc;
char **argv;
{
int i, c, status;
char *setdoto(), *lastchar(), *lastfield(), *copys();
char *v8path();	/* PORT: declared, or its char * return truncates to int */
ptr ckalloc();
register char *s;
char fortfile[20], *t;
char buff[100];
int intrupt();

sigivalue = (int) signal(SIGINT, SIG_IGN) & 01;
sigqvalue = (int) signal(SIGQUIT,SIG_IGN) & 01;
sighvalue = (int) signal(SIGHUP, SIG_IGN) & 01;
sigtvalue = (int) signal(SIGTERM,SIG_IGN) & 01;
enbint(intrupt);

pid = getpid();
crfnames();

/* PORT: the crt0 half of the note beside doload().  footname and proffoot are
   V8 paths handed to clang, which is a host binary and resolves them against
   the real filesystem, so they are absolutised against $V8ROOT here -- the same
   thing setpaths() does in src/cmd/cc.c and for the same reason.  Done after
   the option loop's -TF would have replaced footname?  No: BEFORE it, because
   -TF names a host path deliberately and must not be rewritten.  The loop runs
   below, so an explicit -TF still wins. */
footname = v8path(footname);
proffoot = v8path(proffoot);

loadargs = (char **) ckalloc( (argc+20) * sizeof(*loadargs) );
/* PORT: THE LOADER FLAGS GO THROUGH clang, SO THEY NEED -Wl.  ldname here is
   /usr/bin/clang, not a link editor, and this is the thing that made two drafts
   of these three lines wrong:

	-X		ld64 answers `warning: -X is obsolete' on every link
	-x		clang reads its OWN -x, "specify language", and dies with
			`language not recognized: -u' -- taking the NEXT argument
			as the language name, so the error names neither flag
	-Wl,-x		right: clang hands -x to ld, which strips local symbols

   The second is the instructive one.  Correcting -X to Mach-O's spelling looked
   like a one-character fix and instead moved the failure to a different flag
   entirely, because the argument was never going to ld in the first place.
   Read WHICH PROGRAM parses a flag before correcting its spelling. */
loadargs[1] = "-Wl,-x";
loadargs[2] = "-Wl,-u";
/* PORT: ARM64 added.  This arm is not about the VAX -- it is about how the
   host spells an external symbol.  a.out prepended an underscore and Mach-O
   prepends one too, so `-u _MAIN__' is right here and the Interdata arm just
   below, which asks for `main' with no underscore, is not. */
#if HERE==PDP11 || HERE==VAX
	loadargs[3] = "_MAIN__";
#endif
#if HERE==ARM64
	loadargs[3] = "-Wl,_MAIN__";	/* the operand of -Wl,-u above */
#endif
#if HERE == INTERDATA
	loadargs[3] = "main";
#endif
loadp = loadargs + 4;

--argc;
++argv;

while(argc>0 && argv[0][0]=='-' && argv[0][1]!='\0')
	{
	for(s = argv[0]+1 ; *s ; ++s) switch(*s)
		{
		case 'T':  /* use special passes */
			switch(*++s)
				{
				case '1':
					pass1name = s+1; goto endfor;
				case '2':
					pass2name = s+1; goto endfor;
				case 'a':
					asmname = s+1; goto endfor;
				case 'l':
					ldname = s+1; goto endfor;
				case 'F':
					footname = s+1; goto endfor;
				case 'm':
					macroname = s+1; goto endfor;
				case 't':
					temppref = s+1; goto endfor;
				default:
					fatali("bad option -T%c", *s);
				}
			break;

		case '6':
			if(s[1]=='6')
				{
				*fflagp++ = *s++;
				goto copyfflag;
				}
			else	{
				fprintf(diagfile, "invalid flag 6%c\n", s[1]);
				done(1);
				}

		case 'w':
			if(s[1]=='6' && s[2]=='6')
				{
				*fflagp++ = *s++;
				*fflagp++ = *s++;
				}

		copyfflag:
		case 'u':
		case 'U':
		case '1':
		case 'C':
			*fflagp++ = *s;
			break;

		case 'O':
			optimflag = YES;
#if TARGET == INTERDATA
				*loadp++ = "-r";
				*loadp++ = "-d";
#endif
			*fflagp++ = 'O';
			if( isdigit(s[1]) )
				*fflagp++ = *++s;
			break;

		case 'N':
			*fflagp++ = 'N';
			if( oneof(*++s, "qxscn") )
				*fflagp++ = *s++;
			else	{
				fprintf(diagfile, "invalid flag -N%c\n", *s);
				done(1);
				}
			while( isdigit(*s) )
				*fflagp++ = *s++;
			*fflagp++ = 'X';
			goto endfor;

		case 'm':
			if(s[1] == '4')
				++s;
			macroflag = YES;
			break;

		case 'S':
			strcat(cflags, " -S");
			saveasmflag = YES;

		case 'c':
			loadflag = NO;
			break;

		case 'v':
			verbose = YES;
			break;

		case 'd':
			debugflag = YES;
			goto copyfflag;

		case 'M':
#if HERE==ARM64
			*loadp++ = "-Wl,-M";	/* through clang, as above */
#else
			*loadp++ = "-M";
#endif
			break;

		case 'g':
			strcat(cflags," -g");
			sdbflag = YES;
			goto copyfflag;

		case 'p':
			profileflag = YES;
			strcat(cflags," -p");
			goto copyfflag;

		case 'o':
			if( ! strcmp(s, "onetrip") )
				{
				*fflagp++ = '1';
				goto endfor;
				}
			aoutname = *++argv;
			--argc;
			break;

#if TARGET == PDP11
		case 'f':
			nofloating = YES;
			pass2name = NOFLPASS2;
		break;
#endif

		case 'F':
			fortonly = YES;
			loadflag = NO;
			break;

		case 'I':
			if(s[1]=='2' || s[1]=='4' || s[1]=='s')
				{
				*fflagp++ = *s++;
				goto copyfflag;
				}
			fprintf(diagfile, "invalid flag -I%c\n", s[1]);
			done(1);

		case 'l':	/* letter ell--library */
			s[-1] = '-';
			*loadp++ = s-1;
			goto endfor;

		case 'E':	/* EFL flag argument */
			while( *eflagp++ = *++s)
				;
			*eflagp++ = ' ';
			goto endfor;
		case 'R':
			while( *rflagp++ = *++s )
				;
			*rflagp++ = ' ';
			goto endfor;
		default:
			lflag[1] = *s;
			*loadp++ = copys(lflag);
			break;
		}
endfor:
	--argc;
	++argv;
	}

*fflagp = '\0';

loadargs[0] = ldname;
#if TARGET == PDP11
	if(nofloating)
		*loadp++ = (profileflag ? NOFLPROF : NOFLFOOT);
	else
#endif
*loadp++ = (profileflag ? proffoot : footname);

for(i = 0 ; i<argc ; ++i)
	switch(c =  dotchar(infname = argv[i]) )
		{
		case 'r':	/* Ratfor file */
		case 'e':	/* EFL file */
			if( unreadable(argv[i]) )
				{
				erred = YES;
				break;
				}
			s = fortfile;
			t = lastfield(argv[i]);
			while( *s++ = *t++)
				;
			s[-2] = 'f';

			if(macroflag)
				{
				sprintf(buff, "%s %s >%s", macroname, infname, prepfname);
				if( sys(buff) )
					{
					rmf(prepfname);
					erred = YES;
					break;
					}
				infname = prepfname;
				}

			if(c == 'e')
				sprintf(buff, "efl %s %s >%s", eflags, infname, fortfile);
			else
				sprintf(buff, "ratfor %s %s >%s", rflags, infname, fortfile);
			status = sys(buff);
			if(macroflag)
				rmf(infname);
			if(status)
				{
				erred = YES;
				rmf(fortfile);
				break;
				}

			if( ! fortonly )
				{
				infname = argv[i] = lastfield(argv[i]);
				*lastchar(infname) = 'f';
	
				if( dofort(argv[i]) )
					erred = YES;
				else	{
					if( nodup(t = setdoto(argv[i])) )
						*loadp++ = t;
					rmf(fortfile);
					}
				}
			break;

		case 'f':	/* Fortran file */
		case 'F':
			if( unreadable(argv[i]) )
				erred = YES;
			else if( dofort(argv[i]) )
				erred = YES;
			else if( nodup(t=setdoto(argv[i])) )
				*loadp++ = t;
			break;

		case 'c':	/* C file */
		case 's':	/* Assembler file */
			if( unreadable(argv[i]) )
				{
				erred = YES;
				break;
				}
/* PORT: ARM64 added -- the progress line every Unix host printed. */
#if HERE==PDP11 || HERE==VAX || HERE==ARM64
			fprintf(diagfile, "%s:\n", argv[i]);
#endif
			sprintf(buff, "cc %s %s", cflags, argv[i] );
			if( sys(buff) )
				erred = YES;
			else
				if( nodup(t = setdoto(argv[i])) )
					*loadp++ = t;
			break;

		case 'o':
			if( nodup(argv[i]) )
				*loadp++ = argv[i];
			break;

		default:
			if( ! strcmp(argv[i], "-o") )
				aoutname = argv[++i];
			else
				*loadp++ = argv[i];
			break;
		}

if(loadflag && !erred)
	doload(loadargs, loadp);
done(erred);
}

dofort(s)
char *s;
{
int retcode;
char buff[200];

infname = s;
sprintf(buff, "%s %s %s %s %s %s",
	pass1name, fflags, s, asmfname, initfname, textfname);
switch( sys(buff) )
	{
	case 1:
		goto error;
	case 0:
		break;
	default:
		goto comperror;
	}

if(content(initfname) > 0)
	if( dodata() )
		goto error;
if( dopass2() )
	goto comperror;
doasm(s);
retcode = 0;

ret:
	rmf(asmfname);
	rmf(initfname);
	rmf(textfname);
	return(retcode);

error:
	fprintf(diagfile, "\nError.  No assembly.\n");
	retcode = 1;
	goto ret;

comperror:
	fprintf(diagfile, "\ncompiler error.\n");
	retcode = 2;
	goto ret;
}




dopass2()
{
char buff[100];

if(verbose)
	fprintf(diagfile, "PASS2.");

#if FAMILY==DMR
	sprintf(buff, "%s %s - %s", pass2name, textfname, asmpass2);
	return( sys(buff) );
#endif

#if FAMILY == PCC
#	if TARGET==INTERDATA
	sprintf(buff, "%s -A%s <%s >%s", pass2name, setfname, textfname, asmpass2);
#	else
	sprintf(buff, "%s %s >%s", pass2name, textfname, asmpass2);
#	endif
	return( sys(buff) );
#endif
}




doasm(s)
char *s;
{
register char *lastc;
char *obj;
char buff[200];
char *lastchar(), *setdoto();

if(*s == '\0')
	s = objfdefault;
lastc = lastchar(s);
obj = setdoto(s);

#if TARGET==PDP11 || TARGET==VAX
#	ifdef PASS2OPT
	if(optimflag)
		{
		sprintf(buff, "%s %s %s", PASS2OPT, asmpass2, optzfname);
		if( sys(buff) )
			rmf(optzfname);
		else
			{
			sprintf(buff,"mv %s %s", optzfname, asmpass2);
			sys(buff);
			}
		}
#	endif
#endif

if(saveasmflag)
	{
	*lastc = 's';
#if TARGET == INTERDATA
	sprintf(buff, "cat %s %s %s >%s",asmfname, setfname, asmpass2, obj);
#else
	sprintf(buff, "cat %s %s >%s", asmfname, asmpass2, obj);
#endif
	sys(buff);
	*lastc = 'o';
	}
else
	{
	if(verbose)
		fprintf(diagfile, "  ASM.");
#if TARGET == INTERDATA
	sprintf(buff, "%s -o %s %s %s %s", asmname, obj, asmfname, setfname, asmpass2);
#endif

#if TARGET == VAX
	/* vax assembler currently accepts only one input file */
	sprintf(buff, "cat %s >>%s", asmpass2, asmfname);
	sys(buff);
	sprintf(buff, "%s -o %s %s", asmname, obj, asmfname);
#endif

#if TARGET == PDP11
	sprintf(buff, "%s -u -o %s %s %s", asmname, obj, asmfname, asmpass2);
#endif

#if TARGET!=INTERDATA && TARGET!=PDP11 && TARGET!=VAX
	sprintf(buff, "%s -o %s %s %s", asmname, obj, asmfname, asmpass2);
#endif

	if( sys(buff) )
		fatal("assembler error");
	if(verbose)
		fprintf(diagfile, "\n");
#if HERE==PDP11 && TARGET!=PDP11
	rmf(obj);
#endif
	}

rmf(asmpass2);
}



/* PORT: setpaths and libpath, both of them copies of what src/cmd/cc.c:91 and
   :160 already carry, and forced by the same one-sentence fact: the assembler
   and link editor are the HOST's, reached through clang, and clang is a host
   binary that never sees rootpath().  So every path this driver hands it has to
   be a host path already.

   Two things needed it.  FOOTNAME is "/lib/crt0.o", which exists only inside
   $V8ROOT -- measured, a bare `f77' reported `clang: error: no such file or
   directory: /lib/crt0.o'.  And liblist is { -lF77, -lI77, -lm, -lc }, which
   clang resolves against the macOS SDK: that is the hole cc.c documents at
   length, where -lm answered with a libSystem re-export and the link died on an
   _errno with no address.

   -lc is DROPPED rather than resolved, exactly as cc.c drops it: libv8c is
   already on the line through $(V8LIBS), and letting clang put libSystem ahead
   of it gives a V8 program Apple's variadic ABI. */
static char *
v8path(suffix)
char *suffix;
{
char *r, *getenv(), *copys();
char buf[256];

if( (r = getenv("V8ROOT")) == NULL || *r == '\0' )
	return(suffix);
sprintf(buf, "%s%s", r, suffix);
return( copys(buf) );
}


static char *
v8lib(flag)
char *flag;			/* "-lNAME" */
{
char *r, *getenv(), *copys();
char buf[256];

if(flag[1] != 'l')
	return(flag);
if(flag[2] == 'c' && flag[3] == '\0')
	return(NULL);		/* caller drops it: see the note above */
if( (r = getenv("V8ROOT")) == NULL || *r == '\0' )
	return(flag);
sprintf(buf, "%s/usr/lib/lib%s.a", r, flag+2);
if( access(buf, 4) == 0 )
	return( copys(buf) );
return(flag);			/* fall through to the host, as cc does */
}


doload(v0, v)
register char *v0[], *v[];
{
char **p;
char *q, *v8lib();	/* PORT: declared, same reason as v8path above */
int waitpid;

if(sdbflag)
	*v++ = "-lg";

/* PORT: clang needs telling not to bring its own startup and libc, exactly as
   src/cmd/cc.c:534-536 does.  Without -nostdlib the link picks up Apple's crt
   beside V8's, and without -e the entry point is _main rather than _v8start. */
*v++ = "-nostdlib";
*v++ = "-e";
*v++ = "_v8start";

for(p = liblist ; *p ; ++p)
	{
	/* PORT: -lc IS V8's libc, and here that is three archives plus the host
	   syscall layer -- the same four names cc.c ends its link line with, in
	   the same order.  EXPANDING it is what keeps upstream's liblist
	   meaningful; an earlier draft DROPPED -lc the way cc.c drops it, and
	   the link died on __iob, __flsbuf and __sobuf, because cc.c can drop it
	   only by virtue of appending these four unconditionally afterwards.
	   -lSystem last, because the shim is where the two worlds meet. */
	if( (*p)[1] == 'l' && (*p)[2] == 'c' && (*p)[3] == '\0' )
		{
		*v++ = v8path("/lib/libv8c.a");
		*v++ = v8path("/lib/libv8stubs.a");
		*v++ = v8path("/lib/libv8sys.a");
		*v++ = "-lSystem";
		continue;
		}
	if( (q = v8lib(*p)) != NULL )
		*v++ = q;
	}

*v++ = "-o";
*v++ = aoutname;
*v = NULL;

if(verbose)
	fprintf(diagfile, "LOAD.");
if(debugflag)
	{
	for(p = v0 ; p<v ; ++p)
		fprintf(diagfile, "%s ", *p);
	fprintf(diagfile, "\n");
	}

/* PORT: ARM64 added, and this is the arm whose absence would be silent.  The
   three machines listed are the three Unixes V8 knew, so the test spells "is
   this a Unix with fork and exec" as a list of machine names.  Without ARM64
   here the driver builds, runs, and never invokes the link editor at all --
   the -DCM_ shape, where a missing -D selects an empty arm rather than failing
   to compile. */
#if HERE==PDP11 || HERE==INTERDATA || HERE==VAX || HERE==ARM64
	if( (waitpid = fork()) == 0)
		{
		enbint(SIG_DFL);
		execv(ldname, v0);
		fatalstr("couldn't load %s", ldname);
		}
	await(waitpid);
#endif

#if HERE==INTERDATA
	if(optimflag)
		{
		char buff1[100], buff2[100];
		sprintf(buff1, "nopt %s -o junk.%d", aoutname, pid);
		sprintf(buff2, "mv junk.%d %s", pid, aoutname);
		if( sys(buff1) || sys(buff2) )
			err("bad optimization");
		}
#endif

if(verbose)
	fprintf(diagfile, "\n");
}

/* Process control and Shell-simulating routines */

sys(str)
char *str;
{
register char *s, *t;
char *argv[100], path[100];
char *inname, *outname;
int append;
int waitpid;
int argc;


if(debugflag)
	fprintf(diagfile, "%s\n", str);
inname  = NULL;
outname = NULL;
argv[0] = shellname;
argc = 1;

t = str;
while( isspace(*t) )
	++t;
while(*t)
	{
	if(*t == '<')
		inname = t+1;
	else if(*t == '>')
		{
		if(t[1] == '>')
			{
			append = YES;
			outname = t+2;
			}
		else	{
			append = NO;
			outname = t+1;
			}
		}
	else
		argv[argc++] = t;
	while( !isspace(*t) && *t!='\0' )
		++t;
	if(*t)
		{
		*t++ = '\0';
		while( isspace(*t) )
			++t;
		}
	}

if(argc == 1)   /* no command */
	return(-1);
argv[argc] = 0;

s = path;
t = "/usr/bin/";
while(*t)
	*s++ = *t++;
for(t = argv[1] ; *s++ = *t++ ; )
	;
if((waitpid = fork()) == 0)
	{
	if(inname)
		freopen(inname, "r", stdin);
	if(outname)
		freopen(outname, (append ? "a" : "w"), stdout);
	enbint(SIG_DFL);

	texec(path+9, argv);  /* command */
	texec(path+4, argv);  /*  /bin/command */
	texec(path  , argv);  /* /usr/bin/command */

	fatalstr("Cannot load %s",path+9);
	}

return( await(waitpid) );
}





#include "errno.h"

/* modified version from the Shell */
texec(f, av)
char *f;
char **av;
{
extern int errno;

execv(f, av+1);

if (errno==ENOEXEC)
	{
	av[1] = f;
	execv(shellname, av);
	fatal("No shell!");
	}
if (errno==ENOMEM)
	fatalstr("%s: too large", f);
}






done(k)
int k;
{
static int recurs	= NO;

if(recurs == NO)
	{
	recurs = YES;
	rmfiles();
	}
exit(k);
}






enbint(k)
int (*k)();
{
if(sigivalue == 0)
	signal(SIGINT,k);
if(sigqvalue == 0)
	signal(SIGQUIT,k);
if(sighvalue == 0)
	signal(SIGHUP,k);
if(sigtvalue == 0)
	signal(SIGTERM,k);
}




intrupt()
{
done(2);
}



await(waitpid)
int waitpid;
{
int w, status;

enbint(SIG_IGN);
while ( (w = wait(&status)) != waitpid)
	if(w == -1)
		fatal("bad wait code");
enbint(intrupt);
if(status & 0377)
	{
	if(status != SIGINT)
		fprintf(diagfile, "Termination code %d", status);
	done(3);
	}
return(status>>8);
}

/* File Name and File Manipulation Routines */

unreadable(s)
register char *s;
{
register FILE *fp;

if(fp = fopen(s, "r"))
	{
	fclose(fp);
	return(NO);
	}

else
	{
	fprintf(diagfile, "Error: Cannot read file %s\n", s);
	return(YES);
	}
}



clf(p)
FILEP *p;
{
if(p!=NULL && *p!=NULL && *p!=stdout)
	{
	if(ferror(*p))
		fatal("writing error");
	fclose(*p);
	}
*p = NULL;
}

rmfiles()
{
rmf(textfname);
rmf(asmfname);
rmf(initfname);
rmf(asmpass2);
#if TARGET == INTERDATA
	rmf(setfname);
#endif
}








/* return -1 if file does not exist, 0 if it is of zero length
   and 1 if of positive length
*/
content(filename)
char *filename;
{
#ifdef VERSION6
	struct stat
		{
		char cjunk[9];
		char size0;
		int size1;
		int ijunk[12];
		} buf;
#else
#	include <sys/types.h>
#	include <sys/stat.h>
	struct stat buf;
#endif

if(stat(filename,&buf) < 0)
	return(-1);
#ifdef VERSION6
	return(buf.size0 || buf.size1);
#else
	return( buf.st_size > 0 );
#endif
}




crfnames()
{
fname(textfname, "x");
fname(asmfname, "s");
fname(asmpass2, "a");
fname(initfname, "d");
fname(sortfname, "S");
fname(objfdefault, "o");
fname(prepfname, "p");
fname(optzfname, "z");
fname(setfname, "A");
}




rmf(fn)
register char *fn;
{
if(!debugflag && fn!=NULL && *fn!='\0')
	unlink(fn);
}





LOCAL fname(name, suff)
char *name, *suff;
{
sprintf(name, "/tmp/%s%d.%s", temppref, pid, suff);
}




dotchar(s)
register char *s;
{
for( ; *s ; ++s)
	if(s[0]=='.' && s[1]!='\0' && s[2]=='\0')
		return( s[1] );
return(NO);
}



char *lastfield(s)
register char *s;
{
register char *t;
for(t = s; *s ; ++s)
	if(*s == '/')
		t = s+1;
return(t);
}



char *lastchar(s)
register char *s;
{
while(*s)
	++s;
return(s-1);
}

char *setdoto(s)
register char *s;
{
*lastchar(s) = 'o';
return( lastfield(s) );
}



badfile(s)
char *s;
{
fatalstr("cannot open intermediate file %s", s);
}



ptr ckalloc(n)
int n;
{
ptr p, calloc();

if( p = calloc(1, (unsigned) n) )
	return(p);

fatal("out of memory");
/* NOTREACHED */
}





char *copyn(n, s)
register int n;
register char *s;
{
register char *p, *q;

p = q = (char *) ckalloc(n);
while(n-- > 0)
	*q++ = *s++;
return(p);
}



char *copys(s)
char *s;
{
return( copyn( strlen(s)+1 , s) );
}





oneof(c,s)
register c;
register char *s;
{
while( *s )
	if(*s++ == c)
		return(YES);
return(NO);
}



nodup(s)
char *s;
{
register char **p;

for(p = loadargs ; p < loadp ; ++p)
	if( !strcmp(*p, s) )
		return(NO);

return(YES);
}



static fatal(t)
char *t;
{
fprintf(diagfile, "Compiler error in file %s: %s\n", infname, t);
if(debugflag)
	abort();
done(1);
exit(1);
}




static fatali(t,d)
char *t;
int d;
{
char buff[100];
sprintf(buff, t, d);
fatal(buff);
}




static fatalstr(t, s)
char *t, *s;
{
char buff[100];
sprintf(buff, t, s);
fatal(buff);
}
err(s)
char *s;
{
fprintf(diagfile, "Error in file %s: %s\n", infname, s);
}

/* Code to generate initializations for DATA statements */

LOCAL int nch	= 0;
LOCAL FILEP asmfile;
LOCAL FILEP sortfile;

#include "ftypes"

static ftnint typesize[NTYPES]
	= { 1, SZADDR, SZSHORT, SZLONG, SZLONG, 2*SZLONG,
	    2*SZLONG, 4*SZLONG, SZLONG, 1, 1, 1};
static int typealign[NTYPES]
	= { 1, ALIADDR, ALISHORT, ALILONG, ALILONG, ALIDOUBLE,
	    ALILONG, ALIDOUBLE, ALILONG, 1, 1, 1};

dodata()
{
char buff[50];
char varname[XL+1], ovarname[XL+1];
int status;
flag erred;
ftnint offset, vlen, type;
register ftnint ooffset, ovlen;
ftnint nblank, vchar;
int size, align;
int vargroup;
ftnint totlen, doeven();

erred = NO;
ovarname[0] = '\0';
ooffset = 0;
ovlen = 0;
totlen = 0;
nch = 0;

sprintf(buff, "sort %s >%s", initfname, sortfname);
if(status = sys(buff))
	fatali("call sort status = %d", status);
if( (sortfile = fopen(sortfname, "r")) == NULL)
	badfile(sortfname);
if( (asmfile = fopen(asmfname, "a")) == NULL)
	badfile(asmfname);
pruse(asmfile, USEINIT);

while( rdname(&vargroup, varname) && rdlong(&offset) && rdlong(&vlen) && rdlong(&type) )
	{
	size = typesize[type];
	if( strcmp(varname, ovarname) )
		{
		prspace(ovlen-ooffset);
		strcpy(ovarname, varname);
		ooffset = 0;
		totlen += ovlen;
		ovlen = vlen;
		if(vargroup == 0)
			align = (type==TYCHAR || type==TYBLANK ?
					SZLONG : typealign[type]);
		else	align = ALIDOUBLE;
		totlen = doeven(totlen, align);
		if(vargroup == 2)
			prcomblock(asmfile, varname);
		else
			fprintf(asmfile, LABELFMT, varname);
		}
	if(offset < ooffset)
		{
		erred = YES;
		err("overlapping initializations");
		ooffset = offset;
		}
	if(offset > ooffset)
		{
		prspace(offset-ooffset);
		ooffset = offset;
		}
	if(type == TYCHAR)
		{
		if( rdlong(&vchar) )
			prch( (int) vchar );
		else
			fatal("bad intermediate file format");
		}
	else if(type == TYBLANK)
		{
		if( rdlong(&nblank) )
			{
			size = nblank;
			while( --nblank >= 0)
				prch( ' ' );
			}
		else
			fatal("bad intermediate file format");
		}
	else
		{
		putc('\t', asmfile);
		while	( putc( getc(sortfile), asmfile)  != '\n')
			;
		}
	if( (ooffset += size) > ovlen)
		{
		erred = YES;
		err("initialization out of bounds");
		}
	}

prspace(ovlen-ooffset);
totlen = doeven(totlen+ovlen, (ALIDOUBLE>SZLONG ? ALIDOUBLE : SZLONG) );
clf(&sortfile);
clf(&asmfile);
clf(&sortfile);
rmf(sortfname);
return(erred);
}




prspace(n)
register ftnint n;
{
register ftnint m;

while(nch>0 && n>0)
	{
	--n;
	prch(0);
	}
m = SZSHORT * (n/SZSHORT);
if(m > 0)
	prskip(asmfile, m);
for(n -= m ; n>0 ; --n)
	prch(0);
}




ftnint doeven(tot, align)
register ftnint tot;
int align;
{
ftnint new;
new = roundup(tot, align);
prspace(new - tot);
return(new);
}



rdname(vargroupp, name)
int *vargroupp;
register char *name;
{
register int i, c;

if( (c = getc(sortfile)) == EOF)
	return(NO);
*vargroupp = c - '0';

for(i = 0 ; i<XL ; ++i)
	{
	if( (c = getc(sortfile)) == EOF)
		return(NO);
	if(c != ' ')
		*name++ = c;
	}
*name = '\0';
return(YES);
}



rdlong(n)
register ftnint *n;
{
register int c;

for(c = getc(sortfile) ; c!=EOF && isspace(c) ; c = getc(sortfile) );
	;
if(c == EOF)
	return(NO);

for(*n = 0 ; isdigit(c) ; c = getc(sortfile) )
	*n = 10* (*n) + c - '0';
return(YES);
}




prch(c)
register int c;
{
static int buff[SZSHORT];

buff[nch++] = c;
if(nch == SZSHORT)
	{
	prchars(asmfile, buff);
	nch = 0;
	}
}
