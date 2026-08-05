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

/* Shared plumbing, raw-syscall only -- see the boundary note in synth.c. */
int  kmemu_replace(const char *hostpath, const char *buf, long n);
void kmemu_field(char *dst, long dlen, const char *src, long slen);

#endif
