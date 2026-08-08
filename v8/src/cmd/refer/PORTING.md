# refer

All five programs build and link with V8's own compiler: `refer` itself and the
four it execs — `mkey`, `inv`, `hunt`, `deliv`. They are installed into
`rootfs/usr/lib/refer/`, which is where refer looks for them, and the shim
resolves `/usr/lib/...` inside `$V8ROOT`.

37 of the 46 `.c` files compile untouched. **Three needed an LP64 fix**:
`deliv2.c`, `inv1.c` and `hunt2.c` — see below. Six more are not built at all
(next section).

## Not built: whatabout

`flagger.c`, `kaiser.c`, `thash.c`, `what1.c`, `what2.c` and `what4.c` do not
compile, and are not built. They belong to `whatabout`, which is not in
upstream's own `all` target either. They use the pre-C89 initialiser
`int x 5;` — no `=` — which V8's own grammar already rejects. Skipping them
loses nothing that `all` built.

## SOLVED: refer works end to end

```
$ mkey bib | inv -h751 -n bib
$ refer -n -p bib cite.ms
The C language is described in\\*([.1\\*(.]
.ds [A B. W. Kernighan
.as [A " and D. M. Ritchie
.ds [T The C Programming Language
.ds [I Prentice-Hall
.ds [D 1978
.][ 2 book
...
.][ 1 journal-article
```

Citations resolve, the authors of a record accumulate, and the two records are
classified correctly from their fields — `%I` makes a book, `%J` a journal
article. `tests/wavec` asserts all of it.

Three LP64 faults had to be fixed, all in authentic source, all measured rather
than reasoned about. Every other file still compiles untouched.

### 1. `zalloc` returned an `int`

`deliv2.c`:

```c
zalloc(m,n)          /* no return type, so int */
{
	int t;
	t = calloc(m,n);   /* the pointer, truncated to 32 bits */
	return(t);
}
```

Its three callers all declare it as returning a pointer — `int *zalloc()` in
`hunt2.c`, `long *zalloc()` in `glue1.c`, `struct words *zalloc()` in
`glue5.c` — so every allocation in refer came back with its top half gone.
`hunt` faulted at `0x49c7748`, which is `0x1049c7748` with the leading digit
lost. Textbook: this is CLAUDE.md's "calls malloc without declaring it and casts
the int result to a pointer", found once more.

Fixed by giving `zalloc` a `char *` return and a `char *` temporary. The
neighbouring `hunt7.c` calloc site was checked and is fine — it declares
`char *calloc()` at the top of the file.

### 2. `inv` wrote keepkey fields it was never asked for

`inv1.c`:

```c
if (keepkey)
fd = keepkey ? fopen(nmd, "w") : 0;
```

The `if` makes the ternary's else branch unreachable, so without `-d` the local
`FILE *fd` is **never assigned** and keeps whatever is on the stack. Measured:
`keepkey=0 fd=1ee41dc28`, and no `.id` file created — pure garbage, non-null.

`newkeys()` tests `if (fd)` and, believing keepkey is on, appends `";<off>,<len>"`
to every `.ic` line. `result()` in `hunt5.c` then truncates `res` at the first
`;` — **taking the trailing newline with it**. `refer` decides whether it got an
answer by counting newlines (`newline()` in `refer2.c`), so a perfectly correct
tag arrived with no newline and every lookup reported "No such paper".

Fixed by deleting the redundant `if`; the ternary already handles both cases.

### 3. `prefix` dereferenced NULL where the VAX read its own text

`refer5.c:93` is `another = prefix (".[", sd=lookat());` and `lookat()`
(`refer8.c`) returns `fgets`'s NULL at end of input. So the **last** citation in
a file always reaches `prefix` with a null second argument.

On the VAX that read address 0, which was inside the text segment and mapped, so
the comparison simply failed and `prefix` returned 0. macOS keeps page 0
unmapped. A one-line null check in `deliv2.c` reproduces the VAX's observable
answer.

