# tbl

All 23 files compile with V8's compiler and link freestanding, with no source
changes. It does not yet run.

## It runs

`tbl | nroff` formats a table:

```
Name    Value
alpha   1
beta    2
```

## The bug: reg() returns a pointer, and ct was an int

`reg(col, place)` is declared `char *` in `tr.c` and returns a two-character
number-register name out of the `nregs[]` table — `"40"`, `"4q"`, `"5x"`.
`putline` stored that in `ct`, an `int`, and handed it straight to a `%s`:

```c
int c, lf, ct, form, ...;
...
ct = reg(c,CLEFT);
fprintf(tabout, "\h'|\n(%2su'", ct);
```

On the VAX an int held a pointer exactly. Under LP64 it holds half of one, and
`_doprnt` walked off the truncated address looking for a NUL — tbl died on its
first table row reading `0x1d7fe`.

**The compiler cannot rescue this one.** `PTRCONVFULL` makes a pointer-to-int
*conversion* lossless, but `ct` is a genuine `int` automatic: the value is
stored into four bytes and reloaded from four bytes, and nothing about that is a
conversion. It needs a variable of the right type, so the register name now has
its own `char *ctreg`. `ct` stays an int, because it is genuinely a character
everywhere else in the function — `switch (ct=fullbot[nl])`, `ct=='a'`,
`ct=ctype(...)`. The two uses never overlapped; they only shared a name.

## How it was found, which took longer than it should have

The first backtrace said `_doprnt`, and I read that as "our printf is broken".
It was not — the argument was. Eliminated in order, each by direct measurement:
the read loop, the first `fprintf`, `setinp`, `fgets`, eleven of `tableput`'s
seventeen stages, `deftail`, both cell loops, `point()` (which looked exactly
like the culprit — an implicit-`int` parameter holding a pointer, the shape that
broke `look(1)` — and turned out to be fine, because `acctype()` reads an int
parameter at full width), and `left()`.

What finally named it was disassembling at the fault:

```
->  0x100012c4c <+3788>: ldrsb  x9, [x9]
    0x100012c50 <+3792>: cmp    x9, #0x0
    0x100012c54 <+3796>: b.ne   0x100012c10
```

— a NUL scan, so a `%s`. From there it was one grep for the first `%s` after the
last line that printed.

The lesson is the same one this port keeps teaching, and I keep having to
relearn: **the backtrace names where the program died, not what was wrong.**
Bisecting from the top wasted most of the time; disassembling one instruction at
the fault answered it.

## A note on the file list

`tbl`'s header is called `t..c`, which is not a typo — the `.c` files include it
as `#include "t..c"`. Anything that builds the directory by globbing `*.c` will
compile the header as a translation unit. It is harmless, but it is why the
Makefile lists the objects explicitly.

## Two live SIGSEGVs, found by Bell Labs' own test suite

`src/cmd/tbl/samples.a` is 54 tables written by tbl's authors.  It came in with
the import and was **deleted from git by `.gitignore`'s `*.a`** — a rule for
build outputs matching an `ar` source bundle — so it had never been run.  On its
first run **16 of the 54 died on SIGSEGV**, while `tests/wavec` was green.

Both are the same class and both are one word.

### 1. `tm.c` — `int dpoint` holding the address of a decimal point

`maknew()` splits a numeric field at its decimal point, and `dpoint` is
upstream's flag-and-pointer: tested as a boolean four times and assigned
`(int)str`.  Exact on a VAX; here the cast keeps the low 32 bits and
`str=(char *)dpoint` hands the truncated half back to be dereferenced.

Measured: `EXC_BAD_ACCESS at 0x1e6058a9` in `maknew` — a 32-bit value, which is
this class's signature.

**It is the `n` column**, tbl's characteristic feature — numeric alignment —
and `maknew` is called only for one (`t5.c:72`, `t9.c:49`).  Twenty bytes
reproduce it:

```
.TS
n.
3.5
.TE
```

`l`, `c`, `r` and `a` are all clean, measured, which is exactly why this
survived: `tests/wavec` exercises tbl with hand-written tables and **had never
used an `n` column once**.

### 2. `t5.c`/`t0.c`/`t..c` — `int leftover` holding the overflow line

The same idiom in the same program.  `t5.c:15` is `leftover=0` and `t5.c:25` is
`leftover=(int)cstore`; `t7.c:18` tests it as a boolean and `t9.c:14` passes it
to `domore(dataln) char *dataln;`, which hands it to `prefix()`, which
dereferences it.  `EXC_BAD_ACCESS at 0x4ac8141` in `prefix`.

**Three declarations move, not one** — the definition in `t0.c`, the `extern` in
`t..c`, and the cast in `t5.c`.  `t..c` is one of the tree's `#include`d
non-headers, so unlike qed's `savint` the coupling here is real in the build
graph and every object sees the change.

**And the cast has to move with the declaration.**  `long leftover` alone fixes
nothing: `(int)cstore` truncates *before* the value is widened.  The explicit
cast is the truncation, not the storage.  Same trap as `tm.c`.

`long` rather than `char *` in both, because the boolean uses are upstream's
idiom and a pointer type would need three more edits each to keep them.  Same
one-word fix as yacc's `#define YYSTYPE long` and qed's five signal-handler
variables.

### Audited and deliberately unchanged

`t8.c:302` is `s = (int)table[lin][c].col;` — the identical idiom, and it does
**not** need the fix.  `point()` at `tc.c:57` is `return(s>= 128 || s<0)`, a
test for "is this a pointer or a small marker", and every subsequent use of `s`
is `%c` on the *marker* branch: the pointer branch does `continue`.  So a
truncated pointer changes no answer unless its low 32 bits fall under 128,
which no reachable allocation produces.  Recorded rather than changed, on the
`make`/`meter()` precedent.

### What is still broken: sample32

One of the 54 still dies, and it is a **different class** — `permute()` at
`t5.c:124` writes through a null row pointer (`EXC_BAD_ACCESS at 0x8`, not a
32-bit value) while walking a `^` vertical span in a table that also carries
full-width rules and `linesize(24)`.  Undiagnosed; `tests/wavec` asserts that it
*still* dies, so fixing it requires removing the name from `KNOWN_TBL_FAIL` in
the same commit.
