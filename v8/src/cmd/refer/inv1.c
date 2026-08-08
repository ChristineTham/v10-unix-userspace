# include "stdio.h"
# include "assert.h"

main(argc, argv)
	char *argv[];
{
/* make inverted file indexes.  Reads a stream from mkey which
   gives record pointer items and keys.  Generates a set of files
      a. NHASH pointers to file b.
      b. lists of record numbers.
      c. record pointer items.

   these files are named xxx.ia, xxx.ib, xxx.ic;
   where xxx is taken from arg1.
   If the files exist they are updated.
*/

FILE *fa, *fb, *fc, *fta, *ftb, *ftc, *fd;
int nhash = 256;
int appflg = 1;
int keepkey = 0, pipein = 0;
char nma[100], nmb[100], nmc[100], com[100], nmd[100];
char tmpa[20], tmpb[20], tmpc[20];
char *remove = NULL;
int chatty = 0, docs, hashes, fp[2], fr, fw, pfork, pwait, status;
int i,j,k;
long keys;
int iflong =0;
char *sortdir;

sortdir = (access("/crp/tmp", 06)==0) ? "/crp/tmp" : "/usr/tmp";
/*
 * PORT: argc, and this is hunt1.c's bug in the same directory.
 *
 * Bare `inv' has argc == 1, so argv[1] is the vector's terminating NULL and
 * this test reads it before the program does anything.  The VAX read the 0207
 * at address 0, which is not '-', so the loop was skipped -- and line 61 below
 * then says `argc >= 2 ? argv[1] : "Index"', so the author DID guard the later
 * use of the same pointer and not the loop.
 *
 * `argc > 1' rather than a null check, for hunt's reason: the -i arm does its
 * own `argc--; argv++', so after a dangling option argv[1] is past the
 * terminator, where a null check proves nothing.
 *
 * FOUND BY THE CRASH PROBE, NOT BY THE SWEEP THAT FIXED hunt.  The static
 * audit of S4i read hunt1.c and missed inv1.c four files away, with the same
 * shape and the same fix -- which is the argument for running the empirical
 * half at all.  52 of inv's 53 probe invocations died here.
 */
while (argc > 1 && argv[1][0] == '-')
	{
	switch(argv[1][1])
		{
		case 'h': /* size of hash table */
			nhash = atoi (argv[1]+2); break;
		case 'n': /* new, don't append */
			appflg=0; break;
		case 'a': /* append to old file */
			appflg=1; break;
		case 'v': /* verbose output */
			chatty=1; break;
		case 'd': /* keep keys on file .id for check on searching */
			keepkey=1; break;
		case 'p': /* pipe into sort (saves space, costs time)*/
			pipein = 1; break;
		case 'i': /* input is on file, not stdin */
			close(0);
			if (open(argv[2], 0) != 0)
				err("Can't read input %s", argv[2]);
			if (argv[1][2]=='u') /* unlink */
				remove = argv[2];
			argc--; argv++;
			break;
		}
	argc--;
	argv++;
	}

strcpy (nma, argc >= 2 ? argv[1] : "Index");
strcpy (nmb, nma);
strcpy (nmc, nma);
strcpy (nmd, nma);
strcat (nma, ".ia");
strcat (nmb, ".ib");
strcat (nmc, ".ic");
strcat (nmd, ".id");

sprintf(tmpa, "junk%di", getpid());
if (pipein)
	{
	pipe(fp); fr=fp[0]; fw=fp[1];
	if ( (pfork=fork()) == 0)
		{
		close(fw);
		close(0);
		_assert(dup(fr)==0);
		close(fr);
		execl("/bin/sort", "sort", "-T", sortdir, "-o", tmpa, 0);
		execl("/usr/bin/sort", "sort", "-T", sortdir, "-o", tmpa, 0);
		_assert(0);
		}
	_assert(pfork!= -1);
	close(fr);
		fta = fopen("/dev/null", "w");
		close(fta->_file);
		fta->_file = fw;
	}
else /* use tmp file */
	{
	fta = fopen(tmpa, "w");
	_assert (fta != NULL);
	}
fb = 0;
if (appflg )
	{
	if (fb = fopen(nmb, "r"))
		{
		sprintf(tmpb, "junk%dj", getpid());
		ftb = fopen(tmpb, "w");
		if (ftb==NULL)
			err("Can't get scratch file %s",tmpb);
		nhash = recopy(ftb, fb, fopen(nma, "r"));
		fclose(ftb);
		}
	else
		appflg=0;
	}
fc = fopen(nmc,  appflg ? "a" : "w");
/* The `if (keepkey)' that stood here made the else branch of the ternary
   unreachable, so with no -d flag `fd' kept whatever was on the stack --
   measured as 0x1ee41dc28, not 0.  newkeys() then took its `if (fd)' path and
   appended ";%ld,%d" to every .ic line, which result() in hunt5.c truncates at
   the ';' -- taking the newline with it, so refer saw a tag with no newline and
   said "No such paper".  The ternary already handles both cases.  See PORTING.md */
fd = keepkey ? fopen(nmd, "w") : 0;
docs = newkeys(fta, stdin, fc, nhash, fd, &iflong);
fclose(stdin);
if (remove != NULL)
	unlink(remove);
fclose(fta);
if (pipein)
	{
	pwait = wait(&status);
	printf("pfork %o pwait %o status %d\n",pfork,pwait,status);
	_assert(pwait==pfork);
	_assert(status==0);
	}
else
	{
	sprintf(com, "sort -T %s %s -o %s", sortdir, tmpa, tmpa);
	system(com);
	}

if (appflg)
	{
	sprintf(tmpc, "junk%dk", getpid());
	sprintf(com, "mv %s %s", tmpa, tmpc);
	system(com);
	sprintf(com, "sort -T %s  -m %s %s -o %s", sortdir,
		tmpb, tmpc, tmpa);
	system(com);
	}
fta = fopen(tmpa, "r");
fa = fopen(nma, "w");
fb = fopen(nmb, "w");
whash(fta, fa, fb, nhash, iflong, &keys, &hashes);
fclose(fta);
# ifndef D1
unlink(tmpa);
# endif
if (appflg)
	{
	unlink(tmpb);
	unlink(tmpc);
	}
if (chatty)
	
	printf ("%ld key occurrences,  %d hashes, %d docs\n",
		keys, hashes, docs);
}
