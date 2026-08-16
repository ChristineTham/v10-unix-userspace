# Porting notes for `src/libc/gen`

Per-file notes for this directory. The tree-wide rules are in `CLAUDE.md`, and
`PROVENANCE` records the upstream blob hash of every file here.

## `strncat.C` and `strcatn.c`: the count bounds the READ, and it did not

Both had upstream's loop:

```c
while (*s1++ = *s2++)
	if (--n < 0) {
		*--s1 = '\0';
		break;
	}
```

which copies `s2[n]` and only *then* notices `--n < 0`, overwriting the byte it
just copied with the NUL. So the **output was always correct** and only the read
went one past the bound — which is why it survived every test in the tree, and
why the case that catches it has to hand the function an unreadable pointer
rather than compare a string.

All five callers pass a fixed-width, deliberately unterminated field:
`dumpdir.c:183` and `ttyname.c:62` a directory entry's `d_name`, `who.c:80` and
`w.c:332` `utmp.ut_line`, `w.c:389`/`:555` likewise. Identical in shape to the
`%.Ns` fault in this port's `doprnt.c`, and for the same reason: a count
argument exists *because* the source need not be terminated.

### The authority is upstream's own assembler

`strncat.s` — the code a VAX actually executed — does not do this:

```
	movl	12(ap),r8	# max src length (arg `n')
	bleq	L6		# done if <= 0
	...
	locc	$0,r8,(r7)	# see if there's a null in src
```

It returns without touching `s2` when `n <= 0`, and `locc` scans exactly `r8`
bytes. The `.C` beside it is the *portable reference*, its own header calls it
"the `standard' for the C-library", and it reads one byte more than the code
shipped next to it. **The overread is therefore an artefact of this port
substituting the reference for the assembler**, and removing it restores what V8
ran rather than departing from it.

`strcatn` is the V7-spelled twin with a character-identical body and **no `.s`**,
so there its C is what V8 ran and the same patch is a deviation rather than a
restoration. That difference is recorded in the two comments; the code is the
same. Taken anyway for the reason `sed`'s `trans[]` was.

Replacement, which preserves the observable behaviour exactly — truncate at `n`,
stop early at a NUL, always terminate:

```c
while (--n >= 0 && (*s1 = *s2++))
	s1++;
*s1 = '\0';
```

### The diagnostic

`strncat(buf, (char *)1, 0)` faults on the old loop and never touches `s2` on
the new one — no guard page needed, the same trick `%.0s` uses for `doprnt`.
`tests/libv8c` carries it for both spellings, plus a case that the bound is
still a bound in each direction. **A behavioural test cannot see this class**:
the answer was right the whole time.

## `strncmp.C`, `strncpy.C`, `strcmpn.c`, `strcpyn.c`: checked, and correct

Swept at the same time, since they are the only other routines here that take a
count. All four test the bound before dereferencing — `while (--n >= 0 && ...)`
and `for (i = 0; i < n; i++)` — so `strncat`/`strcatn` were the singleton.

## `getwd.c`: a folded inode does not identify a file, and `pwd` said so

**Unchanged, and the reason it is unchanged is the finding.** `getwd()` walks
to the root, and at each level it looks for the entry in `..` that names the
directory it just came from. Its same-device arm decides that on `d_ino`
alone --

```c
	do {
		if ((dir = readdir(dirp)) == NULL) { ...  goto fail; }
	} while (dir->d_ino != d.st_ino);		/* :62 */
```

-- which is exact on a V7 filesystem, because an inode number is **unique
within a device** there: 16 bits, and a V7 volume could not hold 65536 inodes.
It is not exact here. `v8sys_fold_ino()` (`shim/v8sys/dir.c:309`) maps a
64-bit host inode into V7's `u_short ino_t`, and the same function feeds *both*
sides of that comparison -- `d_ino` in the directory snapshot (`dir.c:467,469`)
and `st_ino` in `stat_translate` (`shim/v8sys/syscall.c:1884`). While that map
was a plain XOR fold, two files in one directory could share a `d_ino` and this
loop stopped on whichever `readdir` yielded **first**. It is a table now, and
the rest of this entry is how that was measured and what it cost.

### Measured — and re-measured after `tests/wavea` finally caught it in the wild

