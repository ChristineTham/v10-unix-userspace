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

## Stage 3 is in: `f77pass1` builds, installs, and compiles Fortran

`arm64.c` is written and all sixteen objects compile under v8cc. `f77pass1`
installs to `/usr/lib/f77pass1`, and `f77 hello.f` now reports
`Cannot load /lib/f1` — one step further than before, which is the case
`tests/wavea` asserts.

The grammar is **generated**, unlike efl's: upstream's rule seds the token
numbers into `%token` lines, cats the five fragments after them, and runs yacc.
V8's own yacc reports **four shift/reduce conflicts**, which is exactly what
upstream's makefile echoes as expected — a good independent signal that the
fragments assembled correctly.

### What `arm64.c` had to change, against the prediction

The three non-portable pieces were the ones flagged above, and the first is
**removed rather than translated**:

| | |
|---|---|
| `.word LWM<n>` + `fixlwm` | **gone.** arm64 has no register-save mask, and `arm64_endfunction()` already owns the frame. Removing a handshake is safer than reimplementing one — a mismatched prologue links and corrupts its own frame. |
| `mvarg`, `prsave`, `goret` | silent. AAPCS64 passes in `x0`–`x7`; pass 2 spills them and emits the epilogue. |
| `realcon[]` | rewritten — VAX D-format octal to IEEE 754. |
| `prcmgoto` | the VAX's one-instruction `casel` became a bounds check plus an indexed load from a `.quad` table. |

**Two smaller things would have been silent wrong answers**, which is why they are
called out rather than absorbed:

- **`.word` is two bytes on a VAX and FOUR to clang.** Porting `prconi`
  literally would have doubled every `INTEGER*2` initialiser with no diagnostic.
  It emits `.short`.
- **`praddr` needed `.quad`.** An address is eight bytes here and `SZADDR` says
  so; `.long` would have truncated every initialised pointer. This is the only
  directive in the file whose *width* differs rather than its spelling.

### The intermediate is BINARY, which re-costs stage 4 again

`putpcc.c`'s second line is *"NEW VERSION USING BINARY POLISH POSTFIX
INTERMEDIATE"*. So `mainp2()`'s text reader in `pcc1/mip/reader.c` — which the
earlier costing was written against — is the **older** format, and `/lib/f1` has
to read a stream of `long`-sized words instead:

- `p2triple(op, var1, var2)` and `p2word(w)` write raw words through a 128-word
  buffer flushed with `write(2)`.
- The word is `long int`, so **eight bytes here and four on a VAX**. Both ends
  are ours, so that is consistent — and it is the third time in this port that a
  `long` turned out to be an ABI decision rather than a type.
- `p2name()` has two forms on `UCBPASS2`; this build does not define it, so names
  are the fixed 8-character form.
- `pccdefs` holds the opcodes (`P2PLUS 6` … `P2LABEL 207`), and the ones above
  200 are f77's own additions.

Measured on a five-line program: 512 bytes of intermediate, and `h.s` carrying
`_s_wsfe`, `_do_fio` and `_e_wsfe` — the same libI77 entry points stage 1's probe
called by hand before any compiler existed.

### The intermediate format, decoded from a real file

`p2triple` is the whole header encoding:

```c
word = op | (var<<8) | ((long int) type)<<16;
```

so every record begins with one word carrying an opcode, a small count, and a
pcc1 type word, followed by however many operand words that opcode implies.
Decoded from `l.x`, the 64-word intermediate for

```fortran
      program p
      integer i
      i = 2
      write(6,10) i
   10 format(1x,i3)
      end
```

| word | value | meaning |
|---|---|---|
| 0 | `0x2c8` | `P2PASS`(200), var 2 — two words of literal assembly follow |
| 1–2 | `0x7865742e`, `0x74` | `".text"`, four chars per word (`p2str`) |
| 3 | `0x100cb` | `P2LBRACKET`(203), var 0 regvars, type 1 = procno |
| 4 | `0` | `BITSPERCHAR*autoleng` — no autos |
| 5 | `0x1c9` | `P2STMT`(201), var 1 — one word of filename |
| 6 | `0x662e6c` | `"l.f"` |
| 7 | `0xb00cf` | `P2LABEL`(207), type 11 = the label number |
| 8–9 | `0x40002`, `0x312e76` | `P2NAME`(2) type `P2INT`, then `"v.1"` |
| 11–12 | `0x40004`, `2` | `P2ICON`(4) type `P2INT`, value **2** |
| 13 | `0x4003a` | `P2ASSIGN`(58) — `i = 2` |
| 14 | `0x300c9` | `P2STMT`, type 3 = line number |
| 15–18 | `0x940104`, `0`, `"_s_wsfe"`, `0` | `P2ICON` with type 148 — `P2INT` under `P2PTR|P2FUNCT`, i.e. a function address — then the name in two words |
| 19–21 | `0x140104`, `0`, `"v.2"` | `P2ICON` type 20 = `P2INT\|P2PTR`, the `cilist` |
| 23 | `0x40046` | `P2CALL`(70) |
| 27 | `"_do_fio"` | and the transfer |

