# f77 — the driver (stage 2 of four)

`f77` is four programs. This document covers the **driver**, `/usr/bin/f77`.
`src/libF77/PORTING.md` covers the runtime libraries (stage 1); `f77pass1` and
`/lib/f1` are stages 3 and 4 and are not built.

| | |
|---|---|
| imported | `v8/usr/src/cmd/f77`, 46 files |
| byte-identical to pristine V8 | **43 of 46** |
| deviations | `defines` (one token), `driver.c` (five sites), `drivedefs` (a second arm) |
| new files | `arm64defs`, `arm64x.c` |
| built from | `driver.o arm64x.o`, as upstream builds `driver.o vaxx.o` |

## It is not only a driver

Upstream links it from two objects for a reason: `driver.c` is 1226 lines and
from `:1159` it holds `dodata()`, the **DATA statement emitter**. `f77pass1`
writes an intermediate file of `name offset length type [value]` records; the
driver sorts it with `sort(1)`, walks it, and lays initialised variables out as
assembler directives. So the driver carries real machine-dependent output, and
`vaxx.c` / `pdp11x.c` are each one machine's four directive printers.

`arm64x.c` is ours, and three of its four functions are byte-for-byte what
`vaxx.c` does. Only `prchars` differs: clang's arm64 assembler does not accept
the VAX assembler's leading-zero octal, so `.byte 0101,0102` would be read as
**decimal** 101 — a plausible wrong answer rather than an error, which is the
direction that costs a session.

## `SZLONG` is 4, and it is pinned by the float layout

This is the most consequential number in the whole f77 port, and it is the
opposite of what LP64 suggests.

`typesize[]` — which exists **twice, identically**, at `driver.c:1152` and
`init.c:45` — is

```c
{ 1, SZADDR, SZSHORT, SZLONG, SZLONG, 2*SZLONG,
  2*SZLONG, 4*SZLONG, SZLONG, 1, 1, 1 }
```

indexed by `ftypes`. So `typesize[TYREAL]` is `SZLONG` and `typesize[TYDREAL]`
is `2*SZLONG`. libF77's `r_nint` takes `float *` and `d_nint` takes `double *`,
so a Fortran REAL is four bytes and a DOUBLE PRECISION is eight — which
**forces `SZLONG` to 4**.

`ftypes` offers only `TYSHORT` and `TYLONG`, and `machdefs` says
`TYINT = TYLENG = TYLONG`, so Fortran's INTEGER, LOGICAL *and* the hidden
character length are all 4 bytes and cannot be given different sizes.

**And that collides with the runtime.** `libI77/fio.h` spells the hidden length
`typedef long ftnlen`, which is **eight** bytes under v8cc, and 37 libF77 files
spell Fortran INTEGER as `long` too. On a VAX those were four bytes — V8's own
compiler says so in one line, `# define NOLONG /* map longs to ints */` at
`src/cmd/ccom/vax/macdefs.h:20` — so the sources are correct for the compiler
they were written for and wrong for this one.

**And `SZLONG` is pinned TWICE, which is what settled it.** Beyond the float
layout, `lengtype()` at `proc.c:951` hardcodes the length constants: `INTEGER*4`
falls to `goto ret` and returns `TYLONG`, `INTEGER*2` gives `TYSHORT`, `REAL*4`
gives `TYREAL` and `REAL*8` gives `TYDREAL`. So `typesize[TYLONG]` must be 4
whatever the floats said, and no other value of `SZLONG` satisfies both.

The runtime was therefore narrowed — see `src/libF77/PORTING.md`, which records
the 39+4 files and the drop from 153 to 112 byte-identical. `tests/wavea` guards
the width as a relation between the probe's `sizeof`, the `SZLONG` in the
generated `arm64defs`, and the requirement that they agree.

## `ARM64` is a new machine token, and the alternative was measured first

`defines` numbers the machines 2..7 and f77 selects one by comparing `HERE`,
`TARGET` and `FAMILY` against them. `#define ARM64 8` continues that numbering.

The alternative was `-DHERE=VAX -DTARGET=VAX`, and it is worth recording that it
**comes out right on every arm the driver takes** — all six were checked. It is
still a lie: `HERE` means the machine the compiler runs on. Naming the machine
costs one line and leaves the next reader able to see that arm64 was considered,
rather than finding `-DTARGET=VAX` in a recipe and being misled. Same reasoning
as `src/include/values.h`'s fourth arm.

### `HERE` means two different things, and only one of them is about a VAX

Six conditionals in `driver.c`, and the split decided the port:

| site | guards | verdict |
|---|---|---|
| `:122` | `loadargs[3] = "_MAIN__"` | **needed** — a symbol-naming convention. a.out prepended an underscore and Mach-O does too |
| `:128` | `loadargs[3] = "main"` | Interdata, no underscore. Not us |
| `:393` | the per-file progress line | **needed** — what every Unix host printed |
| `:561` | `rmf(obj)` when cross-compiling from a PDP11 | not us |
| `:678` | **the entire fork/execv of the link editor** | **needed** |
| `:688` | the Interdata optimiser | not us |