Not inferred from the code. The first measurement entered the ten collision
pairs whose later member was a directory and got **10 wrong out of 10** — 4
printing the colliding entry's name and **exiting 0**, 6 dying with
`getwd: can't change back to .`.

That was a sample of the reachable pairs reported as if it were the population,
and when the suite went red on its own months later the re-measurement was
better: classify **every** directory in `$TMPDIR`, then sample each class.

| | directories | right | named another dir | `can't change back` |
|---|---|---|---|---|
| in a fold-collision group | 121 of 1545 | 32/60 | 6 | 22 |
| not in one | 1424 | **60/60** | 0 | 0 |

A clean separation, and the 47% is structural rather than noisy: the walk stops
at whichever colliding entry `readdir` yields **first**, so the first member of
a group is right and every later one is wrong. Both failure shapes are that one
stop — naming the wrong entry when it is a directory you can `chdir` into, and
failing the final `chdir(pnptr)` at `getwd.c:79` when it is not.

**Two instrument errors on the way, both flattering, both already named in
CLAUDE.md.** Comparing against the *unresolved* path made all 12 samples read as
failures, because `$TMPDIR` sits behind the `/private` firmlink — the fix is
`/bin/pwd -P`. And a collision list truncated to the first 12 of 109 groups
produced a "non-colliding control that failed", which would have falsified the
whole diagnosis had it not been chased. **Classify the whole population before
sampling it.**

The population also grew between the two measurements — 5452 entries / 199
shared folds, then 6031 / 215 — which is what a months-old `$TMPDIR` does and
is the reason the rate is a property of the host rather than of the port.

### The two failed reproductions are worth more than the successful one

**1500 freshly created directories produced ZERO collisions.** APFS hands out
consecutive inodes, and `ino ^ ino>>16 ^ ino>>32 ^ ino>>48` separates
consecutive values perfectly, so N directories made back to back occupy one
contiguous band of N buckets rather than sampling the space. Creating 400 more
inside the real `$TMPDIR` also missed, for the same reason. **Collisions need
inodes spread over time**, which a months-old `$TMPDIR` has and a freshly built
test tree structurally cannot -- which is exactly why `tests/wavea`'s `pwd`
case fires roughly never, and why the churn experiment recorded there
(`tests/wavea/run.sh`, 400 walks against a directory being churned) could not
have found it. It was churning entries it had just made.

### Why the fix is not in this file, and an attempt that had to be withdrawn

The obvious patch is to confirm the candidate with `stat()`, and it **does not
work**. `stat()` returns the folded inode too, so a colliding file answers with
the *same* `st_ino` and the *same* `st_dev`, and the check passes. Written,
then withdrawn.

`ttyname.c:59-65` is the proof that this is not an oversight of upstream's:
there they pre-filter on `d_ino` and then `stat()` the candidate and compare
`st_dev`/`st_ino` -- the careful version of the same idiom -- and **it is
equally defeated here**, for the same reason. Two files that the shim maps to
one `(dev, ino)` pair are indistinguishable to a V8 program *by construction*.
No consumer-side change can separate them.

So no *consumer-side* change can fix this, which is why `getwd.c` is
unmodified; the note it corrects is CLAUDE.md's own 16-bit table, which
recorded a folded `d_ino` as "wraps; harmless *except* the value that wraps to
0". Measured, the wrap to 0 is the one case that **was** already handled
(`fold_ino` never returns it); the collisions it called harmless are not.

It was recorded here as a **structural limit of the fidelity contract**, and
that was too strong by one word: what is structural is that *no pure function*
of a 64-bit inode is injective into 16 bits. The map does not have to be a
pure function. It has to be *stable*, which is a weaker thing, and the fix
below is the difference between the two.

### What the fix is: a table, because stability is weaker than purity

Landed in `shim/v8sys/dir.c`, entirely inside `v8sys_fold_ino()` — the
signature and all three call sites are unchanged. The fold now *proposes* a
number, and if a different host inode has already claimed it the next free one
is taken instead, recorded in a process-local table that is **append-only**: an
assignment is never revised and never evicted.