Three things that fall out of this and are worth having written down:

- **`p2name` fits `"_s_wsfe"` in ONE word plus a zero word**, because the
  non-`UCBPASS2` form declares `union { long int word[2]; char str[8]; }` and
  writes both words. `long` is eight bytes here, so `str[8]` fits entirely in
  `word[0]` — on a VAX it needed both. The record length is the same either way,
  which is why nothing notices.
- **The type word is pcc1's 4-bit-per-level encoding**: `148` is `P2INT` with
  `PTR` and `FUNCT` stacked above it. Our pcc2 uses five bits per level
  (`BTSHIFT` 4→5), so this is the field the translator has to re-shift.
- **`P2PASS` records carry literal assembly through**, which is how `prolog()`'s
  label and `prarif()`'s comparisons reach the output at all. So `/lib/f1`'s
  copy-through path is not an edge case — it is most of what the file contains
  for a small program.

With that decoded, stage 4 is mechanical rather than exploratory: read the words,
rebuild the trees, re-shift the type field, map the eight differing opcodes, and
supply the fourteen names this port's pass 2 wants from pass 1.

## Stage 4 compiles, and the pipeline produces an executable

`f77 hello.f -o hello` now runs all four programs — driver, `f77pass1`,
`/lib/f1`, clang — and produces a linked Mach-O executable.

`/lib/f1` is a **stack machine**, not pcc's second pass. The intermediate is
postfix, so the operands of every operator are already adjacent; rebuilding a
tree only to flatten it again would be work in both directions. Each statement
is a complete expression, which is what makes the operand stack tractable: `STMT`
marks the boundary, so for a call the callee is the first value pushed since the
mark and the arguments are what follow. That removes the only genuinely
ambiguous decoding question, because `LISTOP` carries no arity.

**The frame lives in `arm64.c`, not in `f1`**, and the intermediate settled that
rather than reasoning: the entry stub is written *after* the body, because
`putbracket()` rewrites the header in place. An epilogue emitted at `RBRACKET`
therefore lands after the stub's branch, unreachable, with the body running off
the end — measured, before `prsave()` and `goret()` took it over. That is exactly
where the VAX put `subl2` and `ret`.

### Six defects between "it links" and "it runs", and four are host properties

- **A CASE-INSENSITIVE FILESYSTEM.** `crfnames` picks nine temp suffixes and two
  pairs differ only in case: `s`/`S` and `a`/`A`. On any Unix that is nine files;
  on a default APFS volume `.s` and `.S` are **one**, so
  `sort <initfname >sortfname` truncated the assembly `f77pass1` had just written
  and filled it with sorted data records. The assembler reported either a file
  beginning `0v.1 00000 00020 3` or no file at all, and neither message names the
  cause. Renamed to `srt` and `set`, so no two suffixes differ only in case and
  the collision cannot return by someone picking another letter.
- **`clang` is a DRIVER, not `as`,** three times over: it needed `-c` or it went
  on to link (`Undefined symbols: _e_wsfe ... _main`); it read `.a` as an
  **archive** and said `unknown file type`; and it refuses `-o` with two inputs.
  **Upstream already had the answer to the third** — the VAX arm's comment is
  *"vax assembler currently accepts only one input file"* — so the cat-then-
  assemble arm was reused rather than reinvented, for a different reason.
- **`prcona` emitted `.long` for a label address.** `ld`: *"32-bit pointer in
  64-bit arch, r_length=2"*, which names the relocation and not the directive.
