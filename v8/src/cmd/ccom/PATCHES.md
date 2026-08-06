# Notes on V8 `ccom`

Written after the stage-0 bootstrap; the findings below shape Phase 1b. Three
source changes now, one unguarded and two behind target macros — everything from
"What builds" down still describes the stage-0 state.

---

## Source changes

### `common/optim.c`, in `sconvert()`: two guarded changes

Both are `# ifdef`-ed on macros that `compiler/ccom-arm64/macdefs.h` defines, so
a target where the widths and the instruction set agree with the VAX is
unaffected. The reasoning is in the code, at length, in both places.

- **`PTRCONVFULL`** — a pointer converted to `int` keeps all its bits. `SZPOINT`
  is 64 and `SZINT` is 32; on the VAX they were equal. Truncating broke
  `strspn(3)`'s `return(q-string)`, and fixing only one of the two paths broke
  `fflush(3)` in turn, so it is fixed once, for both.

- **`SIGNCONVKEEP`** — a conversion that changes only signedness is not painted
  onto `/`, `%` or `>>`. Sound for every other operator, because the bits are
  the same either way; wrong for these three, where `gencode.c` reads that type
  to pick `udiv`/`sdiv` and `lsr`/`asr`. Found through `printf("%lx")` of a
  negative long. PLAN.md §4g has the full account.

Worth keeping in view: these are the same seven lines of `sconvert()`, and both
faults have the same shape — a conversion that is a no-op **on the result** is
not necessarily a no-op on what produces it. A third change here should be
suspected of being a fourth.

---

## Source change

### `common/scan.c`: `dimtab[NULL]` → `dimtab[TNULL]`

`NULL` in this line is not a null pointer — it is pcc's *type code* for "no
type", sitting in `manifest.h` alongside `CHAR`, `INT`, `FLOAT` under the comment
"type names, used in symbol table building". V8's `<stdio.h>` defines `NULL` as
plain `0`, so `dimtab[NULL]` was an ordinary integer subscript. Modern `NULL` is
`((void *)0)`, which is not a valid array subscript.

`manifest.h` already defines `TNULL 0` for exactly this concept, and `scan.c:160`
already writes `stab[i].stype = TNULL` eight lines earlier — so this is the
file's own idiom, not an invention. Same value, clearer meaning.

This is the only place in ccom that uses `NULL` as an integer; the other
occurrences are genuine pointer contexts.

---

## What builds

All of the machine-independent pass 1 compiles and **runs** on ARM64:
`cgram.c` (the grammar), `pftn.c`, `trees.c`, `scan.c`, `optim.c`, `reader.c`,
`pjw.c`, `lookup.c`, `xdefs.c`, `common1.c`, `catch2.c`, `t2print.c` — plus most
of the machine-dependent layer: `local.c` (which holds `main`), `local2.c`,
`debug.c`, `printx.c`, `lcatch2.c`, `memcpy.c`.

Verified behaviour, not just compilation:

```
$ ccom-pass1 bad.c bad.s
"":1:syntax error
expected a NAME in list
"":1:saw {

$ ccom-pass1 bad2.c bad2.s          /* struct + int */
"":1:operands of + have incompatible types
```

The 1985 grammar and type system are intact on a machine from 2026.

## What does not build: `vax/gencode.c`, `vax/genaux.c`

22 errors, all one idiom. `doit()` takes its `dest` parameter as a 4-byte `ret`
struct by value:

```c
ret
doit(p, flag, dest, regmask)
NODE *p;
ret dest;
```

and every caller passes literal `0`:

```c
t = doit(p->in.left, VALUE|USED, 0, regmask);
```

An all-zero struct punned as an integer. Legal in 1985 because K&R had no
prototypes to check the call against, and `ret` is exactly `int`-sized. clang
sees the definition in the same translation unit and refuses.

**Deliberately not fixed.** Both files are the VAX instruction emitter, and
Phase 1b replaces them with `compiler/ccom-arm64/`. Patching 22 sites in code
scheduled for deletion buys nothing. The `ccom-vax` make target is kept so the
failure stays visible rather than quietly forgotten.

## The pass-1 → pass-2 contract is three symbols

Linking pass 1 alone leaves exactly `gencode`, `Pflag`, and `bbcnt` undefined.
`gencode(p)` is called once per statement tree from `common/pjw.c`, after
register allocation. `compiler/ccom-arm64/gencode.c` currently stubs all three,
which is what makes `ccom-pass1` linkable.

A three-symbol backend interface is unusually small, and it is small because V8
threw away pcc's table-driven pass 2 in favour of a recursive generator. Good
news for the retarget: there is one seam, not a table format to reverse-engineer.

## Confirmed: the varargs ABI decision in PLAN.md §4 is load-bearing

`ccom-pass1` parses correctly and then dies with SIGSEGV in `sprintxl`,
dereferencing address `0x2`. The cause is V8's own printf:

```c
printx(fmt, list)
char *fmt; long list;                    /* first variadic argument */
{
	bufpt = sprintxl(bufpt, fmt, &list);   /* take its address... */
}

sprintxl(str, fmt, lp)
register long *lp;
{
	case 's':
		for (p = (char *)(*lp++); *p;)     /* ...and walk forward */
```

This is the `varargs.h` idiom — take the address of the last named parameter and
walk forward through the argument block — and it is everywhere in V8, because on
the VAX `calls` pushed all arguments contiguously onto the stack.

Under AAPCS64 the first eight arguments arrive in `x0`–`x7`. `&list` points at
whatever lone stack slot clang spilled that one parameter to; the following
arguments are not behind it. `sprintxl` walks into garbage.

PLAN.md §4 already specifies the fix — v8cc's prologue spills `x0`–`x7` into a
contiguous block so `&arg` arithmetic works — and this crash is the evidence that
it is mandatory rather than merely tidy. Note the payoff: once v8cc compiles
`printx.c` itself, this code works **unmodified**. The bug exists only in the
stage-0 window where clang, which honours AAPCS64 strictly, compiles V8 source.

Practical consequence for Phase 1b: bring the backend up far enough to compile
`printx.c` early, because a great deal of ccom's own diagnostic output goes
through `printx`.
