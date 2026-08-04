# eqn

Builds with V8's own yacc and V8's own compiler. Does not run yet.

## Built with V8's yacc, not the host's

Modern bison emits `#elif`, which Reiser's 1978 cpp does not understand, so
`y.tab.c` would not preprocess. V8's own yacc (`cmd/yacc`, four files) compiles
with v8cc and generates a 1978-compatible parser — and accepts the `={ ... }`
action syntax natively, so the `YACCFIX` sed the Makefile keeps for bison is not
needed either. It reports the same 147 shift/reduce conflicts bison did, so the
two agree about the grammar.

That is the dogfooding PLAN.md §3 asks for: V8's parser generator building V8's
programs.

## YYSTYPE is long

The find worth keeping. yacc's value stack is declared

```c
#ifndef YYSTYPE
#define YYSTYPE int
#endif
```

and a grammar without a `%union` puts whatever it likes on it. V8's grammars put
**pointers** there — eqn pushes the `char *` its lexer just built, and pic and
grap do the same. Free on the VAX; under LP64 every one truncates:

```
lk 0000000100b75c00     the name as the lexer built it
lk 0000000000b75c00     the same name off the parser stack
```

Fixed in `cmd/yacc/y2.c`, which is the one line that emits that `#define`, so
**every yacc-built program in the tree gets it** — eqn, pic, grap, ideal, expr,
make, awk. A grammar declaring its own `%union` still overrides it.

`e.h` then had to agree: it declares `yyval` and `yylval` itself, and they were
`int`.

## Where it is now

Past the parser, and dying in `cvt` — a **write** to read-only memory:

```
stop reason = EXC_BAD_ACCESS (code=2, address=0x10001bfff)
frame #0: eqn`cvt + 380
->  strb w10, [x9]
```

`code=2` is a protection fault rather than an unmapped page, so this is a store
through a pointer into text. eqn has no `cvt` of its own, so this is libc's —
the digit conversion behind `%f`/`%e`/`%g`. Either eqn hands it a buffer it does
not own, or the format string is being written to.

Instrumenting `cvt` shows what it is handed:

```
CVT arg=fff0000000000000     -- negative infinity
 nd=00000006
```

so the digit loop never terminates and walks off its static buffer. `ndigits` is
a normal 6, so the fault is the value.

That led to a **real back-end bug, now fixed**: a ternary whose value is a
*double* rendezvoused in `x0` rather than `d0`. `lvstore()` had always written
the arms correctly, but both readers — the `QNODE` case and the `GENLAB` join —
took the value out of `x0` regardless. `max(x,y)` is a ternary, so
`printf(".nr 10 %gm\n", max(REL(...), 0))` is exactly the shape. There is a
regression test in `tests/libv8c/run.sh`.

**It did not fix eqn**, which still reaches `cvt` with -inf, so the value is
coming from somewhere else — `REL()` itself, or the arithmetic feeding it. `REL`
and `EM` are declared `extern double` in `e.h`; check they are *defined* as
double too, since an implicit-int definition would return an integer in x0 while
the caller reads d0. That mismatch produces exactly this: a bit pattern with
every exponent bit set.

Also noticed and not yet chased: `%g` prints `7.25000` where it should print
`7.25`. V8's `%g` strips trailing zeros; ours does not.