- **`io.c`'s `cilist` offsets describe a C struct and disagreed with it.**
  `XFMT` is `2*SZFLAG + SZIOINT` = 12, right when `SZADDR` equals `SZIOINT` as it
  did on a VAX; here `SZADDR` is 8 and v8cc pads the pointer to 16. `ld` refused
  the object — the good direction, since the alternative is a format pointer
  half in one field and half in another.
- **A data block is aligned by its FIRST element's type**, and a `cilist` holds a
  pointer at offset 16 with a `TYLONG` at 0.
- **`pruse` emitted no alignment after a section switch.** Mach-O keeps whatever
  alignment the assembler last had, so the first atom after `.data` came out
  1-aligned.

### What is still wrong, stated rather than implied

The generated program **links and then faults**. The crash report gives a full
symbolicated backtrace — `main → e_wsfe → en_fio → do_fio → w_ned → wrt_AP` —
faulting on a bad pointer inside libI77's format interpreter, so the defect is in
the Hollerith/character path rather than in the call sequence, which visibly
works as far as `do_fio`.

`tests/wavea` therefore asserts **the executable and not its output**. Claiming
it runs would be false; what is true today is that four programs cooperate to
produce a Mach-O binary, and that is what is checked.

## It compiles and runs Fortran

```
$ f77 loop.f -o loop && ./loop
 sum 1..5 = 15
```

Five programs are asserted in `tests/wavea`, each adding one construct the one
before it lacked: a Hollerith format, a character literal with an integer,
arithmetic on two variables, multiplication with three output items, and a `DO`
loop. Every one of them found a real defect while being written, and **every one
produced a plausible wrong answer rather than a crash**:

| program | defect |
|---|---|
| character literal | `fmt.c:129`, `op_gen(APOS,(int)s,0,0)` — a `char *` through an `int` |
| `i + j` | a `NAME` **addressed** instead of loaded, so it added the two addresses |
| `DO` loop | `CBRANCH` jumps when the condition is **false**; emitting it directly ran the body once |

The addition is the sharpest. `format(1x,i3)` worked throughout, because an
integer edit descriptor puts a *width* in `p1` and never a pointer — so the
failures sorted themselves into "text broken, numbers fine", which is exactly
the wrong diagnosis. And the loop is why its case asserts **15** rather than an
exit status: a test checking only that it ran would have passed against a loop
that executed once.

### `struct syl` carried pointers through `int`, and the sweep had missed it

`fmt.h`'s `{ int op,p1,p2,p3; }` holds a width for `I`, a count for `X` — and a
`char *` into the format string for `APOS` and `H`. Exact on a VAX where
`sizeof(int) == sizeof(char *)`.

The earlier libI77 sweep looked for `long` holding a Fortran INTEGER and could
not see this, because it is the *opposite* pun: an `int` holding a pointer.
Widened the **type**, not the uses — `struct(1)`'s `VERT` lesson — and safe
because `syl` has one end: the interpreter's own state. `op_gen`'s parameters
were undeclared and therefore implicit `int` too, so widening only the struct
would have moved the truncation one call earlier and left the symptom identical.

### What `/lib/f1` implements, and what it refuses

Nineteen operators: `NAME ICON OREG REG PASS STMT LABEL GOTO LBRACKET RBRACKET
LISTOP ASSIGN PLUSEQ STAREQ PLUS MINUS STAR SLASH FORCE`, the six relationals
and `CBRANCH`. **Anything else is refused BY NAME and exits 2**, which is how the
boundary was found at all — each unimplemented operator named itself in turn
(`OREG`, then `REG`, then `PLUSEQ`, then `GT`) instead of being silently skipped.
A code generator that ignores an operator emits a program that links and computes
the wrong thing.

## Stage 5: the rest of Fortran, and what the refusal could not see

The paragraph above is left standing because its central claim turned out to be
**half right, and the wrong half was the reassuring one**. Measured on four
programs written to exercise what stage 4 did not:

| probe | result |
|---|---|
| arrays | refused — `operator 64`, which is `P2LSHIFT` |
| an `INTEGER FUNCTION` | refused — "indirect call not implemented" |
| a subroutine `CALL` | **compiled, linked, SIGSEGV** |
| `REAL` arithmetic | **compiled, linked, hung** |

