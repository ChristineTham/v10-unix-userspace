# Porting notes: quot(8)

PLAN.md §8a step 4e. `cmd/quot/quot.c`, 254 lines, imported with its makefile
and patched in **one line**. Disk usage by user: it reads every inode and sums
blocks, files and byte-years per uid.

```
$ /etc/quot -f fs.img
fs.img:
     3	    4	root
     1	    1	#3
```

## The one patch, and it took the program down on its DEFAULT invocation

```c
qcmp(p1, p2) register struct du *p1, *p2;
{
	if (p1->blocks > p2->blocks) return(-1);
	if (p1->blocks < p2->blocks) return(1);
	return(strcmp(p1->name, p2->name));
}
```

`du[]` is `NUID` = 2048 entries, indexed by uid, and only the uids present in
`/etc/passwd` get a `name`. The jail's passwd holds two. So **2046 entries have
`name == 0`**, and `qsort(du, NUID, ...)` compares those against each other:
`strcmp(0, 0)`, before a single line is printed.

On the VAX address 0 was the first byte of the a.out header in the text segment,
so that compared a byte with itself and returned 0. macOS leaves page 0
unmapped: `quot image` exits 139 in `strcmp`. Measured on the linked binary —
2046 of 2047 comparisons have a null name, the first at comparison #2 — and
confirmed from the other side, by giving every uid 0..2048 a passwd entry, which
makes the *same binary* run clean.

The fix is the empty string, not a null guard:

```c
	return(strcmp(p1->name? p1->name: "", p2->name? p2->name: ""));
```

The VAX's byte at address 0 was `0207` — below every character a name can hold —
so an unnamed uid sorted before a named one at equal block counts, and two
unnamed ones compared equal. `""` reproduces both. **That preserves the observed
ordering rather than only removing the fault**, which matters because the
ordering is visible: `report()` prints a `#N` row for any uid that owns blocks
and has no passwd entry, and stops at the first row with zero blocks.

Same family as `refer5.c`'s `prefix(".[", lookat())` and `ncheck.c`'s
`atol(argv[1])` — see `../ncheck.PORTING.md`, ported in the same step.

## It is in `$(IMGBIN)` and does NOT need `-DDIRSIZ=14`

The group is "programs that talk to a 1985 image", and quot is one. But it reads
**inodes only**: no `<sys/dir.h>`, no `struct direct`, no `DIRSIZ`. Its object is
byte-identical with and without the flag, and `tests/mkfs` asserts that by
compiling `quot.c` twice and `cmp`ing, rather than by this sentence.

Membership is still the right call — the flag is what stops the *next* import
being compiled without it — and here the measured no-op pays for itself:

**quot is the only one of the seven image tools that is not blocked from rung
5.** Its own upstream makefile is `CFLAGS = -O` with no `-D`, so what Bell Labs'
build description produces is the same program. `tests/jail` builds it with V8's
make, V8's cc and V8's sh under `V8JAIL=strict`, then hands the result and the
installed binary the same image and requires the same answer. That is the
positive counterpart to the `who` groveler pair in the same suite, where the
difference between a rung-5 binary and ours is the point.

`quot: quot.o` has no rule for the object, so V8 make's built-in `.c.o` does the
work — the `tsort` idiom.

The makefile's `install` target is not part of the rung-5 claim: it runs `chown
bin` and `chgrp sys`, neither of which this port has, and both of which need
root. The other sixteen rung-5 entries build rather than install for the same
reason.

## The relations it adds to `tests/mkfs`

A seventh reader is only worth having if it computes something the others do
not, and quot does: **`icheck` walks `di_addr[]`, quot computes
`ceil(di_size/BSIZE)`.** Different fields of the same inodes, so they can be
made to disagree — and the disagreement is exactly the metadata:

```
quot's block total + icheck's indirect count == icheck's `used'
quot's file total                            == icheck's `files'
```

Section 6's indirect-block image is where that stops being a tautology: 21
blocks of file against 22 blocks allocated, the extra one being the indirect
block, which holds no file data and so is invisible to `di_size`. Every other
image in the suite reports `i=0`, where both computations would agree even if
one were wrong.

Note quot counts inode 1 — `bflist()`'s bad-block holder, `IFREG` with size 0 —
because `acct()` returns early only on `(di_mode&IFMT) == 0`. That is why the
file totals match `icheck`'s `files` exactly rather than being one short.

## Eliminated by measurement

