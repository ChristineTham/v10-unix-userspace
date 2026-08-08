# Porting notes: icheck(8), dcheck(8) and clri(8)

PLAN.md §8a step 4. `@(#)icheck.c 4.1 (Berkeley) 10/1/80`, `dcheck.c` and
`clri.c`, imported and patched in **two lines across the three**. They are V8's
filesystem checkers, and they are here because `mkfs` needed a reader that was
not us.

`clri` needed **no change at all**. It is 82 lines, it takes the filesystem as an
argument, and everything it computes — `itod`, `itoo`, `BSIZE(dev)`,
`sizeof(struct dinode)` — is right the moment the on-disk headers are.

*Named `icheck.PORTING.md` for the reason `mkfs.PORTING.md` is — upstream keeps
all three as bare files in `cmd/`, and `tools/import.sh` mirrors the upstream
path. One file rather than three because they were ported together and every
interesting fact is about how they differ from each other.*

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
block 83 where `mkfs` put it. (`ncheck` is the third caller, added in step 4e.)

**And step 4e added a second, sharper relation from a different direction.**
`quot` computes a file's size in blocks as `ceil(di_size/BSIZE)` where `icheck`
walks `di_addr[]` — **different fields of the same inodes**, so unlike `s_tfree`
versus the free-list walk these two do not merely re-derive one number, they can
be made to disagree. On a healthy image the disagreement is exactly the
metadata:

```
quot's block total + icheck's indirect count == icheck's `used'
quot's file total                            == icheck's `files'
```

The indirect-block image is what stops that being a tautology: 21 blocks of file
against 22 allocated, the extra one holding no file data and therefore invisible
to `di_size`. Every other image in the suite reports `i=0`. `src/cmd/quot/PORTING.md`.

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

- **The compiler said so.** `illegal pointer combination, op =`
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
The Makefile names the set — `IMGBIN = mkfs icheck dcheck clri` — and generates
their rules from one template, so the next one cannot be compiled without it.
`clri` does not use `DIRSIZ` at all and is in the group anyway, because the
group means *talks to an image* rather than *needs this number*.

What a forgotten flag would do here is worse than in `mkfs`, and is the argument
for naming the group. **Measured**: `dcheck` compiled without it reports a
**correct filesystem as corrupt** — the root as having 1 entry against a link
count of 2 — and exits 0 while doing so. `NDIR(dev)` becomes 4 instead of 64 and
`doff` steps 256 instead of 16, so it sees `.` and never `..`.

*This paragraph first said the opposite — that it would "report a healthy
filesystem as healthy" and only start lying once a directory held more than one
entry per 256 bytes. That was reasoned from the `d_ino == 0` skip rule and it is
wrong: the skip rule explains why the entries it does read are not garbage, not
why it would find them all. It under-counts from the first directory. Left
visible because the wrong version is the more comfortable one to believe.*

`icheck` is unaffected either way — measured, `icheck.o` is byte-identical with
and without the flag, because it never names `struct direct`. It is in the group
because it talks to an image, not because it needs the number.

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

### `clri` says the same thing in V8's own words, and splits it in two

Those three patch bytes at computed offsets: precise, and tied to the layout.
`clri image 3` states the intent, and it is a V8 program doing the damage.

What makes it worth a case rather than a demonstration is that **the two
checkers see different halves of one act**. `clri` zeroes the inode and leaves
the directory entry naming it — which is exactly what `clri(8)` warns about, and
why its man page says to run `fsck` afterwards:

| | before | after `clri image 3` |
|---|---|---|
| `icheck` | `files 3`, `used 2`, `missing 0` | `files 2`, `used 1`, **`missing 1`** |
| `dcheck` | silent | **`3  1  0`** — one entry, zero links |

Neither could report the other's half. A single checker would have made a
half-repaired filesystem look repaired.

**Do not gate anything on the exit status**, and the two programs are wrong in
different ways:

- **`icheck` returns 0 whatever it finds.** `nerror` is set only on an open or
  fstat failure. Measured: an image with one duplicated block and two
  out-of-range addresses exits 0.
- **`dcheck` returns a raw count of bad entries**, through an 8-bit exit status.
  Measured: 254 bad entries exits 254, **320 bad entries exits 64**, and exactly
  256 exits **0**. So a suite gating on `rc != 0` passes on a thoroughly corrupt
  filesystem, and the more corrupt it is the likelier that becomes.

