/*
 * hostok.h -- undo shim/kern/h/param.h's name redirections, so that a file
 * which needs BOTH the kernel headers and the host's can have them.
 *
 * INCLUDE IT AFTER param.h AND BEFORE ANY HOST HEADER.  That ordering is not
 * a style preference; param.h:141-158 records why both halves are forced, and
 * the failure it prevents is a redefinition error rather than a silent loss:
 *
 *	#define printf v8k_printf	(param.h)
 *	#include <stdio.h>		-- rewrites stdio's own declaration into
 *					   a conflicting prototype for v8k_printf
 *
 * WHY A HEADER RATHER THAN A LIST OF #undefs IN EACH FILE.  It was a list, in
 * three files -- tests/streams/{probe,sioprobe,ttyprobe}.c -- and it worked
 * while there were four redirects.  §8a step 5 took the list to THIRTEEN, and
 * a thirteen-line list copied into every consumer is exactly the shape
 * CLAUDE.md records going stale twice already: tests/kmemu's ALLOWED, and the
 * hand-written export list that libv8kern's leak sweep replaced with a
 * subtraction.  A copied list decays independently in each copy.
 *
 * IT FAILED LOUDLY RATHER THAN QUIETLY, WHICH IS WHY IT IS FIXED NOW.  Adding
 * `#define sleep v8k_sleep' broke sioprobe.c's build the moment it landed --
 * <unistd.h>:493 declares sleep(unsigned) and the macro rewrote it.  The three
 * probes could each have grown nine more #undefs; this is one place instead,
 * and the next redirect updates it once.
 *
 * The list below must stay in step with param.h's redirect block.  There is no
 * way to derive one from the other in cpp, so tests/streams asserts the two
 * have the same length -- which is a weaker check than deriving it and a much
 * stronger one than a comment saying "keep these in sync".
 */

#ifndef V8KERN_HOSTOK_H
#define V8KERN_HOSTOK_H

/* the four that predate §8a step 5 -- param.h:266,278,357,373 */
#undef printf
#undef bcopy
#undef psignal
#undef longjmp

/* the nine link collisions §8a step 5 measured with nm -g; param.h has the
 * table, including which three are the silent variable-against-function kind */
#undef free
#undef ialloc
#undef min
#undef max
#undef sleep
#undef access
#undef time
#undef timezone
#undef mount

#endif /* V8KERN_HOSTOK_H */
