# hoc(1) — porting notes

Wave A2 batch 2d.  Kernighan and Pike's programmable calculator: a yacc grammar
and four sources.  Upstream: `v8/usr/src/cmd/hoc`.

## Changes: NONE

All five source files are **byte-identical to upstream** and it compiles,
links (`nm -u` empty) and runs.  Measured:

```
$ hoc
1+2                          3
x = 3
x*x                          9
sqrt(2)                      1.4142136
func sq() { return $1*$1 }
sq(7)                        49
for (i=1; i<4; i=i+1) print i
                             1 2 3
```

## What it exercises that nothing else did

**`sqrt(2)` goes through a function POINTER**, which is why it is the case
`tests/wavea` cares about.  `hoc.h`'s `struct Symbol` carries a union with
`double (*ptr)()`, `init.c` fills it with the maths routines, and `code.c`
calls through it.  So the answer is only right if both halves of the
floating-point work are correct — the `float atof()` declaration that read `s0`
where a `double` comes back in `d0`, and v8cc passing doubles in `x0`-`x7`
against AAPCS64's `d0`-`d7`.  Neither had ever been exercised through an
indirect call; `pic` and `grap` call the maths directly.

`-lm` is not passed and does not need to be.  Upstream's makefile says
`$(CC) $(OBJS) -lm -o hoc`; V8's maths lives in libc, and `shim/libm/dummy.c`
reproduces the 216-byte empty archive V8 actually shipped, so the flag links
nothing on either machine.

## The build idiom, recorded rather than copied

Upstream's makefile carries a trick nothing else in this tree does:

```make
x.tab.h:	y.tab.h
	-cmp -s x.tab.h y.tab.h || cp y.tab.h x.tab.h
code.o init.o symbol.o:	x.tab.h
```

Nothing includes `x.tab.h` — all three sources say `#include "y.tab.h"`.  It is
not a vestige: it is **content-addressed rebuild avoidance in 1984 make**.
`x.tab.h`'s mtime moves only when the token numbers actually change, so
re-running yacc without changing the grammar does not recompile three objects.
The leading `-` is what lets the first run's `cmp` fail.

This port does not reproduce it.  Our objects depend on `y.tab.c`, which is
correct but coarser; GNU make 3.81 has no grouped targets, so `y.tab.h` is not
a target of its own here (the awk block in the Makefile says why), and adding a
second header target to save three compiles would be a rule that races under
`-j`.  `tests/deps` asserts the correctness half — an edit to the grammar
reaches `init.o` — and this note records the optimisation that was declined.

## Still open

Nothing.  `hoc` is feature-complete here.