Append-only is what makes the ordering irrelevant, and the ordering is what
killed candidate (a) below. `getwd` takes `stat(".")` *before* it opens `..`; a
table that never changes an answer gives the same number whichever call arrives
first, so the snapshot no longer has to know what the caller already holds.

**The constraint recorded against this was half false, and the false half was
load-bearing.** What stands: the map must be *stable within a process*. What
does not: it was written here that folded values "are written into files
(`shim/libkmemu/NOTES.md:247` — `e_tdev` in the manufactured `/etc/utmp`) that
another process reads", and that was used to rule out any order-dependent
scheme. Three things wrong with the sentence, measured:

- **`v8sys_fold_ino` has exactly three call sites** — `dir.c`, twice, and
  `syscall.c:1884`. Nothing in `libkmemu` calls it; `grep` found it in
  `procfs.c:169` and `:603`, and both are **comments**.
- **The cited note says the opposite.** `NOTES.md:247` is about `u_ttyino` in
  `/proc`'s u-area, which is "left zero"; filling it *would* be a stat folded
  through `v8sys_fold_ino`, and it explicitly "buys nothing until those nodes
  exist". A hypothetical, read as a fact.
- **`/etc/utmp` has no inode field at all.** V8's `struct utmp` is
  `{char ut_line[8]; char ut_name[8]; long ut_time;}` — 24 bytes — and
  `libkmemu/utmp.c` writes those three and nothing else. `e_tdev` appears
  nowhere in any source file.

That is the third instance in this repo of a sweep matching the *documentation*
of a thing and counting it as the thing. It cost more here than the other two,
because it stood for months as the reason not to try.

### Measured, before and after, on one host and one population

Re-measured minutes apart against the same `$TMPDIR`, which is the only place
these numbers can come from: APFS hands out consecutive inodes and the old fold
separated consecutive values perfectly, so a freshly built test tree collides
zero times no matter how large it is.

| | the old fold | the table |
|---|---|---|
| distinct v7 values for 6729 entries | 6210 | **6729** |
| entries sharing a value with another | 519, over 257 values | **0** |
| `pwd` right, inside a collision group | 32 of 60 | **149 of 149** |
| `pwd` right, every directory in `$TMPDIR` | — | **1752 of 1752** |

`pwd` was already right 60/60 *outside* a collision group, so the separation
was clean in both directions and the fix closes the failing half without
disturbing the other.

**What it costs shows up in the same measurement.** Of the 6210 entries whose
fold was already unique, **6205 keep exactly the old number and 5 do not** — a
colliding entry seen earlier probed onto a free value, and that value turned
out to be the fold of an entry not yet seen. Inherent to assigning without
knowing the future, so "unchanged where it can be" is 99.92%, not 100%.

The remaining departure is the honest one: **which of two colliding inodes gets
the fold depends on which was seen first**, so two processes can disagree about
a colliding inode — 519 of 6729 entries here, 7.7%, are eligible. Against a
`pwd` that named another directory and exited 0 that is the right trade, and
nothing in the live tree consumes an inode number across processes: `ls -i`
prints one, and `find` is not ported at all (there is no `src/cmd/find`).

### The three candidates, and why (b) won

**(a) Disambiguate at the directory snapshot.** The shim sees every entry at
once and could resolve a clash by a rule over the set of inodes present.
**Killed by ordering**, measured rather than suspected: `getwd` calls
`stat(".")` before it opens `..`, so it already holds the unperturbed value
when the snapshot perturbs. Worse than a partial fix — if the cwd is the entry
that gets perturbed the loop matches *nothing*, so a rule repairing the half
that fails today breaks the half that works by luck. For `stat_translate` to
compute the same perturbation it would have to know the parent's inode set,
i.e. scan the parent on every `stat`, which is the `getdirentries`-inside-`ls
-l` cost `v8sys_dirsize` already refused.

A cheap variant — give the later colliding entry `d_ino = 0` — is worse than
the disease: 0 is V7's deleted-entry marker, so `readdir` skips it and **the
file vanishes from `ls`**.

