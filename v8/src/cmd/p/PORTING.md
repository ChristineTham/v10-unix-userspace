# p(1) — porting notes

Wave A2 batch 2d.  A pager: `p [-N] [file...]` prints N lines at a time and
waits for the terminal between pages.  Three sources — `p.c`, `pad.c`,
`spname.c` — of which two changed.

Upstream: `v8/usr/src/cmd/p`.  `PROVENANCE` records the blob hashes.

## Changes

### 1. `p.c:25` — the absolute jerq include

Was `#include "/usr/jerq/include/jioctl.h"`, now `<jioctl.h>`.  An absolute
path into the 1985 filesystem is the one kind of include cpp cannot be told to
resolve elsewhere: no `-I` affects it.  The header itself is authentic and
unchanged and the rootfs carries it at `usr/include/jioctl.h`, copied from
`jerq/include` by the Makefile.

This is the **fourth** instance of the identical change, after `src/cmd/ls.c`,
`src/cmd/mc.c` and `src/lib/libtermlib/termcap.c`.

The block is **live** here, as it is in `mc.c` and unlike the other two: `p.c`'s
first line is

```c
#define	JERQ	No home should be without one
```

so the file turns it on itself.  What it guards is
`ioctl(1, JWINSIZE, &sttybuf)`, which the shim does not implement, so `ismpx`
stays 0 and `p` keeps `DEF` (22) and `WIDTH` (79) — upstream's own fallbacks
five lines further on.

### 2. `spname.c` — `newname[80]` and a bound that was never there

**This is the third copy of `spname` in the tree, and it is not the one that
was already fixed.**  `src/cmd/sh/spname.c` is the later rewrite: `<ndir.h>`,
`opendir`/`readdir`, a `score` out-parameter, `newname[128]` **and** a bound
test at the top of the loop.  This one is the original — `<sys/dir.h>` and a
raw `read(2)` of `struct direct` — and the difference that matters is that it
has **no bound test at all**.

`80` is a sentence about `DIRSIZ`.  A component is copied out of `best[]` by

```c
p = best;
do; while(*new++ = *p++);
--new;
```

so a component can contribute `DIRSIZ` characters.  Upstream's 80 therefore
held about five components of 14; this port raises `DIRSIZ` to 254
(`src/include/sys/dir.h:32`), so **one** component can overrun it by 175 bytes
and an ordinary `/usr/local/share/...` path overruns it on names of any length.
That is exactly `mv(1)`'s `MAXN-DIRSIZ-2`: a constant encoding a relationship
with a number this port changed.

`newname` is 1024 — macOS's `PATH_MAX`, the number `sh`, `mv`, `mkdir` and
`rmdir` already use here — and the loop gains
`if (new >= &newname[1024-DIRSIZ-2]) return((char *)0);`, which is the guard
`sh`'s copy has had all along.

**THE ABSENCE OF THE GUARD IS WHAT HID IT, which is the inverse of how the same
change was found in `sh`.**  There, raising `DIRSIZ` made `newname[128-DIRSIZ-2]`
evaluate to −132, the `>=` was true on the first pass, `spname` returned 0
forever and `cd` stopped correcting — a loud, immediate, visible failure.  Here
there is nothing to go negative, so the identical change to `DIRSIZ` made this
copy **silently worse** instead of visibly broken.  A missing guard is harder to
find than a wrong one.

**`best[]` does NOT need `sh`'s fix, and the reason is worth recording.**  `sh`'s
copy carries upstream's own `#undef DIRSIZ / #define DIRSIZ 14`, because
`<ndir.h>` redefines `DIRSIZ` as the function-like `DIRSIZ(dp)` and
`guess[DIRSIZ+1]` needs a plain number — so `best[]` stayed 15 bytes while
`readdir` returned up to 254 characters, which is the measured one-byte overflow
recorded in `src/cmd/sh/PORTING.md`.  This copy includes `<sys/dir.h>`, has no
`#undef`, and therefore sizes `best[]` at the real 255.  **The two files needed
opposite halves of the same fix.**

