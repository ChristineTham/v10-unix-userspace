# ratfor(1) -- porting notes

Kernighan's Rational Fortran preprocessor: reads C-like control flow and writes
Fortran 66. `struct(1)`, already in this port, is its inverse -- so the pair
round-trips, which is an end-to-end assertion that needs no `f77`.

**Wave A2 batch 3, chosen by size.** The five language systems PLAN.md names
measure, counting `*.c *.y *.l *.g *.h`:

| system | files | lines |
|---|---|---|
| **ratfor** | 7 | **1263** |
| `lcomp` | 8 | 1925 |
| `efl` | 25 | 10089 |
| `f77` | 24 | 16140 (+ 149 files / 5165 lines in `libF77` and `libI77`) |
| `cfront` | 68 | 22442 |

## What changed

**Two words, in one file.** `r.h` declares `yyval` and `yylval` `long` where
upstream declares them `int`. Every other file -- `r0.c`, `r1.c`, `r2.c`,
`rio.c`, `rlex.c`, `rlook.c`, `r.g`, `makefile`, `BUGS` -- is **byte-identical
to pristine V8**, verified against the blob hashes in `PROVENANCE`.

### The declarations that lied about a type

`src/cmd/yacc/y2.c` emits, for a grammar that declares no types:

```c
#ifndef YYSTYPE
#define YYSTYPE long
#endif
YYSTYPE yylval, yyval;
```

so both objects are eight bytes, and `r.h` described four. This is the class
`awk.lx.l` and `struct`'s `lextab.l` already carry, and pic's
`extern float atof()` is the floating-point member of it.

**`yylval` carries a POINTER.** `rlex.c`'s `yylex()` does `yylval = (int) str`
where `str` is the 500-byte token buffer, and the grammar hands `$1` to
`outcode(xp) char *xp`, which walks it as a string. The token that carries it
is `GOK` -- ratfor's catch-all for ordinary Fortran text -- so this is the main
path through the program and not an edge case. **Measured: with `int`, ratfor
dies of SIGSEGV on its first line of input.**

**`yyval` carries only label numbers, and is the more dangerous of the two.**
`genlab()` returns small integers, so the obvious reading is that four bytes
are enough. They are not, because **the two lies cooperate**: `yaccpar` line 73
is `yyval = yylval`, so once a `GOK` token has been read `yyval`'s upper half
holds the upper half of that pointer, and a subsequent `yyval = genlab(3)`
written through an `int` declaration replaces only the lower half. Measured,
reverting this one word alone:

```
      call small(i)
      goto 23003
4294990298continue          <- 0x100005ADA: label 23002 with 1 in bit 32
      call big(i)
4294990299continue
```

**exit status 0**, and Fortran that looks correct apart from the labels. `M1`
announces itself with a signal; `M2` would have shipped. Fixing either alone
leaves the other corrupting it.

### `yypv` is deliberately UNCHANGED

`r.h` also declares `extern int *yypv`, and it stays exactly as upstream wrote
it. `yaccpar` line 26 is `register YYSTYPE *yypv` -- a **local** inside
`yyparse()` -- so no such object exists at file scope and the declaration names
nothing. It survives only because nothing references it; a single use would
fail the link with an undefined `_yypv`. Widening it would invent a claim
rather than repair one.

### `rlex.c`'s `(int)` cast is NOT a second truncation, measured

It reads like one, and this port changed it to `(long)` before measuring. It
is a no-op: with `yylval` declared `long`, v8cc emits

```
	adrp	x10, _str@PAGE
	add	x10, x10, _str@PAGEOFF
	str	x10, [x9]
```

-- a full 64-bit store with no narrowing instruction -- for the `(int)` and
`(long)` spellings alike. The two `.s` files are **byte-identical at 19066
bytes**, so the two objects are too. S1 says a change to `src/` must be forced
by the target, and this one is not, so it was reverted and the measurement
recorded here instead.

Same finding as `awk`'s `maketab.c`, settled the same way: **diff `cc -S`
output rather than reading the C.** The distinction that matters is that v8cc
never materialises the intermediate as a 32-bit quantity, so there is nothing
to cut -- compare `last`, where `asctime(gmtime(&delta))+11` does arithmetic on
the `int` and the truncation is real.

