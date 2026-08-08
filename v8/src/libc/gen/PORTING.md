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
It is not exact here. `v8sys_fold_ino()` (`shim/v8sys/dir.c:125`) XOR-folds a
64-bit host inode into V7's `u_short ino_t`, and the same function feeds *both*
sides of that comparison -- `d_ino` in the directory snapshot (`dir.c:236,238`)
and `st_ino` in `stat_translate` (`shim/v8sys/syscall.c:1086`). So two files in
one directory can share a `d_ino`, and this loop stops on whichever `readdir`
yields **first**.

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

So this is a **structural limit of the fidelity contract**, not a bug in
`getwd.c`, and the note it corrects is CLAUDE.md's own 16-bit table, which
recorded a folded `d_ino` as "wraps; harmless *except* the value that wraps to
0". Measured, the wrap to 0 is the one case that **was** already handled
(`fold_ino` never returns it); the collisions it called harmless are not.

### What a fix would have to be, and why it is its own task

It has to live in the shim, and the constraint recorded here was **half of it
false** -- which matters, because the false half is the one that ruled out the
easy fix.

What stands: the fold must be a **pure function of the host inode**, because a
program that runs `ls -i` twice must see the same number. What does not: this
said folded values "are written into files (`shim/libkmemu/NOTES.md:247` --
`e_tdev` in the manufactured `/etc/utmp`) that another process reads". Three
things wrong with that sentence, measured:

- **`v8sys_fold_ino` has exactly three call sites** -- `dir.c:236`, `dir.c:238`
  and `syscall.c:1144`. Nothing in `libkmemu` calls it. `grep` found it in
  `procfs.c:169` and `:603`, and both are **comments**.
- **The cited note says the opposite.** `NOTES.md:247` is about `u_ttyino` in
  `/proc`'s u-area, which is "left zero"; filling it *would* be a stat folded
  through `v8sys_fold_ino`, and it explicitly "buys nothing until those nodes
  exist". A hypothetical, read as a fact.
- **`/etc/utmp` has no inode field at all.** V8's `struct utmp` is
  `{char ut_line[8]; char ut_name[8]; long ut_time;}` -- 24 bytes -- and
  `libkmemu/utmp.c` writes those three and nothing else. `e_tdev` appears
  nowhere in any source file.

That is the third instance of one shape in this repo: a sweep matching the
*documentation* of a thing and counting it as the thing. It cost more here than
the other two, because it stood as the reason not to try a fix.

It still cannot be fixed by a better hash: 64 bits into 16 must collide, and
the current fold already collides at the birthday rate, so there is nothing to
win there.

### Three candidates, costed

**(a) Disambiguate at the directory snapshot.** The shim sees every entry at
once and can resolve a clash by a rule over the set of inodes present. **The
ordering kills it, and this is now measured rather than suspected**: `getwd`
calls `stat(".")` *before* it opens `..`, so it has already taken the
unperturbed value when the snapshot perturbs. Worse than a partial fix -- if
the cwd is the entry that gets perturbed, the loop matches *nothing* and the
walk fails, so a rule that repairs the 50% of cases that work today breaks the
other 50% that currently work by luck. For `stat_translate` to compute the same
perturbation it must know the parent's inode set, i.e. scan the parent on every
`stat`, which is the `getdirentries`-inside-`ls -l` cost `v8sys_dirsize` already
refused.

A cheap variant -- give the later colliding entry `d_ino = 0` -- is worse than
the disease: 0 is V7's deleted-entry marker, so `readdir` skips it and **the
file vanishes from `ls`**.

**(b) Order of first sighting, process-local.** Injective by construction, and
the reason recorded against it has just been shown false, so it is back on the
table. What is actually left of the objection is narrow: `ls -i` in two
different processes could print different numbers **for a colliding inode
only** (215 of 6031 entries here, 3.5%), and only for one that both runs
observe. Against a `pwd` that is wrong 47% of the time inside a collision
group, that trade looks right. The real difficulty is *bounding* the table --
eviction reintroduces the inconsistency inside a single process, which is the
one place it must not appear.

**(c) Widen the identity.** The correct fix, and the expensive one. `ino_t` is
`u_short` (`sys/types.h:86`) and appears in two **on-disk** records --
`struct direct.d_ino` and `struct filsys.s_inode[]` -- so it cannot move
globally. Per-field narrowing is exactly what `sys/ino.h` and `sys/filsys.h`
already do for `time_t` and `off_t`, so the machinery exists; but `struct
direct` is on-disk for `$(IMGBIN)` and in-memory for the live emulation, which
would make `d_ino` a second `DIRSIZ`: two widths behind one `-D`, with the
whole warning that entry carries. And a 4-byte `d_ino` moves the record to 258
bytes, which the 44 raw directory readers survive only if every one of them
uses `sizeof(struct direct)` rather than a literal.