Reachability is not theoretical: `spopen()` calls `spname()` whenever `fopen`
fails, which is what `p mistypedname` does.  Measured — `p f.tx` in a directory
holding `f.txt` prints `"p f.txt"?` and then the file.

#### The mutation did not fire, and that is the honest record

Reverting both halves — `newname[80]` and the bound test deleted — leaves
`tests/wavea` **entirely green**, including the case written for it (a
255-character component) and a 1017-character five-component path.

The reason is the third documented one, and its mechanism is worth naming.
`newname[80]` is followed in BSS by `guess[255]`, `best[255]` and `nbuf` (257),
so **767 bytes of adjacent static storage absorb the write**.  The copied
string therefore ends up contiguous and NUL-terminated exactly where the
function's return value points, and the program produces the right answer while
scribbling over three of its own variables that are dead by then.  Undefined
behaviour that happens to work — the `strncat` verdict, where the output was
correct the whole time and only the access was out of bounds.

An AddressSanitizer harness would see it, and one was attempted and abandoned:
`spname` reads directories with a raw `read(2)` of `struct direct`, which only
returns V7 records because `libv8sys` synthesizes them, so a standalone
clang+ASan binary finds no entries, never reaches the copy, and measures
nothing.  Instrumenting it properly means an ASan build of the shim, which is a
step of its own.

So this fix rests on **arithmetic, not on a red test**: 255 bytes copied into
80 is not a judgement call.  What `tests/wavea` guards is the half that is
observable — that `spname` still functions at `NAME_MAX` — and its comment says
which half that is.  The same is true of the `sh` copy, whose one-byte overflow
was measured under ASan and recorded in prose rather than turned into a case.

## Eliminated by measurement

### `pad.c`'s seven pointer warnings are upstream's own

v8cc emits `illegal pointer combination` at `pad.c:47,57,57,66,67,108,116`.  All
seven are `char *` against `FILE`'s `_ptr`/`_base`, and **`<stdio.h>` declares
those `unsigned char *` upstream** — byte-for-byte the same declaration this
port installs.  So `pad.c` produced the identical diagnostic on a VAX.  Not an
LP64 issue, not ours, unchanged.

The LP64 audit had flagged `pad.c` for a different reason — `malloc` is called
undeclared at `:11`, `:12` and `:52` — and that turns out not to be what the
compiler is complaining about.  Every one of the three casts its result
(`(PAD *)malloc(...)`), which is the shape `src/cmd/awk/PORTING.md` measured as
a no-op: v8cc never materialises the value as a 32-bit quantity, so there is
nothing to truncate.  `tests/v8ccom`'s rootfs-wide truncation sweep is the
standing guard on that and is silent for `p`.

### `spopen`'s EOF loop is upstream's and stays

```c
c = getc(tty);
if(c != 'n') f=fopen(file, mode);
while(c != '\n') c = getc(tty);
```

If `/dev/tty` is at end of file, `getc` returns −1 forever and the `while` never
terminates.  Measured: `p f.tx 3</dev/null` hangs.  A V8 whose `/dev/tty` was at
EOF did the same thing, so there is no VAX answer to restore and S1 forbids the
change.  What follows for the test suites is that `p` must never be given a
non-responsive fd 3 *and* a name that fails to open — `tests/wavea` supplies a
newline on fd 3, and the crash probe reaches `p: no /dev/tty` and exits 1
because it opens no fd 3 at all.

## Still open

`JWINSIZE` is unimplemented, so `p` cannot size itself to the window.  That is
the same gap `ls.c` and `mc.c` record, and `src/lib/libtermlib/PORTING.md`
explains why closing it is a step of its own: `struct winsize`'s column count is
a `char`, so a terminal past 127 columns reads negative.
