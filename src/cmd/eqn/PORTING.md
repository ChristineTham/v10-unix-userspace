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

Note that string literals are in *writable* data on this target (see
`locnames[]` in `compiler/ccom-arm64/emit.c`), so a literal is not the read-only
thing being written. Find the caller first: `eqn` prints sizes and positions, so
look for the `%g`/`%f` conversions in `size.c` and `text.c`.
