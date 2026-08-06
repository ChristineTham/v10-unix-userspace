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
make test             # all 16 suites (~779 tests)
make test-wavec       # one suite: deps jail selfhost cpp v8ccom v8cc v8sys freestanding
                      #            libv8c wavea waveb sh wavec kmemu streams hooks
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

**There are two makes here, and confusing them is easy.** The `make` above is
GNU make, driving *our* top-level Makefile — the seed harness for rungs 0–3, and
the everyday command. `$V8ROOT/bin/make` is V8's own, and it is what reads *Bell
Labs'* makefiles for rungs 4 and 5. The distinction is the bootstrap claim, not a
preference: rung 4 is only meaningful if the build description is theirs. The
`v8-make.sh` hook refuses the host's make on any makefile `tools/import.sh`
brought in, because GNU make would run it perfectly well and nothing would say
the rung had not happened.

There is a **third** build tool coming, and it is not a spelling of these two.
`mk` is Andrew Hume's successor to make (1987), and it arrives with **V9** —
measured, not recalled: V9's README says "all the source and makefiles(mkfiles)",
and V10's kernel is full of them. So it is needed at the *first* upgrade step. V8 has no trace of one: no `mk`, no
`mk.1`, not one `mkfile`, and the only `mk.c` upstream belongs to `efl`. So `mk`
is a non-question today and a port rather than a rename when it lands. PLAN.md
§4a says what it will cost; `tests/hooks` fails the day the first `mkfile`
appears, because `v8-make.sh` would otherwise wave the whole new tree through
while still reporting success.

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

## `src/sys/` — V8's kernel, and it plays by layer 1's rules

New as of PLAN §8a step 1. `src/sys/dev/stream.c` is Dennis Ritchie's stream
machinery, **byte-identical to upstream** — `tests/streams` compares
`git hash-object` against PROVENANCE, so an edit is a test failure. The
machine-dependent half is `shim/kern/`, in the same relationship
`compiler/ccom-arm64/` has to ccom.

Three things about it generalise to the rest of `sys/`, so know them before
importing more:

- **Machine facts go in `shim/kern/h/`, never in `src/sys/h/`.** A quoted
  include tries the includer's directory first, so `"../h/stream.h"` from
  `src/sys/dev/` finds the authentic header and `"../h/param.h"` falls through
  to `-Ishim/kern/dev`. An authentic header always wins; ours fill gaps.
- **K&R gets a dialect flag, not an edit.** `$(KERNFLAGS)` is `-std=gnu89` with
  implicit-int and implicit-declaration off. That is how "do not modernise K&R
  declarations" is obeyed rather than worked around.
- **Prefer a redirection in the header to a deletion in the source.** `printf`,
  `bcopy` and `uballoc` are all `#define`d aside in `shim/kern/h/param.h`, which
  is what keeps the blob hash intact. A three-line deletion would have been
  easier and would have cost the strongest claim available.

`libv8kern.a` is separate from `libv8sys.a` for libkmemu's reason plus a
storage one: 85 KB of bss, and `qinit()` dirties ~60 KB of pages. `cat` does not
carry it.

## Architecture: three layers, three different rules

The single most important thing to get right is **which layer you are editing**,
because they have opposite policies.

**1. Authentic V8 source (`src/`)** — imported from `third_party/`, then
minimally patched. Changes must be forced by the target (LP64, Mach-O, ARM64
ABI), not by taste. Do not modernise K&R declarations, do not add prototypes, do
not "fix" warnings. Every change is recorded in that program's `PORTING.md` with
the reasoning.

**2. New code (`compiler/`, `shim/`)** — written for this port, modern C.
`shim/v8sys/vfs.c` is the **filesystem switch** (PLAN §8a step 2): one mount
table, two types behind it — passthrough, and `/proc` in `shim/libkmemu/`. The
table is the old `v8dirs[]` with a type column — do not add a second prefix list
beside it. `struct v8fstyp` answers to V8's own `struct fstypsw`; where it
departs (descriptors, not inodes) the header says why.

Dispatch is **by descriptor, not by operation**, and `ioctl` is where that stops
being a detail: `v8s_ioctl` routes on `v8fs_fdtype(fd)`, so `PIOCGETPR` on an
ordinary file is `ENOTTY` and `TIOCGETP` on a `/proc` descriptor is `EINVAL` —
the same command number, two paths, which is the pair `tests/kmemu` asserts. The
sgtty/termios translation in `ioctl.c` did not move when the slot arrived; it
*became* the passthrough type's `t_ioctl`, which is what it always was.
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

