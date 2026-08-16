# libl — porting notes

V8's lex library, imported in Wave A2 batch 2d.  The **second** library this
port has taken, after `libtermlib`.  Five sources, none over 900 bytes.
Upstream: `v8/usr/src/lib/libl`.

## Changes: NONE

All five are **byte-identical to upstream**, and the archive is built the way
upstream's Makefile builds it — five objects, `ar`, installed as
`/usr/lib/libl.a` so `cc -ll` resolves it through the driver's `libpath()`.

## What consumes it

`pp(1)`, and only `pp`, and only `yywrap()`.  A lex-generated scanner calls
`yywrap()` at end of input; a program that does not define its own gets the
library's, which returns 1.  `awk`'s scanner does define one, which is why awk
links without `-ll`.

## `main.o` is in the archive and must never be pulled

`libl/main.c` is four lines and defines `main()`:

```c
main(){ yylex(); exit(0); }
```

It exists for `lex spec.l && cc lex.yy.c -ll` with no main of your own.  Every
lex *program* also defines `main`, and the two do not collide — a linker
searches an archive only for symbols still undefined, and `pp.o`, an explicit
object on the link line, has already satisfied crt0's reference before the
archive is reached.  Upstream relies on exactly that.

`tests/wavea` asserts the member is present rather than asserting the absence
of a collision, because a collision would fail the link loudly; what is worth
checking is the premise that the member is really there to collide.

This is the granularity rule `shim/v8sys/stubs.c` records from the other side —
`exit` and `_exit` shared one member and a program defining its own `exit()`
could not link.  Here the granularity is upstream's and is already right.

## Audited and deliberately unchanged: `yyless.c:11`

v8cc warns `illegal pointer/integer combination, op =` at

```c
yyless(x)
{
	register char *ptr;
	if (x>=0 && x <= yyleng)  ptr = x + yytext;
	else                      ptr = x;      /* <-- int into char * */
```

`x` is an undeclared K&R parameter, so an `int`, and the `else` arm treats it as
an absolute pointer.  On a VAX both are four bytes and it worked; here it
truncates.  **Not changed**, on `make`'s `meter()` precedent: the arm is
reachable only if a lex program calls `yyless()` with a value outside
`[0, yyleng]`, `yyless` is documented as taking a count, and **nothing in the
tree calls it at all** — measured, the only occurrences are `libl`'s own
Makefile.  A change to `src/` must be forced by the target, and an unreachable
truncation is not forced.

Worth knowing when it becomes reachable: the first `.l` in this tree to call
`yyless()` makes this live, and the fix is a cast, not a rewrite.

## Still open

`reject.c` and `allprint.c` have no consumer either.  They are here because the
archive is the unit V8 shipped, not because anything needs them — the same
relationship `ul` has to the parts of `libtermcap` it does not call.
