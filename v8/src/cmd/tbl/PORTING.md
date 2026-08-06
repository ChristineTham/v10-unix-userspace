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