**A GUARD ON THE VOCABULARY IS NOT A GUARD ON THE GRAMMAR.** The refusal is a
`default:` arm in an operator switch, so it can only fire on an operator the pass
does not *recognise*. The two programs that produced wrong binaries used only
operators it did recognise, and mishandled them — which is precisely the failure
mode the refusal was designed to prevent, arriving through the door it does not
watch. Nothing about the design was wrong; the claim made for it was too broad.

**AND THE REFUSAL NAMED A FEATURE THE PROGRAM DOES NOT CONTAIN.** `f(n)` in an
expression was rejected as an *indirect call*. There is no indirect call in it:
`docall` took the callee to be the first value pushed since `P2STMT`, which is
true of a call statement and false of a call in an expression, because the result
temporary is pushed first. A wrong answer in a diagnostic is worse than no
diagnostic — it sends the next reader after a missing feature instead of a bug.
The fix is a second, LOGICAL stack: `LISTOP` merges the top two logical values,
so `CALL` takes the top one as its argument run and the one below as the callee,
needing no arity from the record and no assumption about statements.

### The frame was wrong in two independent ways, and each hid behind a sentence

**`mvarg()` was an empty function and this file said so twice.** Its header
carried two entries claiming the argument copy was "pass 2's business" because
"this port's own prologue spills x0-x7". No prologue spilled anything. The claim
was duplicated in the one file that could have refuted it, which is why neither
copy read as wrong.

**`AUTOREG` and `ARGREG` were both x29 with `ARGOFFSET 0`.** `proc.c:806-814`
gives autos positive offsets from `AUTOREG` on every target but the VAX and the
PDP-11; `putpcc.c` addresses a parameter as `ARGOFFSET + memno` from `ARGREG`.
With one register and no bias a parameter and a temporary land at the SAME
ADDRESS — measured, `OREG reg 29 offset 0 type int` and `OREG reg 29 offset 0
type int *` in one procedure, two objects at one address with nothing in the
record to tell a second pass which it was holding.

Upstream needs two registers because its `ap` points into the CALLER's frame:
`proc.c:316` allocates an `argvec` only when `nentry>1`, so an ordinary
procedure never copies its arguments at all. arm64 passes in registers, so they
must be spilled into this frame regardless — and once both regions are in one
frame, **a constant separates them and no second register is wanted**. The
machine difference removes a register requirement rather than adding one.
`ARGOFFSET` is now the size of the auto area, stated once, with `prolog()`
refusing a procedure whose `autoleng` exceeds it.

### `prcmgoto` — an interface invented rather than read

`vax.c` does not define it: `putcmgo` takes a `#if TARGET == VAX` branch to
`vaxgoto` and its `casel` instruction. So there was no neighbour to copy, and the
first version took `struct Labelblock *labs[]` and emitted the jump table itself.
`putcmgo` passes `int labarray` — the label of a table it has ALREADY written —
so the subscript dereferenced a label number and pass 1 died at address `0x13`,
which is 19, which was the label. **`pdp11.c` is the only other implementation in
the tree and has the signature written out.** The macOS crash report named
`prcmgoto ← putcmgo ← yyparse` in five frames and cost one command; reading the
caller's own argument list would have cost none.

### A jump table is a relocation, so the constant pool moved out of `__TEXT`

`USECONST` was `.section __TEXT,__const`, which is right for numbers and wrong
for `.quad L16`. arm64 Mach-O forbids a relocation in `__TEXT`, so the link
failed — and **f77 reported success over the top of it**, because `doload` ends
with a bare `await(waitpid)`. The observable was a program that compiled, said
nothing, and did not exist. This is the boundary CLAUDE.md records for `sh`'s
`:fix` and `cpp`'s `:yyfix` reaching f77's own constant pool; there it stops rung
5 because the optimisation *is* the build step, here it costs one section name.

### What is implemented now

`NEG MOD LSHIFT RSHIFT BITAND BITOR BITXOR BITNOT NOT ANDAND OROR INDIRECT CONV
COMOP` join the nineteen, and the arithmetic dispatches on type: `w` for an
INTEGER with a re-extension after, `x` for a pointer, `fadd`/`fdiv` on `s` or `d`
for a REAL or a DOUBLE PRECISION. `QUEST` and `COLON` remain unimplemented and
still refuse by name; nothing f77 emits has reached them.

**`.AND.` and `.OR.` do not short-circuit, and must not.** The stream is postfix,
so both operands are evaluated before the operator is read — there is nothing
left to skip. Fortran 77 explicitly does not require short-circuit evaluation of
`.AND.`/`.OR.`, so evaluating both is conforming, and it is what f77's own
intermediate commits to by emitting them as operators rather than as branches.

