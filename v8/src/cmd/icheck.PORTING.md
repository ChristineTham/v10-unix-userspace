# Porting notes: icheck(8) and dcheck(8)

PLAN.md §8a step 4. `@(#)icheck.c 4.1 (Berkeley) 10/1/80` and `dcheck.c`,
imported and patched in **one line between them**. They are V8's filesystem
checkers, and they are here because `mkfs` needed a reader that was not us.

*Named `icheck.PORTING.md` for the reason `mkfs.PORTING.md` is — upstream keeps
both as bare files in `cmd/`, and `tools/import.sh` mirrors the upstream path.
`dcheck` is covered here rather than separately; they were ported together and
the interesting facts are about the pair.*

## Why they were worth doing before anything else in step 4

Everything `tests/mkfs` asserted before these arrived asks whether the image
matches **what this port believes a V8 filesystem is** — and the belief and the
bytes come from the same place, so a shared misunderstanding satisfies both.
`icheck` and `dcheck` are 1985 code that knows nothing about `mkfs`:

```
$ /etc/mkfs fs.img 2000
isize = 1280
m/n = 3 1000
$ /etc/icheck fs.img
fs.img:
files      2 (r=1,d=1,b=0,c=0,l=0)
used       1 (i=0,ii=0,iii=0,d=1)
free    1917
missing    0
$ /etc/dcheck fs.img
fs.img:
```

`free 1917` is walked out of the free list, block by block, through the `fblk`
chain; the superblock's `s_tfree` is a counter `mkfs` maintained while writing.
**Two independent computations of the same number.** And `used + free + s_isize`
is `1 + 1917 + 82` = 2000 = `s_fsize`, with `missing 0` — every block on the
volume accounted for exactly once. If `NICFREE`, the `fblk` layout or `daddr_t`'s
width were wrong by one, the walk would end early and the arithmetic would not
close. `tests/mkfs` asserts the arithmetic rather than the number, so it still
says something at another size.

They also gave `l3tol` its first caller. `ltol3` packs an inode's block list into
3-byte disk addresses and `l3tol` unpacks it; the pair had been patched, then
re-patched when `daddr_t` narrowed, and **nothing in the port had ever read one
back**. Both checkers do, at `icheck.c:278` and `dcheck.c:152`, and they find
block 83 where `mkfs` put it.

## They need no mount, which is why they come before `df`

`icheck fs.img` takes the filesystem as an argument and opens it by name. `df`
does not: upstream's `dfree()` reaches a superblock only through an explicit
`/dev/...` argument or through a mount point whose `st_dev` matches some
device's `st_rdev`, so `df`'s rung 5 waits on §8a step 5.
`src/cmd/df/PORTING.md` has that, including the prediction that step 4 would
close it by itself.

`stat()` on a plain file gives `st_rdev` 0, and `icheck` does
`#define dev status.st_rdev`. That is not a coincidence to be grateful for — it
is the right answer. `BITFS(dev)` is `(dev) & 64`, so 0 selects the V7-derived
free-list format, 1024-byte blocks, 16 inodes per block, which is exactly what
`mkfs` writes. The same reasoning `df.c` records for setting `dev = 0`.

## The one patch: a pointer that had already encoded the old width

`icheck.c`'s `alloc()` declares `register long *p` and walks `sblock.s_bfree`,
the BITFS free-block bitmap. `<sys/filsys.h>` now declares that array `int`,
because a disk record's fields are the widths a VAX gave them. So `int *`.

**This is the third instance of the same thing**, and it is the one that
generalises. Narrowing `daddr_t` at the seam is exactly the prescribed fix —
"fix it where the width is decided, never per program" — and it broke
`libc/gen/ltol3.c`, then `l3tol.c`, then this. Each of those three was *correct
when written*, for a `long` that was eight bytes.

Two things make this instance the useful one:

- **The compiler said so.** `illegal pointer combination, op =` at `icheck.c:375`
  — the only one of the three that produced a diagnostic, because the other two
  do their arithmetic through a `char *` and the type is inert there. A build
  that is otherwise warning-free is what made one line visible.
