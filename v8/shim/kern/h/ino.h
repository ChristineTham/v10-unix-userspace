/*
 * The on-disk inode, for the kernel side -- forwarded, not imported.
 *
 * THIS FILE IS OURS, and shim/kern/h/filsys.h beside it carries the full
 * reasoning: v8/usr/sys/h/ino.h and v8/usr/include/sys/ino.h are one git
 * blob, this port patched the userland copy because struct dinode is a DISK
 * RECORD, and a record written to a disk must have exactly one declaration
 * in the tree.
 *
 * The patch here is four fields: di_size, di_atime, di_mtime and di_ctime,
 * which upstream spells off_t and time_t and which §8a step 4a narrowed to
 * v8_i32.  sizeof(struct dinode) is 64 with it and 80 without, and INOPB(0)
 * is 16 -- so a pristine kernel copy would put 80-byte inodes in
 * 1024/16-byte slots and every inode past the first would be read from the
 * wrong offset.
 *
 * Reached through KERNFLAGS' -Ishim/kern/dev; see filsys.h.
 */

#ifndef V8KERN_INO_H
#define V8KERN_INO_H

#include "../../../src/include/sys/ino.h"

#endif
