# Porting Research Unix V8 Userspace to macOS (ARM64)

**Status:** Phases 0 through 2b complete and tested; **rung 3 of the bootstrap ladder is closed — the compiler reproduces itself (PLAN S4c);** **Wave A done** — 156 of 163 single-file commands in `usr/src/cmd` build, and the seven that do not are accounted for individually (see the foot of `tests/wavea/run.sh`); none is a compiler defect. 278 tests. **Wave B done**; **Wave C done** — nroff, troff, tbl, eqn, pic, spell, man, grap and refer all work; `grap | pic | troff` draws a graph end to end and refer resolves citations against an index its own tools built. V8's own yacc and lex build V8's grammars. **Struct-by-value is implemented (PLAN S4f); the compiler has no known unimplemented feature left.** 495 tests. Phases 4 and 5 not started.
See "Current state" at the bottom.
**Project arc:** V8 first (this plan), then **V9 as the achievable terminus for a complete system**, with **V10 mined selectively** — its source is a pared 1995 snapshot that TUHS itself says is "unlikely" to be made into a working system, and its kernel is reorganised wholesale. V8 is the beachhead where the lessons are cheapest; §8a has what was measured and what it changes.
**Targets:** macOS on Apple Silicon (primary), Linux on ARM64 (secondary).
**Source of truth:** `third_party/Research-Unix-v8/` (TUHS release, Alhadis git mirror; case-collision recoveries documented in `third_party/Research-Unix-v8/CASE_COLLISIONS.md`).

**Layout, and paths in this document.** The repository holds a *series* of
ports, so it is split by what varies per release: `v8/` carries `src/`, `shim/`,
`compiler/`, `tests/` and its own `Makefile`, `build/` and `rootfs/`, and `v9/`
and `v10/` become siblings. `third_party/`, `tools/`, `.claude/` and the prose
are the repository's and are shared. The root `Makefile` only dispatches, so
`make`, `make test` and `make install` still work from there.

**Every path below is relative to `v8/`** unless it starts with `third_party/`,
`tools/`, `.claude/` or `.github/`. The split is by what actually varies, which
is why it is not simply one directory per release: `compiler/ccom-arm64/` is
about arm64 and Mach-O and `shim/kern/` and `shim/libkmemu/` are about macOS, so
a V9 tree inherits their content even though it gets its own copy. Same
authentic-versus-machine-dependent line `src/sys/h/` and `shim/kern/h/` draw one
level down (§8a step 1).

---

## 1. Goal and fidelity contract

Recreate the V8 userspace *experience* on a modern Mac: the real Bourne shell, the real
`awk`/`sed`/`troff`/`make`, compiled by the real V8 C compiler, running against the real V8
libc — with the host OS underneath standing in for the VAX kernel.

**The authenticity rules, in priority order:**

1. **The C compiler is authentic.** `cc` → `cpp` → `ccom` are the original Bell Labs
   programs. The one thing that cannot be original is the machine-dependent code emitter
   (no ARM64 existed in 1985); we write a new backend *inside ccom's own architecture* —
   a new `ccom/arm64/` sibling of `ccom/vax/`, doing exactly what CSRC did whenever a new
   machine arrived. Everything above it (lexer, grammar, type system, trees, optimizer,
   register allocator) is untouched original code.
2. **Assembler and linker are the host's.** `as`/`ld` are replaced by `clang -c` / `clang`
   (Mach-O on macOS, ELF on Linux). These 8,100 lines of VAX-specific tooling add no
   experiential authenticity and their object format (VAX a.out) is useless on XNU.
3. **libc is authentic C over a thin modern shim.** All portable C in V8 libc is kept.
   The 63 VAX `chmk` syscall stubs are replaced by `libv8sys`, a small modern-C layer
   mapping V8 syscall semantics onto the host. VAX assembly leaf routines (string ops,
   `doprnt`, float conversions, `setjmp`) get C or minimal-ARM64 replacements.
4. **Userspace programs are ported, not replaced.** Host passthrough is the exception,
   reserved for kernel-grovelers and machine-administration tools (§7).
5. **Non-goals:** the V8 kernel, the V8 filesystem on real disks, Datakit hardware,
   multi-user login/auth (the Mac owns identity), and binary compatibility with VAX a.out.

---

## 2. Ground truth (from full-tree survey)

| Area | Facts |
|---|---|
| Tree | Full installed-system snapshot: `v8/{bin,etc,lib,usr}` + sources under `v8/usr/src`. 352 programs with source; 290 entries in `usr/src/cmd`. |
| Compiler | `ccom` is **not** table-driven pcc: Bell Labs replaced pass-2 matching with a hand-written recursive code generator. Machine-independent pass 1 ≈ 8,200 LOC; linked VAX-dependent emitter ≈ **3,627 LOC** (`gencode.c` 1274, `genaux.c` 768, `local.c` 636, `local2.c` 319, `debug.c` 314, misc). One binary, no intermediate file. `macdefs.h`: 32-bit everything, `NOLONG` ("map longs to ints"), signed char, `STATSRET` (structs returned via static area). |
| Driver | `cc.c` (456 LOC) execs `/lib/cpp → /lib/ccom → [/lib/c2 if -O] → /bin/as → /bin/ld` by absolute path. |
| cpp | Reiser cpp, 1,358 LOC. Lexer tables biased `+128` — architecturally assumes **signed char**. |
| c2 | Peephole optimizer, 2,554 LOC, VAX-mnemonic-specific. Not retargetable; dropped (see §5). |
| f77 / lint | Ride the *older* `pcc1` tree (a second, table-driven VAX backend). f77 therefore does **not** come free with our backend. lint never generates code → cheap to port. |
| libc | 252 files, ~11.4k LOC. 63 `chmk` stubs (+4 in `libjobs`) = complete shim API. 34 non-syscall VAX `.s` files; 11 string routines have portable `.C` references in-tree. `doprnt` (printf engine) is 765 LOC of VAX asm. `atof`/`frexp`/`ldexp`/`modf`/`ecvt` assume VAX D-float bit layout. malloc = Ritchie first-fit over `sbrk`. |
| Directories | On-disk format is V7: 16-byte entries (2-byte ino + 14-char name). `readdir()` exists but is itself a shim doing raw `read()` of that format; **44 commands read directories raw**. |
| ABI surface | `varargs.h` = pointer arithmetic over a contiguous stack arg block. `setjmp` = VAX frame-walking. `struct stat` has 16-bit `ino_t`. Signals 1–25, no masks/sigaction; numbering ≈ macOS's BSD numbering (differences: 16 unused, 23 SIGTINT). |
| Blit/jerq | Two sibling trees (68000 Blit; WE32000 5620), 267k LOC total. Wire protocol fully specified in ~664 lines of headers + `proto.3c` man page. Terminal is *programmable*: `mux` downloads `muxterm`; apps (`jim`, `proof`, `pi`) download their own terminal halves. Mouse/keyboard never cross the wire. Host `mux` (909 LOC) needs kernel streams line disciplines. `sam` is not in V8; `jim` is the editor. |
| Coverage gaps | All 35 games are binary-only (no source). Sourceless oddments: `e`, `pg`, `more`, `rmail`, assorted scripts. |

---

## 3. Repository architecture

```
v10-unix-userspace/
├── PLAN.md                     ← this file
├── third_party/Research-Unix-v8/   pristine upstream; NEVER edited
├── src/                        ported sources (copied from upstream, then patched)
│   ├── cmd/…                   mirrors v8/usr/src/cmd layout
│   ├── libc/…
│   └── PROVENANCE files        origin path + upstream blob hash per import
├── shim/                       libv8sys — modern C, clang-compiled
│   ├── include/                host-facing internals
│   └── v8sys/                  one file per syscall group
├── compiler/                   only NEW compiler code: ccom/arm64 backend, crt0.s,
│   └── …                       setjmp.s (front/mid-end stays under src/cmd/ccom)
├── tools/                      import script, build glue, test harness
├── tests/                      golden-output fixtures; compiler bootstrap checks
└── rootfs/                     BUILD OUTPUT: v8-style tree (bin, usr/bin, lib, usr/man…)
```

(`blitterm/` was here until Phase 5 was dropped. It never existed on disk —
this listing described the *intended* tree and nothing checked it against the
real one, which is why a directory that was never created sat in the layout
diagram for the whole life of the project.)

**Rules:**
- `third_party/` is read-only reference. Every ported file is *copied* into `src/` by
  `tools/import.sh`, which records the upstream path and git blob hash in a `PROVENANCE`
  file per directory. Diffs against upstream are always reconstructible.
- Vendoring decision (pending): commit `third_party/Research-Unix-v8` to this repo as a
  plain snapshot (drop its inner `.git`, record upstream commit `HEAD` hash in
  PROVENANCE). Upstream is a frozen archive; submodule ceremony buys nothing.
- License: the TUHS/Alcatel-Lucent grant is **non-commercial**; this repo must stay
  non-commercial and carry `COPYING.pdf` forward. New code (shim, backend, Swift app)
  can be MIT but the combined work inherits the restriction.
- Entry point: `rootfs/` + a `v8` launcher (adapting the real `cmd/v8.c`, which chroots
  into `/v8` and runs `sh` — we swap chroot for PATH/MANPATH environment). No sandbox.

**Build system:** top-level POSIX `Makefile` orchestrates stages. Original per-program
makefiles are kept as reference and used where they run unmodified under ported V8
`make`; a thin `Mk.host` wrapper per component supplies paths/flags. Once the toolchain
self-hosts, the world builds with V8 `make` + `v8cc` (dogfooding is the point).

---

## 4. Target model (decisions)

| Item | V8/VAX | Our target | Why |
|---|---|---|---|
| int / long / ptr | 32/32/32 (`NOLONG`) | 32/**64**/**64** (LP64) | **Forced, not chosen** — see §4k. macOS has no ILP32 process model, and ILP64 is unavailable because ccom has exactly four integer types and the port needs exactly four widths. `NOLONG` assumptions become a known bug class, hunted with ported `lint`. |
| char | signed | signed | Matches VAX; Apple ARM64 agrees natively; Linux needs `-fsigned-char` for stage-0 and explicit signedness in our backend. |
| Floats | VAX F/D | IEEE 754 | Rewrite the 5 format-dependent routines; numeric output may differ in last digits — accepted divergence. |
| Struct return | `STATSRET` (static area) | keep `STATSRET` | Authentic V8 semantics, simplest backend, and the shim boundary passes only scalars/pointers so host-ABI struct return never matters. |
| Args/varargs | contiguous stack block | **prologue spills x0–x7 to a contiguous block** | Makes `varargs.h`, `&arg` arithmetic, and prototype-less K&R varargs calls work unmodified. Classic retargeting trick; costs a few stores per call. **Empirically confirmed mandatory:** clang-built `ccom` segfaults in V8's own printf (`printx` takes `&list` and `sprintxl` walks it) because AAPCS64 passes the first 8 args in registers. Once v8cc compiles `printx.c`, it works unmodified. See `src/cmd/ccom/PATCHES.md`. |
| Symbols | `_`-prefixed | `_` on Mach-O (native match!), none on ELF | The 1980s underscore convention *is* the Mach-O convention — a happy accident. Backend flag per target flavor. |
| Addressing | absolute | PIC via `adrp`+GOT uniformly | macOS requires PIE. Uniform GOT access keeps the emitter simple; ld64 relaxes local cases. |
| Errno / signals / stat | V8 values | V8 values inside the world; shim translates at the boundary | Programs see authentic numbers; `perror` prints authentic strings. |

---

## 4a. The bootstrap ladder (revised)

The original plan built the toolchain and then went straight to userspace, with
the **host's** make driving everything and V8's make listed in Phase 1a and
never landed. That is not a bootstrap: the build description was ours rather
than Bell Labs', and the host's userspace was executing every recipe.

The correction, in order. Each rung is built by the rung above it.

| Rung | Built by | Contents |
|---|---|---|
| 0 seed | host clang, host make, host yacc | `cpp`, `ccom-arm64`, `cc`, `libv8sys`, `crt0` — the irreducible minimum |
| 1 tools | v8cc, under GNU make | `libv8c` → **yacc** → `lex` → **make** |
| 2 jail | v8cc | `/bin`: sh, and the filters the makefiles invoke |
| 3 close the seed | v8cc + V8 yacc | regenerate cpp's `cpy.c` with V8 yacc; **fixpoint** v8cc₁ ≡ v8cc₂ |
| 4 handover | **V8 make, inside the jail** | rebuild the compiler with it |
| 5+ world | V8 make, authentic makefiles | everything else |

**make cannot be first.** It has a 440-line `gram.y`, so it needs yacc. The
order is `cc → yacc → make`.

**`make` is V8's build tool. `mk` is not, and will be.** Worth writing down
before the V9 step, because the two are different programs and the names are one
character apart. Measured against the vendored tree: `v8/usr/src/cmd/make/`
exists, `man1/make.1` exists, there is **no `mk`, no `mk.1`, and not one
`mkfile` anywhere** — the only `mk.c` upstream belongs to `efl`. Every build
description in V8 is a `Makefile` in make syntax.

That is chronologically right. `mk` is Andrew Hume's successor to make, from
1987, and it arrives with Plan 9 -- and, **measured rather than recalled, with
V9**: V9's README describes "all the source and makefiles(mkfiles)", V10's
kernel carries `sys/fs/mkfile` and `sys/io/mkfile`, and `v10/cmd` ships both
`mk` and `make`. So `mk` is needed at the **first** upgrade step, not the
second, and it is a non-question only for as long as this stays a V8 port.

What it will cost when it lands, so it is a known edit rather than a
rediscovery:

- **It is a port, not a rename.** `mk`'s model differs where it matters — a
  recipe is one shell script rather than a line at a time, there are no built-in
  suffix rules, and it is parallel by default. A tree of `mkfile`s cannot be fed
  to make, and vice versa.
- **The `v8-make.sh` hook goes quiet on its own.** It decides a build
  description is authentic by looking for a PROVENANCE line naming a
  *makefile*; an imported `mkfile` matches nothing and is waved straight
  through, with the hook still reporting success. `tests/hooks` therefore has a
  case that fails the day the first `mkfile` appears in the tree, which is the
  prompt to teach both the hook and the inventory about the second tool.
- **The ladder gains a rung, not a substitution.** V9's own `mk` has to be built
  by V9's `cc`, so the handover repeats one level up: `cc → yacc → make → mk`.
  V8's make does not disappear — it is what builds the tree that builds `mk`.

**The jail is not `chroot(2)`.** Every V8 binary here is a Mach-O linked
against `/usr/lib/libSystem.B.dylib`, so a real chroot would need `dyld` and
the dyld shared cache inside it, and that cache is SIP-protected; `chroot(2)`
also needs root. But `libv8sys` *is* the kernel as far as V8 code is concerned,
and chroot is a kernel service, so it lives there: `rootpath()` resolves
`/bin/` and `/usr/bin/` inside `$V8ROOT`, and `v8s_execve` routes through it —
which it never used to, so before this, `/bin/sh` always meant the host's shell
no matter what the rootfs held.

**The jail has one deliberate hole.** `as`, `ld`, `ar`, `strip` and `nm` stay
host tools because the object format is Mach-O (§1). `strip` appears 14 times
in the authentic makefiles. That is a decision, not an omission.

**Fall-through is reported, not silent.** `rootpath()` returns the host path
when the rootfs lacks the file, which keeps a partly-ported tree usable — but
that is the exact shape of the bug that cost this port three debugging rounds
at the libc layer (`scanf`, `printf`, `execl` each resolved silently to a host
variadic function). So `V8JAIL=warn` names each escape and `V8JAIL=strict`
refuses it. `tests/jail` runs a build under `strict`: a clean run *is* the
proof, and the guard is verified to fire by pointing a recipe at `/usr/bin/awk`.

**Why the authentic makefiles matter, concretely.** `src/cmd/lex/Makefile:11`
already says `lmain.o: lmain.c ldefs.c once.c` — the exact dependency whose
absence caused the lex heap-overrun bug, which cost a full session to
re-derive. `src/cmd/tbl/makefile` likewise declares `t0.o ...: t..c`. The
dependency knowledge was in the tree the whole time, in files the build
ignored. Program builds move onto their own makefiles, minimally adapted, with
every deviation recorded in that program's `PORTING.md`.

**Seventeen of twenty are on their own makefiles**, in `tests/jail`'s rung-5
sweep: `lex`, `sed`, `fmt`, `tsort`, `tbl`, `yacc`, `spell`, `man`, `troff`,
`refer`, `ps`, `load`, `w`, `make`, `eqn`, `pic`, `grap`. All unmodified, all
under `V8JAIL=strict`. `make` is the one that closes a loop: V8's make building
V8's make from V8's makefile.

The sweep pays for itself in findings rather than in coverage. `sed` exposed a
driver gap (`-n`); `tbl` and `yacc` proved V8's make gets the
`#include`d-non-header dependency lines right; and `Cannot load mv` is how
**eleven commands were discovered to have been imported and never built** —
which in turn exposed `v8s_mknod` passing its path unresolved, the last hole
left by the creation fix.

### The seven "blockers" were four wrong answers and three real ones

The table that used to sit here named a blocker per program. Re-measured, four
of the seven entries were false, and two of those were bugs in the **sweep**
rather than in the port. Recorded in full because the error has a shape: every
wrong entry described something as *generated* or *missing* that was sitting in
the tree, unread.

| program | recorded blocker | what was actually true |
|---|---|---|
| `make` | "`ident.c` — generated" | `ident.c` is checked in — a version string and a changelog comment. `$(OBJECTS): defs` names another `#include`d non-header, and the **sweep did not copy it**, so make said `Don't know how to make defs`. Copies it now; builds. |
| `eqn` | "link fails after `cc -g`" | Upstream declares no target named `eqn`; the target is **`a.out`**. Asked for the wrong one, make fell through to its built-in `.c`→executable rule, linked `eqn.o` alone and failed on every symbol in the other 21 objects — which reads exactly like a broken link. Builds. |
| `pic`, `grap` | "`_errno`; want a `libm` this port never built" | Real, and the diagnosis was inverted — see below. Both build. |
| `sh` | "`msg.o` — a generated source" | `msg.c` is checked in. The makefile runs `:fix msg`, a **VAX shared-text helper**, and that is a genuine stop — see below. |
| `cpp` | `-Dvax=1`, plus `:yyfix` | Genuine, and `:yyfix` turns out to be the *same* stop as `sh`'s. |
| `df` | "`libkmemu`, which upstream never had" | Genuine, but not the same failure as `load` and `w`. `src/cmd/df/PORTING.md`. |

**`-lm` was the interesting one, because the answer was in the archive.** Eleven
upstream makefiles link `-lm`, which reads as a claim that V8 had a math
library. `v8/usr/lib/libm.a` is **216 bytes**: one member, `dummy.o`, 62 bytes,
whose entire symbol table is the name `_________`. It defines nothing. V8's math
is in `libc/math` — which this port now links into `libv8c` — so `-lm` on V8
resolved to an empty archive and always had. Our driver was handing the flag to
clang, which answered with the macOS SDK's libm, a **libSystem re-export placed
ahead of `libv8c` on the link line**; `_errno` then resolved to an indirect
symbol with no address and ld died on
`fixup error (kind=arm64_adrp_lo12) ... target '_errno' does not have address`
— a message naming neither libm nor the jail.

That exposed the general hole behind the specific one: **every `-l` escaped to
the host SDK, unchecked.** `libpath()` in `src/cmd/cc.c` now resolves `-lNAME`
against `$V8ROOT/usr/lib/libNAME.a` first — the same union rule `rootpath()`
uses, reported under `V8JAIL` when it falls through — and `shim/libm/dummy.c`
reproduces V8's stub, so the fix stays a general rule about where libraries live
instead of a flag the driver knows by name.

### Where rung 5 genuinely stops: 1985 wanted its data in read-only text

The three that remain are not three problems. Two are one problem, and it is
the target refusing rather than the port lacking.

| program | the stop |
|---|---|
| `sh` | `:fix` compiles to assembly, rewrites `.data` to `.text` with `ed`, reassembles. `msg.c` holds `struct sysnod commands[]` — a table of **pointers** — and an initialised pointer in `__TEXT` is a text relocation. `-no_pie` is **ignored for arm64**, so it can never be resolved statically the way a.out resolved it at link time. `:fix ctype` works; `:fix msg` cannot, here, ever. |
| `cpp` | `CFLAGS=-O -Dunix=1 -Dvax=1 ...`, and `cpp.c` tests `vax` in three places — running it as written builds a VAX preprocessor. Plus `:yyfix`, which lifts the yacc tables into `rodata.c` and compiles it with `cc -R`; V8's driver passes `-R` straight to `as`. **The same optimisation as `:fix`, by a different mechanism.** |
| `df` | This port modified `df.c` to call `kmemu_fsstat` (0 occurrences upstream), so the link fails. Unlike `load` and `w`, whose source is untouched and whose makefiles therefore work — they build a real program that says `No mem`. A **source** change of ours broke a makefile that was adequate. |

So the boundary is principled rather than a to-do list: rung 5 stops where the
build *description* names the target machine, and there are two ways it can do
that — a flag (`cpp`) and a helper (`sh`, `cpp` again). Both are asking for
shared read-only text, which a VAX gave and an arm64 Mach-O structurally cannot.
`tests/jail` asserts the boundary: the rewrite happens, the result assembles,
and the link then fails on a text relocation. Making that link succeed would be
a real change in what this port can claim, and it has to come past a failing
test to say so.

`df` is the odd one and the useful one: it shows that a source change made for
this port can break the rung-5 claim for a program whose makefile was fine all
along.

### Admin/Mk closes the other half of cmd/, and it runs verbatim

The seventeen above all have makefiles. **More than half of `cmd/` does not**,
and for those the build description is `Admin/Mk` -- a shell script, not a
makefile, so rung 5 for them is a different claim and needed different work.
For each bare `*.c` it does

```sh
	eval D=`Admin/dest $B`
	cc $CFLAGS -o $B $B.c && install $B $D/$B    # strip $1 && cp $1 $2
	rm -f $B.o $B
```

Run unmodified inside the jail under `V8JAIL=strict`, it builds, installs and
cleans up **all fifty** of this port's single-file commands. What that exercises
is not the compiler but the world around it: V8's `sh` doing `set -p`, shell
functions, backquotes, `eval` and `case`; two nested shell scripts with **no
`#!` line**, which V8's `sh` runs itself when `execve` answers ENOEXEC; and
upstream's own install-destination tables. Three host execs in total -- `clang`
twice per program, `strip` once -- and `tests/jail` names them.

Three things it took, each small and none guessable:

- **`/usr/src/` in the mount table**, because `cd /usr/src/cmd` is the one
  absolute path in the script. `$(SRCTREE)` stages `Admin/` and the fifty
  sources there.
- **`strip` in `hosttools[]`.** `install()` is `strip $1 && cp $1 $2`, so a
  refused strip short-circuits the `&&` and nothing installs -- a jail decision
  that presents as a build failure. Reached by the ordinary union fall-through,
  not a new mechanism, so it is PLAN S1's documented exception finally becoming
  reachable, exactly as `as` did.
- **`$(ADMIN)` moved from `third_party/` to `src/cmd/Admin`**, imported with
  PROVENANCE, so the tables the Makefile reads at build time and the ones `dest`
  reads at run time are one copy and cannot drift.

**`who` is the fiftieth program and it does NOT match ours, which is correct.**
Third instance of the `load`/`w` distinction and the cleanest: the build
description is complete -- `cc -Od2 -o who who.c` -- and the binary it produces
says `who: cannot open /etc/utmp`, because `libkmemu` reaches the link through
this port's own groveler rules and deliberately not through the driver's default
library list. `nm -u` on it is empty where ours has the three `utmpx` imports.
`tests/jail` asserts the pair rather than papering over it.

That case first asserted the opposite, passed locally, and failed on a GitHub
runner -- **and the runner was right.** `/etc/utmp` is manufactured lazily by
the first reader, so on a machine where any earlier run had made one, `cp -a`
carried it into the copy and the Mk-built `who` read a file `libkmemu` had left
behind. A fresh runner is the only machine with no history. It is a third shape
of the host-property trap, recorded in CLAUDE.md: not a property of the machine
but of what ran before it, and the question to ask of any green suite is
*would this still pass on a tree that has never been used?*

And it produced a cross-check nothing else could: `Admin/dest` and
`$(call v8dest,...)` are **two independent derivations of the same answer**, in
two languages at two times, and `tests/jail` compares them for all fifty. They
agree on every one -- which is what makes the Makefile's deliberate omission of
the `ulibfiles` arm a measured choice rather than a gap. `tests/wavea` pins the
six names where the tables and the shipped tree genuinely disagree about a
command -- `crontab`, `lint`, `login`, `man`, `pstat`, `uucp` -- so the comment
justifying that omission is falsifiable. `man` is the one this port installs,
and omitting the arm is what makes us agree with the tree.

That case took two wrong answers before the right one, and the reason
generalises: **a name in those tables can be a command or a directory.** `font`,
`macros`, `term` and `tmac` are directories under `/usr/lib`; `lint` and `uucp`
are a command in `/usr/bin` AND a directory of the same name in `/usr/lib`. Ask
with `-e` and six become ten; ask with `-e` on one side and `-f` on the other
and `lint` and `uucp` look like disagreements about a command when the table is
describing the directory. The question this port ever has is where a *command*
goes, so both sides ask for a regular file.

### And a FOURTH kind of stop, which is the dangerous one because it succeeds

The three above all *fail*: a link error, an assembler error, a wrong `-D` you
can read on the command line. The image tools stop differently, and it was found
by measuring rather than by trying to build them.

`mkfs`, `icheck`, `dcheck`, `clri` and `fsck` are bare `cmd/*.c` programs, so
their build description is not a makefile but `Admin/Mk`, which for a `*.c`
runs

```sh
	eval D=`Admin/dest $B`
	cc $CFLAGS -o $B $B.c && install $B $D/$B
```

-- no `-D` of any kind, which is exactly right on a machine whose `param.h` says
`DIRSIZ 14`. **This port's says 254**, because host filenames need it, and the
five are compiled `-DDIRSIZ=14` by the Makefile's `$(IMGBIN)` group. So a
rung-5 build here is not blocked at all: it compiles, it links, it installs, it
runs, and `mkfs` writes an image with 256-byte directory records -- `i_size 512`
with `..` at offset 256 instead of `i_size 32` with `..` at 16.

**And all three checkers pronounce that image clean.** icheck never reads a
directory; dcheck and fsck read 16-byte records, find `d_ino = 0` in the 240
bytes between `.` and `..`, and skip them as deleted entries -- V7's own
encoding -- then find `..` where the 254 writer left it and count two entries
against two links. A wrong writer is invisible to every reader this port has.
`tests/mkfs` section 8 builds it that way and asserts the difference on the
bytes, which is the only place it is visible.

So this is `df`'s lesson again and worse: a change of *ours* -- here a header
rather than a source file -- invalidates a build description that was fine, and
this time nothing fails to tell you. It also puts a real precondition on ever
running `Admin/Mk` for these five: the flag has to come from somewhere the
description can see, or `param.h` has to stop being two numbers at once. That is
§8a step 5's question too, and it is already written down there -- a jailed
program reading a mounted image wants 16-byte records while a passthrough
directory gives it 256-byte ones.

**And it closes on step 4 below, not on anything of its own** — which is only
visible by reading `dfree()` rather than reasoning from "groveler". Upstream
does `fi = open(file, 0)` and then `bread(1L, &sblock, sizeof sblock)`: a
`struct filsys` off the raw device. An unmodified `df` therefore wants a real V8
filesystem image with a valid superblock in block 1, which is §8a step 4's
`mkfs`. Not `/dev/kmem`, which is `w`'s question and a different one. Grouping
the two grovelers under "needs kernel memory" would have aimed the work at the
wrong step — the same error as planning `ps` on `libproc` when it is a `/proc`
client.

## 4d. Shell scripts run inside the jail — CLOSED

A `#!` line is resolved by the host kernel, against the real filesystem, before
the shim gets a say. `/usr/bin/man` opens `#!/bin/sh`, so XNU ran the Mac's
shell, which looked for the Mac's `/usr/man` and ran the Mac's commands and
never called `rootpath()` once. **Every shell script in the world was leaving
the jail this way** — invisibly, because a script that works looks exactly like
a script that works. It was the last hole in the chroot.

`v8s_execve` now reads the interpreter line itself and rewrites the exec through
`vpath()`, which is what a kernel does; the shim is this port's kernel. V7's
rules: interpreter plus at most one argument, no recursion into a second script,
any failure falls through unchanged rather than inventing an error.

**The bug that broke the first attempt, because the shape recurs.**
`rootpath()` returns a pointer into its *own static buffer*, so resolving the
interpreter with a second `vpath()` call overwrote the buffer the argv was still
holding as the script name — argv[1] and the interpreter became the same
string, and V8's `sh` was handed itself to interpret. The script path is now
copied out before the second call. Any code holding two results from
`rootpath()` at once has this problem.

Mutation-verified: disabling the interception fails both jail cases.

## 4e. spell — CLOSED, on V8's own word lists

    $ spell sp.txt
    jumpd
    teh

against `hlista` as Bell Labs shipped it. `spell` is installed per
`src/cmd/spell/Makefile`'s own layout, `deroff` is ported, and `tests/wavec`
checks both a locally generated list and upstream's binary one.

**The bug was that `rhuff()` read a struct straight off disk**, so
`sizeof(struct huff)` *was* the header size of every word list V8 ever wrote —
7 × 4 = 28 bytes under `NOLONG`, and 56 for the identical declaration on LP64.
Beyond the size, `cs` and `xqcs` are marked "left justified", and the
justification is to `L`, which `huff.c` derives as `BYTE*sizeof(long)-1`: 31 on
the VAX, 63 here. **The file is justified to 31 and the decoder to 63.**