**`shim/libkmemu/` may link host libc** — the one component that may, and it is
built: `who` runs, with **no changes to `who.c` at all**, because the shim
manufactures `/etc/utmp` when a reader opens it rather than giving the program a
function to call. `df`, `load`, `w` and `uptime` followed; `load` also needed no
source change, because the shim manufactures a *kernel* — a namelist at `/unix`
and a `/dev/kmem` with the data where the namelist says it is, both generated
from one table in `shim/libkmemu/kmem.c` so they cannot drift apart.
`shim/libkmemu/NOTES.md` has the whole story. It answers
"what is running / what is mounted / who is logged in" through documented,
stable interfaces (`getutxent`, `getfsstat`, `proc_listpids`, `sysctl`) so
Phase 4's grovelers can be honest. The alternative was parsing
`/var/run/utmpx` by hand to keep `libv8sys` raw-syscall-only, and that file's
layout is private and undocumented — a wrong guess there yields a `who` that
looks right and lies. Reaching for libc here narrows what the port depends on.

The boundary matters more than the exception: **per-file, not per-shim**.
Everything in `shim/v8sys/` stays raw-syscall-only (`dir.c` says so at its top,
and that still holds). libc is for reading system facts, never for file I/O,
strings, or anything `rawsys.h` already covers — that would be convenience, and
convenience is how an exception list stops meaning anything. PLAN.md §7 has the
reasoning, and `tests/kmemu` turns it into an assertion: `who` imports exactly
`_setutxent _getutxent _endutxent` and nothing else does.

The same per-file rule holds *inside* libkmemu. Only `utmp.c` names a libc
function; `synth.c` writes the file it produces through `rawsys.h` like the rest
of the shim, so the line is visible in the code rather than only in this list.

### The bootstrap ladder

Order matters and is not obvious. Each rung is built by the one above:

```
0 seed   host clang + host make + host yacc -> cpp, ccom-arm64, cc-seed, libv8sys, crt0
1 tools  cc-seed -> libv8c -> v8cc -> yacc -> lex -> make   (make needs yacc: it has gram.y)
2 jail   v8cc -> /bin: sh and the filters
3 close  regenerate cpp's grammar with V8 yacc; fixpoint v8cc1 == v8cc2
4 hand   V8 make rebuilds the compiler, inside the jail
5 world  V8 make + each program's own authentic makefile
```

**There is one cycle in this build, and `cc-seed` is how it is cut.** The
installed driver is a V8 binary, so it must be *linked* against `libv8c` — and
`libv8c` must be *compiled* by a driver. So `cc-seed` (the same `cc.c` built by
clang, never installed) compiles `libv8c`, `libv8c` links the real driver, and
the real driver compiles everything else. Both drivers exec the same `cpp` and
`ccom`, so the objects are identical either way; only the process differs.
Rung 4 is where the seed stops being needed.

**Rung 3 is closed.** All twenty translation units of `cpp` and `ccom` compile
under v8cc and link freestanding, the self-hosted `cpp` matches the stage-0 one
byte for byte, and the compiler reproduces itself: ccom2 (built by ccom1) and
ccom3 (built by ccom2) generate byte-identical assembly. `tests/selfhost`
asserts it. Note ccom1 == ccom2 is *false* by two instructions and that is
correct — ccom1 inherits one generation of the clang-built stage-0's beliefs,
and stage 2 washes it out. That is what a three-stage bootstrap is for.

**Rung 4 is closed too.** `tests/jail` drives a full rebuild of `ccom` with
V8's make, under `V8JAIL=strict`: V8's make reads the makefile, V8's sh runs the
recipes, V8's cc drives V8's cpp and ccom, and the only thing permitted out is
the documented as/ld exception. The makefile is plain 1985 make — no pattern
rules, no automatic variables past `$@`. The result compiles real source, the
build settles, and a second build from clean generates identical code.

**Rung 5 is demonstrated on seven programs, chosen for their makefile idioms
rather than their size**: `lex` (dependency line on `#include`d non-headers),
`sed` (target, prerequisites and recipe on one line; `*.o` glob), `fmt` (macro
expansion), `tsort` (`.SUFFIXES` and a `.c.o` suffix rule, no explicit object
rules), `tbl` (`t?.o` glob, three flags at once, a 22-target dependency line on
`t..c`), `yacc` (`$(CC)`, `y?.o`, dependencies on `dextern` and `files`), `spell`
(four programs from one makefile — `spellprog` specifically, since `all`
regenerates the word lists).
V8's make handled every one unchanged.

