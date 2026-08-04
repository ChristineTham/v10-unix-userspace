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

Measured. Printing `a`, `b` and the result at every `myalloc`:

```
MA 000003e8 00000001 0000000300001010     1000 x 1   -> ok
MA 00000028 00000008 00000003000017f0       40 x 8   -> ok
MA 000002bc 00000001 0000000105adda80      700 x 1   -> ok, DIFFERENT region
MA 000002bc 00000004 0000000000000000      700 x 4   -> FAILS
MA 0000052c 00000008 0000000000000000     1324 x 8   -> FAILS
MA 00001388 00000004 0000000000000000     5000 x 4   -> FAILS
```

So this is **a malloc problem, not a lex problem**: small requests succeed and
requests from about 2800 bytes upward return 0. Note also that successful
allocations come from two different regions — `0x3000xxxx`, the shim's sbrk
arena, and `0x105adda80`, which is nowhere near it.

That second address is the thing to pull on. `src/libc/gen/malloc.c` is Ritchie
first-fit over `sbrk`, and `ialloc()` splices each new `sbrk` block into a
circularly-linked arena that it requires to be **monotonically increasing**. If
one block comes back far from the last, the ordering assumption breaks and the
search walks off — which is exactly the shape of "small ones work, larger ones
return 0".

Next: print in `malloc` itself — the requested size, what `sbrk` returned, and
`allocb`/`allocp` — for a program that asks for 700, then 2800, then 20000
bytes. That is a libc test, not a lex one, and it belongs in
`tests/libv8c/run.sh` once understood.