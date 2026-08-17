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
| `sys/param.h` | `DIRSIZ` 14 → 254, and an `#ifndef` around it | same, and it is the one that decides; the guard is for mkfs |
| `setjmp.h` | `int[10]` → `long[24]` | AAPCS64 has 21 doublewords to save |
| `sys/proc.h` | four pid fields `short` → `int` | macOS pids run to 99998 |
| `fstab.h` | `FSNMLG` 32 → 1024, and `FSTABFMT` with it | host mount points exceed 31 characters |
| `mtab.h` | two literal `32`s → `1024` | must agree with `FSNMLG`; nothing includes it |
| `sys/types.h` | `daddr_t` `long` → `int` | a disk block address is four bytes on a V8 volume |
| `sys/ino.h` | `di_size` and three times `long` → `int` | same, per field |
| `sys/filsys.h` | `s_time` and `S_bfree[]` likewise | same |

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

## The on-disk formats, and the third kind of "other end"

Added with `mkfs`, PLAN.md §8a step 4. Every other struct in this tree has two
ends and **both of them are ours** — v8cc reads the header, clang re-spells it
in the shim, and a widening is safe as long as the two agree. `struct filsys`,
`struct dinode` and `struct fblk` have an end that is a **1985 disk**, which
cannot be asked to agree with anything.

They were all wrong, and nothing had noticed because nothing had written one:

| | VAX | here, before | after |
|---|---|---|---|
| `sizeof(struct dinode)` | 64 | **80** | 64 |
| `sizeof(struct filsys)` | 4096 | **7960** | 4096 |
| `sizeof(struct fblk)` | 716 | **1432** | 716 |
| `NINDIR(0)` | 256 | **128** | 256 |

**V8's own compiler settles the width in one line.**
`third_party/.../cmd/ccom/vax/macdefs.h:19` reads `# define NOLONG`, commented
"map longs to ints". So on the VAX `long` and `int` were the same 32-bit type,
and every `long` field in a disk record is four bytes.
`compiler/ccom-arm64/macdefs.h` deliberately does *not* define it — its own
comment says leaving `NOLONG` undefined is what makes LP64 expressible at all —
so `SZLONG` is 64 here and `daddr_t`, `time_t` and `off_t` all silently doubled.

**The tree already contradicted itself, which is why this is not a preference.**
`<sys/param.h>` hardcodes `NMASK(0) 0377` and `NSHIFT(0) 8`, both of which say
an indirect block holds 256 addresses, while `NINDIR(dev)` —
`BSIZE(dev)/sizeof(daddr_t)`, one line below them in the same file — computed
128. And `INOPB(0)` is hardcoded 16 while `sizeof(struct dinode)` had become 80,
so `itod()`/`itoo()` would have put inode 17 at byte 1280 of a 1024-byte block.
The constants are upstream's; it is the types that drifted. `tests/mkfs` asserts
those two agreements against each other rather than against transcribed numbers,
so the next drift shows up without anyone remembering a value.

**Why `daddr_t` globally and the other two per field.** A `daddr_t` never
crosses the shim seam: it is a disk block number, it appears in exactly two
programs outside these headers (`df`, which reads a real superblock and needs it
narrow to do so, and `mkfs`), and nothing hands one to macOS. `time_t` and
`off_t` are handed to macOS constantly and are 64 bits there, so narrowing them
globally would break every syscall that carries one. They are narrowed in the
two headers that describe disk records and nowhere else.

**And a global type change reached further than the headers.**
`src/libc/gen/ltol3.c` packs an inode's `i_addr[]` into three-byte disk
addresses, and its arm64 arm strode **eight** bytes because `daddr_t` used to be
that wide. Narrowing the type without narrowing the stride decimated every block
list and read 35 bytes past the end of a stack `struct inode`. Both `ltol3` and
`l3tol` are back to the VAX stride, and `tests/mkfs` round-trips them —
necessary because `l3tol` has no caller in this port at all.

## `sys/param.h`'s `#ifndef`, and a comment that described a guard that was not there

The note in `param.h` used to say "all three use `#ifndef DIRSIZ`, so whichever
header a program reaches first wins". Upstream guards `dir.h` and `sys/dir.h`
and leaves `param.h` **bare** — so on a real V8 this file always won by
redefinition, and the sentence was never true of the file it was in.

It cost nothing while all three spellings said 254, and it was found the moment
something wanted a different one. `mkfs` is compiled `-DDIRSIZ=14`, because what
it writes is a disk image rather than a host directory, and cpp answered
`param.h: 86: DIRSIZ redefined` and gave it 254 regardless. A silently
256-byte-per-record filesystem is exactly the wrong thing to ship. The guard is
now there, it changes nothing for any program that does not define `DIRSIZ`
itself, and `src/cmd/mkfs.PORTING.md` has why the override is right.

