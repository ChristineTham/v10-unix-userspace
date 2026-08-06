/*
 * V7 directory records, with the SAME deliberate change src/include/dir.h
 * makes: DIRSIZ is 254 rather than 14, so a record is 256 bytes rather than 16.
 * The reasoning is there and is not repeated here.
 *
 * WHY THIS FILE EXISTS SEPARATELY, which is the whole point of the patch.
 * V8 ships TWO directory headers and they are not aliases:
 *
 *	<dir.h>      struct dir     -- readdir(3)'s view
 *	<sys/dir.h>  struct direct  -- the on-disk record, read(2) directly
 *
 * Only the first was patched.  So libc's readdir agreed with the shim (which
 * writes 256-byte records, and whose V8_DIRSIZ says so), while the SEVEN
 * commands that read directories raw -- rm, rmdir, mv, w, make and the rest --
 * included this one and parsed the same bytes as 16-byte records.
 *
 * IT LOOKED FINE, which is why it lasted.  A 256-byte record read as sixteen
 * 16-byte ones yields one real entry followed by fifteen whose d_ino is 0, and
 * `d_ino == 0 means unused' is exactly V7's own rule for a deleted entry -- so
 * every one of those commands skipped the padding and got the right answer,
 * sixteen times slower, with nothing to say anything was wrong.
 *
 * What it does NOT survive is a program that trusts st_size.  ps(1)'s getdir()
 * sizes its array as st_size/sizeof(struct direct) and then requires
 * read(fd, dp, st_size) to return exactly st_size -- so it read the first 288
 * bytes of a 2304-byte directory, found "." and ".." and one free slot, and
 * called that /dev.  Found while measuring what ps would need; see
 * shim/v8sys/dir.c for the other half of the fix, which is that fstat on an
 * open directory now reports the size of what read(2) will actually produce.
 */
#ifndef	DIRSIZ
#define	DIRSIZ	254
#endif
struct	direct
{
	ino_t	d_ino;
	char	d_name[DIRSIZ];
};
