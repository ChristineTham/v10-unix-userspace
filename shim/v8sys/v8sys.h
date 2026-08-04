/*
 * libv8sys -- the layer that stands in for the VAX kernel.
 *
 * V8's libc reaches the kernel through 63 hand-written assembly stubs (plus 4
 * more in libjobs), each a single `chmk` instruction carrying a syscall number.
 * That is the entire contract between the V8 world and the machine underneath
 * it, and replacing those stubs with this library is what lets authentic V8
 * userspace run on a system that is not a VAX.
 *
 * Everything here is modern C compiled by clang.  It is NOT authentic V8 code
 * and does not pretend to be -- it is the seam.  Above it, V8 sources are
 * ported; below it, macOS and Linux.
 *
 * CALLING CONVENTION.  v8cc passes arguments in one positional sequence in
 * x0-x7, which coincides with AAPCS64 for every non-variadic function of eight
 * arguments or fewer.  No syscall stub exceeds that, and none is variadic, so
 * the boundary needs no thunks.  (Variadic calls across the boundary do NOT
 * work -- Apple passes those on the stack.  V8's own printf lives above the
 * seam, so this never arises.)
 *
 * WHAT THE V8 WORLD SEES.  Errno values, signal numbers, struct stat layout and
 * directory records are all V8's, translated here.  A ported program printing
 * strerror(errno) prints the authentic V8 message, and one reading a directory
 * with read(2) gets authentic 16-byte V7 records.
 */

#ifndef V8SYS_H
#define V8SYS_H

/* ------------------------------------------------------------ V8 types */

typedef unsigned short	v8_ino_t;	/* 16 bits: see PLAN.md S2 */
typedef unsigned short	v8_dev_t;
typedef long		v8_off_t;
typedef long		v8_time_t;

/*
 * struct stat as V8 lays it out (v8/usr/include/sys/stat.h).  32 bytes on the
 * VAX, and it must stay byte-identical here because ported programs read the
 * fields directly and some copy the struct around.
 *
 * st_ino is 16 bits, which no modern filesystem respects -- inode numbers are
 * folded, never to zero, in stat_translate().
 */
/*
 * macOS defines st_atime/st_mtime/st_ctime as macros expanding to
 * st_atimespec.tv_sec, which would rewrite the field names below into
 * nonsense.  V8 programs spell them the plain way, so the macros are undone
 * here -- the host struct stat is only touched inside syscall.c, before this
 * point in that file.
 */
#undef st_atime
#undef st_mtime
#undef st_ctime

struct v8_stat {
	v8_dev_t	st_dev;
	v8_ino_t	st_ino;
	unsigned short	st_mode;
	short		st_nlink;
	short		st_uid;
	short		st_gid;
	v8_dev_t	st_rdev;
	v8_off_t	st_size;
	v8_time_t	st_atime;
	v8_time_t	st_mtime;
	v8_time_t	st_ctime;
};

/* V8 mode bits.  No S_IFIFO and no S_IFSOCK: V8 had neither. */
#define V8_S_IFMT	0170000
#define V8_S_IFDIR	0040000
#define V8_S_IFCHR	0020000
#define V8_S_IFBLK	0060000
#define V8_S_IFREG	0100000
#define V8_S_IFLNK	0120000

/*
 * A V7 directory entry, which is what V8 has on disk: a 16-byte record of a
 * 2-byte inode number and a 14-byte name, NOT null-terminated when it fills the
 * field.  44 commands read directories with read(2) and expect exactly this,
 * and V8's own readdir() is itself a shim over the same format.
 */
/*
 * Must match DIRSIZ in src/include/dir.h -- see the note there for why it is
 * 254 and not V7's 14.
 */
#define V8_DIRSIZ 254
struct v8_direct {
	v8_ino_t	d_ino;
	char		d_name[V8_DIRSIZ];
};

/* ------------------------------------------------------- errno mapping */

/* v8/usr/include/errno.h.  ELOOP is a V8 addition, out of numeric order. */
#define V8_EPERM	1
#define V8_ENOENT	2
#define V8_ESRCH	3
#define V8_EINTR	4
#define V8_EIO		5
#define V8_ENXIO	6
#define V8_E2BIG	7
#define V8_ENOEXEC	8
#define V8_EBADF	9
#define V8_ECHILD	10
#define V8_EAGAIN	11
#define V8_ENOMEM	12
#define V8_EACCES	13
#define V8_EFAULT	14
#define V8_ENOTBLK	15
#define V8_EBUSY	16
#define V8_EEXIST	17
#define V8_EXDEV	18
#define V8_ENODEV	19
#define V8_ENOTDIR	20
#define V8_EISDIR	21
#define V8_EINVAL	22
#define V8_ENFILE	23
#define V8_EMFILE	24
#define V8_ENOTTY	25
#define V8_ETXTBSY	26
#define V8_EFBIG	27
#define V8_ENOSPC	28
#define V8_ESPIPE	29
#define V8_EROFS	30
#define V8_EMLINK	31
#define V8_EPIPE	32
#define V8_EDOM		33
#define V8_ERANGE	34
#define V8_ELOOP	35

extern int v8_errno;		/* the world's errno lives here */

int v8sys_errno(int hosterr);	/* host errno -> V8 errno */
int v8sys_faile(int hosterr);	/* map a captured host errno, return -1 */

/* ------------------------------------------------- directory emulation */

int v8sys_isdirfd(int fd);
long v8sys_dirread(int fd, void *buf, long n);
void v8sys_dirclose(int fd);
int v8sys_diropen(const char *path, int fd);

/* ------------------------------------------------------------- signals */

int v8sys_signo_to_host(int v8sig);
int v8sys_signo_from_host(int hostsig);

#endif /* V8SYS_H */
