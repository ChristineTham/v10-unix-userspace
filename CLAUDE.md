# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Research Unix V8 (Bell Labs, 1985) userspace, rebuilt from source on macOS/ARM64 —
not emulated. The authentic Bell Labs C compiler compiles the authentic V8 C
library and the authentic V8 programs, on hardware that did not exist in 1985.

`PLAN.md` is the specification: fidelity contract, target model, phase
breakdown, and the running record of what has been learned. Read §1 (fidelity
contract) and §4a (bootstrap ladder) before making architectural decisions.

## Commands

```bash
make -j8              # full build (~4s clean)
make test             # all 12 suites (~358 tests)
make test-wavec       # one suite: deps jail cpp v8ccom v8cc v8sys freestanding
                      #            libv8c wavea waveb sh wavec
./tests/deps/run.sh   # a suite directly (same thing, no build first)
make clean            # remove build/ and rootfs/
```

Building a single object or program requires an **absolute** path, because
`$(BUILD)` is absolute:

```bash
make $(pwd)/build/stage0/bin/cat
```

Test suites are shell scripts that print `name: N passed, M failed`. There is no
per-case filter; edit the script or run its commands by hand.

### Running V8 binaries

Nothing runs without `V8ROOT` — the shim resolves V8 paths inside it, and when
unset it silently falls back to the host filesystem.

```bash
export V8ROOT=$PWD/rootfs
$V8ROOT/bin/cc -o prog prog.c        # V8's compiler
V8JAIL=strict $V8ROOT/bin/make       # build with V8's make, refusing host escapes
V8DBG=1 $V8ROOT/bin/cc -c x.c        # type tracing from the ARM64 backend
```

`V8JAIL=warn` names each host binary reached; `strict` refuses it. Use `strict`
when the claim is "this ran entirely on V8 code".

## Architecture: three layers, three different rules

The single most important thing to get right is **which layer you are editing**,
because they have opposite policies.

**1. Authentic V8 source (`src/`)** — imported from `third_party/`, then
minimally patched. Changes must be forced by the target (LP64, Mach-O, ARM64
ABI), not by taste. Do not modernise K&R declarations, do not add prototypes, do
not "fix" warnings. Every change is recorded in that program's `PORTING.md` with
the reasoning.

**2. New code (`compiler/`, `shim/`)** — written for this port, modern C.
`compiler/ccom-arm64/` is the machine-dependent half of the compiler, written
*inside ccom's own architecture* (`local.c`, `local2.c`, `gencode.c`, `macdefs.h`
— the same file names and hooks pcc expects). `shim/v8sys/` is `libv8sys`,
standing in for the VAX kernel.

**3. `third_party/`** — read-only, never edited in place. To bring a file into
`src/`: `tools/import.sh v8/usr/src/cmd/cpp`, which records the upstream blob
hash in a `PROVENANCE` file so the diff against pristine V8 stays reconstructible.

### The deliberate exception list

`as`, `ld`, `ar`, `strip`, `nm` are the **host's**, because the object format is
Mach-O; porting V8's a.out assembler and link editor is out of scope. The `cc`
driver execs `clang` for assembly and linking. This is a decision, not a gap —
do not "fix" it. Everything else is ported rather than passed through; host
passthrough is meant to be the exception, not the rule.

### The bootstrap ladder

Order matters and is not obvious. Each rung is built by the one above:

```
0 seed   host clang + host make + host yacc -> cpp, ccom-arm64, cc, libv8sys, crt0
1 tools  v8cc -> libv8c -> yacc -> lex -> make      (make needs yacc: it has gram.y)
2 jail   v8cc -> /bin: sh and the filters
3 close  regenerate cpp's grammar with V8 yacc; fixpoint v8cc1 == v8cc2
4 hand   V8 make rebuilds the compiler, inside the jail
5 world  V8 make + each program's own authentic makefile
```

Rungs 3–5 are unfinished. `rootfs/bin/cc` is still a **clang-built host binary**
(`nm` shows 0 V8 symbols), so it does not go through the shim and the jail cannot
observe it.

### The jail

`rootpath()` in `shim/v8sys/syscall.c` is a chroot implemented in the shim, not
the kernel: `chroot(2)` needs root, and every V8 binary is a Mach-O linked
against `libSystem`, so a real chroot would need the SIP-protected dyld cache
inside it. Consequence, and it is load-bearing: the jail is **per-binary, not
per-process-tree**. Host binaries never call `rootpath()`, so they see the real
macOS with no special casing; anything `cc` produces links `libv8sys`, so it is
jailed by construction.

## The bug classes that actually bite