Both are upstream's behaviour. Every case in `tests/mkfs` reads the output for
that reason. *This section first said "neither program sets its exit status",
which is true of `icheck` and false of `dcheck`; the audit measured it.*

**`dcheck` does not terminate on a superblock claiming more than 65535 inodes.**
`ino` is `ino_t` (16-bit) and `nfiles` is `unsigned`, so the guard
`if (ino >= nfiles)` never fires and the outer loop has no other exit. Measured
on such an image with a 20-second deadline: 6.7 million lines of `read error`,
block number past 106 million on a 110,000-block filesystem, killed by SIGALRM.
Faithful to the VAX — both widths were the same there — but it is a hang rather
than a wrong answer, so **anything that runs `dcheck` on an image it did not
make needs a deadline**, the way `tests/wavec` uses `perl -e 'alarm'`. Nothing
in the suite does today; every image it points `dcheck` at, it built.

`icheck` has the same width in a quieter form: `mino` at `icheck.c:159` is
`ino_t` and takes an `int` product, so a superblock claiming 79968 inodes gives
14432 and the scan **silently stops there**. Measured: a bad address planted in
inode 14000 is reported and an identical one in inode 15000 is not, with no
diagnostic. Also faithful, and note what protects you — `mkfs` caps the i-list
at `MAXISIZE/NIPB`, or 65520 inodes. **Sixteen inodes of margin**, and only for
images this port made; a foreign or corrupt superblock is exactly what a checker
is pointed at.

**`icheck -s` opens the file O_RDWR before it knows anything about it**
(`icheck.c:124`), and a V7 superblock has no magic number. Measured: 200 KB of
`/dev/urandom` with a plausible `s_isize`/`s_fsize` planted at offset 1024 had
blocks 1 and 17 rewritten. The interlock at `icheck.c:470` is real — a short
read clears `sflg` and prints `No update` — but it only saves you when the bogus
`s_fsize` reaches past end of file. Upstream, and unchanged; what changed is the
shape of the exposure. On V8 the argument was `/dev/rrp0a`. Here it is an
ordinary file in a directory of ordinary files.

## The second patch: `time()` writes eight bytes into a field I narrowed to four

`icheck.c:525` is upstream's `time(&sblock.s_time)`. `s_time` is a *disk* field,
so `<sys/filsys.h>` narrows it to the four bytes a VAX gave it, while `time_t`
stays eight because it crosses the shim seam. Measured: the overflow lands on
`s_tfree`, at offset 220 against `s_time`'s 216.

It is harmless today and **twice over**, which is exactly why it is fixed rather
than noted: the high half of a current `time_t` is zero, and `s_tfree` is
assigned zero two lines further down anyway. Both protections are accidents. The
first expires in 2106; the second the moment someone reorders two statements.

Swept, because a global narrowing is the kind of change that reaches past the
file it was made in — this is the fourth thing it touched, after `ltol3`,
`l3tol` and `icheck`'s `long *p`:

```bash
grep -rn 'time(&' src shim
```

Thirty-seven calls, and this is the only one whose argument is a field this port
narrowed. The rest take a `time_t`, a `struct stat`'s `st_mtime`, or `utmp`'s
`ut_time` — all eight bytes here, and all with both ends inside this port.

**The general rule, now on its second confirmation:** when you narrow a type at
the seam, sweep for what already encodes the old width — raw pointers first,
because subscripting is self-correcting, and then anything that takes the
*address* of the narrowed thing, because a callee's idea of the width is not
visible at the call.

### The sweep above was ONE-DIRECTIONAL, and importing one program falsified it

Left standing because the rule it states is right and the *search* was not.
`grep -rn 'time(&'` looks for the **write** direction only. The same seam has a
read direction — a narrowed field's address handed to something that
*dereferences* eight bytes — and `ctime(&`, `localtime(&`, `gmtime(&` are
spellings that pattern cannot match.

`fsck` (§8a step 4d, `src/cmd/fsck.PORTING.md`) brought both: a second
`time(&superblk.s_time)`, and `ctime(&dp->di_mtime)` in `pinode()`. The read one
is far worse than anything here. `di_mtime` is followed by `di_ctime`, so
`ctime()` reads a time about 7.3 × 10¹⁸ seconds in the future and `gmtime()`'s
year loop counts towards it one year at a time: **a live lock with an empty
stdout**, in the only program in this port that writes to filesystems.

Two things generalise, and the second is the one to carry.

