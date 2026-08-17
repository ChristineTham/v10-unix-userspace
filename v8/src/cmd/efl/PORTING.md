# Porting efl(1)

Stuart Feldman's **Extended Fortran Language** (Bell Labs CSTR #78, 1977): a
language with structured control flow, C-like data structures and generic
procedures, which compiles to portable Fortran 66. It is the second of the
five language systems, taken after `ratfor` because the owner's criterion was
size and because `efl` is the interesting one — it emits Ratfor-adjacent
Fortran and is the largest thing in this port after `cfront`.

| | lines |
|---|---|
| `gram.c` (checked-in parser) | 1584 |
| `mk.c` | 990 |
| `io.c` | 708 |
| `defs` | 679 |
| `simple.c` | 692 |
| `main.c` | 664 |
| ...20 more `.c` | |
| four grammar fragments | 933 |
| **total** | **11931** |

## The headline: zero source changes, one compiler fix

**All 34 imported files are byte-identical to pristine V8**, verified against
`PROVENANCE` with `git hash-object`. Nothing in `src/cmd/efl/` deviates.

That is not because efl is easy. It is because efl's LP64 exposure is almost
entirely absent by construction and its one real hazard turned out to be in
**our compiler**, not in Bell Labs' source.

### Why the dominant bug class does not apply here

`defs:36` is

```c
typedef int *ptr;
```

— a generic pointer type that is *already pointer-sized*. Every node-returning
function in the tree is declared `ptr` (there are ~40 such declarations across
`alloc.c`, `blklab.c`, `exec.c`, `gram.c`, `misc.c`, `mk.c`, `simple.c`,
`io.c`, `main.c`, `pass2.c`, `dcl.c`), and `defs:465` declares the allocators
themselves:

```c
ptr intalloc(), calloc(), allexcblock(), allexpblock();
```

So the `struct(1)` shape — `#define VERT int`, a pointer stored in an `int` —
is not present, and neither is the `malloc` undeclared shape: efl declares
everything. `tests/v8ccom`'s rootfs-wide truncation sweep found exactly **one**
narrowed call across the whole 12k-line program, `conval` (`misc.c:105`), and
it genuinely returns an int — it is the function whose entire job is to extract
the integer value of a constant node. It is in `INTFNS` with that reason.

**The measurement worth keeping**: a program can be enormous and still have
almost no LP64 surface, and what decides that is one `typedef`. Compare
`struct(1)`, where `#define VERT int` was the whole fix and cascaded into two
headers.

### The two size invariants upstream states, measured

`defs` carries two comments that are assertions about layout:

```c
extern struct exprblock	/* must be same size as varblock */
extern struct ctlblock	/* must be same size as execblock */
```

Both hold under LP64, and not by luck:

| | VAX | LP64 |
|---|---|---|
| `varblock` / `exprblock` | 64 / 64 | **96 / 96** |
| `execblock` / `ctlblock` | 32 / 32 | **48 / 48** |

The pairs are member-for-member parallel — the same sequence of `ptr`, `int`
and bit-field group in each — so any pointer-width change moves both by the
same amount. This is the *opposite* of `sys/fblk.h`, which measured 716 by
coincidence of two types happening to agree; here the equality is structural
and would survive a further width change.

Measured with a throwaway program compiled by v8cc against `defs`, not
computed by hand.

## The compiler bug efl found

`misc.c:401`, and the line that crashed is `:417`:

```c
setvproc(p, v)
register ptr p;
register int v;
{
...
	p->vproc = q->vproc = v;
```

A chained assignment to two bit fields. It crashed at address **0x80**, and
that address is the whole diagnosis: `vproc` is a 2-bit field at bit 6, and
`PROCINTRINSIC` is 2, so `2 << 6 == 128 == 0x80`. ccom was storing the
**spliced field value as an address**.

```asm
ldr  w12, [x10]          ; first insert -- value in a separate register
bfi  x12, x11, #6, #2
str  w12, [x10]

ldr  w10, [x10]          ; second -- value loaded INTO the address register
bfi  x10, x11, #6, #2    ; x10 is now 0x80, the address is gone
str  w10, [x10]          ; KERN_INVALID_ADDRESS at 0x80
```