This is a third shape of the same theme as the shim's other accommodations: not
a bug in 1985, a trap on a machine with a guard page.

### 4. Fixed but only observable with `-C`: the posting-list terminator

`doquery()` in `hunt2.c` reads posting lists with

```c
long k; ... unsigned getw(); ...
k = getw(fb);
if (k== -1) break;
```

On the VAX `long` was 32 bits, so the `-1` terminator round-tripped. Under LP64
it widens to **4294967295** and the test never fires. Measured directly in the
`# if D2` trace: `next term finds 4294967295`.

With `colevel == 0` the loop still escapes through `if (j>=nf) break`, which is
why nothing noticed — and why the first version of the guard for this passed with
the fix reverted. It only bites with `-C`, where that escape is disabled and
`hunt` spins forever. `tests/wavec` now runs `hunt -C1` under an alarm.

Fixed by sign-extending the 32-bit read: `k = (int)getw(fb);`.

### Two corrections to what this file used to say

Recorded because the errors are more instructive than the fixes.

**"`hunt` is where to look next"** was right, and I then wrongly overturned it.

**"The fault is in `inv`, not `hunt`. `inv` processes zero keys for ANY input"**
was wrong, and wrong because of an invalid measurement: I ran `inv refs` **with
no stdin**. `inv` reads mkey's stream on standard input — `pubindex` is one line,
`mkey $* | inv -h751 -n $1` — so of course it processed zero keys. The
artefacts I recorded as evidence (`.ia` 3080, `.ib` 4, `.ic` 0, "identical for 1
and 5 records, so not a threshold") were all measurements of a program reading an
empty terminal. Driven properly, `inv` had been building a correct index the
whole time.

The lesson is narrower than "check your setup": **an artefact that looks the same
for every input is evidence the program never ran, before it is evidence about
the program.** I read a constant output as a deep invariant instead.

### Still open, and deliberately not touched

- `hunt` exits **255** when run standalone. `main` in `hunt1.c` falls off the end
  without returning a value, so the status is whatever is in `x0`. That is
  authentic — V8's `main` returned nothing either — and `refer`, which is what
  actually consumes `hunt`, exits 0. Not worth diverging from upstream for.
- `getl`/`putl` in `hunt2.c` build a `long` out of two `int`s via a cast
  (`int x[2]; ... lp = x; return(*lp);`). Under LP64 that is coincidentally
  *more* correct than on the VAX, where it read only the first word. Unreached
  here because our indexes have `iflong == 0`; left alone, and noted so the next
  reader does not mistake it for something this port changed.

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

## bare `hunt` read `argv[1]` before doing anything

`hunt1.c:40`'s option loop is `while (argv[1][0] == '-')` with no `argc` guard,
so `hunt` with no arguments at all dereferences the NULL at `argv[1]`. The VAX
read `0207`, which is not `'-'`, and skipped the loop. Guarded, and
`todir(argv[1])` below it — the same argument on the same path — gets the `""`
that reproduces an unopenable index name.

Reached only by invoking `hunt` directly: `lookbib` always appends an index
path, and the in-process `huntmain` gets `glue3.c`'s synthetic vector.

**Not swept, and the comment in the source says so rather than implying
otherwise:** the arms *inside* that loop each do `argc--; argv++` and then read
`argv[1]`, so a dangling `-r`, `-i`, `-l` or `-t` can still advance past the
terminator — where the guard cannot help, because what sits there is not a NULL.
The bare invocation is the one that was measured to crash.

This is the second address-0 fault in refer after `refer5.c`'s
`prefix(".[", lookat())`. Note there are **four** `prefix()` functions in this
tree — `deliv2.c`, `kaiser.c`, `thash.c`, `what2.c` — and only `deliv2.c`'s is
built or guarded; the other three belong to `whatabout/`, which the Makefile
does not compile. Checked, not assumed. PLAN.md §4i.