Every number in that table was **wrong on its first writing**, by between 10 and
120 lines: they were measured before this file's own PORT comments were added,
and the comments pushed everything below them down. That is the
self-invalidating-citation shape at its most literal, and the remedy is the one
already written down — `sed -n "${l}p"` on every citation before committing.

`:678` is the one whose absence would be silent. The three machines listed are
the three Unixes V8 knew, so the test spells *"is this a Unix with fork and
exec"* as a list of machine names — and without `ARM64` there the driver builds,
runs, and **never invokes the link editor at all**. That is the `-DCM_` shape:
a missing `-D` selects an empty arm rather than failing to compile.

`TARGET == ARM64` needs no new arms. It falls into the generic assembler line at
`:553` (`as -o obj asmfname asmpass2`, two inputs), skipping both the VAX's
one-input-file `cat` workaround and the `/lib/c2` peephole.

## `machdefs` is generated, which is upstream's own mechanism

Upstream's makefile has

```make
machdefs : vaxdefs
	cp vaxdefs machdefs
```

so a per-machine defs file **is** the hook f77 provides for a new target. The
port's Makefile copies `arm64defs` instead, and no edit to that rule was needed.
The `machdefs` checked in to the source directory is that rule's output,
committed upstream by accident — measured byte-identical to `vaxdefs`.

**A quoted include tries the includer's own directory first**, so putting ours on
`-I` is not enough: the source directory's copy would win. The recipe therefore
**copies `driver.c` and `arm64x.c` into the build directory and compiles them
there**, beside the generated `machdefs` and the three staged non-headers. That
needs no edit to Bell Labs' include lines, and it is the same fall-through
`shim/kern/dev/tty.h` relies on from the other side.

Mutation-verified: compiling in the source directory instead **fails the build**
(`driver.c:638: liblist undefined`), and `cpp` confirms it saw `SZADDR 4`.

## The link path, and three things clang does not forgive

`ASMNAME` and `LDNAME` are `/usr/bin/clang`, the deliberate exception at
PLAN.md §1 rule 2, spelled exactly as `src/cmd/cc.c` spells it. Three
consequences, all measured:

- **`FOOTNAME` is a V8 path handed to a host binary.** A bare `f77` reported
  `clang: error: no such file or directory: '/lib/crt0.o'`. `v8path()`
  absolutises it against `$V8ROOT`, which is what `setpaths()` in `cc.c:91`
  does and for the same one-sentence reason: clang never sees `rootpath()`.
- **`-lNAME` escapes to the macOS SDK.** `v8lib()` resolves each against
  `$V8ROOT/usr/lib/libNAME.a`, mirroring `libpath()` at `cc.c:160`. That is the
  hole `cc.c` documents at length, where `-lm` answered with a libSystem
  re-export and the link died on an `_errno` with no address.
- **`-lc` is EXPANDED, not dropped.** An earlier draft dropped it the way
  `cc.c` does, and the link died on `__iob`, `__flsbuf` and `__sobuf` —
  because `cc.c` can only drop it by virtue of appending `libv8c.a`,
  `libv8stubs.a`, `libv8sys.a` and `-lSystem` unconditionally afterwards. `-lc`
  **is** V8's libc, so expanding it to those four keeps upstream's liblist
  meaningful.

Plus `-nostdlib -e _v8start`, as `cc.c:534-536` passes them.

### The loader flags needed `-Wl,`, and correcting the spelling was not the fix

`ldname` is clang, not a link editor. Three drafts:

| | |
|---|---|
| `-X` | ld64 answers `warning: -X is obsolete` on **every** link |
| `-x` | **clang reads its own `-x`**, "specify language", and dies with `language not recognized: '-u'` — naming the *next* argument, so the error names neither flag |
| `-Wl,-x` | right |

The middle one is the lesson. Correcting `-X` to Mach-O's spelling looked like a
one-character fix and moved the failure to a different flag entirely, because
the argument was never going to `ld` in the first place. **Read which program
parses a flag before correcting its spelling.** `-u _MAIN__` and `-M` needed the
same routing.

## What was audited and deliberately not changed

- **`driver.c:82-85`**, `sigivalue = (int) signal(SIGINT, SIG_IGN) & 01`. The
  cast truncates a function pointer, and the test is on **bit 0**, which
  survives truncation — so the answer is identical on both machines. Same shape
  as `libF77/main.c:20-21`.
- **`case 'o'` reads the argv terminator.** `aoutname = *++argv` with `-o` last
  gives NULL. **Predicted a SIGSEGV and measured none**: the NULL is only
  *placed* in the `execv` argument array, never dereferenced, so it terminates
  the vector early and clang reports `argument to '-o' is missing`. Not the
  address-0 class, and identical on a VAX.
- **`doload` discards the loader's exit status.** It ends `await(waitpid);` — a
  bare statement, no `if` — so `f77` exits **0** on a failed link. Upstream's,
  on upstream's hardware. Every test therefore asserts the artefact rather than
  the status, which is `rm(1)`'s lesson.