`rhuff()` now reads an explicit 28-byte `struct dhuff` and shifts those two
fields up by `L - DL` on load; `whuff()` is its exact inverse. Everything else
in the header is a count and moves unchanged. The in-memory type is untouched,
so the decoder's arithmetic is exactly V8's.

**Four earlier attempts failed, and the pattern in them is the useful part.**
Each changed one declaration — narrow the struct to `int`; read an on-disk
struct without re-justifying; pin `L` and `MASK`; add `-DHALFWORD`. Every one
addressed a real difference and none was sufficient, because three facts have
to hold at once: the file's *size*, the file's *justification*, and the
decoder's *word*. Fixing any two loads the table and then mis-decodes, which
looks like progress and is not.

Two things caught what the wrong output did not. The suite failing to
**terminate** stopped attempts 1 and 2 — the wrong output was visible
immediately and was not enough on its own. And fixing the reader without the
writer passed a locally generated list while breaking upstream's, which is why
`tests/wavec` now checks **both**: a reader and writer can agree with each
other and be wrong about the format together.

`-DHALFWORD` is *not* in the fix. It was added on the strength of a comment
that matches LP64 exactly (`sizeof(unsigned)==sizeof(long)/2`) and removed
again when the mutation test showed spell correct without it.

## 4f. struct-by-value (STARG) — CLOSED, and grap with it

`grap` is in `stage0`, installed, and runs end to end through `pic` and `troff`.
It used to stop at

    compiler error: gencode: unimplemented operator 99 (STARG)

`STARG` is pass 1's node for **passing a struct by value**, and the ARM64 back
end had never had a path for it — the note above `acctype()` had said so since
the back end was written, as a thing no program had yet required. grap requires
it: `Point` is `{struct obj *obj; double x, y;}`, 24 bytes, and
`line(type, p1, p2, desc)` takes two of them by value. Nothing else in the tree
does, which is how it survived 156 Wave A programs and all of Wave B and C.

### The convention: decided, positional

**Copy the aggregate into consecutive 8-byte argument slots** — the V8/VAX
convention, where the VAX pushed the struct whole (`ccom/vax/stin:276`).

The alternative was AAPCS64: composites over 16 bytes by reference, 16 and under
in one or two registers, all-float structs in up to four float registers. Right
for a call into host code, wrong here. v8cc already passes every argument
positionally in x0–x7 with a spill area, deliberately, because that is what
makes V8's `printf(fmt, args)` work — Apple's ABI puts variadic arguments on the
stack. Taking Apple's aggregate rule would have left one convention for scalars
and another for structs, which is neither. The cost is that a V8-compiled
function taking a struct by value cannot be called from clang-compiled code;
nothing at the `libv8sys`/libSystem boundary does that.

### What the implementation turned on

- **The callee needed nothing.** Pass 1's `oalloc()` (`common/pftn.c:1516`)
  already lays a struct parameter out as `tsize()` contiguous bytes in the
  argument area, since `BACKPARAM` is undefined here. The two halves agreed all
  along; only the caller was missing.
- **`countargs()` counts slots, not arguments.** Getting that wrong is silent:
  with a struct counted as one slot, grap builds, links, and emits nothing.
  Mutation-verified in `tests/wavec`.
- **`stn.stsize` arrives already rounded** to a multiple of `ALSTACK` —
  `argsize()` (`common/catch2.c:177`) mutates the node in place with `SETOFF`.
  Measured: a 12-byte struct presents as 128 bits, a 5-byte one as 64. The copy
  therefore reads up to 7 bytes past the object, which is exactly what the VAX
  did from the same field with `ALSTACK` 32. Authentic, not a defect; recovering
  the true size would mean patching pass 1, a change by taste.

Verified against clang-compiled equivalents on three shapes: a 24-byte struct in
registers, one straddling the x7/stack boundary (slots 6,7,8), and a struct
argument whose address expression contains a call.

### And it closed the `acctype()` limitation, which it also caused

`acctype()` widens an int `VPARAM` to its full 8-byte slot, because K&R makes an
undeclared parameter `int` and 271 parameters across 109 files hold a pointer in
one. Its note had recorded, since the back end was written, that this is wrong
for an int **member** of an aggregate parameter — and that the case was *"not
reachable for aggregates larger than 8 bytes, those hit the unimplemented STARG
path first"*. Implementing STARG made it reachable, and the first 12-byte struct
of three ints found it: each member was read with an 8-byte load that swallowed
the next, so the sum was right in the low 32 bits and rubbish above them. Not a
crash — visible only once something reads the result as a long.

The note's prescription was *"have pass 1 mark member references"*. What it gets
instead is the same information through an interface pcc already provides:
`bfcode()` is handed the parameter symbols with their declared types, so
`arm64_aggparam()` in `local.c` records the byte ranges of the aggregate ones
and `acctype()` asks before widening. **No authentic source changed.** The
earlier instance — pic's `makeattr()`, fixed at the source in
`src/cmd/pic/misc.c` — stays fixed there, since that change was right
independently: the union genuinely went from 4 bytes to 8.

`tests/v8ccom` carries four cases for this, including the union verbatim from
the note and a control asserting a genuine undeclared pointer parameter is
**still** widened. Mutation-verified in both directions: removing the guard
fails three of them, and removing the widening entirely fails the build.

### What it found on the way out: pic's TROFF token

grap emits `.lf` on every graph, and `pic` **segfaulted** on it. `picy.y`
declared `%token <i> TROFF` while `picl.l` stores `yylval.p = tostring(yytext)`,
so on LP64 the pointer lost its top 32 bits — measured under lldb as a fault at
`0x4a57c50`, which is `0x104a57c50` with the leading digit gone. Fixed by the
declaration; the sweep of all eight pointer-valued tokens found no others. See
`src/cmd/pic/PORTING.md`.

The general lesson, and it is the same one as the rung-5 makefiles: **grap alone
looked perfectly correct.** The bug only existed at the seam, and the seam is
only exercised by running the preprocessor into what consumes it. `tests/wavec`
now runs `grap | pic | troff` and asserts drawing commands come out the far end.

## 4g. A signedness repaint in pass 1 — CLOSED

`printf("%lx", v)` dropped the last digit of a **negative** long:
`0xbf1a36e2eb1c432d` printed as `bf1a36e2eb1c432` followed by a NUL. It looked
like a fault in `src/libc/stdio/doprnt.c`, this port's C replacement for
`doprnt.S`. It was not; `doprnt.c` never changed.

### What it actually was

`optim.c`'s `sconvert()` drops a conversion that changes nothing but signedness
and paints its type onto the operand. Its own header comment says so — *"the
unsigned-ness is ignored"* — and for almost every operator that is sound, since
a signed and an unsigned value of the same width have identical bits, so the
conversion is a no-op **on the result**.

Division, remainder and right shift are the exceptions. For those the type is
not a description of the result but an **instruction selector**: `gencode.c`
reads the node's own type to choose `udiv`/`sdiv` and `lsr`/`asr`. Repainting
the node therefore changes what it computes, silently. The file already knows
this elsewhere — the `ASG`-op block a few lines above declines the identical
rewrite with *"this is not true for /=, %=, or floats"*.

Guarded under `SIGNCONVKEEP`, at the `paint:` label rather than at either jump,
because there are two ways in: `t == lt` once both sides have been `DEUNSIGN`ed
(a same-width conversion) and the fall-through at the bottom (a narrowing one).
Measured: both were broken.

### Why it survived this long

It needs an unsigned operand **with its top bit set**, and nothing in the tree
had one reach a signed context. `lookup.c`'s two hashes — the compiler's own
symbol table, the busiest `%` in the build — both mask with `& 077777` first,
so the top bit can never be set and the compiler self-hosted over the bug
without trouble. `spell`'s hash shifts 4-byte `unsigned` into 8-byte `long`,
which is a genuine widening and was always kept.

Not strictly an LP64 fault: a VAX would have repainted `(long)(u % b)` too. What
the target changes is *reachability*. In 1985 it took a value above 2^31; under
LP64 it is any pointer, or any long holding a bit pattern — which is exactly
what `doprnt.c` hands `convert()`, and `digits[val % base]` supplies the
conversion with no cast in sight. The top bit is set on the **first iteration
only**, because `val /= base` shifts it away, so exactly one digit was wrong —
which reads as an off-by-one in a buffer, not as a signedness fault.

### What it was measured against

`digits[-3]`, not a wrong digit: the signed remainder indexed off the front of
the digit string and picked up a NUL. Confirmed by dumping the buffer rather
than by reading the source — `convert()` wrote two NULs and reported a length of
16 for 15 digits. The instruction pair `sdiv`/`msub` next to `udiv` in the same
loop, from `cc -S`, is what identified the compiler as the site.

Costs nothing: a retained `long` ↔ `unsigned long` CONV reaches `arm64_widen()`,
whose switch has no case for an 8-byte type, so it emits no instruction.

`tests/v8ccom` carries six cases (`%`, `/`, `>>`, the bare subscript, the
narrowing exit and the `%=` form) and `tests/libv8c` two more for `%lx`, `%lX`,
`%lo`, `%ld` and `%lu` of negative longs. Mutation-verified: with `SIGNCONVKEEP`
undefined all eight fail, returning `-3`, `-4`, `-5` and `0` respectively. The
`%ld` case is there so that a "fix" which merely forced the conversion unsigned
would be caught — `-1L` must still print as `-1`.

### And the same routine had a SECOND signedness fault, of a different kind

Found by sweeping after §4h's `int`-truncation bug, not by a program failing.
The section above says a signedness change "is a no-op **on the result**", and
that is true of the 32 bits and false of the 64-bit register holding them. The
back end keeps every sub-register type extended per its own signedness — an
`int` sign-extended, an `unsigned int` zero-extended — and compares with an
x-form `cmp`. So `int → unsigned` is a real `mov w,w` and `unsigned → int` a
real `sxtw`, and the paint left the register carrying the source type's
extension under the destination type's name:

```c
register unsigned u;  u = 0; u = u - 1;   /* 0x00000000ffffffff */
register int      i;  i = 0; i = i - 1;   /* 0xffffffffffffffff */
u == i                                    /* false; C says compare as unsigned */
```

**An explicit `(unsigned)i` did not fix it, because the cast is precisely what
`sconvert()` was deleting.** That is the tell — it presents as a broken
comparison rather than a broken conversion, and no amount of casting helps.

Guarded under `SIGNCONVKEEP` too, but **at the `t == lt` jump, not at the
`paint:` label** — and that placement is the whole of its correctness, inverting
the note above. The label is also reached by the narrowing fall-through, and
there the destination type is painted onto a memory reference and the *load*
does the extension: already right, and returning early would stack a CONV on a
tree `adjust()` may have rewritten, converting twice. The representation fault
exists only where the widths already agree. `ICON` is excluded because a
constant is converted by value through `ccast()`, which masks correctly.

Free above four bytes, for the same reason as before: `arm64_widen()` has no
case for an 8-byte type, so `long ↔ unsigned long` still emits nothing.

This is the **third** fault in those seven lines after `PTRCONVFULL` and
`SIGNCONVKEEP`'s first site, and `src/cmd/ccom/PATCHES.md` had written down that
it should be expected — *"a third change here should be suspected of being a
fourth"*. All three inherit one assumption from the routine's own header: that a
register is exactly as wide as an `int`.

## 4h. An `int` that never wrapped, and the two unary operators — CLOSED

`arm64_trunc()` in `compiler/ccom-arm64/gencode.c`; the account of the original
find (dumpdir's `checksum()`, and every dump tape unreadable by the two programs
written to read it) is in CLAUDE.md. What belongs here is the **sweep**, because
the first version of the operator list was the binary operators only:

| | signed | unsigned |
|---|---|---|
| `-x` (`neg`) | wrong for `INT_MIN` alone, which comes out **positive** | wrong for **every nonzero value** — the operand is zero-extended and `neg` sets all 64 top bits |
| `~x` (`mvn`) | **already correct**, and left alone: bits 63..32 all equal bit 31, and flipping every bit preserves that | wrong for **every value** |

Both hid the way the checksum did: `~mask` is nearly always consumed by an `&`
against a zero-extended value, which discards the wrong top half and restores
the right answer. Only a comparison or a divide reads it whole.

`tests/v8ccom` is 106 cases. Nine of the new ones **count instructions in
`cc -S` output** rather than checking a value, because a redundant extension
still gives the right answer — so the guard that `COMPL` is conditional on
`tyunsigned()` cannot be asserted behaviourally at all. Four mutations, each
firing on exactly its own cases: dropping `UNARY MINUS` (4 red), dropping the
`COMPL` arm (2), disabling the pass-1 guard (4), and the inverse — making
`COMPL` widen unconditionally (1 red, and it is the negative control).

**Where the sweep stopped, and what it cleared.** The question is "every place a
register's contents can disagree with the type painted on it", and the rest of
that list was checked and is sound: loads pick `ldrsw`/`ldr w` by type; the
foreign-call seam is handled at both ends by `arm64_widen`/`arm64_extendarg`;
`INCR`/`DECR` and the compound-assignment path were already in; a narrow
*automatic* is re-narrowed by its own `str w`; constants go through `ccast()`,
which masks — measured, `(unsigned)-1` was already right. **Bitfields were the
one real candidate left and they are correct**, at partial and full width in
both signednesses (`unsigned:20`, `int:20`, `unsigned:32`, `int:32` all agree
with clang). `QUEST`/`COLON` cannot be reached — gencode's own header notes they
are gone before pass 2 runs.

## 4i. The address-0 sweep — CLOSED, and it found nine crashes

The VAX put the text segment at address 0, so `*(char *)0` returned a byte of
the program instead of trapping. macOS leaves page 0 unmapped.

**WHICH BYTE, THOUGH -- AND THE ANSWER RECORDED HERE FOR MONTHS WAS WRONG.**
This section used to say `0207`, "the low byte of the a.out magic", and it is
repeated in that form in CLAUDE.md and in a dozen `PORTING.md`s and source
comments. Measured: V8's shipped binaries are **ZMAGIC** -- `od -An -tx1 -N4`
on `usr/bin/lex`, `bin/cat` and `bin/ls` all give `0b 01 00 00`, which is 0413,
not 0407. And `usr/include/a.out.h` says

```c
#define	N_TXTOFF(x)  ((x).a_magic==ZMAGIC ? 1024 : sizeof (struct exec))
```

with the kernel agreeing -- `usr/sys/sys/text.c:132` reads from `BSIZE(0)` into
`u_base` 0. So **virtual address 0 is the first byte of crt0, not the header**,
the header is not mapped at all, and the byte is `0x00`.

Every fix built on the wrong premise is nonetheless still correct, which is why
this went unnoticed:

| what the code did | with `0207` | with `0x00` |
|---|---|---|
| `strcmp(name, 0)` (`quot`) | below every name character | the **empty string**, still first |
| `atol(0)` (`ncheck`, `icheck`, `dcheck`) | non-digit, returns 0 | empty, returns 0 |
| `**++argv == '-'` (`fsck`, `hunt`, `inv`, `join`) | not `'-'` | not `'-'` |
| `oputs(0)` (`nroff`) | "a couple of stray characters" | **nothing at all** |

The last row is the one that changes, and it changes in the fix's favour:
`n2.c`'s note guessed that a few bytes came out before the first NUL, and in
fact the very first byte *is* NUL, so emitting nothing is exactly right rather
than approximately. The conclusions stand; the reasoning behind them did not.
And the first 16 bytes at virtual 0 are byte-identical in `lex`, `cat` and
`ls` -- they are crt0, so this is deterministic rather than a sample.

It also *earns* something. Because the value is known exactly, a VAX answer
can now be computed for a **structure** and not just for a single byte: read
through the VAX `struct _iobuf` those bytes give `_flag` `0xd050`, which is how
`lex`'s `fflush(NULL)` was settled in S4j below. Every source comment and
`PORTING.md` carrying the old value has been corrected, each keeping a line
saying what it used to claim -- deleting the error would lose the warning.

Three instances had been found one at a time — `refer`'s `lookat()`, `quot`'s
`qcmp`, `ncheck`'s `-i` loop — and CLAUDE.md said in so many words that the
sweep was not done. Doing it properly, by *shape* rather than by program, found
nine more, every one a measured SIGSEGV on the program's last argument:
`unexpand` (**with no arguments at all**), `icheck -b`, `dcheck -i`, `fsck -t`,
`join -o`, `join -j1`, `yacc -o`, bare `hunt`, and `-F` on `nroff` and `troff`.

Three findings are worth more than the fixes.

**The same loop existed three times and only one was swept.** `n =
atol(argv[1])` inside an option's number loop is byte-for-byte identical in
`ncheck`, `icheck` and `dcheck`. It was found in `ncheck`, fixed, and written up
under `ncheck` — so the note was filed by program rather than by shape, and the
two siblings kept crashing. `icheck.PORTING.md` had audited that very loop for a
`blist[500]` overrun and stopped one line short of the null.

**The crash is not always in the program.** `yacc -o` faulted in the *shim*:
with the output file unopenable, `error()` runs `cleantmp()`, whose two
`unlink()`s name temp files `setup()` had not yet assigned — and `dotlink()` in
`shim/v8sys/syscall.c` inspects a path before the syscall can answer `EFAULT`,
against the shim's own rule that a null path is the kernel's to reject
(`rootpath()` returns one unchanged for exactly that reason). `v8s_link` had the
same hole one function away, unreached by anything, which is the `v8s_mknod`
lesson again.

**Two were audited and deliberately left alone**, because a change to `src/` has
to be forced by the target. `make`'s `meter()` dereferences an unchecked
`getpwuid()` but returns on `meteron == 0`, which nothing sets; `ls.c:259`'s
unchecked `calloc` would *write* to page 0, and that faults on a VAX too, so
there is no answer to restore. `ls.c:225`'s `-R` loop **was** changed — it starts
at `dfplast`, which is exclusive, and past twenty entries the slot is
`realloc`'d rather than `calloc`'d — on the `sed trans[]` argument: a garbage
four-byte `fname` usually landed in mapped memory on a VAX and a garbage
eight-byte one essentially never does here. Measured honestly: ten `ls -R` runs
over a 33-entry tree did **not** fault, so it is a latent read, not a
reproduced crash.

### The same sweep found the port disagreeing with what V8 ran

`strncat` read `s2[n]`. The loop copies the byte and only then notices
`--n < 0`, overwriting it with the NUL — so the output was always right and only
the read went past, which is why nothing noticed in five callers that all pass a
fixed-width unterminated field. Identical in shape to `%.Ns` in this port's
`doprnt.c`.

What settles it is that **`libc/gen/strncat.s` exists and does not do this**.
The assembler a VAX actually executed opens `movl 12(ap),r8 / bleq L6` —
returning without touching `s2` when `n <= 0` — and scans with `locc $0,r8,(r7)`,
bounded to exactly `n`. The `.C` beside it is the portable reference, calls
itself "the `standard' for the C-library" in its own header, and disagrees with
the code shipped next to it. The overread is therefore an artefact of **this
port substituting the reference for the assembler**, and removing it restores
V8. `strcatn`, the V7-named twin, has the same body and no `.s`; its comment
records a deviation rather than a restoration, and that difference in
justification is the only difference between the two patches.

Diagnostic, needing no guard page: `strncat(buf, (char *)1, 0)` faults on the
old loop and reads nothing on the new one.

### What the suites gained

`wavea` 104 → 112, `mkfs` 146 → 151, `wavec` 59 → 64, `libv8c` 34 → 36,
`v8sys` 69 → 71. Every crash case is paired with one asserting the option still
*works*, because a guard that stops the fault by consuming nothing would pass
the first and fail the second — mutation-verified in exactly that form, plus the
four guards reverted individually.

## 4c. The self-host fixpoint (rung 3) — CLOSED

Every translation unit of `cpp` and `ccom` compiles with `v8cc` and links
freestanding, the self-hosted `cpp` matches the stage-0 one byte for byte, and
**the compiler reproduces itself**: ccom2 (built by ccom1) and ccom3 (built by
ccom2) generate byte-identical assembly. `tests/selfhost` asserts all of it.

Note that ccom1 == ccom2 is *false*, by two instructions, and that is correct
rather than a defect. ccom1 was built by the stage-0 compiler, which clang built
against the host headers, so it inherits one generation of that compiler's
beliefs — here, one unsignedness question resolved differently, showing up as a
`mov w10, w10` (not a no-op on AArch64: a `w` write zeroes the upper half).
Stage 2 washes it out. That is what a three-stage bootstrap is *for*, and
testing ccom1 == ccom2 would have been testing the wrong property.

**The bug that blocked this for most of a session**, recorded because the shape
recurs: `commdec()` emitted `.comm _stab,80040` with **no alignment argument**,
where clang emits `.comm _stab,80040,3`. Omitting it does not mean "natural
alignment" — the assembler infers one from the *size*, and for an 80KB object
inferred 0x8000, which the linker then reported reducing. Every common symbol in
the compiler was being placed by an alignment derived from how big it happened
to be, so objects moved whenever anything around them changed. It presented as
heap corruption inside `malloc` on the first identifier the compiler ever read,
and it was link-order sensitive, which is what made it look like everything
except what it was. The warning naming it appeared on every single link.

Two caveats, both real:

- The `case ASSIGN` narrowing fix in `gencode.c` (the value of `a = b` is `b`
  converted to the type of `a`) is correct C and did not fix this. It carries
  **no regression test** — two attempts to build one failed — so it is a
  candidate for reconsideration, not settled.
- `tests/selfhost`'s fixpoint cases are **not mutation-verified**. Reverting the
  alignment fix does not fail them, because that fault needed a particular
  object arrangement. A pass means "converges", not "correct".

## 4b. Phase 6 — Installation: the V8 world as something you can live in

The end state is a `v8` command that drops you into what looks like a real
Eighth Edition system, while the Mac underneath stays reachable.

```
$PREFIX/v8/            <- $V8ROOT
  bin/                 sh, cc, and the filters
  lib/                 cpp, ccom, crt0.o, libv8c.a, libv8sys.a
  etc/                 passwd, group, ttys, termcap, motd, fstab
  dev/                 null, tty, console  -- symlinks to the host's
  tmp/                 jailed, so temp files do not leak to host /tmp
  usr/include usr/lib usr/man usr/src
$PREFIX/bin/v8         the launcher
```

`$PREFIX` is `/usr/local` or `~/.local`; the latter needs no sudo and is the
default.

### Two requirements are satisfied by construction

**Host binaries see the real macOS.** They are not linked against `libv8sys`,
so they never call `rootpath()`. There is no jail for them to escape. This is
the dividend of implementing chroot in the shim rather than the kernel: it is
**per-binary, not per-process-tree**, so a native `python3` started from the V8
shell needs no special case.

**Programs the user compiles are jailed.** Anything the jail's `cc` produces
links `libv8sys`, so it resolves paths through the same rootpath. You cannot
compile a program inside the jail that escapes it.

### Decided: union semantics, V8-only PATH

Every absolute path resolves in the jail first; where the jail has nothing, the
host shows through. `ls /` shows a V8 root, `cd ~/work` reaches the real home,
`/usr/bin/python3` still finds the host's. `v8` starts with `PATH=/bin:/usr/bin`
— the V8 world alone — so a tool that has not been ported is conspicuous rather
than silently satisfied by a modern namesake. That is the failure mode that let
the `scanf` and `execl` bugs survive, and it is not worth re-creating for
convenience. Host directories are an explicit opt-in.

### The four pieces of real work

1. **`pwd` must lie correctly.** `src/libc/gen/getwd.c` calls no `getcwd`
   syscall: it walks up from `.` matching entries against `stat("..")` until
   the inode equals its own parent. Inside the jail that walks straight out and
   returns `/usr/local/v8/usr/src`. Fix: have the shim report `$V8ROOT` as its
   own parent — same `st_dev`/`st_ino` as its `..` — and getwd stops there,
   yielding `/usr/src`. The shim already reconciles getwd's expectations
   against the host at `syscall.c:1721`, so this is the same kind of lie in the
   same place, not a new mechanism.

2. **`open(O_CREAT)` resolves the parent, not the target.** `rootpath()`
   decides by stat'ing the path, but a file being created does not exist yet,
   so under union semantics every new file would fall through to the host.
   Creation has to resolve the parent directory instead. Invisible until
   someone redirects output into `/tmp` and finds it on the Mac.

3. **`$V8ROOT` no longer depends on the environment — in the shim.** `v8root()`
   in `shim/v8sys/syscall.c` takes `$V8ROOT` first and falls back to a root
   compiled in at build time (`-DV8ROOT_DEFAULT`, set to `$(ROOTFS)` by the
   Makefile and to the prefix by `make install`). Before this, an unset
   `$V8ROOT` meant `rootpath()` silently returned the HOST path and a V8 binary
   run outside the launcher operated on the real filesystem while appearing to
   work — the same silent fall-through class as the variadic libc gaps.
   `cat /usr/lib/yaccpar` now works with `$V8ROOT` unset.

   The driver has the same default, from the same `-DV8ROOT_DEFAULT`, so
   `cc -o prog prog.c` works with `$V8ROOT` unset too.

   **A retracted finding, kept because the mistake is instructive.** This was
   first written up as a v8cc bug — the define appeared in `cc-seed` and not in
   the `cc.o` v8cc produced from identical source, which looked exactly like the
   compiler dropping a quoted `-D`. It was not. The Makefile line had been
   edited with `perl -0pi -e "s|...|...$(V8ROOT_DEFAULT)...|"`, and in Perl
   `$(` is the real-GID variable, so the substitution produced

       -DV8ROOT_DEFAULT='"20 20 12 61 79 80 81 702 ...V8ROOT_DEFAULT)"'

   v8cc handles a quoted `-D` correctly; `cc -E -DFOO='"bar"'` yields
   `char *s = "bar";`. Two lessons, both already in this file's spirit: verify a
   claim about a tool against the tool, not against a build that might be
   feeding it something else — and read the command make actually ran (`make -n`
   showed it immediately) before concluding anything about the program it ran.

4. **`/etc` is not decoration.** `getpw.c`, `getlogin.c` and `ttyslot.c` read
   it. Upstream ships genuine V8 `group`, `fstab`, `hosts` and `ttys`, which can
   be imported with provenance like any other file; `passwd` has to be
   synthesized at install from the real user, or `ls -l` shows bare uids.

**B5 unblocked the driver.** `$V8ROOT/bin/cc` is now a V8 binary — compiled by
the seed driver, linked freestanding against `crt0 + libv8c + libv8sys` — so it
goes through the shim and `V8JAIL=strict cc -o prog prog.c` compiles and links
without leaving the jail. Still open before installation is honest: `cpp` and
`ccom` remain clang-built, so the *passes* the driver execs sit inside the
rootfs without obeying it.

## 5. Phase 1 — Toolchain bootstrap

**Stage 0 — host clang builds the tools that build the world.**
Order: `yacc` → `cpp` → `ccom` (pass 1 + new backend) → `cc` driver; also `make`, `lex`, `m4`.
Flag recipe: `-std=gnu89 -fcommon -Wno-implicit-int -Wno-implicit-function-declaration
-Wno-return-type` (+ `-fsigned-char` on Linux). Known patches from survey: `mktemp()` on
string literals (`m4`, `yacc`), `(int)signal(...)` idiom (`make`, `f77`), pointer-in-int
(none outside dropped `c2`). Zero `asm()` statements in any stage-0 tool — confirmed.

**Stage 1 — the ARM64 backend (`compiler/ccom-arm64/`, the long pole).**
Write `macdefs.h`, `local.c`, `local2.c`, `gencode.c`, `genaux.c`, `printx.c` siblings —
~3.6k LOC of new code in ccom's architecture. Emits AArch64 assembly acceptable to
`clang -c` (`.s` with Mach-O/ELF directives per target flavor). Register model: v8
expression temporaries on x9–x15, args spilled per §4, callee-saved x19–x28 for register
variables, honest AAPCS64 frame so host `lldb`/unwinders cope. No `-O` path: `c2` is
VAX-text-specific and is **dropped**; `-O` becomes a warning no-op in the driver
(peephole quality is not the point of this project; host assembler does the rest).
`debug.c` (.stabs) stubbed initially; revisit if `adb`-style debugging ever matters.

**Stage 2 — driver + self-host.**
Patch `cc.c` paths `/lib/*` → `$V8ROOT/lib/*`, exec host `as`/`ld` with translated
argv, keep the entire user-facing flag surface. Milestone gates:
1. `v8cc` compiles `cat.c` → runs.
2. `v8cc` compiles all of libc (§6) → world links against v8 libc.
3. **Fixpoint:** `v8cc(clang-built)` compiles ccom → `v8cc₁`; `v8cc₁` compiles ccom →
   `v8cc₂`; `v8cc₁ ≡ v8cc₂` output-identical. From then on the compiler that builds the
   world is itself built by itself — the authenticity claim becomes checkable.

**Deferred/dropped in toolchain:** `as`, `ld`, `nm`, `size`, `strip`, `ar`, `ranlib`,
`prof`, `lcomp`, `adb`, `sdb` → host tools (exception list, §7). `f77`+`pcc1` backend,
`cfront` (C++ 1.0!), `cyntax`, `efl` → stretch goals, each needing separate scoping.
`lint` → port early (pass-1 only, no codegen) with an LP64 `macdefs` — then run it over
everything we port as our `NOLONG`-assumption detector.

## 6. Phase 2 — libc and the shim

**`libv8sys` (new, modern C):** implements the enumerated 63+4 stubs. Groups:

| Group | Treatment |
|---|---|
| Pure passthrough (~30) | `read write close lseek link unlink chdir chmod chown fchmod fchown access kill umask mkdir rmdir symlink readlink dup dup2 pipe getpid getppid getuid geteuid getgid getegid setuid setgid nice sync alarm pause utime execve fork` (vfork→fork) |
| Translate structs/values | `stat/fstat/lstat` (16-bit ino via an append-only per-process table, never 0 — §8a; dev squeeze), `time/ftime/times`, `wait/wait3`, `select` (fd_set width), `open` (see directories), errno mapping host→V8 |
| Emulate | `sbrk/brk` (reserved anonymous mmap arena, monotonic break), `signal` (V8 reset-on-delivery semantics + `SIGDOPAUSE`/`SIGDORTI` packing over sigaction; number translation ≈ identity except 16/23; **and a signal trampoline of our own** — the raw syscall takes `struct __sigaction`, and the handler is entered through the `sa_tramp` userland supplies, not directly), `ioctl` (sgtty `TIOC*` ↔ termios; `FIONREAD`; `TIOC[GS]PGRP`), `nap` (ms sleep), `syscall()` (dispatch into this table), `#!` handling in execve if needed |
| Directory reads | `open()` on a directory returns a shim-backed fd streaming **synthesized V7 16-byte records** (snapshot via host `fdopendir`; names >14 bytes truncated — documented quirk). Fixes all 44 raw readers *and* V8 `readdir()` with zero source changes. |
| Stub ENOSYS | `mount umount gmount procmount swapon reboot acct stime settod profil vadvise vlimit vtimes chroot ptrace mpx` + streams `FIOPUSHLD/FIOPOPLD`; fd-passing `FIOSNDFD/FIORCVFD` stubbed now, possible `SCM_RIGHTS` emulation later (needed only by `pt`/advanced upas plumbing) |

**libc proper:** port all portable C as-is. Replacements: 11 string routines from their
in-tree portable `.C` references; `memcpy/memset/...` in C; **`doprnt` rewritten in C**
against printf(3)'s documented behavior (golden-tested); `atof/ecvt/frexp/ldexp/modf` in
IEEE C; `setjmp/longjmp` as new ARM64 assembly in `compiler/` (plain register-save;
V8 frame-walking longjmp semantics preserved observably); new `crt0.s`. `libc/math/`
constants adjusted for IEEE range. `galloc` (address-space-scanning GC allocator):
unsupported, port-on-demand if a consumer surfaces. Headers: `usr/include` imported
with an LP64 pass; `types.h`/`param.h` get the target model, everything else stays.
Also port: `libtermlib` (termcap), `libcurses`, `libmp`, `libcbt`, `libdbm`, `libplot`
(terminal backends), `libjobs`, lex/yacc runtimes.

## 7. Phase 3 — Userspace, in waves

**Wave A — pure filters (78 programs, most compile day-one after libc):**
`cat echo wc sort uniq tr sed grep egrep fgrep cmp comm join cut paste split head tail
od diff diff3 sdiff awk expr cal look rev fold expand unexpand pr mc sum crypt pack
compress compact m4 col deroff units number factor primes seq yes vis tee dd tsort …`
Golden-output tests against fixture files from `v8/usr/doc` examples.

**Wave B — shell, process, files:**
`sh` (7,225 LOC Bournegol — the flagship), `test find ls cp mv rm ln mkdir rmdir touch
du file pwd chmod chown chgrp kill nice nohup sleep time xargs? (absent — check) apply
hist getopt printenv newer tar cpio stty tty mesg reset tabs ul p dired ed qed`.
`csh` (12.5k, BSD) second-string; `ex/vi` (23k) here or Wave C — flagged `.s`/ASM, needs a look.

**Wave C — document preparation (the crown jewels, 22 programs):**
`troff/nroff` (14k shared core) with PostScript path: V8 predates devpost, so plan is
`troff -t` raw output → port `pti`/use `tcat`-style interpreter → **write a small
`devpost`-style backend or borrow the V10 postscript drivers when we get there**; interim:
nroff + terminal. Then `eqn neqn tbl pic grap ideal refer spell diction style monk ptx
checkeq deroff soelim man`. Milestone: `man 1 ls` renders the real V8 page through real
V8 troff; the whole `usr/man` + `usr/doc` tree becomes our test corpus.

**Wave D — dev tools:** `make yacc lex m4` (already stage-0), `lint` (early, see §5),
`cb ctags cflow cref mkstr xstr struct ratfor trace cbt` + `bc dc hoc` (hoc!). 

### Phase 4 started: `date` is in, and `libkmemu` needs one decision first

**Decided (and recorded here so it is not re-litigated):** `ps` shows the V8
world's own process subtree by default, with everything reachable behind a flag;
and any column with no honest macOS source prints a sentinel rather than a
plausible number. Authentic format, honest content — a fabricated `WCHAN` is the
one thing that would make the output a lie.

**Done:** `date` is imported, built and installed. It needs no kernel state at
all. Its only visible difference from the host is the zone name — `GMT+10:00`
where macOS says `AEST` — because V8 predates the tz database and names the zone
itself. `tests/wavea` compares the fields V8 and the host must agree on and
deliberately not that one.

**The blocker, found by measuring rather than by planning.** `who` reads
`/etc/utmp` as a stream of `struct utmp`, and nothing in this world writes one.
The plan says `libkmemu` should answer from `sysctl`/`libproc`/`utmpx`, which is
right — but **`libv8sys` deliberately uses no host libc**. Every existing shim
file goes through raw syscalls (`shim/v8sys/rawsys.h`, and `dir.c` says so at the
top). `getutxent(3)` is a libc call.

So `libkmemu` can be built two ways, and the choice widens the host surface
either way:

- **Parse `/var/run/utmpx` directly**, keeping the no-libc rule. Measured: the
  file is 1884 bytes = **three 628-byte records**, the first holding a
  `utmpx-1.00` signature. But 628 does not match Darwin's documented
  `struct utmpx`, so the on-disk layout is private and undocumented — and it is
  not a stable interface. Reverse-engineering it by arithmetic is exactly the
  shape that cost four wrong attempts on spell's huff format, and a wrong guess
  produces a `who` that looks right and lies.
- **Let `libkmemu` alone link host libc** and call `getutxent`, `getfsstat`,
  `proc_listpids`. Documented, stable, and correct. The cost is that one shim
  component stops being raw-syscall-only, which is a real change to what
  `libv8sys` is — the same *kind* of decision as the as/ld exception, and so the
  same kind that belongs on the sanctioned list rather than being assumed.

**DECIDED: the second.** `libkmemu` — and only `libkmemu` — may link host libc
and call the documented interfaces: `getutxent(3)`, `getfsstat(2)`,
`proc_listpids`/`proc_pidinfo`, `sysctl(3)`. This is now a sanctioned exception
on the same list as as/ld/ar/strip/nm, and it is recorded in CLAUDE.md with the
others.

The boundary, so it does not spread:

- The exception is **per-file**, not per-shim. Everything already in
  `shim/v8sys/` stays raw-syscall-only; `dir.c`'s note at the top still holds
  for it. A new `shim/libkmemu/` is where libc is allowed.
- It is allowed for **reading facts about the running system** and nothing else.
  Not for file I/O, not for string handling, not for anything `rawsys.h` already
  covers — those would be convenience, and convenience is how an exception list
  stops meaning anything.
- The reason it is justified where parsing `/var/run/utmpx` is not: the syscall
  interface is stable and documented, the on-disk layout is neither. Reaching
  for libc here **narrows** what this port depends on rather than widening it.

### `libkmemu` is built, and `who` works — with no changes to `who.c` at all

`shim/libkmemu/` exists, `who` is imported, built, installed and tested, and
`tests/kmemu` (34 cases) guards both the answer and the boundary. Full notes in
`shim/libkmemu/NOTES.md`; the short version:

- **`who.c` needed zero changes.** The shim manufactures `/etc/utmp` when a
  reader opens it, so `who` does the `fopen` it always did. A libkmemu call
  would have cost one recorded deviation per program instead — and `w` reads the
  same file. It is also what the real system did: `/etc/utmp` was an ordinary
  file kept current by `init` and `login`, both on the kernel's side of this
  seam, and the shim is that side.
- **The exception stayed narrow, and it is checkable rather than asserted.**
  `nm -u rootfs/bin/who` was recorded here as exactly
  `_setutxent _getutxent _endutxent`. **Re-measured it is all eight of
  `KMEMU_IMPORTS`, and six binaries import them, not one** -- `df ps who load
  uptime w`, identically, because `KMEMU_LDADD` is `-Wl,-force_load`. The
  claim that every other binary imports nothing holds with one stated
  exemption: `lib/ccom` (36) and `lib/cpp` (26) are still the clang-built
  stage-0 binaries. CLAUDE.md carries the current form.

**What writing the boundary test actually yielded**, which was more than the
feature. Sweeping every Mach-O in the rootfs found five functions that had been
resolving out of libSystem unnoticed — the "a missing libc function does not fail
the link" class, five more times, none of them visibly broken:

| Symbol | What it meant | Fix |
|---|---|---|
| `getgrent` etc. | **`ls -g` read the Mac's group database from inside the jail** | build V8's `getgrent.c` |
| `ftime` | a syscall this shim never implemented | `v8s_ftime` + a TZif reader in `shim/v8sys/tz.c` |
| `tolower`, `toupper` | V8 has both in C; never built | build them |
| `atof` | 319 lines of VAX assembly, so never ported | `src/libc/gen/atof.c`, new code, bit-compared against the host's |

`tests/freestanding` could not have caught any of them — it links its own small
programs, so it proved the *shim* was clean and never the world built on it. A
guard on a seam is not a guard on what crosses it.

**And one thing it broke open — CLOSED.** V8's `sleep(3)` is `alarm` + a handler
+ `for(;;) pause()`, and building it hung, because **no V8 program in this port
could catch a signal**: `v8s_signal` gave the raw `sigaction` syscall a userland
`struct sigaction` where the kernel wants `struct __sigaction`, which carries a
signal-trampoline pointer at offset 8 — exactly where the userland struct keeps
`sa_mask`. Every handler was installed with a null trampoline, and `tests/v8sys`
covered signal numbering and never delivery, which is how it survived.

The kernel does not jump to a handler; it jumps to a trampoline the process
named in `sa_tramp`, and that trampoline is what calls `sigreturn(2)`
afterwards. libc's `sigaction()` exists largely to convert the one struct into
the other and fill in its own `_sigtramp`, so a shim that goes straight to the
syscall inherits the job. `shim/v8sys/sigtramp.s` is three instructions —
XNU's entry registers are already AAPCS64 argument order, so the C half in
`signal.c` declares its parameters in the kernel's order and the shuffle
disappears.

Three flags make it V7's `signal(2)`: no `SA_RESTART`, `SA_RESETHAND`, and
`SA_NODEFER`. The last is the one that matters twice. `sigaction` blocks the
signal for the duration of the handler and `sigreturn` unblocks it, so a handler
that **longjmps out never unblocks it** — and `sleep(3)` longjmps out of its
SIGALRM handler, `sh` out of its SIGINT handler, while our `setjmp.s` saves
registers only, as the VAX original did. Without it the first `sleep()` works
and every later one hangs: the same bug in a better disguise. V7 had no signal
mask at all, and V8's header agrees from the other side — it spells deferral as
an opt-in, `DEFERSIG(handler)` setting the low bit of the handler address, which
nothing in the tree uses.

`v8s_alarm` went with it: it returned 0 unconditionally, having passed
`setitimer`'s old-value argument as 0, so every `sleep()` silently cancelled its
caller's pending alarm. Reading that value back is where Darwin's
`struct timeval` being `{ long tv_sec; int tv_usec; }` starts to matter.

So `sleep` is V8's own now, and **the allowed-leak list is what took it off**:
`tests/kmemu` fails when an entry goes stale, so nothing had to be remembered.

Still on that list, named rather than tolerated: **`pic` and `grap` use Apple's
libm** (`sin`, `cos`, `sqrt`, `atan2`, `exp`, `log`, `pow`, `floor`, `ceil`).
V8 shipped a libm and this port has never built one. Found by the same sweep,
not by a bad drawing.

**SUPERSEDED, and in the direction that mattered.** libm was excused as
"non-variadic, so it works"; it was returning *wrong answers* the whole time,
because v8cc passes doubles in `x0`-`x7` and AAPCS64 passes them in `d0`-`d7`.
V8's math is in `libc/math` and is now built, so `nm -u` on both `pic` and
`grap` is **empty** and the allowed list is `ALLOWED=""`.