Root cause is in `compiler/ccom-arm64/gencode.c`. `lvaddr()` describes a bit
field's containing word in a slot of a static pool, and released the slot **on
return** rather than in `lvfree()`:

```c
lv->cont = &contbuf[contdepth++];
lvaddr(p->in.left, lv->cont);
contdepth--;			/* <- one scope too early */
```

An lvalue stays live across the evaluation of the right-hand side, and here the
right-hand side is *another bit-field assignment*: `p->f = q->f = v` is
`ASSIGN(FLD(p), ASSIGN(FLD(q), v))`. Both assignments got `contbuf[0]`. Two
independent symptoms followed from one cause:

- **wrong object** — the outer store went to `q`'s word, and the address of
  `p`'s word (`x9`) was computed and never used;
- **clobbered address** — the inner `lvfree()` had already returned that
  register to the pool, so `regalloc()` handed it straight back to `lvload()`.

The fix moves the release into `lvfree()`, where every other resource in the
struct is already released. All three `lvaddr()` call sites were checked to
reach exactly one `lvfree()` on every branch first.

**The author's own error message names the misconception**: `"bit fields nested
too deeply"`. C has no bit field inside a bit field, so the recursion is always
exactly one deep — the quantity that actually needed bounding is *bit-field
lvalues live at once*, and `a.f = b.f = v` has two. The constant is `NCONTLV`
now and the message says so.

Guarded by six cases in `tests/v8ccom`, mutation-verified: reverting the fix
fires exactly the four aimed cases and leaves the negative control green.

### Why 172 programs never reached it

The shape needs a chained assignment to two **different** bit fields, and the
register clash on top of that needs enough pressure that `regalloc()` has the
freed register to hand back. `p->vproc = q->vproc = v` with three `register`
declarations is that shape. It is the same story as `STARG` — no program in
Waves A, B or C passed a struct by value until `grap` did.

### The instrument, which contradicts a recorded rule in the useful direction

CLAUDE.md says not to reach for lldb because v8cc emits no unwind info, and
lldb indeed hung here. But **macOS's own crash report gives a full symbolicated
backtrace of a V8 binary**: `~/Library/Logs/DiagnosticReports/efl-*.ips` named
`setvproc ← extname ← yyparse ← dofile ← main ← v8start`, six frames, plus the
faulting address. It walks the x29 frame-pointer chain, which v8cc *does*
maintain, rather than needing DWARF. That is a better first instrument than the
bisect-by-`fprintf` the `struct(1)` port needed, and it costs one command.

## The build: three generated inputs, three different V8 programs

Nothing else in this port has this shape.

| input | made by | note |
|---|---|---|
| `tokdefs` | `grep -n . tokens \| sed ...` | token numbers ARE line numbers |
| `lex.c` | `lex lex.l`, then `fixuplex` | which is an **ed(1) script** |
| `gram.c` | **nothing** | checked in; upstream's yacc rule is commented out |

### `fixuplex` is an `ed` script, and `ed` is a compiler pass

```sh
ed - lex.yy.c <<!
/input/s/getc(yyin)/efgetc/
/yylex/+1a
...27 lines...
.
w
q
!
```

Two edits. The substitution routes character input through efl's own `efgetc`
(`defs:55`, `#define efgetc (efmacp?*efmacp++:getc(yyin))`) so that `include`
and `define` can push text back. The append inserts three global-flag checks
ahead of the DFA — something the lex skeleton has no way to express.

V8's `lex` emits exactly what the script expects (`getc(yyin)` on the `input`
line, `yylex(){` for the append), and V8's `ed` applies both: 1455 → 1482 lines.

**A failed `fixuplex` is a silent no-op that reports success**, and that is the
opposite of what it looks like. This was first written down here as *"not
idempotent — V7 ed prints `?` and keeps reading commands, so a second run
appends the block twice"*, which is wrong; the correction is the interesting
part.

Measured, with a three-line script whose first command cannot match:

```
?
ed exit: 0
--- file after a script whose FIRST command failed ---
alpha
beta
gamma
```

V7 `ed` reading from a here-document **aborts the whole script on the first
error** — the append never ran, `w` never ran, the file is untouched — and it
**exits 0**. So running `fixuplex` a second and third time on an
already-patched file changes nothing (measured: 1482 lines and one `pushlex`
block each time), and the real hazard is the other one: a `fixuplex` whose
substitution stopped matching hands the build an **unpatched scanner** with
nothing in the exit status to say so.

