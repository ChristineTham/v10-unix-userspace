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
 * ...AND THE VERSION THIS CLIENT ACTUALLY ASKS FOR, because the extension
 * below is not optional for it and 9P has a mechanism for saying so.
 *
 * Tversion IS the negotiation: a client offers a string, and a server answers
 * with the largest version it speaks or "unknown".  A conforming 9P2000 server
 * offered "9P2000.v8" answers "9P2000" -- which is exactly the signal this
 * client needs, because it sends P9_OFFCUR on every read and a server without
 * a cursor would hand it zero bytes from a file with a nonzero length.  That
 * is a silent empty file, and it is what happened before this existed.
 *
 * So the client refuses a mount it cannot read rather than reading nothing
 * from it.  A fallback -- client-side offsets against a conforming server, at
 * the cost of the dup/fork/exec correctness the cursor buys -- is possible and
 * is not written, because nothing here has a foreign server and an unexercised
 * rule cannot be seen to be incomplete.
 */
#define P9_VERSION_V8	"9P2000.v8"

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

/*
 * ------------------------------------------------- THE ONE EXTENSION, AND WHY
 *
 * 9P HAS NO SEEK, AND THAT IS NOT AN OVERSIGHT: Plan 9's KERNEL holds the file
 * offset, in the Chan behind the descriptor, and every Tread carries the
 * absolute offset the kernel computed.  9P is a pread/pwrite protocol because
 * the thing above it is a kernel.
 *
 * THIS PORT HAS NO KERNEL ABOVE IT.  The client is libv8sys, linked into each
 * V8 program, and a file offset in ITS memory is wrong three different ways at
 * once, all of them ordinary Unix:
 *
 *	dup(2)	   two descriptors, ONE offset.  Two client rows diverge.
 *	fork(2)	   parent and child share the offset.  A copied table does not.
 *	execve(2)  the offset survives.  A table in process memory does not --
 *		   vfs.c:167 recorded that before there was anything to lose.
 *
 * What all three share is that the offset belongs to the OPEN FILE
 * DESCRIPTION -- the `struct file' -- and not to the descriptor or to the
 * process.  And this design has an exact counterpart for it: ONE CONNECTION
 * PER open(2).  A dup shares the socket, a fork shares the socket, a program
 * replacing itself keeps the socket.  So the offset goes where the open file
 * description already is, which is the far end of the wire, and the client
 * holds no per-file state at all -- see shim/v8sys/p9cl.c.
 *
 * THE EXTENSION IS ONE CONCEPT AND IT IS CONFINED TO A SENTINEL.  A fid has a
 * cursor.  Tread with offset == P9_OFFCUR uses it and advances it; any other
 * offset is 9P's own pread, byte for byte, and DOES NOT TOUCH the cursor.
 * Twrite is in the rule too, and §8a step 5f is where it stopped being a trap:
 * the client had always sent P9_OFFCUR on Twrite while only do_read resolved
 * it, so a do_write missing do_read's two lines would have written every byte
 * at offset 0xFFFFFFFFFFFFFFFF.  Found by an auditor reading the two files
 * together, one step before anything could reach it.
 *
 * THE FIRST DRAFT OF THIS NOTE SAID A CONFORMING CLIENT COULD NOT TELL, AND
 * tests/streams/p9probe.c REFUTED IT WITHIN THE HOUR -- which is the right way
 * round, and worth leaving written down.  The probe reads a directory at
 * 2^64-1 on purpose, to prove the unsigned-offset crash guard is still there,
 * and that offset is now the sentinel.  So the honest statement is narrower:
 * the extension is invisible at every offset a conforming client can read a
 * byte from, and 2^64-1 is not one -- offset + count does not fit in the field
 * there, so a client sending it is probing rather than reading.  The probe's
 * case moved to 2^64-2, which exercises the identical arm.
 *
 * Tseek/Rseek then read and set that cursor, and they are numbered outside
 * 100..127 so that no conforming client can collide with them.
 *
 * ------------------------------------------------ AND THE SECOND EXTENSION,
 * WHICH IS THE SAME SENTENCE ABOUT A DIFFERENT SYSCALL.  9P HAS NO ACCESS,
 * and for the reason it has no seek: Plan 9's kernel decides permission when
 * it opens the file, and Plan 9 has no access(2) at all to ask in advance.
 * V7 does -- sys/sys2.c's saccess() -- and `test -r' and `test -w' are ordinary
 * programs in this world that call it.
 *
 * THE CLIENT CANNOT COMPUTE THE ANSWER AND HAS ALREADY BEEN WRONG TRYING.
 * v8s_access's first version recomputed permission from the image's mode bits
 * against the HOST's uid, while the server was running Bell Labs' access()
 * with u.u_uid == 0 and taking fio.c's root bypass -- so `test -r' said no on
 * every file of every image and `cat' printed it.  The bits were the server's
 * and the identity was the host's.  §8a step 5e replaced the computation with
 * a report of what the operations would do, which was right while every write
 * answered EROFS and stops being right at 5f.
 *
 * So the question goes over the wire and Bell Labs' own access() answers it,
 * with the server's identity on both sides.  Taccess carries a fid and V7's
 * three mode bits; Raccess carries nothing, because 9P already has Rerror and
 * "may I" is a question whose negative answer IS an errno.  Numbered beside
 * Tseek and outside 100..127 for the same reason.
 *
 * ------------------------------------------------- AND THE THIRD, WHICH THE
 * ABSENCE OF THE FIRST TWO WAS BEING USED AS A REASON NOT TO ADD.  9P HAS NO
 * LINK, and syscall.c and vfs.h both recorded that as a reason link(2) was
 * "not deferred work" -- alongside symlink, whose refusal is permanent.  But
 * "9P2000 has no message for it" is the exact situation that produced Tseek
 * and Taccess above.  Twice the answer to a missing message was to add one;
 * the third time it was written down as grounds for refusing.  The two cases
 * are not alike and the sentence had flattened them:
 *
 *	symlink  A V7 filesystem CANNOT REPRESENT one.  There is no i_mode
 *		 for it, readlink(2) is EINVAL here for the same reason, and
 *		 no message could change that.  Permanent, and correctly so.
 *
 *	link     A V7 filesystem IS BUILT ON hard links.  i_nlink is a field
 *		 in the inode, sys/sys2.c:458's link() is `ip->i_nlink++'
 *		 followed by a namei with NI_LINK, and nami.c:484's NI_LINK
 *		 arm IS ALREADY IN THE IMPORTED TREE, unreachable only because
 *		 nothing sent it a request.  That is chdir's shape -- a real
 *		 gap -- not symlink's.
 *
 * AND THE COST OF LEAVING IT WAS NOT A SLOW PATH.  Measured against a server
 * that had just accepted `echo > /mnt/f' and `mkdir /mnt/d': `ln /mnt/f
 * /mnt/g' answered "Read-only file system", and `mv' of a DIRECTORY failed
 * outright -- mv.c's mvdir() at :204 has no fork-and-cp fallback, so it
 * printed "mv: cannot link" and left the directory where it was.  Only mv of a
 * plain FILE degraded gracefully, and that is what made this look cosmetic.
 *
 * Tlink carries dfid (the new name's parent), fid (the existing file) and the
 * name, which is 9P2000.L's field order -- borrowed because it is the closest
 * real precedent, and numbered outside 100..127 with the other two rather than
 * at .L's 70, so that no conforming client can collide and nothing here claims
 * to speak .L.  Rlink carries nothing: like Raccess, the negative answer is an
 * errno and 9P already has Rerror for those.
 */
