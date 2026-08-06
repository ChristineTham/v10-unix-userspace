static char *sccsid = "@(#)mkdir.c	4.1 (Berkeley) 10/1/80";
/*
** make directory
*/

#include	<signal.h>
#include	<stdio.h>

int	Errors = 0;
char	*strcat();
char	*strcpy();

main(argc, argv)
char *argv[];
{

	signal(SIGHUP, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, SIG_IGN);

	if(argc < 2) {
		fprintf(stderr, "mkdir: arg count\n");
		exit(1);
	}
	while(--argc)
		mkdir(*++argv);
	exit(Errors!=0);
}

mkdir(d)
char *d;
{
	/*
	 * PORT: 1024, not V7's 128.  Both are filled by unbounded copies --
	 * strncpy(pname, d, slash) and strcpy(dname, d) -- so the argument's
	 * length is the only bound there is, and 128 was chosen when a name was
	 * DIRSIZ = 14 bytes and 128 held nine components.  This port raises
	 * DIRSIZ to 254 and macOS NAME_MAX is 255, so ONE legal component
	 * overruns both.  Measured: a 255-character name SIGSEGVs, and it does
	 * so after creating the directory and unlinking it again, so it destroys
	 * what it made and reports nothing.
	 *
	 * 1024 is macOS's PATH_MAX -- the longest argument the kernel will hand
	 * over -- which makes the copies bounded by construction rather than by
	 * a guard that has to be maintained.  src/cmd/mkdir/PORTING.md.
	 */
	char pname[1024], dname[1024];
	register i, slash = 0;

	pname[0] = '\0';
	for(i = 0; d[i]; ++i)
		if(d[i] == '/')
			slash = i + 1;
	if(slash)
		strncpy(pname, d, slash);
	strcpy(pname+slash, ".");
	if (access(pname, 02)) {
		fprintf(stderr,"mkdir: cannot access %s\n", pname);
		++Errors;
		return;
	}
	if ((mknod(d, 040777, 0)) < 0) {
		fprintf(stderr,"mkdir: cannot make directory %s\n", d);
		++Errors;
		return;
	}
	chown(d, getuid(), getgid());
	strcpy(dname, d);
	strcat(dname, "/.");
	if((link(d, dname)) < 0) {
		fprintf(stderr, "mkdir: cannot link %s\n", dname);
		unlink(d);
		++Errors;
		return;
	}
	strcat(dname, ".");
	if((link(pname, dname)) < 0) {
		fprintf(stderr, "mkdir: cannot link %s\n",dname);
		dname[strlen(dname)] = '\0';
		unlink(dname);
		unlink(d);
		++Errors;
	}
}