Two consequences. The `&&` chain in the Makefile recipe is *not* the guard here
— it catches a failed `lex`, not a failed `ed`. And `tests/wavea`'s two content
cases are the only instrument there is: the `input()` macro must reach
`efgetc`, and `getc(yyin)` must survive **nowhere**. Note the aim of the first
one: `efgetc` occurs **twice** in the generated file, once from fixuplex and
once from lex.l's own action code, so a global count would conflate the edit
with source that was always there.

The general rule this cost: *verify a recorded diagnosis before building on it,
including your own from an hour ago.* The false version had been written into
three files — the Makefile, this one, and CLAUDE.md — from general knowledge of
`ed` rather than from a measurement that takes one command.

### The tools are V8's, and PATH is set to exactly one directory

`$(BINDIR)/{grep,sed,ed}` beside `$(LEX)`, for the reason `maketab` is built by
V8's cc and run: upstream's makefile invokes them, so using the host's would
leave nothing to say the step was V8's. `fixuplex` reaches `ed` through PATH,
so PATH is `$(BINDIR)` **alone** rather than prepended — the Mac's `ed` would
run this script perfectly well and silently.

The *shell* running `fixuplex` is deliberately the host's, spelled `/bin/sh`:
V8's `sh` is not a `$(BINDIR)` binary (it has its own `:fix` pass), and every
other recipe in the Makefile runs under GNU make's `/bin/sh` anyway. Rung 5 is
where V8's sh runs recipes; the tool upstream's makefile *names* at this step
is `ed`.

### `gram.c` is checked in, and that leaves an invariant with no guard

Upstream's rule for it is commented out, with the reason stated:

```make
# gram.c can no longer be made on a pdp11 because of yacc limits
```

So `gram.c:1-83` spells out 83 token numbers and the build derives the same 83
from `tokens` — **two hand-maintained copies of one list, with no step left
that would notice them disagreeing**. Token numbers are line numbers in
`tokens`, so inserting one line renumbers everything below it and the lexer
starts returning numbers the parser reads as different tokens. Both halves
still compile.

This is the shape that let `v8fsd`'s and `p9cl`'s errno tables agree perfectly
about a set that was missing seven names. `tests/wavea` compares them as
**sets in both directions**, because two lists of 83 can differ and still both
be 83. The third source that is neither table is the tree itself.

`tests/deps` states the rest of it from the other end: `gram.head` and
`gram.exec` are asserted to be inputs to **nothing** (`nodep`). A `dep` there
would be a lie about how the parser is made.

## `defs` and `tokdefs` are the 15th and 16th included non-headers

Re-derived tree-wide (excluding the four `PORTING.md` prose hits, since the
documented sweep matches its own documentation):

```
cb/cbtype.c  ccom/common  ccom/y.debug  cpp/yylex.c  efl/defs  efl/tokdefs
eqn/e.def  lex/ldefs.c  lex/once.c  make/defs  refer/refer..c  refer/what..c
tbl/t..c  yacc/dextern  yacc/files  yacc/y.debug
```

`efl/defs` is the ordinary kind: 24 of the 25 objects include it, and it is
invisible to a header scanner and to a `*.c` glob alike. Note it is the
**second** file in the tree called `defs` — `make/defs` is the first — so a
sweep keyed on the basename would conflate them.

**`efl/tokdefs` is a new species: it is GENERATED.** Not a `.h`, not a `.c`,
and not in the source tree at all. `tests/deps` asserts `tokens → tokdefs →
{lex.o, init.o}` and, as the negative control, that `tokdefs` is *not* an input
to `mk.o` — which is upstream's own dependency line, `lex.o init.o : tokdefs`.

## The K&R global-member idiom, which is efl's whole style

The build emits **1620** `struct/union or struct/union pointer required`
warnings. They are all one thing: efl passes `ptr` (i.e. `int *`) everywhere
and writes `p->tag`, `p->sthead`, `p->vproc` on it, relying on pre-ANSI C's
rule that structure member names are global and denote fixed offsets.