**`df` and `load` are in too.** `df` needed the one sanctioned source
deviation (§7's "statfs backend") plus a manufactured `/etc/mtab` and
`/etc/fstab`; `src/cmd/df/PORTING.md` has it, including why displacing Bell
Labs' own fstab was the honest call. `load` needed **no source change at all** —
the shim manufactures a *kernel*: a namelist at `/unix` and a `/dev/kmem` with
the data where the namelist says it is, both generated from one table in
`shim/libkmemu/kmem.c` so they cannot drift apart. That machinery is what `w`
and `ps` will extend: a symbol with an honest source gets a row, and one without
gets no row, so `nlist` leaves `n_type` zero and the program says it cannot find
the symbol rather than being handed a number.

**`w` and `uptime` are in, and they closed Phase 4 in an unexpected place.**
`src/cmd/w/PORTING.md` has it; the short version is that `w` names nine kernel
symbols and gets two — `_avenrun`, already there for `load`, and `_bootime`, one
new row from `kern.boottime`. That is enough for the whole of `uptime`, which
prints the host's exact uptime and load averages in V8's 1981 format, from the
same binary under a hard link.

The other seven describe a VAX proc table reached through **VAX page tables**
and a swap device, so under the sentinel rule they get no row, and `w`'s full
form opens `/dev/mem`, finds nothing and says `No mem`. That message is now an
*assertion*: if it ever changes, something has manufactured a `/dev/mem` for
page tables to be walked in, and that is a decision to argue rather than
discover. One recorded source deviation, because the namelist check tested
`nl[0]` — `_proc` — as a proxy for "is there a namelist", and the proxy stopped
meaning that on a kernel that answers for some symbols and not others.

### REVISED, by measurement: `ps` is a `/proc` client, so it moves to §8a step 3

This section previously said `ps` would be ported "on top of `libproc`". That
was written before anyone read `ps`. V8's own does this:

```c
prlist = getdir("/proc", 0);                     /* ps.c        */
fd = open(strcat(strcpy(sstr, "/proc/"), s), 0); /* doselect.c  */
Ioctl(fd, PIOCGETPR, pp);                        /* struct proc */
Sread(fd, UBASE, up);                            /* u-area, by virtual address */
```

That is **Killian's process filesystem, V8's own invention** — `sys/pioctl.h`
carries the whole ioctl set, `PIOCGETPR` through `PIOCNICE`, including the
debugger operations, and `sys/sys/proca.c` is the filesystem. V8's `ps` has no
`sccsid` and no Berkeley attribution; `w.c` opens
`@(#)w.c 4.4 (Berkeley) 6/5/81`. **The two process tools in this one tree are
from different eras**, and only Bell Labs' own made the jump to `/proc`.

So `ps` is not a kmem groveler and should not be made into one. Bolting
`libproc` onto its front end would mean rewriting `doselect.c` and `getargs.c`
against an interface V8 had already abandoned, to avoid building the interface
V8 actually used. Both `ps` and the full form of `w` are answered by one `/proc`
server — §8a step 3 — which already existed in the sequence and is now the thing
two programs are waiting on rather than a nice-to-have. `p_wchan` stays
unanswerable either way, and the sentinel rule still covers it.

**Phase 4 is therefore complete as far as it can go without `/proc`:** `date`,
`who`, `df`, `load`, `w`/`uptime`. `ps` and full `w` are §8a step 3.

**Case-by-case for grovelers (the user-sanctioned exception list):**

| Program | Policy |
|---|---|
| `ps`, `w`/`uptime`, `load`, `who` | **Port the front end**, back it with a small `libkmemu` in the shim: process/utmp facts from `sysctl`/`libproc`/`utmpx`, presented through V8-shaped structs. Authentic output format, modern truth. |
| `date` | Port (printing); `-s`/`settod` path stubs out. |
| `df`, `du` | `du` ports cleanly; `df` via `statfs` backend, V8 output format. |
| `iostat vmstat dmesg pstat fstat oops showq finddev netstat sa ac last log wall wwv` | **Not ported** — host equivalents; V8 semantics are kernel-specific. Revisit `vmstat`/`iostat` via host_statistics if desired. |
| `fsck mkfs icheck dcheck ncheck clri quot dump restor dumpdir 512restor mkbitfs bad144 rarepl rp07* dskcpy flcopy arff hideblock fcopy tp mt tape` | **Excluded** — raw VAX disks/tapes. Possible far-future fun: V8 filesystem-in-a-file with `mkfs`/`fsck` against images. |
| `init getty login su passwd newgrp accton savecore settod reboot halt swapon mount umount procmount update cron at daemon` | **Excluded** — machine/identity administration belongs to macOS. (`cron`/`at`: host provides.) |
| Networking: `inet/*` (TCP suite), `uucp` (16.6k), Datakit (`dk/*`, `netfs`, `cu ct server track rcp dcon`) | **Excluded initially** — V8 networking lives in kernel streams + dead hardware. Stretch: Datakit-over-TCP emulation to light up `dcon`/`rx`; uucp-over-TCP. |
| `upas` (3.1k) + `Mail` (12.7k) | **Port later**, local-mailbox mode only (upas rewrite rules are a treasure). `rmail` is binary-only — reconstruct trivially or skip. |
| Games | Source exists only for `bcd`/`morse` (port). The 35 binaries: **unportable** (no source). Note for V10: check its tree. |
| `compat` (PDP-11 v6/v7 binary emulator!) | Optional curio — would let V6 binaries run on the Mac. Not scheduled. |

## 8. Phase 5 — Blit terminal (`blitterm`, Swift) — **DROPPED 2026-08-11**

**Not being built, and the two tiers are dropped for two different reasons.**

Tier 1 is redundant: `sam` and `acme` are in Plan 9 from User Space, so the
software the Blit is remembered for runs natively on this Mac already, and
rebuilding a terminal to reach it is effort spent on the frame rather than the
picture.

Tier 2 is **solved elsewhere** — in `ipad-v8`, a sibling project of this one.
That distinction is worth keeping rather than collapsing into "dropped",
because plan9port genuinely does *not* answer Tier 2: it substitutes the
editors, not the terminal, and the downloaded-program-over-a-serial-line
architecture has no counterpart in it. So the honest record is that one tier
was made unnecessary and the other was built somewhere else, and neither is a
gap in the port.

**And this heading said "Phase 4" while every table in this file said 5.** The
drift is left visible here because it is the same class as the case count
above — a number in prose that no build reads, so nothing can disagree with it
except another copy. Phase 4 is the grovelers; this was always 5.

What it *would* have been, kept because §8a's `/dev/fd` work cites it and
because Tier 3 is the only place the WE32000 is discussed:

- **Tier 1 (scheduled):** Swift/AppKit app, 800×1024 1-bit portrait bitmap (CGImage-backed,
  scalable), 9×14 cell text frames, mouse+menus, implementing `muxterm` behavior natively.
  It **absorbs the mux role** (spawns a pty per layer running `$V8ROOT` `sh`) because host
  `mux` needs kernel streams we won't have. Protocol source of truth: `proto.h`/`msgs.h`/
  `jioctl.h`/`proto.3c` (664 lines, fully enumerated in survey).
- **Tier 2 (stretch):** port terminal-side app halves natively (C on the `jerq.h` graphics
  model — `layers`/`libj` are portable C) so `jim` (host half in v8 world, terminal half in
  the app) and `proof` run. This is the two-machine architecture done honestly.
- **Tier 3 (out of scope):** WE32000 CPU emulation booting real ROMs.
- Wire-protocol fidelity option (RS-232 framing, CRC, real `32ld` bootstrap) kept as a
  toggle for Tier 2, letting unmodified host-side `jx`/downloads work someday.

## 8a. The V9 arc: a filesystem switch, spoken over 9P

Decided after measuring the V9 and V10 trees rather than from the roadmap. This
section supersedes the loose "then V9, then V10" of the project arc and gives
Phase 7 a shape.

### What was measured

**V8 has no FFS.** Searched the whole kernel -- 108 headers, 18k lines. No
`struct fs`, no `struct cg`, no `fs.h`; the only hits for cylinder-group and
fragment vocabulary are IP fragmentation and resource maps. The kernel's
`h/filsys.h` is byte-identical to the userland one, so there is no richer view
hiding below. `struct dinode` is `di_addr[40]` -- thirteen 3-byte addresses --
not FFS's 32-bit `di_db[]`/`di_ib[]`.

**But V8 has TWO on-disk formats**, chosen by `BITFS(dev) = (dev) & 64`. The
device number is the format tag:

| | free-list | BITFS |
|---|---|---|
| block | 1024 (V7 was 512) | 4096 |
| inodes/block | 16 | 64 |
| allocation | V7 free list in the superblock | bitmap, `s_bfree[961]` |
| placement | none | cylinder-aware |

The bitmap variant independently reinvents FFS's three ideas -- bigger blocks,
bitmap allocation, locality -- with none of FFS's structures. `alloc.c` says so
in its own voice: *"try for an acceptable free block in next three cylinders,
then start over at the beginning"*, `"same cylinder?"`, and, candidly,
`"unfortunately device dependent"` and `"this code is UGLY, fix it"`.
Convergent evolution, not adoption.

Three consequences. The bitmap lives **in the superblock**, not per-cylinder-group
across the disk, so 961 longs x 32 bits x 4096 is a hard **120 MB ceiling**.
Cylinder geometry is **hardcoded** as `4*10` in the allocator rather than read
from the superblock. And there are no fragments.

**`mkfs` can only create the free-list format** -- it is written in terms of
`BSIZE(0)` throughout, and `BITFS(0)` is false. Yet `fsck`, `icheck`, `dcheck`
and `ncheck` are all BITFS-aware. The kernel and the checkers handle both; the
creator handles one. BITFS looks like work that shipped in the kernel ahead of
its tooling.

**`/proc` is V8's, not something to invent.** `sys/sys/proca.c` is in V8's
kernel already -- Killian's paper is V8 -- and it is still there in V10 as
`sys/fs/proca.c`, by then one filesystem type among several.

**`mk` arrives at V9, not V10.** V9's README describes "all the source and
makefiles(mkfiles)"; V10's kernel carries `sys/fs/mkfile`, `sys/io/mkfile` and
`lccmkfile` variants, and `v10/cmd` ships **both** `mk` and `make`. So the first
upgrade step needs mk, not the second. `tests/hooks` already fails the day the
first `mkfile` lands, which is the right trigger; only the prose was wrong.

**V10 is a quarry, not a destination.** TUHS: the v10 source was "pared from a
1995 snapshot", is "in no sense a formal distribution", and "it is unlikely that
it can be made into a working system without a fair amount of hand-waving". Its
kernel is reorganised wholesale -- `fs/ io/ os/ ml/ md/ vm/` where V8 has
`sys/ h/ dev/ conf/`. V9, by contrast, is a coherent VAX snapshot from early
1987 laid out much like V8.

**So: V9 is the achievable terminus for a complete system. V10 is mined
selectively** -- `mk`, the filesystem switch, the stream evolution
(`streamio.c` becomes `io/stream.c`), `/proc`'s maturation. That is a change to
the stated arc and it is deliberate: "V10 is the goal" read as "port all of
V10", and the source cannot keep that promise.

### The seam rule, which is what keeps this faithful

9P is adopted as the **wire protocol between the shim and its file servers**,
and nothing more.

- **Below the seam**: V8 programs call `open(2)`; the shim translates; a server
  answers. The V8 world cannot tell, so this is the same category of decision as
  Mach-O instead of a.out, or clang as the assembler. It costs no fidelity.
- **Above the seam**: Plan 9 *semantics* -- per-process namespaces, `bind`,
  union directories -- are **not** adopted. A V8 program must not find itself in
  a world V8 never had, and nothing in V8's userspace asks for one.

Why 9P rather than an ad-hoc protocol: it is specified and stable since 2000,
about thirteen message types, and it is *designed* for the exact problem that
forces a server here -- many clients, one authority, per-client fids. There are
reference implementations to test against (`u9fs`, `go9p`, plan9port), and
`9pfuse`/Mac9P/FSKit mean the **host** can mount the V8 world, which closes the
ingest-and-extract gap that a raw image otherwise leaves open.

Plain **9P2000**. Not `.u`, whose Unix extensions carry things V8's userspace
does not have; not `.L`, which is Linux-shaped.

### The architecture

One switch, several servers, one protocol. The switch is the VFS V10 grew and
V8 was already growing (`proca.c`, `neta.c` sit in V8's `sys/sys/`).

**MEASURED AFTER STEP 1, AND STRONGER THAN THAT SENTENCE CLAIMED: V8 already had
the switch, and it was already dispatching.** `sys/h/conf.h` defines

```c
struct fstypsw {
	int (*t_put)(); struct inode *(*t_get)(); int (*t_free)();
	int (*t_updat)(); int (*t_read)(); int (*t_write)(); int (*t_trunc)();
	int (*t_stat)(); int (*t_nami)(); int (*t_mount)(); int (*t_ioctl)();
};
```

and `sys/dev/conf.c` fills it with **four entries**: the ordinary filesystem
(`rnami`, `smount`), the network filesystem (`na*`, `sys/neta.c`, 710 lines),
**`/proc` (`pr*`, `sys/proca.c`, 716 lines)**, and mpx (`mp*`, `sys/mp.c`).
`iget.c`, `nami.c`, `rdwri.c`, `ioctl.c` and `sys3.c` all dispatch through it.
So V8 was not "growing towards" a VFS -- it had one, four years before Sun's
paper, and Killian's `/proc` was already a client of it.

Two consequences, both narrowing what has to be designed:

- **Step 2 has a shape to answer to** rather than one to invent: eleven
  operations, and the things that will plug in were written against them.
- **Step 3 has less to write than it looked.** `proca.c` already *is* those
  operations.

**CORRECTED IMMEDIATELY, by reading `proca.c` instead of its function names.**
The paragraph above first said step 3 was "an import, not an implementation",
`stream.c`-style. It is not, and the difference is the substrate.
`stream.c` needed nine names. `proca.c`'s operations are written against the
V8 kernel's *internals*: `struct inode` and `ip->i_number`, the u-area
(`u.u_offset`, `u.u_count`, `u.u_error`), `iomove()`, the `proc[]` table. None
of that exists in the shim, and `fstypsw`'s signatures *are* that model --
`t_read(ip)` takes no buffer because the buffer is in `u`.

Measured, classifying all 716 lines by whether they touch VAX virtual memory
(`struct pte`, `prclmap`, `btop`, `P1OFF`, `p_szpt`, `Prbufmap`):

| Half | Functions | Lines | VAX-VM |
|---|---|---|---|
| Filesystem | `prput` `prget` `prfree` `prupdat` `prread` `prwrite` `prtrunc` `prstat` `prnami` `prmount` **`prioctl`** `prlock` `prunlock` `prxdup` | ~500 | 2 refs |
| Process image | `prusrio` `priomove` `prclmap` `prclunmap` `prxread` | ~169 | 20 refs |

So the filesystem half is portable and the process-image half is not -- and the
split falls in a useful place. `prioctl` is 131 lines with **no** VM reference
at all, and it is what answers `PIOCGETPR`; `prread` on `ROOTINO` is the 50-line
directory listing that `ps` does `getdir("/proc")` against. Both of `ps`'s
non-u-area needs are in the portable half.

What `ps` reads *through* the VAX half is the u-area, and there the answer is
not to port `prusrio` but to manufacture the `struct user`, from `proc_pidinfo`
-- `u_comm`, `u_uid`, `u_ruid`, `u_ttyino`, `u_ofile` are all things Darwin will
say without anyone reading another process's memory. That is `libkmemu`'s
pattern exactly, one level down, and it keeps `task_for_pid` and its
entitlements out of the design.

The 9P seam sits *below* `fstypsw`, not in place of it: a `t_read` that speaks
9P to a server is one entry in the table beside a `t_read` that calls `pread`.

```
V8 program -> libv8sys -> 9P -> +-- passthrough server   (today's transparent mode)
                                +-- proc server          (Killian's /proc)
                                +-- v8fs server          (V8's own alloc/iget/nami
                                                          over a raw image)
```

**Raw images, not VHD and not .dmg.** Fixed VHD is only raw plus a 512-byte
footer, so size is not the objection -- the objection is that nothing which
understands VHD understands a V7 filesystem inside it, so it buys wrapper
tooling and no content tooling. Raw buys something real: **SIMH runs V8 and V9
on an emulated VAX and attaches raw `.dsk` images.** A `.dmg` is worse than
either, because macOS would attach it and mount it with a *host* filesystem,
which defeats the purpose. If VHD is ever wanted, `qemu-img convert` is one
lossless command away.

**The acceptance test this makes possible**, and it is the strongest validation
available to this project: build a filesystem with **our** `mkfs` on ARM64,
attach the image to SIMH running **Bell Labs' own V8**, and run **their** `fsck`
on it. The judge is the original system. (Caveat to verify first: SIMH has an
open issue about silently adding a signature to `.dsk` files.)

**Two modes, and the honest name is not "secure".** Transparent mode is today's
behaviour. Image mode serves the jail from a raw V8 image. It is not a security
boundary -- the jail is per-binary rather than per-process-tree, and a V8 binary
can still issue raw macOS syscalls -- so calling it secure would invite someone
to lean on a guarantee it does not provide. What it gives is isolation and
self-containedness: one artefact, and no accidental writes to the Mac.

Two costs, accepted: the test matrix doubles -- which is also the best feature,
since the same program on the same input must agree across two independent
filesystem implementations -- and the 14-character limit becomes *enforced*
rather than emulated, so the build tree cannot live inside an image. Image mode
is a deployment target, not a development environment.

### What this retires

Three documented lies, all currently recorded as losses:

- `dir.c` truncates names to 14 characters and **can alias two entries onto one
  name**.
- ~~`stat.c` folds inode numbers to 16 bits and **they can collide**~~ — closed.
  A 64→16 map cannot be both pure and injective, but it does not have to be
  pure, only *stable within a process*: `v8sys_fold_ino` is now an append-only
  table, measured 6729 distinct values for 6729 entries where the fold gave
  519 sharing 257. `src/libc/gen/PORTING.md`.
- `df` carries this port's one sanctioned source deviation (S7), because there
  is no superblock to read.

And ten programs then written off as "raw VAX disks": `fsck mkfs icheck dcheck
ncheck clri quot dump restor dumpdir`.

**ALL TEN ARE IN, AND NONE OF THEM NEEDED THIS STEP.** Every one takes its
subject as an *argument* -- a filesystem image for the seven checkers, a tape
file for the three dump tools -- so a mount was never the obstacle. What they
needed was step 4a's on-disk widths, one wire format (`struct spcl`), and one
compiler fix. What step 5 still buys them is `df`'s numbers, `mklost+found`,
and `quot`'s own default argument `/dev/usr`.

### Sequence

Ordered so that value lands before risk, and so each step is testable alone.

1. **Streams.** `streamio.c` is 1093 lines of portable multiplexing -- no MMU,
   no disk, no scheduler. Independent of everything below, highest ceiling,
   and forward-compatible (V10 renames it `io/stream.c`). Unlocks the Datakit
   and `netfs` work PLAN S7 currently excludes. (It used to say "and feeds
   blitterm Tier 2" as well; Tier 2 is dropped, and the streams work stands on
   the other two reasons — which is why the sentence is trimmed rather than
   re-argued.)

   **The engine is IN -- `dev/stream.c`, byte-identical to upstream.** This is
   the first Bell Labs *kernel* source in the port; `src/sys/PORTING.md` and
   `shim/kern/NOTES.md` have it, and `tests/streams` began at 43 cases. The claim worth
   repeating is that not one line changed, and it was affordable because the
   footprint is **nine names**: NULL, caddr_t, u_char, u_short, spl6, splx,
   panic, printf, uballoc. A 483-line message-passing engine that barely knows
   what machine it is on -- which is also why streams are the piece of V8 that
   outlived the VAX kernel into System V and SunOS.

   Two decisions generalise to the rest of `sys/`. Machine facts go in
   `shim/kern/h/`, reached because a quoted include tries the includer's
   directory first and `src/sys/h/` is authentic-only -- so **an authentic
   header always wins and ours fill the gaps**, with no flag saying which is
   which. And K&R gets a *dialect flag* rather than an edit: `-std=gnu89` with
   the implicit-int and implicit-declaration diagnostics off is how S1's "do not
   modernise K&R declarations" is actually obeyed.

   **AND THE SYSCALL SIDE IS IN TOO** -- what follows is the survey that
   preceded it, kept because every number in it was checked before anything was
   written, with the outcome at the end of this step.

   **`sys/streamio.c`**, the syscall side -- `stopen`,
   `stread`, `stwrite`, `stioctl`, `I_PUSH`/`I_POP`. **Surveyed rather than
   estimated**, and the survey says not yet; `src/sys/PORTING.md` has it in
   full. The number that made `stream.c` affordable was nine external names.
   `streamio.c` needs **~34**, across **11** authentic headers, four of which
   (`user.h`, `proc.h`, `inode.h`, `file.h`) are the whole process model --
   `struct user` alone is referenced 69 times.

   **`tsleep` is the one that decides it** — the first compile error, not a
   caveat. **SETTLED, and the answer is milder than this said.** It used to
   read "it can only mean run `queuerun` and re-poll, which changes the
   engine's semantics"; measured against the file, that overstates it. Every
   `tsleep` is inside a condition **re-test loop** (`stopen`'s
   `while (sp->flag&STWOPEN)`, `stread`'s `for(;;) ... continue`), so
   sleep/wakeup is advisory and an early return is harmless. All nine `wakeup`s
   live in this same file, and seven of them in `strput` and `stwsrv` — the
   stream head's own `qinit` procedures, registered in `strdata`/`stwdata` and
   therefore reached by `putnext` and `queuerun()`, not by another thread. The
   engine calls neither: `stream.c` has no `tsleep` and no `wakeup` at all.

   So the only producer that is genuinely another process is the **driver at
   the bottom of the stack** — and what sits at the bottom is a question step 2
   already answered for filesystems: the host. The driver end is a host
   descriptor, `tsleep` is `queuerun()` then `poll()` on it with the timeout,
   and `wakeup` is a no-op. **Faithful, not a semantic change** — the kernel's
   `tsleep` waits for the driver to interrupt and this waits for the fd
   standing in for it. `shim/kern/dev/machdep.c` already has the first half in
   `splx()`, and its comment anticipates the second.

   **And what looked like the remainder belongs to a different file.** The case
   with no host fd is a stream between two V8 processes — which in V8 is
   `pipe(2)` *literally*: `sys/pipe.c:16` says "Allocate 2 open inodes, stream
   them, and splice them together", and `:67-70` cross-connect the queues. Not
   a corner case, the commonest IPC there is. But it is **not `streamio.c`'s
   problem**: `pipe.c` is a separate 129-line file, the dependency runs
   *pipe.c → streamio.c* (for `nilinfo`), and `streamio.c` mentions pipes
   exactly once, in a comment. Every stream it opens itself hangs off an
   inode's `i_sptr` with a *device* below — the single-ended case the answer
   above covers. This port meanwhile answers `pipe(2)` with the host's, in
   `v8s_pipe`, and has done all along.

   **So step 1's precondition is met — and so are the four hazards that stood
   behind it.** All four are now settled in `src/sys/PORTING.md`, and three of
   them turned on reading *upstream's* declarations rather than only ours:

   - `streamio.c:713` is an upstream LP64 bug: `sizeof(arg)` where the object
     is `int fmt`. Eight sibling `copyout`s in the same function name the
     object and one names the pointer, so it is a typo, and it becomes a
     recorded deviation at import time.
   - The `short pgrp` was **not** a conflict with V8 at all. `h/proc.h:28-29`
     declares `short p_pid` and `short p_pgrp`, so the field is exactly as wide
     as a VAX process id and loses nothing there. The narrowing is this port's
     own widening of `p_pid` to `int` for `ps`, so the answer is the one
     `daddr_t` already gets: the kernel-side `struct proc` keeps upstream's
     `short` and the shim supplies ids in that range, while the /proc-facing
     header stays `int`.
   - The `char count` wrap needs **128 processes inside one stream**. `stenter`
     has six callers, all of them system-call entry points, none reachable from
     another and none stored in a `qinit` — measured — so one process
     contributes 1 and a per-binary shim never exceeds it.
   - The second `struct user` is forced by exactly three of the ten fields
     `streamio.c` touches — `u_procp`, `u_qsav`, `u_ofile` — all pointer-shaped
     and all frozen at VAX widths by the /proc ABI. Enumerating them found a
     live defect in `procfs.c`'s copy, since fixed.

   **DONE. `streamio.c` IS IN, and the estimate held.** 19 of the 34 external
   names already existed — 11 from the imported engine, 3 macros from the
   authentic `stream.h`, 5 from `shim/kern` — and the 15 left came to four
   small files in `shim/kern/sys/`, named after the ones V8 keeps them in:
   `slp.c` (tsleep, wakeup, the setjmp half of `u_qsav`), `fio.c` (the u-area,
   the proc entry, the file table, the inode edge), `subr.c` (the twelve
   mechanical names), `ioconf.c` (the table `config(8)` generated upstream).
   Eleven headers: **six imported authentic** — `dir.h`, `inode.h`, `ioctl.h`,
   `ttyld.h`, `file.h`, `inline.h` — and **five stand-ins** in `shim/kern/h/`,
   split one at a time by whether the header describes V8 or describes a VAX.
   `tests/streams` went from 43 cases to 111.

   Two deviations, both LP64, both argued from a sibling one function away, and
   both asserted by *diffing against third_party* rather than by hashing —
   `stream.c` can be guarded by its hash because nothing in it changed, and
   `streamio.c` cannot. `:713`'s `sizeof(arg)` on a 4-byte object is the
   recorded one; the second was found by the compiler. **`urcvfile` declares
   only `stq`, so `arg` is an implicit `int`** and its caller passes a
   `caddr_t` — a wild pointer write from `FIORCVFD` on LP64. `v8cc` widens
   undeclared K&R parameters on purpose, and `streamio.c` is compiled by
   *clang*, so the widening this port leans on everywhere else is simply absent
   here. The twin one function up, `usndfile`, declares it correctly.

   **Three things the survey got wrong, all in the harmless direction.**
   `wakeup` was predicted to be a no-op; it is a counter, because that is what
   lets `tsleep` tell "a producer ran" from "nothing will" across its
   `queuerun()`, and a no-op would have thrown the answer away. `nulldev` was
   said to be safe because its callers discard the result — false, all three
   `qopen` sites consume it; the real reason is that `strdata` sits on the
   *head's* queue, which `qopen` never reaches. And an `int` return was said to
   leave x0's top half unspecified — measured, a `w0` write zeroes bits 63:32
   and a forwarded call emits no truncation at all, which gives the first
   driver a sharper rule: a `qopen` must never return a negative int, because
   `-1` becomes `0xffffffff`, neither NULL nor 1.

   **And an `lp64-auditor` pass afterwards found a bug in the new shim code,
   one line below the paragraph arguing against it.** `u_uid`/`u_gid` were a
   bare cast to `short` directly beneath the note explaining why a bare cast is
   wrong for `p_pid`. The magic value there is 0-means-root, and
   `streamio.c:44` lets root bypass a stream's exclusive-use lock. Fixed; and
   it is CLAUDE.md's own warning about correcting one of these — *the fix lands
   on one line and the line beside it keeps the assumption.*

   A genuinely pure stratum existed — `qattach`, `qdetach`, `streadable`,
   `nilopen`, `nilput`, 86 lines, 7.9% — and was unreachable in isolation,
   because a byte-identical import has to compile and link *whole*. Answering
   the per-binary question first and importing once was the right order: it
   meant writing `tsleep` once, from a settled design, rather than twice under
   a build that would not link.
   **STEP 1b: WHICH DRIVER GOES UNDERNEATH, SURVEYED THE SAME WAY.**
   **`ttyld` LANDED — see the closing note at the end of this step.** Nothing
   in the rootfs links `libv8kern.a` -- it is exercised only by
   `tests/streams/`'s three probes, so 1700 lines of authentic kernel are
   unreachable from any shipped binary. Three candidates were costed by
   external-name count, which is the method that made step 1 tractable:

   | | ext. names | missing | headers | verdict |
   |---|---|---|---|---|
   | `dev/spipe.c` (83 lines) | 19 | 2 (`minor()`, `NSP`) | 5, **2 new** | **impossible** |
   | `dev/ttyld.c` (596 lines) | ~~28~~ **15** | ~~3~~ **1 (`max()`)** | 6, **5 already in**, 1 empty | cheap to compile, one number missing |
   | host-fd driver (new, layer 2) | **0** | **0** | **0** | ~~do this first~~ **there is nothing for it to be under** |

   **AND THE THIRD ROW WAS ANSWERING A QUESTION V8 DOES NOT ASK.** The host-fd
   driver was costed as the bottom end for `/dev/tty`. **V8's `/dev/tty` is not
   a stream, not a device, and has no code behind it**: it is a hard link to
   `/dev/fd/3`, and opening anything in `/dev/fd` is `dup(2)`. Four independent
   confirmations, each read in `third_party/` rather than recalled --
   `proto-dev:91` (major 40 minor 3, link count 2), `conf/devices:55` (`device
   40 std`, no driver name), `dev/conf.c:565` (every `cdevsw` slot `nodev`, null
   `streamtab`), and `sys/sys2.c:174`, where `open1()` special-cases it *before*
   the permission check with `getf(minor)` / `ufalloc()` / `u_ofile[i] = fp` --
   the body of `dup(2)`. `man4/fd.4` says the same in prose. V7's `syopen`
   driver is still in the tree at `sys/sys/sys.c` and is dead code: not in
   `conf/files`, and it could not compile, since `u_ttyp` is not in V8's
   `struct user`.

   **This is the third time the plan has been wrong about what a program talks
   to** -- after `ps` (`libproc` vs `/proc`) and `w` (a 1981 Berkeley groveler
   in a 1985 tree). The shape is identical each time: a plausible mechanism
   assumed, and the actual one written down in the source.

   Two smaller corrections in the same block. **`conf/devices:82` is `bf`, not
   `ttyld`; `ttyld` is `:75`.** And the ordering argument -- "`ttyld` has no
   bottom end, so it cannot be exercised until something is under it" -- is
   false *at the open path*: `ttyopen` (`ttyld.c:41-63`) never dereferences
   `q->next`, ignores its `dev` argument, sends nothing downstream, and needs
   only a free slot in its own static `tty[NTTY]`; `qattach` has already spliced
   the module in before `qopen` runs (`streamio.c:809-836`, then `:111`/`:643`).
   So `ttyld` can be pushed onto the probe's existing loopback stream today.
   What genuinely needs a bottom end is *traffic* -- `QFULL` back-pressure,
   `QDELIM` propagation, and `M_IOCACK`/`M_IOCNAK` for its ioctls.

   **What landed instead is `/dev/fd`, as the third filesystem type** --
   `v8fs_fdfs` in `shim/v8sys/vfs.c`, PLAN §8a step 2's switch doing the job it
   was built for. Zero kernel code, zero bss, no `libv8kern.a`, and it is what
   V8 has. `shim/NOTES.md` has the whole account: the `stat`/`fstat`
   asymmetry, the four measured differences from macOS's own `/dev/fd` (the
   shared offset is **not** one of them -- Darwin dups too), the `v8` launcher
   as this world's init (`init.c:379-381` dups the terminal to fd 3, which is
   the only reason fd 3 means anything), and the two gaps it made live:
   `v8s_creat` bypassing the switch entirely, and `v8s_dup`/`v8s_dup2` dropping
   the descriptor's filesystem.

   Among V8's `dev/` stream drivers, `spipe` is the **only** hardware-free one
   with a major number; every other binds real hardware, and the remaining
   hardware-free modules are all line disciplines. So a shipped stream *device*
   here would have to be a layer-2 invention, which is a weaker claim than a
   line discipline exercised over the bottom `tests/streams` already has.

   **AND THE `ttyld` ROW WAS RE-MEASURED TOO, BECAUSE THE SAME SURVEY WROTE IT.**
   Paren-matching its calls and subtracting its own eight definitions gives
   **15** distinct callees, not 28 — the old figure counted variables. Fourteen
   of the fifteen are already in `libv8kern.a`, checked with `nm`; the one
   missing is `max()`, eight lines at `sys/rdwri.c:236`, beside the `min()`
   already in `shim/kern/sys/subr.c`. `partab[]` is data, not a name to
   implement: `sys/partab.c` — note **`sys/`, not `dev/`** — is 51 lines and
   imports whole.

   **Re-verified a third time, and the count survives with one subtraction
   that the figure had already made.** `outconv` looks like a sixteenth callee
   and is not: `ttyld.c:351` defines it. So the honest tally is 16 names
   called, 8 of them its own, 15 external, 14 present. Two numbers in this
   section were nonetheless wrong and are corrected above and below — the
   `max()` line, which this paragraph gave as `:235` and the paragraph four
   below gives as `:236` (236 is right), and the header count.

   **The sixth header is not a header.** `ttyld.c:6` includes `"tty.h"`, and
   `h/tty.h` is **zero bytes** — the only zero-length header in `h/`.
   `conf/makefile:61-62` says why:

   ```make
   ../h/tty.h: /usr/include/sgtty.h ../h/ioctl.h
	   touch ../h/tty.h
   ```

   It is a make timestamp node standing for "sgtty.h and ioctl.h are current",
   deliberately empty. The include is a no-op by design, not a missing file —
   the mirror of `sys/sys.c`, where a file that *is* present turned out not to
   be built. Read the makefile before calling a file missing or present.

   **What genuinely is not in the tree is `NTTY`.** It occurs exactly twice,
   both inside `ttyld.c` (`:12` and `:50`), and is defined nowhere in
   `usr/sys`; `nttyld.c` beside it uses `NNTTY` under a `#if NNTTY > 0` guard,
   which is the 4BSD config-generated pattern. `conf/config_how` confirms it —
   `config` writes "zillions of header files" into `/usr/sys/<sysname>/` from a
   machine description that **is not shipped**, and `conf/config` is a VAX
   `a.out` binary rather than source, so it cannot be regenerated either. So an
   `NTTY` here is a **layer-2 decision and must be spelled as one**, the way
   `libkmemu`'s `u_ssize`/`NSTACK` is. The `64` this section used to give has no
   source: `proto-dev` has 8 hardware ttys and 64 `spipe` `pt` nodes, which
   makes it a plausible guess and nothing more. Re-measured wider than last
   time: there is no `#define NTTY` anywhere in **`third_party/` at all**, not
   merely in `usr/sys`, and the two occurrences are both array bounds. The
   other three `grep` hits are different symbols — `NNTTY` (`nttyld.c`'s own
   count) and `NTTYDISC` (`chtty.c:62`, a line-discipline *number*) — so a
   pattern of `NTTY` without a word boundary overcounts this three-fold.

   **AND THE DECISION HAS A HOME THE PORT ALREADY SPECIFIES, which is the part
   this survey never said.** `ttyld.c:6` is `#include "tty.h"` — a *quoted*
   include, so it tries the includer's directory first and then the `-I` path.
   That is exactly the mechanism `"../h/param.h"` uses to reach the authentic
   header while `"../h/param.h"` from elsewhere falls through to
   `-Ishim/kern/dev`. So `shim/kern/dev/tty.h` is where this port's `NTTY`
   belongs: machine facts in `shim/kern/`, never in `src/sys/`, and the
   generated-header slot filled by the layer that is allowed to decide. No
   edit to `ttyld.c` is needed to carry it, which is what keeps the import
   byte-identical.

   **`spipe` is structurally impossible, and not by a small margin.** It is the
   64 `/dev/pt/pt00`-`pt63` nodes -- odd minor master, even slave -- and its two
   ends exchange a `struct block *` **by direct function call**
   (`spipe.c:73`). Every object involved is process bss: `spipes[256]`,
   `queue[NQUEUE]` (`stream.c:16`), `qfreelist[4]`, `streams[NSTREAM]`
   (`streamio.c:20`). A per-binary shim gives each process its own copy of all
   four, so it can only wire a process to itself; `spopen` refuses the mismatch
   by design at `spipe.c:36-37`. `fork` does not rescue it (copy-on-write
   diverges) and `exec` clears `fdtyp[]` (`vfs.c:136-143`).

   **`ttyld.c` is astonishingly cheap and is the reason to do the host-fd
   driver first.** ~~Three~~ **Five** of its six headers are already in, which
   is what the table above says and what this sentence contradicted: `param.h`
   and `conf.h` in `shim/kern/h/`, `stream.h`, `ioctl.h` and `ttyld.h` in
   `src/sys/h/` -- and `ttyld.h` came in with `streamio.c` and is **unused
   today**. All twelve stream primitives it calls exist, and the three missing
   names are an 8-line `max()` from `rdwri.c:236` beside the `min()` already in
   `subr.c`, a 51-line pure-data `sys/partab.c` that imports whole, and one
   `#define NTTY`. `conf/devices:75`
   -- `standard line-discipline 0	tty	tty	info` -- makes it
   **line-discipline 0, not a device**, and `init.c:377` is what pushes it:
   `ioctl(0, FIOPUSHLD, &tty_ld)` on the terminal, immediately before the three
   dups. (This line cited `:82`, which is `bf`/`bufld`, and concluded from the
   missing bottom end that `ttyld` "cannot be exercised until something is under
   it". Both corrected above: the open path needs nothing below, only the
   traffic does.)

   **DONE, AND THE SURVEY'S COST ESTIMATE HELD EXACTLY.** `dev/ttyld.c` and
   `sys/partab.c` are imported **byte-identical** and hash-guarded in
   `tests/streams`; `max()` was the one missing name and sits in
   `shim/kern/sys/subr.c` beside `min()`; `NTTY` is 128 in
   `shim/kern/dev/tty.h`, **derived** as `NSTREAM` because a slot is one
   discipline attached to a stream and a process cannot hold more streams than
   that. The quoted-include fall-through delivered the generated header with no
   edit to Bell Labs' source, which is what keeps the hash guard available. The
   archive's externals are still exactly `_longjmp _memcpy _setjmp`, and its
   zero-initialised storage went 96332 -> 98124 bytes, +1792 for `tty[128]`.

   `tests/streams/ttyprobe.c` is a third probe beside the engine and syscall
   ones, and it drives the discipline through `qinfo->qopen` rather than by
   name. 27 cases; the load-bearing one is exhaustion, because that is where
   the `qopen`-must-not-return-negative rule lives -- 128 fit, the 129th is
   refused, and the refusal is 0. Mutating `return(0)` to `return(-1)` turns it
   red, and mutating `NTTY` to 64 turns four others red.

   **STEP 1c IS DONE TOO, AND THE PARAGRAPH BELOW SAYING IT IS "the next
   increment" IS KEPT ONLY BECAUSE IT NAMES THE FIVE FUNCTIONS.** The same
   probe now carries a driver (83 lines of code, measured) and builds the real
   thing the way
   `init.c:368-382` does -- `stopen` the driver, `v8k_stconf` the discipline,
   `FIOPUSHLD` to push it between -- so `streamio.c`'s stream head sits on top,
   `ttyld.c` in the middle, and only the bottom layer is ours. **80 new cases,
   streams 140 -> 236.** All five functions run, plus `outconv`, and the four
   flag-gated arms with them: `LCASE` through `maptab[]` (a Model 33 with no
   lower case -- `\a` is `A`, `\(` is `{`), escape handling, `TANDEM`
   back-pressure at upstream's own `(600+60)/2`, and `outconv`'s padding
   delays for the tty 37, vt05, tn 300 and ti 700.

   Three results worth carrying:

   - **A driver, not a module, and one line says why.** `ttyldin` sends data
     UP through `q->next` and flow control DOWN through `WR(q)->next` in the
     same loop. A module stacked above sees the first and never the second.
     Mutating `putctl(wrq->next, M_STOP)` to `putctl(q->next, M_STOP)` turns
     exactly one case red, and that case is unreachable from above.
   - **`ttysig` ends in a real `kill(2)`.** DEL -> `ttyldin` -> `ttysig` ->
     `M_SIGNAL` -> `streamio.c:379` `gsignal` -> the shim's `psignal` ->
     `rawsys2(SYS_kill)`. The assertion is that a handler ran.
   - **Two ioctl arms that `stioctl`'s return value cannot tell apart.**
     `TIOCSETP` passes the block down and the driver's ack is what wakes the
     sleeper; `TIOCSETC` is `qreply` at the discipline and the device never
     sees it. Both return 0. Only the driver can distinguish them, and the
     `fromdev`-1 arm had never been taken by anything in this port.

   **AND THE AUDITOR FOUND A LIVE BUG IN THE DRIVER, WITH V8's FIX IN TWO
   UPSTREAM FILES.** `stioctl` builds every `M_IOCTL` 20 bytes long whatever
   the command, `ttldioc`'s `TIOCSETP` arm does not touch `wptr`, and
   `streamio.c:793-798` copies `wptr - rptr` back -- so an ack passed through
   unchanged writes **16 bytes into a 6-byte `struct sgttyb`**.
   `dev/cons.c:56-58` and `dev/dz.c:229` each spend one line resetting `wptr`
   for the SET commands and falling through to `TIOCGETP`, which must not. No
   behavioural test could see it: the ten out-of-bounds bytes are the ten
   `copyin` read from that address moments earlier, so the write round-trips
   and memory ends up correct. The guard is a PAGE -- `PROT_READ`, so the
   authentic over-READ still succeeds and only the write can fault -- and it
   measures 0 with the line and SIGBUS without. The 8-into-6 that `TIOCGETP`
   still does IS upstream's; reproduced, not repaired.

   And **the tab does not expand by default** -- `outconv`'s loop is behind
   `(t_flags&TBDELAY)==XTABS` and `ttyopen` sets `ECHO|CRMOD` only. The first
   draft of that case expected `a       b` from reading the loop and not its
   guard one line above it. Both cases are now in: the literal tab a default
   terminal gets, and the seven spaces once the flag is set.

   **Writing `max()` found `min()` misdeclared in two places**, both saying
   upstream had "no declared return type"; `rdwri.c:249` is the word `unsigned`
   on its own line. Nothing observable changed, which is why it survived --
   and it would have been copied straight into `max`. Adding the sibling is
   what forced the declaration to be read.

   ~~**What is NOT exercised is everything below the open**: `ttyldin`,
   `ttyinsrv`, `ttyosrv`, `ttysig` and `ttldioc` compile and link and nothing
   drives them, because driving them needs something under the discipline to
   send to. That is the next increment, and it is a driver rather than a
   module.~~ **Done -- see the step 1c block above.** The prediction held
   exactly: it was a driver and not a module, and the reason is `ttyldin`
   reaching both ways in one loop. `src/sys/PORTING.md` has the whole account,
   including the four flag-gated arms, which are now driven too.

   Three things the survey settled that were not asked:

   - **`qopen`'s rule is harsher than CLAUDE.md records.** Beside
     `stopen:124/131`, the `FIOPUSHLD` path at `streamio.c:645-650` is
     `else if (nip!=1) panic("pushld qopen returns inode", nip)` -- so anything
     but 0 or 1 panics, not merely misbehaves. Verified in the file. Both
     authentic candidates return literal 0/1 only.
   - **Half of V8's `/dev` was streams.** `third_party/.../v8/proto-dev` is a
     recursive listing of the running research machine: **184 of 378 nodes
     (49%)** belong to stream drivers, and the largest single block is
     `spipe`'s 64.
   - **`stty` can be ported today, with no driver at all, and stays faithful.**
     `FIOLOOKLD` returns -1, so `ldisc` matches neither `tty_ld` nor `ntty_ld`,
     every new-tty ioctl is skipped, and `swdisc()` prints `stty: can't switch
     line disciplines` -- explicit and honest, the same shape as `load` and `w`
     under rung 5. `getmodes()`'s return is discarded at `stty.c:182` and the
     one warning that would have said so is **commented out** at `:183-186`.
     It needs `libc/gen/linedis.c` (20 lines, pure data), which is not ported.
     The five ioctls it does use are already translated.

   And the customers exist already: `ps.c:25` does `getdir("/dev/pt", devlist)`,
   `w.c:563` has a `/dev/pt/pt??` fixup, and `ttyname.c:36` searches `/dev/pt/`
   -- all three against a `rootfs/dev/pt` that is an **empty directory**.

2. **The 9P switch itself**, with exactly one server behind it: **passthrough**,
   reproducing today's behaviour byte for byte. Nothing user-visible changes;
   the whole point is that the suites stay green while the floor is replaced.
   This is the step that must not be skipped, because it is the only one where
   a regression is unambiguous.

   **The switch is in** -- `shim/v8sys/vfs.h` and `vfs.c`, with `syscall.c`
   dispatching. `struct v8fstyp` answers to `struct fstypsw`, entry by entry,
   and the correspondence is written beside each one. **The mount table is
   `v8dirs[]` generalised, not a new table beside it**: two prefix lists that
   have to agree by hand is the standing invitation `kmem.c`'s one-table rule
   exists to refuse, and a `/proc` entry is now one row.

   Where it departs from `fstypsw` and why: V8's operations take a
   `struct inode *` and find their buffer in the u-area, so `t_read(ip)` has no
   length argument. This shim has neither, and building that substrate before
   there is a second filesystem to justify it would be inventing a customer --
   so the operations are descriptor-shaped. `t_ioctl` is deliberately **absent**
   rather than present-and-null: it arrives with `/proc`, which is the type that
   needs it, because a slot no type ever fills reads as a seam and guards
   nothing.

   **And the step earned its keep immediately, twice.** Moving the mount table
   out took the `V8ROOT_DEFAULT` fallback with it -- invisible to the whole
   build, which passes `-D`, and caught only by `tests/v8sys`, which compiles
   the shim its own way. Then `rmdir` stopped removing anything: V7 `namei()`'s
   rule that **the empty path is the current directory** lived in `vpath()`, and
   `t_path` calls the resolver directly, so the switch bypassed it. It is a
   namespace rule and now lives in the resolver where every type gets it.
   `tests/waveb` found it on the first run -- which is precisely the argument
   for doing this step with one server behind it rather than alongside `/proc`.

   Not yet dispatched, and named rather than left implicit: `chmod`, `chown`,
   `link`, `unlink`, `mkdir`, `access` and the rest still call the passthrough
   path directly. They resolve through the same mount table, so the namespace is
   already unified; what they lack is a per-type entry point, and each will get
   one when a type needs to answer differently. `/proc` needs none of them --
   `prtrunc` is a no-op upstream too.
3. **`/proc` as the second server.** Authentic V8, small, and it is what makes
   `ps` and `w` honest -- a server that records `fork`/`exec` *knows* the V8
   subtree instead of guessing it from `libproc`. Note `p_wchan` stays
   unanswerable either way; the sentinel rule still applies.

   **STARTED: the directory works, and it is manufactured rather than
   imported.** `shim/libkmemu/procfs.c` is the second filesystem type in the
   switch. `ls /proc` lists every live process -- 632 of them, matching
   `ps ax` -- as 256-byte V7 records with `d_ino = pid + 64` and five-digit
   zero-padded names, `.` and `..` both at ROOTINO, and a fixed
   `(nproc + 2)`-record directory with holes. Every one of those conventions is
   `proca.c`'s and is cited to its line in the source.

   **The decision not to import `proca.c` was made by reading it.** Its
   operations are written against the V8 kernel's internals -- `t_read(ip)`
   takes an inode and no length, because the length is `u.u_count` -- so
   standing it up needs `struct inode`, the u-area, `iomove`, `proc[]`,
   `pfind`, `iget`, `namei`, `open1`, `tsleep`/`wakeup`, `psignal`, `setrq`:
   about twenty-five substrate functions, which is most of the kernel.
   `stream.c` needed nine names; this needs a kernel. So the conventions are
   V8's and the answers come from `proc_listpids`, which is `libkmemu`'s
   existing bargain one level up -- `who` reads an `/etc/utmp` nothing else
   writes; `ps` reads a `/proc` nothing else mounts.

   Two things the port had to decide for itself, both recorded in the source.
   The table is **1024 slots** because V8's `NPROC (20 + 8 * MAXUSERS)` depends
   on a config file `mkconf` generated that is not in the vendored tree for any
   machine -- and an overflow is *reported*, not dropped, because a process
   lister that silently omits processes is the one thing it must not be. And
   `d_ino` is 16 bits while a macOS pid is not, so the sum wraps; harmless,
   because the *name* carries the pid, except for pid 65472, which wraps to
   exactly 0 and would vanish from every reader. Folded to 1, same defence as
   `v8sys_fold_ino`.

   **`PIOCGETPR` is done, and it brought `t_ioctl` with it.** The ioctl slot
   was deliberately left out of `struct v8fstyp` until a type needed it; that
   type arrived, so the slot did too. The sgtty/termios translation did not
   move -- it *became* the passthrough type's implementation, which is what it
   always was in fact, and only the dispatch is new. `v8s_ioctl` now routes on
   the descriptor's filesystem exactly as V8's `sys/ioctl.c` routes on the
   inode's. The assertion that says this is real is a pair: `PIOCGETPR` on an
   ordinary file is `ENOTTY`, and `TIOCGETP` on a `/proc` descriptor is
   `EINVAL` -- **the same command number taking two different paths**.

   **The layout, measured from the V8 side so it does not have to be derived
   again.** `PIOCGETPR` copies a `struct proc` verbatim, so its shape *is* the
   `/proc` ABI and both ends must agree exactly -- the same hazard `struct utmp`
   and `struct exec` already have, and the same answer: spell it in the shim,
   assert it from the V8 side. Two guards, because a `_Static_assert` sees only
   one compiler.

   | | |
   |---|---|
   | `sizeof(struct proc)` | **208** -- 200 upstream, plus the pid widening below |
   | `p_stat` 27, `p_time` 28, `p_nice` 29 | chars |
   | `p_flag` 56 | int |
   | `p_uid` 60, `p_pgrp` 64, `p_pid` 68, `p_ppid` 72 | ints (**was shorts**) |
   | `p_dsize` 88, `p_ssize` 96, `p_rssize` 104 | `size_t` |
   | `p_swaddr` 128, `p_wchan` 136, `p_textp` 144 | |
   | `p_clktim` 152 | `u_short` |
   | `p_pctcpu` 180 | float |
   | `sizeof(struct user)` | **4016** |
   | `UPAGES` 10, `NBPG` 512 | so `UBASE` = `0x80000000 - 5120` = **0x7fffec00** |

   `UBASE` fits in 31 bits, so `ps`'s `Sread(fd, UBASE, up)` is an ordinary
   `lseek` and needs no special handling.

   **`p_pid` had to be widened, and how it hid is the interesting part.** V7
   wrapped `mpid` at 30000, so `short` was the whole pid range; macOS runs to
   99998, and the truncation is *signed* -- pid 44145 came back as **-21391**.
   `ps` would have printed a negative pid. It hid because a freshly booted host
   has low pids: every check passes until the host's counter crosses 32767,
   then the same code starts lying. Found by mutation-testing something else
   entirely, when a mutation produced two extra failures it had no business
   producing. `src/include/PORTING.md`, which this port's include tree now has;
   `tests/kmemu` asserts the *field width* beside the runtime value, because
   the width is true at every pid and the comparison only at high ones.

   **Two host facts that would each have produced plausible wrong output.**
   `pti_total_user` is in **mach ticks, not nanoseconds** -- `<sys/proc_info.h>`
   says only "total time", and on Intel the timebase is 1/1 so the two coincide
   exactly. On Apple Silicon it is 125/3 and a `%cpu` computed as nanoseconds is
   wrong by 41.67x while staying in range. Measured against
   `CLOCK_PROCESS_CPUTIME_ID`; the rate comes from `hw.tbfrequency`, which is
   `sysctl` and already sanctioned, rather than from `mach_timebase_info()`,
   which would be a second way to ask. And the **stat codes disagree on every
   value but one**: macOS `SIDL/SRUN/SSLEEP/SSTOP/SZOMB` are 1..5 and V8's are
   4/3/1/6/5, so a straight copy stays in range and prints the wrong letter for
   every process. Both are guarded by tests that fail on the magnitude, not just
   the presence, of an answer.

   **The u-area is done too, and it is a region of the same file rather than a
   second format.** `ps` reads it with `Sread(fd, UBASE, up)` -- a seek to a
   *virtual address*, because `/proc/<pid>` as a byte stream is the process's
   address space and `proca.c` serves it through `prusrio`. So `pr_read` hands
   out `struct user` when the offset lands in `[UBASE, UBASE+4016)` and reports
   end of file everywhere else. `struct user` is declared by explicit padding
   rather than spelled out: 4016 bytes containing the VAX process control
   block, four disk maps and the kernel stack, to reach the twelve fields `ps`
   reads. The pads are checked by `_Static_assert`, so one of the wrong length
   moves the next field and the build fails.

   Two things there are decisions rather than measurements, and are written as
   such. **`u_ssize` is set to `NSTACK`'s worth on purpose**: `getargs` reads
   the process's stack image to recover `argv`, this port has no stack image,
   so the read has to *fail* and send `getargs` to its own documented
   `"(u_comm)"` fallback -- which is exactly what V8 prints for a swapped-out
   process. Zero would not do it: `ctob(0)` is a zero-length read, which
   *succeeds*, and `getargs` then scans backwards past the start of its own
   buffer. And **`u_ttyino` is left zero, which is a `/dev` question rather
   than a `/proc` one**: `gettty()` looks the number up in the directory
   records of `/dev`, `/dev/dk` and `/dev/pt`, and inside the jail `/dev` holds
   exactly one entry (`kmem`), so `ps` prints `?` whatever the field says.
   Filling it is a stat of `/dev/ttys<minor>` folded through `v8sys_fold_ino`
   -- `e_tdev`'s minor does map to the name, measured -- and buys nothing until
   the jail's `/dev` carries tty nodes.

   The file's *size* is upstream's rather than invented:
   `ptob(p_tsize+p_dsize+p_ssize+UPAGES)` (proca.c:88), the process image plus
   the u-area.

   **`ps` runs.** Bell Labs' 1985 process lister, ten objects from upstream's
   own OFILES, listing every process on a Mac through the `/proc` above --
   **two source changes**, both width fixes, both recorded in
   `src/cmd/ps/PORTING.md`. What it actually took was elsewhere:

   - **A COMPILER BUG, and it is the find of this step.** `printp` calls
     `sprintf` with nine arguments; AAPCS64 puts the ninth at `[sp, #0]`; and
     `arm64_endfunction()` was pushing the callee-saved registers at the bottom
     of the frame, so the last one saved *was* `[sp, #0]`. `printp` overwrote
     the register it had saved for `main` and handed it back corrupted, so
     `main`'s `dp` came back pointing into `cmdline` and `ps` walked off its
     `/proc` array. The frame now has three regions -- locals, saves, call area
     -- and CLAUDE.md carries the general lesson. This said "a sweep found
     exactly four functions in the tree with a >8-argument call", and offered
     that as why 156 Wave A programs plus Wave B and C never saw it.
     **Re-measured: at least 26 (file, function) pairs make a call with nine
     or more arguments**, Wave C full of them. The rare half was never the
     argument count -- it is a function that uses REGISTER VARIABLES *and*
     makes the wide call. Count the shape that gates the bug, and say which
     half you counted.
   - **`/dev/dk`, `/dev/pt`, `/dev/drum`,** which `ps` `getdir`s and opens
     before touching `/proc` at all, calling `error()` on any failure. Empty,
     and the emptiness is the true answer.
   - **The pid widening**, without which every pid above 32767 printed
     negative.

   The privilege boundary is per *field* and reported rather than papered over:
   `PROC_PIDT_SHORTBSDINFO` answers for all 614 processes, and the task info
   carrying memory and cpu is denied for the 216 this user does not own, so
   those columns read zero. The first version asked `PROC_PIDTASKALLINFO` for
   everything and returned ENOENT when it failed -- which made `ps` print
   "/proc ioctl error" 215 times and list two thirds of the system. **A process
   that exists must not answer ENOENT**; that is the shim claiming it is gone.

   Also required before `ps` will start at all, and unrelated to `/proc`: it
   `getdir`s `/dev`, `/dev/dk` and `/dev/pt` and opens `/dev/drum`, calling
   `error()` on any failure (`ps.c:21-28`). The last two directories and the
   drum do not exist here.

   **`sys/proca.c` is 716 lines of authentic V8 already written to the eleven
   operations**, and about 500 of them are portable -- see the table above.
   `prioctl` answers `PIOCGETPR` in 131 VM-free lines and `prread` lists the
   directory in 50. What it needs that the shim has not got is the *substrate*:
   `struct inode`, the u-area, `iomove`, `proc[]`. That is the real cost of this
   step, and it is shared with step 5, which wants `iget.c` anyway.

   **Upgraded from "makes them honest" to "is the only way to have them at
   all", by measurement** -- see S7. V8's `ps` does not grovel `/dev/kmem`; it
   `getdir`s `/proc`, opens `/proc/<pid>`, and asks `PIOCGETPR` for the
   `struct proc` and the u-area at virtual address `UBASE`. It is Bell Labs'
   own code written against Bell Labs' own filesystem. So the surface this step
   must present is fixed by upstream rather than chosen: a directory of pids,
   a file per pid whose contents are that process's address space, and the
   `pioctl.h` ioctl set. `PIOCGETPR` alone answers `ps`; the debugger half
   (`PIOCSTOP`/`PIOCWSTOP`/`PIOCRUN`/`PIOCSMASK`) is what `pi`/`adb` would want
   later and is not needed to close this step.
4. **`mkfs` and the raw image. DONE — mkfs runs and the image is authentic.**
   V8's `mkfs`, free-list/1024 format only. `$V8ROOT/etc/mkfs image 2000` gives
   `s_isize` 82, `s_fsize` 2000, `s_tinode` 1278, `s_tfree` 1917, an
   *m*-interleaved free list starting 497/494/491, and a root inode 2 of mode
   `040777` whose single data block holds `.` at offset 0 and `..` at offset 16.
   `tests/mkfs` is 46 cases and `src/cmd/mkfs.PORTING.md` is the record.

   **THE FORMATS WERE ALL BROKEN BEFORE THE PROGRAM WAS EVEN IMPORTED, and that
   is the finding of this step.** Measured against the port's own headers:
   `dinode` 80 where the VAX gives 64, `filsys` 7960 where it gives 4096, `fblk`
   1432 for 716, `NINDIR(0)` 128 for 256. Every other struct in this port has
   two ends and **both are ours** — v8cc reads the header, clang re-spells it in
   the shim — so a widening is safe if the two agree. A disk image has an end
   that is a VAX, and it cannot be asked to agree with anything.

   V8's own compiler settles the width in one line: `# define NOLONG`, commented
   "map longs to ints", at `cmd/ccom/vax/macdefs.h:19`. So `long` was 32 bits
   there and `daddr_t`, `time_t` and `off_t` all silently doubled here.
   `daddr_t` is narrowed globally — nothing hands one to macOS — and the other
   two per field, in the two headers that describe disk records. **The tree had
   already contradicted itself about this**: `param.h` hardcodes `NMASK(0) 0377`
   and `INOPB(0) 16`, asserting NINDIR 256 and `sizeof(dinode)` 64, one line from
   the `NINDIR` that computed 128. `src/include/PORTING.md`.

   Three further things it cost, each recorded where it belongs:

   - **A bug class this port had not met.** `gmode()` is `return((&m0)[i])` —
     the address of a K&R parameter, indexed forward through the next three.
     Exact on a VAX at four bytes a slot, wrong at v8cc's eight, and not fixable
     in the compiler. It blocked *every* run: `'d'` is index 3, so the root
     inode came out with file type 0. Swept; a singleton in the tree.
   - **Two of this port's own patches disagreeing.** `libc/gen/ltol3.c` strode
     eight bytes because `daddr_t` had been eight; narrowing the type without
     the stride decimated every block list and read 35 bytes past the end of a
     stack `struct inode`. `l3tol` has no caller here at all, so `tests/mkfs`
     round-trips the pair.
   - **`DIRSIZ` means two different things now.** 254 for host directories, 14
     for what mkfs writes, and `param.h` needed the `#ifndef` its own comment
     already claimed before `-DDIRSIZ=14` could take. §8a step 5 inherits the
     genuine conflict: a jailed program reading a mounted image gets 16-byte
     records and a passthrough directory gives it 256-byte ones.

   **`df`'s rung 5 did NOT close here, and this file predicted it would.** The
   prediction was reasoned rather than measured: it named the missing ingredient
   — a real superblock — and assumed nothing else was in the path. Two things
   are, and only one was foreseen. The port's change to `df.c` is a
   *replacement* rather than a supplement, so the `open`, `fstat` and `bread` are
   gone and this `df` would not look at a perfect image; and an image in a file
   is not a filesystem behind `/dev/<spec>`, which needs either the image named
   in the jail's `/dev` or the mount of **step 5**. What step 4 changed is real
   but smaller than claimed: from *blocked on data that has to be invented* to
   *contained*. `src/cmd/df/PORTING.md` has it, prediction left visible.

   **`icheck` and `dcheck` are the confirmation instead, and they are in.** They
   take the image as an argument, so they need no mount; they read every inode
   and every directory rather than one superblock; and they are 1985 code that
   knows nothing about `mkfs`, which is the property that matters — everything
   else in `tests/mkfs` asks whether the image matches *this port's* idea of a
   V8 filesystem, and the idea and the bytes come from the same place.

   ```
   files      2 (r=1,d=1,b=0,c=0,l=0)
   used       1 (i=0,ii=0,iii=0,d=1)
   free    1917
   missing    0
   ```

   `free` is walked out of the free list block by block; `s_tfree` is a counter
   `mkfs` kept while writing. Two independent computations, and
   `used + free + s_isize` is exactly `s_fsize` with nothing missing. They also
   gave `l3tol` its first caller in this port — `ltol3` writes those 3-byte
   addresses and nothing had ever read one back. **One patched line between the
   two programs**, and it was the third instance of narrowing a type at the seam
   and meeting something that already encoded the old width; it is the only one
   of the three the compiler diagnosed, because the other two do their
   arithmetic through a `char *`. `src/cmd/icheck.PORTING.md`.

   Three corruption cases are in the suite, because a checker that approves of a
   good image proves very little: an orphaned block reads as `missing 1`, an
   out-of-range address is named with its inode, and a wrong link count is seen
   by `dcheck` while `icheck` correctly stays silent.

   `-DDIRSIZ=14` became a property of the **group** rather than a flag on
   `mkfs`, and `dcheck` is why: it parses the records `mkfs` writes. A forgotten
   flag there is worse than in `mkfs` — it would report a healthy filesystem as
   healthy, and only start lying once a directory held more than one entry per
   256 bytes.

   `clri` is in too, and it needed **no change at all** — 82 lines, and everything
   it computes is right the moment the on-disk headers are. It earns its case by
   splitting one act between the two checkers: it zeroes an inode and leaves the
   directory entry naming it, so `icheck` reports the orphaned block and `dcheck`
   reports the dangling entry, and neither could report the other's half.

   **`fsck` IS IN, AND IT REPAIRS.** Berkeley 4.13, 1925 lines, all five phases,
   and each corruption above handed to it with **the other program** asked
   whether the repair happened: the orphaned block goes back in the free list
   (icheck: `missing 0`, 1917 -> 1918), the link count is adjusted (dcheck
   silent), the out-of-range address is named with the same inode, and clri's
   two halves -- which no single checker could describe -- are both fixed in one
   pass, after which a second pass reports nothing. `tests/mkfs` is 126 cases;
   `src/cmd/fsck.PORTING.md` is the record.

   **ONE COMPILE ERROR IN 1925 LINES, and that is the measurement worth keeping
   about step 4a.** `MAXDATA` is inside `#ifdef pdp11` / `#ifdef vax` and there
   is no arm for this machine. Everything else -- seven union arms over `dinode`,
   `filsys`, `fblk`, `direct` and a `daddr_t` indirect block, all of which must
   come to exactly 4096 -- compiled unchanged, because the formats were fixed in
   the *headers* rather than in the programs. `MAXDATA` is a ceiling ADDRESS
   (`MAXDATA - sbrk(0)`), which is a size only where the data segment starts
   near zero; the arm added names the arena and derives the ceiling, keeping the
   VAX's 400 KB on purpose so the authentic scratch-file path stays reachable.

   **The other finding is one of this port's own, and it is a hang.** §8a step 4a
   narrowed `time_t` per field in the disk records; `icheck` met the *write*
   direction of that seam and swept `grep -rn 'time(&'`. fsck brought a second
   write **and the read direction**, which that pattern cannot match:
   `ctime(&dp->di_mtime)` dereferences eight bytes, takes `di_ctime` as the high
   half, and `gmtime()` counts towards the year 2.3e11 one year at a time -- a
   live lock with an empty stdout, in `pinode()`, which runs only on a damaged
   filesystem. So every clean-image case passed throughout. Also fixed:
   `scrfile[80]`, filled by an unbounded copy from `-t`, which with a 2000-char
   argument made fsck **modify a filesystem that was well**.

   `-DDIRSIZ=14` changed kind here too. For the plain readers a wrong DIRSIZ is a
   wrong answer; `pass2()` copies DIRSIZ bytes per component into
   `pathname[200]`, so at the host's 254 a single component overruns it. The
   Makefile's `$(IMGBIN)` group is now a memory-safety property as well.

   **`mklost+found` is what is left, and it is genuinely blocked rather than
   deferred.** Every reconnect fsck attempts prints `SORRY. NO lost+found
   DIRECTORY` and falls through to `CLEAR?`. Upstream's `mklost+found` is a
   20-line shell script that pre-creates 256 slots so fsck can reconnect an
   orphan without extending a directory -- it needs a *mounted* filesystem, so it
   waits for step 5. Run against a passthrough directory it would prove the
   shell works and nothing about the filesystem. `tests/mkfs` asserts the `SORRY`
   line, so the case goes red the day step 5 lands.

   **4e: `ncheck` and `quot`, which take the ten-program list to seven.** Both
   read the image as an argument, so neither waited for step 5, and the reason
   to have them is that each computes something the other five do not.
   `icheck` walks `di_addr[]`; `quot` computes `ceil(di_size/BSIZE)` -- different
   *fields* of the same inodes, so the two can be made to disagree, and the
   disagreement is exactly the metadata:

   ```
   quot's block total + icheck's indirect count == icheck's `used'
   quot's file total                            == icheck's `files'
   ```

   Section 6's indirect-block image is where that stops being a tautology: 21
   blocks of file against 22 allocated. `ncheck`'s contribution is different in
   kind -- every checker here reports in inode *numbers*, and `ncheck` is the
   only program that turns one into a path. So the corruption cases now end by
   handing icheck's own output to `ncheck -i`. The manual's own composition
   runs verbatim too: `ncheck fs | sort +0n | quot -n fs`, three V8 programs.

   **THE TWO BUGS WERE BOTH "V8 ASSUMES ADDRESS 0 IS READABLE", AND ONE WAS ON
   THE DEFAULT COMMAND LINE.** `quot`'s `qcmp` reaches `strcmp(0,0)` because
   2046 of its 2048 `du[]` entries have no passwd name and `qsort` compares
   those against each other -- so plain `quot image` SIGSEGV'd before printing a
   line. `ncheck -i 5` faulted in `atol` on the vector's NULL terminator. The
   VAX had its text segment at address 0 and both quietly worked; this is
   `refer5.c`'s class, and finding two more of it in one 600-line pair says the
   sweep for it is not done. Fixed with the empty string and a null test
   respectively -- both chosen to reproduce the VAX's *answer* rather than
   merely to remove the fault, since `quot`'s ordering is visible in its output.

   **And a third bug that is OURS.** `%.Ns` in `src/libc/stdio/doprnt.c` read
   `s[prec]`: the loop condition ran before the precision test. That file is
   this port's C rewrite of `doprnt.S`, and `%.Ns` exists precisely for a
   fixed-width field that need not be terminated -- `ncheck` prints `d_name`
   that way, so the byte read is the next entry's `d_ino`, and at the end of a
   mapped page it is a fault. Nothing in the tree had reached it before, which
   is why 32 `printf` cases had not.

   `ncheck` and `quot` are the group's two extremes on `-DDIRSIZ=14` and having
   both is what makes membership mean something. Built at 254, `ncheck` reads a
   *correct* image and prints **nothing at all, exit 0**. `quot`'s object is
   **byte-identical** either way -- and that measured no-op is why **`quot` is
   the first image tool to close rung 5**: its own makefile passes no `-D`, so
   Bell Labs' build description produces the same program, and `tests/jail`
   hands the rung-5 binary and the installed one the same image and requires the
   same answer. The corollary is a trap, and it now has a structural guard:
   `ncheck` must never join `$(V8BIN)`, because that is what `$(SRCTREE)` stages
   for `Admin/Mk` and Mk passes no `-D` either. `tests/jail` asserts `$(V8BIN)`
   and `$(IMGBIN)` are disjoint -- measured, `make` emits no warning of any kind
   when a name is in both.
   **4f: `dump`, `restor` and `dumpdir` -- the last three of the ten, and the
   only ones that are not filesystem tools.** A dump is a *tape* format, so
   these answer to a different 1985 record; both `-f` flags take an ordinary
   file, so no tape device is needed either.

   **`struct spcl` IS A WIRE FORMAT WITH THE SAME PROBLEM EVERY ON-DISK STRUCT
   HAD BEFORE STEP 4A**, and `<dumprestor.h>` is narrowed the way `<sys/ino.h>`
   is: `c_date` and `c_ddate` to four bytes. Three things make that forced
   rather than stylistic. The struct **already had a VAX-shaped half** -- it
   embeds a `struct dinode`, fixed at 64 by step 4a -- so eight-byte times would
   have been VAX-correct after `c_dinode` and shifted before it. The record is a
   **fixed 1024 bytes**: `dumptape.c` copies exactly `BSIZE(0)` out of a `char
   tblock[NTREC][1024]`, so what reaches the tape is the first 1024 of a
   1124-byte struct, and widening a header field slides everything after it and
   drops eight more bytes off `c_addr`. And **the checksum cannot see it** --
   `restor` sums those same 256 ints and `dump` writes a compensating word, so
   it catches a writer and reader who disagree and both are ours. Measured from
   the V8 side and asserted: `spcl 1124`, offsets
   `0 4 8 12 16 20 24 28 32 96 100`. `struct idates` is deliberately left alone,
   because `/etc/ddate` is a *text* file.

   **`getgrnam` was resolving from `-lSystem`**, so `dump` would have read the
   Mac's `/etc/group` from inside the jail -- the `getgrent`/`ls -g` leak again,
   caught the same way by `nm -u`. V8 ships the source; it is in `libv8c` now.

   **A THIRD UPSTREAM SOURCE FOR THE INSTALL DESTINATION, and it is right where
   `Admin/dest` falls through.** Eleven imported makefiles say where they
   install themselves. They agree with the tables on nine; the two they do not
   are both programs in *no table at all*, so `dest` answers by fall-through --
   "nobody said", not "V8 said". `cpp` settles it: its makefile says `/lib`, the
   shipped tree says `/lib`, `dest` says `/usr/bin`. This port already has `cpp`
   in `/lib` but by accident, since it is a toolchain target with its own rule.
   `dump` had no such accident, so the Makefile follows the makefile. The whole
   set is recomputed by `tests/wavea` so the exception list cannot grow quietly.

   Upstream ships a **1985 VAX `a.out` of `dump`** in its own source directory
   (magic `0413`, 30275 bytes). It is deliberately not imported: it is a build
   artefact whose name is the makefile's target, so a rung-5 case asking
   `[ -x dump ]` after copying the directory would pass on a binary that cannot
   run here.

   **THE ROUND TRIP CLOSES, AND IT MAKES `restor` THIS PORT'S SECOND FILESYSTEM
   WRITER.** `mkfs` builds an image, `dump 0f` writes a tape, `dumpdir f` lists
   it, `restor rf` rebuilds a filesystem from it -- and the *restored* image is
   judged by the five readers that know nothing about tapes: `icheck`
   `files 5 (r=3,d=2) used 4 missing 0`, `dcheck` silent, `ncheck` naming the
   same three paths, `fsck` clean in all five phases without modifying it. That
   is a statement no single program here could make about itself.

   **AND WHAT STOPPED IT WAS A BUG IN v8cc, NOT IN THE PROGRAMS.** Neither
   reader could read a tape `dump` wrote, and the tapes were correct -- an
   independent 32-bit sum over every record gives exactly `CHECKSUM`. The
   failure was `checksum()`'s `register int` accumulator: **an `int` must wrap
   at 32 bits and this back end never wrapped one**, because every integer
   lives in an x register and arithmetic was emitted 64-bit. The value
   disagreed with itself -- it *printed* `Checksum error 244736`, which is 84446
   octal, the number it was looking for. `arm64_trunc()` in
   `compiler/ccom-arm64/gencode.c` fixes it at what this called four emission
   sites -- **`arm64_trunc()` has five call sites today**, the unary `-` and
   `~` having been added by the sweep that followed; the
   three-stage fixpoint still holds. CLAUDE.md has the general rule. Note the
   writer was unaffected, because `spcl.c_checksum = CHECKSUM - s` is a store
   and `str w` re-narrows: **the port could write correct 1985 tapes it could
   not itself read.**

   Both readers also carry the fsck seam -- `ctime(&spcl.c_date)` -- and it
   behaves in two completely different ways: on a **level 0** tape `c_ddate` is
   zero so the read is accidentally correct, and on an **incremental** it is the
   live lock with empty stdout. A test written against a level-0 tape passes.
   Fixed with `time_t` temporaries in both, plus the write direction at
   `dumpmain.c:18`, which was invisible for three independent reasons at once.
5. **`v8fs` as the third server** -- V8's own `alloc.c`, `iget.c`, `nami.c`,
   `rdwri.c` over that image. Then `mklost+found`, and the other nine.

   **SURVEYED, the same way steps 1 and 1b were, and the four files named in
   that sentence are NOT the unit.** Costing by external-name count before
   importing anything is what made `stream.c`, `streamio.c` and `ttyld.c`
   tractable, so it was done here first. Every number below was measured
   against `third_party/`.

   The four files call **47** names they do not define between them. Six are
   already in `libv8kern.a`. The rest resolve to eleven other kernel files, and
   **following them transitively converges at 42 files and 17,393 lines** --
   `vmmem.c`, `vmpt.c`, `vmdrum.c`, `vmswap.c`, `vmpage.c`, `vmsched.c`,
   `trap.c`, `vaxtrap.c`, `uba.c`, `main.c`, down to the M780/M750
   memory-controller registers. That is not an import; that is the VAX kernel.

   **But the explosion comes through exactly two functions.** The dominant
   callee group is the **buffer cache**, `dev/bio.c`, which supplies ten of the
   47 -- `bread breada bwrite bdwrite bawrite brelse getblk geteblk clrbuf
   bflush`. Classifying all 23 of its functions by VAX-VM contact, the way
   `proca.c` was classified above:

   | | functions | VAX-VM names |
   |---|---|---|
   | the cache | 21, incl. every name above | none -- only `spl0`/`spl6`/`splx` |
   | dead here | `swap` (:523), `physio` (:675) | `vtopte btop ctob btoc dptopte useracc vslock vsunlock` |

   `vtopte` is at `:555`, inside `swap`; `useracc` `:684`, `vslock` `:715`,
   `vsunlock` `:720`, all inside `physio`. **Nothing else in `bio.c` touches
   the VAX at all**, and `pte.h` is included by `bio.c` alone, for `swap`. So
   the eight are dead code that must *link* and never run -- panic stubs -- and
   the closure collapses.

   **The real unit is six files and 2743 lines**: the four, plus `sys/subr.c`
   (239, for `bmap`) and `dev/bio.c` (783). They define 58 names and need 59,
   which split:

   | | count | |
   |---|---|---|
   | already in `libv8kern.a` | 9 | `copyin copyout panic spl6 splx stread stwrite tsleep wakeup` |
   | macros from authentic headers | 23 | `BSIZE BMASK BSHIFT NINDIR INOPB itod itoo BITFS MIN ...` |
   | already redirected in `shim/kern/h/param.h` | 3 | `bcopy printf psignal` |
   | defined *inside* the files themselves | 2 | `BUFHASH` (`bio.c:42`), `INOHASH` (`iget.c:18`) |
   | scan artefacts | 3 | `int unsigned dp` |
   | **genuinely new** | **19** | below |

   The 19 are `access findmount fubyte fuibyte fustrlen mfind munhash sleep
   spl0 subyte suibyte suser tablefull uprintf useracc vslock vsunlock vtopte
   xrele`. Four (`useracc vslock vsunlock vtopte`) are the panic stubs above.
   Three (`fuibyte subyte suibyte`) are reached only from `subr.c:162,188`,
   the character-at-a-time user I/O. The rest are ordinary kernel services of
   the kind `shim/kern/sys/` already holds fifteen of.

   **THE 19 ARE 20, AND FIVE OF THEM ARE NOT C FUNCTIONS.** Specified
   name-by-name against upstream and re-read at source; `src/sys/PORTING.md`
   has it all. The four that change the plan:

   - **`plock` is missing from the list**, called three times by `nami.c`
     and defined at `sys/pipe.c:105`. It was invisible because `h/inline.h`
     makes it a **macro** under `#ifndef UNFAST` and `iget.c:13` includes
     `inline.h` while `nami.c` does not -- so the same name is a macro in
     one of the six and a real call in another.
   - **`fubyte fuibyte subyte suibyte spl0` are inlined by `sys/asm.sed`**
     before the assembler sees them (`conf/makefile:103`), and `fuibyte` has
     no definition anywhere. `fubyte` **zero-extends** (`locore.s:776`,
     `movzbl`), so a sign-extending shim turns byte `0xFF` into EFAULT; and
     `subr.c:162`'s `?:` binds looser than `<`, so for `id != 0` the raw
     return value **is** the error test and success must be exactly 0.
   - **`mfind` must be declared `struct cmap *`** -- `h/cmap.h:36` inside
     `#ifdef KERNEL`, and `rdwri.c:10` includes it. The returning-NULL claim
     holds; the width claim inverts. `pte.h` carries the same shape for
     `vtopte` and is the last header with no home.
   - **Four names collide at link time**, measured with `nm -g`: `access`
     against `libv8stubs.a`'s userland `access(2)` (**and the returns are
     inverted, 0/1 against 0/-1**), and `free`, `sleep`, `ialloc` against
     `libv8c.a`. Plus `SIGKILL` and `SIGXFSZ` missing from
     `shim/kern/h/param.h`.

   **BUILT, AND THE FOUR COLLISIONS ARE NINE.** `src/sys/PORTING.md` has the
   account; the three that change the shape of the claim:

   - **`alloc.c` is a SECOND deviation, and it came as a warning.** `:34` is
     `register long *p` over `s_bfree`, the superblock free-block bit map that
     §8a step 4a narrowed to `v8_i32` because it is on disk. The array
     narrowed and the pointer did not, so the bit-clear is an 8-byte
     read-modify-write on a 4-byte word and the scan strides eight, covering
     half the map and then 961 words past the buffer. Same `NOLONG` cause as
     `nami.c`'s -- and upstream states it five lines below, in a comment
     reading `BITS PER LONG`. `nami.c`'s stopped every path lookup; this one
     would have corrupted a free map and been blamed on `mkfs`.
   - **Nine collisions, and the five nobody costed split by KIND.** Six are
     function-against-function and the linker catches those. Three are a
     **variable against a function** -- `time`, `timezone`, `mount` -- where
     a K&R tentative definition is a COMMON symbol and resolving a common
     against a text definition is what a linker is *supposed* to do. Silent.
     Only `nm -g` sees them, and `tests/kmemu`'s pairwise sweep had been
     reading three archives when the build makes five -- and the first
     correction to that said four, missing `libkmemu.a`.
   - **`dev/conf.c` is vestigial and Bell Labs say so.** Its `fstypsw[0]`
     names `rnami`, defined nowhere; `nami.c:167` upstream says *"USED TO BE rnami"*
     above `fsnami`, and `conf/config_diff:11` is *"dev/conf.c is no more.
     config makes a conf.c for each machine"*. The live source is
     `conf/devices:70-73`, which gives `fsnami`. `nfstyp` is **1** here.

   17 suites, 1614 passed, 0 failed. `libv8kern.a` imports exactly
   `_longjmp _memcpy _setjmp`.

   **`mfind` is the one that is not a stub, and answering it honestly is the
   interesting part.** `rdwri.c:182-183` calls `mfind(dev, bn)` then
   `munhash(dev, bn)` in the **live** write path -- before overwriting a file
   block, invalidate any in-core mapping of it. Upstream searches `cmap[]`, the
   VAX core map. There is no core map here and no V8 process maps a file block,
   so `mfind` returning `(struct cmap *)0` is **correct** rather than a stub,
   and `munhash` is then unreachable by construction.

   **The headers cost 20, and only one is a VAX document.** Three are already
   imported (`dir.h`, `inode.h`, `inline.h`); `pte.h` (85 lines, 21 VAX
   references) is the only one that describes the machine, and only `bio.c`
   wants it. The other sixteen are 1286 lines of ordinary structure.

   **AND THAT PARAGRAPH COSTED THEM BY LINE COUNT AND BY VAX-REFERENCE, WHICH
   IS THE ONE PAIR OF MEASURES THAT CANNOT SEE WHAT THEY ARE.** Measured at
   import time: **fourteen of the twenty are the same upstream blob as a file
   this port has already imported.** V8 ships one header at
   `/usr/sys/h/` *and* `/usr/include/sys/`, byte for byte --
   `git hash-object sys/h/param.h include/sys/param.h` gives
   `5409ff39...` twice, and the same holds for `dir.h inode.h filsys.h ino.h
   fblk.h buf.h proc.h conf.h user.h systm.h mount.h acct.h vlimit.h`.

   So the real question is not what the headers cost but **which copy the
   kernel side should see, where the port has already patched one** -- and the
   answer differs per header, because every patch was a *userland* decision:

   - `filsys.h`, `ino.h`, `fblk.h` are patched to `v8_i32`/`v8_u16` per field
     by step 4a. The kernel reads the same disk, so it needs the same patch.
     **Importing the pristine kernel copies would have reintroduced step 4a's
     bug on the far side of one disk** -- a `struct filsys` with an 8-byte
     `s_time` over images `mkfs` writes with 4 -- and no reader in the tree
     could have seen it, because every reader uses the patched header and only
     the kernel the pristine one. Neither line count nor VAX-reference count
     is capable of noticing that an ordinary-looking struct is an on-disk
     record.
   - `dir.h` is already split, and the split states the rule the tree
     follows: `src/sys/h/dir.h` is **pristine** (upstream says 14) and only
     `src/include/sys/dir.h` is patched, to 254. Kernel side pristine,
     userland side patched where the port had to widen something. `inode.h`
     is the same.
   - `param.h` is settled below, and the settlement is *not* the third
     spelling the caution expected.

   `src/sys/PORTING.md`, "the six files, and what the headers turned out to
   be", has the table.

   **Two things the import RETIRES, which the sentence in step 5 does not
   suggest.** `shim/kern/sys/subr.c` hand-writes `min`, `max` **and `iomove`**
   -- and `rdwri.c` defines all three (`:236`, `:250`, `:266`), so the import
   replaces three stand-ins with the authentic source that the shim's own
   comments already cite. And `shim/kern/h/buf.h` is a 30-line stand-in whose
   header comment reads *"There is no buffer cache here and no disk driver, so
   importing it would put a description of hardware in the tree to obtain two
   constants."* That was true when it was written for `streamio.c` and step 5
   is precisely the thing that falsifies it: the authentic 107-line `h/buf.h`
   becomes required, and it has **zero** VAX references -- the prediction was
   wrong about the header as well as about the cache.

   **A caution carried from step 4a.** `h/param.h` is where `DIRSIZ` is
   decided, and this port raises it 14 -> 254 in the *userland* copy while
   `mkfs` is compiled `-DDIRSIZ=14` because what it writes is a disk image. A
   kernel-side `src/sys/h/param.h` is a third spelling of that number and must
   be settled deliberately, not inherited.

   **SETTLED, AND THERE IS NO THIRD SPELLING -- upstream's `h/param.h:75`
   already says `DIRSIZ 14`,** which is exactly what the kernel side wants.
   A pristine import would have been *correct* on the number the caution was
   about, and the precedent permits it too (see `dir.h` above). Two other
   things rule it out, and the first is the one nobody would look for in a
   file called `param.h`:

   - **`param.h:169-171` includes `"../h/types.h"` under `#ifdef KERNEL`,**
     and upstream's `h/types.h:23` is `typedef long daddr_t;`. So importing
     `param.h` drags in a pristine `types.h` with an **8-byte `daddr_t`** --
     the `filsys.h` hazard above, arriving by a second route. (`:48` also
     pulls `<signal.h>`, which in a kernel compile is the host's.)
   - **`shim/kern/h/param.h` holds the `_OFF_T`/`_INO_T`/`_DEV_T` guards**
     that stop Darwin redefining `struct inode`'s layout, and the
     `printf`/`bcopy`/`uballoc` redirections that keep `stream.c`
     byte-identical. An authentic `src/sys/h/param.h` **wins the quoted
     include** and takes all of that from `stream.c`, `streamio.c` and
     `ttyld.c`, which compile against it today.

   Upstream's is also headed `"Tunable variables"` and carries `NBPG PGSHIFT
   CLSIZE CLOFSET UPAGES clbase clrnd`. That reads like a machine description
   by this tree's own test, but it is the weakest of the three arguments and
   should not be leaned on: `CLSIZE 2` is what selects the 1024/4096
   geometry, so it is as much a disk fact as a machine one.

   Not imported. The filesystem geometry goes into `shim/kern/h/param.h` at
   upstream's values -- which is what that file's header comment already says
   the policy is -- and a test **compares the values against the authentic
   `src/include/sys/param.h`** rather than trusting the transcription.

   **AND THE `lp64-auditor` WAS RUN OVER ALL SIX BEFORE ANY OF THIS IS BUILT,
   WHICH IS WHERE THE SURVEY STOPS BEING ARITHMETIC.** Its central claims were
   re-read at source rather than taken on report. Four findings change the
   plan, and the first two are the kind that make a build fail in a way that
   reads like a port bug:

   - **`nami.c:145-148` upstream breaks path resolution outright, and it is `NOLONG`
     again.** Under `#if DIRSIZ == 14` upstream hand-unrolls the name compare
     as `*(long *)&nm[0]`, `&nm[4]`, `&nm[8]`, `*(short *)&nm[12]` -- exactly
     4+4+4+2 = 14 **because V8's `long` is 32 bits** (`ccom/vax/macdefs.h:20`,
     `# define NOLONG`, which §4a already records). Here it is 8+8+8+2, reading
     two bytes past both fields, so `dsearch()` fails to match a name that is
     present and every `namei()` returns ENOENT. The arm *is* the one selected:
     `src/sys/h/dir.h` is already imported at `DIRSIZ 14`, and 14 is right
     there because it describes a disk record. The fix is `int` for `long` in
     those four lines -- a recorded deviation that reproduces the VAX exactly.
     The **wrong** fix is `-DDIRSIZ=254` to reach the `strncmp` arm, which
     makes the symptom vanish by changing the on-disk format.
   - **`-DKERNEL` is required, and the Makefile comment that says otherwise is
     about a different file.** `h/buf.h:62`, `h/inode.h:56`, `h/mount.h:20` and
     `h/filsys.h:40` each open an `#ifdef KERNEL` that guards *every*
     pointer-returning declaration these files use -- `getblk`, `geteblk`,
     `bread`, `breada`, `alloc`, `baddr`, `ialloc`, `iget`, `namei`,
     `findmount`, `getfs`. Without it, `-Wno-implicit-function-declaration`
     makes each an implicit `int` at ~30 call sites and truncates the address.
     Upstream compiles with it (`conf/makefile:23`, `COPTS= ${IDENT}
     -DKERNEL`). `$(KERNFLAGS)` has neither `-DKERNEL` nor `-fcommon`; only
     `$(STREAMIOFLAGS)` adds them, and its comment says `-DKERNEL` "buys
     exactly one thing", which is true of `streamio.c` and false here.
     `h/systm.h` has **no** `#ifdef KERNEL` at all, which is why some
     declarations are unconditional and the gap is easy to miss.
   - **Three names collide with libc, and `free` is the live one.**
     `alloc.c:205` defines `free(dev, bno)`; `src/libc/gen/malloc.c:143`
     defines `free(ap)`. Two definitions with incompatible signatures in one
     world -- loud if both members are pulled, and **silent** if only the
     kernel one is, at which point the block allocator is handed a heap pointer
     and reads a superblock out of it. `h/systm.h:12`'s `time_t time;` is a
     tentative definition of libc's `time`, latent only because nothing in
     `shim/` calls `time()` today and armed the day `libkmemu` does -- and it
     is invisible to `tests/kmemu`'s `nm -u` sweep, because the name is
     *defined* here rather than imported. `h/mount.h:21`'s `mount[]` is the
     same shape, and arrives precisely when `-DKERNEL` does.
   - **Two of the three stand-ins this import "retires" do not go quietly.**
     `min`/`max` agree exactly -- upstream puts `unsigned` on its own line at
     `rdwri.c:235` and `:249`, and `h/systm.h:61-62` declares them `unsigned`
     independently, so the shim's re-recorded note is right and deleting it
     loses nothing. But `iomove` **conflicts**: the shim prototypes
     `void iomove(void *cp, unsigned n, int flag)` and upstream is
     `register caddr_t cp`, which is a hard error against a visible prototype
     -- and the `void *` was deliberate, because `streamio.c` passes a
     `u_char *`. And upstream's `nulldev() { }` (`subr.c:212`) falls off the
     end where the shim's returns 0 on purpose, which is the `qopen` register
     litter its own comment records. So the import *reinstates* a
     nondeterminism the port had already removed.

   Two smaller things it measured that the shim has to answer: `clrbuf`
   unconditionally zeroes `BUFSIZE/sizeof(int)` = **4096** bytes regardless of
   `BSIZE(dev)` being 1024, so the buffer allocator must hand out 4096-byte
   buffers; and `shim/kern/h/user.h` has **no `u_dbuf` or `u_dent`**, both of
   which `nami.c` needs -- and `u_dbuf`'s placement in `struct user` decides
   whether the two-byte overread above is merely wrong or faults.

   **AND THE COLLISION CLASS WAS THEN MEASURED ACROSS THE WHOLE PORT, WHICH
   FOUND ONE ALREADY THERE.** The auditor's point about `free` is that
   `tests/kmemu`'s `nm -u` sweep cannot see it: that sweep looks at what a
   binary *imports*, and a collision is about what an archive *defines*. So the
   four archives were diffed against each other:

   | | defines | overlaps libSystem |
   |---|---|---|
   | `libv8c.a` | 196 | **153** — by design, it *is* a libc |
   | `libv8kern.a` | 115 | 1 (`panic`) |
   | `libv8sys.a` | 106 | 1 |
   | `libkmemu.a` | 10 | 0 |

   A blanket "no archive may define a libSystem name" is therefore impossible.
   The useful assertion is pairwise, and it is **not empty today**:
   `libv8kern.a` and `libv8c.a` both define **`min` and `max`** —
   `shim/kern/sys/subr.c` for the kernel, and `src/libc/gen/min.c` and `max.c`
   for the world. They are **different functions**: libc's are

   ```c
   min(a,b) { return (a<b? a: b); }        /* implicit int */
   ```

   and the kernel's are `unsigned`, which `rdwri.c:235`/`:249` and
   `h/systm.h:61-62` independently agree on. Nothing links both archives today
   — only `tests/streams` builds against `libv8kern.a`, and it pulls `setjmp`
   from libc without pulling `min.o` — so it is latent. But it is the `free`
   shape, present before step 5 adds any, and the difference is *signedness*,
   which this port has already been bitten by three times through
   `SIGNCONVKEEP`. Importing `rdwri.c` does not create this; it makes the
   kernel's pair authentic while leaving libc's `int` pair beside it.

   So step 5 wants a **pairwise archive-overlap assertion** rather than an
   import-sweep, seeded with `min`/`max` as the known and explained pair —
   which is the shape `tests/kmemu`'s allowed-leak list already has, and which
   went stale exactly once before, when nothing audited an entry.

   Clean, and said out loud because a survey that only lists hazards cannot be
   audited: **the `urcvfile` class is absent.** Every implicit-`int` K&R
   parameter across the six was enumerated, and not one holds a pointer -- each
   is an `fstyp` index, a 0/1, a `B_READ`/`B_WRITE` flag, a char or a `dev_t`.
   The narrowed-field-address class is clean in both directions:
   `iupdat(ip, &time, &time, w)` passes the address of the *full-width* global
   and the narrowing happens at the assignment into the `v8_i32` field, which
   is where it belongs. And the `di_addr[40]` three-byte pack/unpack in
   `iexpand` and `iupdat` is correct -- but *only* because `daddr_t` is 4
   bytes, which is what makes §4a's global narrowing load-bearing rather than
   cosmetic.

   **STEP 5c IS DONE, AND IT IS THE FIRST TIME ANY OF THIS CODE HAS RUN.**
   The six files compiled and linked in step 5b; nothing had executed a line of
   them. `tests/streams/fsprobe.c` now drives `namei -> fsnami -> dsearch ->
   iget -> bmap -> readi -> bread` over an image `mkfs(8)` wrote, and `cmp`
   confirms the 28000 bytes that come back are the file mkfs was handed. The
   file is two directory levels down and 28 blocks long, so the walk covers a
   subdirectory and `bmap`'s **indirect** arm as well as its direct one.
   262 -> 315 cases; `src/sys/PORTING.md` has the account. Three pieces had to
   exist first and each went where upstream put its equivalent:

   - a **block driver**, in the probe rather than `shim/kern/`, because nothing
     in the port consumes one -- the unconsumed-component rule, with
     `sioprobe.c` as precedent -- registered through a new `v8k_bdconf()` in
     `shim/kern/sys/ioconf.c`, the file already named for the switch tables
     `config(8)` generates;
   - **`shim/kern/sys/main.c`**, new, standing in for `sys/main.c` (the startup:
     `binit`, `iinit`), `sys/machdep.c` (the storage `valloc` carves) and
     `sys/param.c` (the size formulae). Two of those three describe a VAX, and
     `param.c`'s formulae are unevaluable here because **MAXUSERS was never
     shipped** -- the `NTTY` situation again, so `NINODE` is *derived* from Bell
     Labs' own inode:file ratio and this port's `NFILE`, not picked;
   - **`allocmount()`**, beside `findmount` in `v8fs.c`, transcribed including
     upstream's `!mp->m_flags & M_MOUNTED` precedence quirk, which is correct
     only because `M_MOUNTED` is 1 and is the structure's only flag.

   Three findings worth carrying past this step. **`shim/kern/h/buf.h` was dead
   and nothing said so** -- bio.c's import brought the authentic `buf.h` into
   the tree and the includer's-directory-first rule silently redirected
   `streamio.c` to it, while a `tests/deps` case named for our copy stayed
   green. **Bell Labs' own comment at `alloc.c:414` is stale against their code
   twelve lines below it** (`panic: no fs` versus `panic("getfs")`), and this
   port had copied the comment down as behaviour -- the recorded-diagnosis rule
   reaching the one place the fidelity contract guarantees we never read
   critically. And a subagent audit found **sixteen stale line citations** across
   the tree, eight of them caused by inserting one PORT comment; the `alloc.c`
   self-citations are now a test rather than prose.

   **STEP 5d IS DONE: THE WRITE HALF, AND V8's OWN CHECKERS PASS ON WHAT IT
   WROTE.** `writei`, `bmap`'s **allocating** arm, `alloc()`/`free()`,
   `ialloc()`/`ifree()`, `itrunc` and `nami.c`'s `NI_CREAT`/`NI_DEL` all run.
   The probe overwrites a block, extends into a new one, extends past block 9
   so the indirect block is made too, creates a file **by name**, writes
   several hundred blocks into it, deletes it, and flushes with
   `update()`/`bflush()`. 315 -> 372 cases; the tree stood at **1767 across 17
   suites** on the day of that step -- past tense on purpose, because this is a
   record of a moment and not a claim about now, and two copies of that same
   number elsewhere had gone stale reading as current.

   The instrument is the superblock's own `s_tfree`/`s_tinode` rather than a
   count of device writes, and the strongest pair is the round trip: after the
   delete both are exactly what they were before the create. **The free-list
   CHAIN is the point** — `alloc()` only follows it when `s_nfree` hits 0
   (`alloc.c:163-176`), so the probe reads `s_nfree` at run time and allocates
   more than that; it is V7's `struct fblk`, written by mkfs and walked by a
   1985 kernel, the metadata half of step 5c's data claim.

   Four findings, and the first two were both *notes that stopped being true
   without being touched*:

   - **`u_limit[LIM_FSIZE]` was 0**, so `writei`'s IFREG arm rejected every
     write to a regular file — with **EMFILE**, upstream's own choice, which
     points an investigation at the file table. Fixed by `v8k_uinit()` in
     `shim/kern/sys/main.c`, transcribing `main.c:52-79`.
   - **`access()`'s `s_ronly` arm became restorable at step 5c** and the note
     saying it was impossible was still there. Step 5d is the first step to
     call `access()` with `IWRITE` at all.
   - **A MUTATION KILLED THE TEST RUNNER INSTEAD OF FAILING A CASE.** With
     `u_limit` 0, `writei` calls `psignal(u.u_procp, SIGXFSZ)`; the probe never
     calls `v8k_procinit`, so `v8k_hostpid` was 0, `v8k_hostof` returned 0,
     `psignal`'s guard was `hp < 0`, and the syscall was **`kill(0, SIGXFSZ)`
     — the whole process group**. `gsignal` eleven lines away had carried the
     equivalent guard since it was written. Both refuse 0 now.
   - **The acceptance test is icheck, dcheck and fsck**, and mutation M3 is the
     case for having them: breaking `alloc`'s free-list refill so blocks repeat
     left **every probe case green** and was caught only by Bell Labs'
     programs. A probe's reader and writer share a belief; theirs do not.

   `src/sys/PORTING.md` has the account, including the two probe comments the
   mutations proved wrong.

   **What step 5 still does NOT have is a MOUNT.** Everything above is reached
   by calling the kernel's functions directly from a probe. A fourth type in
   `shim/v8sys/vfs.c` — so an ordinary V8 program's `open(2)` lands in v8fs and
   `/bin/cat` can read a file out of an image — is its own step, because it is
   about the shim's dispatch rather than about Bell Labs' code, and because it
   needs `smount`, which is not imported.

   **STEP 5e WAS COSTED AND IT CANNOT BE BUILT IN-PROCESS. The mount is the
   thing that forces the server, and there are two independent reasons.**

   The fourth type has to call `namei`, `iget` and `readi`, so the client has
   to link `libv8kern`. Measured, 297 program objects against the archive's
   266 defined names: **56 collisions, 33 objects, 29 programs, 27 names** —
   and **25 of the 56 are silent**. `cat.c:10`'s `char buf[4096]` is a K&R
   tentative definition, i.e. a common; `shim/kern/sys/main.c:213`'s
   `struct buf *buf` is a real definition; the linker prefers the real one, and
   `cat`'s buffer becomes an eight-byte pointer. It needs no `-force_load` — one
   undefined reference to a kernel entry point pulls `main.o` in. Whether you
   *notice* is a property of the layout: under `-force_load` it is SIGSEGV, and
   in the natural link it is **exit 0 with byte-identical output**, having
   scribbled 4088 bytes over `_buffers` and `_nbuf`, the buffer cache's own
   pointers. The names are the 1985 vocabulary — `buf bread alloc bmap tty file
   bwrite getblk iput itrunc panic copyin copyout` — and the checkers are
   over-represented because they reimplement the kernel's algorithms under the
   kernel's names. `ld -r -exported_symbols_list` hides 22 of the 27 and cannot
   hide the other five, because a common cannot be made a private extern.

   The second reason needs no measurement and `shim/v8sys/vfs.c:167` had already
   written it down: the descriptor table **does not survive `exec`**. A v8fs
   descriptor is an inode pointer and an offset in process memory, so
   `cat /mnt/a > /mnt/b` — `sh` opens the target, `cat` inherits it — is
   impossible in the client however the symbols are arranged.

   So the mount is **step 5e: a v8fs server**, and the 9P seam this section
   opens with stops being the eventual shape and becomes the requirement. Three
   things are decided by the above rather than left open: the server is a host
   binary, so the collisions never arise; it is the single authority for the
   buffer cache, so two writers cannot corrupt an image; and **the fid must live
   in something that survives `exec`**, which points at one connection per open
   file — the socket then *is* the descriptor and no client-side table has to be
   inherited.

   **That last one rests on `getpeername(2)` and it was measured before being
   relied on**, because the design has no fallback if it is wrong. A client
   `connect`ed to a bound `AF_UNIX` path, `dup2`'d to fd 7, and `exec`'d:

   | descriptor | `fstat` | `getpeername` |
   |---|---|---|
   | client side, after `exec` | `S_IFSOCK` | **the bound path**, len 106 |
   | server side (`accept`ed) | `S_IFSOCK` | empty, len 16 |
   | `socketpair` | `S_IFSOCK` | empty, len 16 |

   So a program that has just replaced itself can identify its own inherited
   mount descriptors positively, without writing a byte to them — and the
   asymmetry is a feature rather than a limitation, because a pipe-like
   `socketpair` or an `accept`ed connection **cannot** be mistaken for one.

   `shim/kern/NOTES.md` has the full collision measurement; `tests/kmemu`
   asserts that no V8 binary links `libv8kern`, so re-opening the in-process
   option means deleting a case that says why it was closed.

   **BOTH HALVES ARE BUILT.** `shim/v8fsd/v8fsd.c` is the server and
   `shim/v8sys/p9cl.c` the client, a fourth type in the switch, configured from
   `V8MOUNT=<prefix>=<socket>`. `cat`, `ls`, `tail`, `wc`, `grep` and `sh`
   redirection all work against a disk image unmodified; `tests/streams` 372 ->
   452. The `getpeername` measurement above is doing exactly the job it was
   taken for, and it earns its place a second way that was not foreseen: since
   the connection is the open file description, **the offset belongs on the
   server too**, and the client ends up holding no per-file state at all. Which
   is where 9P turned out to assume something this port does not have -- it has
   no seek message, because Plan 9's *kernel* held the offset in the Chan. One
   extension, confined to a sentinel offset plus `Tseek`/`Rseek`; `shim/p9/p9.h`
   argues it and `shim/kern/NOTES.md` records the three bugs an end-to-end run
   found that no probe could.

   **Step 5f then made it WRITABLE**, and the first thing it had to settle was
   not a message but a claim: "read only" had described the PROTOCOL and never
   the filesystem. `readi` sets `IACC`, so `iput` ran `IUPDAT` and dirtied the
   disk inode on every read. Re-measuring the recorded finding with an
   instrumented driver corrected it twice -- `O_RDONLY` was doing no work at
   all (O_RDWR with no flush is **zero** pwrites across twenty-two reads,
   because the delayed buffer is never recycled), and there is a THIRD accident
   nobody had named: `time` is set once from the superblock's `s_time` and
   nothing advances it, while `mkfs` stamps every inode with that same number,
   so the write stores the bytes already there. The driver prints the pwrite
   and `cmp` prints nothing; perturb one `di_atime` first and exactly four
   bytes move. Round-trip class, third instance, and the first in an artefact
   rather than in memory.

   So 5f is four things and only one of them is a message. **The clock
   advances** -- `v8fs_clock()` is the other half of the substitution `iinit`
   already makes for `clkinit`, through a raw `gettimeofday` because
   `libv8kern` may name no libc function. **Read-only is a mount flag and Bell
   Labs wrote both lines** -- `fsmount()` at `sys3.c:299,316` opens the device
   `!ronly` and stores `ronly & 1`, so `v8fsd -r` makes `iupdat` return before
   it breads anything and not even an atime moves. **Something flushes**, once
   per poll wakeup, which is `sync(2)`'s own body. And then Twrite, Tcreate
   (including `DMDIR`, through eleven transcribed lines of `sys2.c` that write
   `.` and `..`), Tremove, Twstat and a second extension, `Taccess` -- 9P has
   no `access(2)` for the reason it has no seek, and the client had already
   been wrong about permission in both available directions.
   `sh -c 'echo x > /mnt/fresh'` creates a file on a disk image through Bell
   Labs' `namei`/`ialloc`/`writei` in another process; `mkdir`, `rm`, `rmdir`
   and `cat a > b` work; and `icheck`, `dcheck` and `fsck` say the result is
   clean with the block count back to exactly where it started.
   `streams` 535 -> 567; eight mutations, eight fire.

   **What 5f leaves is a shorter list than it inherited, and the count was
   never eleven.** An auditor counted FOURTEEN slotless syscalls: nine that
   refuse (ten `MOUNTED()` calls, because `link` guards both names), plus
   `access`, `readlink` and `chdir` which answer instead, plus `chroot` --
   which passes its path completely unresolved, mounted or not -- and `execve`.
   `access`, `unlink`, `mkdir` and `rmdir` are slots now. `chmod`, `chown` and
   `utime` are one `Twstat` away and were deferred to keep the step reviewable.
   `link` and `symlink` have no 9P2000 message and a V7 image holds no symlink;
   `mknod` for a device is meaningless on an image no kernel will mount. And
   `chdir` is still the one gap the client cannot close alone, because nothing
   in the shim tracks a working directory.

   **AND FILLING THE IMAGE KILLED THE SERVER**, which 5f made reachable and
   5f-b's audit found. Bell Labs' out-of-space path is a kludge they label one
   in capitals -- `alloc.c:185-196` sleeps five clock ticks on `lbolt` hoping
   another process frees a block -- and this port maps `sleep()` onto a
   `tsleep` that PANICS with no device below and no timeout. `cat big > /mnt/x`
   on a 200-block image took the whole server down, dropping every client's
   connection rather than only the writer's. The panic is right for the streams
   its comment reasons about; `alloc.c:194` is a second caller with a different
   answer, and the answer is provable in two greps: that line is the only
   sleeper on `lbolt` in the imported tree, and `clock.c:290` -- the clock
   interrupt -- is its only waker in the whole kernel, which this port neither
   has nor imports. **And a second defect was hiding behind it**: `kmkdir`
   never consulted `u.u_error` after `writei`, so once the panic was gone,
   `mkdir` on a full image exited **0** for a directory `fsck` calls damaged.
   Upstream ignores the same return and can afford to, because it IS the
   syscall and `u_error` reaches the user. `streams` 592 -> 597.

   **Step 5f-b closed those three, and they are ONE MESSAGE.** A Twstat carries
   a whole stat and the server applies whichever fields are not the all-ones
   "do not touch" sentinel, so chmod is a wstat setting `s_mode`, chown one
   setting `s_uid`/`s_gid`, and utime one setting `s_atime`/`s_mtime`. What is
   left refusing is `link` and `symlink` alone; `mknod`'s device arm answers
   **EPERM** rather than EROFS now, because EROFS is a claim about the medium
   and stopped being true in 5f, while EPERM is a claim about the operation and
   is true of both worlds. `streams` 567 -> 592; eight mutations, eight fire.

   **Reading upstream before writing the client found three deviations in the
   server's own arms**, all of them collapsed today because `u_uid` is 0 and so
   both rules always permit -- which is exactly why they survived. `chmod` and
   `utime` gate on `owner(1)` (`fio.c:215-228`: ownership OR superuser) and the
   code tested `suser()` alone while **the comment above it claimed ownership**
   and cited the wrong file; upstream's `chmod` strips ISVTX for a non-root
   caller; and upstream's `utime` sets `IACC|IUPD|ICHG` where the arm set two of
   the three.

   **And the missing arm had been declined for a reason that was true and never
   covered the case that arrived.** `s_atime` was not honoured because "nothing
   in this world sets atime alone" -- still true, and `mv(1)` sets BOTH.
   `mv.c:129` is `utime(target, &s1.st_atime)`, and on a mount it is not an
   unusual path but the **only** path, because `link(2)` is refused so mv always
   falls through to fork, `/bin/cp` and utime. Declining an arm because nothing
   sets a field *alone* is a different claim from nothing setting it at all.

   **The auditor's best find was the two ends of one wire an hour apart.**
   `do_wstat` parsed the owner with `atoi`, which has no error return, so
   `"nobody"` and `"--"` both set the inode's uid to **0 -- root** -- and got an
   Rwstat back. 9P2000 specifies that field as a NAME, so any conforming foreign
   client sends one. The reading end of the same field had had the guard since
   an earlier audit, with the contract written beside it: *root maps to root, and
   non-root never maps to root.* Range is not parseability and only the second is
   guarded -- `"65536"` truncates, because `sys4.c:294` is `ip->i_uid = uap->uid`
   unchecked, and that is V7's own answer.

