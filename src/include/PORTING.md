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
| `fstab.h` | `FSNMLG` 32 → 128, and `FSTABFMT` with it | host mount points exceed 31 characters |
| `mtab.h` | two literal `32`s → `128` | must agree with `FSNMLG`; nothing includes it |

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

## `fstab.h`, and why a path is not a name

The `DIRSIZ` change above is about *names*. This one looks like the same change
and is not, and the difference is the whole reason the bug survived being
documented.

`shim/libkmemu/mtab.c` used to truncate a mount point to the field and say so
in a comment: "same loss as dir.c's 14-character names and utmp's 8 — the field
is the field, and a V8 df could not have shown more either." **That premise is
false.** A truncated name is a wrong name and still just a name. A truncated
*path stops resolving*, and `df`'s `dfree()` branches on it:

```c
if (stat(file, &stbuf) == 0 && (stbuf.st_mode&S_IFMT) == S_IFDIR)
        ... look the mount point up by device ...
else if (strncmp("/dev/", file, sizeof "/dev/" - 1) != 0)
        strcpy(&specbuf[5], file), file = specbuf;   /* it must be a device */
```

So `/Library/Developer/CoreSimulator/Volumes/iOS_23F77` truncated to
`/Library/Developer/CoreSimulato`, failed to `stat`, and fell into the arm that
assumes the string names a device. `mpath()` then found no mtab entry with that
"device", so the `dir` column printed empty, and `file + sizeof "/dev"` printed
the path's first nine characters — giving a row reading `/Library/` in the
`dev` column. **A V8 machine could not reach that branch with a mount point,
because mount points were short.**

128 covers every mount point this host has — the longest is 52 — and makes the
mtab record exactly 256 bytes, the same shape `DIRSIZ` 254 chose.

**Widening moves the boundary; it does not remove it.** So a mount point that
still will not fit is now *reported on stderr and left out*, the way `/proc`
reports a process-table overflow rather than quietly listing fewer processes.
An entry whose path cannot be stored cannot be described truthfully, and a
garbled row is worse than an absent one that says it is absent. Both
manufactured files apply the same rule, and they have to: `df`'s `devlen()`
merges any fstab entry whose device is not already in mtab, so dropping a mount
from one file and not the other hands it straight back through the merge.

One consumer had to move with it, and finding it is the whole reason to count
the spellings: `src/libc/stdio/fstab.c`'s `fstabscan()` read a line into a flat
`char buf[256]`, which was ample for two 32-byte fields and is *smaller than a
line the widened header now permits* (265 bytes). `fgets` would have truncated
it and `fs_string` would then have failed to find its `:` and dropped the entry.
No line on this host comes close; the point is that the parser must honour what
the struct promises. It is `2 * FSNMLG + 16` now — derived, so it cannot drift.

Four places spell this number — `FSNMLG` here, `FSTABFMT` in the same header,
`<mtab.h>`'s two literals, and `shim/libkmemu/mtab.c`'s own copy. `FSTABFMT`
carries the digits by hand because V8's cpp is from 1985 and has no `#`
stringification. `<mtab.h>` is patched even though **nothing in this port
includes it** — `df` declares its own `struct mtab` from `FSNMLG` — precisely
because leaving it at 32 would cost nothing today and would tell the next
reader the record is 64 bytes when it is 256. That is the `DIRSIZ` failure
exactly: three headers, two patched, the unpatched one still believed.

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