This is safe here for a reason worth stating, because it would not be safe in
general: **v8cc computes the offsets from the LP64 layout of whichever struct
declares the member, and efl's blocks are designed to agree.** `struct
headbits` is the common prefix of every block type; `tag`, `subtype` and
`blklevel` live in it, so `p->tag` means the same offset whatever `p` points
at. The remaining shared names (`sthead`, `leftp`, `vproc`, ...) are used only
on blocks that declare them, and the two size invariants above are upstream's
own statement that the parallel pairs must stay interchangeable.

Deliberately **not** changed: S1 forbids it, and there is nothing to fix. The
warnings are upstream's own on upstream's hardware — a VAX ccom printed them
too.

Other warning classes, all measured and all benign:

| count | warning | verdict |
|---|---|---|
| 1620 | `struct/union ... pointer required` | the K&R idiom, above |
| 131 | `illegal pointer combination, op =` | `ptr` ← `struct foo *`; both 8 bytes |
| 8 | `illegal member use: tag/subtype/blklevel` | same idiom, named member |
| 1 | `illegal pointer/integer combination, op =` | `exec.c:176`, `char *` ← `ptr`; both 8 bytes |
| 2 | `illegal pointer combination, op >` / `<=` | `alloc.c:135`, `free()` bounds-checking its own arena |

## The arena, and what was NOT changed

efl replaces the C library's allocator with its own (`alloc.c`): `alloc`,
`calloc`, `malloc`, `free` and `cfree` are all defined there, over a static
`int mem[MEMSIZE]` of 12240 ints = **48960 bytes**. That is upstream's design —
it is how `prmem()` and the allocation histogram work — and the linker
therefore never pulls libc's `malloc`, so stdio's buffers come out of efl's
arena exactly as they did on a VAX.

Two LP64 consequences were measured and neither is changed:

- **Capacity.** Blocks grew 64→96 and 32→48 bytes, so the arena holds about
  two-thirds as many. Exhaustion is a *clean* failure — `alloc()` prints the
  highwater mark and calls `fatal1("out of memory")` — not a crash, so raising
  `MEMSIZE` would be a change not forced by the target. If a real program hits
  it, that is the moment to change it, with the program as the evidence.
- **Alignment.** `alloc()` counts in `int` words and returns `mem + i + 1`, so
  half of all allocations are 4 mod 8 and a `ptr` member inside such a block is
  a misaligned 8-byte access. ARM64 under Darwin permits unaligned normal
  loads and stores (SCTLR_EL1.A is clear for EL0), and v8cc emits plain
  `ldr`/`str` rather than pair instructions, so it works. Recorded rather than
  "fixed": making the arena `long`-granular is a real change to an authentic
  file, and nothing observable requires it.

## What was audited and deliberately not changed

- **`exec.c:176`**, the single pointer/integer warning: `s = t = calloc(...)`,
  `char *` from a `ptr`. Both are 8 bytes; `convic` beside it is properly
  declared at `defs:463`.
- **`conval` has no return on its `fatal()` path.** `fatal()` exits, so the
  fall-through is unreachable; upstream's, and not forced by the target.
- **The `#include "stdio.h"` in `defs`** is a quoted include of a system
  header, which resolves through the rootfs. Left alone.
- **`Test`** is a four-line shell script referencing `/usr/sif/efl/test/*.e`,
  which the distribution does not ship. Imported for completeness, an input to
  nothing.

## Install destination

`cp a.out /usr/bin/efl` — the `a.out` idiom, one of the nine makefiles that
spell the install source as `a.out` rather than as the program name. That is
the case `tests/wavea`'s makefile-versus-`Admin/dest` parser was taught in the
`ratfor` step, so efl needed no further work there and is covered by it.

## Still open

- **`MEMSIZE` under a large input.** Nothing here has compiled a program big
  enough to exhaust the arena. The failure would be loud, so this is a
  known unknown rather than a risk.
- **Rung 5.** efl's own makefile has not been run under `V8JAIL=strict`. It
  should mostly work — the `a.out` target is `eqn`'s shape, already handled —
  but `lex.c: fixuplex` invokes `lex` and `fixuplex` by bare name, so it needs
  `/usr/bin/lex` and a PATH that finds `ed`. Not attempted; not claimed.
