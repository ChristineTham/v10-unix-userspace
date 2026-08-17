# libF77 and libI77 — the Fortran runtime

`f77` stage 1 of four. This document covers **both** libraries, because they are
one deliverable: the f77 driver's library list is a fixed
`{ "-lF77", "-lI77", "-lm", "-lc" }` (`drivedefs`), in that order, and neither
half is useful alone.

| | files | lines | imported |
|---|---|---|---|
| `src/libF77` | 121 | 1819 | `v8/usr/src/libF77` |
| `src/libI77` | 33 | 3788 | `v8/usr/src/libI77` |

Both are **siblings of `usr/src/lib`, not children of it** — the same trap
`usr/src/libplot` sprang, where every survey read the child and missed seven
libraries. `tools/import.sh` maps them to `src/libF77` and `src/libI77`.

## What f77 is, and why only these two are here

f77 is four programs, and the survey that established that is the reason this
step is scoped the way it is:

| component | installs to | lines | new machine-dependent code |
|---|---|---|---|
| `f77` driver | `/usr/bin/f77` | 1268 | `drivedefs` — paths only |
| `f77pass1` | `/usr/lib/f77pass1` | ~13000 | `machdefs` + `vax.c` (650) + `vaxx.c` (42) |
| **`/lib/f1`** | `/lib/f1` | 6441 | `UClocal2.c` (1172) + `UCtable.c` (837) + `order.c` (527) |
| `libF77.a` + `libI77.a` | `/usr/lib` | 5607 | ~none |

`f77pass1` emits **pcc intermediate code**, not assembly, and `/lib/f1` is a
separate compiler that turns it into assembly — `pcc1/pcc/makefile`'s install
arm is `mv fort ${DESTDIR}/lib/f1`, built from `pcc1/mip` compiled `-DFORT`.

**`/lib/f1` cannot reuse `compiler/ccom-arm64/`, and the two `mfile2` files say
why in their first forty lines.** pcc1 matches on *shapes and cookies* —
`SAREG`, `SOREG`, `SNAME`, `OPSIMP`, `INTAREG`, nine cookies. pcc2 — V8's ccom,
hence this port's back end — matches on *types*: `TCHAR`, `TSHORT`, `TSTRUCT`,
`optab.tyop/ltype/rtype`, three cookies. f77's own `README` says it outright:
*"f77 is a pcc1 compiler. c is a pcc2 compiler these days."* That sentence reads
as a remark about stabs; it is a remark about the whole back end.

### And that paragraph said "different compilers", which is too strong — measured

The sentence above was written before the two `manifest` files were compared, and
the comparison narrows the claim considerably. **The operators are the same
language**; it is the pass-2 *algorithm* that differs.

| | |
|---|---|
| pcc1 numbered names | 128 |
| ours | 171 |
| shared by name | 118 |
| **identical in name AND number** | **110** |
| disagree | **8** |

And the eight are one fact, not eight: pcc2 **widened the type-encoding field by
one bit**. `BTSHIFT` 4→5, and `BTMASK`, `PTR`, `FTN`, `ARY` all follow from it;
the remaining three are `CAST`/`INIT` renumbered by 3 and `UNDEF` 0 vs 17. f77's
README predicted precisely this — *"the pcc internal representation of types
(which is put out in the type field of the stab) has changed too"* — and it is
the one sentence of that README I had taken at face value.

What genuinely differs is the node and the algorithm:

```
pcc1 in: { op, rall, type, su,   name, stalign, left, right }
ours in: { op, goal, type, cst[NCOSTS], name, pad, left, right }
```

`rall`/`su` is register allocation plus a Sethi-Ullman number, matched against
shapes; `goal`/`cst[]` is a cost vector matched against types. So `UCtable.c`
really cannot be reused and `compiler/ccom-arm64/local2.c` really does implement
a different interface.

**But it makes a cheaper path real, and task #9 carries it.** Rather than writing
a new instruction-selection table in pcc1's shape (~2500 lines), a translator
could read f77's text intermediate, build **our** NODEs — 110 opcodes pass
through unchanged, 8 map, and the type word needs a per-level 4→5 bit shift —
and hand them to `p2compile()`. That is a few hundred lines.

### And this port's pass 2 is nearly separable, which is what makes it tractable

Measured on the built objects. `reader.o local2.o gencode.o emit.o printx.o
t2print.o xdefs.o catch2.o` define **139** names and need **34** — of which
**20 are libc** (`printf`, `malloc`, `memcpy`, the stack-check symbols) and only
**fourteen** come from pass 1:

