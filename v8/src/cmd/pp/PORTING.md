# pp(1) — porting notes

Wave A2 batch 2d.  Pretty-prints C source as troff intermediate output.  One
`.c` and one `.l`.  Upstream: `v8/usr/src/cmd/pp`.

## Changes: NONE

Both source files are **byte-identical to upstream**.

## The idiom: a lex file with NO yacc file

Every other lexer in this tree (`awk`, `grap`, `pic`, `expr`'s embedded
scanner) is half of a yacc/lex pair and depends on `y.tab.c` for its token
numbers.  `scan.l` stands alone: its tokens are five `#define`s in `pp.h`.  So
the generated `lex.yy.c` has exactly one generated input and no grammar edge,
which is what `tests/deps` asserts.

That makes `pp` **the first consumer of `libl`**, V8's lex library, imported in
the same batch from `usr/src/lib/libl` — the second library this port has taken
after `libtermlib`.  Only `yywrap()` is actually pulled in; `scan.l` uses
`input()`, which the generated scanner defines itself, and neither `yyless()`
nor `REJECT`.

## Measured and deliberately unchanged

**`pp.c:11`'s `BMASK redefined` is upstream's own diagnostic.**  `pp.c` includes
`<sys/types.h>`, upstream's `sys/types.h:35` includes upstream's
`sys/param.h`, and that defines `BMASK 0777` — then `pp.c:11` defines its own
`BMASK 0377`, a byte mask, with the comment *"because we can't always say
unsigned char"*.  Two unrelated constants sharing a name.  pp's wins, because
it comes last; V8's cpp warned about this in 1985 for the same reason.

## Fonts: the default cannot be reproduced from source

`pp`'s default font is Memphis (`pp.c:95`) and it emits

```
pp: can't open /usr/lib/font/dev202/Memphis.out
```

before falling back.  This is a **data** limit, not a port defect.  V8 ships
143 font tables in `usr/lib/font/dev202` as binaries; `usr/src/cmd/troff/dev202`
holds the *source* tables for about eighty of them and **Memphis is in neither
that directory nor anywhere else in the archive** — the `more(1)`/`pg(1)`
category, shipped compiled with no source.  This port installs exactly the
eleven tables troff's own `DESC` mounts, derived from the `fonts` line rather
than listed, and Memphis is not one.

`pp -fR` uses Times Roman, which is mounted, and produces complete output —
`x T 202`, `x res`, `x init`, the font mounts, and positioned text.  That is the
case `tests/wavea` asserts.

## Still open

Installing the Memphis family would take five binary tables out of
`third_party` as data, on `/etc/termcap`'s precedent.  Deferred: it is a
question about which fonts the world should carry, not about `pp`.
