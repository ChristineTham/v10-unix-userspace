# find(1)

Imported in Wave A2. Four changes, all forced by the target, and the first is
the port's dominant bug class arriving in eleven places at once.

## 1. `struct anode` punned as ints — every predicate but `-name` was false

`find` builds an expression tree of

```c
struct anode { int (*F)(); struct anode *L, *R; } Node[100];
```

and each predicate reads its own operands by casting the node to a local
struct: `type()` uses `struct { int f, per, s; }`, `size()` uses
`{ int f, sz, s; }`, and so on for eleven functions.

On a VAX all three members are four bytes and the pun is exact. Here they are
**pointer-sized**, so the second field lands at offset 4 — the upper half of
the function pointer — and the third at 8, which is `L`.

Every predicate that reads its second field was therefore silently false:
`-type -perm -links -size -user -group -atime -mtime -ctime -exec -ok`.
Measured: `find /usr/lib -type f -print` printed nothing while `-print` alone
found 83 entries.

**`-name` is the exception and it is an accident.** Its pun is
`{ int f; char *pat; }`, and the pointer's own alignment pushes `pat` to
offset 8, where `L` really is. So the one predicate anybody tries first is the
one that works, which is why this survived import, compilation and a smoke
test.

Fixed by changing `int` to `long` in the eleven puns — upstream's idiom kept,
with the width the target forces. Verified against the host: `find /usr/lib
-type f` now returns **74**, the same as the Mac's own `find`.

## 2. The directory loop was bounded by `lstat`'s `st_size`

Upstream reads a directory in blocks, bounded by `offset < dirsize` where
`dirsize = Statb.st_size` from the `lstat` at the top. Exact on a V7
filesystem, where a directory's size *is* its records.

Not exact here. The shim builds V7 records out of the host's variable-length
entries, so the two numbers are unrelated — nine entries is 2304 bytes of
records where APFS reports 288 — and only **fstat** reports the snapshot
length. `stat` and `lstat` deliberately do not: `shim/v8sys/dir.c` records that
doing it for `stat(2)` would put a `getdirentries` loop inside every `ls -l`.

So `find` computed one entry from 288/256, skipped it as `.`, and reported an
empty filesystem.

The loop ends on a short read now. That needs neither number and cannot
disagree with either, and it drops the lseek arithmetic's dependence on a fixed
block size.

## 3. `dsize>>4` and `i<14` are DIRSIZ 14 written as literals

`entries = dsize>>4` divides by `sizeof(struct direct)` at the 1985 value; this
port's is 256. And the name copy is bounded by a literal 14.

Both now name the constants. **Swept**: `find.c` is the only file in `src/cmd`
with a hardcoded record size or name width — every other raw directory reader
(`du`, `ls`, `rm`, `tar`, `news`, `ncheck`, `dcheck`, `dumpdir`, `restor`,
`fsck`, `mkfs`) uses `DIRSIZ` symbolically and follows whatever the port sets.

## 4. `Pathname[200]` and `Home[128]` are 1985 buffers sized against DIRSIZ

The same class as `mv`'s `MAXN`, `mkdir`'s `pname[128]` and `rmdir`'s
`name[500]`: a path assembled one component at a time, where a component can
now be 255 bytes. Both raised to 1024.

**And that broke a relationship, which is `mv`'s lesson.** The `-cpio` output
does `strcpy(hdr.h_name, Pathname)` into a `char h_name[256]` — safe by
construction while `Pathname` was 200, and an overflow the moment it was not.
`h_name` is an **archive format field** and cannot grow with it, so the copy is
bounded and a name that will not fit is **reported and skipped** rather than
truncated: a short name in a cpio archive names a different file. That is
`mtab.c`'s rule — an entry that cannot be described truthfully must not be
described at all.

## Still open

`-type L` maps to `S_IFLNK`, and the shim's `lstat` reports symlinks
correctly, but the rootfs contains **zero** symlinks (measured), so that arm is
unexercised here.
