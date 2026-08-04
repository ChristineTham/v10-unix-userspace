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

The first `fprintf` tbl reaches is in `t1.c`:

```c
while (gets1(line))
	fprintf(tabout, "%s\n", line);
```

with `char line[512]` a local array and `tabout` set to `stdout` two lines
earlier. Print both before the call — that is one line of instrumentation and it
either exonerates them or names the problem.

Note also `t0.c`:

```c
FILE *tabout  /* = stdout */;
```

with the initialiser commented out, so `tabout` is null until `tbl()` assigns
it. `main` calls `signal(SIGPIPE, badsig)` *before* that assignment, so anything
that printed on the way would print through a null FILE. Worth confirming the
order actually holds here.

The header is called `t..c`, which is not a typo — it is included by the `.c`
files as `#include "t..c"`.
