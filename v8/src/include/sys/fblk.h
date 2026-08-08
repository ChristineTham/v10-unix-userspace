/*
 * PORT: imported solely to pin df_nfree, and finding that it had NEVER been
 * imported is the point.
 *
 * rootfs/usr/include is built by copying third_party's pristine headers and
 * then overlaying ours (Makefile:1960 then :1966).  A header nobody imported
 * therefore stays UPSTREAM'S, silently -- so every program that reads a free
 * list has been compiling `struct fblk' against 1985's own declaration, while
 * the two structs beside it in the same image, dinode and filsys, are patched
 * copies in src/include.  It happened to be right, because `int' is 32 bits
 * here as it was on the VAX and daddr_t comes from our patched <sys/types.h>,
 * so sizeof came out 716.  Right by coincidence twice over.
 *
 * df_nfree is now v8_i32 for the reason every other record field is: the width
 * belongs to the free-list block, not to the compiler.  See <sys/types.h>.
 */
struct fblk
{
	v8_i32 	df_nfree;
	daddr_t	df_free[NICFREE];
};