#define P9_Tseek	128
#define P9_Rseek	129
#define P9_Taccess	130
#define P9_Raccess	131
#define P9_Tlink	132
#define P9_Rlink	133

/*
 * ------------------------------------------------- AND A FOURTH, WHICH TLINK
 * FLUSHED OUT WITHIN THE HOUR.  9P HAS ONE REMOVE AND V7 HAS TWO SYSCALLS.
 *
 * Plan 9 has no rmdir(2) at all -- remove(2) takes anything -- so Tremove
 * carries no flag and a server must decide from the inode.  v8fsd did:
 * `isdir = (f->f_ip->i_mode & IFMT) == IFDIR', then NI_RMDIR or NI_DEL.  That
 * is the right reading of Tremove for a foreign client and the WRONG answer
 * for V7's unlink(2), which is `nmarg.flag = NI_DEL' unconditionally
 * (sys4.c:160-169) -- V7 unlinks a directory ENTRY without touching the
 * directory, and mv(1) is built on exactly that.
 *
 * IT WAS INVISIBLE UNTIL LINK EXISTED, which is this port's most repeated
 * shape.  With no way to give a directory a second name, every directory a
 * client could remove had i_nlink == 2, and NI_RMDIR and NI_DEL differ only
 * above that.  Tlink made mvdir's sequence reachable and all three
 * disagreements arrived at once, measured:
 *
 *   THE ERRNO.  nami.c:363's `if(dip->i_nlink <= 2) ... else EBUSY' refuses,
 *   where V7's unlink succeeds.  mv printed "?? cannot unlink".
 *
 *   THE ON-DISK RESULT.  NI_RMDIR sets `dip->i_nlink = 0' -- it DESTROYS the
 *   directory -- where NI_DEL decrements.  So even in the case that
 *   "worked", unlink of a directory freed an inode that V7 would have left
 *   with nlink 1 as an unattached directory for fsck to find.  Ours was
 *   tidier and was not V7.
 *
 *   AND THE FAILURE PATH CORRUPTS THE PARENT.  nami.c:361 does
 *   `if(dp->i_nlink > 0) dp->i_nlink--' BEFORE the EBUSY test two lines
 *   later, and the error arm does not put it back.  Measured with dcheck
 *   after one failed mv: root had 3 entries and a link count of 2.  That is
 *   upstream's own bug on upstream's own hardware and it is NOT fixed here
 *   -- src/sys is imported and a change there must be forced by the target.
 *   What is fixed is this port sending a message the caller never asked for.
 *
 * So: two syscalls, two messages.  Tunlink is unlink(2) and Tremove keeps its
 * conforming Plan 9 meaning for anyone else.
 *
 * THIS SENTENCE SAID "Tunlink is unlink(2) -- ALWAYS NI_DEL --" and that is
 * not what shipped.  v8fsd's removeop has to reconcile V7's unlink with a
 * fiction the client already tells (dotlink() absorbs rmdir(1)'s two dot
 * unlinks), so on a directory it asks whether this is the last name.  The
 * argument is in removeop; what matters here is that the wire message does not
 * carry the decision, so no reader of this header should think it does.  It
 * carries a fid and nothing else, exactly as Tremove does, and clunks it the
 * same way whatever the outcome.
 */