```
arm64_aggparam cerror codgen dope ftitle getlab maxarg
mkdope node opst pjwend pjwreader talloc tfree
```

Most are small. `talloc`/`tfree`/`node` are the node allocator; `dope`/`mkdope`/
`opst` are the operator dope *vector*, a table; `getlab` is a label counter;
`cerror` the error reporter; `ftitle` a filename; `maxarg` the argument-area
size; `arm64_aggparam` this port's own aggregate-parameter recorder; `pjwend`/
`pjwreader` its debug hooks.

So `/lib/f1` is **a reader plus fourteen glue definitions**, not a second code
generator. Still unmeasured: whether `p2compile()` wants globals beyond those
fourteen, and whether the cost model needs anything pass 1 currently
initialises.

It cannot be *tested* alone, because nothing produces the intermediate file
until `f77pass1` exists — so stages 3 and 4 should land together, and task #12's
INTEGER-width decision gates stage 3's correctness.

So these two libraries are the only portable part, and they are the only part
buildable with the tools that exist today.

## The headline: V8's shipped `libI77.a` could not link

`err.c:81` and `err.c:97` call `setvbuf`. `wrtfmt.c:48`, `wrtfmt.c:58` and
`wsfe.c:41` use `_bufend(cf)`, which libI77's own `stdio.h` defines as
`_bufendtab[(p)->_file]`. Searched across **every `.a` Bell Labs shipped**:

| symbol | found in |
|---|---|
| `_sobuf` | `lib/libc.a`, `usr/lib/11libc.a` — resolves |
| `setvbuf` | `usr/lib/libI77.a` **and nowhere else** |
| `_bufendtab` | `usr/lib/libI77.a` **and nowhere else** |

Two unresolvable symbols, so no Fortran program on a real V8 could reach `ld`'s
exit. libI77 is a System V library dropped into V8 and never reconciled, and
`err.c:93` is upstream's own comment — *"IOLBUF and setvbuf only in system 5+"*
— sitting four lines above an **unguarded** call to it.

Reproducing that faithfully would mean shipping something unusable, so the port
closes it in layer 2: `shim/libI77/sysv.c`, archived **into `libI77.a`** because
the driver's liblist has no fourth name and because putting `setvbuf` in
`libv8c` would invent a C library V8 never shipped. `shim/libm/dummy.c` is the
precedent.

Measured after: the two archives import 66 external names, and exactly **two**
are unsatisfied by `libv8c` + `libv8stubs` + `libv8sys` — `MAIN__`, which is
undefined by design because f77pass1 generates it per program, and `environ`,
which `crt0.o` defines and which is therefore not in any archive.

## `libI77` ships its own `stdio.h`, and it must never be compiled against

It is System V's, and it disagrees with V8's about layout:

| | V8's `<stdio.h>` | `libI77/stdio.h` |
|---|---|---|
| `_flag` | **`short`** | **`char`** |
| offset of `_file` | 26 | **25** |
| `_NFILE` | 120 | 128 |
| `_IOLBF` | 0200 | **0100** (which is V8's `_IOSTRG`) |
| `_IORW` | 0400 | **0200** |
| field order | `_cnt` first | `_cnt` first **only `#if vax`** |

`libI77.a` and `libv8c.a` disagreeing about `FILE` by one byte is the DIRSIZ
trap arriving through a vendored header, and it is the mirror of the rule
`shim/kern/h/param.h` already states from the other side: a header that
redeclares a type libc owns must not be left to include order.

Upstream's makefile is `CFLAGS = -I. -g` and therefore **did** read it — the
shipped `libI77.a` proves it, carrying `_bufendtab` and `_setvbuf`.

**The `-I` turns out not to be needed anyway**, which is what makes the
decision cheap:

- `fio.h`, `fmt.h`, `lio.h` are all `#include "..."` and resolve by the
  includer's own directory.
- Exactly one file needs anything else: **`ecvt.c`**, the only includer of
  `<nan.h>` and `<values.h>`.
- `ecvt.c` is also the only file that can *safely* be given a flag — measured,
  it mentions `FILE`, `printf`, `getc`, `stdout`, `stderr` and `stdin` **zero
  times**, so `libI77/stdio.h` cannot reach it.

`tests/deps` asserts this on **content, not on a make edge**: these objects have
no `.d` files, so a `nodep` would pass whether or not the header were read —
the `shim/kern/h/buf.h` lesson, where a case stayed green while the header
named in it was no longer the one the compile opened. The discriminator is
`_NFILE`, which `cpp` reports directly as `_iob[120]` (V8) against `_iob[128]`
(libI77), and both directions are asserted so the case cannot be vacuous.

## `ecvt.c` gets a staging directory holding one header

Pointing `-I` at libI77's source would also pick up its own `values.h`, which
has three machine arms — `u3b`, `vax`, `gcos` — and no arm64, so `_DEXPLEN` and
`_HIDDENBIT` come out undefined. Measured: the build fails.

So `nan.h` alone is copied to `$(BUILD)/libI77/inc/`, and `<values.h>` falls
through to the patched copy in `/usr/include`.

**`nan.h` is needed only for an empty macro**, which is worth saying because it
reads like a real dependency. `ecvt.c:44` is `KILLNaN(value)` with upstream's
own comment *"(3b only)"*; outside `#if u3b` the header's else-arm defines it to
nothing. Identical on arm64 and on a VAX — nothing to port, only a file that has
to exist for cpp to find.

## `values.h` needed a fourth machine arm — see `src/include/PORTING.md`

`cc` here predefines `unix` and `arm64`, measured. None of upstream's three arms
is taken, and the failure is loud rather than a wrong number, which is the good
direction. The arm is IEEE 754 and **`DMAXPOWTWO` had to be restated**: the
generic formula at the bottom of the file assumes the mantissa is wider than a
long, and under LP64 it is not —

| machine | `BITS(long)` | `DSIGNIF` | second shift |
|---|---|---|---|
| VAX | 32 | 56 | 25 |
| arm64 | 64 | **53** | **−10** — undefined, evaluated at run time by `ecvt.c:64` |

## `_bufend` reaches a real function through an object-like `-D`

V8's cpp has no function-like `-D`: measured, `cc -D'_bufend(p)=...'` leaves the
name unexpanded and ccom reports *"call of non-function"*. The object-like
rename does work, and the cast is not decoration —

```
I77_BUFEND = '-D_bufend=(unsigned char *)v8_bufend'
```

Without the cast the rename produces a call to an **undeclared** function, so
its result is implicit `int`, and all three sites are `cf->_ptr + n <
_bufend(cf)` — pointer against int. v8cc said so on all three: *"illegal
pointer/integer combination, op <"*. That is the truncated-pointer-return class
arriving through a flag rather than through source. A cast binds looser than a
call, so `(unsigned char *)v8_bufend(cf)` casts the *result*.

There is no declaration to add instead: `wrtfmt.c` and `wsfe.c` include only
`"fio.h"`, `"fmt.h"` and V8's `<stdio.h>`, all three of which are layer 1.

## The one genuine LP64 defect: `signal_.c`

Eight lines, three truncations:

```c
signal_(sigp, procp)
int *sigp, (**procp)();
{
int sig, proc;            /* proc is an int */
sig = *sigp;
proc = *procp;            /* a function pointer stored in 32 bits */
return( signal(sig, proc) );
}
```

A Fortran `CALL SIGNAL` installed a handler at a truncated address — a
guaranteed fault on delivery. Exact on a VAX. Fixed by declaring `proc` as
`int (*proc)()`; both uses are unchanged. v8cc had flagged it: *"illegal
pointer/integer combination, op ="*.

The **return** direction is still narrow and is not fixable here — `signal_` has
an implicit `int` return and a Fortran caller receives it in an `INTEGER`, so
widening would not help. Every caller in this tree uses `CALL SIGNAL` and
discards it. Recorded rather than changed.

## Five real symbol collisions with `libv8c`, reconciled by link order

`cabs`, `sinh`, `cosh` (libF77) and `ecvt` (libI77) — plus `tanh` — are all
defined by `libv8c` as well. `-lF77 -lI77` preceding `-lc` is what makes the
Fortran ones win, and that is upstream's own arrangement rather than luck.

**`cabs` is not even the same function.** `libc/math/hypot.c` declares
`cabs(struct complex arg)` — one struct by value — where `libF77/cabs.c`
declares `cabs(double real, double imag)`. A Fortran program that got libc's
would read its imaginary part out of the caller's second slot.

The first sweep for this compared source **filenames** against **symbols** and
was wrong in both directions: 7 candidates, 3 false positives (`rewind.c`
defines `f_rew`, `open.c` defines `f_open`, `close.c` defines `f_clos`), and it
**missed `cosh`**, which is defined in `sinh.c`. 43% false positives and a live
miss, from keying on the wrong name.

## What was audited and deliberately not changed

- **`main.c:20-21`**, `if( (int)signal(SIGQUIT,sigqdie) & 01)`. The cast
  truncates a function pointer, but the test is on **bit 0**, which survives
  truncation, so the answer is the same on both machines. Upstream's own
  `SIG_ERR` confusion (`(int)-1 & 01` is 1, so an *error* reads as "was
  ignored") is unchanged: it was there on the VAX too.
- **`err.c:95-96`**, `extern char * _sobuf; setbuf(stdout, _sobuf);`. V8's libc
  defines `_sobuf` as an **array** (`flsbuf.c:12`), so this reads its first
  bytes as an address. Measured harmless: `_sobuf` is in BSS and reads as 0, so
  `setbuf(stdout, NULL)` sets `_IONBF` (`setbuf.c:12`) and stdout is unbuffered
  where upstream wanted line-buffered. Identical on a VAX, which read four bytes
  instead of eight. Not forced by the target.
- **The object lists are upstream's, not a glob.** libF77's directory holds 121
  `.c` and its ten object macros name 118 — `derf_`, `derfc_`, `erf_`, `erfc_`,
  `mclock_`, `outstr_`, `subout` are in the directory and in no macro. libI77's
  `OBJ` names 27, and the bottom of its makefile still carries dependency lines
  for `stest.o` and `ftest.o`, which have **no sources at all**. A glob would
  have built files upstream does not.

## What the probe proves, and what it cannot

`tests/wavea/f77probe.c` is the consumer these libraries do not have yet — the
`tgotoprobe.c` shape, for the third recorded reason a mutation fails to fire:
not dead code, not a vacuous case, just nothing calling it.

**`XB` is the load-bearing token.** `_bufend`'s three call sites are all inside
*"can I skip inside the buffer, or must I fseek?"*, and a purely forward write
never reaches them, so the format is `(A2,T1,A1)` — `T1` moves the cursor back
to column 1 over an already-written `AB`.

**What no link test can see: `setvbuf`.** Measured during mutation — with the
shim object removed from the archive, `_v8_bufend` is undefined and the link
fails, but `setvbuf` **resolves silently from `-lSystem`**, because macOS has
one. That is this port's documented "a missing libc function does not fail the
link" hazard, and it means the link case covers one of the shim's two halves
and not the other.

## Mutation results

| mutation | effect |
|---|---|
| `-I$(I77SRC)` on `ecvt.o` (upstream's own `CFLAGS = -I.`) | **build fails** — `_DEXPLEN undefined` |
| drop `-D_bufend=...` | **build fails at link** — `__bufend` undefined from `wsfe.o`, `wrtfmt.o`; 6 wavea cases |
| drop `sysv.o` from `libI77.a` | 6 wavea cases + 2 deps cases |
| drop `v8_bufend`'s NULL arm | **fires nothing, and that is correct** — see below |

**The fourth is the informative one.** The claim in the comment was that
returning `_base + BUFSIZ` unconditionally lets an unbuffered stream advance
`_ptr` into low memory. The first half is true and the consequence is not: with
`_IONBF` every `putc` goes through `_flsbuf`'s unbuffered arm, which writes the
character directly and never dereferences `_ptr`. Measured with a probe calling
`setbuf(stdout, 0)` first — which is what `f_init` does on a tty
(`err.c:94-96`) — both spellings print the same bytes. The arm is kept, because
advancing `_ptr` off NULL is undefined whether or not anything reads it, and
the comment now says it is defensive rather than load-bearing. **No case is
written for it**, deliberately: there is nothing observable to assert.

## Open

- **`ftnint`/`ftnlen` are `long`, which is 8 bytes here and 4 on a VAX.** Safe
  today because both ends of the convention are ours. **f77's arm64 `machdefs`
  must agree when stage 3 lands** — upstream's says `SZLONG 4` and `TYLENG =
  TYLONG`, so generated code would pass 4-byte hidden lengths to a library
  expecting 8. This is the first thing to check in stage 3.
- **`libI77/values.h` and `src/include/values.h` are now two copies**, differing
  by an SCCS stamp, one machine name (`u3b2`), and this port's arm64 arm. Only
  the second is on any include path. If a second libI77 file ever includes
  `<values.h>`, the staging directory is where that gets decided.
- **`libI77` has no read path exercised.** The probe writes; `rsfe.c`,
  `lread.c`, `rdfmt.c` and `iio.c` have no consumer at all. A reading probe is
  cheap and is not written yet.
