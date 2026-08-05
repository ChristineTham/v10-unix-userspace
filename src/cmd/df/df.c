static	char *sccsid = "@(#)df.c	4.6 (Berkeley) 7/8/81";
#include <stdio.h>
#include <fstab.h>
#include <sys/param.h>
#include <sys/filsys.h>
#include <sys/fblk.h>
#include <sys/stat.h>
/*
 * df
 */

#define NFS	32	/* Max number of filesystems */

struct mtab {
	char path[FSNMLG];
	char spec[FSNMLG];
} mtab[NFS];
struct stat stb;
int dev;
#define	L10BS	6
#define	L10IS	5
#define PCTFW	3
int	DEVNMLG;		/* length of longest device name */
int	DIRNMLG;		/* length of longest mount point name */

char *mpath();

/*
 * PORT: libkmemu's answer, spelled here rather than by including its header.
 * shim/libkmemu/kmemu.h is modern C with prototypes, and V8's compiler is from
 * 1985 -- it has none.  Four longs in 1024-byte units; mtab.c is the other end.
 */
struct kmemu_fs { long blocks, bfree, files, ffree; };
int kmemu_fsstat();

daddr_t	blkno	= 1;

int	lflag;
int	iflag;

struct	filsys sblock;

int	fi;
daddr_t	alloc();

main(argc, argv)
register int argc;
register char **argv;
{
	register int i;
	register int r = 0;

	/*
	 * PORT: `argc >= 1' was `argv[1][0]' with no argv[1] to read.
	 *
	 * With no arguments argc is 1, so the test reaches argv[1] -- the NULL
	 * that terminates the vector -- and dereferences it.  With `df -i' it
	 * happens on the SECOND pass: the flag is consumed, argv is advanced,
	 * and argv[1] is the terminator again.  So every invocation of this
	 * program crashed here, which is why the segfault came before any output.
	 *
	 * On the VAX address 0 was inside the text segment and readable, so
	 * *(char *)0 returned a byte of the program, compared unequal to '-',
	 * and the loop simply ended.  macOS keeps page 0 unmapped.  Third time
	 * in this port -- refer5.c's prefix(".[", lookat()) and grap's were the
	 * others; CLAUDE.md names the class.
	 */
	while (argc > 1 && argv[1][0]=='-') {
		switch(argv[1][1]) {

		case 'l':
			lflag++;
			break;

		case 'i':
			iflag++;
			break;

		default:
			fprintf(stderr, "usage: df [-i] [-l] [filsys...]\n");
			exit(0);
		}
		argc--, argv++;
	}

	if ((i=open("/etc/mtab", 0)) >= 0) {
		r = read(i, mtab, sizeof mtab);	/* Probably returns short */
		(void) close(i);
		r /= sizeof mtab[0];
	}
	devlen(r);	/* reads in all of /etc/fstab, too */
	printf("%-*.*s %-*.*s %*.*s %*.*s %*.*s",
		DIRNMLG, DIRNMLG, "dir",
		DEVNMLG, DEVNMLG, "dev",
		L10BS, L10BS, "kbytes",
		L10BS, L10BS, "used",
		L10BS, L10BS, "free");
	if (lflag)
		printf(" %*.*s", L10BS, L10BS, "hardway");
	printf(" %*.*s", PCTFW + 1, PCTFW + 1, "%use");
	if (iflag)
		printf(" %*.*s %*.*s %*.*s",
			L10IS, L10IS, "iused",
			L10IS, L10IS, "ifree",
			PCTFW + 1, PCTFW + 1, "%ino");
	putchar('\n');
	if(argc <= 1) {
		for (i = 0; i < NFS && mtab[i].spec[0]; ++i)
			dfree(mtab[i].path);
		return (0);
	}

	for(i=1; i<argc; i++)
		dfree(argv[i]);
	return (0);
}