## What was audited and deliberately NOT changed

- **`rlex.c:80`, `while(argc>1 && argv[1][0]=='-' && ...)`** -- the address-0
  argv class, and upstream **guards it**: `argc>1` is evaluated first and `&&`
  is a sequence point, so the walk onto the argv terminator that `diffh.c:93`,
  `mc.c:49` and `diff3.c:49` all take cannot happen here. Recorded because the
  shape matches the sweep and the verdict is the opposite one.
- **`r1.c:184,237`, `forstk[forptr++] = malloc(...)`** -- `malloc` is
  **declared** `char *malloc()` at the foot of `r.h`, and `r1.c` includes it.
  `forstk` is `char *forstk[10]`. Correct as written on either machine.
- **The `int` arrays** -- `swexit[5]`, `nextcase[5]`, `brkstk[10]`,
  `typestk[10]`, `brkused[10]`, `keytran[]`, `linect[10]` hold label numbers,
  token numbers and line counts. None holds an address.
- **`BUGS`** -- upstream's own file, recording a 1981 report from Rick Becker
  that an unclosed `for(...` sends ratfor into a loop. Reproduced here and left
  alone: it is upstream's defect on upstream's hardware, which is S1.

## The sweep that could not have found this

CLAUDE.md documents this exact class and prescribes a sweep for it:

```bash
grep -rnE '(extern|static)?[[:blank:]]*(int|short)[[:blank:]]+yylval' src/cmd/*/*.l src/cmd/*/*.c
```

Run over the tree **with ratfor already imported**, its only hit is
`awk.lx.l:11` -- the PORT comment describing the awk instance. It misses both
live instances here, and it is narrow in three independent ways:

- **file set** -- it globs `*.l` and `*.c`; ratfor's grammar is `r.g` (not
  `.y`), its lexer is `rlex.c` (not `.l`), and both declarations are in a
  **header**, which the sweep does not glob at all;
- **symbol set** -- it matches `yylval` only, and `yyval` is the same lie about
  the same yacc symbol, in the same header, one line away;
- **it matches its own documentation** -- the `time(&` shape, where writing a
  finding down grows the population. Here it is the degenerate case: the
  comment is the *only* hit.

So a green run of the documented sweep was compatible with two live
total-failure instances sitting in the tree. Widened in CLAUDE.md to
`*.y *.l *.c *.h *.g` and to `(yylval|yyval)`, with the comment exclusion the
other sweeps already carry.

## Build

`makefile` is upstream's and states the shape: six objects plus `y.tab.o`,
`yacc -d r.g`, and every object depending on `r.h` **and** `y.tab.h`, one line
per object. Two notes on the Makefile block:

- **`y.tab.h` is not a target of its own.** It comes out of the same `-d`
  invocation as `y.tab.c`, and GNU make 3.81 has no grouped targets, so naming
  both would be two rules sharing a recipe and would race under `-j`.
  Everything needing the header depends on `y.tab.c` -- which here is *every*
  object, because `r.h` includes `"y.tab.h"` and all six `.c` files include
  `r.h`. Same decision, and the same reason, as the `awk` and `grap` blocks.
- **`y.tab.o` needs an explicit rule**, or the `%.o: %.c` pattern takes the
  stem `y.tab` and goes looking for a nonexistent `$(RATSRC)/y.tab.c`. The name
  is upstream's own: its link line is `cc r*.o y.tab.o`.

`yacc` reports **2 shift/reduce conflicts**, which are upstream's grammar
(dangling `else`) and are not a port artefact.

## Where it installs

`/usr/bin`, and the two sources agree: the shipped tree has
`v8/usr/bin/ratfor`, and the makefile's install arm is `cp a.out
/usr/bin/ratfor`. `ratfor` appears in no `Admin` table, so `Admin/dest` answers
`/usr/bin` by fall-through -- "nobody said" rather than "V8 said" -- which is
`cpp`'s and `dump`'s pattern, with the difference that here the fall-through
happens to be right.

**That install line exposed a third latent bug in `tests/wavea`'s
makefile-versus-`Admin/dest` parser**; see the entry in CLAUDE.md. The parser
required the program's own name among the *sources* of the `cp`, and the source
here is `a.out`.
