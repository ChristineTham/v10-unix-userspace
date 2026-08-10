/*
 * The image block driver -- a V8 block device whose platter is a host file.
 *
 * WHY IT IS HERE NOW, HAVING DELIBERATELY NOT BEEN BEFORE.  It lived in
 * tests/streams/fsprobe.c under CLAUDE.md's unconsumed-component rule: nothing
 * in this port consumed a block device, so a driver in the shim would have
 * been "a component with no caller", inventing a difference the kernel does
 * not have.  §8a step 5e supplies the caller.  The v8fs server is a host
 * binary that links libv8kern and serves a disk image over 9P, and it needs
 * exactly this driver -- so the rule that kept it out is the rule that now
 * moves it in.
 *
 * AND THE PROBE USES THE SAME ONE, which is the part worth having.  fsprobe
 * has 236 cases that drive namei/iget/bmap/readi/writei down to a driver; if
 * the server had a driver of its own, none of them would say anything about
 * it.  Sharing makes the probe's coverage the server's coverage, and means a
 * bug in the units below can only be in one place.
 *
 * Include shim/kern/h/param.h before this: struct bdevsw comes from
 * shim/kern/h/conf.h and dev_t from param.h.
 */

#ifndef V8KERN_IMGDEV_H
#define V8KERN_IMGDEV_H

struct bdevsw;

/*
 * Fill in the driver's row WITHOUT registering it.  This exists for the
 * probe's rejection cases, which need a row that is right in every respect but
 * one -- and building that from the real pointers is what makes them cases
 * about v8k_bdconf rather than about three local stubs.
 */
void	v8k_imgrow(struct bdevsw *bd);

/*
 * Attach an already-open image and register the driver.  Returns the MAJOR
 * number v8k_bdconf assigned, or -1.
 *
 * The caller opens the file, so it chooses O_RDONLY or O_RDWR and reports its
 * own error with its own name in it.  There is one image at a time: the fd is
 * a single static, because two would need two minor numbers and a minor number
 * here already means something else -- bit 6 of the device selects the
 * filesystem block size (BITFS), so minors are not free to enumerate disks.
 */
int	v8k_imgattach(int fd);
void	v8k_imgdetach(void);

/*
 * The device number, PACKED HERE rather than by the caller, and that is a
 * structural fix rather than a convenience.  shim/kern/h/param.h defines
 * makedev/major/minor and warns they must not be replaced by the host's, which
 * shift a 32-bit dev_t differently -- and any file that includes
 * <sys/socket.h> gets <sys/types.h> with it and is silently redefined, with no
 * -Wmacro-redefined because it happens inside a system header.  V8 shifts the
 * major by 8, Darwin by 24, and dev_t here is a u_short, so makedev in such a
 * file is ZERO for every major.  This file includes no host header, so it
 * still has V8's macros; asking it removes the question.
 */
dev_t	v8k_imgdev(void);

/*
 * Transfers the driver actually served.  The probe asserts on these -- that a
 * second read of the same block does NOT reach the disk, which is the only
 * externally visible evidence the buffer cache is a cache.
 */
long	v8k_imgreads(void);
long	v8k_imgwrites(void);

#endif /* V8KERN_IMGDEV_H */
