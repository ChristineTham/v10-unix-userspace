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

stdio *reading* is not the cause either: `getc` over a 37000-byte file returns
every byte. So neither half of stdio is at fault.

And the truncation is not in the skeleton copy — `lmain.c:104` copies `ncform`
with `while ((i=getc(fother)) != EOF) putc(i,fout);` and that runs *after* the
actions, while the cut is mid-action. So lex stopped emitting actions early,
without reporting anything.

Next: find where the action text is written — it is copied from the `.l` file as
lex parses, so look in `sub1.c`/`sub2.c` for the routine that echoes an action,
and for a fixed-size buffer or a counter that could stop at 8192. `lex` has
several `char [ ]` limits of its own (`NCH`, `DEFSIZE`, `NSTATES`); one of them
may simply be too small for a scanner this size, in which case the fix is the
same kind of constant bump `brkincr.h` needed for the shell.
