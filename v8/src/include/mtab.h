/*
 * PORT: 1024, not V7's 32, matching FSNMLG in <fstab.h> -- see the account
 * there and in src/include/PORTING.md.  Upstream spells these as two literal
 * 32s rather than as FSNMLG, and this header includes nothing, so they are
 * spelled by hand here too and have to be kept in step by reading.
 *
 * NOTHING IN THIS PORT INCLUDES THIS HEADER, which is exactly why it is
 * patched.  df(1) declares its own `struct mtab' from <fstab.h>'s FSNMLG and
 * never looks here, so leaving these at 32 would cost nothing today and would
 * tell the next reader the record is 64 bytes when it is 2048.  That is the
 * DIRSIZ failure -- three headers, two patched, the unpatched one still
 * believed.
 */
struct mtab {
	char	m_path[1024];		/* mounted on pathname */
	char	m_dname[1024];		/* block device pathname */
};