**LP64 is the dominant one.** V8 assumes `sizeof(int) == sizeof(char *)`. The
tree calls `malloc` without declaring it and casts the `int` result to a pointer,
and undeclared K&R parameters are `int` but routinely hold pointers — common
enough that the compiler widens them deliberately, at `acctype()` in
`compiler/ccom-arm64/gencode.c`, which carries a note on the one case where that
widening is wrong. Symptoms are wild pointers and heap corruption far from the
cause. When something is mysteriously broken, check widths first.

**A missing libc function does not fail the link — it resolves from `-lSystem`.**
For a non-variadic function that silently works and hides the gap. For a
*variadic* one it is an ABI mismatch: v8cc passes every argument positionally in
x0–x7, Apple's ARM64 ABI passes variadic arguments on the stack. This bit three
times (`scanf`, `printf` via the driver, `execl` — the last made `system()` start
an interactive shell that looked exactly like a hang). `tests/libv8c` now guards
the shape.

**A stale object does not look like a build problem — it looks like wrong code.**
Four debugging rounds went to correct source compiled from already-fixed files.
`tests/deps` exists for this; see below.

**Files `#include`d that are not headers** are invisible to every dependency
scanner *and* to a `*.c` glob: `lex/ldefs.c`, `lex/once.c`, `tbl/t..c`,
`refer/refer..c`, `make/defs`, `yacc/dextern`, `yacc/files`. All are declared
explicitly in the Makefile.

## Build-system discipline

The Makefile is written defensively for reasons recorded in its comments. Before
changing it:

- **Define variables above first use.** Make expands a variable in a target *or a
  prerequisite* when it reads the rule, so a variable defined lower down expands
  to nothing and the dependency silently is not there. This has happened three
  times. `tests/deps` now fails on any `--warn-undefined-variables` warning.
- **macOS ships GNU Make 3.81.** No grouped targets (`&:`) — a two-target rule is
  two rules sharing a recipe, which races under `-j`. Mtimes compare at
  **whole-second** granularity though APFS records nanoseconds, so a file edited
  in the same second as the build that consumed it is missed.
- **Never depend on a phony target** for a real file. Phony means always out of
  date; that once recompiled 39 objects on every single `make`.
- **Order-only (`| foo`) means "exist before me", not "I depend on you."**
- Chained pattern rules make the middle file an *intermediate*, which make
  deletes. `.SECONDARY` where that matters.

`tests/deps` asserts the graph with `make -q` (nothing is compiled) and includes
negative controls. If you add build rules, add cases — and verify by mutation
that they can fail.

## Porting a program

The recurring task. Steps 4 and 6 are the ones most often skipped and the two
with a history of costing multi-round debugging.

1. `tools/import.sh v8/usr/src/cmd/NAME` — never copy by hand; this records the
   upstream blob hash in `PROVENANCE` so the diff against pristine V8 stays
   reconstructible.
2. LP64 audit before building, not after. The hazard shapes:
   ```bash
   grep -nE 'char \*[a-z_]*\(\);|\(int\) *signal|int +[a-z]+ *= *(malloc|sbrk)' src/cmd/NAME/*.c
   ```
3. Makefile block. Use `$(V8CC_DEPS)` on object rules and `$(V8DEPS)` /
   `$(V8LIBS)` / `$(V8LDFLAGS)` on the link rule — never respell the library
   list, and never introduce a variable below its first use.
4. **Declare any `#include`d file that is not a `.h`.** Invisible to every
   dependency scanner *and* to a `*.c` glob. Note the `[ \t]*` after the `#` —
   V8 writes `# include`, so a pattern anchored on `#include` silently finds
   nothing, which is the failure this step exists to prevent:
   ```bash
   grep -rnE '#[ \t]*include[ \t]*"[^"]*"' src/cmd/NAME | grep -v '\.h"'
   ```
5. Add the program to `.PHONY` and to the `stage0` target.
6. Add cases to `tests/deps` for the new rules, including step 4's. Verify by
   mutation that they can fail.
7. Write `src/cmd/NAME/PORTING.md`: what changed and why, what was eliminated by
   measurement, what is still open. Then add cases to the relevant wave suite.

Object files land in `build/stage0/NAME/`; `rootfs/` is the installed view that
`$V8ROOT` points at, and the copies there are real make targets — a program is
not testable until it is installed.

## Conventions

- Each ported program gets a `PORTING.md` recording what changed and **why**, what
  was eliminated by measurement, and what is still open. These are the project's
  memory; read the relevant one before touching a program.
- Prefer measuring over reasoning. The hardest bugs here were settled by making
  the program print what a value *is* rather than arguing about what it should
  be — `V8DBG=1`, or logging what `malloc` wrote versus what was found there later.
- A guard that has never been seen to fail is not a guard. New test suites are
  verified by mutation (break the thing, watch the test fail, restore).
