#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/dir.h>
#include <sys/user.h>
#include <sys/proc.h>
#include <sys/reg.h>
#include <sys/pioctl.h>

#define	SYSADR	0x80000000L

#define	UBASE	(SYSADR-UPAGES*NBPG)

#define Malloc(type,n)		(type *)malloc((n)*sizeof(type))
#define Calloc(type,n)		(type *)calloc((n),sizeof(type))
#define Realloc(type,ptr,n)	(type *)realloc(ptr,(n)*sizeof(type))
#define Nalloc(type,pp,n)	(type *)nalloc(pp, sizeof(type), (n)*sizeof(type))

char *malloc(), *calloc(), *realloc(), *nalloc(), *balloc();

#define	Ioctl(fd,c,buf)		(ioctl(fd, c, buf) >= 0)
#define	Seek(fd,n)		(lseek(fd, (int)(n), 0) == (int)(n))
#define	Read(fd,buf,n)		(read(fd, (char *)buf, (n)) == (n))

#define Kread(addr, destp)	(((long)(addr) & SYSADR) && Seek(kernel, addr) && \
				Read(kernel, (char *)(destp), sizeof(*(destp))))

#define Sread(fd, addr, destp)	(Seek(fd, addr) && \
				Read(fd, (char *)(destp), sizeof(*(destp))))

#define min(a,b)	((a) <= (b) ? (a) : (b))
#define max(a,b)	((a) >= (b) ? (a) : (b))
#define minmax(x,a,b)	min(b,max(a,x))

typedef struct Dirnode {
	struct Dirnode *next;
	struct direct *begin;
	struct direct *end;
} Dirnode;

typedef struct Select {
	long flag;
	char *id;
	dev_t dev; ino_t ino;
} Select;

#define	SELTTY	1
#define SELXFL	2
#define SELFIL	4

typedef struct Psline {
	int weight;
	char *string;
} Psline;

char *printp(), *fdprint(), *iprint();

/*
 * PORT: *ctime() added.  Upstream's own omission, and ps is the outlier --
 * date.c, ls.c, pr.c, who.c, fsck.c, dumpdir.c and restor.c all declare it.
 * V8's <time.h> declares no functions at all, so without this K&R gives
 * ctime() an implicit int return.
 *
 * On a VAX that fiction was free: int and char * were both four bytes, so
 * printp.c:24's `ctime(&up->u_start)+4' computed the right address anyway.
 * Under LP64 it is fatal, and specifically because of the `+4'.  The back end
 * deliberately does NOT narrow a signed-int CALL return -- see the long note
 * at the `mov %s, x0' in ccom-arm64/gencode.c, which exists because opendir
 * calls malloc undeclared -- so the pointer arrives intact in x0.  It is the
 * ARITHMETIC that loses it: `+4' is a PLUS of type int, and arm64_trunc()
 * emits `sxtw x9, w9' after it, which is correct for an int and destroys a
 * pointer.  Mach-O loads at 0x100000000, so bit 32 is set in every static
 * address and the truncated value lands in __PAGEZERO every time -- SIGSEGV
 * on `ps -T', not an intermittent one.
 *
 * Measured tree-wide by scanning the emitted code of all 97 installed
 * binaries for the shape (bl -> mov xN,x0 -> arithmetic -> sxtw): 64 sites,
 * of which 63 call a function that really does return int (strlen, dysize,
 * atoi, yacc's apack, troff's width/roman/decml/abc...).  This was the only
 * one.  PLAN.md S4j.
 */
char *gettty(), *getfs(), *getuname(), *getargs(), *memcpy(), *ctime();

Dirnode *devlist, *prlist, *getdir();

Select *selbeg, *selend;

int Fflag, fflag, hflag, lflag, Nflag, nflag, Tflag, uflag;

int aflag, rflag, tflag, xflag;

int mypid, myuid, drum, kernel;

char *progname;
