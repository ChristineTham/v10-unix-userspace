# Porting notes: mkfs(8)

PLAN.md §8a step 4. `mkfs` is `@(#)mkfs.c 4.2 (Berkeley) 4/20/81`, imported
unchanged and then patched in two places. It builds a free-list/1024 filesystem
image; `mkbitfs` is not ported and `mkfs` cannot make one.

*The file is `mkfs.PORTING.md` rather than `mkfs/PORTING.md` because upstream
keeps mkfs as a bare `cmd/mkfs.c`, and `tools/import.sh` mirrors the upstream
path. A directory would either move the source away from where a re-import puts
it or be a directory holding one markdown file.*

## What it produces

`$V8ROOT/etc/mkfs image 2000`, decoded from the bytes:

| | |
|---|---|
| `s_isize` | 82 — 80 inode blocks plus 2 |
| `s_fsize` | 2000 |
| `s_tinode` | 1278 — 1280 inodes less the two allocated |
| `s_tfree` | 1917 — data blocks 82..1999, less the root's one |
| `s_m` / `s_n` | 3 / 1000, and `s_free[0..2]` is 497 494 491, so the free list really is interleaved by *m* |
| inode 1 | `IFREG`, `nlink` 0 — the bad-block holder `bflist()` writes |
| inode 2 | `040777`, `nlink` 2, `size` 32, one data block |
| that block | `.` at offset 0, `..` at offset 16, both inode 2 |

That is a V8 filesystem, made by Bell Labs' `mkfs`, compiled by Bell Labs'
compiler, on a machine that postdates it by forty years. Every number above is
asserted in `tests/mkfs`.

### The indirect block, and why it needed a case of its own

Every image above reports `i=0` — two files, one data block, all thirteen
addresses fitting in the inode. That leaves the riskiest structure in the format
untouched, so `tests/mkfs` builds a third image with a 20-block file:

```
used      22 (i=1,ii=0,iii=0,d=21)
```

An inode's `di_addr[]` is **three**-byte packed and goes through `ltol3`; an
indirect block is a raw `daddr_t` array, and `NINDIR(0)` is
`BSIZE/sizeof(daddr_t)`. So this is where the width is most directly
load-bearing — and where it would have hidden best. `mkfs` writes that array
with `sizeof(daddr_t)` and `icheck` reads it with `sizeof(daddr_t)`, so at eight
bytes they would hold 128 entries and **agree with each other perfectly**. Only
a VAX would disagree, or the hardcoded `NMASK(0) 0377` and `NSHIFT(0) 8` in
`param.h`, which is why the suite asserts those against `NINDIR` rather than
trusting either.

The case therefore reads end to end rather than counting: each of the file's
blocks is stamped with its own index, and entries 0, 5 and 9 of the indirect
block are followed to blocks that must say `block-10`, `block-15`, `block-19`.
A four-byte stride puts entry 5 at byte 20; an eight-byte one reads byte 40 and
lands in entry 10's slot. Verified by mutation — `LADDR` 10 → 9 turns exactly
these six cases red and moves nothing else.

**The step this does not finish is `df`'s rung 5.** `df.c` carries a
`kmemu_fsstat()` call this port added, which is what breaks its upstream
makefile; upstream's `dfree()` instead does `bread(1L, &sblock, sizeof sblock)`
and prints what the superblock says. Now that a correct superblock exists, that
call can come back out — see `src/cmd/df/PORTING.md`. It is a separate change
and is not made here.

## The two source changes

### `gmode()` read its own parameters at the wrong stride

Upstream is one line:

```c
gmode(c, s, m0, m1, m2, m3)
	for(i=0; s[i]; i++)
		if(c == s[i])
			return((&m0)[i]);
```

Take the address of the first of four undeclared parameters and index forward
through the other three. On a VAX that is exact: `ARGINIT` is 32, arguments sit
four bytes apart, and `&m0` is an `int *`. v8cc spills x0–x7 into **eight**-byte
slots — `SZARG` is `SZLONG` — so `(&m0)[1]` reads the top half of `m0`'s slot and
`(&m0)[2]` the bottom half of `m1`'s. Measured:

```
gmode('-', "-bcd", IFREG,IFBLK,IFCHR,IFDIR) -> 0100000   correct
gmode('b', ...)                             -> 0         want 060000
gmode('c', ...)                             -> 060000    want 020000
gmode('d', ...)                             -> 0         want 040000
```

**This blocked every run, not an unusual one.** The built-in prototype is
`"d--777 0 0 $ "`; `'d'` is index 3, so the root inode got file type 0,
`cfile()`'s switch matched no arm, and `iput()` fell to its default:

```
isize = 1280
m/n = 3 1000
bad mode 777
```

The four are now copied into a real array, which is what the address arithmetic
meant. It cannot be fixed in the compiler — eight-byte argument slots are the
ABI, not a preference.

**It is a bug class this port had not met, and it is a singleton.** The shape is
*taking the address of a scalar K&R parameter and indexing past it*, which is
the same family as `extern float atof()` and the `yylval.p` token bug: a
declaration asserting a width the machine no longer has, except that here the
declaration is the calling convention. Swept:

```bash
grep -rnE '\(&[a-z_][a-z_0-9]*\)\s*\[' src shim compiler
```

finds `mkfs.c:324` and nothing else. The *forward* form of the same idiom —
V8's `printf(fmt, args)` walking `&args` — is used in `exec.c`, `doprnt.c`,
`scanf.c`, `sprintf.c`, `printf.c`, `fprintf.c` and `troff/n1.c`, and every one
of those already walks with an eight-byte type. Only the indexed form was
missed, and only one file uses it.

### `char string[50]` is a 1985 buffer holding a 2026 pathname

The `mv`/`mkdir`/`rmdir`/`sed` family again, and now `PATH_MAX`-sized at 1024
like all four of those. `string` holds three different things, and two of them
are **host pathnames** — the boot program at `f2`, and the contents of every
regular file in a prototype. The third is a directory entry name, which this
port's `DIRSIZ` lets run to 254, so 50 no longer bounds that either.

What made it worth fixing rather than noting is where the overflow lands. In the
linked binary `_string` is at `0x100013880` and `_utime` at `0x1000138b8` —
**56 bytes away** — so a 57-character token rewrites the timestamp every inode
is stamped with. Measured, two otherwise identical prototypes:

```
content path 10 chars -> every inode mtime 1786141106 = 2026-08-07   correct
content path 70 chars -> every inode mtime  795046515 = 1995-03-12   ASCII from the path
```

An image that is wrong only in its dates passes every casual look. Past
`_utime` are `_errno` and then `__sibuf`, stdio's own 4096-byte input buffer —
the one `getc(fin)` is reading from.

`getstr()` is bounded with it, and reports rather than truncates, for the reason
`fstab.h`'s widening gives: a name that cannot be stored cannot be written
truthfully, and half a pathname inside a filesystem image is worse than a
refusal. Two tokens went with it: `case EOF` beside upstream's `case '\0'`, and
`c != EOF` in the scan loop. `getch()` returns `*charp++` for the built-in
prototype, where the end is a NUL, but `getc(fin)` for a proto *file*, where it
is `EOF` — so upstream's end-of-input arm could never fire on the path it was
written for, and a prototype missing its final `$` appended `0xFF` off the end
of bss for as long as the read kept failing. Same on the VAX; it is corrected
here only because the bound would otherwise have reported a runaway as a long
token.

## `DIRSIZ` is 14 for this program and 254 for everything else

`mkfs` is compiled `-DDIRSIZ=14`. It was the only thing in the tree that was;
it is now the Makefile's `$(IMGBIN)` group of five — `mkfs`, `icheck`, `dcheck`,
`clri`, `fsck` — because the flag turned out to be a property of *talking to an
image* rather than of writing one. `src/cmd/icheck.PORTING.md` has why `dcheck`
joined, and `src/cmd/fsck.PORTING.md` why for `fsck` it is a memory-safety
property as well as a format one.

This port raises `DIRSIZ` 14 → 254 in three headers because macOS filenames
exceed fourteen characters. That is right for every directory the shim serves
off the host and wrong for every directory `mkfs` puts inside an image, because
an image is not read only by us.

**The failure would not have looked like one.** A V8 kernel reading a 256-byte
record finds `.`, then fifteen slots whose `d_ino` is 0 — which is V7's own
encoding for a deleted entry, the same accident `src/include/sys/dir.h` already
documents — so it reads the directory correctly and then *allocates into* those
slots, and the result parses on neither side. A three-entry root has `i_size`
768 at 254 against 48 at 14.

Getting the flag to work needed a one-line patch to `<sys/param.h>`, and the
patch is interesting because **the port's own comment there described a
mechanism that was not present**. It said "all three use `#ifndef DIRSIZ`, so
whichever header a program reaches first wins". Upstream guards `dir.h` and
`sys/dir.h` and leaves `param.h` bare, so on a real V8 `param.h` always won by
redefinition. The claim cost nothing while every spelling said 254 and was found
the moment something wanted a different one: cpp answered
`param.h: 86: DIRSIZ redefined` and handed `mkfs` 254 anyway.

