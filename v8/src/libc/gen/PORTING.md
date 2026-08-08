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

### Measured, and reproduced 10 times out of 10

Not inferred from the code. `$TMPDIR` on this host holds **5452 entries whose
folds collapse to 5250 distinct values -- 199 values shared by two or more
entries, 401 entries involved**. Entering, in turn, the ten collision pairs
whose later member is a directory and asking V8's `pwd` where it was:

| | |
|---|---|
| tried | 10 |
| **wrong** | **10** |
| of those, printed the colliding entry's name and **exited 0** | 4 |
| of those, died with `getwd: can't change back to .` | 6 |

The split is the whole hazard. Where the collider is another *directory*, `pwd`
prints a real path that is not where you are, successfully. Where it is a
regular file -- six of ten here were `*.finvestlens.audit.log` -- the `chdir`
back fails and the error is at least loud.

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

It has to live in the shim, and it has to survive one hard constraint: the fold
must stay a **pure function of the host inode**, because a program that runs
`ls -i` twice must see the same number, and folded values are written into
files (`shim/libkmemu/NOTES.md:247` -- `e_tdev` in the manufactured
`/etc/utmp`) that another process reads. That rules out assigning numbers in
order of first sighting, which is the easy way to be injective and is
order-dependent. It cannot be fixed by a better hash either: 64 bits into 16
must collide, and the current fold already collides at the birthday rate, so it
is as good as random and there is nothing to win there.

What is left is disambiguation at the *directory snapshot*, where the shim sees
every entry at once and can resolve a clash by a rule that depends only on the
set of inodes present -- with the perturbation recorded so `stat_translate`
agrees. The ordering is the difficult part rather than the table: `getwd` calls
`stat(".")` **before** it opens `..`, so a value perturbed by the snapshot
arrives after the caller has already taken the unperturbed one.
