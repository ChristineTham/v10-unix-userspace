/*
 * libkmemu -- what the kernel knows, for the V8 commands that only report it.
 *
 * See synth.c for what this library is allowed to do and what it is not.
 */
#ifndef KMEMU_H
#define KMEMU_H

/*
 * The entry point the shim calls, through a WEAK symbol, from open(2).  Handed
 * the path as the V8 world spelled it, before rootpath() resolves it, plus the
 * rootfs it would resolve into.
 *
 * Returns 1 if it manufactured a file, 0 if the path is not one it knows.  A
 * program that does not link libkmemu leaves the weak symbol null and never
 * reaches here, which is what keeps tests/freestanding honest.
 */
int kmemu_synth(const char *v8path, const char *root);

/* One of these per synthetic file.  Writes `hostpath'; 0 on success, -1 not. */
int kmemu_utmp(const char *hostpath);
int kmemu_mtab(const char *hostpath);
int kmemu_fstab(const char *hostpath);
int kmemu_unix(const char *hostpath);
int kmemu_kmem(const char *hostpath);

/*
 * What df(1) would have read out of a superblock.  Counts are in 1024-byte
 * blocks, which is BSIZE(dev) for a device number with bit 64 clear -- the
 * only kind this port hands out.  See mtab.c for why df gets this through a
 * call where who(1) got a manufactured file.
 */
struct kmemu_fs {
	long	blocks;		/* total, 1K units */
	long	bfree;		/* available to a user */
	long	files;		/* total inodes */
	long	ffree;		/* free inodes */
};
int kmemu_fsstat(const char *path, struct kmemu_fs *out);

/* Shared plumbing, raw-syscall only -- see the boundary note in synth.c. */
int  kmemu_replace(const char *hostpath, const char *buf, long n);
void kmemu_field(char *dst, long dlen, const char *src, long slen);

#endif