**Adding them exposed a defect that predates them.** A comparison was left in the
FLAGS for `CBRANCH` to read, which is right until a second comparison arrives
before the first is used — and `i .gt. 3 .and. j .lt. 4` is exactly that.
Nothing had ever produced two comparisons in one expression, so nothing could
show it.

### Half the type conversions are stated and half are implied

f77 emits an explicit `CONV type double` over an INTEGER and **nothing at all**
over a REAL: in pcc an operator's type is the type of its RESULT and its operands
may be narrower. A pass honouring only the stated conversions read a
single-precision bit pattern with `fadd d`, so `2.5 + 1.5` came out `2.5` and the
program printed 7.5 where it should print 12 — a plausible number, from an
expression in which every operator was one this pass knows.

### v8cc's floating-point convention is asymmetric, and both halves were measured

A `double` is **passed in an x register** and **returned in `d0`**. Measured on
v8cc's own output rather than assumed symmetric: `double twice(x)` reads its
parameter from where x0 was spilled and ends `fmov d0, d16`. CLAUDE.md records
each half in a different note; nothing had put them side by side. `FORCE` is
therefore the return path — `proc.c` gives every non-subroutine a `retslot` auto
and the exit label is followed by that slot and a `FORCE` — and it dispatches on
type, while an argument always goes to an x register.

## Stage 6: what a twenty-program corpus found

Stage 5 ended with f77 compiling arrays, functions, floats and logicals, and
with the claim that `/lib/f1` refuses by name anything it cannot do. A corpus
of twenty ordinary Fortran programs — character variables, FORMAT, DATA,
COMMON, EQUIVALENCE, SAVE, intrinsics, arithmetic IF, implied DO, internal
I/O — found **eight** more defects. Only one of them announced itself the way
stage 5's did.

Three of the twenty "failures" were bugs in the test programs, not in f77, and
the tell for two of them is worth keeping: a result of `2.101947696e-44` is the
integer 15 read through an implicitly-REAL declaration. **A denormal is almost
always an integer being read as a float.** The third was `name counter too
long, truncated to 6`, which is F77's own six-character identifier limit.

### The flags do not survive a call, and the survey that missed it was complete

`flushcc()` in f1 spills a pending comparison into a register, and its comment
said it is called "at the top of the comparison arms, which is the only thing
that writes flags". That was a true and complete account of the code that
existed when it was written. A `bl` writes NZCV too, and calls arrived later.

    if (i .lt. 3 .and. fn(i) .gt. 1)

emitted `cmp` for the first test, `bl _fn_` — which destroys the flags — and
only then, at the second comparison, ran `cset` on flags the callee had
overwritten. Measured with a callee that leaves LT set on the way out,
`.FALSE. .AND. .TRUE.` came out **TRUE**. Nothing refused and nothing crashed.

`flushcc()` now runs at the top of `docall()`. That is the right place and not
merely a working one: in postfix everything between the comparison and the call
is address pushes, which write no flags, and a nested call flushes before its
own `bl`, so the induction closes. The pool is callee-saved, so a value spilled
there survives the call it is being protected from.

**The value cases are not the guard.** They depend on what the callee happens
to leave; an ordinary callee gave the right answer either way. The guard is
structural — between a `cmp` and the `bl` that destroys it there must be a
`cset` — and that is true at every callee.

### A NAME's `var` field is a flag, and the reader went straight for the name

`putpcc.c:1232-1235` writes an extra word before the name when the offset is
nonzero. f1 read the name immediately, so it took the offset's four bytes as
the head of the string and every record after that was read at the wrong
alignment. The first thing the misalignment produced was **opcode 0**, which is
not an operator at all — so the diagnostic named a construct no program
contains.

The trigger is any **constant subscript past the first**, in a plain local
array: `a(1)` has offset 0 and sets no flag, which is why it worked throughout
and is now the control. A variable subscript computes its address with
PLUS/STAR instead and was never affected, which is why 2-D arrays and DO loops
were fine. `addrinto()` already added `con` to the page address — and already
handled a negative one — so nothing downstream needed changing.

### ABS is a conditional expression, and in a postfix stream that is `csel`