This is the same disease as the original `DIRSIZ` bug one level up: three
headers, a belief about how they interact, and the belief checked in only two
of them.

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

**1024 because that is the host's own width for the field**, not because it
looks big enough: `struct statfs`'s `f_mntonname` is `char[MAXPATHLEN]`, so 1024
is exactly the set of mount points the host can report, and any smaller choice
is a boundary that has to be defended against the next machine. 128 was tried
first — the longest mount point on the development Mac is 52, and 128 makes the
record a tidy 256 bytes. CI refuted it within the hour: a GitHub runner mounts a
Siri asset bundle at 140 characters. Picking the host's own number is what stops
this being a guess.

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
line the widened header now permits* (2064 bytes). `fgets` would have truncated
it and `fs_string` would then have failed to find its `:` and dropped the entry.
No line on this host comes close; the point is that the parser must honour what
the struct promises. It is `2 * FSNMLG + 16` now — derived, so it cannot drift.

Four places spell this number — `FSNMLG` here, `FSTABFMT` in the same header,
`<mtab.h>`'s two literals, and `shim/libkmemu/mtab.c`'s own copy. `FSTABFMT`
carries the digits by hand because V8's cpp is from 1985 and has no `#`
stringification. `<mtab.h>` is patched even though **nothing in this port
includes it** — `df` declares its own `struct mtab` from `FSNMLG` — precisely
because leaving it at 32 would cost nothing today and would tell the next
reader the record is 64 bytes when it is 2048. That is the `DIRSIZ` failure
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

`sys/types.h` is patched in exactly one line — `daddr_t`, above — and the rest
of it is upstream. It says `typedef long size_t` and `typedef long time_t`,
which happen to be right under LP64: both are what macOS uses, so both cross the
shim seam unchanged.

That was recorded here as "checked rather than assumed" when the file was still
untouched, and the check was sound as far as it went. What it missed is that
being right at the *syscall* seam and being right at the *disk* seam are two
different questions, and until §8a step 4 there was no disk to ask the second
of. `long` is 64 bits here and was 32 on the VAX, so **every one of these
typedefs is wrong for a disk record and right for everything else** — which is
why the fix is one global narrowing plus four fields, and not a rewrite of this
file.

## The widths are now spelled out, and LP64 turns out to be forced

The section above pins the record structs by *choosing types whose width
happens to be right* — `int di_size` is four bytes because this port is LP64,
and the header never says so. That worked and was fragile in a specific way:
the declaration and the reason lived in different places, so a change to the
data model would have moved every one of these fields silently.

`<sys/types.h>` now declares `v8_i16`, `v8_u16`, `v8_i32`, `v8_u32`, and every
field of every on-disk and on-tape record is spelled with one of them. `char`
stays `char` — a char array in a record is bytes, not a narrow integer.
Covered: `dinode`, `filsys` (including both arms of its union), `fblk`, `spcl`.
`direct`, `dir` and `idates` already used `ino_t`/`daddr_t`/`char`, which are
explicit already.

### Asking whether the model could move, and measuring the answer

The obvious question this raises is whether `int` should simply be 64 bits, so
that none of this is needed and V8's own `sizeof(int) == sizeof(char *)`
assumption — the source of this port's *dominant* bug class — comes back true.
It was tried, not argued: `SZINT`/`ALINT` flipped to 64 in
`compiler/ccom-arm64/macdefs.h` and the tree built.

**It cannot be done, and the reason is a counting argument.** V8's ccom has
exactly four integer types — `CHAR SHORT INT LONG` at `common/manifest.h`
:224-227, with no `long long` anywhere in the front end — and this port must
express exactly four widths:

| width | who needs it |
|---|---|
| 8 | `char`, `di_addr[40]` |
| 16 | `ino_t`, `di_mode`, `d_ino` — V8's own short fields |
| **32** | `daddr_t`, the on-disk times, `c_magic`, `c_checksum` |
| 64 | a pointer; the host's `time_t` and `off_t` |

Four types, four widths, so `char/short/int/long` must be `8/16/32/64` — and
that assignment **is** LP64. Making `int` 64 bits leaves nothing that can spell
32, and every field in the table above would have to become `char[4]` with
hand-packing, which means editing the authentic programs that read them. The
build fails first somewhere much smaller — `local.c`'s `sz_incode()` tests
`inwd == SZINT` before `SZLONG`, so with the two equal a 64-bit initialiser is
emitted as `.long`, four bytes — but the disk formats are where it would end.

