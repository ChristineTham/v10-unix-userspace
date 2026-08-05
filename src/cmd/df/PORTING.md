# df

Runs, and its numbers agree with the host's `df -k`. Two changes to authentic
source, both recorded below; `odf.C` is upstream's older version and is not
built, exactly as upstream's own makefile leaves it.

```
$ df
dir                             dev       kbytes   used   free %use
/System/Volumes/Data            disk3s5   482797652 428181808 54615844  89%
/System/Volumes/Hardware        disk1s3   512000  18036 493964   4%
...
```

## 1. `argc >= 1` read an `argv[1]` that was not there

```c
while (argc >= 1 && argv[1][0]=='-') {
```

With no arguments `argc` is 1, so the test reaches `argv[1]` — the NULL that
terminates the vector — and dereferences it. With `df -i` it happens on the
*second* pass: the flag is consumed, `argv` is advanced, and `argv[1]` is the
terminator again. **Every invocation of this program crashed here**, which is
why the segfault arrived before any output at all.

On the VAX address 0 was inside the text segment and readable, so `*(char *)0`
returned a byte of the program, compared unequal to `'-'`, and the loop simply
ended. macOS keeps page 0 unmapped.

Third instance of the class in this port, after `refer5.c`'s
`prefix(".[", lookat())` and `grap`'s. `argc > 1` is the fix, one character.

## 2. The superblock comes from libkmemu, not from block 1 of a disk

The original opens `/dev/<device>` and reads block 1 as a `struct filsys`.
There is no such block here: a macOS volume has no `filsys` anywhere on it, and
opening the raw device needs root. PLAN.md §7 sanctions this one — *"df via
statfs backend, V8 output format"*.

**Why not the `who` treatment.** `who` needed no changes because the shim
manufactures the *file* it reads. The same trick here would mean manufacturing a
fake *disk*: a device file with a superblock at offset 1024, plus
`stat("/dev/x").st_rdev` made equal to `stat(mountpoint).st_dev` so df's
mount-point lookup matches, plus `/dev/` added to the jail's redirect list. And
`df -l` walks the free-block *list* out of that superblock, so it would need a
fabricated free list too — inventing data rather than reporting it, which is the
one thing this port will not do to make output look right.

So `dfree()` fills `sblock` from `kmemu_fsstat()` and everything downstream is
untouched: the arithmetic, the field widths, the `printf`, the `-i` columns are
all V8's. `dev` is 0 because bit 64 clear is what selects `BSIZE` 1024 and
`INOPB` 16, the units libkmemu answers in.

`df -l` now prints `bad free count` — df's own words for "there is no free
list", which is true. Left that way deliberately.

## What the format cannot hold, and what that costs

`s_isize` is `unsigned short` and `ino_t` is `u_short`, so the i-list size and
the free-inode count are **16-bit**. This volume has 548 million inodes. A V7
superblock cannot describe it and neither could a V8 `df`.

`df -i` therefore reports the format's ceiling — 65535 — rather than a number
somewhere in between, which would be the plausible lie. `tests/kmemu` asserts
the ceiling exactly, so a future change that starts scaling or truncating into a
believable-looking figure fails.

Block counts are fine: `s_fsize` and `s_tfree` are `daddr_t`, which is `long`.

## What `used` means on APFS, which is not what it says

Every volume in an APFS container reports the **container's** `f_blocks`, so
`/`, Preboot, VM and Data all show the same 482 GB total, and df's
`used = blocks - free` is container-wide rather than the volume's own. The
host's own `df` gets per-volume usage from `getattrlist`, which is not a number
any superblock ever held.

df's arithmetic is authentic and its inputs are honest; the two together say
something APFS does not mean. Recorded rather than papered over by inventing a
per-volume total `statfs` did not report.

## /etc/fstab is manufactured too, and that displaced an authentic file

The rootfs carried **upstream's own `/etc/fstab`** — Bell Labs' real one,
listing `/dev/ra00` through `/dev/ra23`. Phase 6c installed it so `/etc` reads
as genuine, and as a museum piece it is exactly right.

`df` does not only read `mtab`. `devlen()` walks `fstab`, **merges** any entry
not already mounted into the mtab array, and takes both column widths from it.
So with the authentic file in place df printed rows for `/usr1`, `/fsave`, `/v8`
and `/sys` — disks belonging to a VAX in New Jersey in 1985 — `statfs`'d
whatever those paths happen to resolve to here, and cut every real mount point
to ten characters because `/usr/spool` was the longest name it had seen.

An fstab describes the filesystems *this* machine has. Keeping Bell Labs' makes
df report a machine that is not this one, which is the same kind of plausible
lie as a fabricated `WCHAN`. The original is still in `third_party/`, and in the
tree at `v8/etc/fstab`.

Mounts that are not devices — devfs, automounter maps, SMB shares — are written
with type `xx`, which upstream's own `fstab.h` defines as "ignore totally". Its
word, not one invented here.

## The bug that cost the most: mtab fields are terminated, utmp's are not

Same width, same fixed-size array, opposite rule.

`df` does `strcpy(&specbuf[5], mtab[i].spec)` into a 38-byte buffer. A 32-byte
field filled to the brim with no terminator — which is exactly what
`kmemu_field` produces, correctly, for utmp — runs on through the *next* record
until it finds a NUL, overflows `specbuf`, and smashes whatever static follows
it. Here that was the digit buffer `ecvt` hands to `printf`, so the `%use`
column started emitting hundred-digit strings **several rows after** the row
that caused it:

```
/System/Vo disk 512000  18036 493964 41620729767668150000000000000000...%
```

Found by printing what df was about to convert: the integers were all correct
and only the conversion was wrong, and `file` had become
`/dev//Library/Developer/CoreSimulatordisk5s1` — two fields run together, which
named the bug.

So the rule is per-file rather than per-format: `who` reads utmp with `%-8.8s`
and `strncmp` and must **not** see a terminator on a full field; `df` reads mtab
with `strcpy` and must.

`tests/kmemu` checks both the terminator byte and the symptom. The byte check is
host-dependent — it only bites when a mount name is exactly 32 characters, which
none is here — so it was the *symptom* check that caught the mutation. Worth
keeping both.

## Still open

- Two rows print with a blank `dir` column, where `mpath()` finds no mtab entry
  whose `spec` matches. They are real mounts with real numbers, so the output is
  not wrong, only unlabelled. Not chased yet.
- `df /some/path` with an explicit argument is untested.