6. **The SIMH cross-check**, as an acceptance test rather than a CI job.
7. **FSKit host client.** Public API since macOS 15.4, no kernel extension;
   lets the host mount a V8 image in Finder and disposes of the
   ingest-and-extract problem. This was scheduled "with Phase 5 ... beside
   blitterm" purely because both are Swift, and with 5 dropped it loses that
   companion and nothing else — the *reason* for it was never blitterm. It
   also got materially cheaper in the meantime without anyone noting it here:
   §8a step 5e built a 9P server that already answers walk/open/read/write/
   stat/wstat/create/remove over a socket, so an FSKit extension is a second
   *client* of `v8fsd` rather than a second implementation of the filesystem.
8. **V9**, which needs `mk` first -- see S4a. The kernel layout is close enough
   to V8 that steps 1-6 should carry.

BITFS is **not** on that list: `mkfs` cannot create one, its 120 MB ceiling and
hardcoded geometry make it the less useful format, and the checkers that do
understand it can be pointed at an image made elsewhere if it ever matters.

### Verify before relying on it

- **NetBSD's `sys/fs/v7fs/`** has a userland I/O backend (`v7fs_io_user.c`) and
  looked like ready-made host tooling. It targets **512-byte-block V7**, and
  V8's free-list format is **1024** while BITFS exists nowhere outside V8. Check
  whether block size is parameterised; if it is not, this is not reusable and
  host tooling comes from V8's own code instead. Do not plan on it.