`abs`, `iabs`, `dabs`, `min` and `max` were all refused with `operator 22
(COLON)`. Fortran has no conditional operator; f77 builds one. `intr.c:672`
expands ABS as `0 <= t ? t : -t` and `putpcc.c:1382-1383` expands MIN/MAX the
same way, so the commonest intrinsic in the language is a QUEST/COLON tree.

The stream decides the implementation. Measured on `j = iabs(i)`, both arms are
already evaluated when COLON arrives — a postfix stream cannot express
short-circuiting and this one does not try to, because f77 assigns to a
temporary first (`intr.c:670-671`) precisely so both arms are safe to evaluate.
So COLON is physically nothing and QUEST is one `csel`. Two details: at most
one of the three operands can own the flags, so the arms are materialised
first; and `csel w` zeroes bits 63:32, so the result needs the same `sxtw`
every arithmetic site here already emits.

### Every floating value crossing a call is a double — K&R's rule, twice over

Three separate symptoms, one cause. `sqrt(x)` was refused outright; `x ** 0.5`
printed **3.01e+23** for the square root of two; and a Fortran REAL FUNCTION
returned through `s0` where its caller read `d0`.

K&R C has no float return. Measured: **not one** of the 66 typed functions in
`src/libF77`, `src/libI77` or `src/libc/math` returns `float` — all are
`double`. f77's own table disagrees with its own runtime, calling `r_sqrt`'s
result TYREAL while `src/libF77/r_sqrt.c` is
`double r_sqrt(float *x) { return sqrt(*x); }`. And `putpcc.c:551-552` forces a
TYREAL result as `P2DREAL`, so f77 states the rule for Fortran functions too.

On a VAX that disagreement cost nothing, for the reason `wrt_E`'s bug cost
nothing until it met IEEE: D_floating's leading 32 bits have F_floating's exact
layout, so reading a returned double's first word as a float is the same
number. Here `d0`'s low half is the low mantissa. **Second instance of that
coincidence, in a second component.**

The first draft special-cased "the callee is one of the thirteen in
`intr.c:381-396`'s `callbyvalue[]`". Reading `putforce` made the rule uniform
and the special case went away — which is the better outcome, because a table
of names is a thing that goes stale. What remains is that a float argument
passed **by value** promotes to double, which is the same K&R rule at the other
end of the call.

The precision is what discriminates a fix from a coincidence: `exp(1.0)` gives
`2.71828175` and `dexp(1.0d0)` gives `2.71828183`. Both are right for their
width, and a pass reading `s0` got neither.

### The register pool was a worst case; pass 1 states the real number

Character concatenation was refused for want of a sixth register. The pool was
five, fixed. `src/cmd/f77/arm64.c`'s `regnum[]` offers x19–x22 as register
variables and `arm64defs` caps that at `MAXREGVAR 4` — but the LBRACKET record
opens every procedure with the number pass 1 **actually** took, and measured
over the whole corpus that number is 0. So four callee-saved registers sat idle
in every procedure this pass had ever compiled, and x28 was saved by the
prologue and allocated by nobody.

Measured, in registers live at once: one-way concatenation 1, two-way 6,
three-way 8, four-way 10, five-way still refuses. The old pool of five could
not do a **two-way** — `a // b`, the simplest concatenation there is.

The remaining pressure is not the pool. `doassign()` returns the stored value
and pushes it, and `s_cat`'s argument vector is built by a run of assignments
whose values nothing consumes, so each holds a register until the statement
ends. Returning the *lvalue* instead would free them and is the obvious next
step — recorded rather than done, because it changes the most load-bearing
function in the pass and the boundary above is honest and measured.

### The internal-I/O control block, and an excuse that expired

`io.c` already carried `IOALIGN` and a comment that corrected only the external
READ/WRITE offsets, deferring the rest because "an unexercised correction is a
claim nothing can check". That reason expired the moment something read from a
CHARACTER buffer: `ld` refused the object with `pointer not aligned in
v.2+0x4`. `icilist` puts its pointers at 8 and 24 where upstream's arithmetic
put them at 4 and 16.

**The rewrite is upstream's own arithmetic**, and that is checkable rather than
asserted: set `SZADDR` equal to `SZIOINT`, as a VAX had them, and `IOALIGN`
becomes the identity and all six offsets collapse to exactly the expressions
upstream wrote — 4, 8, 12, 16, 20, 24. The OPEN, CLOSE and INQUIRE lists still
have the defect and keep the marker, with the same expiry condition stated.