- **`f77 -T` prints `Compiler error in file (null)`.** `infname` is NULL before
  any file is seen, and a VAX printed the empty string. That is task #2, the
  known `printf %s` deviation, with a second instance recorded.
- **`PASS2OPT` is left undefined**, which removes the `/lib/c2` step rather than
  needing a flag: `driver.c` wraps it in `#ifdef PASS2OPT` inside
  `#if TARGET==PDP11 || TARGET==VAX`. Defining it would claim a VAX peephole
  pass this port does not have.
- **`SDB` is off** in `arm64defs`. f77's stab output is a.out's, and its README
  spends its whole first half apologising for it.
- **`AUTOREG`, `ARGREG`, `ARGOFFSET`, `SAVESPACE` are stated but unexercised.**
  Every consumer is in `vax.c`, `putpcc.c` and `proc.c`, which stage 3 replaces.
  Marked as unexercised rather than trusted.

## Open

- `PROFFOOT` points at `/lib/mcrt0.o`, which this port does not build, so `-p`
  fails loudly at the link. Deliberate: silently profiling nothing is worse.
- Stage 3 must use this same `arm64defs`, and the `SZLONG` note above is the
  first thing to reconcile — see task #12.

## Stage 3 groundwork: `putpcc.c` was missing an `#endif`

Measured while sizing `f77pass1`: **14 of its 15 objects compile under v8cc with
nothing but `arm64defs`**, which is a far better starting point than the ~13000
lines suggested. The one failure is `putpcc.c`, and it is an upstream defect that
**only a non-VAX PCC target can find**.

Counted over the whole file: **17 `#if` against 16 `#endif`**. The
`#if TARGET == VAX` at `:49` never closes. With `TARGET==VAX` — the only value
f77's makefile has ever been given — every line from there to EOF is *included*
and nothing is wrong. With any other target cpp skips looking for the matching
`#endif`, finds none, and swallows the rest of the file: the two braces closing
`if(!headerdone)` and `puthead()` vanish, and ccom reports

```
"putpcc.c":75:syntax error
"putpcc.c":75:saw EOF
```

fourteen lines later, at the next function. **The error names neither the
conditional nor the flag.** The PDP11 arm ten lines above shows the shape
intended: `#if`, the machine's two statements, `#endif`, all inside the
`if(!headerdone)` block.

### What `arm64.c` must supply, derived rather than estimated

The 14 objects need 45 external names; subtracting libc leaves what `vax.c`
provides:

| kind | names |
|---|---|
| data | `intcon[]`, `realcon[][]`, `regnum[]`, `maxregvar` |
| directives | `prlabel`, `prconi`, `prcona`, `prconr`, `praddr`, `preven`, `prlocvar`, `prext`, `prhead`, `prtail` |
| control | `goret`, `prarif`, `prcmgoto`, `prendproc`, `fixlwm` |
| the big one | `prolog` |

Most are three to eight lines. `prolog` is ~150. The stab half of `vax.c` — 120
lines across `prstab`, `prdbginfo`, `prstleng`, `stabtype`, `prcomssym` — reduces
to a stub because `arm64defs` leaves `SDB` undefined.

**And it cannot be verified without stage 4.** Whether the emitted intermediate
is *correct* is only answerable by `/lib/f1` consuming it, so the two land
together; `f77 hello.f` producing a working binary is the test, and nothing
smaller is.

### And `prolog` is a rewrite, not a translation — the last unknown, measured

`prolog()` is the largest thing `arm64.c` owes, and it is not portable in the way
the fifteen directive printers are. Three of its VAX assumptions have no arm64
counterpart:

- **`.word LWM<procno>` in the function's first word** (`vax.c:359`) — the VAX
  register-save mask that `calls`/`callg` read. arm64 has nothing like it.
- **`ap`-relative argument access**, via `mvarg()` emitting
  `movl n(ap), m(fp)`. AAPCS64 passes in `x0`–`x7`.
- **`prsave()`'s `subl2 $LF<n>,sp`**, a frame whose size is a forward-referenced
  assembler symbol.

And the first of those is a **two-way contract between the passes**, which is the
part that makes stages 3 and 4 one piece of work rather than two.
`fixlwm()` (`vax.c:474-482`) emits

```
	.set	LWM<procno>,0x<mask>
```

*after* the body has been generated, once `highregvar` is known — so pass 1 emits
a forward reference to a symbol whose value pass 2's register allocation decides,
and the assembler resolves it.

On arm64 there is no mask, and `arm64_endfunction()` in
`compiler/ccom-arm64/emit.c:437` already lays out the frame itself — in **three
regions**, an arrangement CLAUDE.md records as having cost a real bug when two of
them collided. So the prologue `prolog()` writes has to match an epilogue the
back end already emits, and that contract must be **designed rather than
ported**.

**That is why nothing smaller than `f77 hello.f` can test either stage**, and why
neither should be written speculatively: a prologue that looks right and does not
match the epilogue produces a binary that links and corrupts its own frame,
which is precisely the class this port has spent its life finding.
