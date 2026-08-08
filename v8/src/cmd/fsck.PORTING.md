# fsck(8) — PLAN.md §8a step 4d

`@(#)fsck.c 4.13 (Berkeley) 81/03/09`, 1925 lines, one translation unit.
Imported by `tools/import.sh v8/usr/src/cmd/fsck.c`; upstream blob hash in
`src/cmd/PROVENANCE`. Installed at `/etc/fsck`, which both `Admin/etcfiles` and
the shipped tree agree on.

It is the fifth image tool and **the first that repairs**. `icheck`, `dcheck`
and `clri` are covered in `src/cmd/icheck.PORTING.md`; `mkfs` in
`src/cmd/mkfs.PORTING.md`. Everything before this one either writes a
filesystem from nothing or reads one and prints. fsck reads a damaged one and
writes it back, which is a different risk and gets a different first test: on a
clean image it must change **no byte at all**.

## One compile error in 1925 lines, and what that measures

```
"src/cmd/fsck.c":270:MAXDATA undefined
```

That was the whole of it. A 1925-line 1981 filesystem checker, full of
`struct dinode`, `struct filsys`, `struct fblk`, `struct direct`, `daddr_t`
arithmetic and a 4096-byte seven-armed union over all of them, compiled under
v8cc on arm64 with **one** undefined name. That is the payoff of step 4a: the
formats were fixed in `src/include`, so every program that reads them arrives
already correct. Had the headers still been LP64-wide, `struct bufarea`'s union
would have been 8192 bytes with seven arms disagreeing about it, and the failure
would have been silent short reads rather than a diagnostic.

## The five patches

### 1. `MAXDATA` — the only machine-dependent line

```c
#ifdef pdp11
#define MAXDATA	((MEMSIZE)54*1024)
#endif
#ifdef vax
#define	MAXDATA ((MEMSIZE)400*1024)
#endif
```

There is no arm for this machine, and adding one is not a matter of picking a
number, because of how the value is used:

```c
	memsize = (MEMSIZE)sbrk(0);
	memsize = MAXDATA - memsize - sizeof(int);
```

`MAXDATA` is a **ceiling address**, not a size: the top of the data segment less
how far the break has already reached. That difference is an arena only where
the data segment starts near zero — true on a VAX and a pdp11, false on Mach-O,
where the break is several gigabytes up. And `MEMSIZE` is `typedef unsigned`,
32 bits under LP64, so the cast truncates an address and the subtraction
underflows. Same family as *V8 assumes address 0 is readable*.

The arm added names the arena and derives the ceiling:

```c
#define	MAXDATA (((MEMSIZE)sbrk(0)) + (MEMSIZE)400*1024)
```

so the two lines above compute 400K−4 exactly. The truncation is then *harmless
rather than tolerated*: both `sbrk(0)` values are truncated identically and the
difference is exact mod 2³². Written as a definition rather than as an edit to
`main()` for the reason `shim/kern/h/param.h` redirects `printf` — **a
definition can be machine-dependent where a rewritten statement is just a
rewritten statement.**

**The magnitude is deliberately the VAX's.** Only the base of the arithmetic is
forced by the target; enlarging the arena would be taste, and it would cost
something real. fsck's scratch-file path — the `NEED SCRATCH FILE (%ld BLKS)`
branch in `makefs()`, which spills the block and inode maps to a file when they
do not fit in core — is authentic code that runs *only* when memory is short.
An 8 MB arena would make it unreachable for any image this port can produce.

### 2 and 3. The time_t seam, in both directions — and this is the finding

`src/include/sys/ino.h` and `src/include/sys/filsys.h` narrow `di_atime`,
`di_mtime`, `di_ctime` and `s_time` to `int`, because a `dinode` is 64 bytes and
a superblock 4096 and V8's own compiler defined `NOLONG`. `time_t` is eight
bytes here. So **the address of one of those fields is not a `time_t *`**, and
the width is invisible at the call site.

`icheck.PORTING.md` records the write direction, found and fixed there, with the
sweep `grep -rn 'time(&' src shim` — "thirty-seven calls, and this is the only
one whose target is a disk field". That was true of the tree at that moment.
Importing fsck falsified it **twice**, and the second one is a spelling the
pattern could never have matched.

