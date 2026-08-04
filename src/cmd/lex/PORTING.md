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

The routine is `cpyact()` at `sub1.c:272` — "copy C action to the next ; or
closing }" — reading through `gch()` at `sub1.c:368`:

```c
gch(){
	register int c;
	prev = pres;
	c = pres = peek;
	peek = pushptr > pushc ? *--pushptr : getc(fin);
```

A ternary over a pushback stack, and the cut is mid-action at `dodef`, so
`cpyact` stopped mid-copy rather than the output being lost after the fact.

Two things to try next, in this order:

1. **Print in `cpyact`** — the character count and `c` at each step — and see
   what `gch()` returns where the copy stops. That is one write() and it
   distinguishes "gch returned EOF early" from "cpyact took a branch it
   should not have".
2. If `gch` is returning EOF early, look at `fin` and the pushback ternary. Note
   that `pushptr > pushc ? *--pushptr : getc(fin)` mixes a `char` dereference
   with `getc`'s int, and the arms of a ternary rendezvous in one register — a
   shape this back end has been wrong about twice now, once for the join
   register and once for floats.
