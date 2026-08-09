/*
 * <ndir.h> for the host, so that tests/sh can compile src/cmd/sh/spname.c
 * EXACTLY AS IT IS and run it under AddressSanitizer.
 *
 * Why a header rather than a sed: spname.c is authentic V8 source and the
 * thing under test is a buffer size in it, so a probe that rewrites the file
 * on its way in is testing a different file.  spname.c says `#include
 * <ndir.h>' and uses three names -- DIR, struct direct, and V8's TWO-argument
 * opendir -- so putting this directory first on the include path supplies all
 * three and the source is untouched.
 *
 * This is a host-side probe on purpose.  A V8-world one cannot reach a scratch
 * directory: spname() walks an absolute path from `/', the shim's jail is a
 * PREFIX table (mounts[] in shim/v8sys/vfs.c) rather than a real chroot, and
 * the jail's `/' has no component matching a host temp path -- so spname dies
 * on the first component and returns 0 whatever the buffers do.  Measured, not
 * assumed: with V8ROOT set to a scratch tree, `ls /' lists it and `ls /sp'
 * says not found.
 */

#ifndef V8TEST_HOST_NDIR_H
#define V8TEST_HOST_NDIR_H

#include <dirent.h>

/* V8's struct direct is the host's struct dirent for our purposes: spname
 * reads only d_name from it. */
#define direct dirent

/* V8's opendir takes a second argument (the "follow" flag); the host's does
 * not.  spname passes 0. */
#define opendir(p, f) opendir(p)

#ifndef NULL
#define NULL 0
#endif

#endif /* V8TEST_HOST_NDIR_H */