- SIMH's `.dsk` signature behaviour, before treating a round trip as clean.

## 9. Verification strategy

1. **Compiler:** stage-2/stage-3 fixpoint (bit-identical assembly); differential testing —
   compile K&R programs with v8cc and clang-gnu89, compare runtime behavior; `ccom/vax/tests`
   corpus if usable.
2. **libc/shim:** unit tests per syscall group; dir-emulation edge cases (long names, big
   dirs); signal semantics (reset-on-delivery, longjmp-from-handler).
3. **Commands:** golden stdin/stdout fixtures per filter; `usr/man`+`usr/doc` as the troff
   corpus (byte-compare nroff output; visual-compare troff→PS).
4. **World test:** `rootfs` self-build — the ported world rebuilds itself with v8 `make`,
   `sh`, `cc` from inside.
5. CI later: macOS ARM64 + Linux ARM64 matrix.

## 10. Risks

| Risk | Mitigation |
|---|---|
| LP64 latent bugs everywhere (`NOLONG` heritage) | Port `lint` first; LP64 macdefs; crash-early shim asserts. |
| ARM64 backend correctness (the long pole) | Differential testing vs clang; tiny-function-first bring-up; fixpoint gate. |
| `doprnt` C rewrite fidelity | Golden tests incl. `%e/%f/%g` edge cases vs printf(3) manual. |
| 14-char names / 16-bit inodes | Documented quirks of the world; truncation + hash-fold; collisions acceptable in practice. |
| Signal-semantics mismatches | Keep programs' observable V8 behavior; job control (`libjobs`, `csh`) may lag basic signals. |
| Streams-dependent code (`mux`, fd-passing) | Blit app absorbs mux; `SCM_RIGHTS` emulation only if a real consumer needs it. |
| License (non-commercial) | Repo stays non-commercial; COPYING.pdf carried; new code MIT-on-top. |
| Case-insensitive FS surprises beyond the 16 recovered | `CASE_COLLISIONS.md` manifest; import script warns on case-folding collisions. |

