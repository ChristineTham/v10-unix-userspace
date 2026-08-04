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

Measured, and neither divides by zero:

```
EM ps/eff/gs 0000000a 0000000a 0000000a     10, 10, 10
EM ps/eff/gs 00000007 00000007 0000000a      7,  7, 10
```

So `EM` computes `m * 7/10` and `REL` computes `m * 10/7`. No infinity is being
*calculated*.

Also eliminated: a `double` in the **spilled** argument position — the ninth
vararg, which lands on the stack rather than in x7 — prints correctly. That was
the best remaining structural guess, since eqn's crashing call has exactly that
shape.

## Where this stands

Measured at the call site in `shift.c:51`:

```
CALL sh/r1/r2 bfd99999a0000000 fff0000000000000 7ff0000000000000
              shval = -0.4     REL(shval) = -inf  REL(-shval) = +inf
```

and measured *inside* `REL`, one call earlier:

```
REL ps/eff 0000000a 0000000a m=bfd99999a0000000     ps=10, EFFPS=10, m=-0.4
```

So **`REL` receives the right argument, computes with the right divisor, and
returns an infinity.** `m *= (float)10/10` is `m * 1.0`; nothing in the body can
produce ±inf. The value is lost at the return, or in how the caller reads it.

Written out on its own — same body, same signature, same call shape — it works:

```c
double RELx(m, ps) double m; int ps;
{
	m *= (float) gsize / ps;
	if (m <= 0.001 && m >= -0.001) return 0; else return m;
}
	-> a=-0.4000 b=0.4000 c=0.0000
```

Both of the obvious differences are eliminated:

1. **`EFFPS` is a function, not a macro** — `main.c:298`, implicit `int` return,
   defined before its uses. No ternary, nothing unusual about the expression.
2. **`shift.c` does include `e.h`**, which has `extern double EM(), REL();`, so
   the declaration *is* in scope at the call and `gencall` should read d0.

So the remaining move is to stop reasoning and read the generated code: compile
`shift.c` with `-S` and look at the two `bl _REL` sites — specifically whether
the result is taken from `d0` or from `x0`, and whether anything between the
call and the `printf` argument slot touches it. Compare against the standalone
version above, which works; the diff between those two pieces of assembly is the
bug.

That is a five-minute job and it is the right next step. Everything cheaper has
been tried.