The guard is a **relation over the init records** — every pointer in a control
block sits on an eight-byte boundary — so those three lists are covered the day
they are corrected and nobody edits the case.

### ASSIGN crashed the front end, and it took two ports of one fact

`assign 20 to lbl` — with no GOTO at all — SIGSEGV'd inside `f77pass1` at
address **1**. `putop`'s OPCONV loop descends a conversion chain and refreshes
`lp` and `lt` at the bottom, *before* the `while` re-tests `p`'s tag, so at a
leaf it reads `leftp` — offset 24 — out of a `Constblock`, whose union ends
there. On a VAX that offset was past the end of a 24-byte block and the byte
came from the next heap object: garbage into `lt`, which is dead once the loop
condition fails. Here the block is 32 bytes, offset 24 is `cd[1]`, `ALLOC`
leaves it zero, and `lp->headblock.vtype` reads address 1.

**It is reachable only because SZADDR stopped equalling SZLONG.** `expr.c:477`
skips the conversion when `typesize[ltype] >= typesize[rtype]`; assigning a
TYADDR constant to an INTEGER was `4 >= 4` on a VAX and is `4 >= 8` here, so
the OPCONV that trips the loop was never built before. Breaking out when `p` is
no longer a `TEXPR` is exactly what the VAX did next.

The branch itself cannot work here and is refused with its reason. An assigned
GOTO needs a code address in a Fortran INTEGER; `exec.c:554` requires that
variable to be an integer, INTEGER is four bytes, and Mach-O loads text above
4GB — measured, `str w23, [x13]` drops the 1 in bit 32 of `0x10001f140`. Before
the GOTO record's `var` flag was read, f1 emitted `b L4` for every assigned
GOTO, because `p2op(P2GOTO, P2INT)` puts P2INT — which is 4 — in the type
field. The assembler then reported an undefined label, naming something the
program never had.

## Stage 6b: a second corpus of ten, and four more

The twenty programs above were not exhaustive — they were where the sweep
stopped. Ten more (`.EQV.`, CHARACTER functions, EXTERNAL, FORMAT repeat
counts, `LGE`/`LLT`, multiple RETURN, a REAL DO index, concatenation in an
argument, OPEN/CLOSE, BLOCK DATA) found four further defects. Three are LP64
width arriving in a **layout** rather than in a value, and the fourth is a
feature Fortran has had since 1966.

### An argument occupies a slot, not its own width

`proc.c`'s `nextarg()` allocates argument offsets by summing `typesize[type]`.
That is right on a machine where every argument type is the same size, and on a
VAX they all were — `SZADDR`, `SZLONG` and `TYLENG`'s width are all 4 — so
summing sizes and counting slots gave the same answer. Upstream says so itself:
`lastargslot/SZADDR` at `proc.c:314-320` treats the running total as a slot
count.

Here a pointer is 8 and a hidden character length is 4, so the two part company
the first time a **length precedes a pointer**. A CHARACTER FUNCTION is exactly
that shape — result pointer, result length, then the argument pointer — so f77
addressed the third argument at `ARGOFFSET+12` while `prsave()` spills it at
`+16`.

**The assembler caught it, which is the good direction.** `ldr x1, [x29, #1036]`
has an offset that is not a multiple of 8, so the scaled form cannot encode it,
the unscaled form is limited to ±256, and clang refused the file. A packed
offset that happened to land 8-aligned would have read the wrong argument in
silence. `character*(*) t` as the *only* argument worked throughout, because
with one pointer before one length the two layouts agree — which is why it is
the control.

### A data block's alignment came from its first record's type

`dodata()` picks a block's alignment from the first record it reads, and on a
VAX that could not matter: `ALIADDR`, `ALILONG` and `ALIINT` were all 4, so the
strictest thing a block could contain was already what its first member asked
for. Here a block whose first record is a `.long` gets 4.

Measured on a DATA array followed by two FORMATs: `v.1` is 12 bytes, so `v.2`
began at 12 and `v.3` at 36 — putting `v.3`'s format pointer, **correctly** at
offset 16 within its own block, at address 52. `ld: pointer not aligned in
v.1+0x34`, naming the nearest preceding symbol rather than the block at fault.