Two things came out of that which our own rules could not have surfaced. `sed`
found a *driver* gap — `-n`, the VAX shared-text flag, now accepted and ignored
like `-O`. And `tbl` and `yacc` prove V8's make gets the `#include`d-non-header
dependency lines right, which means **the knowledge our Makefile had to be told
was in the tree the whole time.** That is the argument for doing the rest of
them: upstream's makefiles exercise the toolchain in ways our rules never do,
because our rules were written to work.

**Where rung 5 stops, and it is a real line rather than a to-do.** A makefile
that names the target machine cannot be used unchanged. `src/cmd/cpp/Makefile`
opens `CFLAGS=-O -Dunix=1 -Dvax=1 ...`, and `cpp.c` tests `vax` in three
places, so running it as written would build a VAX preprocessor. It is the only
one in the tree that does (`grep -l 'Dvax' src/cmd/*/[Mm]akefile`). PLAN.md §4a
already says program builds move onto their own makefiles "minimally adapted,
with every deviation recorded" — this is what that clause is for, and the
adaptation is one flag. Do not let a green rung-5 test tempt you into pretending
such a makefile ran unmodified.

`tests/jail` builds `lex` from `src/cmd/lex/Makefile`
— upstream V8, unmodified — with V8's make, cc and yacc, in a directory holding
nothing but V8 sources, under `V8JAIL=strict`. That makefile is the one worth
proving: its line 11 declares `lmain.o: lmain.c ldefs.c once.c`, the dependency
whose absence caused this port's worst bug. Moving the remaining programs onto
their own makefiles is mechanical from here; the pattern and the guard exist.

**Phase 6 is done.** `make install PREFIX=... BINDIR_HOST=...` (defaults
`/usr/local/v8` and `/usr/local/bin`) rebuilds with the prefix stamped into
every binary and writes a launcher script. `v8` drops you at `/` in a world
whose `/bin`, `/etc`, `pwd` and compiler are all V8's, with the Mac still
reachable through PATH.

The install rebuild starts with `make clean`, and that is load-bearing: make
does not track a change to `V8ROOT_DEFAULT` because it is a recipe flag rather
than a prerequisite, so an incremental build would install binaries still
carrying the build tree's path. The *installed* `cpp` and `ccom`
are still the clang-built ones (`nm` shows 0 V8 symbols); they sit inside the
rootfs without obeying the jail. Swapping them for the self-hosted ones is
Phase 6 work, not a ladder rung.

### The jail

`rootpath()` in `shim/v8sys/syscall.c` is a chroot implemented in the shim, not
the kernel: `chroot(2)` needs root, and every V8 binary is a Mach-O linked
against `libSystem`, so a real chroot would need the SIP-protected dyld cache
inside it. Consequence, and it is load-bearing: the jail is **per-binary, not
per-process-tree**. Host binaries never call `rootpath()`, so they see the real
macOS with no special casing; anything `cc` produces links `libv8sys`, so it is
jailed by construction.

**A path resolver that keys on existence cannot resolve a creation, and that
gap was one-directional.** `rootpath()` redirects a path whose rootfs copy
exists — right for a reader, unanswerable for a name that does not exist yet —
so `creat("/etc/x")` went to the *Mac's* `/etc` while `open("/etc/x", 0)` read
the jail's. It always failed on macOS, and it failed with **EACCES**, which
reads as a permissions problem rather than as a missing jail. `v8s_creat`,
`v8s_link` and `v8s_mkdir` resolved nothing at all. Closed: `V8P_MAKE` keys on
the *parent*, and `mkpath()` (LOOK first, then MAKE) is what every creating
syscall uses. Readers keep `vpath()`, so the union is unchanged. `shim/NOTES.md`.

`v8s_execve` also interprets `#!` itself. The kernel would resolve a shebang
against the real filesystem before the shim saw it, so every shell script ran
under the Mac's shell — the last hole in the chroot, and the most invisible.
Watch for the aliasing trap when touching it: `rootpath()` returns a pointer
into its own static buffer, so holding two results at once silently gives you
the same string twice.

## The bug classes that actually bite

