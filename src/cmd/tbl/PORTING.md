# tbl

All 23 files compile with V8's compiler and link freestanding, with no source
changes. It does not yet run.

## Where it is

It dies in `_doprnt` — our libc's, so the fault is a bad argument rather than a
bad formatter — reading address `0x1d7fe`:

```
stop reason = EXC_BAD_ACCESS (code=1, address=0x1d7fe)
frame #0: tbl`_doprnt + 3788
```

The same with input on stdin as from a file, so it is not the input path.

`0x1d7fe` is worth noting: it is *not* the low half of a stack address (those
look like `0x16f...`), so this is unlikely to be the plain pointer-truncation
that `fdprintf` and `sprintf` had in troff. Something is being read as a `%s`
argument that was never a string.

## Where to look

The crash is **not** in the first `fprintf`, which was the obvious suspect.
Instrumenting the read loop in `t1.c` shows it completing:

```
G          gets1 called
L[.TS]     line holds ".TS", correctly terminated
P          fprintf returned
```

and the fault follows, in `tableput()` — the branch `.TS` triggers. The earlier
backtrace pointed at `_doprnt` because a *later* `fprintf`, inside the table
processing, is the one handed something bad.

So this is not a libc problem and not the stdio path. Eliminated along the way,
each by direct test:

* the arguments to the first `fprintf` — a real `FILE *` and a real stack address
* `fprintf` to a buffered stream through a global `FILE *`, standalone
* `setinp`, which does nothing at all when reading stdin
* `fgets`, which reads `hello\n` as six bytes and NUL-terminates

`tableput()` is a list of seventeen calls, and bracketing each one puts the
fault in the twelfth:

```
06 07 08 09 10 11      <- printed before each call; 11 is runout()
```

`runout` (in `t7.c`) is the output generator — `deftail()`, then `putline(i,i)`
for every row — so it is where the table's own strings are first printed, which
matches a `%s` fault in `_doprnt`.

Those strings live in tbl's private arena: `alocv()` in `tb.c` hands out slices
of `calloc`'d blocks, and the cells are `struct colstr` — which contains
pointers, so it doubled under LP64. Worth checking whether every allocation for
it is computed with `sizeof` rather than a literal, since a literal would now be
half what is needed and the table would overlap itself. Narrowed once more, inside `runout`:

```
11        runout() entered
R1 R2     deftail() completed
R3        putline(0,0) entered -- and never returns
```

So it is `putline`, on the **first row**, in `t7.c`. And the arena is probably
not the cause: `alocv`'s only call site is

```c
table[nlin] = (struct colstr *) alocv((ncol+2)*sizeof(table[0][0]));
```

which uses `sizeof`, so it sizes correctly under LP64; and a three-row,
two-column table needs a few hundred bytes of the 2000 (`MAXCHS`) a block holds.

`putline` (in `t8.c`, not `t7.c`) walks a row's cells, and it uses an idiom
worth looking at first:

```c
s = table[nl][c].col;
...
if ((int)s>0 && (int)s<128)
```

That is V7's trick of keeping **small markers in pointer slots** — a cell whose
`col` is a number under 128 is a code, not an address. `vspen(s)` and friends
test the same way. The compiler no longer truncates a pointer cast to `int`
(`PTRCONVFULL`, see `common/optim.c`), which is what makes a real pointer
correctly fail the `< 128` test — but every one of these comparisons deserves
reading, because the idiom depends on the exact behaviour that changed.

Done, and the cells are sound:

```
R3                          putline(0,0) entered
cell 0000000300000c78       both real addresses in the sbrk arena,
cell 0000000300000c7d       neither a marker, both processed
```

Bracketing `putline`'s sections in turn puts the fault past the *second* cell
loop — the one that computes `chfont` — not the first:

```
R3                       putline(0,0) entered
B C D E F                every section marker up to `vspf=0; chfont=0;`
c2 s=0000000300000c78    first cell, a real address
cf                       chfont computed
c2 s=0000000300000c7d    second cell
cf                       and again -- then nothing
```

Both iterations reach `cf` and neither reaches the marker after
`if (point(s)) continue;`, which means `point()` returned **true** both times and
the loop simply ended: `ncol` is 2. So `point()` is fine, the loop is fine, and
the fault is in whatever follows it.

That is worth stating plainly because `point()` looked like the culprit:

```c
point(s)
{
return(s>= 128 || s<0);
}
```

— an implicit-`int` parameter holding a pointer, which is exactly the shape that
broke `look(1)`. It works here because `acctype()` in the back end reads an
`int` parameter at the full slot width. The idiom is sound on this target; it
just is not where this bug is.

Everything before that point is now eliminated by direct measurement: the read
loop, the first `fprintf`, `setinp`, `fgets`, `tableput`'s first eleven stages,
`deftail`, the cell pointers, and both cell loops. Narrowed once more. Bracketing the tail statement by statement:

```
T1 T3 T4 T5     then nothing
```

T5 sits immediately before

```c
if (allh(i) && !pr1403)
	{ ... }        /* T6, T7, T8 -- none printed, so this branch is skipped */
else
	fprintf(tabout, ".nr 35 1m\n");
if (chfont)
	fprintf(tabout, ".nr %2d \n(.f\n", S1);
fprintf(tabout, "\\&");
vct = 0;
for(c=0; c<ncol; c++)
	{
	uphalf=0;
	if (watchout==0 && i+1<nlin && (lf=left(i,c, &lwid))>=0)
```

so the condition was simply false and the fault is in that handful of lines. The
`fprintf`s there take no pointer arguments, which leaves `left(i, c, &lwid)` —
and `&lwid` is the address of a local `int` passed to what may well be an
undeclared parameter. That is the `look(1)` shape again, and while `acctype()`
covers an `int` parameter read at full width, a parameter *written through* is
worth checking separately.

Bracket `left()` next, and print `&lwid` on both sides of the call.

## A note on the file list

`tbl`'s header is called `t..c`, which is not a typo: the `.c` files include it
as `#include "t..c"`. Anything that builds the directory by globbing `*.c` will
compile the header as a translation unit. It is harmless — it defines no code —
but it is why a naive object list has a `t..o` in it.
