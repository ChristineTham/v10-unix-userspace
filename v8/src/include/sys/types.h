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

/*
 * PORT: the widths an on-disk or on-tape record fixes, said out loud.
 *
 * Every field of a 1985 record has a width that belongs to the RECORD and not
 * to the compiler.  Until now the headers expressed that by writing `int' and
 * relying on this port being LP64 -- true today, and true only by coincidence
 * of the data model.  `int di_size' does not mean "an int"; it means "exactly
 * four bytes, because a VAX wrote four bytes there".  These typedefs let the
 * struct say the second thing.
 *
 * IT IS ALSO WHY THE MODEL CANNOT MOVE.  V8's ccom has exactly four integer
 * types -- CHAR SHORT INT LONG, manifest.h:224-227, no `long long' anywhere in
 * the front end -- and this port has to express exactly four widths: 8 for
 * char, 16 for ino_t and di_mode, 32 for daddr_t and the on-disk times, 64 for
 * a pointer.  Four types, four widths, so char/short/int/long must be
 * 8/16/32/64 and that assignment IS LP64.  Making `int' 64 bits leaves nothing
 * that can spell 32, and every on-disk field would have to become char[4] with
 * hand-packing -- which would mean editing the authentic programs that read
 * them.  Measured, by building the tree that way: it fails first in the
 * initialiser path, and the disk formats are where it would end.
 *
 * So these are not a step towards changing the model.  They are the opposite:
 * they pin the places that would have to be revisited if it ever did, and they
 * make a width a property of the format rather than a property of the target.
 *
 * char stays char.  A char array in a record is bytes, not a narrow integer.
 */
typedef	short		v8_i16;		/* exactly 2 bytes */
typedef	unsigned short	v8_u16;
typedef	int		v8_i32;		/* exactly 4 bytes -- a VAX `long' */
typedef	unsigned int	v8_u32;

typedef	struct	_physadr { int r[1]; } *physadr;
typedef	v8_i32	daddr_t;	/* upstream `long': see the note above */
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
