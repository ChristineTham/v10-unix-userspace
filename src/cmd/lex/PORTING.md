# lex

Builds with V8's own yacc and V8's own compiler, and runs. Its output is
truncated.

## Build structure

`once.c` and `ldefs.c` are **included**, not compiled — `lmain.c` starts with
`# include "ldefs.c"` and `# include "once.c"`. The object list is
`lmain.o y.tab.o sub1.o sub2.o header.o`, five objects plus the grammar.

Anything that builds the directory by globbing `*.c` will try to compile the two
included files on their own and fail — `once.c` uses `NCH`, which `ldefs.c`
defines. That is not a porting problem; it is the same shape as `tbl`'s `t..c`.

`lex` opens its skeleton as `/usr/lib/lex/ncform`, which the shim resolves inside
`$V8ROOT`.

## It no longer crashes

`left[]` and `right[]` are `int` arrays holding two different things: a node
index into the parse tree, and — for a character class — a **pointer** into the
packed-character array. `cfoll()` in `sub2.c` does

```c
char *p;
p = left[v];
```

and then scans it for a NUL. On the VAX an int held a pointer exactly; under
LP64 it holds half of one, and lex died scanning `0x4995010`. Both are `long`
now. Every allocation uses `sizeof(*left)`, so the sizes follow.

The compiler cannot rescue this one either — genuine `int` arrays, stored into
four bytes and reloaded from four, no conversion anywhere. Exactly `tbl`'s
`ct = reg(...)`, which is now the third instance of this shape.

The 8192-byte output was a red herring: it was simply the last flushed stdio
buffer before the crash, not a truncation.

## Where it is now

It runs to completion and fails honestly:

```
(Warning) OOPS - calloc returns a 0        [x9]
(Error) Too little core for state generation
1323/1700 nodes(%e), 0/5000 positions(%p), 1/700 (%n), ...
```

so `malloc` is refusing. `calloc` itself is fine (`src/libc/gen/calloc.c` is
`num *= size; malloc(num)`, and both are exercised by the test suite).

Two candidates, and the counters above should distinguish them:

1. **The tables genuinely doubled.** `left` and `right` are now 8 bytes an
   element, so lex asks for twice the core it used to for the same grammar. If
   the arena or a fixed limit is the constraint, this is the same kind of
   constant bump `brkincr.h` needed for the shell.
2. **`malloc` fails at a size it should not.** The sbrk arena is 1GB and the
   suite covers malloc, but not at these sizes; a direct test asking for the
   same total lex does would settle it in one run.

Print the requested size at the failing `myalloc` first — it says which.