**Write direction, `check1()`.** `time(&superblk.s_time)` puts four bytes onto
`s_tfree` at offset 220. Worse here than in icheck: icheck assigns
`s_tfree = 0` two lines later so the damage is masked, while fsck calls
`sbdirty()` on the next line and `ckfini()` commits it. Measured with the fix
reverted, on the clri'd image: fsck prints `2 files 1 blocks 1916 free` and
writes a superblock saying `s_tfree = 0`. **A repaired filesystem comes back
claiming zero free blocks.**

**Read direction, `pinode()`, and it is the one that hangs.**

```c
	p = ctime(&dp->di_mtime);
```

`ctime()` dereferences eight bytes, so it reads `di_mtime` as the low half and
`di_ctime` as the high half — about 7.3 × 10¹⁸ seconds for an image made today.
`gmtime()` then runs

```c
	for(d1=70; day >= dysize(d1); d1++) day -= dysize(d1);
```

≈ 2.4 × 10¹¹ times. **Not a wrong date: a live lock, with an empty stdout**,
because nothing flushes. Diagnosed by sampling the stack rather than by reading
the source — `pinode → ctime → localtime → gmtime`, 1708 of 1708 samples.

Both fixed with a `time_t` temporary, which is icheck's precedent.

**The generalisable part is where it hid.** `pinode()` runs only on a *damaged*
filesystem — from `direrr`, `adjust`, `clri` and `linkup` — so every clean-image
case passed throughout, and the first corrupt image locked the program up. A
checker whose error paths are only reached by errors has untested error paths;
that is the same shape as *an unexercised rule cannot be seen to be incomplete*.

The sweep, re-run in both directions and now complete:

```bash
grep -rnE '(ctime|localtime|gmtime|asctime)[ \t]*\([ \t]*&' src shim compiler
grep -rnE '\btime[ \t]*\([ \t]*&' src shim compiler
```

Nineteen read-direction calls and twenty-two write-direction ones, over `*.c`
and `*.h` in `src`, `shim` and `compiler`. (icheck's note says thirty-seven, and
the difference is the *spelling*, not the tree: `grep -rn 'time(&'` searches two
directories rather than three and does not allow the space in `load.c`'s
`time (&t)`. Which is its own small lesson about sweeps.) Every other target is
a `time_t`, a `long`, a `TIMETYPE` (`long int`), or a host `struct stat`'s
`st_mtime` — all eight bytes. `struct utmp`'s `ut_time` is a
`long` deliberately, and `shim/libkmemu/utmp.c` says why. **The rule to carry
forward is wider than the sweep that found it: it is not `time(&`, it is *any
callee reached through the address of a narrowed field*.**

### 4. `scrfile[80]` — a command-line argument that made fsck write

```c
	p = scrfile;
	while(*p++ = **argv)
		(*argv)++;
```

`fsck -t <path>` names a scratch file, and upstream copies the argument into an
80-byte file-scope array with no bound. Same class as `rmdir`'s `name[500]`,
`mkdir`'s `pname[128]` and `mv`'s `MAXN 100` — but reachable with no filesystem
involved at all, in the one program here that writes to filesystems.

