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

## The truncation

Running it on `pic`'s scanner produces a `lex.yy.c` that stops mid-token:

```
			if (c == '(')	/* it's name(...) */
				dodef
```

at **exactly 8192 bytes** — a clean power of two, and a multiple of BUFSIZ.

stdio is **not** the cause. A direct test — `fopen`, a hundred `fprintf`s, then
`exit(0)` with no `fclose` — writes all hundred lines and the last one is
intact, so `_cleanup` flushes a `fopen`'d stream properly.

So lex is either writing through a fixed 8KB buffer of its own, or stopping
early for a reason it is not reporting. Next: find where lex writes `lex.yy.c`
(`sub1.c`/`sub2.c` copy the `ncform` skeleton and emit the actions between), and
check for a `char buf[8192]`-shaped limit or a silently-ignored write error.
