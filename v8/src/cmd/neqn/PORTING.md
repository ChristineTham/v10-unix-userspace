# neqn

The nroff half of the eqn pair. Builds with V8's own yacc and V8's own
compiler, links with an empty `nm -u`, and formats equations for a fixed-pitch
device. **One forced change**, in `e.h`.

## It is a separate program, not a link and not a flag

V8 ships both binaries: `usr/bin/eqn` at 47104 bytes and `usr/bin/neqn` at
31256, **different inodes and not byte-identical**. So this is neither the
`vi`/`ex` shape (one binary, four names) nor the `pcat`/`unpack` shape (a link
the tarball lost).

`usr/src/cmd/neqn` is its own directory holding an **older snapshot** of the
same program. Measured against `usr/src/cmd/eqn`:

| | |
|---|---|
| files in both, byte-identical | **0** |
| files in both, differing | 21 |
| only in neqn | `e.y`, `e.def`, `io.c` |
| only in eqn | `eqn.y`, `input.c`, `main.c`, `NEW`, `eqntest.a` |

The two do not even agree about their own file names — `e.y` against `eqn.y`,
and neqn's `io.c` is eqn's `input.c` and `main.c` merged. That is the
two-upstream-copies situation `lib/libtermlib/termcap.c` against
`cmd/ex/termlib/termcap.c` already put in this tree, with the difference that
**both are built here, because Bell Labs built both**.

## The one forced change: `e.h`'s value-stack declarations

`e.h` declares yacc's stack variables itself:

```c
extern int	yyval;
extern int	*yypv;
extern int	yylval;
```

This port's yacc emits `#define YYSTYPE long` (`cmd/yacc/y2.c`) because V8's
grammars push **pointers** on the value stack — `text()` is handed the `char *`
the lexer just built. So `yyval` and `yylval` have to agree with it or the
parser and the rest of neqn disagree about the width of every value.
`cmd/eqn/e.h` carries the identical change for the identical reason, and this
is the **fourth** instance of the class after eqn, awk and ratfor.

Worth recording: it was found by CLAUDE.md's documented sweep for exactly this
class, run **before building** rather than after a crash — the first time that
sweep has caught an instance ahead of the symptom.

**And this instance is LOUD in one translation unit and SILENT in twenty**,
which is the whole reason the sweep was worth running early. Measured by
reverting the change:

```
"build/stage0/neqn/y.tab.c":72: redeclaration of yylval from some line 51
"build/stage0/neqn/y.tab.c":72: redeclaration of yyval from some line 49
```

`e.y` includes `e.h`, so yacc writes the *definition* into the same translation
unit as the `extern` and v8cc refuses: the build stops. That is `egrep`'s
shape, where the same class announced itself because declaration and definition
met in one file, and the change is forced by the BUILD rather than by a wrong
answer. But **20 of the 21 objects include `e.h` and never see yacc's
definition**, so in those the `extern int` compiles clean and reads four bytes
of an eight-byte value. Both failure modes are present at once, and only the
loud one is guaranteed to be noticed.

That is also why no *test* is aimed at this change: the build is the guard, and
it is a stronger one than a case. Mutation-verified in that form -- reverting
it fails `make`, which is the finding.

## `yypv` is deliberately left `int *`, and it is not a third instance

It looks like one: a pointer that walks the value stack would stride four bytes
over eight-byte cells. It is not, because **the object it names does not
exist**. In the yaccpar this port uses, `yypv` is

```c
	register YYSTYPE *yypv;		/* yaccpar:26 */
```

a **local** inside `yyparse()`. The `extern` in `e.h` is left over from an
older yacc whose skeleton made it a global. Measured: the only occurrence of
the name in the whole of `src/cmd/neqn` is that one declaration, so nothing
references it and no code is emitted for it. Widening it is not forced by the
target, which is what S1 asks, and would quietly suggest the symbol is real.

## `-DNEQN` is the whole of the build difference, and a missing `-D` is silent

Upstream's makefile is eqn's with `CFLAGS=-O -DNEQN`. It selects three arms:

- `integral.c` — skips the em-based repositioning of the integral's limits.
- `lookup.c`, first arm — makes `ndefine` a synonym for `define` and `tdefine`
  a no-op; without it the two swap.
- `lookup.c`, second arm — the character table. This is the observable one.

That is the `-DCM_N` shape from `libtermlib`: **a missing `-D` selects an arm
rather than failing to compile**, so the program would build, link, run, and
quietly emit *troff* output from the binary installed as the nroff one.
`tests/wavea` therefore asserts the difference in the OUTPUT, never the flag on
the command line. The sharpest observable is `approx`:

| | |
|---|---|
| with `-DNEQN` | `~\b\d~\u` — a literal **backspace**, i.e. an nroff overstrike |
| without | `\v'-.2m'\z\(ap\v'.25m'\(ap\v'-.05m'` — ems and a troff special character |

Measured end to end, the same input through both binaries shows the same split
at a larger scale: neqn works in absolute units (`\v'20u'`, `.ne 80u`) and eqn
in ems (`\v'0.7m'`, `2.3m`).

## Audited and found clean

Run before building, per the checklist:

- **`char *malloc()` is declared** in both files that call it (`lex.c`,
  `lookup.c`), so there is no truncated pointer return.
- **No address-0 argv defect.** `io.c`'s option loop is
  `while (svargc > 0 && svargv[1][0] == '-')`, and `svargc` is `--argc`, so
  when the last option is consumed the count reaches 0 and `&&` short-circuits
  before `svargv[1]` can reach the terminator. The `svargv[1] = nullstr` below
  it writes to that terminator slot, which exists.
- **No K&R varargs walk**, no `time(&narrowed-field)`, no typed yacc tokens
  (`%token<...>`), so the global `YYSTYPE long` covers the grammar.

## `e.def` is an `#include`d non-header, in eight files

`shift.c`, `lookup.c`, `lex.c`, `move.c`, `integral.c`, `funny.c`, `text.c`
and `diacrit.c` all `#include "e.def"`, which is `y.tab.h` under another name.
Invisible to a header scanner *and* to any `*.c` glob — the shape that made
`lex`'s `once.c` go stale — so the Makefile declares it and `tests/deps`
asserts the edge.
