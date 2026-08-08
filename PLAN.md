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
├── rootfs/                     BUILD OUTPUT: v8-style tree (bin, usr/bin, lib, usr/man…)
└── blitterm/                   Phase 5 Swift app (Xcode project)
```

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
| int / long / ptr | 32/32/32 (`NOLONG`) | 32/**64**/**64** (LP64) | macOS has no ILP32 process model. LP64 is the well-trodden path; `NOLONG` assumptions become a known bug class, hunted with ported `lint`. |
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
   against the host at `syscall.c:619`, so this is the same kind of lie in the
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
| Translate structs/values | `stat/fstat/lstat` (16-bit ino via hash-fold, never 0; dev squeeze), `time/ftime/times`, `wait/wait3`, `select` (fd_set width), `open` (see directories), errno mapping host→V8 |
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
  `nm -u rootfs/bin/who` is exactly `_setutxent _getutxent _endutxent`. Every
  other binary in the rootfs imports nothing.

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
not by a bad drawing. It is now the only entry.

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

## 8. Phase 4 — Blit terminal (`blitterm`, Swift)

Native macOS app playing the 5620/jerq role. Tiers:

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
- `stat.c` folds inode numbers to 16 bits and **they can collide**, so `find`
  hunting hard links may report false matches.
- `df` carries this port's one sanctioned source deviation (S7), because there
  is no superblock to read.

And ten programs currently written off as "raw VAX disks": `fsck mkfs icheck
dcheck ncheck clri quot dump restor dumpdir`.

### Sequence

Ordered so that value lands before risk, and so each step is testable alone.

1. **Streams.** `streamio.c` is 1093 lines of portable multiplexing -- no MMU,
   no disk, no scheduler. Independent of everything below, highest ceiling,
   and forward-compatible (V10 renames it `io/stream.c`). Unlocks the Datakit
   and `netfs` work PLAN S7 currently excludes, and feeds blitterm Tier 2.

   **The engine is IN -- `dev/stream.c`, byte-identical to upstream.** This is
   the first Bell Labs *kernel* source in the port; `src/sys/PORTING.md` and
   `shim/kern/NOTES.md` have it, `tests/streams` is 43 cases. The claim worth
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

   **Still to do here: `sys/streamio.c`**, the syscall side -- `stopen`,
   `stread`, `stwrite`, `stioctl`, `I_PUSH`/`I_POP`. **Surveyed rather than
   estimated**, and the survey says not yet; `src/sys/PORTING.md` has it in
   full. The number that made `stream.c` affordable was nine external names.
   `streamio.c` needs **~34**, across **11** authentic headers, four of which
   (`user.h`, `proc.h`, `inode.h`, `file.h`) are the whole process model --
   `struct user` alone is referenced 69 times.

   **`tsleep` is the one that decides it, and it is not a machine-dependent
   fill-in.** In the kernel it blocks until *another process* calls `wakeup`.
   A per-binary shim has no other process: the only producer that could run is
   `queuerun()`, on the same thread, at `splx()`. So here it can only mean
   "run `queuerun` and re-poll", which changes the engine's semantics rather
   than supplying a machine fact. The per-binary question is not a caveat on
   this step; it is its first compile error.

   A genuinely pure stratum exists -- `qattach`, `qdetach`, `streadable`,
   `nilopen`, `nilput`, 86 lines, 7.9% -- and is unreachable in isolation,
   because a byte-identical import has to compile and link *whole*. Two-thirds
   of the file would be provably dead at the end of it. **So: answer the
   per-binary question first, then import once.** Doing it the other way means
   writing `tsleep` twice and settling its semantics under a build that will
   not link.

   Four hazards are already recorded against the day it happens, including an
   upstream LP64 bug at `streamio.c:713` and a real conflict between two of
   this port's commitments: `stream.h`'s `short pgrp` takes a pid, and
   `stream.h` is byte-identical and asserted so, therefore cannot be widened
   the way `p_pid` was.
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
     -- and CLAUDE.md carries the general lesson. A sweep found exactly four
     functions in the tree with a >8-argument call, which is why 156 Wave A
     programs plus Wave B and C never saw it.
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
   pass, after which a second pass reports nothing. `tests/mkfs` is 97 cases;
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

   `-DDIRSIZ=14` changed kind here too. For the four readers a wrong DIRSIZ is a
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
5. **`v8fs` as the third server** -- V8's own `alloc.c`, `iget.c`, `nami.c`,
   `rdwri.c` over that image. Then `mklost+found`, and the other nine.
6. **The SIMH cross-check**, as an acceptance test rather than a CI job.
7. **FSKit host client**, with Phase 5. Public API since macOS 15.4, no kernel
   extension; lets the host mount a V8 image in Finder and disposes of the
   ingest-and-extract problem. Swift-side, so it belongs beside blitterm.
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
| 5 | blitterm Tier 1 (Swift) | L–XL | 3B |
| 6 | Stretch: jim via Tier 2, upas/Mail, f77, cfront, Datakit-over-TCP | XL each | various |

Critical path: **0 → 1a → 1b → 1c/2b → 3A → 3B → 5**.
First shippable milestone: *"v8 sh runs in Terminal.app, built by v8cc against v8 libc"* (end of 3B).
Second: *"man 1 ls through real troff"* (3C). Third: *"windows on a Blit"* (5).

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
| 5 blitterm | not started | |
| 6 installation | done | `make install` stamps the prefix into every binary and writes the `v8` launcher; `jail` 62/62 |
| 8a.1 streams | engine in | `src/sys/dev/stream.c` byte-identical to upstream; `streams` 43/43. `streamio.c` surveyed and deferred — see S8a step 1 |
| 8a.2 fs switch | done | `shim/v8sys/vfs.c`, one mount table, passthrough behind it |
| 8a.3 `/proc` | done | `ls /proc`, `PIOCGETPR`, the u-area at `UBASE`; `ps` runs |
| 8a.4 `mkfs` | **done** | `mkfs` writes a real free-list/1024 V8 filesystem; `mkfs` 46/46. It began by finding that **every on-disk struct in the tree was the wrong size** |

`make test` runs everything — seventeen suites, about 1040 cases.

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