- **A sweep is a statement about a tree at a moment.** "Thirty-seven calls, and
  this is the only one" was true when written and false the day `fsck.c` landed.
  Where the property matters, it belongs in a suite: `tests/mkfs` now asserts
  `s_tfree` against icheck's walked free count on a repaired image, which is a
  claim about behaviour rather than about a grep.
- **The pattern to sweep is not `time(&`.** It is *any callee reached through
  the address of a narrowed field*. Re-run both:

  ```bash
  grep -rnE '(ctime|localtime|gmtime|asctime)[ \t]*\([ \t]*&' src shim compiler
  grep -rnE '(^|[^a-z_])time[ \t]*\([ \t]*&'                  src shim compiler
  ```

## Not changed

- `icheck -s` rebuilds the free list and **writes to the image**. Nothing in
  the suite uses it; a test that did would need its own copy. See the O_RDWR
  note above for what it will do to a file that is not a filesystem.
- **`icheck.c:84` writes one past `daddr_t blist[500]`** when 500 or more `-b`
  numbers are given, and **LP64 accidentally defused it**. Measured: `_blist`
  ends at `0x1000127D4` and the next common symbol `_bmap` is at `0x1000127D8`,
  so the write lands in four bytes of alignment padding that exist only because
  a pointer is 8-aligned here. On the VAX both were four-byte and four-aligned
  and this landed on `bmap` itself — a wild pointer on the first `duped()`.
  Left alone: it is upstream's, it is unreachable below 500 arguments, and the
  padding is not going anywhere.
- **`dcheck.c:229` is an off-by-one** — `if(i > NINDIR(dev))` should be `>=` —
  so at `i == 256` it returns uninitialised stack as a block number, which is
  then read and parsed as directory entries. Needs a 267-block directory,
  17,088 entries. Upstream, unreachable here, recorded so it is not rediscovered.
- **`dev` is host noise for a device node.** `dev = makedev(0, bigflag)` is
  applied only when the argument is a regular file; for a block or character
  special, `dev` keeps macOS's `st_rdev` truncated to V8's 16-bit `dev_t`, and
  `BITFS(dev)` is then *bit 6 of a macOS device number*. If set, `BSIZE` becomes
  4096 and every offset in the program is wrong. On the VAX that bit **meant**
  "bitmap filesystem"; here it is arbitrary. Not reachable from the suite, which
  only ever passes a regular file, and both man pages' synopses say
  `/dev/rrp0a`.
- `icheck -b` takes up to `NB` 500 block numbers to report on, `MAXFN` 500 is
  unused, and `struct dinode itab[BIGINOPB*NI]` is 64×4 = 256 inodes = 16 KB.
  All fine at VAX widths and all of them fine here now that `sizeof(dinode)`
  is 64 again.
- The 16-bit `ino_t` ceiling applies to these as it does to everything reading
  this format. An image with more than 65535 inodes cannot be described, which
  is the format's limit and not the port's.

## `-b` and `-i` read past the end of argv, and this file walked past it

`icheck -b 5` and `dcheck -i 5` — the option's last number also being the last
argument — SIGSEGV'd on `n = atol(argv[1])`, where `argv[1]` is the NULL the
kernel plants at `argv[argc]`. On the VAX that read the `0x00` at address 0,
found no digit, returned 0 and broke the loop.

**The loop is byte for byte `ncheck.c`'s, and this file audited it without
seeing the null.** The section above examines that exact `for` for a
`blist[500]` overrun and stops one line short. Three copies of it exist in the
tree — `ncheck -i`, `icheck -b`, `dcheck -i` — and when the fault was found in
the first, the note was filed under *that program* instead of under the shape,
so the other two kept crashing for as long again.

Patched the way `ncheck` was, `n = argv[1] == 0? 0L: atol(argv[1])`, which
reproduces the VAX's answer rather than merely guarding: `atol` returned 0 and
the loop ended, which is exactly what a `break` would look like from outside and
is *not* the same thing from inside, because the byte was still consumed.

`tests/mkfs` §9 carries both, each paired with a case asserting the option still
works — `dcheck -i 2` must still name all three references to the root. A "fix"
that stops the loop consuming anything passes the crash case and fails that one;
mutation-verified in that form.

## Still open

- **`fsck`** — 1925 lines, and the one that repairs rather than reports. It is
  what would put the directory entry `clri` leaves behind into `lost+found`.
- **`mklost+found`**, which the `mkfs` man page says should run immediately
  after `mkfs`. It needs a mounted filesystem, so it waits for step 5.
- Nothing here has been read by a real V8 kernel. That is step 6.