**LP64 is the dominant one.** V8 assumes `sizeof(int) == sizeof(char *)`. The
tree calls `malloc` without declaring it and casts the `int` result to a pointer,
and undeclared K&R parameters are `int` but routinely hold pointers — common
enough that the compiler widens them deliberately, at `acctype()` in
`compiler/ccom-arm64/gencode.c`. That widening must not reach an int *member* of
an aggregate parameter, and `arm64_aggparam()` in `local.c` is what stops it:
`bfcode()` records the byte ranges of the aggregate parameters from the declared
types it is handed, and `acctype()` asks. Symptoms are wild pointers and heap
corruption far from the cause. When something is mysteriously broken, check
widths first.

One shape of it is worth naming because it hides in a *declaration* rather than
in code: **a yacc token declared with a scalar type while the lexer stores a
pointer into `yylval.p`.** On the VAX `.i` and `.p` were the same four bytes;
on LP64 the address loses its top half. Found in `pic` (`TROFF`) and then, by
sweeping, in `grap` (`PIC`) — neither had ever been reached by any input in the
tree. The whole-tree table is in `src/cmd/grap/PORTING.md`; only grammars that
declare types can have it, and the untyped ones are already covered globally by
this port's change to `src/cmd/yacc/y2.c:302` (`#define YYSTYPE long`, not
`int`). Re-run the sweep after importing any program with a grammar:

```bash
grep -n 'yylval\.p *=' src/cmd/*/*.l
grep -nE '^%(union|type|token)[ \t]*<' src/cmd/*/*.y
```

**A same-size conversion is not always a no-op.** Widths are the common fault;
signedness is the other one. A signed and an unsigned value of the same size
have identical bits, so converting between them changes nothing about the
*result* — and `optim.c`'s `sconvert()` therefore drops the conversion and
paints its type onto the operand. That is sound for every operator whose bits
come out the same either way, and wrong for `/`, `%` and `>>`, where
`gencode.c` reads that same type to choose `udiv`/`sdiv` and `lsr`/`asr`. There
the type is an *instruction selector*, not a description. Guarded under
`SIGNCONVKEEP`; PLAN.md §4g has the account. Two rules follow: a back-end
operator whose instruction depends on `tyunsigned()` must be added to that
guard, and `sconvert()` must not be "simplified" — the same seven lines have
now produced two faults of this shape, `PTRCONVFULL` being the other.

The diagnostic is the reusable part, because the symptom is narrow enough to
mislead. It needs an unsigned operand **with its top bit set**, so
`printf("%lx")` of a negative long lost *exactly one digit* — `val /= base`
clears the top bit after the first iteration, and every later digit was right.
One wrong digit reads as an off-by-one in a buffer, and is not. When a value is
wrong in exactly one place, stop reasoning about the source and read what was
emitted: `cc -S`, then look for an `sdiv` or `asr` where the C says unsigned.

**Read the program before deciding how to port it — twice now the plan was
wrong about what a program talks to.** PLAN.md said `ps` would be ported "on
top of `libproc`". V8's `ps` is a **`/proc` client**: `getdir("/proc")`,
`open("/proc/<pid>")`, `ioctl(PIOCGETPR)` for the `struct proc`, and the u-area
read at virtual address `UBASE`. That is Killian's process filesystem, V8's own
invention — `sys/pioctl.h` and `sys/sys/proca.c`. Bolting `libproc` on would
mean rewriting `doselect.c` against an interface V8 had already abandoned, in
order to avoid building the one it used. So `ps` waits for `/proc` (PLAN §8a
step 3), and so does the full form of `w`.

`w` is the counterpart: it is `@(#)w.c 4.4 (Berkeley) 6/5/81` and grovels
`/dev/kmem` and VAX page tables, while `ps` carries no `sccsid` at all. **Two
process tools in one tree, from different eras**, and the era shows in what
they open. Only the `uptime` half of `w` runs here; the full half says `No mem`,
and `tests/kmemu` asserts that message so a future `/dev/mem` is a decision
rather than a discovery. `src/cmd/w/PORTING.md`.

**V8 spells DIRSIZ in THREE headers and `#ifndef` means first-include wins.**
`<dir.h>` (`struct dir`), `<sys/dir.h>` (`struct direct`) and `<sys/param.h>`
(no struct, and the one that decides — `w.c` and `ps.h` both include it first).
This port raises 14 → 254; patching two of the three changed nothing for
exactly the programs that read directories raw, while looking like it had. All
three now agree, plus `shim/v8sys/v8sys.h`'s `V8_DIRSIZ` and
`src/libc/gen/readdir.c`'s `ODIRSIZ` — five spellings of one number.

