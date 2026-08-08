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
