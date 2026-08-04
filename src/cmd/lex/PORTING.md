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

`myalloc` held `calloc`'s result in an `int`, which is fixed — and worth
recording for the coincidence:

```c
register int i;
i = calloc(a, b);
if (i == 0) warning("OOPS - calloc returns a 0");
```

The shim's sbrk arena starts at **0x300000000**, whose low word is exactly zero,
so a truncated pointer from it *is* 0 and the test fired on a perfectly good
allocation. A rare truncation that announces itself; usually half a pointer is a
plausible address and the program dies somewhere else entirely.

That was not the whole story. With `i` a `char *` and `calloc` properly declared
(`extern char *calloc()` in `ldefs.c`), the warning still fires nine times, so
**malloc is genuinely returning 0** for some of lex's requests. Some succeed —
the run reports `1323/1700 nodes` — so it is not failing outright.

Next: print `a`, `b` and the returned pointer at each `myalloc` call. Nine
failures out of many suggests a particular size or a particular point in the
arena's growth, and the numbers will say which. Then reproduce that size
directly against `malloc` in a standalone program — the libc suite covers malloc
but not at these sizes or this allocation pattern.

Note that `left` and `right` are now 8 bytes an element, so lex asks for roughly
twice the core it used to; if the arena has a limit being hit, that is why it is
being hit now and was not before.