Now 1024 (macOS's `PATH_MAX`) **and** bounded, which the other three did not
need: `PATH_MAX` is what the kernel accepts as *one* argument, and `argv[]` can
carry more. `getline(stdin, scrfile, sizeof(scrfile))` at the other end was
already bounded by `sizeof`, so it widened for free.

Measured with the bound removed and a 2000-character `-t` argument: the copy
runs into `lfname`, `checklist`, `big` and `lncntp`, and fsck reports
`***** FILE SYSTEM WAS MODIFIED *****` **on a filesystem that was well**. Not a
crash — a write. `tests/mkfs` pairs the long argument with a `cmp` against the
original, because neither half alone would see it: the overflow does not crash,
and it does not change what fsck prints about the image.

### 5. `rawbuf[32]`

`rawname()` turns `/dev/disk4` into `/dev/rdisk4` with `strcpy` + two `strcat`
into a 32-byte static sized for `/dev/rp0`. Reached from `blockcheck()`, so the
argument only has to *stat* as a block device — it does not have to be short or
to live in `/dev`. Now `1024+2`, written as a sum because the `+2` is the `r`
this function inserts plus the NUL: the longest input is 1023 characters, so the
longest output is 1024 and it needs 1025 bytes.

Not exercised by any test here, and that is stated rather than glossed: nothing
in the jail's `/dev` is a block device, so this is a fix to an unreachable path,
made because the constant is the same 1985 one and the program is the one that
writes.

## `-DDIRSIZ=14` changed kind with this program

The flag was already a property of the `$(IMGBIN)` group rather than of `mkfs`,
because `dcheck` parses the records `mkfs` writes. For those four, forgetting it
produces a wrong *answer*. For fsck it produces a wrong *program*: `pass2()`
copies up to `DIRSIZ` bytes per path component into `pathname[200]` with no
bound, so at this port's host `DIRSIZ` of 254 a **single** component overruns it
by 54 bytes. 200 bytes is upstream's sentence about 14 — thirteen levels of
nesting — and changing the other number silently rewrote it, exactly as
`MAXN-DIRSIZ-2` did in `mv`.

`pathname[200]` is therefore left at 200. It is not a 1985 buffer that needs
widening; it is correct arithmetic about a number the Makefile already pins.
`tests/deps` asserts `sys/param.h -> fsck object` so the edge cannot be lost.

## Eliminated by measurement

- **`%ld` against a narrowed value.** Seven sites apply `%ld` to what step 4a
  made an `int` — `n_files`, `n_blks`, `n_free`, `di_size`, `nscrblk`. Measured
  rather than reasoned about, with a purpose-built program: v8cc sign-extends an
  `int` argument into the 8-byte slot, so `%ld` prints 1917, 1234567 and −1
  correctly, singly and three to a call. Not a bug. (Six of the seven go through
  `pwarn`/`pfatal`'s implicit-`int` parameters first; the live output
  `2 files 1 blocks 1917 free` settles those too.)
- **`(&param)[i]`** — the idiom that blocked `mkfs`. None:
  `grep -nE '\(&[a-zA-Z_][a-zA-Z_0-9]*\)[ \t]*\['` is empty.
- **A call with more than eight arguments** — the frame-layout bug `ps` found.
  None; the widest is seven.
- **`struct bufarea`'s union is exactly 4096 in all seven arms**, and only
  because the headers are right: `b_buf` 4096, `b_lnks` 2048×2, `b_indir`
  1024×4, `b_dinode` 64×64, `b_dir` 256×16 (needs the DIRSIZ flag), `filsys`
  4096, `fblk` 716. Any one wrong and `getblk`'s `bread(..., BSIZE(big))` reads
  short in silence.
- **`l3tol(iaddrs, dp->di_addr, NADDR)`** — fsck is `l3tol`'s second caller in
  this port after `icheck`, and reads 13×3 bytes into 13×4. Correct with the
  arm64 arm restored in step 4a. fsck never calls `ltol3`; it does not write
  block addresses back.
- **`fmin`, `fmax`, `devname` and `getline` collide with libSystem exports** and
  are benign, by the same measurement that settled `_errno` and `_optarg`: the
  Mach-O linker replaces a tentative definition from an **archive**, not from a
  dylib, and none of these names is in `libv8c.a`, `libv8sys.a`, `libv8stubs.a`
  or `libkmemu.a`. The standing tripwire: **if C99 `fmin`/`fmax` are ever added
  to `libv8c.a`, fsck's two block-range variables become math functions**, with
  a linker warning as the only notice.
- **`BITFS(stat_block.st_rdev)`** — V8's `dev_t` is 16 bits and macOS's is 32, so
  this is bit 6 of a truncated device number, and a set bit would make `big`
  `BIG` and every offset in the program wrong. Inert here, and by control flow
  rather than by a patch: `big` is only derived under `S_IFBLK`, and the image
  files this port checks are regular. `icheck` needed a patch for the same shape
  because it derives `big` for regular files too.
- **`IFMPC` / `IFMPB` are undefined** in V8's `sys/inode.h`, and the `MPC`/`MPB`
  macros that use them are never expanded — `SPECIAL` has them commented out
  upstream. No change needed.
- **`sys/inode.h` is included for `IFMT`/`IFDIR`/`IFREG` alone.** `struct inode`
  is never named. Imported unmodified for this program; `tests/deps` gives it
  its own case, because a header with exactly one consumer is the kind that
  quietly stops being a prerequisite.

## What fsck fixes that the readers could only report

This is the argument for having it, and `tests/mkfs` is written as the pairing
rather than as fsck agreeing with itself. Each corruption the suite already
induced is handed to fsck, and **the other program** is asked whether the repair
happened.

| the damage | who could see it | what fsck does |
|---|---|---|
| `di_addr[0]` zeroed | icheck: `missing 1` | `1 BLK(S) MISSING` → salvage; icheck then `missing 0`, free 1917 → 1918 |
| root `nlink` 2 → 3 | dcheck: `2 2 3` | `COUNT 3 SHOULD BE 2 / ADJUST?` → the field is 2 and dcheck is silent |
| address past the volume | icheck: `8388607 bad; inode=2` | `8388607 BAD I=2`, same block and same inode |
| `clri` on inode 3 | icheck *and* dcheck, one half each | **both, in one run**: `NAME=/hello REMOVE?` and `1 BLK(S) MISSING` |

The clri row is the one worth keeping. `clri` zeroes an inode and leaves the
directory entry naming it, so icheck sees an orphaned block and dcheck sees a
dangling entry and neither can describe the other's half. fsck repairs the pair
in a single pass, and the arithmetic closes: the proto image had 1915 free
blocks, `hello`'s block comes back, 1916. A second pass reports nothing.

## Still open

- **`mklost+found`.** `third_party/.../v8/etc/mklost+found` is a 20-line shell
  script that pre-creates 256 empty files in `lost+found` and removes them,
  so the directory has slots and fsck can reconnect an orphan without extending
  it. Every reconnect above instead prints **`SORRY. NO lost+found DIRECTORY`**
  and falls through to `CLEAR?`. Running the script needs a *mounted*
  filesystem, so it waits for §8a step 5 — it is `mkdir`, `cd`, `tee`, `rm` on a
  real directory, and on a passthrough directory it would prove the shell works
  and nothing about the filesystem. `tests/mkfs` asserts the `SORRY` line rather
  than leaving the gap implicit, so the case goes red the day step 5 lands and
  names the sentence to rewrite.
- **Exit status.** fsck returns 0 whatever it finds or repairs, on the
  explicit-argument path. `exit(8)` exists but only on the `-p` checklist arm.
  Same as `icheck` and `dcheck`, and `tests/mkfs` deliberately reads output
  rather than status for all five; a future change that started exiting nonzero
  would be a deviation and should be recorded as one.
- **The no-argument path** reads `/etc/fstab` through `getfsent()` and would
  `blockcheck()` each entry. Untested, and untestable until there is something
  in the jail's `/dev` for it to find. Note that `sync()` runs unconditionally at
  the top of `main()` and is a real host syscall.
- **A 2 GB ceiling.** `lseek(fcp->rfdes, blk<<BSHIFT(big), 0)` shifts a 32-bit
  expression, so fsck cannot address past block 2²¹ at 1K. Identical to the VAX,
  where `daddr_t` was also 32 bits. Authentic, not a port defect.
- **`imax` is `ino_t`**, so 65535 inodes, and `dcheck`'s non-termination above
  that (recorded in `icheck.PORTING.md`) has no counterpart here — fsck's loops
  are bounded by `lastino`. Also authentic.

## `fsck -t` dereferenced argv before it checked argc

```c
if(**++argv == '-' || --argc <= 0)		/* upstream */
	errexit("Bad -t option\n");
```

`||` evaluates left to right, so with `-t` last the `++argv` lands on the NULL
at `argv[argc]` and `**argv` reads it *before* the count test can fire. It reads
as a guarded line and is not. On the VAX address 0 held `0207`, which is not
`'-'`, so the first operand was false, the second then fired, and `errexit`
printed `Bad -t option` — the right behaviour, reached by luck.

Swapped to `--argc <= 0 || **++argv == '-'`. Only the order changed, and it
reaches the same `errexit` without the read. Found in the whole-tree sweep of
this class (PLAN.md §4i), which turned up nine crashes; `fsck -t` was one.

**`rawname()` here is NOT the same function as `dump`'s, and this one is the
safe one.** `fsck.c:466` returns `cp` unchanged when the name contains no `/`;
`dump`'s (`dumpmain.c:239`) returns `0`, and two of its callers stored or
compared that without checking. Upstream disagreeing with itself in one source
tree — worth knowing before assuming a shared helper behaves the same way in
both. An audit of this sweep initially reported fsck as having the null-return
form; reading it settled that it does not.
