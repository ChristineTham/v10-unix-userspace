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

Next: instrument `tableput()` the same way — it is one function, and the same
G/L/P bracketing will name the call.

## A note on the file list

`tbl`'s header is called `t..c`, which is not a typo: the `.c` files include it
as `#include "t..c"`. Anything that builds the directory by globbing `*.c` will
compile the header as a translation unit. It is harmless — it defines no code —
but it is why a naive object list has a `t..o` in it.