**LP64 is not the only width problem: V8's 16-BIT RANGES are the other, and
they fail later and quieter.** LP64 breaks a pointer immediately; a 16-bit field
holds a value the host has simply not reached yet. Three so far, and they are
one class:

| field | V8's range | the host's | how it failed |
|---|---|---|---|
| `DIRSIZ` | 14-char names | any length | truncated names, `pwd` could not `chdir` back |
| `d_ino` | 16-bit inode | 64-bit | wraps; harmless *except* the value that wraps to 0 |
| `p_pid` | `short`, wrapped at 30000 | to 99998 | **negative pids** — 44145 read as −21391 |
| `FSNMLG` | 32-char mount points | to 140 seen | `df` printed a mount point as a *device* |

**A truncated PATH is not the same class of loss as a truncated NAME**, and
conflating them is what let `FSNMLG` hide: `shim/libkmemu/mtab.c` documented the
truncation as "the same loss as dir.c's 14-character names" and was wrong. A
name is an opaque string to every reader; a path has to *resolve*, and `df`'s
`dfree()` branches on `stat()` succeeding — so the truncation did not shorten a
column, it sent df down the arm that assumes the string names a device. Widening
moves that boundary rather than removing it, so what still overflows is now
**reported and dropped** rather than truncated: an entry whose path cannot be
stored cannot be described truthfully. `src/include/PORTING.md`.

The `p_pid` one is the shape to remember: **a freshly booted host has low pids**,
so every check passes until the counter crosses 32767 and the same binary starts
lying. It was found by mutation-testing something unrelated, when a mutation
produced two extra failures it had no business producing. `tests/kmemu` now
asserts the *field width* beside the runtime value, because the width is true at
every pid and the comparison only at high ones. Widened in
`src/include/sys/proc.h`; `src/include/PORTING.md` is that tree's record, and it
holds the rule that a struct there has **two ends**, since v8cc and clang each
read one.

**A directory's `st_size` is the size of what `read(2)` gives, not what the host
charges.** The shim builds 256-byte records from variable-length host entries,
so the numbers are unrelated (nine entries: 2304 bytes of records, APFS says
288). Every reader that loops to EOF never noticed; `ps`'s `getdir` sizes an
array from `st_size` and demands `read` return exactly that. Fixed in
`v8sys_pt_fstat` via `v8sys_dirsize()` — **fstat only**, because nothing can
read a directory without opening it, and doing it for `stat(2)` would put a
`getdirentries` loop inside every `ls -l`.

**A preprocessor that is never fed downstream is not tested.** Both token bugs
above were invisible while the program itself looked perfectly correct, because
they lived at the seam: `grap`'s output crashed `pic`, and `grap` alone was
fine. The wavec suite now runs `grap | pic | troff` and asserts drawing commands
come out the far end. Pipe a new Wave C program into what consumes it before
believing it works.

**V8 assumes address 0 is readable.** The VAX put the text segment at 0, so
`*(char *)0` returned a byte of the program rather than trapping. macOS keeps
page 0 unmapped. `refer` depends on it: `prefix(".[", lookat())` in `refer5.c`
gets a NULL from `lookat()` at end of input, and on the VAX that quietly failed
the comparison. Symptom is a segfault on the *last* item of a file, so a test
with one citation will not find it.

**A missing libc function does not fail the link — it resolves from `-lSystem`.**
For a non-variadic function that silently works and hides the gap. For a
*variadic* one it is an ABI mismatch: v8cc passes every argument positionally in
x0–x7, Apple's ARM64 ABI passes variadic arguments on the stack. This bit three
times (`scanf`, `printf` via the driver, `execl` — the last made `system()` start
an interactive shell that looked exactly like a hang). `tests/libv8c` guards the
variadic shape, and `tests/kmemu` sweeps **every Mach-O in the rootfs** with
`nm -u` — which found five more the first time it ran, including a `getgrent`
that made `ls -g` read the Mac's group database from inside the jail. Note what
that says about `tests/freestanding`: it links its own small programs, so it
proved the shim was clean and never the world built on it. **A guard on a seam is
not a guard on what crosses it.**