Raising the floor to `ALIADDR` rather than looking ahead: `dodata` sees records
one at a time and decides on the first, so the alternative is a second pass over
the sorted file. This costs a few bytes of padding, and `vargroup` 1 and 2
already take `ALIDOUBLE`, which is the same 8 here.

### A procedure passed as an argument is `blr`

`external sq / call apply(sq, x)` spills sq's address into apply's parameter
area, so at the call site the callee arrives as an **OREG** — a value — rather
than as the ICON-with-a-name every direct call gives. f1 refused it as "a call
through a value rather than a name", which is an accurate description of
EXTERNAL.

Materialised before the arguments are placed, and into the pool, which is
callee-saved: x0–x7 are about to be written and a callee address in one of them
would be overwritten by the argument that belongs there. Done through the stack
slot rather than a local copy, so the cleanup loop frees the register once.

### The OPEN deferral named the wrong instrument

`io.c`'s note said the first program to OPEN a file "will refuse to link". It
does not. A READ/WRITE control block is **initialised data**, so its pointers
are relocations and `ld` checks their alignment — but an OPEN block is filled in
by `ioset()` at **run time**, so there is no relocation and nothing to check.
Measured: the program linked cleanly and SIGSEGV'd, because f77 stored `osta` at
20 where the C struct reads it at 24.

**An expiry condition that names the wrong instrument is not a tripwire.** The
deferral was written in good faith, with a stated trigger, and the trigger could
never fire.

`MAXIO` moved with the offsets and is the sharper half: it is the size of the
biggest control block, stated at the top as `SZFLAG + 10*SZIOINT + 15*SZADDR`.
That is exactly `inlist`'s end on a VAX — 104 — and **164 here against a struct
that now reaches 196**. `io.c` allocates the block as an auto of that many
words, so a short MAXIO is an INQUIRE writing past its own frame slot. It is
restated where the last offset is known rather than recomputed from a member
count.

Every offset is checkable two ways: against the struct in `src/libI77/fio.h`,
and by setting `SZADDR` equal to `SZIOINT` — as a VAX had them — whereupon
`IOALIGN` becomes the identity and each collapses to the expression upstream
wrote. And the guard is a **relation over the intermediate**: every pointer f77
stores into an I/O control block sits on an eight-byte boundary, derived rather
than transcribed, so OPEN, CLOSE and INQUIRE are covered by one case.

## Where the corpus stopped finding things

Batches five and six — twenty more programs covering statement functions,
substring assignment, `INTEGER*2` arrays, nested implied DO, `STOP`, `REWIND`,
`BACKSPACE`, `ENDFILE`, COMPLEX, direct-access I/O, `T` editing, recursion over
an array, DATA repeat counts, `IMPLICIT`, character arrays, and calls three deep
— found **zero** defects. Batch six was aimed deliberately at the seams the
earlier fixes touched: eight arguments, a mixed character/integer/character
argument list, a character function returning a concatenation, and an indirect
call used twice in one expression.

Three of the sixty programs "failed" for reasons that were mine rather than
f77's, and each is worth knowing because it imitates a compiler defect:

- a result of `2.101947696e-44` is the integer 15 read through an implicitly
  REAL declaration — **a denormal is almost always an integer read as a float**;
- a result of exactly `0.000000000e+00` from an integer function is the same
  mistake after the FORCE fix, because the integer comes back in `x0` and the
  caller reads `d0`, which is untouched;
- `unbalanced quotes` on a line that looks balanced is **column 72**, which
  fixed-form Fortran truncates at. That line was 76 characters.

### What is left, and it is asserted rather than written down

Four boundaries remain, each refusing by name with an accurate reason, and each
now has a case — because a refusal nothing tests is an unexercised rule whose
failure mode is that it quietly stops refusing.

| limit | why |
|---|---|
| a ninth argument | AAPCS64 puts arguments 9+ on the stack, and the callee half needs f77 to tell `prsave()` the count |
| a second ENTRY point | `argvec` is allocated only when `nentry > 1`, and nothing here consumes it |
| a five-way concatenation | register pressure, and the fix is not a bigger pool — see `doassign()` above |
| an assigned GOTO | a code address does not fit in a four-byte Fortran INTEGER, and Mach-O loads text above 4GB |

The mutations that validate them are the ones that make each **stop** refusing:
raise the argument cap, raise the ENTRY cap, and make `ralloc()` reuse a
register instead of exiting. Each fires on exactly the case written for it.
