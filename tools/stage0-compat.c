/*
 * Stage-0 scaffolding only.
 *
 * V8 programs reach directly into V8 libc's internals.  While we are building
 * them against the *host* libc (stage 0), those internals do not exist, so we
 * supply them here rather than patching authentic source.
 *
 * Every symbol in this file is a placeholder that disappears in Phase 2b, when
 * the real V8 libc is ported and provides the genuine article.  If you find
 * yourself adding anything here that is not a straight V8-libc internal, it
 * probably belongs in shim/ (libv8sys) instead.
 */

#include <stdio.h>

/*
 * libc/stdio/data.c:   unsigned char _sobuf[BUFSIZ];
 *
 * stdout's statically allocated buffer.  cpp(1) closes stdout when given an
 * output file argument and hands this buffer to setbuf() for the replacement
 * stream -- avoiding a second buffer allocation on a machine with 256K of RAM.
 *
 * Sized by the *host* BUFSIZ, since that is what the stage-0 cpp.c saw when it
 * declared `extern char _sobuf[BUFSIZ]` and what setbuf() will assume.
 * (V8 declares it unsigned char; cpp declares it char. The original disagreed
 * with itself too -- harmless, K&R did not check.)
 */
char _sobuf[BUFSIZ];
