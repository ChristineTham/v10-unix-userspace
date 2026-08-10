/*
 * 9P2000 -- the wire between the shim and its file servers.  PLAN.md §8a.
 *
 * WHY THERE IS A WIRE AT ALL.  §8a step 5e was costed as a fourth type in
 * shim/v8sys/vfs.c that called into libv8kern directly, and that is not
 * available: linking the kernel archive beside a V8 program is 56 symbol
 * collisions over 29 programs (25 of them silent -- `cat's char buf[4096]
 * becomes an eight-byte pointer and the program still exits 0), and
 * independently vfs.c:167 had already recorded that a descriptor table in
 * process memory cannot survive exec, so `cat /mnt/a > /mnt/b' could never
 * work.  shim/kern/NOTES.md has both measurements.  Either alone forces the
 * kernel into a separate process, and once it is there something has to be
 * spoken between them.
 *
 * WHY 9P AND NOT SOMETHING SMALLER.  PLAN.md §8a: it is specified and stable
 * since 2000, it is about thirteen messages, it is designed for exactly this
 * shape (many clients, one authority, per-client fids), and there are
 * reference implementations to check against.  An ad-hoc protocol would be
 * smaller today and would have no second opinion available ever.
 *
 * Plain 9P2000.  Not .u, whose Unix extensions carry things V8's userspace
 * does not have; not .L, which is Linux-shaped.
 *
 * ONE CODEC, COMPILED TWICE, AND THAT IS THE POINT OF THIS DIRECTORY.  The
 * client half lives in libv8sys and may name no libc function; the server is
 * an ordinary host binary that links libv8kern and has all of libc.  Those are
 * two different worlds, and the obvious arrangement -- an encoder in each --
 * is the standing invitation vfs.c's one-table rule exists to refuse: two
 * things that must agree byte for byte, with nothing to say when they stop.
 * So this file names no libc function either, which is the strictly harder of
 * the two constraints, and both sides compile the same source.
 *
 * The one thing that cannot be shared is the I/O primitive, so it is the seam:
 * p9_io_read and p9_io_write are declared here and defined by whoever links
 * this.  The framing LOOP stays here on purpose -- a short read is the bug
 * that only appears under load, and two copies of that loop is one copy that
 * is wrong.
 */

#ifndef V8_P9_H
#define V8_P9_H

/*
 * WIDTHS ARE SAID OUT LOUD, for the reason src/include/PORTING.md gives about
 * on-disk records: a wire format has an end that is not ours.  `unsigned' is
 * 32 bits here by LP64 and would be 64 under ILP64, and the difference would
 * be four extra bytes in every message with nothing local to notice.
 */
typedef unsigned char		p9_u8;
typedef unsigned short		p9_u16;
typedef unsigned int		p9_u32;
typedef unsigned long long	p9_u64;

_Static_assert(sizeof(p9_u8)  == 1, "p9_u8 is not one byte");
_Static_assert(sizeof(p9_u16) == 2, "p9_u16 is not two bytes");
_Static_assert(sizeof(p9_u32) == 4, "p9_u32 is not four bytes");
_Static_assert(sizeof(p9_u64) == 8, "p9_u64 is not eight bytes");

/* ------------------------------------------------------------ the protocol */

#define P9_VERSION	"9P2000"

/*
 * msize is negotiated, and this is what the client proposes.  8192 + 24 is
 * u9fs's and plan9port's customary pair: the 24 is IOHDRSZ, the largest
 * fixed part any message puts in front of its data (Twrite's
 * size[4] Twrite tag[2] fid[4] offset[8] count[4] = 23, rounded up), so a
 * client may read or write a full 8192 without the header pushing the message
 * over msize.
 */
#define P9_IOHDRSZ	24
#define P9_MSIZE	(8192 + P9_IOHDRSZ)

/* The smallest legal message: size[4] type[1] tag[2]. */
#define P9_HDRSZ	7

#define P9_NOTAG	0xffff		/* Tversion's tag */
#define P9_NOFID	0xffffffffU	/* Tattach's afid when there is no auth */

#define P9_Tversion	100
#define P9_Rversion	101
#define P9_Tauth	102
#define P9_Rauth	103
#define P9_Tattach	104
#define P9_Rattach	105
/* 106 is Terror and is deliberately not a message: only servers report errors */
#define P9_Rerror	107
#define P9_Tflush	108
#define P9_Rflush	109
#define P9_Twalk	110
#define P9_Rwalk	111
#define P9_Topen	112
#define P9_Ropen	113
#define P9_Tcreate	114
#define P9_Rcreate	115
#define P9_Tread	116
#define P9_Rread	117
#define P9_Twrite	118
#define P9_Rwrite	119
#define P9_Tclunk	120
#define P9_Rclunk	121
#define P9_Tremove	122
#define P9_Rremove	123
#define P9_Tstat	124
#define P9_Rstat	125
#define P9_Twstat	126
#define P9_Rwstat	127