A `-D` is exactly the kind of thing that gets forgotten, so `tests/mkfs` asserts
it **on the bytes of a generated image** — `..` at offset 16, root `i_size` 32 —
rather than on the command line, which would only be reading the rule back to
itself. Dropping `IMGDIRSIZ` to 254 turns six cases red.

### And the byte assertions are not belt and braces — they are the only guard

Measured after `fsck` landed, by building `mkfs` the way Bell Labs' own
`Admin/Mk` builds a bare `cmd/*.c`: `cc $CFLAGS -o $B $B.c`, with no `-D` at
all. That is correct on a machine whose `param.h` says 14, and here it produces
a `mkfs` that writes `i_size 512` with `..` at offset 256.

**icheck, dcheck and fsck all pronounce that image clean.** It is the accident
above, running the other way: the 240 bytes between `.` and `..` are zero, a
zero `d_ino` is a deleted entry, so a 16-byte-record reader skips fifteen empty
slots and finds `..` where the 254 writer left it — two entries, two links,
nothing missing. A wrong *writer* is invisible to every reader this port has,
which means the group's own checkers cannot be the guard for the group's own
flag. `tests/mkfs` section 8 builds it that way and asserts the difference at
`i_size` and at offset 16, because that is the only place it exists.

That also puts a precondition on ever running `Admin/Mk` for these five, which
PLAN.md §4a now records as a fourth kind of rung-5 stop — the kind that
succeeds.

**Named rather than left to be discovered: this becomes a real conflict at
§8a step 5.** When `v8fs` mounts an image, a program inside the jail reading a
directory will get 16-byte records from the image and 256-byte records from a
passthrough directory — the same `read(2)` on the same descriptor type,
answering with two different record sizes. That is a decision for the step that
introduces the second filesystem, not for this one.

## `ltol3`/`l3tol`, which mkfs is the only caller of

Two of this port's own patches disagreeing, rather than a fact about the
machine. `src/libc/gen/ltol3.c`'s arm64 arm skipped **five** bytes between
addresses, because `daddr_t` was `long` and LP64 made that eight; `<sys/types.h>`
now narrows `daddr_t` to four, so the VAX stride is right again and both files
are back to it. Their declared parameter moved with the stride — upstream says
`long *` because on a VAX that *was* `daddr_t`.

Left unfixed it corrupted every inode `mkfs` wrote, and in two ways at once. The
block list was decimated — `di_addr[k]` received `i_addr[2k]` — and the last six
addresses came from **35 bytes past the end** of a `struct inode` that is a
stack local in both `cfile()` and `bflist()`. One of them read as block 6035408
of a 1935-block image.

`l3tol` has **no caller in this port** — `fsck` and `icheck` are its customers
and neither is imported — so `tests/mkfs` round-trips the pair directly. It is
the only thing that will ever notice them disagreeing again.

## Reproduced, not introduced

Each of these behaved identically on the VAX, where `long` was also 32 bits, and
each is left alone:

- `s_fsize = n` and `di_size = i_size` truncate a size over 2 GiB.
- `s_isize` is `unsigned short`; the numeric path caps the ilist at
  `MAXISIZE/NIPB`, the **prototype path does not**, so a large enough proto can
  wrap it.
- `s_tinode` is `ino_t`, and `ino` itself wraps at 65536.
- `bflist()`'s `d = s_fsize-1; while(d%f_n) d++;` carries negative near 2^31 and
  the free list comes out empty.
- The proto path sets `s_isize = n+3` where the numeric path sets `n+2`.
  Upstream's own inconsistency; asserted in `tests/mkfs` so that meeting it does
  not read as an off-by-one here.

One limit moved, and outward. `lseek(fso, bno*BSIZE(0), 0)`: the product's
language type is `int`, which would wrap at 2 GiB, but v8cc evaluates it in 64
bits (measured — `ldrsw` then a 64-bit `lsl`), so the ceiling is `daddr_t`'s own
2^31 blocks, or 2 TiB. The VAX ceiling was 2 GiB. Do not "fix" this with a
`(long)` cast: it changes no generated code and would hide that v8cc's
evaluation width is what carries it.

## Still open

- `df`'s rung 5, above.
- `mklost+found`, which the man page says should run immediately after `mkfs`.
  It needs a mounted filesystem, so it waits for step 5.
- `fsck`, `icheck`, `dcheck`, `clri` — 2773 lines between them, and the first
  real readers of `l3tol`. They are the natural way to validate an image without
  trusting the program that wrote it.
- The SIMH cross-check (step 6). Nothing here has been read by a real V8 kernel.
