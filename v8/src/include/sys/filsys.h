/*
 * Structure of the super-block
 *
 * PATCHED: two fields, for the reason <sys/ino.h> and <sys/types.h> give in
 * full -- this is a disk record, V8's VAX compiler defined NOLONG, and every
 * `long' in it is four bytes.  daddr_t is narrowed globally in <sys/types.h>,
 * which covers s_fsize, s_tfree and S_free[]; the two below have to be spelled
 * here because their types must stay 64 bits everywhere else in the port.
 *
 * S_bfree is patched even though nothing in this port can make a BITFS
 * filesystem -- mkfs is free-list only and mkbitfs is not ported.  It is
 * patched because it is what sizeof(struct filsys) is mostly made of, so
 * leaving it would put the right answer for the arm we use inside a structure
 * whose total is wrong, and because a header that is half-corrected is the
 * DIRSIZ failure again: three spellings, two fixed, the third still believed.
 *
 *	free-list arm	 964 bytes used of the 1024-byte block
 *	bitmap arm	4096 exactly, which is the BITFS block size
 */
struct	filsys
{
	v8_u16	s_isize;		/* size in blocks of i-list */
	daddr_t	s_fsize;   		/* size in blocks of entire volume */
	v8_i16 	s_ninode;  		/* number of i-nodes in s_inode */
	ino_t  	s_inode[NICINOD];	/* free i-node list */
	char   	s_flock;   		/* lock during free list manipulation */
	char   	s_ilock;   		/* lock during i-list manipulation */
	char   	s_fmod;    		/* super block modified flag */
	char   	s_ronly;   		/* mounted read-only flag */
	v8_i32 	s_time;    		/* last super block update (upstream time_t) */
	daddr_t	s_tfree;   		/* total free blocks*/
	ino_t  	s_tinode;  		/* total free inodes */
	v8_i16	s_dinfo[2];		/* interleave stuff */
#define	s_m	s_dinfo[0]
#define	s_n	s_dinfo[1]
	char   	s_fsmnt[14];		/* ordinary file mounted on */
	ino_t	s_lasti;		/* start place for circular search */
	ino_t	s_nbehind;		/* est # free inodes before s_lasti */
	union {
		struct {
			v8_i16 	S_nfree;/* number of addresses in s_free */
			daddr_t	S_free[NICFREE];/* free block list */
		} R;
		struct {
			char	S_valid;/* 1 on disk means bit map valid */
#define BITMAP	961
			v8_i32	S_bfree[BITMAP];/* bit map (upstream long) */
		} B;
	} U;
};
#define s_nfree U.R.S_nfree
#define s_free  U.R.S_free
#define s_valid U.B.S_valid
#define s_bfree U.B.S_bfree

#ifdef KERNEL
struct	filsys *getfs();
#endif
