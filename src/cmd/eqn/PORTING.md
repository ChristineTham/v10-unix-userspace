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

**It did not fix eqn.** `REL` and `EM` are properly defined as `double`, and
returning an int constant from a `double` function works (tested directly). The
caller is elsewhere.

Making `_doprnt` print the format string when it reaches a float conversion
names it at once:

```
FMT[%gm'%s%s\*(%d%s%s\v'%gm'
]
```

which is `shift.c:51`:

```c
printf(".as %d \v'%gm'%s%s\*(%d%s%s\v'%gm'\n",
	yyval, REL(shval,ps), DPS(ps,subps), sh1, p2,
	DPS(subps,ps), sh2, REL(-shval,ps));
```

**Nine arguments.** That is the interesting part: `fmt` takes x0 and the eight
varargs take x1–x7 and then *the stack*. Every `printf` the port has run so far
fitted in registers, so this is the first time V8's `&args` walk has had to cross
from the spill block into the caller's stack arguments.

The prologue is built for exactly that — it allocates the 64-byte spill block
*before* pushing x29/x30 so the eight spilled registers sit immediately below the
caller's stack arguments and the two read as one contiguous array (see the frame
diagram at the top of `compiler/ccom-arm64/local.c`). So the design is right;
what needs checking is whether the arithmetic agrees at the boundary.

Tested directly, and **it works**: nine arguments print correctly, ninth
included, and so does a `double` in the spilled position. Both are now in
`tests/libv8c/run.sh`, because that seam deserves a test whether or not eqn
needs one.

So the argument block is sound and it really is the *value*. `shval` is a
`float` local (`shift.c:19`) assigned from `EM()`, which returns `double`, and
then passed back to `REL()`, which takes `double`. Two narrowing/widening
conversions around a float local, in a nine-argument call.

Tested directly, and that works too:

```c
float shval, b1;  b1 = 1.5;
shval = b1 + EMx(0.2, ps);
printf("shval=%.4g rel=%.4g negrel=%.4g\n", shval, RELx(shval,ps), RELx(-shval,ps));
    -> shval=1.700 rel=3.400 negrel=-3.400
```

So the float local, the double return, the narrowing assignment, the widening
back and the unary minus are all sound in isolation. Which is the pattern this
port keeps producing: **every construct correct alone, wrong in combination.**

What has NOT been checked is where `shval` comes from in the real program — the
branch above line 32 sets it from `b1 + EM(...)` but the one at line 41 sets it
from `-(0.4 * (h1-b1)) - b2`, and `eht`/`ebase` are `float` arrays indexed by
`yyval`. If those arrays hold garbage, everything downstream is -inf honestly.

Measured, and they are sane:

```
eht=3ff0000000000000     eht[11] = 1.0
eba=0000000000000000     ebase[11] = 0.0
p1= 0000000b             11
```

So `shift()` starts from good values and produces -inf itself. Both `EM` and
`REL` are divisions —

```c
EM:   m *= (float) EFFPS(ps) / gsize;
REL:  m *= (float) gsize / EFFPS(ps);
```

— which is how an infinity gets made. `gsize` is `int gsize = 10;` in `glob.c`,
properly initialised, so the suspect is `EFFPS(ps)` evaluating to zero, or `ps`
itself being zero at this call.

Next, and this is a short one: print `ps`, `EFFPS(ps)` and `gsize` inside `EM`.
One of the three is zero and the division names it.

Also noticed and not yet chased: `%g` prints `7.25000` where it should print
`7.25`. V8's `%g` strips trailing zeros; ours does not.
