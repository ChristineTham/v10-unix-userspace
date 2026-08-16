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

## The blank `dir` rows, chased: a truncated path is not a shortened name

This section used to say two rows printed with a blank `dir` column, that they
were "real mounts with real numbers, so the output is not wrong, only
unlabelled", and that it had not been chased. Both halves of that were wrong.

The cause was `/etc/mtab`'s 32-byte field. `dfree()` opens with

```c
if (stat(file, &stbuf) == 0 && (stbuf.st_mode&S_IFMT) == S_IFDIR)
        ... find the mount point's device ...
else if (strncmp("/dev/", file, sizeof "/dev/" - 1) != 0)
        strcpy(&specbuf[5], file), file = specbuf;
```

so it *branches on the mount point resolving*. Truncated to
`/Library/Developer/CoreSimulato`, it does not, and df takes the arm that
assumes the string names a device: `mpath()` then matches nothing, printing an
empty `dir`, and `file + sizeof "/dev"` prints the path's first `DEVNMLG`
characters into the `dev` column — which is where `/Library/` came from. The
numbers were not "real" either; they were `statfs` of a path that does not
exist, which happened to fall back to the enclosing volume.

`FSNMLG` is 1024 now — `MAXPATHLEN`, the host's own width for this field, after
128 turned out to be too small for a 154-character Siri asset bundle on a CI
runner — and anything that still will not fit is reported and
dropped rather than truncated. `src/include/PORTING.md` has the reasoning and
`shim/libkmemu/NOTES.md` the shim side; `tests/kmemu` asserts that every mount
point in mtab is a directory that exists, which is the invariant that broke.

## Still open

- **`/` is labelled `/System/Volumes/Data`.** Not the jail, and not this port:
  `stat("/")` and `stat("/System/Volumes/Data")` report the **same `st_dev`** on
  the host as well, because APFS puts them in one volume group. `dfree()`
  identifies a mount point by matching `stat(mountpoint).st_dev` against
  `stat("/dev/<spec>").st_rdev`, takes the first match, and cannot tell the two
  apart. A V8 machine had one device per filesystem, so the question never
  arose. Fixing it means identifying by mount point rather than by device,
  which is a change to df's algorithm rather than to its inputs.
- **`/dev` is labelled `/System/Volumes/Data` too**, and that one *is* the jail:
  `rootfs/dev` exists (libkmemu writes `/dev/kmem` there, and `/dev/dk`,
  `/dev/pt` and `/dev/drum` are build targets for `ps`), so `stat("/dev")`
  inside the jail reports the volume the rootfs lives on rather than devfs.
  Same root cause as the row above — identification by device number — with the
  jail supplying the collision instead of APFS.
- `df /some/path` with an explicit argument is untested.

## Why df fails rung 5, and why it is not the same failure as load and w

`df` is not in `tests/jail`'s rung-5 sweep. PLAN.md grouped it with `load` and
`w` as "blocked on `libkmemu`, which upstream never had", and that description
flattens a distinction worth keeping.

`load` and `w` **are** in the sweep and **do** build. Their source is upstream's,
unmodified; upstream's makefile links it; the result is a real program that
cannot answer at runtime and says `No mem`. Rung 5 is a claim about the build
*description* being Bell Labs', not about the binary being the installed one,
and those two are where that distinction stops being academic.

`df` fails earlier and for the opposite reason. **This port changed `df.c`** to
call libkmemu directly:

```c
struct kmemu_fs { long blocks, bfree, files, ffree; };
int kmemu_fsstat();
...
if (kmemu_fsstat(mp0, &kfs) < 0) {
```

`grep -c kmemu third_party/.../v8/usr/src/cmd/df/df.c` is **0**. The call is
ours. So upstream's makefile is not insufficient — it is describing a program
that no longer exists, and the link dies on an undefined `_kmemu_fsstat`.

The distinction generalises, and it is the useful part: a **source** change made
for this port can break the rung-5 claim for a program whose makefile was
perfectly adequate. `load` and `w` kept the claim precisely because they were
left alone and allowed to fail honestly at runtime instead.

Closing it is not a makefile change, and it is **not** the `/dev/kmem` question
either — that was the obvious guess and reading `dfree()` refutes it. Upstream
opens the *device* and reads block 1:

```c
	fi = open(file, 0);
	...
	bread(1L, (char *)&sblock, sizeof(sblock));
	blocks = (long) sblock.s_fsize - (long)sblock.s_isize;
	free = sblock.s_tfree;
```

`struct filsys` off the raw disk. So an unmodified `df` needs a **real V8
filesystem image** behind `/dev/<spec>` with a valid superblock in block 1 —
which is PLAN.md §8a **step 4** (mkfs and a raw image), not step 3's `/proc`
and not libkmemu at all.

### That paragraph used to end "it will close as a side effect of step 4", and step 4 has landed and it did not

Worth leaving the prediction visible, because it was reasoned rather than
measured and it was wrong in a characteristic way — it named the one missing
ingredient and assumed nothing else was in the path.

`mkfs` now writes a correct superblock (`src/cmd/mkfs.PORTING.md`), and `df`'s
rung 5 is exactly as blocked as it was. Two things are in the way and only one
of them was foreseen:

1. **The port's change is a replacement, not a supplement.** `dfree()` above
   does not read a superblock and then override it; the `open`, the `fstat` and
   the `bread` are all gone. Even handed a perfect image, this `df` would not
   look at it. That is a fact about the diff, and re-reading the diff is what
   settles it — the same move as running a makefile before recording a program
   as blocked.