## 11. Effort map and sequencing

Rough focused-effort sizing (S≈a day, M≈days, L≈a week+, XL≈weeks):

| # | Work | Size | Depends on |
|---|---|---|---|
| 0 | Repo hygiene: vendor third_party, import tooling, top Makefile | S | — |
| 1a | Stage-0: yacc/cpp/ccom-pass1/make/lex/m4 under clang | M | 0 |
| 1b | **ARM64 backend for ccom** | **XL** | 1a |
| 1c | Driver rework, crt0, setjmp, self-host fixpoint | M | 1b, 2a |
| 2a | libv8sys shim (syscalls, dirs, signals, sbrk, ioctl) | L | 0 |
| 2b | libc port (strings, stdio+doprnt, floats, headers LP64) | L | 1b, 2a |
| 3A | Wave A filters + golden harness | M–L | 2b |
| 3B | Wave B shell/files (sh!) | L | 3A |
| 3C | Wave C doc-prep + man corpus | L | 3A |
| 3D | Wave D dev tools + lint-the-world | M | 1c |
| 4 | Grovelers via libkmemu (ps, w, who, df) | M | 2a |
| ~~5~~ | ~~blitterm Tier 1 (Swift)~~ — dropped, see §8 | — | — |
| 6 | Stretch: jim via Tier 2, upas/Mail, f77, cfront, Datakit-over-TCP | XL each | various |

Critical path: **0 → 1a → 1b → 1c/2b → 3A → 3B**, and it ends there now that 5
is dropped — every rung on it is done, which is what makes §8a the whole of the
remaining work rather than a branch off it.
First shippable milestone: *"v8 sh runs in Terminal.app, built by v8cc against v8 libc"* (end of 3B).
Second: *"man 1 ls through real troff"* (3C). Both reached. The third used to be
*"windows on a Blit"*; with 5 dropped it is **§8a's**, and §8a step 5 has already
passed it — *"V8's own `namei` walks a disk image in another process, and `sh`
redirects into it"*.

## 12. Open items (defaults chosen, veto anytime)

1. Vendor `third_party/` into this repo as a snapshot (default: yes, pending your commit approval).
2. LP64 target model (default: yes; ILP32 impossible on macOS).
3. Drop `c2`/`-O` (default: yes).
4. Host-tool exception list as in §5/§7 (default: as written).
5. Blit Tier 1 scope: app absorbs mux role rather than porting host `mux` (default: yes).


---

## Current state (updated as work lands)

| Phase | State | Evidence |
|---|---|---|
| 0 repo hygiene | done | `third_party/` vendored with provenance, `tools/import.sh` |
| 1a stage-0 | done | `cpp` 13/13 |
| 1b ARM64 back end | done | `v8ccom` 77/77 — arithmetic, control flow, pointers, arrays, globals, statics, recursion, structs, bitfields, floats, 12-argument calls |
| 1c driver | done | `v8cc` 11/11, `make rootfs` |
| 2a libv8sys | done | `v8sys` 54/54 — including signal *delivery*, not just numbering |
| 2b V8 libc | done | 89 objects, compiled by v8cc: stdio (incl. `%f`/`%e`/`%g`), the string family, malloc, ctype, qsort, getenv, the directory routines, `setjmp`/`longjmp`, perror and IEEE floats (`libv8c` 30/30) |
| 3A Wave A | done | **156 of 163** single-file commands in `usr/src/cmd` build, including `ls`. **All 48 imported into `src/cmd` are now real installed binaries in `rootfs/bin`** (every `.c` there but `cc`, which has its own rule), and `wavea` 73/73 runs the *installed* ones rather than temp-directory copies — the rule the rest of the tree already followed. That change is what surfaced seven commands (`ascii`, `bcd`, `cal`, `morse`, `ptx`, `units`, `vis`) that compiled but had never been shipped, and two data files (`/usr/lib/units`, `/usr/lib/eign`) that `units` and `ptx` read by absolute path — without which they answer "no table" and "Cannot open  file /usr/lib/eign" |
| 3B Wave B | done | The **Bourne shell** runs (`sh` 21/21) — see `src/cmd/sh/PORTING.md`. The file and process tools run too (`waveb` 21/21): `cp`, `mv`, `mkdir`, `rmdir`, `sed`, `ed`, `dc`, `factor`, `primes`, `tsort`. **38 multi-file command directories** compile and link, including `ps`, `w`, `df`, `tbl`, `qed`, `adb`, `yacc`, `man`, `diff3`, `dump`, `su`, `cron`, `compress` |
| 3C Wave C | **done** | **nroff, troff, tbl, eqn, pic, spell, man, grap and refer all run** (`wavec` 56/56). `tbl \| nroff` formats a table, `eqn \| nroff` sets an equation, `grap \| pic \| troff` draws a graph end to end, and `refer` resolves citations against an index its own `mkey`/`inv` built. eqn, pic and grap are built with **V8's own yacc and lex**, themselves compiled by v8cc. nroff fills and honours `.br`, `.ll`, `.ce`, `.sp`, `.na`; troff emits the device-independent stream for the 202 typesetter, with its tables compiled by `makedev` at build time. See the `PORTING.md` under `troff`, `tbl`, `pic`, `grap` and `refer` |
| 4 grovelers | **done** | `date`, `who`, `df`, `load`, `w`/`uptime` all run. `who` and `load` needed **no source change at all**; `df` and `w` one recorded deviation each. `ps` was the exception and is now done too, under S8a step 3 rather than here — V8's `ps` is a `/proc` client, not a kmem groveler, which was a plan revision forced by reading it. Only the full form of `w` remains, and it says `No mem` on purpose. See S7 |
| 5 blitterm | **dropped, 2026-08-11** | Decided by the owner rather than by measurement, and **nothing is given up**, which took two passes to establish. Tier 1's *editing* experience is native already: Plan 9 from User Space ships `sam` and `acme`, Rob Pike's own descendants of the Blit's software. That covers Tier 1 and **not** Tier 2, because plan9port is not a 5620 emulator — the Blit ran *downloaded* programs over a serial line under `mux`/`layers` (`proto.h`, `jioctl.h`) and plan9port reimplements none of it. **Tier 2 is solved in a sibling project, `ipad-v8`**, so the two-machine architecture exists as a working artefact and is simply not *this* repository's to build. The caveat is kept rather than deleted because it is still true of plan9port, and it names precisely the part that needed a second answer |
| 6 installation | done | `make install` stamps the prefix into every binary and writes the `v8` launcher; `jail` 128/128 |
| 8a.1 streams | **done** | Both halves of Ritchie's stream machinery run on ARM64. `src/sys/dev/stream.c` is byte-identical to upstream; `src/sys/sys/streamio.c` — the syscall side, 1093 lines — carries **two recorded LP64 deviations**, and `tests/streams` diffs it against third_party rather than hashing it, so the deviation list is itself a test. A stream opens, a write goes down the stack and comes back up through `strput` and out of `stread`; the module stack pushes, looks up and pops; FIORCVFD passes a file; a hangup poisons the file table; and `tsleep` blocks in `poll(2)` on a real host descriptor and wakes when the device speaks. `streams` 43 -> 111. Six authentic headers imported, five stand-ins written, fifteen names in four files named after V8's own. See S8a step 1 |
| 8a.2 fs switch | done | `shim/v8sys/vfs.c`, one mount table, passthrough behind it |
| 8a.3 `/proc` | done | `ls /proc`, `PIOCGETPR`, the u-area at `UBASE`; `ps` runs |
| 8a.4 `mkfs` | **done** | `mkfs` writes a real free-list/1024 V8 filesystem and **all ten of the "raw VAX disk" programs run** — `mkfs icheck dcheck clri fsck ncheck quot dump restor dumpdir`, none of which needed a mount, because each takes its subject as an argument. The round trip closes: dump → tape → restor → a second filesystem the other five pronounce clean. `mkfs` 146/146. It began by finding that **every on-disk struct in the tree was the wrong size** and ended by finding that **an `int` never wrapped at 32 bits** — plus, on the way, two of this port's own `time_t`-seam bugs in both directions, three of upstream's address-0 assumptions, and one in our `doprnt` |

| 8a.5 v8fs | **the mount works, is probed, and WRITES** | V8's own filesystem code RUNS. Step 5c reads: `mkfs(8)` writes an image and Bell Labs' kernel walks `namei → fsnami → dsearch → iget → bmap → readi → bread` to a driver, with `cmp` confirming 28000 bytes two directories down and 28 blocks long — so a subdirectory and `bmap`'s **indirect** arm. Step 5d writes: `writei`, `bmap`'s **allocating** arm, `alloc()`/`free()` including the **free-list chain**, `ialloc()`/`ifree()`, `itrunc`, and `namei` with `NI_CREAT`/`NI_DEL` — a file created by name, grown past the superblock's cached free list, deleted, and the accounting exactly restored. The acceptance test is **icheck, dcheck and fsck**, three programs that know nothing about the probe. `streams` 111 → 372. Six imported files, one new stand-in (`shim/kern/sys/main.c`, for `sys/main.c` + `machdep.c` + `param.c`). **No MOUNT, and step 5e costed it and found it needs the SERVER**: a fourth type in `vfs.c` would have to link `libv8kern` into every client, and that is **56 symbol collisions over 29 programs, 25 of them silent** — `cat`'s `char buf[4096]` becomes an eight-byte pointer and the program exits 0 having overwritten the buffer cache's own pointers. Independently, `vfs.c:167` had already recorded that the descriptor table does not survive `exec`, so `> /mnt/f` could never work in the client. Both roads lead to the 9P server this section opens with. **Step 5e then BUILT it**: `shim/v8fsd/v8fsd.c` is a host binary that links `libv8kern`, holds an image open and answers 9P2000 on a Unix socket — Tversion/Tattach/Twalk/Topen/Tread/Tclunk/Tstat/Tflush, with the write half answering `EROFS` until step 5f. `tests/streams/p9probe.c` walks to a file two directories down, reads 28000 bytes over the wire and `cmp` says they are the ones that went in; `streams` 372 → 412. Three things the costing did not predict: **a driver set belongs to a CONFIGURATION rather than to the kernel library** (putting `imgdev.o` in the archive made it import `_pread`/`_pwrite` and broke the externals guard, so it goes on the link line, and the probe and the server now share one driver); **one process suffices only because nothing sleeps** (the synchronous driver means `iowait` at `bio.c:426` finds `B_DONE` set, so a request completes between two `poll()` returns); and **the connection deadline must apply only mid-message**, since a connection here IS an open file and may idle as long as the program holds it. **And step 5e then closed with the CLIENT**, `shim/v8sys/p9cl.c`, a fourth type in the switch: with `V8MOUNT=/mnt=sock`, `rootfs/bin/cat /mnt/sub/deep` returns 28000 bytes byte-identical to what `mkfs` was handed, and `ls`, `tail`, `wc`, `grep` and `sh` redirection all work unmodified. `streams` 412 → 449. The design is one sentence — **the connection IS the open file description** — and everything follows: one `connect()` per `open(2)`, so the object Unix shares across `dup` and `fork` and carries through a program replacing its image has an exact counterpart on the wire; the fid is therefore a **constant**; the offset lives **on the server**; and the client holds no per-descriptor state for a regular file at all, which is why an inherited descriptor works. That exposed **the one thing 9P has no message for**: 9P is a pread protocol because Plan 9's *kernel* held the offset, and there is no kernel here — so a fid has a cursor, `P9_OFFCUR` uses and advances it, and `Tseek`/`Rseek` (numbered outside 100..127) read and set it. `tail(1)`, a 1985 program, exercises it. Three bugs no probe could have found: **a `Tclunk` in `t_close`** destroyed the fid every `dup` shares, so `cat < /mnt/hello` printed nothing — a clunk is not `close(2)`, it is the *last* close, and the right number is zero; **a directory read carries BARE stat structures** while only `Rstat` has the outer count, which presented as `ls: /mnt unreadable`; and **a directory's `st_size` is the snapshot's length**, `dir.c:114`'s rule needing to be applied a second time in a second filesystem. Descriptor identification moved from the process table to **`getpeername(2)`**, because the table dies with the image and a raw `read` on a 9P socket *hangs* rather than answering wrong. The eleven slotless entry points now refuse a mounted path with `EROFS` rather than reaching the host — `rootpath()` no longer prepends `$V8ROOT` for a mount, so without the guard `rm /mnt/x` would ask the Mac. `access()` is implemented over `t_stat`; `readlink` is `EINVAL`; **`chdir` is the one genuine gap**, since nothing tracks a working directory. **A SECOND PASS THEN BUILT THE PROBE THOSE CASES COULD NOT BE**: the shipped binaries are the right headline claim and they reach neither `fstat` on a directory descriptor, nor `lseek` in three whences, nor `dup` sharing one offset. `tests/streams/p9clprobe.c` is the shim's own sources in a host binary (the `tests/v8sys/test.c` shape, built by the Makefile so `$(SHIM_SRC)` is not spelled twice), and it found four things beyond the three it was written for. The **vacuous** guard has an observable after all -- `stat` and `fstat` on a directory *deliberately disagree*, and nothing had asked both; the pair recorded in the comment (64/768) was arithmetically impossible and described no directory, so the case asserts a **ratio** over two directories instead. `p9walk` answered **ENOENT where V7 answers ENOTDIR**, because a short Rwalk carries no errno -- reconstructed from the qids it was discarding, and guarded by three cases, the middle one being the discriminator a one-sided fix would pass. A **transport was leaking its signal semantics into the filesystem**: a server that died raised SIGPIPE and *killed* `cat` where a V8 disk gives `EIO`, fixed with per-socket `SO_NOSIGPIPE` (never `SIG_IGN`, which would stop `yes | head` terminating) and tested with `shutdown(2)` on the client's own descriptor. And a mutation that **would not fire for a third reason** -- `do_seek`'s overflow guard is about undefined behaviour that happens to give the right answer, invisible to any behavioural test -- so the suite now runs its traffic through a **UBSan build of the server**, silent today and fatal with the guard reverted. `streams` 449 -> 535; nine mutations, nine fire. **AND STEP 5f MADE IT WRITABLE**, which began by re-measuring the claim that it was not: `readi` sets `IACC`, so `iput` ran `IUPDAT` and dirtied the disk inode on every read, and "read only" had described the protocol rather than the filesystem. The recorded finding named two accidents hiding it and an instrumented driver corrected both -- `O_RDONLY` was doing **no work at all** (O_RDWR with no flush is zero pwrites across twenty-two reads), and the third accident nobody had named is that **the clock is frozen**: `time` is set once from `s_time` and `mkfs` stamps every inode with that number, so the write stores the bytes already there and no comparison of the image can see it. Perturb one `di_atime` and exactly four bytes move. So 5f is four things: the clock advances (`v8fs_clock()`, a raw `gettimeofday` because `libv8kern` may name no libc function); read-only becomes a MOUNT FLAG with Bell Labs' own two lines from `fsmount()` (`d_open(dev, !ronly)` and `s_ronly = ronly & 1`), so `v8fsd -r` stops even an atime; `update()` runs once per poll wakeup, which is `sync(2)`'s body; and then Twrite, Tcreate with `DMDIR`, Tremove, Twstat and **`Taccess`, a second extension for the same reason as `Tseek`** -- 9P has no `access(2)` because Plan 9 has none, and the client had been wrong about permission in both directions available to it. `sh -c 'echo x > /mnt/fresh'` now creates a file on a disk image through `namei`/`ialloc`/`writei` in another process, `mkdir`/`rm`/`rmdir`/`cat a > b` work, and the three checkers say clean with the block count back to exactly where it started. Four defects were found by RUNNING it rather than reading it: `iupdat(ip, &time, &time, 1)` transcribed verbatim passes **the address of libc's `time()`** in a file that includes `hostok.h`; `itrunc` already zeroes `i_size` and clears the flags on purpose, so a draft that repeated both undid upstream's crash-consistency reasoning; `do_remove` took the ROOT as the parent, so `rm /mnt/d/f` unlinked from the wrong directory; and the target must be `iput` before the remove or every deleted file leaks its blocks. `streams` 535 -> 567; eight mutations, eight fire -- the one that would not fire needed a case that writes TWICE through one descriptor, since `echo` writes once. **AND STEP 5f-b GAVE chmod, chown AND utime THEIR SLOTS**, which are one `Twstat` between them, leaving only `link` and `symlink` refusing. Reading `sys4.c` and `fio.c` first found three deviations in the server's existing arms and one missing one, all invisible because `u_uid` is 0: `chmod` and `utime` gate on `owner(1)` -- ownership OR superuser -- where the code tested `suser()` alone **while the comment above it claimed ownership** and cited `sys3.c`, which is `fsmount`; upstream strips ISVTX for a non-root `chmod`; upstream's `utime` sets `IACC|IUPD|ICHG` and the arm set two. The missing arm is `s_atime`, declined earlier because "nothing in this world sets atime alone" -- true, and it never covered **`mv`, which sets both**: `mv.c:129` is `utime(target, &s1.st_atime)`, taking one `struct stat` field's address and relying on adjacency for a `time_t[2]`, and on a mount it is the ONLY path because `link(2)` is refused, so mv always forks `/bin/cp` and then utimes the copy. Declining an arm because nothing sets a field *alone* is a different claim from nothing setting it at all. The auditor's best find was **the two ends of one wire an hour apart**: `do_wstat` parsed the owner with `atoi`, which has no error return, so `"nobody"` and `"--"` both set the inode's uid to **0, root** -- the identity `fio.c:193` lets bypass every check -- and got an Rwstat back, while the *reading* end of the same field had had the guard since an earlier audit with the contract spelled out beside it. Range is not parseability, and only the second is guarded: `"65536"` truncates because `sys4.c:294` is `ip->i_uid = uap->uid` unchecked. Nothing could have caught it -- `do_wstat` had no client caller until this step -- so the case is a new `-w` mode in `p9probe`, since no V8 program can send a name. Also: `p9_pstatw()` and `p9_nostat()` exist because a second hand-rolled copy of 9P's double-length patch is how two ends of a protocol drift by two bytes; four dead declarations shared a line with two live ones directly under a paragraph saying "a declaration with no call site is an unconsumed component", and the one that mattered was `iinit`, which **5f itself had changed** to `void iinit(int)` while the declaration still said `int iinit()`. `streams` 567 -> 592; eight mutations, eight fire, and one case was found VACUOUS FOR TWO INDEPENDENT REASONS -- `ls` renders an unknown file type as `-`, and `p9tostat` rebuilds the type from `DMDIR` rather than passing the server's IFMT through,  so the guard had to move to a directory. **AND STEP 5g GAVE link A SLOT, WHICH LEAVES ONLY symlink REFUSING** -- and the reason it had been refused was the port's own argument for the opposite. syscall.c and vfs.h both said "Neither is deferred work: 9P2000 has no message for either", which is EXACTLY the situation that produced Tseek/Rseek and Taccess/Raccess: twice the answer to a missing message was to add one, and the third time the same fact was written down as grounds for declining. Nor were the two a pair -- a V7 filesystem cannot REPRESENT a symlink (permanent, and readlink already answers EINVAL on it) and IS BUILT ON hard links, with `nami.c:484`'s NI_LINK arm already sitting in the imported tree, unreachable only because nothing sent it a request. It read as cosmetic because the loudest consumer degrades quietly: `mv` of a FILE falls back to fork-and-cp and exits 0, while `mv` of a DIRECTORY has **no fallback at all** (`mv.c:204`) and printed "cannot link" -- measured against a server that had accepted `echo > /mnt/f` and `mkdir /mnt/d` seconds earlier, where `ln` answered **"Read-only file system"**. Tlink is 132/133; symlink keeps its refusal and loses its word (EPERM, `v8s_mknod`'s device-arm distinction applied one line along), and cross-type link is **EXDEV**, an answer rather than a refusal. **AND ADDING link EXPOSED THAT `unlink(2)` OF A DIRECTORY HAD BEEN THE WRONG SYSCALL ALL ALONG**: 9P has one remove and V7 has two calls, Plan 9 having no `rmdir(2)`, so v8fsd decided from the inode -- invisible until link existed, because every removable directory had `i_nlink == 2` and NI_DEL and NI_RMDIR differ only above that. Three disagreements at once: the errno (`nami.c:363` EBUSY where V7 succeeds), the on-disk result (NI_RMDIR zeroes `i_nlink` and frees the inode where NI_DEL decrements), and **the failure path corrupting the parent** -- `nami.c:361` decrements before the EBUSY test two lines later and does not put it back, measured with dcheck as root having 3 entries and a link count of 2. That last is upstream's own bug and is deliberately NOT fixed. Tunlink (134/135) is V7's unlink; the reconciliation is one line, forced by a fiction the port had already told (`dotlink()` absorbs rmdir(1)'s first two unlinks, so the third must do the whole job), and it asks the only question the two cases differ on: does another name reach this directory? **Nothing but fsck could see the regression** -- the listing was right and icheck and dcheck were both silent. Two of nine mutations would not fire and both said the same thing, that the guard was upstream's already: a duplicate ENOTDIR, and an EINVAL for `.`/`..` **whose stated reason was false** (`nami.c:88-95` returns EEXIST for NI_LINK the moment the name is found, and `..` always is -- so the guard was less faithful than no guard). And the errno tables were missing **seven** names that the guard on them structurally could not see: it compares the two tables to EACH OTHER and they agreed perfectly about a set that was too small, so EBUSY EFAULT EINTR ELOOP ENODEV ENOTTY EXDEV all reached the client as EIO. The fix is a third source that is neither table -- a sweep of `u.u_error = E...` over the imported kernel, derived every run. `streams` 592 -> 636; nine mutations, seven fire and the two that do not are findings. **AND THE STEP'S OWN CITATIONS WENT STALE WHILE IT WAS BEING WRITTEN**, which is why the tree-wide sweep exists: the commit that corrected do_walk's ENOTDIR citation wrote a number for an arm that had already moved two lines, so five of the nineteen stale citations found afterwards were fixes from the previous day. `tests/streams/cites.awk` checks all 1132 of them -- 286 uniquely resolvable, 846 excluded and the excluded count printed -- on the one property that needs no guess about what a citation means: nobody cites a blank line, a `*/`, or a bare `break;`. `streams` 636 -> 638. **AND STEP 5h MADE `mv` OF A DIRECTORY ACROSS DIRECTORIES ACTUALLY WORK, by retracting 5g's retraction.** 5g had shipped `isdir = i_nlink <= 2` for Tunlink and explained it as reconciling V7's unlink with a fiction the port already told; every clause was true and the conclusion was not, because what it described is a **COMPENSATING ERROR** -- one workaround split across two files -- and read it as a design. The real defect was the message shape: `Tunlink` carried `fid[4]`, copied from Tremove, which imports Plan 9's noun into a V7 verb. `remove(2)` names a FILE; `unlink(2)` names a DIRECTORY and an ENTRY, and `..` is where they separate -- an entry that exists only from the directory's side, so **no fid can name it**, which is exactly why `v8fsd` zeroes `f_pino` for a fid walked to `sub/..`. The diagnosis was already sitting in `do_remove`'s own header comment ("a fid names a FILE, V7's unlink names a DIRECTORY and an ENTRY, and nothing in a struct inode bridges the two") and had gone a whole step without being read as one. Tunlink is `dfid[4] name[s]` now, matching Tlink; `do_unlink` is a separate function that infers nothing, holds no target inode, and therefore needs none of removeop's three hazard guards -- all of which were properties of carrying a fid rather than of removing a name. With the client's `dotlink()` absorption dropped **for a mount only** (macOS still cannot perform the two dot unlinks, so the passthrough half is unchanged), `rmdir(1)`'s three calls run V7's own arithmetic and Tunlink is ALWAYS NI_DEL -- the sentence `p9.h` claimed originally, restored by removing the thing that made it false. The two halves had to go together: keep the heuristic once the `..` unlink lands and `nami.c:361` decrements the parent a SECOND time. Measured -- mutation M4 restores the absorption alone and **fsck alone goes red**, with icheck, dcheck and the block-count identity all silent, a third instance of the independent-reader rule. Two more findings: `v8s_link`'s dot arm could not tell `mkdir(1)` from `mv(1)` because it tested the wrong operand -- both pass a directory as `a`, and what separates them is whether the target ENTRY exists (mkdir's does, mv's was unlinked one line earlier at `mv.c:216`) -- so the predicate moved to `b` and the change is **monotone**, the same discipline as rootpath()'s access-to-lstat fix; and `p9parent()` had one caller's policy baked into a shared helper, refusing `.`/`..` unconditionally, which was right for create and a hard limit on link and unlink. A case got STRONGER by losing a guard: `dotdot as a new name` asserted 22 (EINVAL, from our client) and its own comment already predicted the wire answer, so letting `..` through made it assert **17, EEXIST from Bell Labs** -- the same shape as 5g's two deleted `do_link` checks. Only `symlink` still refuses, permanently. `streams` 638 -> 639 (four cases inverted, one expected value corrected, and one added for a defect the step found in the sweep itself); four mutations, four fire, and M2 fires on **exactly one case**. AND THE CITATION SWEEP COULD NOT SURVIVE ITS OWN MOTIVATING INPUT: a file that cites ITSELF spun it forever, because awk keys `getline` on the FILENAME so cite() opening the file scanfile is mid-read shares its stream and close() rewinds the outer loop. Reproduced in three lines; fixed by loading each file once into a cache, which also took the sweep from minutes to 0.45s since it had been re-reading each target once PER CITATION. Reverting the whole fix emits 16MB of spurious STALE lines in five seconds, so the capture is head-bounded too. See S8a step 5 |

`make test` runs everything — seventeen suites, **2140 cases**.

That number said **1767** for long enough to be worth a note rather than a
correction. It is not a count anybody maintains: it is printed by the suites on
every run and transcribed here by hand, so it decays in one direction only —
downwards, because work only ever adds cases — and a stale one reads as
plausible forever.

**And the first version of this note drew the wrong lesson from it, within the
hour.** It said CLAUDE.md's copy had stayed current at 2108, and concluded that
*"two hand-transcribed copies of one measurement will not go stale together"*.
Then a grep found a **third** copy — `ARTICLE.md:30`, "1767 tests across 17
suites" — stale at exactly the same number. Two of the three had gone stale
together, which is the thing the sentence said could not happen.

The real rule is duller and more useful: **copies written at the same moment
decay together, and only a copy that is independently maintained can disagree.**
CLAUDE.md stayed current because it is the file loaded into every session, not
because three copies police each other. So disagreement is a *sufficient*
signal and never a necessary one, and the only reliable answer is to re-derive:

```bash
make test 2>&1 | awk -F'[ ,]+' '/passed, [0-9]+ failed/{p+=$2; f+=$4; n++} END{print n" suites, "p" passed, "f" failed"}'
```

### What actually works today

V8's own preprocessor and compiler, with a new ARM64 back end, driven by V8's
own `cc`, produce object code that assembles, links and runs correctly on Apple
Silicon. Everything above the code generator is untouched 1985 Bell Labs source.
As of S8a step 4 that includes a program whose *output* is a 1985 artifact
rather than a 2026 one: `mkfs` writes a filesystem a VAX could mount.

### The lesson this port keeps teaching

Every back-end bug found so far has lived in a *combination* of features that
real code uses and unit tests do not — register-variable clobbering, lvalues
evaluated twice, the conditional join, `sp` moved per call, and now narrow
return values at the clang seam. 62 synthetic tests passed while the first real
libc function died; 163 passed while the first syscall error check was broken.
So the suites lead with authentic V8 sources, and a new synthetic test is only
worth writing once a real program has shown which combination to test.

The second lesson, earned four wrong hypotheses deep into the malloc bug: stop
reasoning about what a value should be and make the program print what it is.
`V8DBG=1` type tracing settled that one in seconds, and instrumenting `cat`'s
decision points settled the diagnostic bug the same way.

### The third lesson: a stale object does not look like a build problem

Four separate debugging rounds in this port were spent on code that was
correct, compiled from sources that had already been fixed. A stale object
presents as *the code being wrong*, which is why it costs so much: it sends you
into the source, and the source looks fine, so you go deeper.

The worst was lex. `once.c` widened `left[]` and `right[]` to `long`;
`parser.y` allocates them with `sizeof(*left)`. A `y.tab.o` built before the
widening allocates 1700x4 bytes for arrays written as 8-byte longs — a 2x
overrun straight through the neighbouring block's malloc header. It presented
as "calloc returns 0", and the search for it was unbounded until one
measurement (log what malloc *wrote*, compare with what was *found there
later*) proved the allocator innocent.

`make` was audited end to end afterwards rather than patched again. Five
distinct mechanisms were found, four of them real defects:

1. **Order-only prerequisites misread as dependencies.** Every rule that
   compiled with v8cc said `| rootfs`. Order-only means "exist before me", not
   "I depend on you", so editing `cc.c` or a header in `src/include` rebuilt
   nothing that used them. Measured: `touch src/cmd/cc.c` changed the rebuild
   set by exactly zero objects.
2. **Phony targets as normal prerequisites.** The yacc and lex generator rules
   named `v8yacc`/`v8lex`, which are phony and therefore always out of date, so
   all of eqn, all of pic and lex's `y.tab.o` — 39 objects — recompiled on
   every `make`. That is the same bug as (1): one misuse of `rootfs` produced a
   permanent overbuild in one place and a total underbuild in another. It also
   *caused* the lex bug, by making builds slow enough that hand-copying
   `y.tab.o` felt like a reasonable shortcut.
3. **`#include`d files that are not headers.** `lex/ldefs.c`, `lex/once.c`,
   `refer/refer..c`, `tbl/t..c`, `yacc/dextern`, `yacc/files` — invisible both
   to a header scanner and to a `*.c` glob. Only tbl's and lex's were declared.
4. **Directory stamps standing in for installed files.** A stamp records "the
   install ran", which is not the question: deleting one installed terminal
   table left the stamp alone, so `make` never restored it.
5. **Make's one-second timestamp granularity** (GNU make 3.81, what macOS
   ships). APFS records nanoseconds; make compares whole seconds. A file edited
   in the same second as the build that consumed it is silently missed. Nothing
   in the rules can fix this — it is make's comparison, not the graph — but a
   dependency test that does not sleep past a second boundary will pass
   vacuously.

