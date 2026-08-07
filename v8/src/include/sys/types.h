/*
 * Basic system types and major/minor device constructing/busting macros.
 *
 * ONE LINE IS PATCHED HERE -- daddr_t -- and the reason is the only one in this
 * port that comes from outside the machine.  Every other patched header answers
 * to macOS or to AAPCS64; this one answers to a 1985 disk.
 *
 * V8's own VAX compiler settles the width, and it says so in one line --
 * `# define NOLONG', commented "map longs to ints", at line 19 of third_party's
 * cmd/ccom/vax/macdefs.h.  So on the VAX `long' and `int' were the SAME
 * 32-bit type, and every `long' field in an on-disk structure is four bytes.
 * compiler/ccom-arm64/macdefs.h deliberately does not define NOLONG -- its
 * comment says leaving it undefined is what makes LP64 expressible at all --
 * so SZLONG is 64 here and every one of those fields silently doubled.
 *
 * The tree ALREADY CONTRADICTED ITSELF about this, which is why the change is
 * not a matter of taste.  <sys/param.h> hardcodes `NMASK(0) 0377' and
 * `NSHIFT(0) 8', both of which say an indirect block holds 256 addresses,
 * while `NINDIR(dev) (BSIZE(dev)/sizeof(daddr_t))' -- one line below them, in
 * the same file -- was computing 128.  Likewise `INOPB(0)' is hardcoded 16 and
 * sizeof(struct dinode) had become 80, so itod()/itoo() would have put inode 17
 * at offset 1280 of a 1024-byte block.  The constants are upstream's; it is the
 * type that drifted.
 *
 * WHY daddr_t AND NOT time_t OR off_t, which broke the same way: a daddr_t
 * never crosses the shim seam.  It is a disk block number, it appears in
 * exactly one program outside these headers (df, which reads a real
 * superblock and needs it narrow to do so), and nothing hands one to macOS.
 * time_t and off_t are handed to macOS constantly and are 64 bits there.  So
 * those two are narrowed per FIELD, in the two headers that describe disk
 * records -- <sys/ino.h> and <sys/filsys.h> -- and nowhere else.
 */

#ifndef _TYPES_
#define	_TYPES_

/* major part of a device */
#define	major(x)	((int)(((unsigned)(x)>>8)&0377))

/* minor part of a device */
#define	minor(x)	((int)((x)&0377))

/* make a device number */
#define	makedev(x,y)	((dev_t)(((x)<<8) | (y)))

typedef	unsigned char	u_char;
typedef	unsigned short	u_short;
typedef	unsigned int	u_int;
typedef	unsigned long	u_long;

typedef	struct	_physadr { int r[1]; } *physadr;
typedef	int	daddr_t;	/* upstream `long': see the note above */
typedef	char *	caddr_t;
typedef	u_short	ino_t;
typedef	long	swblk_t;
typedef	long	size_t;
typedef	long	time_t;
typedef	long	label_t[14];
typedef	u_short	dev_t;
typedef	long	off_t;
typedef long	portid_t;

#ifndef NBBY
#include <sys/param.h>
#endif NBBY
/*
 *	Set of fds used with the select system call.
 *	The macros depend on NBPW, NBBY, & NOFILE from sys/param.h.
 */
#define FDWORDS		(NOFILE+NBPW*NBBY-1)/(NBPW*NBBY)
typedef struct		fd_set { unsigned long fds_bits[FDWORDS]; } fd_set;
#define FD_SET(n,s)	(s).fds_bits[(n)/(NBPW*NBBY)] |= 1<<((n)%(NBPW*NBBY))
#define FD_CLR(n,s)	(s).fds_bits[(n)/(NBPW*NBBY)] &= ~(1<<((n)%(NBPW*NBBY)))
#define FD_ISSET(n,s)	((s).fds_bits[(n)/(NBPW*NBBY)] & (1<<((n)%(NBPW*NBBY))))
#define FD_ZERO(s)	{int i; for(i=0;i<FDWORDS;i++)s.fds_bits[i]=0; }

#endif _TYPES_