/* qid type bits, and the mode bits that mirror them in a stat */
#define P9_QTFILE	0x00
#define P9_QTTMP	0x04
#define P9_QTAUTH	0x08
#define P9_QTMOUNT	0x10
#define P9_QTEXCL	0x20
#define P9_QTAPPEND	0x40
#define P9_QTDIR	0x80

#define P9_DMDIR	0x80000000U
#define P9_DMAPPEND	0x40000000U
#define P9_DMEXCL	0x20000000U

/* Topen/Tcreate mode.  The low two bits are V7's open(2) flags exactly. */
#define P9_OREAD	0
#define P9_OWRITE	1
#define P9_ORDWR	2
#define P9_OEXEC	3
#define P9_OTRUNC	0x10
#define P9_ORCLOSE	0x40

#define P9_MAXWELEM	16		/* wnames per Twalk, fixed by the spec */

/*
 * 256 because it must exceed the longest name any namespace on either side of
 * this wire can produce, and that is V8's DIRSIZ, which this port raises to
 * 254.  Deliberately a round number rather than `DIRSIZ + 2': mv(1)'s
 * MAXN - DIRSIZ - 2 is this repo's standing example of a constant that encodes
 * a RELATIONSHIP and silently rewrites itself when one side moves.  The server
 * asserts the inequality where DIRSIZ is actually in scope.
 */
#define P9_NAMELEN	256

struct p9qid {
	p9_u8	q_type;
	p9_u32	q_vers;
	p9_u64	q_path;
};

#define P9_QIDSZ	13		/* type[1] version[4] path[8] */

struct p9stat {
	p9_u16		s_type;
	p9_u32		s_dev;
	struct p9qid	s_qid;
	p9_u32		s_mode;
	p9_u32		s_atime;
	p9_u32		s_mtime;
	p9_u64		s_length;
	char		s_name[P9_NAMELEN];
	char		s_uid[P9_NAMELEN];
	char		s_gid[P9_NAMELEN];
	char		s_muid[P9_NAMELEN];
};

/* ------------------------------------------------------------- the cursor */

/*
 * EVERY get AND put GOES THROUGH THIS, AND THE ERROR IS STICKY.  A server
 * reads its messages off a socket, so a malformed one is not a hypothetical:
 * the length in the header and the fields inside it are two claims by the same
 * untrusted party, and they need not agree.  A parser that bounds-checks at
 * each call site is a parser where one call site does not, so the check is in
 * the primitive and the answer is read once at the end.
 *
 * A get past the end returns 0 as well as setting b_bad.  That matters: it
 * means a truncated message decodes to something harmless rather than to
 * whatever was in the buffer from the message before it, which is the
 * ttyprobe short-read lesson -- a case has to be a pure function of what it
 * was sent.
 */
struct p9buf {
	unsigned char	*b_base;	/* the message's first byte */
	unsigned char	*b_p;		/* the next byte to read or write */
	unsigned char	*b_end;		/* one past the last usable byte */
	int		 b_bad;		/* sticky: something did not fit */
};

void	p9_init(struct p9buf *b, void *base, long len);
int	p9_ok(struct p9buf *b);
long	p9_len(struct p9buf *b);	/* bytes used so far */

void	p9_p8(struct p9buf *b, p9_u32 v);
void	p9_p16(struct p9buf *b, p9_u32 v);
void	p9_p32(struct p9buf *b, p9_u32 v);
void	p9_p64(struct p9buf *b, p9_u64 v);
void	p9_pstr(struct p9buf *b, const char *s);
void	p9_pdata(struct p9buf *b, const void *p, long n);
void	p9_pqid(struct p9buf *b, const struct p9qid *q);
void	p9_pstat(struct p9buf *b, const struct p9stat *s);

p9_u32	p9_g8(struct p9buf *b);
p9_u32	p9_g16(struct p9buf *b);
p9_u32	p9_g32(struct p9buf *b);
p9_u64	p9_g64(struct p9buf *b);
long	p9_gstr(struct p9buf *b, char *dst, long max);
unsigned char *p9_gdata(struct p9buf *b, long n);
void	p9_gqid(struct p9buf *b, struct p9qid *q);
int	p9_gstat(struct p9buf *b, struct p9stat *s);

/* ------------------------------------------------------------- the framing */

/*
 * p9_hdr reserves size[4] and writes type[1] tag[2]; p9_fin goes back and
 * fills the size in.  Nothing else may write those four bytes, which is why
 * the base is in the cursor rather than passed to both.
 */
void	p9_hdr(struct p9buf *b, void *base, long max, int type, p9_u32 tag);
long	p9_fin(struct p9buf *b);

/*
 * The seam.  libv8sys defines these over rawsys; the server over libc.  Both
 * return the byte count, 0 at end of file, or -1.  They are NOT required to
 * transfer everything asked for -- handling that is p9_send/p9_recv's job, and
 * having it in one place is the reason this file exists.
 */
extern long p9_io_read(int fd, void *buf, long n);
extern long p9_io_write(int fd, const void *buf, long n);

long	p9_send(int fd, struct p9buf *b);
long	p9_recv(int fd, void *buf, long max);

#endif /* V8_P9_H */