So these typedefs are not a step toward changing the model. They are the
opposite: they mark every place that would have to be revisited if it ever did.

### How the change was verified

A refactor that is a no-op has to be shown to be one, and "the tests pass" is
weaker than it sounds here.

- **Layout, measured directly.** `tests/mkfs/probe.c` prints `sizeof` and
  `offsetof` from the V8 side: `dinode 64`, `filsys 4096`, `fblk 716`,
  `direct 256`, `spcl` offsets `0 4 8 12 16 20 24 28 32 96 100` — unchanged.
- **The artefacts, byte for byte.** A filesystem image built before and after
  differs at exactly seven offsets — and two images built by the *same* binary
  1.2 s apart differ at exactly the same seven. The refactor moves no byte the
  clock does not.
- **The tape likewise**, once the comparison is made honestly: dumping two
  *different* images exaggerates it, because the dinode times inside `c_dinode`
  differ too. Dumping the *same* image with both binaries gives 28 differing
  bytes, and classifying every one by its position within the 1024-byte record
  puts 14 in `c_date` and 14 in `c_checksum`, with **zero** anywhere else.
- **A guard, mutation-verified.** `tests/mkfs` asserts `v8_i16/u16/i32/u32` are
  `2 2 4 4`. Widening `v8_i32` to `long` turns it red *first*, ahead of the
  eight downstream size assertions it causes — which is the point of naming the
  root cause rather than only its symptoms.

### And `sys/fblk.h` had never been imported at all

Found while doing this. `rootfs/usr/include` is built by copying third_party's
pristine headers and then overlaying ours (`Makefile:1867` then `:1873`), so a
header nobody imported silently stays 1985's. `struct fblk` — an on-disk record
in the same image as `dinode` and `filsys`, both of which are patched copies
here — was one of them. It measured 716 anyway, because `int` is 32 bits here
as it was on the VAX and `daddr_t` came from our patched `<sys/types.h>`: right
by coincidence, twice over, and **invisible to anyone auditing "the port's
headers"**, because it was not among them. `tests/deps` had even written the
gap down — its case read *"sys/fblk.h is upstream, so sys/filsys.h stands for
it"* — and now depends on the real file.

## `values.h` — a fourth machine arm, and an LP64 defect in the formula below it

Imported for `f77` stage 1: `src/libI77/ecvt.c` is the tree's **only** includer
of `<values.h>`, and it did not compile.

`values.h` describes a floating-point format and selects one by a macro the
compiler predefines. Upstream's three arms are `u3b || u3b5 || u3b2`,
`pdp11 || vax`, and `gcos`. **Measured, this port's `cc` predefines `unix` and
`arm64`** — so none of the three is taken and `_DEXPLEN`, `_HIDDENBIT` and
`_IEEE` are simply undefined. On a real V8 the `vax` arm was taken, because
V8's cc predefined `vax`; the arm64 arm is the exact counterpart.

The failure is **loud** — `ecvt.c` will not compile — rather than a wrong
number, which is the good direction and the reason nothing else in the tree was
affected. This is `sys/fblk.h`'s shape with the opposite outcome: another header
nobody had imported, still 1985's, and this one could not stay right by
coincidence because there was no arm to be right.

The values are IEEE 754 binary64/binary32, identical to the `u3b` arm because
the 3B20 was IEEE too. They are stated separately anyway: sharing the arm would
make this machine claim to be that one.

### And `DMAXPOWTWO` had to be restated, because the generic formula is LP64-unsafe

At the bottom of the file, unconditional in upstream:

```c
#define DMAXPOWTWO	((double)(1L << BITS(long) - 2) * \
				(1L << DSIGNIF - BITS(long) + 1))
```

It needs `DSIGNIF > BITS(long)` for the second shift to be non-negative:

| machine | `BITS(long)` | `DSIGNIF` | second shift |
|---|---|---|---|
| VAX | 32 | 56 | 25 |
| arm64 | 64 | **53** | **−10** |

A negative shift is undefined behaviour, and `ecvt.c:64` evaluates it at run
time (`if (value >= 2.0 * MAXPOWTWO)`). The quantity meant is the largest
double whose neighbours are one apart, i.e. `2^DSIGNIF` — which is *directly*
expressible here precisely because `DSIGNIF` is smaller than a long rather than
larger.

`DMAXPOWTWO` and `FMAXPOWTWO` are therefore `#ifndef`-guarded at the bottom
rather than given a fourth `#if`: what has to be said is *"this machine already
answered"*, and that is what `#ifndef` says.

`tests/wavea` asserts the arm's three constants (`11 1 1`) so that removing it
is a decision rather than a silent regression.