The build now expresses what it actually reads: `$(V8CC_DEPS)` — driver,
`cpp`, `ccom` and the header tree — is a normal prerequisite of every object
compiled by v8cc, and the generators are named as the files they are. Clean
build 12s serial, 4s at `-j8` (parallel was previously racy: a two-target rule
is two rules sharing a recipe under make 3.81, not a grouped target).

`tests/deps` makes this a tested property rather than a remembered one. It uses
`make -q` to ask "would this be remade?" without compiling anything, asserts
each target is up to date *before* the touch — so a rule that is always stale
cannot pass for the wrong reason — and restores mtimes with `cp -p`. It was
verified by mutation: removing the `refer..c` dependency, reverting the driver
dependency, and restoring the phony generator prerequisite each fail it.

The generalisation: **the build must express dependencies rather than rely on
invocation order.** Every one of the four incidents was closed structurally,
not by remembering to run something first.

### The LP64 seam, which is the deepest structural decision here

V8 assumes `sizeof(int) == sizeof(char *)`. It is not an incidental assumption —
the tree calls `malloc` without declaring it and casts the `int` result to a
pointer, which was free on a VAX and is fatal under LP64. The target model is
LP64 by deliberate choice (`macdefs.h`), so the consequences have to be paid
somewhere, and where they are paid is a real decision:

* **Not by narrowing at call sites.** Tried, and it breaks `opendir` outright:
  the compiler cannot distinguish a declared `int` return from K&R's implicit
  one, because they are the same node.
* **Not by patching every call site in 290 programs.** Correct C, and what a
  real 64-bit port does, but it would put a diff on most of the tree and the
  fidelity contract is the point of the project.
* **At the seam**, which is where it now lives. Everything clang-compiled that
  V8 calls by name returns a full 64-bit value (`shim/v8sys/stubs.c`), so V8's
  "a value in a register is as wide as a register" assumption holds inside the
  V8 world, and the shim absorbs the difference. That is exactly the shim's job.

The same reasoning settled two more:

* **String literals go in writable data**, as V8's own VAX back end put them,
  because 1985 C had no `const` and the tree writes to them.
* **A pointer converted to `int` keeps all its bits** (`PTRCONVFULL`,
  `common/optim.c`). What made this one hard is that the damage is normally
  *symmetric* — pass 1 truncated pointers in memory and pointers in registers
  by different routes, so an expression with a pointer on each side still came
  out right. It only broke where the two sides took different paths, and
  fixing one side alone silently broke the other.

The one place the seam could not absorb it is **`DIRSIZ`, now 254 rather than
14** (`src/include/dir.h`). A V7 directory record cannot name most of a real
macOS filesystem, and truncated components are not a degraded experience but a
broken one: `pwd` failed in any directory with a long component above it. The
tree names that size symbolically, so one header does it.

### The LP64 hazards found so far, as a class

Each is the same mistake — 1985 code assuming `sizeof(int) == sizeof(char *)` —
surfacing somewhere different, and each was invisible until real code ran:

| Where | Symptom |
|---|---|
| `malloc` called undeclared | pointer truncated on return; `opendir` segfaulted |
| syscall returning `int` | `-1` tested as positive; every error check silently failed |
| implicit-`int` parameter holding a pointer | `look` wrote through a truncated address |
| pointer difference with one operand in a register | `strspn` returned -2^32; `strtok` walked off the end |
| `opbigsz` narrowing a pointer AND | `malloc` walked half a pointer |
| `PTRTYPE` defaulting to `INT` | pointer arithmetic done at 32 bits |
| `(&m0)[i]` over K&R parameters | argument slots are 8 bytes here and 4 there; `mkfs` could not make a filesystem |
| `long` fields in an on-disk struct | `dinode` 80 not 64, `filsys` 7960 not 4096; every image would have been unreadable |
| `ltol3`'s stride, after `daddr_t` narrowed | block lists decimated, six addresses read past a stack struct |

None of them is a bug in what Bell Labs wrote. All of them are the port's to
absorb, and the rule that has held is: **fix it where the width is decided —
the target model, the seam, or the one conversion routine — never per program.**

The last three, added at S8a step 4, extend the rule rather than restate it.
The first two rows are the same mistake as the rest and were found the same way,
by running real code. The third is different in kind and is the one to
remember: **narrowing a type at the place where the width is decided is exactly
the prescribed fix, and it still broke a caller** — `ltol3` had been patched for
an eight-byte `daddr_t` and was right when written. So "fix it where the width
is decided" carries an obligation with it: sweep for what already encodes the
old width. `grep -rn '<type>' src shim` cost one command and would have found
it, and did, once someone looked.

### Deliberate gaps in the back end

Switches are linear compare chains (correct, but dense switches in troff and
the shell want a jump table). Debug symbols are stubbed; when they arrive they
should be DWARF through the host assembler, not VAX stabs. The ELF/Linux path
is written but untested.

## 4j. The crash probe, and three ways a crash probe lies to you

The address-0 sweep (S4i) was static: subagents reading source, plus greps. Its
empirical counterpart is `tests/crash-probe.sh` -- run every installed Mach-O
binary bare and then with each of the 52 single-letter options as its **last**
argument, under a deadline, and report anything that dies on a signal.
Read-only programs only; anything that creates, moves, removes or formats is
excluded by name, which is why `fsck -t` could never have been found this way
and the static audit is the sole coverage for that set.

**The answer is 96 signal deaths in 4134 invocations, across six programs.**
Getting to that number took four runs, because the first three were wrong in
three different ways -- and every one of them inflated the count, which is the
direction that wastes the most time.

### The three ways it lied

**It was not hermetic.** Every invocation shared one working directory, so
programs read each other's litter: `yacc` leaves `(null).tab.c`, and several
write a file named after the option they were handed. `dcheck` then appeared to
crash on 45 different options -- because its loop calls `check(*argv)` for
**every** argument including options, so `dcheck -Q` does `open("-Q")`, and with
a zero-length file of that name sitting there it read a superblock out of it and
walked off a garbage `s_isize`. A real dcheck fault, provoked by the probe
rather than by the option. A crash prober has to be a pure function of the
program and its arguments, or its findings are a function of iteration order.
Fixed with a fresh directory per invocation.

**The shell cannot tell a signal from an exit status.** `$?` is 128+N when a
child dies of signal N, but a program may exit(134) of its own accord -- and a
V8 program whose `main()` falls off the end returns whatever was in the
register. `primes` does exactly that, and 42 of its 53 garbage statuses landed
in 129..159 and were reported as SIGABRT. The child now runs under perl, which
keeps the real wait status and can ask `$? & 127`.

**And the first diagnosis of the first problem was wrong.** The `dcheck` entries
were signals 9 and 10, in one program and no other, which reads exactly like a
concurrent rebuild replacing a Mach-O mid-execution -- so that is what it was
recorded as, and a filter was added to discard SIGKILL *and SIGBUS* as
contamination. That filter would have hidden 48 genuine crashes. SIGKILL is
never a program bug and is still discarded; SIGBUS very much can be one.

The lesson is not any of the three individually. It is that **an ad-hoc crash
prober is a measuring instrument, and this one was wrong three times while
looking authoritative each time** -- 254, then 195, then 148, then 96. Validate
it against a known crasher and a known-clean program before believing a number
from it, which is what the run against `lex`, `primes` and `echo` now does.

### What the six are, and they are not one class

| program | count | what it is |
|---|---|---|
| `lex` | 53 | **partly fixed, and "one root cause" was wrong** -- it is three faults with three different answers, 40 + 11 + 2. The 40 are `warning()`'s unconditional `fflush(fout)` and ARE ours, with a measured VAX answer; the 11 and the 2 are the empty-spec path, which crashed a VAX too. See below |
| `nroff` | 38 | **fixed** -- only *unrecognised* options, bare `nroff` fine. One root cause, and the mechanism is address-0 after all: the `default:` arm calls `done(02)`, nroff's NORMAL shutdown, which reaches `done3()` -> `twdone()` -> `oputs(t.twrest)` before `ptinit()` has read the terminal table, so the field is still null. Guarded in `oputs`, which is the single point the four table fields share |
| `pr` | 2 | **fixed** -- and the diagnosis that stood here was wrong. It said "`-m` sets `Ncols = eargc`, which is 0"; `Ncols` is 0 for about four lines, and `findopt`'s own `switch (Ncols) { case 0: Ncols = 1; }` puts it back before returning. The real cause is an **uninitialised `FILE *`**, not a zero count, and it is a different class from every other row -- see below |
| `sed` | 1 | **fixed** -- `-e` with no script. `eargc-- <= 0` where the loop's `--eargc` has already taken one off, so eargc 1 means the option and nothing after; rline()'s copy loop then walks the terminator. The `-f` arm had the identical off-by-one and did *not* crash, surviving only on `rootpath()` handing a NULL path to the kernel and `doprnt` printing "(null)" -- fixed with it, because a walk off the end that lands on a soft floor is still a walk off the end |
| `ps` | 1 | **fixed** -- and "a wild pointer (0x53c5c), so not the address-0 class" was right about the class and wrong about the pointer. `0x53c5c` is the *low half* of `0x100053c5c`, which is `ctime`'s static `cbuf` plus 4. `ps.h` is the one file in the tree that calls `ctime()` undeclared, so `+4` is int arithmetic and `arm64_trunc()` sign-extends it. Fixed by declaring it; see below |
| `bcd` | 1 | bare. `while ((c=getchar())!='\0' && c!='\n')` never tests EOF, so redirected empty stdin spins and overruns a fixed buffer -- **a VAX would have overrun it too**, so upstream's defect and not automatically ours to patch under S1 |

**`nroff`'s is worth carrying forward**, because it is `yacc -o`'s shape from
S4i: an error path that reuses the normal shutdown path runs before
initialisation. Two independent instances now, and both were found the same
way -- by asking for a backtrace rather than by reading the source, which is
what turned nroff's from a plausible theory ("done() runs before init1()") into
the actual faulting instruction (`ldrsb x9, [x9]` in `oputs`, x9 zero).

### Two were fixed, and one of them is the argument for the whole exercise

`ptx -w` and `ptx -g` were S4i's class exactly: the guard reads `argc >= 2`
while the loop above tests `argc>1`, so argc still counts the program name and
two arguments means ptx and the option with nothing after. The guard is left as
upstream wrote it and the *read* is what changed, so `ptx -w` still reaches the
`Wrong width:` complaint a VAX printed instead of being silently accepted.

**`inv` is the one that matters.** `inv1.c:32` was `while (argv[1][0] == '-')`
with no argc guard -- `hunt1.c:40`'s bug, character for character, in the same
directory, four files away. The static sweep read `hunt` and fixed it and did
not find `inv`. Its author had even guarded the *later* use of the same pointer,
line 61's `argc >= 2 ? argv[1] : "Index"`, and not the loop. 52 of inv's 53
invocations died on it. **A static audit and an empirical probe find different
things, and this is the case that proves it** -- no amount of re-reading
`hunt1.c` would have turned up `inv1.c`.

### `pr -m` is a third class, and the manual is what settles it

Not address-0 and not an argv overrun: **an uninitialised `FILE *`**. `main`
declares `FILS fstr[NFILES]` as an *auto* array and points the global `Files`
at it. In `-m` mode main opens the operands itself, so `print()` guards with
`Multi != 'm'` to avoid reopening `Files[0]` over the top of the first one.
That test is a **proxy** for "our inputs are already open", and it is right
whenever `-m` actually got files. With no operands main's loop never runs,
nothing is opened, `nfdone` is 0, and the `/* no files named, use stdin */`
line calls `print(nulls)` -- where the proxy still says "already open".

Measured at the fault rather than reasoned about, and the numbers are the
point: `Files[0].f_f` is **`0x8`** and `f_nextc` is `0x30`. So `get()` does not
take its EOF arm at all; it falls to `q->f_nextc = getc(q->f_f)` and reads
`_cnt` through a `FILE *` of 8.

**The old note here said "offset 8 of a null `FILE *`", and that is the wrong
split of the same number.** `_cnt` is the *first* member of `struct _iobuf`, so
its offset is 0 and the pointer is 8 -- but a fault address of `0x8` is
consistent with `NULL + 8` and with `8 + 0`, and **nothing in the backtrace
distinguishes them**. Reading the struct definition is what does. Worth
carrying: a faulting address is `base + offset` and the address alone never
tells you which is which, so a "null pointer, big offset" reading is a
*hypothesis* until the layout is checked.

Fixed by spelling the intent directly -- `Nfiles == 0` -- which agrees with the
proxy everywhere else, because `Nfiles` is incremented only in main's `-m` arm
and is therefore 0 for every non-multi call.

**There is no VAX answer to restore, and that is what makes this one
different.** `quot` and `ncheck` had one: address 0 held a known byte (`0x00`,
see S4i), so the VAX *computed* something and the fix reproduces it. An
uninitialised `FILE *` is
arbitrary on any hardware -- here it came from whatever ran before `main`,
which under Mach-O includes dyld and under a V8 a.out was only `crt0`. So the
authority is `pr.1`, which states the rule with no exception for `-m`:

> For no file arguments, or for a file argument `-`, *pr* prints its standard
> input.

The defect is upstream's and the SIGSEGV is this target's. Where the VAX's
behaviour is garbage rather than an answer, **the manual is the thing to
restore**; `bcd` is the same question answered the other way, because there the
manual does not help and the overrun happens on a VAX too.

`tests/wavea` has four cases, and the fourth is the one that earns its keep.
Mutating the fix back to upstream fails the three crash cases; mutating it to
the *plausible* wrong fix -- calling `mustopen` unconditionally -- passes all
three and fails only `pr -m still merges two named files into columns`, because
that reopens `Files[0]` with stdin and silently prints the wrong column.

### `ps -T`: the two compiler decisions contradict each other

`0x53c5c` reads as a wild pointer and is not one. It is the **low half** of
`0x100053c5c`, which is `ctime`'s static `cbuf` plus 4. `printp.c:24` is
`strcpy(sstr+4, ctime(&up->u_start)+4)`, and `ps.h` was the only file in this
tree calling `ctime()` undeclared -- V8's `<time.h>` declares no functions at
all -- so K&R gave it an implicit `int` return.

Two plausible causes were eliminated by measurement, and both would have been
this port's fault rather than upstream's. `u_start` is **not** a narrowed time
field: `sys/user.h` is unpatched and `time_t` is 8 bytes there. And the u-area
**is** populated: the offset v8cc computed from the authentic header (2744)
matches the shim's own `AT(u_start, 2744)` assertion, and the value at the
fault was a real 2026 timestamp.

The pointer is not lost at the call. `gencode.c` deliberately does not narrow a
signed-`int` CALL return, because in this tree `int` usually means "undeclared"
-- opendir's `malloc` is why. It is lost at the **arithmetic**: `+4` is a `PLUS`
of type `int`, and `arm64_trunc()` -- added earlier the same day, S4i -- emits
`sxtw` after it. Correct for an int, fatal for a pointer, and under Mach-O the
image loads at `0x100000000` so a truncated pointer is in `__PAGEZERO` every
time rather than occasionally.

**So two decisions in one file contradict, and both are wanted.** They cannot
both be satisfied there, because `int` from a declaration and `int` from a
guess are the same node -- which the CALL note already said. C's answer is that
int arithmetic wraps, so the caller must declare the function; that is the fix
`malloc` got one level up.

The blast radius was bounded by **measuring the emitted code** rather than
grepping sources, because whether a declaration was in scope is not a textual
property of a call site. All 97 installed binaries were disassembled and
scanned for `bl` -> `mov xN,x0` -> arithmetic -> `sxtw`: **64 sites, 63 of them
calling something that genuinely returns int** (`strlen`, `dysize`, `atoi`,
yacc's `apack`, troff's `width`/`roman`/`decml`/`abc`). This was the only one.
`tests/v8ccom` now pins both halves of the seam.

### `lex` is THREE bugs, and the probe cannot tell them apart

"One root cause" was wrong. Re-measured, the 53 split 40 + 11 + 2 across three
faults, and only the first is ours:

| n | invocations | fault | site |
|---|---|---|---|
| 40 | every letter outside `rRcCtTvVfFnN` | `fflush(fout)` | `sub1.c`, `warning()` |
| 11 | bare, and `-c -C -f -F -n -N -r -R -v -V` | `fprintf(fout, ...)` | `header.c:84`, `:93` |
| 2 | `-t -T` | `free(NULL)` | `lmain.c:158` -> `free2core` |

**A FOURTH WAY THE PROBE MISLEADS, and it is a limitation rather than a bug.**
It feeds every program `/dev/null`, so for a program that *requires* input all
53 invocations also reach the empty-spec path. Fixing the first fault therefore
changes the probe's count by **zero** -- the 40 now die further along, at the
second -- and the real gain is invisible to it. What was actually fixed is
`lex -a spec.l` on a specification `lex spec.l` compiles perfectly.

The 40 have a **measured** VAX answer, and getting it needed the correction in
S4i above: V8 binaries are ZMAGIC, so virtual 0 is crt0, and those bytes give
`_flag` `0xd050`. `fflush` opens `(iop->_flag&(_IONBF|_IOWRT))==_IOWRT`;
`0xd050 & 06` is 0, so the `&&` short-circuits before `_base` is read and
`fflush(NULL)` returned 0 having touched nothing. `if(fout) fflush(fout)`
restores exactly that, guarded at the caller as `quot` and `ncheck` were.

The 13 are upstream's. `fprintf(NULL, ...)` on a VAX got past the `_IONBF` test
and `_doprnt` wrote through `_ptr`, which those crt0 bytes make `0x08aed05e` --
145 MB, far past a 56 KB lex's break, so SEGFLT; `free(NULL)` gives
`0xFFFFFFF8`, VAX system space, PROTFLT. So they belong with `bcd` and
`ls.c:259`, not with `quot`. A one-line `if(sect == DEFSECTION) error(...)` at
`parser.y:676` would close both and is written down in `lex/PORTING.md` so the
option is known, but it restores no VAX behaviour and S1 says leave it.

### What is left

**Re-run after the three fixes: 4134 invocations, 54 signals, no taint** --
measured, not the arithmetic. 53 of them are `lex` and 1 is `bcd`.

Note what those 53 are *not*: they are not 53 bugs, and they are not the 13 that
remain unfixed. They are **all 53 invocations reaching the 11 + 2 empty-spec
faults**, because the probe supplies no specification, so the 40 that used to
die in `warning()` now fall through to `ctail()`. The count is the same on both
sides of a real fix -- which is the point of the paragraph above, and the reason
`tests/wavea` asserts `lex -a spec.l` rather than anything this number can see.

Both programs are upstream defects that crashed on upstream's hardware.
**96 -> 58 -> 57 -> 55 -> 54**, and the remaining number is not a to-do list.

### The skip list was a coverage hole, and it is CLOSED -- as a mode, not a note

`SKIP` in `crash-probe.sh` named ~40 programs that create, move, remove or
format things, and excluding them is why `fsck -t` could only ever have been
found by the static audit. They can be probed safely, because **the jail is
per-binary**: a V8 binary resolves every path inside `$V8ROOT`, so giving each
invocation a throwaway *copy* of the rootfs bounds anything it does.

It had been done once, by hand, and written down as a note -- and that is the
part that failed. **The count in the note drifted from seventeen to eighteen**,
because the `v8` launcher was installed afterwards and a transcribed set cannot
notice. So it is now a mode: `PROBE=mutating`, each invocation in its own
`cp -ac` clone (**0.146 s** for 15 MB, measured, not the 0.3 s guessed here).
18 programs, 954 invocations, **zero** signal deaths -- and containment proved
by hashing the real rootfs before and after rather than asserted.

The one blob became **two lists, because the reasons are opposite**. `UNSAFE`
escapes the jail, so no throwaway rootfs contains it: `halt reboot shutdown
init sync mount umount` act on the **host**, `kill` signals host pids, `adb`
wants ptrace on them, `su login passwd cron at mail write` are interactive or
touch system state, and `as ld ar` are the host's by S1. `MUTATES` only changes
things inside the jail. Only the first is a permanent exclusion.

**And the same pass found the scan list itself was wrong**, in the same
direction as the `/etc` omission it had already been fixed for.
`$ROOT/usr/lib/spell/*` treats spell as a *directory* by analogy with `refer`;
`/usr/lib/spell` is a Mach-O **file**, V8's spellprog. It matched nothing, so
spell was never probed, and `/usr/lib/man` was never named. The population is
derived now -- `find` over the installed directories, filtered to Mach-O --
verified to be the old set plus exactly those two (95 -> 97) with **every**
Mach-O in the rootfs reachable, and the script prints what it covered.

## 4k. Why the data model is LP64, settled by counting

Raised as a direct question: should this be a native 64-bit port with a 64-bit
`int`, rather than one that keeps emulating 32-bit `int` semantics? The
instinct behind it is sound, and the answer turned out to be a counting
argument rather than a preference.

### What ILP64 would have bought, fairly stated

V8 was written when `sizeof(int) == sizeof(long) == sizeof(char *)`. LP64 is
the configuration that *breaks* that assumption, and this port's **dominant**
bug class is the consequence: K&R code storing a pointer in an undeclared
`int`. Under ILP64 a whole family disappears rather than being patched —
`char *p = malloc(n)` with `malloc` undeclared, the `acctype()` widening in
`gencode.c` and the `arm64_aggparam()` machinery that keeps it off aggregate
members, `arm64_trunc()` in its entirety, and the §4g representation guard
(with `int` and `unsigned` both at register width, the paint is a no-op again).
`mkfs`'s `gmode()` — `return((&m0)[i])` over four undeclared parameters, which
this plan records as *not fixable in the compiler because the slot size is the
ABI* — simply works, because an 8-byte `int` and an 8-byte argument slot agree.

That is a real prize and it is why the question deserved measuring rather than
answering from the existing decision.

### Why it is nevertheless unavailable

V8's ccom has exactly four integer types — `CHAR SHORT INT LONG`,
`common/manifest.h:224-227`, and there is no `long long` anywhere in the front
end. The port has to express exactly four widths: 8 for `char`, 16 for `ino_t`
and `di_mode`, **32** for `daddr_t` and every on-disk time and the tape's
`c_magic`/`c_checksum`, and 64 for a pointer. Four types, four widths, so
`char/short/int/long` must be `8/16/32/64` — which is LP64. It is the only
assignment that covers the set.

Move `int` to 64 and **32 becomes unspellable**. Every field of `struct
dinode`, `struct filsys`, `struct fblk` and `struct spcl` that is four bytes
because a VAX wrote four bytes would have to become `char[4]` with hand-packing
— which means editing the authentic programs that read them, to preserve a
format that exists to be authentic. Measured rather than reasoned: `SZINT` and
`ALINT` were flipped to 64 and the tree built. It fails first in a much smaller
place — `local.c`'s `sz_incode()` tests `inwd == SZINT` before `SZLONG`, so
with the two equal a 64-bit initialiser is emitted as `.long` — but the disk
formats are where it would end.

### And the premise was worth checking too

The question arose from a session that looked like sustained 32-bit trouble.
Re-counted: of roughly seventeen findings, **three** were int-width — the
`arm64_trunc` truncation and the §4g signedness conversion. The other fourteen
were null-pointer dereferences (§4i), an out-of-bounds read in `strncat`, and
an off-by-one in `ls -R`, none of which any data model affects. The compiler
findings came first and were the most dramatic, which made the whole run read
as width work.

### What was done instead

The adjacent move, and the one that actually retires the risk: **make the
widths explicit rather than implicit**. `<sys/types.h>` now declares `v8_i16`,
`v8_u16`, `v8_i32`, `v8_u32`, and every record field is spelled with one, so a
struct says what the *format* requires instead of relying on the target
happening to agree. `src/include/PORTING.md` has the full account, including
the three-way verification (layout measured from the V8 side, both artefacts
compared byte for byte against a same-binary clock noise floor, and a
mutation-verified guard that fires ahead of its own symptoms) and the discovery
that `sys/fblk.h` had never been imported at all — so an on-disk record had
been compiling against 1985's own header, correct only by coincidence.
