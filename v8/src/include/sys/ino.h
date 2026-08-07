/*
 * Inode structure as it appears on
 * a disk block.
 *
 * PATCHED: four fields spelled `int' that upstream spells `off_t' and `time_t'.
 * This is a DISK RECORD, and its other end is not another program in this port
 * -- it is a 1985 filesystem image, which SIMH can be pointed at.  V8's VAX
 * compiler defined NOLONG, so `long' there was 32 bits and all four of these
 * are four bytes; <sys/types.h> has that argument in full and narrows daddr_t
 * globally for the same reason.  time_t and off_t cannot be narrowed globally,
 * because they cross the shim seam to macOS where they are 64 bits -- so they
 * are narrowed HERE, per field, in the header that describes the record.
 *
 * The size is not a preference, it is arithmetic upstream already committed to:
 * <sys/param.h> hardcodes `INOPB(dev) (BITFS(dev)? 64: 16)', so a 1024-byte
 * block holds sixteen inodes and sizeof(struct dinode) MUST be 64.  Unpatched
 * it was 80, and itod()/itoo() would have placed inode 17 at offset 1280 of a
 * 1024-byte block -- past the end, into the next inode block, silently.
 *
 * tests/mkfs measures all four fields from the V8 side, because a
 * _Static_assert would only ever see one compiler and this struct is read by
 * two (v8cc here, clang in the shim) plus, in principle, a VAX.
 */
struct dinode
{
	unsigned short di_mode;	/* mode and type of file */
	short	di_nlink;	/* number of links to file */
	short	di_uid;		/* owner's user id */
	short	di_gid;		/* owner's group id */
	int	di_size;	/* number of bytes in file  (upstream off_t) */
	char	di_addr[40];	/* disk block addresses */
	int	di_atime;	/* time last accessed	    (upstream time_t) */
	int	di_mtime;	/* time last modified	    (upstream time_t) */
	int	di_ctime;	/* time created		    (upstream time_t) */
};
/*
 * the 40 address bytes:
 *	39 used; 13 addresses
 *	of 3 bytes each.
 */
