# refer

All five programs build and link with V8's own compiler: `refer` itself and the
four it execs — `mkey`, `inv`, `hunt`, `deliv`. They are installed into
`rootfs/usr/lib/refer/`, which is where refer looks for them, and the shim
resolves `/usr/lib/...` inside `$V8ROOT`.

40 of the 46 `.c` files compile untouched. **No source changes were needed.**

## Not built: whatabout

`flagger.c`, `kaiser.c`, `thash.c`, `what1.c`, `what2.c` and `what4.c` do not
compile, and are not built. They belong to `whatabout`, which is not in
upstream's own `all` target either. They use the pre-C89 initialiser
`int x 5;` — no `=` — which V8's own grammar already rejects. Skipping them
loses nothing that `all` built.

## UPDATE: the `/bin` gap is closed; `hunt` is the remaining bug

The section below described the state before the V8 world had its own `/bin`.
That landed with the make bootstrap: `/bin/` and `/usr/bin/` are on the shim's
redirect list, `v8s_execve` routes through `vpath()`, and V8's own `pwd` is
installed. **refer no longer hangs** — it runs to completion.

What is left is a different, further-along failure. Given a bibliography:

```
$ mkey refs
refs:0,74	kernig ritchi the progra langua 1978        <- correct
$ hunt -p refs kernighan
                                                        <- nothing
```

`mkey` produces the right keys; `hunt` finds no record for them, so refer emits
`.nr [W \w''` and passes the `.[` / `.]` block through unsubstituted. Nothing
leaves the jail during the run (checked with `V8JAIL=warn`), so this is refer's
own logic rather than a path or exec problem. ~~`hunt` is where to look next.~~ **It is not — see below.**

### CORRECTION: the fault is in `inv`, not `hunt`

Driving the pipeline by hand with the helpers at their installed path
(`$V8ROOT/usr/lib/refer/`, not `/usr/bin` — refer execs them from there):

```
$ mkey refs
refs:0,57	kernig the progra langua 1978        <- correct
$ inv refs
$ ls -l refs.i?
refs.ia  3080     <- the hash table, written
refs.ib     4     <- essentially empty
refs.ic     0     <- the posting lists: NOTHING
```

`hunt` finds no records because **there are no records to find.** `inv` writes
the hash table and then writes an empty `.ic`. Every hour spent reading
`hunt1.c`'s header handling was spent on the wrong program — the header it
reads is consistent with the file `inv` produced, which is why
`_assert(kk == nhash)` passes and the read looks clean.

The `hpt`-is-`long*` and `iflong` observations below remain true and remain
worth checking, but they are not the cause. Look at `inv`'s output path first:
`inv5.c` and `inv6.c` are where the tables are written.

This is the second time in this port that a "reader" bug has turned out to be a
writer bug (spell was the first — PLAN.md §4e), and both times the reader
looked wrong because it was the program producing the visible symptom.

### Narrowed: the index header, and `iflong`

`hunt1.c` reads the inverted index like this:

```c
fread (&nhash,  sizeof(nhash),  1, fa);
fread (&iflong, sizeof(iflong), 1, fa);
if (master == 0)
	master = (unsigned *) calloc (lmaster, iflong ? 4 : 2);
hpt = (long *) calloc(nhash, sizeof(*hpt));
kk = fread( hpt, sizeof(*hpt), nhash, fa);
_assert (kk == nhash);
```

Two things stand out, and they are the same shape as the bug that stopped
`spell` (PLAN.md §4e — a struct whose size *was* an on-disk format):

1. **`hpt` is `long *`** — 4 bytes on the VAX under `NOLONG`, 8 here. The index
   `inv` writes and the index `hunt` reads therefore both moved to 8, so they
   still agree *with each other*; that is why `_assert(kk == nhash)` does not
   fire and the header appears to read correctly. It would NOT agree with an
   index written by a VAX, and spell showed that a reader and writer agreeing
   with each other proves nothing about the format.

2. **`iflong` is an explicit width flag in the file** — `calloc(lmaster,
   iflong ? 4 : 2)` — so the format already distinguishes narrow from wide
   entries, and `inv` decides which to write. Whether our `inv` sets it
   consistently with what our `hunt` then assumes is the first thing to check.

So the header is read without error and the lookup still finds nothing, which
puts the fault after the read: either the hash of a key does not match what
`inv` stored, or the master-table indexing is using the wrong entry width. The
next measurement is to build the D1 debug prints already in this file
(`# if D1`), which print `read %d hashes, iflong %d, nhash %d` — the values
themselves, which is what settled every hard bug in this port.

## Historical: why refer needed a V8 `/bin`

refer runs, gets as far as its helpers, and then does not produce output,
because of one line in `glue2.c`:

```c
savedir()
{
	if (refdir[0]==0)
		corout ("", refdir, "/bin/pwd", "", 50);
	trimnl(refdir);
}
```

`corout` runs the program with `execl(rprog, "deliv", arg, 0)`, so `/bin/pwd`
is invoked with an **empty extra argument**. V7's `pwd` ignored arguments;
macOS's `/bin/pwd` answers `usage: pwd [-L | -P]` and exits, so `refdir` stays
empty and refer cannot find its way back to the working directory.

The right fix is not to patch refer — the code is correct for the system it was
written for. It is that **`/bin/pwd` should be V8's `pwd`**, which this port
already builds (Wave A).

What that needs:

* `rootpath()` in `shim/v8sys/syscall.c` currently redirects `/usr/lib/`,
  `/usr/share/`, `/usr/dict/`, `/lib/` and `/usr/pub/` into `$V8ROOT`.
  `/bin/` is deliberately **not** on that list, because `system(3)` and
  `popen(3)` exec `/bin/sh` and that has to keep working.
* Adding `/bin/` means the V8 world gets its own `/bin`, including its own
  `sh` — which this port has, and which passes 21 tests. That is *more*
  authentic, not less, but it changes what every `system()` call in the tree
  runs, so it wants its own change and its own test pass rather than being
  folded in here.

Until then refer builds, links, and starts its helpers correctly; it is the
`pwd` round trip that is missing.

## What it did cost: execl

refer's first symptom was not a hang but an **interactive shell** — `sh: no job
control in this shell`, then `sh-3.2$`. `system()` was reaching the host's
`execl`, which is variadic, so the `-c` and the command string never arrived and
`/bin/sh` started as a login shell.

`execl`, `execv` and `execle` were missing from `libv8c.a` — V8 kept them in
`libc/sys` as assembly, which the shim replaced. They are now in
`src/libc/gen/exec.c`. See the comment there, and the guard in
`tests/libv8c/run.sh` that now checks no variadic libc function is left for the
host to supply.