- **The narrowed-field-address seam.** quot's only contact with a narrowed field
  is `now - ip->di_mtime` at `quot.c:152`, which is a **value** read and
  therefore correct — the address is what lies, not the field. Both sweeps in
  CLAUDE.md are empty on this file. Verified end to end: backdating inodes 2..5
  to 1986 makes `quot -b` print `162.51` byte-years for 4 blocks over 40.6
  years.
- **`now = time((long *) NULL);` with `time` undeclared** — suspected first, and
  the measurement refuted it. Implicit `int` return, `time` really returns
  `long`, and v8cc emits `bl _time; mov x9, x0` with **no `sxtw`**: a helper
  returning `0x280003034` comes back intact through a global `long` and through
  a pointer cast. The classic implicit-int-return truncation does not occur in
  this compiler's codegen. Worth recording, because the hazard is usually stated
  as though it did.
- **`(&param)[i]`** — none.
- **Declarations.** `struct passwd *getpwent()`, `char *malloc()`, `char *copy()`
  are all declared. No `float atof()`, no `(int)signal`.
- **Calls with more than eight arguments** — none; the widest is 4.
- **libc gaps and the variadic ABI** — `nm -u` on the linked binary is **empty**.
  `getpwent` reads the **jail's** `/etc/passwd`, because `src/libc/stdio/getpwent.c`
  opens the literal path through `fopen` and `rootpath()` applies — no repeat of
  the `getgrent`/`ls -g` host-database leak. `printf("%15.2f", byteyears)` goes
  through V8's own `_doprnt`, so there is no `s0`/`d0` exposure.
- **Symbol collisions.** 41 file-scope names probed; one hit, `acct`, and it is
  benign — quot defines it as a *function*, so it wins, and libSystem is a dylib
  rather than an archive besides.
- **`sizeof(dinode)`.** `ITABSZ/ISIZ(0)` is 16 blocks per pass and `sizeof itab`
  is 16384 = 16 × 1024. Correct **only because step 4a fixed the width**: at the
  old 80 bytes quot would have advanced 21 blocks while reading 20.
- **The 16-bit ceiling.** quot has none — `ino` and `nfiles` are `unsigned` — so
  where `dcheck` hangs it walks past end of file and exits 1 after one `read
  error`. No deadline needed.

## Reproduced, not introduced

- **`quot.c:63`, `if (n>NUID) continue;` should be `>=`.** A passwd entry with
  uid exactly 2048 writes `du[2048].name`, eight bytes past an 81920-byte array.
  Measured: `_du` spans `0x10001b010`–`0x10002f010`, so the write lands 20 bytes
  into `_itab`, over `itab[0].di_addr[12..19]`, which `bread()` refills from disk
  before use. **Left as upstream wrote it.** The port doubled the write from four
  bytes to eight by widening `char *name`, but the write is out of bounds either
  way and unreachable without a uid of exactly 2048; fixing it would be taste
  rather than a change forced by the target. Note upstream guards the *reachable*
  side correctly — `acct()` has `if (ip->di_uid >= NUID || ip->di_uid<0) return;`
  for the uid that comes off the disk.
- **`quot /nope` prints `quot: cannot open /nope` and exits 0.** Only the `read
  error` path exits nonzero, and that one calls `exit(1)` where `ncheck`
  zero-fills and continues. Same rule as the other six: read the output, not the
  status.
- **`quot -c` with a negative `di_size`** writes `sizes[n]` at negative `n`.
  Measured with `di_size = -2000000` → `sizes[-1953]`, which lands in `_itab` and
  is refilled from disk. No crash.
- **`dev = stb.st_rdev`** without `makedev(0, bigflag)`, the same caveat
  `icheck.PORTING.md` records.

## Still open

- **`quot` with no arguments reads `/dev/usr`.** `dargv[]` is compiled in and
  names a VAX device that does not exist here, so the bare invocation prints
  `quot: cannot open /dev/usr`. That is upstream's default and correct; it
  becomes answerable when §8a step 5 puts a mounted image behind `/dev`.
Nothing else. `-n` looked like it belonged here and does not: the manual page
says

> Cause the pipeline `ncheck filesystem | sort +0n | quot -n filesystem` to
> produce a list of all files and their owners.

and that runs **verbatim**, with V8's `ncheck`, V8's `sort` and V8's `quot`:

```
$ /etc/ncheck fs.img | /usr/bin/sort +0n | /etc/quot -n fs.img
root	/hello
root	/sub/.
3	/sub/deeper
```

`tests/mkfs` runs it, because a program that works alone and not in the
composition its own manual documents is the shape `grap | pic | troff` already
caught once. (`3` rather than `#3` is upstream: the `-n` arm prints `%d` where
`report()` prints `#%d`.)
