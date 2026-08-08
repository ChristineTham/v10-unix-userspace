/*
 * The filesystem switch.  PLAN.md section 8a step 2.
 *
 * WHY THIS SHAPE.  V8 already had one.  sys/h/conf.h defines
 *
 *	struct fstypsw {
 *		int (*t_put)(); struct inode *(*t_get)(); int (*t_free)();
 *		int (*t_updat)(); int (*t_read)(); int (*t_write)();
 *		int (*t_trunc)(); int (*t_stat)(); int (*t_nami)();
 *		int (*t_mount)(); int (*t_ioctl)();
 *	};
 *
 * and sys/dev/conf.c fills it with four entries -- the ordinary filesystem, the
 * network filesystem, Killian's /proc, and mpx -- with iget.c, nami.c, rdwri.c
 * and ioctl.c all dispatching through it.  V8 had a VFS four years before Sun's
 * paper.  So the interface below answers to that one rather than inventing a
 * rival, and the correspondence is written next to each entry.
 *
 * WHERE IT DIFFERS, AND WHY IT HAS TO.  V8's operations take a `struct inode *'
 * and find their buffer in the u-area: `t_read(ip)' has no length argument
 * because the length is `u.u_count'.  This shim has neither an inode table nor
 * a u-area -- it hands out host descriptors -- so the operations here are
 * descriptor-shaped.  Building the inode/u-area substrate is real work, shared
 * with step 5 (which wants iget.c anyway), and doing it before there is a
 * second filesystem to justify it would be inventing a customer.
 *
 * ONE ENTRY TODAY.  Passthrough, reproducing the previous behaviour exactly.
 * That is the whole point of this step: the floor is replaced while the suites
 * stay green, so that when /proc arrives as the second entry a failure means
 * /proc rather than the switch.
 */

#ifndef V8SYS_VFS_H
#define V8SYS_VFS_H

struct v8_stat;

/*
 * Path resolution has two modes, and the distinction is load-bearing -- see
 * shim/NOTES.md.  V8P_LOOK asks whether the rootfs has the path, which is right
 * for a reader and unanswerable for a name being created; V8P_MAKE asks about
 * the parent instead.
 */
#define V8P_LOOK	0
#define V8P_MAKE	1

struct v8fstyp {
	const char *t_name;

	/*
	 * t_path -- V8's t_nami, reduced.  Turn a V8 path into whatever this
	 * filesystem's other operations want.  For passthrough that is a host
	 * path; for a server-backed type it would be a handle.
	 */
	char *(*t_path)(char *p, int mode);

	int   (*t_open)(char *rp, int flags, int mode);	/* t_get  */
	int   (*t_close)(int fd);			/* t_put  */
	long  (*t_read)(int fd, char *b, long n);	/* t_read */
	long  (*t_write)(int fd, char *b, long n);	/* t_write */
	long  (*t_seek)(int fd, long off, int whence);
	int   (*t_stat)(char *rp, struct v8_stat *st, int follow);   /* t_stat */
	int   (*t_fstat)(int fd, struct v8_stat *st);

	/*
	 * t_ioctl -- V8's own, and the slot this header used to say was
	 * deliberately absent "until a type needs it".  That type has arrived:
	 * PIOCGETPR is how ps(1) asks /proc for a struct proc, and proca.c
	 * answers it from prioctl (proca.c:290) through exactly this vector.
	 *
	 * Note what the arrival changed and what it did not.  The sgtty/termios
	 * translation in ioctl.c is not per-filesystem and did not move; it
	 * became the PASSTHROUGH type's implementation of this operation, which
	 * is what it always was in fact.  Only the dispatch is new.
	 */
	int   (*t_ioctl)(int fd, int cmd, char *arg);	/* t_ioctl */
};

/*
 * The mount table is the prefix list that used to be `v8dirs[]'.  Generalising
 * it rather than adding a second list beside it is deliberate: two prefix
 * tables that have to agree are the standing invitation kmem.c's one-table rule
 * exists to refuse.  A /proc entry is one more row here.
 */
struct v8fstyp *v8fs_typefor(const char *path);
struct v8fstyp *v8fs_fdtype(int fd);
void            v8fs_bind(int fd, struct v8fstyp *t);
void            v8fs_unbind(int fd);

extern struct v8fstyp v8fs_pass;

/*
 * /dev/fd -- the third type, and the one that answers for /dev/tty.  V8's
 * controlling terminal is not a device: open("/dev/fd/n") is dup(n), and
 * /dev/tty is the hard link at n = 3.  vfs.c has the four citations.
 */
extern struct v8fstyp v8fs_fdfs;

#endif /* V8SYS_VFS_H */