**(b) Order of first sighting.** Taken, in the narrowed form above: not a
counter from 1, which would make *every* inode number process-local, but the
fold with a probe only where the fold is contended. The recorded difficulty was
*bounding* the table, and it dissolved once eviction was ruled out entirely —
a table that never shrinks cannot become inconsistent, it can only fill. At
65535 distinct inodes it stops recording and returns the plain fold, which is
the old behaviour and is still stable because the fold is pure. `tests/v8sys`
drives that path with 65535 synthetic inodes and asserts that an assignment
made before the wall is still honoured after it.

**(c) Widen the identity.** Still the correct fix and still the expensive one.
`ino_t` is `u_short` (`sys/types.h:86`) and appears in two **on-disk** records
— `struct direct.d_ino` and `struct filsys.s_inode[]` — so it cannot move
globally. Per-field narrowing is what `sys/ino.h` and `sys/filsys.h` already do
for `time_t` and `off_t`, so the machinery exists; but `struct direct` is
on-disk for `$(IMGBIN)` and in-memory for the live emulation, which would make
`d_ino` a second `DIRSIZ`: two widths behind one `-D`, with the whole warning
that entry carries. And a 4-byte `d_ino` moves the record to 258 bytes, which
the 44 raw directory readers survive only if every one of them uses
`sizeof(struct direct)` rather than a literal.

### What is still open

Two things, both narrow, both written down rather than fixed.

**The synthetic filesystems assign outside the map.** `/proc` hands out
`ROOTINO` 2 and `pid + PRMAGIC` (`shim/libkmemu/procfs.c:887` and `:891`), and
`/dev/fd` hands out `minor + 1` (`shim/v8sys/vfs.c`). Neither consults the
table, so a real file can be given the same v7 inode as a `/proc` entry. That
is correct as it stands — V7 identity is the `(dev, ino)` pair and those
filesystems report a different `st_dev` — but it is correct by an argument
rather than by construction, and a fourth filesystem type should be asked the
same question. Reserving their ranges in the claim bitmap would cost 130
values out of 65535 and buy construction instead of argument.

**`st_dev` WAS narrowed by truncation, the pair leaned on it harder than it
looked, and it had ALREADY STOPPED BEING INJECTIVE — which this paragraph said
was fine.** It used to read: *"15 mounted filesystems, whose truncated devs are
`0003 0005 0007 0008 000e 0010 0011 0012 0017 001b 001f 0023 0026 88d9` — all
distinct, so the pair* is *injective today,"* qualified with the honest caveat
that this was *"because macOS keeps APFS volume minors small, not by
construction."*

**Count the values it lists. There are fourteen, for fifteen filesystems.** A
list of 14 numbers cannot be the distinct images of 15 inputs, so the sentence
refuted itself in its own evidence — the same shape as the `64/768` directory
pair, where two plausible numbers side by side read as one measurement and were
arithmetically impossible together.

Re-measured, and the caveat had come true. `stat_translate`'s `& 0xffff` reads
like a narrowing into V8's 16-bit `dev_t` and is not one: Darwin packs the major
at **bit 24** and V8 at bit 8, so the mask kept the minor and deleted the major
outright. Over this host's mount table — 15 mount points, **14 distinct
filesystems**:

| | distinct `st_dev` |
|---|---|
| `& 0xffff` | **13** — `/Volumes/Cloud` (major 54, minor 7) and an APFS volume (major 1, minor 7) both become `0x0007` |
| `makedev(major(h), minor(h))` | **14** — injective |

Two filesystems the host calls different were one filesystem to every V8 program
on this machine, today. `syscall.c`'s `hostdev()` re-encodes into V8's own
layout now (`syscall.c:1883` for `st_dev`, `:1901` for `st_rdev` — **both**, since
`df.c:142` and `fsck.c:435` compare one against the other). The exact counts
above are properties of this Mac's mount table; what the port controls, and what
`tests/v8sys` asserts, is that **distinct host devices stay distinct**.

And the pair is doing real work rather than sitting idle: **eight of those
fifteen mount points share host inode 2** — every APFS volume root — so they
all map to v7 inode 2 and `st_dev` is the only thing separating them.
`getwd.c:63-70`'s cross-device arm compares both fields and is correct;
`v8sys_fold_ino` deliberately does not key on the device, because
`getdirentries64` reports `d_ino` and no `d_dev`, and keying only `stat` on the
pair would make the two call sites disagree — which is the bug this whole entry
is about.