2. **An image in a file is not a filesystem behind `/dev/<spec>`.** Upstream's
   `df` reaches the superblock down one of two paths: an explicit `/dev/...`
   argument, or a mount point whose `stat().st_dev` matches some
   `stat("/dev/<spec>").st_rdev`. The first needs the image *named* in the
   jail's `/dev`; the second needs it *mounted*, which is §8a **step 5**.

So the honest revision: step 4 changed this from **blocked on data that has to
be invented** to **contained**. The numbers a V8 `df` wants now exist somewhere
real. What is left is naming and mounting, and the decision that goes with it —
whether the installed `/bin/df` should answer about the Mac (as it does, which
is what PLAN §7 sanctioned) or about a V8 image, because with upstream's source
it cannot do both.

Worth separating from `w`, which *is* the `/dev/kmem` question
(`src/cmd/w/PORTING.md`). Two grovelers, two different data sources, and
grouping them under "needs kernel memory" would have sent the work in the wrong
direction — the same mistake as assuming `ps` wanted `libproc` when it is a
`/proc` client.

## The garbled row had a SECOND cause, and widening the field could not fix it

`FSNMLG` 32 → 1024 cured the truncation. It did not cure the symptom, which
came back the moment the host mounted a Time Machine snapshot and an SMB share
mid-test:

```
dir                                                  dev       kbytes   used   free %use
                                                     /Volumes/ 46253757308 35674958276 10578799032  77%
```

Empty `dir`, and the mount point's first nine characters in `dev` — the exact
shape `/Library/` produced before, from a completely different cause. The path
this time is **not truncated**. It is 100 characters, stored whole, and
correct:

```
/Volumes/.timemachine/NAS2(TimeMachine)._smb._tcp.local./7AB74491-.../Backup
```

What fails is the `stat`:

```
$ stat '/Volumes/.timemachine/NAS2(TimeMachine)._smb._tcp.local./…'
stat: Permission denied
```

`getfsstat(2)` reports the mount, so it genuinely exists; the process just
cannot stat it. `dfree()` branches on `stat(file)` succeeding and takes its
"this string is a device name" arm when it does not — so **one symptom, two
causes**, and the first fix taught nothing about the second.

Note which mounts do *not* need the fix, because it sharpens what the rule is.
`/System/Volumes/Data/home` (autofs) and the snapshot volumes stat fine and
merely have no `/dev` node; `df` prints upstream's own
`mounted on unknown device` on stderr and moves on. That is an honest answer
from a 1985 program meeting a filesystem it has no vocabulary for. **The
discriminator is the stat, not the device.**

`unstattable()` in `shim/libkmemu/mtab.c` drops such a mount and says so, by the
same rule and for the same reason as `toolong()`: an entry that cannot be
described truthfully is better absent-and-announced than present-and-wrong. It
is applied to **both** files, because `devlen()` merges any fstab entry not
already in mtab and would otherwise hand the dropped mount straight back.

A raw `SYS_stat64` rather than `stat(3)`: `tests/kmemu` asserts libkmemu's libc
surface symbol by symbol, and a seventh import is a decision to make
deliberately rather than smuggle in behind a bug fix.

### What this says about the sweep

The suite-wide sweep for host assumptions had finished an hour earlier and could
not have found this: **the host did not have these mounts at the time.** Two of
the three failures were test assumptions of exactly the class swept for — one of
them in the very block whose comment already records learning this lesson once,
where the qualifier added then was about path *length* and the assumption left
standing was that a mount in the window is one `df` can put in a table at all.

A sweep is a snapshot of the machine it ran on. That is not an argument against
sweeping; it is the reason the fixes must assert relations rather than values.

## The row's identity came from one place and its numbers from another

Found by moving the repository to a second volume, which is the only condition
that can expose it — and it had been wrong for the life of the port.

`dfree()` is handed a mount point.  When that is a directory it walks the mtab
looking for the entry whose device number matches, and on a match it rewrites
`file` to `/dev/<spec>`; both display columns then come from that entry (the
`dev` column is the spec, the `dir` column is `mpath(file)`).  The **numbers**
were still taken from `mp0`, the path the caller passed in.

Inside the jail those two are not the same thing.  `stat("/")` resolves to
`$V8ROOT`, so the `/` row is *correctly* labelled with the volume that holds
the rootfs — and it was reporting the block count of the host's root.  Measured
on a tree living on `/Volumes/Photos`:

```
dir                        dev       kbytes
/Volumes/Photos            disk5s1   1948455240     <- host root's count
...
/Volumes/Photos            disk5s1   976557016      <- what disk5s1 actually has
```

Two rows naming one filesystem and disagreeing by a factor of two.  The fix is
one line — take `mp0` from the entry the loop matched — and it is the
"check which end supplies each operand" rule: the dev and dir columns had
already moved to the matched entry and the numbers had not.

**It was invisible because the repository had always sat on the host's root
volume**, where the caller's path and the matched entry's path name the same
filesystem.  That is the host-property trap arriving in a *program* rather than
in a test, and the test beside it had the same assumption from the other end —
`tests/kmemu` compared one row, chosen as df's own first row, against the host's
figure for that row's device, which agrees only under the same condition.  It
now checks **every** row by mount point, which is a relation this port controls;
re-running the old defect against it names two bad rows rather than one.
