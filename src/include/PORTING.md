# Porting notes: the include tree

These are V8's own headers, imported and minimally patched. Each one carries
its reasoning in a comment at the top of the file, so this note is an index and
the rules they share rather than a second copy of the arguments.

`rootfs/usr/include` is a *generated view*: the Makefile copies all of
`third_party/.../usr/include`, then overlays `src/include` on top. So a header
that needs no change is not here at all, and the files below are exactly the
deviations.

## What is patched

| File | Change | Forced by |
|---|---|---|
| `dir.h` | `DIRSIZ` 14 → 254 | host filenames exceed 14 characters |
| `sys/dir.h` | `DIRSIZ` 14 → 254 | same, and it is a *different struct* |
| `sys/param.h` | `DIRSIZ` 14 → 254 | same, and it is the one that decides |
| `setjmp.h` | `int[10]` → `long[24]` | AAPCS64 has 21 doublewords to save |
| `sys/proc.h` | four pid fields `short` → `int` | macOS pids run to 99998 |

## The two rules

**A number V8 spells more than once must be changed everywhere at once, and
`#ifndef` decides which spelling wins.** `DIRSIZ` lives in three headers, all
guarded, so the first one a program includes is the one that takes effect —
and `w.c` and `ps.h` both reach `<sys/param.h>` first. Patching two of the
three changed nothing for exactly the programs that read directories raw, while
looking like it had. Five places have to agree; the other two are
`shim/v8sys/v8sys.h`'s `V8_DIRSIZ` and `src/libc/gen/readdir.c`'s `ODIRSIZ`.

**A struct here has two ends, because two compilers read it.** v8cc reads this
tree; the shim is clang-compiled and re-spells any struct it has to produce
(`struct v8utmp` in `shim/libkmemu/utmp.c`, `struct v8proc` in
`shim/libkmemu/procfs.c`). A `_Static_assert` in the shim catches that file
drifting from itself and can say nothing about whether v8cc agrees, because it
only ever sees one compiler. `tests/kmemu` measures the same offsets from the
V8 side for that half. Change a struct here and both ends need updating.

## `sys/proc.h`, in more detail

The pid widening was found by mutation-testing something else, and it is worth
recording how it hid. `p_pid` is `short` upstream because V7 wrapped `mpid` at
30000, so 16 bits was the entire range. macOS runs pids to 99998 — 99698
observed live on the development machine — and the truncation is *signed*, so
pid 44145 came back as −21391. `ps` would have printed a negative pid, and
`ps <pid>` would have failed to match.

It hid because **a freshly booted host has low pids**. Every check passed for as
long as the host's counter stayed under 32767 and started failing when it
didn't; the run that exposed it was an unrelated mutation whose extra failures
had no business appearing. `tests/kmemu` therefore asserts the *field width*
alongside the runtime value — the width is true at every pid, the comparison
only at high ones.

`p_uid` is widened with the three pid fields even though the highest uid
measured on this host is 501, because it is free: a 16-bit `p_uid` followed by a
32-bit `p_pgrp` takes four bytes of alignment padding, which is exactly what the
wider field costs. Zero-cost is a different argument from forced-by-the-target,
and it is the one being made.

`struct xproc` in the same header still says `short xp_pid`. It is the kernel's
parallel structure for handing exit status to a parent, nothing in this port
reads it, and widening it would be a change with no reader to justify it.

## Not changed, and deliberately

`sys/types.h` is verbatim upstream. It says `typedef long size_t` and
`typedef long time_t`, which happen to be right under LP64 — checked rather than
assumed, since the natural guess is that a 1985 header would need patching here.
On the VAX `int` and `long` were both 32 bits so either spelling worked, and V8
happened to pick the one that survives.
