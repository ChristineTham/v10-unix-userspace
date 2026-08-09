/*
 * tty.h -- the header `config' would have written, and the one number in the
 * stream machinery this port has to decide for itself.
 *
 * WHY IT IS OURS AND NOT UPSTREAM'S.  dev/ttyld.c:6 is `#include "tty.h"',
 * and there is no such file to import: h/tty.h upstream is ZERO BYTES, a make
 * timestamp node (conf/makefile:61-62 touches it to mean "sgtty.h and ioctl.h
 * are current"), and it is not the file ttyld.c gets -- a quoted include tries
 * the includer's own directory first, and dev/ has no tty.h at all.  What
 * ttyld.c is really asking for is the per-configuration header 4BSD's
 * `config' generates from a machine description: conf/files:98 marks the file
 * `optional tty pseudo-device', so `pseudo-device tty N' in that description
 * becomes `#define NTTY N'.  The description is not shipped, and conf/config
 * is a VAX a.out binary rather than source, so it cannot be regenerated.
 * There is no `#define NTTY' anywhere in third_party/.
 *
 * So this file sits in shim/kern/dev/ and is reached through -Ishim/kern/dev,
 * exactly the way "../h/param.h" from src/sys/dev/ falls through to the
 * stand-in beside it.  Machine facts in shim/kern/, never in src/sys/ -- and
 * ttyld.c is imported byte-identical because of it.
 *
 * WHY 128, WHICH IS DERIVED AND NOT PICKED.  NTTY bounds `struct ttyld
 * tty[NTTY]', the pool ttyopen() allocates a slot from (ttyld.c:41-63).  A
 * slot is one line discipline ATTACHED TO A STREAM, not one terminal: ttyopen
 * returns 1 immediately when qp->ptr is already set, so it is one slot per
 * stream, and a process cannot have more streams than NSTREAM.  NSTREAM is
 * 128 (src/sys/research/sparam.h:6) and is authentic, so 128 makes the
 * discipline exactly never the scarcer resource -- the stream table is the
 * limit, which is where a limit belongs.
 *
 * On a VAX NTTY counted configured terminal lines and was much smaller than
 * NSTREAM.  It cannot mean that here: the shim is per-binary, so tty[] is
 * per-process rather than per-machine, and "how many terminals does this
 * machine have" is not a question one process can answer.  "How many streams
 * can this process hold" is, and it is the tight bound.
 *
 * The 64 an earlier draft of PLAN.md gave had no source -- proto-dev has 8
 * hardware ttys and 64 spipe pt nodes, which makes it a plausible guess and
 * nothing more.  Sizing is not the argument either way: struct ttyld is 14
 * bytes, so the whole array is under 2 KB against libv8kern's existing 94 KB
 * of zero-initialised storage.
 */
#ifndef	NTTY
#define	NTTY	128		/* = NSTREAM, sparam.h:6 -- see above */
#endif