dfree(file)
char *file;
{
	register daddr_t i;
	register char	*mp;
	long	blocks;
	long	free;
	long	used;
	long	hardway;
	struct	stat stbuf;
	static char specbuf[FSNMLG + sizeof "/dev/"] = "/dev/";
	/* PORT: the argument as handed in, before the loop below replaces it
	 * with "/dev/<spec>" for the display columns.  statfs wants the mount
	 * point, and by then it is gone. */
	char	*mp0 = file;
	struct	kmemu_fs kfs;

	if(stat(file, &stbuf) == 0 && (stbuf.st_mode&S_IFMT) == S_IFDIR)
	{
		struct stat mstbuf;

		for (i = 0; i < NFS && mtab[i].spec[0]; ++i)
		{
			strcpy(&specbuf[5], mtab[i].spec);
			if(!stat(specbuf, &mstbuf) && mstbuf.st_rdev == stbuf.st_dev)
			{
				file = specbuf;
				break;
			}
		}
		if (i == NFS || mtab[i].spec[0] == '\0')
		{
			fprintf(stderr, "%s mounted on unknown device\n", file);
			return;
		}
	}
	else
	if (strncmp("/dev/", file, sizeof "/dev/" - 1) != 0)
		strcpy(&specbuf[5], file), file = specbuf;
	/*
	 * PORT: the superblock comes from libkmemu, not from block 1 of a disk.
	 *
	 * The original is
	 *	fi = open(file, 0); ... fstat(fi, &stb); dev = stb.st_rdev;
	 *	if (lflag) sync();
	 *	bread(1L, (char *)&sblock, sizeof(sblock));
	 * and there is no disk here to read: a macOS volume has no struct
	 * filsys anywhere on it, and opening the raw device needs root.  PLAN.md
	 * section 7 sanctions this one -- "df via statfs backend, V8 output
	 * format".  See PORTING.md; the alternative was manufacturing a fake
	 * disk, free list included, which is inventing data rather than
	 * reporting it.
	 *
	 * Everything below this point is untouched: the arithmetic, the widths,
	 * the printf and the -i columns are V8's.  dev is 0 because bit 64 clear
	 * is what selects BSIZE 1024 and INOPB 16, the units libkmemu answers in.
	 */
	if (kmemu_fsstat(mp0, &kfs) < 0) {
		fprintf(stderr,"cannot stat %s\n", mp0);
		return;
	}
	dev = 0;
	/* i-list size is what makes (s_isize-2)*INOPB come out as the inode
	 * count; both it and s_tinode are 16-bit in V7 and SATURATE here.  A
	 * 548-million-inode volume cannot be described by this superblock, and
	 * that is the format's ceiling rather than a guess -- a V8 df could not
	 * have shown it either.  df -i therefore reports the ceiling. */
	sblock.s_isize = kfs.files / INOPB(dev) + 2 > 65535 ?
	    65535 : kfs.files / INOPB(dev) + 2;
	sblock.s_fsize = kfs.blocks + sblock.s_isize;
	sblock.s_tfree = kfs.bfree;
	sblock.s_tinode = kfs.ffree > 65535 ? 65535 : kfs.ffree;
	sblock.s_nfree = 0;		/* there is no free list; -l says so */
	blocks = (long) sblock.s_fsize - (long)sblock.s_isize;
	free = sblock.s_tfree;
	used = blocks - free;
	if(BITFS(dev)) {
		blocks *= BSIZE(dev) / BSIZE(0);
		free *= BSIZE(dev) / BSIZE(0);
		used *= BSIZE(dev) / BSIZE(0);
	}
	printf("%-*.*s %-*.*s %*ld %*ld %*ld",
		DIRNMLG, DIRNMLG, mp = mpath(file),
		DEVNMLG, DEVNMLG, file + sizeof "/dev",
		L10BS, blocks, L10BS, used, L10BS, free);

	if (lflag) {
		hardway = 0;
		if(BITFS(dev))
			hardway = alloc();
		else
			while(alloc())
				hardway++;
		printf(" %*ld", L10BS, free = hardway);
	}
	printf(" %*.0f%%",
		PCTFW, blocks == 0 ?
		0.0 : (double) used / (double) blocks * 100.0);
	if (iflag) {
		int inodes = (sblock.s_isize - 2) * INOPB(dev);
		used = inodes - sblock.s_tinode;
		printf(" %*ld %*ld %*.0f%%",
			L10IS, used,
			L10IS, sblock.s_tinode, 
			PCTFW, inodes == 0 ?
			0.0 : (double) used / (double) inodes * 100.0);
	}
	printf("\n");
	close(fi);
}