#define P9_Tunlink	134
#define P9_Runlink	135

/*
 * Taccess mode, which is V7's access(2) numbering exactly.  sys/sys2.c:541-546
 * is three tests of the form `if (uap->fmode & (IREAD>>6)) access(ip, IREAD)',
 * so the syscall's bits are the owner-position mode bits shifted DOWN by six:
 * 4, 2, 1.  Read rather than recalled -- a first draft of this comment had the
 * shift the other way round.  Spelled out here rather than reached through
 * <sys/stat.h> so that both ends read the same three numbers.
 */
#define P9_AREAD	4
#define P9_AWRITE	2
#define P9_AEXEC	1

/*
 * The sentinel.  All ones is the right choice rather than a free one: it is
 * the only offset a conforming client can never legitimately send, because a
 * read there could not return a byte -- offset + count would not fit in the
 * 64 bits the field has.
 */
#define P9_OFFCUR	0xffffffffffffffffULL

/* Tseek whence, V7's lseek(2) numbering exactly -- sys2.c's L_SET/L_INCR/L_XTND */
#define P9_SEEKSET	0
#define P9_SEEKCUR	1
#define P9_SEEKEND	2

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
 * a RELATIONSHIP and silently rewrites itself when one side moves.
 *
 * THE TWO ENDS OF THIS WIRE HAVE DIFFERENT DIRSIZes, and this comment used to
 * say "the server asserts the inequality where DIRSIZ is actually in scope"
 * as though there were one.  There are two, and the layer decides which:
 * v8fsd.c sees Bell Labs' 14, because it reads DISK RECORDS through
 * src/sys/h/dir.h, and p9cl.c sees this port's 254 through v8sys.h's
 * V8_DIRSIZ.  The delegated assertion therefore checked 15 while the sentence
 * doing the delegating was about 255 -- a guard that could not fail for the
 * reason it gave.  Both are asserted now, each in the file that can see its
 * own number, and only the client's is close: 256 against 255.
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
void	p9_pstatw(struct p9buf *b, const struct p9stat *s);	/* + the outer count */
void	p9_nostat(struct p9stat *s);				/* every field "do not touch" */

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