Every remaining import is on `tests/kmemu`'s allowed list, named with its reason,
and the suite fails if an entry goes stale. Today that is libm alone, for
`pic`/`grap` (V8 shipped one; this port has never built one). `sleep` was the
other entry until signal delivery worked, and the staleness check is what took
it off: V8's own `sleep.c` built, nothing imported the host's any more, and the
suite failed until the entry was deleted.

**The struct a syscall takes is not always the struct libc takes.** For four
months no V8 program in this port could catch a signal, because `v8s_signal`
handed the raw `sigaction` syscall a userland `struct sigaction` where the
kernel wants `struct __sigaction` — 24 bytes, with a signal-trampoline pointer
at offset 8, exactly where the userland struct keeps `sa_mask`. Every handler
was installed with a null trampoline; `sigaction` returned 0 and nothing looked
wrong until delivery, when the process hung or died. Fixed:
`shim/v8sys/sigtramp.s` is the trampoline the kernel enters, and `shim/NOTES.md`
has the whole account. Two things generalise. **libc's wrapper is often not a
thin one** — `sigaction()` exists largely to convert between those two structs
and fill in a trampoline, so "the shim goes straight to the kernel" means
inheriting work libc was doing. And a struct at this seam that is the wrong
shape costs *nothing* at the call and fails much later, so the ones this port
depends on are now `_Static_assert`ed on size and offset.

**A guard on numbering is not a guard on delivery.** `tests/v8sys` checked that
signal numbers translated and never that a handler ran, so a shim in which no
handler could ever run passed 44 of 44. Same family as the `tests/freestanding`
gap below. Delivery cases each fork a child with a deadline, because the failure
mode is a hang rather than a wrong answer — run inline, the first one takes the
suite down and prints nothing.

**A stale object does not look like a build problem — it looks like wrong code.**
Four debugging rounds went to correct source compiled from already-fixed files.
`tests/deps` exists for this; see below.

**Files `#include`d that are not headers** are invisible to every dependency
scanner *and* to a `*.c` glob: `lex/ldefs.c`, `lex/once.c`, `tbl/t..c`,
`refer/refer..c`, `make/defs`, `yacc/dextern`, `yacc/files`. All are declared
explicitly in the Makefile.

**The compiler has no known unimplemented feature.** The last one was `STARG`,
passing a struct by value, which went unnoticed through 156 Wave A programs and
all of Wave B and C because none of them does it. `grap` does. `placeargs()` in
`compiler/ccom-arm64/gencode.c` now copies the aggregate into **consecutive
argument slots** — the V8/VAX convention, deliberately not AAPCS64's
by-reference rule for composites over 16 bytes, because v8cc passes every
argument positionally and a second convention for one node type would make it
neither. PLAN.md §4f records the decision and what it was measured against.

Two things there are worth carrying forward. `countargs()` counts **slots, not
arguments**, and getting that wrong is silent — the program builds, links and
emits nothing. And `stn.stsize` arrives at pass 2 **already rounded** to a
multiple of `ALSTACK`, because `argsize()` mutates the node in place; so the
copy reads a few bytes past a struct whose size is not a multiple of 8, exactly
as the VAX did from the same field.

**The frame has THREE regions, and fixing one collision created another.**
`arm64_endfunction()` in `emit.c` lays out, top to bottom: saved `x29/x30`,
locals, callee-saved registers, call area. The saves cannot sit immediately
below `x29` — pass 1's `oalloc()` hands out automatics as negative offsets from
the frame pointer under BACKAUTO, so every save lands on a local, and the first
real libc function died on it. The fix moved them to the *bottom*, which put
them exactly where AAPCS64 puts the ninth and later arguments of a call:
`[sp, #0]`. A function that both used register variables and called something
with more than eight arguments **overwrote the register it had saved on behalf
of its caller**, and handed the corrupted value back on return.

Two lessons, and the second is the reusable one:

- **A sweep found exactly four functions in the whole tree with a >8-argument
  call**: `printp` in `ps`, `dfree` and `main` in `df`, `ngs` in `ls`. That is
  why 156 Wave A programs plus all of Wave B and C never saw it. When a
  back-end bug depends on a code shape, count the shapes before assuming
  coverage.
