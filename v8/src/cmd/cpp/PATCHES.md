# Patches to V8 `cpp`

Divergences from pristine upstream. Hashes of the originals are in `PROVENANCE`;
`git hash-object` any file here and compare to see whether it still matches.

Four source changes, all permanent (none is stage-0 scaffolding).

---

### 1. LP64: `pperror` / `yyerror` / `ppwarn` truncated pointer arguments

```c
-pperror(s,x,y) char *s; {
+pperror(s,x,y) char *s; char *x, *y; {
```
(and the same for `yyerror(s,a,b)` and `ppwarn(s,x)`)

`x` and `y` were implicitly `int` under K&R rules, and every caller passes a
`char *` — see the `/* VARARGS1 */` lint annotation above the definition. On the
VAX, `int` and `char *` were both 32 bits, so this was correct. Under LP64 the
pointer is truncated to 32 bits on the way in and `fprintf` reads a 64-bit
pointer back out, printing garbage or crashing on any error message.

Verified by triggering the error paths: `Can't find include file %s` and
`unknown flag %s` now print the filename, not a truncated address.

Every `pperror`/`yyerror` format string in the file uses `%s` or no specifier at
all — there are no `%d` callers — so widening to `char *` is exhaustive.

Callers that pass fewer arguments than the three declared (`pperror("no space")`)
now draw a "too few arguments" warning from clang. This is the K&R idiom working
as designed: the extra parameters are never read because the format string does
not reference them. Confirmed at `-O1` against the `token too long`,
`If-less endif`, and `undefined control` paths.

### 2. `fout` initialised at runtime rather than link time

```c
-STATIC	FILE	*fout	= stdout;
+STATIC	FILE	*fout;		/* set in main */
```
plus `fout = stdout;` as the first statement of `main`.

In V8, `stdout` is `&_iob[1]` — a link-time constant address, so the static
initialiser was legal. Under a hosted libc `stdout` is a runtime-initialised
pointer and cannot appear in a static initialiser. Equivalent behaviour, and it
stays correct once we link against real V8 libc in Phase 2b.

### 3. + 4. Target machine: `arm64`

```c
-#if pdp11 | vax
+#if pdp11 | vax | arm64          /* COFF 128 -- signed char */
```
in **both** `cpp.c` (line ~33) and `yylex.c` (line ~4), and a new arm in the
predefine ladder:
```c
+# if arm64
+	varloc=stsym("arm64");
+# endif
```

Adding a machine to these `#if` ladders is the mechanism CSRC used for every new
port, so this is the authentic way to express "this compiler targets ARM64"
rather than a workaround.

The `vax` define did double duty and both halves matter:

* **Signed-char table bias.** `cpp` indexes its character-class tables through
  `(fastab+COFF)[c]`, where `COFF` is 128 precisely so that a *negative* `char`
  lands in the low half of the table. Apple ARM64 keeps `char` signed like the
  VAX; Linux ARM64 does not, so the build passes `-fsigned-char` on both.
* **Predefined macro.** Building with upstream's `-Dvax=1` would make our
  compiler announce itself as a VAX to every program it preprocesses, sending
  them down VAX-specific code paths (including inline VAX assembly).

`yylex.c` carrying its own second copy of the `COFF` block is easy to miss, and
missing it is not a compile error — it silently gives the two halves of the same
program different table biases. The symptom was `#if MANYPROC` in
`<sys/param.h>` failing with "Illegal character M in preprocessor if", because
identifiers stopped being recognised inside `#if` expressions only. These are the
only two copies of the idiom in the V8 tree.

---

## Not source changes

* **1978 yacc action syntax.** `cpy.y` writes actions as `={ ... }`; modern
  yacc/bison requires `{ ... }`. The top-level `Makefile` filters the grammar
  through `sed 's/={/{/g'` on the way to yacc, leaving `cpy.y` pristine. V8's own
  yacc, once built, needs no such filter. (bison accepts `%term` and `%binary`
  as-is.)
* **`_sobuf`.** `cpp` borrows V8 stdio's static stdout buffer when writing to a
  named output file. Supplied by `tools/stage0-compat.c` for now; real V8 libc
  provides it in Phase 2b.
* **`:yyfix` / `rodata.c`.** Upstream splits the yacc tables into a separate
  translation unit compiled with `cc -R` to land them in read-only text. That is
  a VAX memory optimisation with no modern equivalent; skipped.
