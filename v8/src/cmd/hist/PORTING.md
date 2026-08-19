# `=`(1) and `==`(1) -- redo commands from history

Two commands from one 365-line file.  `=` re-runs the last line of `$HISTORY`
(or the last line matching a pattern); `==` opens a small line editor on it
first, in which a bare `=` steps back to the previous distinct command.

Upstream: `usr/src/cmd/hist/{=.c,Makefile}`.  `Makefile` is

```
	cp a.out /usr/bin/=
	ln /usr/bin/= /usr/bin/==
```

so the two names are one inode.  Measured the way `pcat`/`unpack` and `e`/`ed`
were, because an install arm can be one that never ran: the shipped
`usr/bin/=` and `usr/bin/==` are byte-identical at 13312 with **different**
inodes, the tarball having lost the link on extraction.

Both names are in none of `Admin/binfiles`, `etcfiles` or `libfiles`, so
`Admin/dest` answers `/usr/bin` by fall-through and the shipped tree agrees.

## The one forced change: the vector cell is a machine word

`=.c` keeps a single vector, `matchvec`, and fills it from **both ends with
two different kinds of thing**:

| | filled by | read back by |
|---|---|---|
| `matchvec` upward | `linesave()`, with `ftell()` offsets | `fseek(f, *lastmatch, 0)` |
| `matchend` downward | `prevline()`, with `savestr()` results | `strcmp((char *)(*ip), hp)` |

Upstream declares it `int *` and allocates with `Malloc(int, ...)`, which is
exact on a VAX because a pointer was four bytes there.  Here the pointer half
loses its top half, and `prevline()` is not an obscure path: it is what a bare
`=` typed at `==`'s prompt does, i.e. the ordinary way to step back a line.

**Measured** by printing the store and the load side by side:

```
SAVED 300001818  CELL 1818
SAVED 104f1d818  CELL 4f1d818
```

`0x300001818 & 0xffffffff` is `0x1818`, which is in the unmapped low pages, so
the `strcmp` faults.  The value **moves between runs**, which is this port's
recorded tell that it is a real pointer being truncated rather than garbage --
a constant-looking wrong value that does *not* move is a truncation of
something else.

Before and after, hermetically (a fresh `$HISTORY` each run, because `=`
appends what it ran to the file and so is not a pure function of its input):

| | status | output |
|---|---|---|
| pristine | **139** (SIGSEGV) | `echo charlie` |
| fixed | 0 | `echo charlie` / `echo bravo` / `bravo` |

The fix is **widen the TYPE, not the uses** -- `struct(1)`'s `VERT` verdict and
`bc`'s arena cell, reached a third time.  What the cell has to be is one
machine word, and no V8 type spells that; `long` is pointer-sized here and was
int-sized on a VAX, so the source stays right for both machines.  Five
declarations and one cast:

```
int *matchvec, *matchend, *lastmatch, *lastuniq, vecsize;
                                     ->  long *matchvec, ... ; int vecsize;
Malloc(int, vecsize = VECSIZE)       ->  Malloc(long, ...)
Realloc(int, matchvec, ...)          ->  Realloc(long, ...)
register int *ip;                    ->  register long *ip;
*--lastuniq = (int)savestr(linebuf); ->  (long)
```

`vecsize` is split onto its own line and stays `int`: it counts cells, it is
never a cell, so widening it is not forced by the target.

**Neither direction warns.** Upstream wrote an explicit `(int)` on the store
and an explicit `(char *)` on the load, so v8cc has nothing to complain about
and the build is clean before and after.  The compiler was never going to find
this one; only running it did.

## The link is load-bearing

`main()` opens

```c
	int edit = argv[0][1] != '\0';
```

so the program decides which command it is from its own name -- `=` has a NUL
at `[1]` and `==` has `=`.  That is `compress`/`uncompress`/`zcat`'s rule, and
it decides what the tests have to be: an inode comparison proves the link
exists but not that it reached the *program*, so each name gets a behavioural
case.  Given the same stdin, `=` ignores it and runs the last line while `==`
reads it and steps back, which is the pair that discriminates.

Note it does **not** strip a leading directory the way `compress.c:385` does,
so the distinction only holds when the command is reached through `PATH` by its
bare name; invoked as `/usr/bin/=` it sees `[1] == 'u'` and takes the editor
path.  That is upstream's behaviour on any machine, not a port defect, and it
is left alone under S1.

## Audited and deliberately unchanged

- **`ntell` is an `int` holding `ftell()`'s `long`.**  Exact on a VAX, and
  exact here for any history file under 2 GiB.  Not forced by the target.
- **`Realloc` does not rebase `matchend`, `lastmatch` or `lastuniq`.**  When
  the vector grows past `VECSIZE` (512 matching lines) and `realloc` moves the
  block, those three still point into the freed one.  Upstream's own bug on
  upstream's own hardware -- a VAX had it identically -- so it is recorded
  rather than fixed, per S1.
- **`savestr()` leaks and can return 0 without the caller checking.**  Same
  verdict: upstream's, unchanged by the target.
- **`execl("/bin/sh", ...)`** is correct here: `/bin/sh` inside the jail is
  V8's own shell.
