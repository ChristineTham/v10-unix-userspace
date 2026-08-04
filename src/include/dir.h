/*
 * V7 directory records, with ONE deliberate change: DIRSIZ is 254 rather than
 * 14, so a record is 256 bytes rather than 16.
 *
 * Why not the original 14:
 *
 * V7 filenames were 14 characters and V8 inherited that.  The shim presents
 * host directories as V7 records, and with a 14-byte name field every longer
 * component was silently truncated -- so a V8 program could not name most of a
 * real macOS filesystem.  That is not an edge case: pwd(1) failed in any
 * directory with a long component anywhere above it, which includes every
 * mktemp directory and most of a user's home.  getwd(3) assembled
 *
 *	/private/var/folders/3t/lwjdn7s93v56gr/T/tmp.mRsDThXz5R
 *
 * out of truncated pieces and then could not chdir back to it.  Left at 14, the
 * port works only where every ancestor directory happens to be short.
 *
 * Why it is safe to change:
 *
 * The tree names this size symbolically.  Of the commands that read directories
 * raw, all but one use `sizeof (struct direct)` or DIRSIZ rather than a literal
 * 16 -- the exception is fcopy(1), which reads a raw device and belongs to the
 * filesystem tools that are out of scope anyway.  Everything else is recompiled
 * against this header and picks the new size up on its own.
 *
 * 254 makes the record exactly 256 bytes with the 2-byte inode, which stays a
 * whole divisor of the DIRBLKSIZ (512) buffer readdir(3) reads into.
 *
 * This is the same kind of decision as LP64 and writable string literals: a
 * 1985 assumption a modern host cannot honour, changed once in the place that
 * defines it rather than worked around per program.  The shim's V8_DIRSIZ
 * (shim/v8sys/v8sys.h) and readdir.c's ODIRSIZ must agree with it.
 */
#ifndef	DIRSIZ
#define	DIRSIZ	254
#endif
struct	dir
{
	ino_t	d_ino;
	char	d_name[DIRSIZ];
};