- **A test that checks the callee RECEIVED its arguments cannot see the caller
  being destroyed.** `tests/v8ccom` already had two nine-argument cases and both
  passed throughout. The new ones need **three frames**, because the damage
  lands on the caller of the function making the wide call.

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
- **make's built-in `%.c: %.y` and `%.c: %.l` rules are cancelled, and must stay
  cancelled.** The built-in yacc recipe ends `mv -f y.tab.c $@`, and
  `src/cmd/ccom/common/cgram.y` sits next to the *checked-in* `cgram.c` the ccom
  rules deliberately use. So `make -B` on anything reaching `cgram.o` rewrites
  authentic source in place, with a different yaccpar and absolute paths in its
  `# line` directives. Seen happening here; `git diff` was the only thing that
  noticed. `tests/deps` asserts the built-ins stay dead.
- **Never depend on a phony target** for a real file. Phony means always out of
  date; that once recompiled 39 objects on every single `make`.
- **Order-only (`| foo`) means "exist before me", not "I depend on you."**
- Chained pattern rules make the middle file an *intermediate*, which make
  deletes. `.SECONDARY` where that matters.

`tests/deps` asserts the graph with `make -q` (nothing is compiled) and includes
negative controls. If you add build rules, add cases — and verify by mutation
that they can fail.

## Automation in this repo

`.claude/` carries five things, all of them encoding a bug that has already
happened here:

- **`hooks/block-third-party.sh`** (PreToolUse) refuses any write under
  `third_party/`, which would silently destroy the provenance hashes.
- **`hooks/v8-make.sh`** (PreToolUse, Bash) refuses the **host's** make where an
  authentic V8 makefile is what would be read — `cd src/cmd/lex && make`,
  `make -C`, `make -f`. Rungs 4 and 5 exist to prove the build *description* is
  Bell Labs' and not ours, and GNU make would run it perfectly well, which is
  the problem: the objects come out and nothing says the rung did not happen.
  Keyed on `PROVENANCE` rather than a list of names, so a program is covered the
  day its makefile is imported. The everyday `make -j8` / `make test` is
  untouched by construction — our own Makefile has no PROVENANCE line.
- **`hooks/check-makefile.sh`** (PostToolUse) runs
  `make -n --warn-undefined-variables` after any Makefile edit, and flags
  multi-target rules that carry a recipe. ~60ms.
- **`agents/lp64-auditor.md`** — subagent for the width, collision and variadic
  hazards. Run it on a freshly imported program before building.
- **`skills/port-program/`** — the workflow below, with `audit.sh` bundled.
  Invoke with `/port-program NAME`.

`tests/hooks` covers the two blocking hooks, because a hook fails in the
direction that is hardest to notice: it lets something through and says nothing,
so the tripwire simply is not there. `v8-make.sh` had two such bugs in its first
draft — two `jq` calls on the same stdin, so `cwd` came back empty and every
command looked like it ran at the project root; and a split on `&&` that threw
away the `cd` in `cd src/cmd/lex && make`, the single most likely spelling of the
mistake it exists to catch. Both passed a casual look. The negative cases matter
as much: a hook that blocks the everyday build gets switched off within the hour.

CI (`.github/workflows/ci.yml`) builds and tests on `macos-14` (ARM64 — an x86
runner would not exercise the AAPCS64 bugs this port keeps finding), then
asserts that a no-op `make` does zero work and that a clean `-j8` build passes.

## Porting a program

The recurring task. Steps 4 and 6 are the ones most often skipped and the two
with a history of costing multi-round debugging. `/port-program NAME` walks it.

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
  verified by mutation (break the thing, watch the test fail, restore). Two
  traps in doing that: **verify the object actually rebuilt** — mtimes compare
  at whole-second granularity, and a mutation that silently did not get
  compiled looks exactly like a test correctly passing (this has now produced
  two false "the guard did not fire" readings) — and remember that mutation
  proves a test can fail, never that it can *pass elsewhere*.
- **A test that asserts a property of the machine passes here and fails in CI,
  and mutation testing cannot see it.** Both of the CI breaks in this repo were
  this: `p_nice == NZERO` assumed the host's baseline nice is 0 (a GitHub runner
  starts jobs renice'd), and "some pid exceeds 32767" assumed a host that has
  been up a while (a runner is always freshly booted — the very property that
  let the 16-bit `p_pid` survive). Assert a *relation* the port controls — a
  difference between two processes, a field width — and where coverage genuinely
  depends on the host, print "not exercised" rather than passing silently or
  failing. `tests/kmemu`'s nice and pid checks are the worked examples.