daddr_t
alloc()
{
	int i, j, n;
	daddr_t b;
	struct fblk buf;

	if(!BITFS(dev)) {
		i = --sblock.s_nfree;
		if(i<0 || i>=NICFREE) {
			printf("bad free count, b=%D\n", blkno);
			return(0);
		}
		b = sblock.s_free[i];
		if(b == 0)
			return(0);
		if(b<sblock.s_isize || b>=sblock.s_fsize) {
			printf("bad free block (%D)\n", b);
			return(0);
		}
		if(sblock.s_nfree <= 0) {
			bread(b, (char *)&buf, sizeof(buf));
			blkno = b;
			sblock.s_nfree = buf.df_nfree;
			for(i=0; i<NICFREE; i++)
				sblock.s_free[i] = buf.df_free[i];
		}
		return(b);
	}
	n = 0;
	for(i = 0; i < BITMAP; i++)
		for(j = 0; j < 32; j++)		/* 32: bits per int */
			if(sblock.s_bfree[i] & (1 << j))
				n++;
	return(n * BSIZE(dev) / BSIZE(0));
}

bread(bno, buf, cnt)
daddr_t bno;
char *buf;
{
	register int n;
	extern errno;

	lseek(fi, bno<<BSHIFT(dev), 0);
	if((n=read(fi, buf, cnt)) != cnt) {
		printf("\nread error bno = %ld\n", bno);
		printf("count = %d; errno = %d\n", n, errno);
		exit(0);
	}
}

/*
 * Given a name like /dev/rrp0h, returns the mounted path, like /usr.
 */
char *mpath(file)
char *file;
{
	register int i;

	for (i=0; i<NFS; i++)
		if (eq(file, mtab[i].spec))
			return mtab[i].path;
	return "";
}

eq(f1, f2)
char *f1, *f2;
{
	if (strncmp(f1, "/dev/", 5) == 0)
		f1 += 5;
	if (strncmp(f2, "/dev/", 5) == 0)
		f2 += 5;
	if (strcmp(f1, f2) == 0)
		return 1;
	if (*f1 == 'r' && strcmp(f1+1, f2) == 0)
		return 1;
	if (*f2 == 'r' && strcmp(f1, f2+1) == 0)
		return 1;
	if (*f1 == 'r' && *f2 == 'r' && strcmp(f1+1, f2+1) == 0)
		return 1;
	return 0;
}

mtabcmp(mp0, mp1)
struct mtab *mp0;
struct mtab *mp1;
{
	/*
	 * don't let empty mtab slots sort to the front
	 * as dfree will break
	 * the wrong way to fix it: the whole algorithm is wrong
	 */
	if (mp0->path[0] == 0)
		return (1);
	if (mp1->path[0] == 0)
		return (-1);
	return (strncmp(mp0->path, mp1->path, sizeof (mp0->path)));
}

devlen(r)
register int r;
{
	register struct	fstab	*fsp;
	register int		i;

	DEVNMLG = 0;
	DIRNMLG = 0;
	if (setfsent() == 0)
		perror(FSTAB), exit(1);
	while( (fsp = getfsent()) != 0){
		if (  (strcmp(fsp->fs_type, FSTAB_RW) != 0)
			&&(strcmp(fsp->fs_type, FSTAB_RO) != 0) )
			continue;
		for (i = 0; mtab[i].spec[0]; ++i)
		{
			if (strncmp(mtab[i].spec, fsp->fs_spec + 5,
				sizeof fsp->fs_spec - 5) == 0)
				break;
		}
		if (i == r && i < NFS)
		{
			strncpy(mtab[r].spec, fsp->fs_spec + 5,
				sizeof fsp->fs_spec - 5);
			strncpy(mtab[r].path, fsp->fs_file, sizeof fsp->fs_file);
			++r;
		}
		if (DEVNMLG < (i = strlen(fsp->fs_spec)))
			DEVNMLG = i;
		if (DIRNMLG < (i = strlen(fsp->fs_file)))
			DIRNMLG = i;
	}
	endfsent();
	DEVNMLG -= sizeof "/dev";
	if (DEVNMLG < sizeof "dev" - 1)
		DEVNMLG = sizeof "dev" - 1;
	if (DIRNMLG < sizeof "dir" - 1)
		DIRNMLG = sizeof "dir" - 1;
	qsort(&mtab[0], r, sizeof mtab[0], mtabcmp);
}