- **The sweep is one command and it comes up empty otherwise.** Every other
  reader of `s_free`, `s_bfree`, `i_addr` and `di_addr` in the tree indexes by
  subscript, which follows the element type on its own. Only a raw pointer
  carries the width itself:

  ```bash
  grep -rnE 'long[ \t]*\*' src shim | ...   # against those four arrays
  ```

  The rule to carry forward: **when you narrow a type at the seam, sweep for
  what already encodes the old width** — and look for raw pointers first,
  because subscripting is self-correcting and pointers are not.

It is in the `BITFS` arm, which nothing in this port can reach — `mkfs` is
free-list only and `mkbitfs` is not ported — so it is a latent bug rather than an
observed one. Fixed anyway: the port builds without warnings, and a new warning
is a loss of signal even when the code behind it is unreachable.

## `-DDIRSIZ=14`, and why it is a group rather than a flag on `mkfs`

`dcheck` walks every directory in the image and counts references, so it parses
the same 16-byte records `mkfs` wrote. When `mkfs` was alone the flag looked
like a property of that program; it is a property of **talking to an image**.
The Makefile names the set — `IMGBIN = mkfs icheck dcheck` — and generates their
rules from one template, so the fourth one cannot be compiled without it.

What a forgotten flag would do here is worse than in `mkfs`, and is the argument
for naming the group: `dcheck` would read a correct root directory as sixteen
records, find `.` and `..` and fourteen entries whose `d_ino` is 0, skip those
by V7's own deleted-entry rule — and report a **healthy filesystem as healthy**.
It would only start lying once a directory had more than one real entry per
256 bytes. It would not fail to build, it would not fail to run, and it would
not be wrong on the first thing anyone tested it against.

## The corruption cases, which are the half that proves anything

A checker that approves of a good image proves very little. Three corruptions
are in `tests/mkfs`, each producing a different and correct diagnosis:

| what was changed | what it says |
|---|---|
| the root's only block address zeroed | `used 0`, `missing 1` — the block is referenced by nothing and is not in the free list |
| the root's `nlink` 2 → 3 | `dcheck` prints `2  2  3`; **`icheck` says nothing**, correctly |
| the root's block address set to 0x7fffff | `8388607 bad; inode=2, class=data (small)`, and `missing 1` for the block it orphaned |

The middle row is the one worth keeping both halves of. A link count is not a
block, so `icheck` staying silent is the right answer — and a checker that
answered everything would be the more suspicious result.

**Neither program sets its exit status.** Both report on stdout and return 0
whatever they find, which is upstream's behaviour; every case in `tests/mkfs`
reads the output for that reason. A future change that started exiting nonzero
would be a deviation and belongs in this file.

## Not changed

- `icheck -s` rebuilds the free list and **writes to the image**. Nothing in
  the suite uses it. On a plain file rather than a device that is a normal
  write, so there is no new hazard, but a test that used it would need its own
  copy of the image and does not exist yet.
- `icheck -b` takes up to `NB` 500 block numbers to report on, `MAXFN` 500 is
  unused, and `struct dinode itab[BIGINOPB*NI]` is 64×4 = 256 inodes = 16 KB.
  All fine at VAX widths and all of them fine here now that `sizeof(dinode)`
  is 64 again.
- The 16-bit `ino_t` ceiling applies to these as it does to everything reading
  this format. An image with more than 65535 inodes cannot be described, which
  is the format's limit and not the port's.

## Still open

- **`fsck`** — 1925 lines, and the one that repairs rather than reports.
- **`clri`** — 82 lines, and the natural way to *make* a corrupt image on
  purpose rather than with `dd`. The three cases above patch bytes by offset,
  which is fine but ties them to the layout; `clri` would state the intent.
- **`mklost+found`**, which the `mkfs` man page says should run immediately
  after `mkfs`. It needs a mounted filesystem, so it waits for step 5.
- Nothing here has been read by a real V8 kernel. That is step 6.
