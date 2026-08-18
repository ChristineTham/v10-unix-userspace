# Running 1985 on Apple Silicon

## Rebuilding Research Unix V8's userspace from source, with its own compiler

> **Credits.** The project's direction and its architectural ideas — the fidelity
> contract, the insistence on an authentic compiler, the pseudo-kernel, the
> filesystem-image modes, the push toward 9P — are Christine Tham's. The
> research, the implementation, the debugging and this write-up are Claude's.
> Where the article says "we decided", it means an idea was proposed and then
> tested against the source until it either held or changed shape. Several
> changed shape. Those are the interesting parts.

---

## What this is

Research Unix Version 8 — Bell Labs, 1985 — rebuilt from source and running on
macOS/ARM64. Not emulated. The authentic Bell Labs C compiler compiles the
authentic V8 C library and the authentic V8 programs, on hardware that did not
exist when they were written.

Today the world has **97 installed binaries**, including the Bourne shell,
`troff`, `nroff`, `tbl`, `eqn`, `pic`, `grap`, `refer`, `spell`, `make`, `yacc`,
`lex`, `ps`, and the ten tools that build and check a filesystem — `mkfs`,
`fsck`, `icheck`, `dcheck`, `ncheck`, `clri`, `quot`, `dump`, `restor`,
`dumpdir`. `grap | pic | troff` draws a graph end to end. `refer` resolves
citations against an index its own tools built. `mkfs` writes a V7 filesystem
image that three independent checkers pronounce clean. The compiler reproduces
itself: the ccom built by ccom, built by ccom, generates byte-identical
assembly. **2686 tests across 17 suites** guard it.

The tree is 119k lines of authentic Bell Labs source under `src/`, against 8k
lines of shim and 4k lines of ARM64 back end — and 12k lines of tests. That
ratio is the project: the new code is a thin machine-dependent layer under a
large body of 1985 code that is left alone.

The interesting part is not that it works. It is *what went wrong on the way*,
because almost none of it failed the way you would expect. This is mostly an
article about bugs that succeeded.

---

## Part 1: Getting the source, and keeping it honest

In 2017 Alcatel-Lucent and Nokia Bell Labs agreed not to assert copyright over
non-commercial use of Research Unix editions 8, 9 and 10, and The Unix Heritage
Society published them. V8 is a full installed-system snapshot: `v8/{bin,etc,lib,usr}`
plus sources under `usr/src`. 352 programs with source; 290 entries in
`usr/src/cmd`.

The first problem arrived before a line was compiled. **macOS is
case-insensitive by default**, and the V8 tree is not. `usr/lib/ideal/lib/`
contains `arc`, `ARC` and `Arc` — three different files. Checking the tree out
on APFS silently loses two of them.

The choice was between a case-sensitive volume and renaming on checkout. We took
renaming, with a manifest: `CASE_COLLISIONS.md` records every group, which file
was kept, and what the others became (`ARC__case2`, `Arc__case3`). Thirteen
groups. It is a compromise, it is written down, and — as Part 9 explains — it is
one of the things a filesystem image would retire.

The second decision matters more. `third_party/` is **read-only, forever**.
Nothing is edited in place. To work on a file you import it:

```bash
tools/import.sh v8/usr/src/cmd/cpp
```

which copies it into `src/` and records its **upstream git blob hash** in a
`PROVENANCE` file. That single detail is what makes the whole project auditable:
at any moment, for any file, you can compute `git hash-object` and get an exact
diff against pristine 1985 source. Every deviation is therefore visible, and
none can hide.

A `PreToolUse` hook refuses any write under `third_party/`, because the failure
mode — silently destroying a provenance hash — leaves no trace.

---

## Part 2: The fidelity contract

Before any code, a set of rules in priority order. They exist so that hard
questions have answers instead of opinions.

1. **The C compiler is authentic.** `cc` → `cpp` → `ccom` are the original
   programs. The one thing that cannot be original is the code emitter — no
   ARM64 existed in 1985 — so we write a new backend *inside ccom's own
   architecture*, exactly as Bell Labs did whenever a new machine arrived.
2. **Assembler and linker are the host's.** V8's `as` and `ld` are 8,100 lines
   of VAX-specific tooling producing an object format XNU cannot load. `cc`
   execs `clang` instead. This is a decision, not a gap.
3. **libc is authentic C over a thin modern shim.** All portable C is kept. The
   63 VAX `chmk` syscall stubs become `libv8sys`.
4. **Programs are ported, not replaced.** Host passthrough is the exception.
5. **Non-goals:** the V8 kernel, real disks, Datakit hardware, multi-user login.

Rule 2 is the load-bearing one, because it establishes that *exceptions are
named and finite*. Every later argument — "may this component link libc?", "may
this program be patched?" — gets decided by asking whether it belongs on the
list, and the list is short enough to read.

---

## Part 3: The bootstrap ladder

You cannot compile V8's compiler with V8's compiler until V8's compiler runs.
The ladder resolves that, and the order is not obvious:

```
0 seed   host clang + host make + host yacc -> cpp, ccom-arm64, cc-seed, libv8sys, crt0
1 tools  cc-seed -> libv8c -> v8cc -> yacc -> lex -> make
2 jail   v8cc -> /bin: sh and the filters
3 close  regenerate cpp's grammar with V8 yacc; fixpoint v8cc1 == v8cc2
4 hand   V8 make rebuilds the compiler, inside the jail
5 world  V8 make + each program's own authentic makefile
```

**`make` cannot come first.** It has a 440-line `gram.y`, so it needs `yacc`.
The order is `cc → yacc → make`.

**There is exactly one cycle, and `cc-seed` cuts it.** The installed driver is a
V8 binary, so it must be *linked* against `libv8c` — and `libv8c` must be
*compiled* by a driver. So `cc-seed` (the same `cc.c`, built by clang, never
installed) compiles `libv8c`; `libv8c` links the real driver; the real driver
compiles everything else. Both execs the same `cpp` and `ccom`, so the objects
are identical either way — only the process differs.

**Rung 3 is the one that proves the compiler.** ccom2 (built by ccom1) and ccom3
(built by ccom2) generate byte-identical assembly. Note that ccom1 ≠ ccom2, by
two instructions, and *that is correct*: ccom1 inherits one generation of the
clang-built stage-0's beliefs, and stage 2 washes it out. That is what a
three-stage bootstrap is for.

**Rung 4 is where it stops being our build.** V8's `make` reads a 1985 makefile,
V8's `sh` runs the recipes, V8's `cc` drives V8's `cpp` and `ccom`, and the only
thing permitted out is the documented as/ld exception. There is a jail mode
(`V8JAIL=strict`) that refuses any escape to a host binary, so a green build is
a claim about V8 code rather than about what happened to be on `PATH`.

Rung 5 is demonstrated on **eighteen** programs chosen for their *makefile
idioms* rather than their size: `lex` (dependency lines on `#include`d
non-headers), `sed` (one-line rules and `*.o` globs), `fmt` (macro expansion),
`tsort` (suffix rules), `tbl` (a 22-target dependency line), `yacc`, `spell`,
`troff` (14 objects out of a 22-file directory — scale is its own idiom), `eqn`
(whose target is `a.out`, not its own name), `refer`, `ps` (V8 make's `&`, which
nothing else uses), `pic` and `grap` (`-lm`), `man`, `load`, `w`, `quot`, and
`make` — **V8's make building V8's make from V8's makefile**, the only entry
that closes a loop. V8's make handled every one unchanged.

And it taught us something our own rules could not have: `tbl` and `yacc` prove
V8's make gets `#include`d-non-header dependencies right — meaning **the
knowledge our Makefile had to be told was in the tree the whole time.**

**And then fifty more programs that have no makefile at all.** That is the other
half of `cmd/`, and its build description is not a makefile but a shell script:
`Admin/Mk` loops over the bare `*.c` files running
``eval D=`Admin/dest $B`; cc $CFLAGS -o $B $B.c``, then `strip $1 && cp $1 $2`.
Run verbatim inside the jail under `V8JAIL=strict`, it builds, installs and
cleans up all fifty of this port's single-file commands — exercising V8's `sh`
(`set -p`, functions, backquotes, `eval`, `case`), two nested shell scripts with
no `#!` line, and V8's `cc` driving V8's `cpp` and `ccom`. Three host execs in
total: `clang` twice per program, and `strip` once.

Two things fell out of that which our own rules could not have surfaced. Four
makefiles died on `Cannot load mv` — which is how we discovered that **eleven
commands had been imported and never built**, so the V8 world had no `cp`, no
`mv` and no `sed`. And `Admin/dest` turned out to be a *second, independent*
derivation of where each program installs, computed by V8's shell at run time
against our Makefile's computation at build time; the suite compares all fifty.

Which raised a question we had got wrong for a long time. This port put
everything in `/bin`, and that was wrong for **forty-one commands**: V8's `/bin`
is a 56-entry root-filesystem set from when `/` had to fit on one pack, and
`wc`, `tr`, `sort`, `sed`, `yacc`, `lex` and `dc` live in `/usr/bin`. Three
upstream sources say so and they agree on all but two entries — and the two they
disagree on are programs that appear in no table, where `Admin/dest` is
answering by *fall-through*, which is "nobody said" rather than "V8 said". The
split is now 25 in `/bin` and 56 in `/usr/bin`, derived from Bell Labs' own
tables at build time rather than decided by us.

---

## Part 4: The compiler

### What ccom actually is

The single biggest surprise in the survey: **V8's `ccom` is not table-driven
pcc.** Bell Labs replaced pass-2's pattern matcher with a hand-written recursive
code generator. Machine-independent pass 1 is ~8,200 lines; the VAX-dependent
emitter is 3,627 (`gencode.c` 1274, `genaux.c` 768, `local.c` 636, `local2.c`
319). One binary, no intermediate file.

That is good news: the interface to replace is a set of named functions with
clear jobs, not a machine description language. Our `compiler/ccom-arm64/` is
~3,900 lines and uses ccom's own file names and hooks — `local.c`, `local2.c`,
`gencode.c`, `macdefs.h` — because that is what pass 1 expects to call.

### The target model, and one decision that carries everything

| | V8/VAX | Ours |
|---|---|---|
| int / long / ptr | 32 / 32 / 32 (`NOLONG`) | 32 / **64** / **64** (LP64) |
| char | signed | signed *(Apple ARM64 agrees natively)* |
| floats | VAX F/D | IEEE 754 |
| struct return | `STATSRET` (static area) | keep `STATSRET` |
| symbols | `_`-prefixed | `_` on Mach-O — **the 1980s convention is Mach-O's** |

The consequential one is **argument passing**. V8's `varargs.h` is pointer
arithmetic over a contiguous stack block, and K&R code takes `&arg` and walks
forward. AAPCS64 passes the first eight arguments in registers, so that walk
reads garbage.

So our prologue **spills x0–x7 to a contiguous block**. Every argument gets one
8-byte slot, positionally. It costs a few stores per call and it makes
`varargs.h`, `&arg` arithmetic and prototype-less K&R varargs calls work
unmodified.

This was not a guess. Clang-built `ccom` segfaults inside V8's own `printf`,
because `printx` takes `&list` and `sprintxl` walks it. Once v8cc compiles
`printx.c`, it works with no changes. The decision is empirically forced.

It also has a consequence that returns in Part 6: **v8cc's calling convention is
not AAPCS64**, and every place the two meet is a seam with rules.

---

## Part 5: The shim — standing in for a VAX kernel

`libv8sys` is ~3,500 lines of modern C implementing V8's syscall semantics on
XNU. Three things about it are worth the telling.

### It names no libc function at all

The V8 world calls `write()`, and the shim implements `write()` *by doing what
`write()` does*. Define it in C and the shim's own call binds to itself and
recurses. Linker aliasing does not help — the alias is global, so it captures
the shim's own call too. (Observed, before this was understood:
`EXC_BAD_ACCESS` in `v8s_write+4` — a stack overflow in a function prologue.)

So the shim goes straight to the kernel: syscall number in `x16`, `svc #0x80`,
carry flag signals failure. A V8 program links `-nostdlib` and imports
**nothing**. `tests/freestanding` asserts that with `nm -u`.

That property turns out to be one of the most valuable things in the project,
for a reason nobody anticipated — see Part 6.

### The jail is a chroot implemented in the shim

`chroot(2)` needs root, and every V8 binary is a Mach-O linked against
`libSystem`, so a real chroot would need the SIP-protected dyld cache inside it.
Instead `rootpath()` resolves V8 paths inside `$V8ROOT`.

The consequence is load-bearing: **the jail is per-binary, not
per-process-tree.** Host binaries never call `rootpath()`, so they see the real
macOS with no special casing; anything `cc` produces links `libv8sys`, so it is
jailed by construction.

`v8s_execve` also interprets `#!` itself, because the kernel would resolve a
shebang against the *real* filesystem before the shim saw it — so every shell
script ran under the Mac's shell. That was the last hole in the chroot, and the
most invisible.

### Directories are a lie the shim tells convincingly

In V8 a directory is an ordinary file, and 44 commands read it directly as
16-byte records: a 2-byte inode number and a 14-character name. macOS refuses
`read(2)` on a directory outright.

So `open()` notices it was handed a directory and registers a shim entry:
the host directory is snapshotted into a buffer of synthetic V7 records, and
later `read()` calls are served from it. Nothing above the seam changes — not
V8's `readdir()`, not the 44 raw readers.

What is lost is documented rather than hidden: names are truncated to the
directory record's width, and inode numbers are folded to 16 bits. Both are
authentic V8 limits — a V8 program could not have seen more either.

**And the second of those turned out not to be a limit at all, which took
months to notice.** The note said folded inodes "can collide; harmless". They
are not harmless, because a 16-bit inode narrows an *identity* rather than a
value, and three of V7's idioms are built on identity. `getwd(3)` walks to the
root by finding the entry in `..` whose `d_ino` equals `stat(".")`'s `st_ino`
— and the same fold feeds both sides of that comparison, so two entries of one
directory sharing a fold made `pwd` stop on whichever `readdir` yielded first.
Measured across every directory in a months-old `$TMPDIR`: right 32 times in 60
inside a collision group, against 60 of 60 outside. Six of the failures printed
**another directory's path and exited 0**.

A confirming `stat()` does not help — it returns the folded inode too, so the
colliding file answers with the same `st_ino` *and* the same `st_dev`.
`ttyname.c` contains upstream's careful version of the idiom, pre-filter then
confirm, and it is defeated identically. So no consumer-side change can fix it,
and a patch to `getwd.c` was written and withdrawn.

What was recorded as the reason it *could not* be fixed was that the fold "must
stay a pure function". That is what makes 64-into-16 impossible. It is not what
the port needs: it needs the map to be **stable within a process**, which is
strictly weaker and admits an append-only table — the fold proposes a number, a
contended one takes the next free, and an assignment is never revised. On the
same host, minutes apart: 6729 entries went from 6210 distinct values to
**6729**, and `pwd` from 32-of-60 to **1752 of 1752**.

Two things about that are worth more than the fix. **A test tree structurally
cannot reproduce it** — APFS hands out consecutive inodes, which the old fold
separated perfectly, so 1500 directories created back to back collided zero
times; collisions need inodes spread over months. And the recorded constraint
was false in every clause: it cited a file that says the opposite, about a
field that does not exist, in a struct with no inode in it. A wrong *cause*
eventually trips a test. **A wrong constraint never does, because the code it
forbids does not exist.**

---

## Part 6: The bugs

This is the heart of it. Almost every serious bug in this project **succeeded**.
It produced a plausible answer, or no answer at all, rather than an error.

### The dominant class: LP64

V8 assumes `sizeof(int) == sizeof(char *)`. The tree calls `malloc` without
declaring it and casts the `int` result to a pointer. Undeclared K&R parameters
are `int` but routinely hold pointers.

`refer` was the textbook case. `zalloc` had no return type, so it returned
`int`, so every allocation came back with its top half gone. `hunt` faulted at
`0x49c7748`, which is `0x1049c7748` with the leading digit lost.

The compiler therefore widens undeclared parameters deliberately. Which caused
the *next* bug: that widening must not reach an int **member** of an aggregate
parameter. A note in the source had predicted it and declared the case
unreachable "for aggregates larger than 8 bytes" — and then we implemented
passing structs by value, which made it reachable.

### A yacc token declared with the wrong type

`pic`'s grammar declares `%token <i> TROFF` while the lexer stores a pointer
into `yylval.p`. On the VAX `.i` and `.p` were the same four bytes. Under LP64
the address loses its top half.

`grap`'s output crashed `pic`, and `grap` alone looked perfectly correct. Then
sweeping every grammar in the tree found the identical fault in `grap`'s own
`PIC` token — **which no input had ever reached.**

That produced a rule: *a preprocessor that is never fed downstream is not
tested.* The test suite now runs `grap | pic | troff` and asserts drawing
commands come out the far end.

### A same-size conversion that is not a no-op

`optim.c`'s `sconvert()` drops a conversion that changes only signedness and
paints its type onto the operand. Sound for every operator whose bits come out
the same either way — and wrong for `/`, `%` and `>>`, where the backend reads
that same type to choose `udiv`/`sdiv` and `lsr`/`asr`. There the type is an
*instruction selector*, not a description.

The diagnostic is the reusable part, because the symptom was absurdly narrow.
It needs an unsigned operand **with its top bit set**, so `printf("%lx")` of a
negative long lost *exactly one digit* — `val /= base` clears the top bit after
the first iteration, and every later digit was right.

One wrong digit reads as an off-by-one in a buffer. It is not. When a value is
wrong in exactly one place: stop reasoning about the source, and read what was
emitted.

### An `int` must wrap at 32 bits, and for this port's whole life it did not

The one that should have been found first and was found late. Every integer
here lives in an x register, correctly extended — an `int` is loaded with
`ldrsw` and is a correct 64-bit quantity. Arithmetic was then emitted 64-bit,
`add x9, x9, x10`. Right for every result that *fits*, and wrong the moment one
does not, because a register has no 32-bit edge to wrap at. The value then
**disagrees with itself**:

```c
printf("%d", i)     /* reads the low half   -- RIGHT */
if (i == 84446)     /* compares all 64 bits -- WRONG */
```

Found in `dumpdir`'s `checksum()`, which sums 256 arbitrary ints off a tape
record. It computed exactly `CHECKSUM`, **printed** exactly `CHECKSUM`, and took
the not-equal branch — so every dump tape this port wrote was unreadable by the
two programs written to read it.

It needs an overflowing accumulator that lives in a **register**: an automatic
is stored back through `str w` and re-narrowed by the store, so only `register`
exposes it. That pair is why 1187 tests had not reached it.

Then sweeping the class found the two unary operators, and for unsigned they
are not edge cases at all:

| | signed | unsigned |
|---|---|---|
| `-x` (`neg`) | wrong for `INT_MIN` only, which comes out **positive** | wrong for **every nonzero value** |
| `~x` (`mvn`) | **already correct** — bits 63..32 all equal bit 31, and flipping every bit preserves that | wrong for **every value** |

Both hid the way the checksum did: `~mask` is nearly always consumed by an `&`
against a zero-extended value, which discards the wrong top half and restores
the right answer. Only a comparison or a divide reads it whole.

**And fixing it turned a test red, which was the test's fault.** A case expected
`100000000000` from a function whose `long` arithmetic is returned through an
implicit `int` return — so the answer is `1215752192`, and clang `-std=gnu89`
agrees. It only ever read as the full value because the truncation never
happened. That is the third time here that a compiler fix broke a test
calibrated against the bug.

### A same-register return is not a same-type return

Floating point was somewhere the port had simply never looked, and there were
two bugs, each hiding the other so that fixing one alone changed nothing.

`extern float atof()` — in five upstream files — where `atof` returns `double`.
On the VAX both came back in `r0/r1`; on ARM64 a `float` return is `s0` and a
`double` return is `d0`, the same register read at a different width. So
`atof("0.5")` gave 0.

Underneath it: **v8cc passes doubles in `x0`–`x7` and AAPCS64 passes them in
`d0`–`d7`**, so every call into the host's libm read its argument from the wrong
register and `sqrt(2.0)` returned 0.000000. The test suite had tolerated libm as
an allowed leak on the grounds that it was "non-variadic, so it works" — an
argument about the *shape* of the call that is only as good as the register
classes agreeing.

The fix was not to port libm. V8's math is in `libc/math` and not in any libm at
all: `v8/usr/lib/libm.a` is **216 bytes** — one member, `dummy.o`, whose entire
symbol table is the name `_________`. It defines nothing. Eleven upstream
makefiles link `-lm`, and the honest answer to them is the empty archive V8
actually shipped, which `shim/libm/dummy.c` reproduces.

Together these meant `pic` never computed a correct radius, and every drawing it
or `grap` produced was geometrically wrong. The suite missed it twice over: its
inputs used only **default** sizes, which are compiled-in constants that call
neither `atof` nor the math library, and its end-to-end check counted drawing
commands with a pattern that only matched *because every coordinate was zero*.
**The test had been calibrated against the broken output, and fixing the program
broke the test.**

### An on-disk struct has an end that is not ours

Until `mkfs`, every struct in the port had two ends we control — v8cc reads the
header, clang re-spells it in the shim — so a widening was safe if both agreed.
A filesystem image has to agree with 1985. Measured before the fix: `dinode` 80
against the VAX's 64, `filsys` 7960 against 4096, `fblk` 1432 against 716.

**V8's own compiler settles it in one line.** `# define NOLONG` — "map longs to
ints" — at `cmd/ccom/vax/macdefs.h:20`. So `long` was 32 bits there, and
`daddr_t`, `time_t` and `off_t` all silently doubled here.

Three things generalise. The tree **already contradicted itself and nobody had
looked**: `param.h` hardcodes constants that assert `sizeof(dinode) == 64`, one
line from a `NINDIR` computed from the widened struct. A header nobody imported
**silently stays 1985's** — `sys/fblk.h` was reading upstream's declaration and
measured correctly anyway, by coincidence, twice. And narrowing a type globally
reaches past the headers: it broke a hand-written ARM64 routine that strode
eight bytes for exactly that type.

The widths are now said out loud, because the model cannot move. `int di_size`
did not mean "an int"; it meant "exactly four bytes, because a VAX wrote four
bytes there" — true only by coincidence of LP64, with the declaration and the
reason in different files.

### The same number spelled in six places

V8 spells `DIRSIZ` in **three** headers, and `#ifndef` means first-include wins.
This port raises 14 → 254; patching two of the three changed nothing for exactly
the programs that read directories raw, while looking like it had.

And the sentence recording that was itself wrong for months: upstream guards two
of the three and leaves `param.h` **bare**, so on a real V8 it always won by
*redefinition* rather than by being first. It cost nothing while every spelling
said 254, and was found the instant something wanted a different one — `mkfs`
writes a disk image and is compiled `-DDIRSIZ=14`.

**A wrong writer is invisible to every reader we have**, which is the part worth
keeping. Built without the flag, `mkfs` writes `..` at offset 256 instead of 16
— and `icheck`, `dcheck` and `fsck` all pronounce it clean, because the 240
bytes between are zero, a zero `d_ino` is V7's own deleted-entry marker, and a
16-byte-record reader skips fifteen empty slots and finds `..` exactly where the
254 writer put it. So the group's own checkers cannot guard the group; the only
guard is asserting the bytes of a generated image.

### Sixteen-bit ranges, which fail later and quieter than LP64

LP64 breaks a pointer immediately. A 16-bit field holds a value the host has
simply not reached yet.

| field | V8's range | the host's | how it failed |
|---|---|---|---|
| `DIRSIZ` | 14-char names | any length | truncated names; `pwd` could not `chdir` back |
| `d_ino` | 16-bit inode | 64-bit | `pwd` printed another directory's path, exit 0 |
| `p_pid` | `short`, to 30000 | to 99998 | **negative pids** — 44145 read as −21391 |
| `FSNMLG` | 32-char mount points | to 140 seen | `df` printed a mount point as a *device* |
| `u_uid` | `short`, to 32767 | to 100000+ | a uid ≡ 0 mod 65536 reads as **root** |

The `p_pid` row is the shape to remember: **a freshly booted host has low pids**,
so every check passes until the counter crosses 32767 and the same binary starts
lying. It was found by mutation-testing something unrelated, when a mutation
produced two extra failures it had no business producing.

And the `u_uid` row was written **one line below the paragraph arguing against
it**. The shim folds a Darwin pid into a VAX `short` and says at length why a
bare cast is wrong — *"a truncation can silently produce the one value the code
reads as absent"* — and then cast `u_uid` and `u_gid` with `(short)` on the next
two lines. The fix lands on one line and the line beside it keeps the
assumption. Found by a subagent, not by the person who wrote both lines.

### A 1985 buffer size is the same class

Raising `DIRSIZ` did not just widen a field; it invalidated every buffer sized
*against* it. `mv`'s guard was `strlen(target) > MAXN-DIRSIZ-2`, which became
`> -156`, so **every** directory move was refused with a false message. A
constant can encode a *relationship*, and changing one of its terms silently
rewrites the sentence.

### V8 assumes address 0 is readable

The VAX put the text segment at 0, so `*(char *)0` returned a byte of the
program rather than trapping. macOS keeps page 0 unmapped.

`refer` hit it at end of input, so a test with one citation would not find it.
`df` hit it in `while (argc >= 1 && argv[1][0]=='-')` — which dereferences the
NULL terminating `argv` — so **every invocation of `df` crashed before printing
anything.**

`quot`'s is the one to remember, because nothing about it is an edge case:
`du[]` is indexed by uid, only the uids in `/etc/passwd` get a `name`, so **2046
of 2048 entries are null** and `qsort` compares them against each other. That is
the *default* invocation, before a line is printed. It was found by auditing
before building, not by running.

Then the paragraph that used to sit here said "the sweep is not done", and it
was right: **doing it found nine more, all measured SIGSEGVs.** Every one is the
program's last argument, which is the whole trigger — `icheck -b 5`, `dcheck -i
5`, `join -j1`, `yacc -o`, bare `hunt`, `nroff -F`, and `unexpand` with **no
arguments at all**, where `expand.c` one file away has the guard.

Three things generalise:

- **The same loop existed three times and only one was fixed.** `n =
  atol(argv[1])` inside an option's number loop is byte-for-byte identical in
  `ncheck`, `icheck` and `dcheck`. Fixing `ncheck` and writing it up did not find
  the other two, because **the note was filed under the program rather than
  under the shape**. `icheck`'s own porting note had even audited that exact
  loop for a different overrun and gone one line past the null.
- **The crash is not always in the program.** `yacc -o` faulted in *our shim*:
  the output file cannot be created, the error path unlinks temp names that were
  never assigned, and the shim inspected the path before the syscall could
  answer `EFAULT`.
- **Fix to the VAX's answer, not just to the absence of the fault.** Address 0
  held `0x00`, so `strcmp(name, 0)` compared against the **empty string** — below
  every name — and an unnamed uid sorted before a named one while two unnamed
  ones compared equal. Reproducing that is a different patch from a null guard
  returning 0, and `quot`'s ordering is visible in its output.

**And for months that byte was recorded here as `0207`, "the low byte of the
a.out magic".** It is wrong, it was repeated in a dozen files, and **every fix
built on it is still correct** — which is exactly why nobody caught it. V8's
shipped binaries are **ZMAGIC**, so `N_TXTOFF` is 1024 and the header is never
mapped: virtual 0 is the first byte of **crt0**. `0x00` and `0207` are both "not
`'-'`", both non-digits, and both below any name character, so the guards agree.

The payoff of getting it right is that a VAX answer can now be computed for a
*structure* rather than a byte. Those sixteen crt0 bytes are identical in every
V8 binary, so reading them through the VAX `struct _iobuf` gives `_flag`
`0xd050` — which is how an unchecked `fopen("/dev/tty")` was settled. `getc(p)`
is `(--(p)->_cnt >= 0 ? …)`, which **writes** to virtual 0, and ZMAGIC text is
read-only shared, so a VAX takes a protection fault too. What the port lost
there is an *accident*, not a behaviour: `fopen` used to fall through to the
**host's** `/dev/tty`, a different device entirely.

### A missing libc function does not fail the link

This is the one that kept giving. A function missing from `libv8c.a` does not
break the link — it resolves from `-lSystem`.

For a non-variadic function that silently works and hides the gap. For a
*variadic* one it is an ABI mismatch, because v8cc passes everything
positionally and Apple's ABI passes variadic arguments on the stack. It bit
three times: `scanf`, `printf` via the driver, and `execl` — the last made
`system()` start an interactive shell that looked exactly like a hang.

Then, much later, writing a boundary test for something else swept **every
Mach-O in the rootfs** with `nm -u` and found five more at once:

| symbol | what it meant |
|---|---|
| `getgrent` | **`ls -g` was reading the Mac's group database from inside the jail** |
| `ftime` | a syscall the shim had simply never implemented |
| `tolower`, `toupper` | V8 has both in C; never built |
| `atof` | 319 lines of VAX assembly, so never ported |

And a sixth soon after: `getfsent`, where Apple's `struct fstab` has `char *`
members and V8's has `char[32]` — an instant segfault in `df`.

The lesson is sharper than "add the missing functions". `tests/freestanding`
could not have caught any of them, because **it links its own small programs**.
It proved the *shim* was clean and never the world built on it.

> **A guard on a seam is not a guard on what crosses it.**

### No V8 program could catch a signal

`v8s_signal` handed the raw `sigaction` syscall a userland `struct sigaction`.
The kernel wants `struct __sigaction`:

```
userland struct sigaction:   size 16  handler@0            mask@8   flags@12
kernel   struct __sigaction: size 24  handler@0  tramp@8   mask@16  flags@20
```

The kernel does not jump to a handler. It jumps to a **trampoline** the process
supplied in `sa_tramp`, which calls the handler and then asks for the
interrupted context back with `sigreturn(2)`. libc's `sigaction()` exists
largely to fill that in — so a shim going straight to the syscall inherits the
job.

Every handler in the port was installed with a null trampoline. `sigaction`
returned 0 and nothing looked wrong until delivery. It was found by building
V8's own `sleep(3)` — `alarm`, a handler, `for(;;) pause()` — which simply hung.
`tests/v8sys` covered signal *numbering* and never delivery, which is how it
survived.

The fix is a three-instruction trampoline, and one flag that is not obvious:
**`SA_NODEFER`**. `sigaction` blocks the signal for the duration of the handler
and `sigreturn` unblocks it — so a handler that `longjmp`s out never unblocks
it. V8's `sleep` longjmps out of its SIGALRM handler and `sh` out of its SIGINT
handler. Without the flag the *first* sleep works and every one after it hangs.
The test therefore sleeps three times, because one would not have found it.

### Two fields, same width, opposite rules

The shim manufactures `/etc/utmp` for `who` and `/etc/mtab` for `df`. Both are
arrays of fixed-width character fields. V7 does **not** terminate a field that
is exactly full, and `who` uses `%-8.8s` and `strncmp` — so a terminator would
be wrong.

But `df` does `strcpy(&specbuf[5], mtab[i].spec)` into 38 bytes. An unterminated
32-byte field runs into the *next record*, overflows `specbuf`, and smashes the
static that follows it — which was the digit buffer `ecvt` hands to `printf`. So
`df`'s `%use` column emitted hundred-digit strings **several rows after** the row
that caused it.

Found by printing what `df` was about to convert: every integer was correct, only
the conversion was wrong, and the path had become
`/dev//Library/Developer/CoreSimulatordisk5s1` — two fields run together, which
named the bug.

### And a class that is not a bug at all

Four debugging rounds went to correct source compiled from already-fixed files.
The worst: `lex`'s `once.c` widened two arrays to `long`, `parser.y` allocates
them with `sizeof(*left)`, and a `y.tab.o` built before the widening allocated
1700×4 bytes for arrays written as 8-byte longs. A 2× heap overrun through the
next block's malloc header, presenting as *"calloc returns 0"*.

There is now a test suite for the build graph itself.

---

## Part 7: How the testing works, and why

Three rules emerged, each from a failure.

**A guard that has never been seen to fail is not a guard.** Every new test is
verified by mutation: break the thing, watch the test fail, restore. This has
caught several tests that could never have failed — including one where removing
`atof` from the build left `atof.o` in the archive, because `ar r` *replaces*
members and never removes them.

**Prefer measuring to reasoning.** The hardest bugs were settled by making the
program print what a value *is*. And the corollary, learned the hard way twice:

- A recorded diagnosis of `refer` was wrong because `inv` had been run **with no
  stdin**. Every artefact recorded as evidence measured a program reading an
  empty terminal. *An artefact identical for every input is evidence the program
  never ran, before it is evidence about the program.*
- `gettimeofday`'s second argument is a `struct timezone *` and looks exactly
  like the answer for `ftime`. The **raw syscall writes something else there**.
  libc's wrapper does return the right offset — which is what made it look
  verified when it was not. The number was real; it was not a measurement of the
  path being used.

**A flaky test is worse than no test.** A `load` test compared against a
`sysctl` taken at a different instant, and load averages move. The fix brackets
a fresh run between two samples — and the *first* fix was itself flaky, because
`2.45 - 0.05` is `2.4000000000000004`, so a printed `2.4` fell outside a bracket
it sat exactly on. The margin has to exceed the noise in computing the margin.

**A measuring instrument you wrote is a suspect.** This is the lesson that cost
the most. A crash probe runs every installed binary against every single-letter
option and counts signal deaths. It reported 254, then 195, then 148, then
**96** — and only the last is true. Each error inflated the count, which is the
direction that wastes the most time, and each looked authoritative:

- **It was not hermetic.** All invocations shared one working directory, so
  programs read each other's litter. `dcheck` then "crashed" on 45 options,
  because its loop calls `check(*argv)` for *every* argument including options,
  so `dcheck -Q` opened a file literally called `-Q` and read a superblock out
  of it. **A prober must be a pure function of the program and its arguments**,
  or its findings are a function of iteration order.
- **The shell cannot tell a signal from an exit status.** `$?` is 128+N for a
  signal, but a program may `exit(134)` itself — and a V8 `main()` that falls
  off the end returns whatever was in the register. `primes` did, and 42 of its
  garbage statuses landed in 129..159 and were counted as SIGABRT.
- **The first diagnosis of the first fault was wrong.** Signals 9 and 10 in one
  program and no other reads exactly like a concurrent rebuild replacing a
  Mach-O mid-execution, so a filter was added discarding SIGKILL *and SIGBUS* —
  which would have hidden 48 genuine crashes.
- **And the population itself was transcribed.** The scan was six literal globs,
  one of which treated `/usr/lib/spell` as a directory by analogy with `refer`.
  It is a Mach-O *file*. The set is now derived by `find`, and it grew by
  exactly the two directories the transcription missed.

What survived all four errors unchanged was the *set* of programs, which is what
the fixes were actually driven from. Validate a prober against a known crasher
and a known-clean program before believing any number from it.

**A test that asserts a property of the machine fails on some other machine, and
mutation testing cannot see it.** Both CI breaks in this repo were this:
`p_nice == NZERO` assumed the host's baseline nice is 0 (a GitHub runner starts
jobs renice'd), and "some pid exceeds 32767" assumed a host that has been up a
while — the very property that let the 16-bit `p_pid` bug survive. Assert a
*relation the port controls*, and where coverage genuinely depends on the host,
print "not exercised" rather than passing silently.

It runs the other way too. A `who -i` case compared one line of output against
*every* line of another command's, an equality that holds only while the host
has exactly one login session. It passed for months, passes on a runner, and
broke the moment a second terminal was open.

And a third shape, which is not a property of the machine but **of what ran
before it**. The shim manufactures `/etc/utmp` lazily, when a reader first opens
it — so after any earlier `who`, the file is real and gets carried into copies. A
new case compared a `who` built by Bell Labs' own script against ours; it passed
here and failed on a runner, and **the runner was right**: the script-built
binary has no shim at all, and what it had been reading was a file an earlier run
left behind. A fresh runner is the only machine with no history, which makes it
the only one that can see this. Ask of any green suite: *would this still pass on
a tree that has never been used?*

**And `make -j8 test` is not `make -j8` followed by `make test`.** Under `-j`,
make runs the suites concurrently with each other's prerequisite *builds*, so a
suite reads objects another suite's build is midway through writing. Measured:
42 failures across four suites, four suites never running at all, and every
message reading like a real defect. Serially, the same tree was clean. The tell
is the *shape* — whole suites failing on build steps rather than on assertions.

There are also two blocking hooks — one refusing writes to `third_party/`, one
refusing the *host's* make where a Bell Labs makefile would be read — and
`tests/hooks` tests those, because a hook fails in the direction hardest to
notice: it lets something through and says nothing. That hook had two such bugs
in its first draft, both of which passed a casual look.

---

## Part 8: The grovelers, and the honesty problem

Some V8 programs do not compute anything. They report what the kernel knows.
`who` reads `/etc/utmp`; `df` reads `/etc/mtab` and then a superblock; `w`,
`load` and `ps` grovel `/dev/kmem` through a namelist. None of that exists here.

The decision — proposed and then sharpened against the source — was that
**`shim/libkmemu/` alone may link host libc**, calling documented interfaces
(`getutxent`, `getfsstat`, `sysctl`) to answer what is running and who is logged
in. The boundary is drawn *per file*, not per library: everything in
`shim/v8sys/` stays raw-syscall-only, and inside libkmemu only the file that
reads system facts touches libc; the file that writes the result uses raw
syscalls like the rest of the shim.

The justification is specific. Parsing `/var/run/utmpx` directly would have kept
the no-libc rule — but that file's layout is private and undocumented (measured:
628-byte records behind a `utmpx-1.00` signature, matching no documented
struct), and a wrong guess yields a `who` that *looks right and lies*. Reaching
for libc there **narrows** what the port depends on.

Then the design question: how do the programs get the data?

For `who`, the shim **manufactures the file it already reads**. `who.c` needed
**zero changes** — the whole port is in the shim. That is also what the real
system did: `/etc/utmp` was an ordinary file kept current by `init` and `login`,
both of which live on the kernel's side of this seam. We just do the bookkeeping
lazily, when a reader opens it.

For `load`, the shim manufactures an entire **kernel**: a namelist at `/unix`
saying where `_avenrun` lives, and a `/dev/kmem` in which it lives there. One
table generates both files, because two lists agreeing by hand is a standing
invitation. `load.c` also needed zero changes. `nlist(3)` is authentic V8 libc,
so `/unix` had to be a real a.out — and under LP64 `struct exec` is 64 bytes
where the VAX had 32.

For `df` the answer was different, and it is the project's one sanctioned source
deviation. `df` reads block 1 of a raw device as a `struct filsys`. Faking that
would mean manufacturing a fake *disk*, and `df -l` walks the free-block *list*
out of it — so it would need a fabricated free list. That is inventing data
rather than reporting it, which is the line. `df` gets its numbers from a call
instead, and the deviation is recorded.

The governing rule throughout: **any column with no honest source prints a
sentinel rather than a plausible number.** A fabricated `WCHAN` is the one thing
that would make `ps` output a lie. Similarly `df -i` reports **65535** — the
16-bit ceiling of the V7 superblock — rather than a believable-looking figure,
because a V8 `df` could not have described a 548-million-inode volume either.

### And `ps` turned out not to be a groveler at all

The plan said `ps` would be ported "on top of `libproc`". Reading it first —
which is a rule this project has now learned three separate times — says
otherwise. **V8's `ps` is a `/proc` client**: `getdir("/proc")`,
`open("/proc/<pid>")`, `ioctl(PIOCGETPR)` for the `struct proc`, and the u-area
read at a virtual address. That is Killian's process filesystem, V8's own
invention, already in V8's kernel. Bolting `libproc` on would have meant
rewriting `ps`'s selection logic against an interface V8 had already abandoned,
in order to avoid building the one it used. So `/proc` was built, and `ps` is a
client of it.

`w` is the counterpart, and the contrast is the point. It is
`@(#)w.c 4.4 (Berkeley) 6/5/81` and grovels `/dev/kmem` and VAX page tables,
while `ps` carries no `sccsid` at all. **Two process tools in one tree, from
different eras**, and the era shows in what they open. Only the `uptime` half of
`w` runs here; the full half says `No mem`, and the suite asserts that message,
so a future `/dev/mem` is a decision rather than a discovery.

Which produced the cleanest demonstration of what rung 5 actually claims. Bell
Labs' own build script compiles `who` with `cc -Od2 -o who who.c` — complete and
correct, and knowing nothing about the shim this port invented. So the binary it
produces says `who: cannot open /etc/utmp`, while ours answers. The suite runs
both, on the same host, seconds apart, and asserts the pair. **Rung 5 is a claim
about the build description being Bell Labs', not about the binary being the
installed one** — and having the two disagree out loud is what keeps that
distinction honest.

---

## Part 9: What reading V9 and V10 changed

The plan says V8 first, then V9, then V10. Before building the next large piece,
we went and read V9 and V10. Several assumptions did not survive. **Most of what
this section concluded has since been built — Part 10 is what came of it.**

### V8 has no FFS — but it has two filesystems

Searched the whole kernel: 108 headers, 18k lines. No `struct fs`, no `struct
cg`, no cylinder groups, no fragments. `struct dinode` is `di_addr[40]` —
thirteen block addresses packed three bytes each. It is the V7 filesystem.

**But there are two on-disk formats**, chosen by `BITFS(dev) = (dev) & 64` — the
device number is a format tag. Free-list at 1024-byte blocks, or a bitmap
variant at 4096 with **cylinder-aware placement**. `alloc.c` says so itself:
*"try for an acceptable free block in next three cylinders"*, `"same cylinder?"`
— and, candidly, `"unfortunately device dependent"` and `"this code is UGLY, fix
it"`.

So V8 independently reinvented FFS's three ideas — bigger blocks, bitmap
allocation, locality — with none of FFS's structures. Convergent evolution. And
its bitmap lives *in the superblock*, which caps a filesystem at 120 MB; and
`mkfs` can only create the *other* format, though `fsck` and friends understand
both. The bitmap filesystem shipped in the kernel ahead of its tooling.

### `/proc` is V8's own

`sys/sys/proca.c` is in V8's kernel already — Killian's paper is V8 — and it is
still there in V10 as one filesystem type among several. A process server is
therefore not an invention to be justified; it is a V8 feature to be
implemented.

### `mk` arrives at V9, not V10

V9's README describes "all the source and makefiles(mkfiles)"; V10's kernel
carries `sys/fs/mkfile` and `sys/io/mkfile`, and `v10/cmd` ships **both** `mk`
and `make`. So Andrew Hume's `mk` is needed at the *first* upgrade step. There
is already a test that fails the day the first `mkfile` appears in the tree,
because otherwise the make-guarding hook would wave a whole new build system
through while still reporting success.

### V10 is a quarry, not a destination

TUHS's own words: the V10 source was "pared from a 1995 snapshot", is "in no
sense a formal distribution", and "it is unlikely that it can be made into a
working system without a fair amount of hand-waving". Its kernel is reorganised
wholesale — `fs/ io/ os/ ml/ md/ vm/` where V8 has `sys/ h/ dev/ conf/`.

V9, by contrast, is a coherent VAX snapshot from early 1987 laid out much like
V8. So **V9 becomes the achievable terminus for a complete system, and V10 gets
mined selectively**: `mk`, the filesystem switch, the stream evolution, `/proc`'s
maturation.

### 9P, below the seam

The proposal was to adopt Plan 9's filesystem and 9P — accepting that it would
make the V8 port less faithful. The research produced a better answer, because
the sacrifice turns out to be unnecessary.

There are two different things called "9P" here:

- **9P as the wire protocol between the shim and its file servers.** V8 programs
  never see it. They call `open(2)`; the shim translates; a server answers. This
  is *below* the seam — the same category of decision as Mach-O instead of
  a.out. It costs no fidelity, because the V8 world cannot tell.
- **Plan 9 semantics exposed upward** — namespaces, `bind`, union directories.
  That *would* be unfaithful, and it is also unnecessary: nothing in V8's
  userspace asks for it.

And 9P-as-transport is better than the ad-hoc protocol that would otherwise have
been invented. It has been specified and stable since 2000, it is about thirteen
message types, and it is *designed* for the exact problem that forces a server —
many clients, one authority, per-client fids. There are reference
implementations to test against, and `9pfuse`/Mac9P/FSKit mean the **host** can
mount the V8 world.

The architecture is one switch, several servers, one protocol — which is the VFS
that V10 grew and that V8 was already growing:

```
V8 program -> libv8sys -> 9P -> +-- passthrough   (today's transparent mode)
                                +-- proc          (Killian's /proc)
                                +-- v8fs          (V8's own alloc/iget/nami
                                                   over a raw image)
```

**Status, so this is not read as done:** the *switch* exists and carries three
types, and `/proc` is one of them — that is Part 10. The **protocol** does not.
Today the types are C function tables answering to V8's own `struct fstypsw`,
called directly; 9P is what replaces the direct call when a server has to live
in another process, which is what `v8fs` over a raw image and an FSKit host
client will both need. Building the switch first, with the cheapest possible
types behind it, is what let the suites stay green while the floor moved.

### The best test available to this project

Images are **raw** — not VHD, not `.dmg`. Fixed VHD is only raw plus a 512-byte
footer, so size is not the objection; the objection is that nothing which
understands VHD understands a V7 filesystem *inside* it. A `.dmg` is worse still,
because macOS would attach it and mount it with a host filesystem.

Raw buys something real. **SIMH runs V8 and V9 on an emulated VAX and attaches
raw disk images.** Which makes this possible:

> Build a filesystem with **our** `mkfs`, on ARM64. Attach the image to SIMH
> running **Bell Labs' own V8**. Run **their** `fsck` on it.

If the original system accepts a filesystem this port produced, that is a
stronger statement than any test we could write — because the judge is not us.

---

## Part 10: The filesystem, and a kernel that reads and writes

Part 9's plan has largely happened. What follows is what it cost, and the three
places the plan was wrong.

### The switch, and a type that made two dormant rules live

There is one mount table and **three** filesystem types behind it: passthrough,
`/proc`, and `/dev/fd`. Dispatch is **by descriptor, not by operation**, which
stops being a detail at `ioctl`: the same command number is `ENOTTY` on an
ordinary file and `EINVAL` on a `/proc` descriptor, two paths through one entry
point.

`/dev/fd` is the cheap type and the instructive one. It implements **three**
operations and inherits the rest: identity for path resolution, `dup(minor)` for
open, a synthesized character device for stat — and everything after open is
passthrough's *unchanged*, because a dup'd descriptor **is** an ordinary host
descriptor. Giving it a type of its own would have invented a difference the
kernel does not have.

Adding a third type made two rules live that had never been exercised, and both
were incomplete. `creat` went straight to path resolution without dispatch — so
**no second type could ever have seen a creat**, and `/proc` is read-only, so
nothing had noticed. And `dup()` dropped the descriptor's type, so duplicating
an open `/proc` file returned one whose reads went to the host. Both are the
same shape as a syscall found earlier passing its path unresolved because
nothing called it: **an unexercised rule cannot be seen to be incomplete.**
Expect a fourth type to find a third.

### `/dev/tty` is not a device, and the plan was wrong about it for the third time

The plan costed a host-fd stream driver to sit under `/dev/tty`. **V8's
`/dev/tty` is not a stream, not a device, and has no code behind it**: it is a
hard link to `/dev/fd/3`, and opening anything in `/dev/fd` is `dup(2)`. Four
confirmations, all read rather than recalled — the device prototype gives it
major 40 minor 3 with link count 2, the device table names no driver, every
`cdevsw` slot for that major is `nodev` with a null `streamtab`, and the kernel
special-cases it in `open1()` *before* the permission check with `dup`'s body
written out. The manual page says it in prose too.

What makes fd 3 the terminal is `init`: open the tty as fd 0, push the tty line
discipline, then `dup(0)` three times. **"Controlling terminal" is a userspace
convention in V8, not a kernel fact** — so the launcher that starts this world is
its init, and has to arrange it.

Two things generalise past the instance. **A survey's citations decay
independently of its conclusions**: the same block cited a line number for the
line discipline that turned out to be a different driver entirely. And its
ordering argument — "the discipline has no bottom end so it cannot be exercised"
— was false *at the open path*, because that function never dereferences its
downstream queue and sends nothing. Only *traffic* needs a device below.
**Re-read the source a survey cites before building on the survey.**

### Two kinds of guard, because a patched file cannot have a hash

`stream.c` — Dennis Ritchie's stream engine — is imported **byte-identical**, and
the suite compares `git hash-object` against the recorded provenance, so an edit
is a test failure. That is the strongest claim available and it costs nothing.

`streamio.c`, the 1093-line syscall side, carries **two recorded LP64
deviations** and therefore cannot be guarded that way. **A file with deviations
needs a different guard, and "it has a porting note" is not one.** The suite
diffs it against pristine upstream and asserts that exactly one line was lost,
that it is the specific `sizeof` copyout, and that the second deviation added
exactly the declaration it was supposed to. That makes the deviation *list* a
test rather than prose — which is what a hash gives you for free and a patched
file otherwise loses entirely. Count removals and additions separately: the two
deviations here are not the same shape, and a first draft that assumed "two
changed lines" failed.

The tty line discipline followed, also byte-identical — and keeping it that way
is the whole reason its one missing number went where it did. It includes
`"tty.h"`, which is *not* the zero-byte make-timestamp file of that name in the
header directory but the per-configuration header `config(8)` generates from a
machine description that **was not shipped**; the config binary is a VAX
executable, so it cannot be regenerated either. There is no `#define NTTY`
anywhere in the upstream tree. So the number is this port's decision — and
because a quoted include falls through to the shim's directory, supplying it
took **no edit to Bell Labs' source**, which is exactly what keeps the hash
guard available.

The value is derived rather than picked: a slot is one discipline *attached to a
stream*, so the stream table bounds it exactly. On a VAX the number counted
configured terminal lines; it cannot mean that here, because the shim is
per-binary, so "how many terminals has this machine" is not a question one
process can answer.

### Writing the sibling is how you find the misdeclaration

The discipline needed one function that was not yet present: an eight-line
`max()`. Writing it found `min()` — sitting beside it, working, in use — recorded
in two files as having "no declared return type". Upstream puts the word
`unsigned` on the line above the name, both times. Nothing observable was wrong,
because every call is bounded by a 1024-byte block, and that is precisely why
the note survived for months. It would have been copied straight into `max`.

**Adding the second instance is what forces a declaration to be read instead of
recalled**, and that generalises well beyond this file.

### One keystroke, six layers, and why it had to be a driver

Importing the discipline was not the same as running it. Its open path worked
on a bare pair of queues, but the five functions that do the actual work —
input processing, line gathering, output conversion, signal generation, the
ioctl handler — compiled, linked, and had nothing driving them. A line
discipline is the *middle* of a stream. It needs a stream head above and a
device below, and it had neither.

The tempting shortcut is to stack a second module on top and watch what comes
out. It does not work, and one line of upstream says why: the input routine
sends data **upward** through its own `q->next` and flow control **downward**
through the write queue's, in the same loop. Anything sitting above sees the
first and is structurally blind to the second. So the missing piece was a
*driver* — a bottom end — and 83 lines of it.

With that, the stack is built exactly the way `init` builds one: open the
driver, register the discipline, push it between. Three layers, of which only
the bottom is ours. The stream head is Bell Labs', the discipline is Bell Labs'
and byte-identical, and what runs between them is 1985 code doing what it was
written to do.

The case worth keeping is a single keystroke. Press DEL on the terminal, and:
the driver delivers the byte upward as a message; the discipline recognises it
as the interrupt character; it flushes both queues and sends a signal message
up the stream; the stream head turns that into a process-group signal; the
shim's signal routine turns *that* into a real `kill(2)`; and a handler in the
test process runs. Six layers, three of them authentic, and the assertion is
not that a flag was set but that **the handler ran**.

Two smaller findings came with it, and both are the same shape as everything
else in this project.

The first: two `ioctl` commands that the return value cannot tell apart. Setting
the terminal parameters passes the request *down to the device*, and the
acknowledgement that wakes the sleeping system call is the driver's. Setting the
special characters is answered *by the discipline itself* and the device never
sees it. Both return zero. The only way to observe the difference is to ask the
far end whether it saw anything — which is a test you cannot write until there
is a far end to ask.

The second: an expectation that was wrong because the guard sits one line above
the loop. The output converter expands tabs into spaces, and the first version
of that test expected a tab to come out as seven spaces. It came out as a tab.
The expansion is conditional on a flag meaning *this terminal cannot do tabs
itself* — a fact about 1970s hardware, not a default. The measured answer was
right and the expected one was wrong, so the fix was two tests instead of one:
the literal tab a default terminal gets, and the expansion once the flag is set.
That is the same lesson as `max()` finding `min()`, arriving from the other
direction: **the answer that surprises you is the one to go and read the guard
for.**

### The bug a passing test cannot see

Running the LP64 auditor over the new driver — the project's habit of auditing
freshly written scaffolding, not just freshly imported source — turned up
something no test in the suite could have caught. Not because the suite was
thin, but because this bug is of a kind a passing test cannot distinguish from
correctness.

An `ioctl` on a stream is a message sent down and an acknowledgement sent back,
and the system call copies the reply into the caller's buffer using *the reply's
own length*. The stream head builds every such message at a fixed twenty bytes
whatever the command. The discipline's "set the terminal parameters" path
doesn't change that length. So the acknowledgement my driver sent back said
"sixteen bytes" for a structure that is six bytes long — a ten-byte write past
the end of the caller's object, on every single call.

Bell Labs spend one line on this, in each of two drivers: reset the reply length
before acknowledging a *set*, and deliberately fall through to the *get* case,
which must not reset it because it has something to return. I had not written
that line. The comment where it should have been even recorded the mechanism
correctly — "the system call copies the reply's length either way" — and treated
it as a reason not to care, when it is the reason to.

The part worth keeping is why no test caught it. The ten bytes written past the
end are *the same ten bytes* the system call read from that address moments
earlier, so the overwrite round-trips: memory ends up byte-for-byte correct and
every value anyone could check is right. A sentinel cannot see it. The only
observable is the fault — which only happens if the object sits within ten bytes
of the end of a mapping.

So the test arranges exactly that: the structure at the last six bytes of a
writable page, with the next page **readable but not writable**. Readable
matters, and it is the whole trick — the authentic twenty-byte over-*read* has
to keep working, so that the only thing which can fail is the write. It runs in
a child process, because the failure is a signal rather than a value. With Bell
Labs' line: clean exit. Without it: SIGBUS.

That turns a convention into a guard. And there is a coda: the *get* path still
copies eight bytes into the same six-byte structure — and that one is upstream's,
because their drivers leave the length alone there too. A VAX did exactly this.
Reproduced, not repaired.

Where the driver *lives* was the last decision, and it went against the obvious
one. It is test scaffolding, not part of the shim — because nothing in the port
consumes a tty driver. `/dev/tty` had already turned out to be a hard link to a
file descriptor rather than a device. Putting a driver in the shim would have
created a component with no caller, and that is the mirror of this project's
most repeated lesson: an unexercised rule cannot be seen to be incomplete, and
an unconsumed component invents a difference the kernel does not have.

### Writing down what is still dark, and finding the list wrong

With all eight functions running, the honest thing was to write down which
*arms inside them* still were not — six of them, flag-gated paths that a normal
terminal never takes. Writing the list paid for itself immediately, because two
of its six entries were wrong.

One claimed an arm was unreachable and cited the exact line that reaches it.
The note said a particular flush path "needs an ioctl the system call handles
itself" — as though handling it were the obstacle. Handling it *is* the
mechanism: the system call's own line puts the message straight onto the
discipline's queue. Accurate citation, reverse inference. That is the third
time this project has hit that specific shape, and the tell is always the same:
a sentence that cites something true and then draws the opposite conclusion
from it.

The other listed a delay flag that the file does not mention at all — a name
copied out of a header into a list of things supposedly in the source. One
`grep` settled it.

The four real ones were worth the trip. **A function written for this import
had never once executed.** It was the single name the discipline needed that
the shim did not have, and writing it is what caught a wrong comment about its
sibling. It has exactly one call site in the entire tree, inside a padding
delay for a Teletype Model 37, and nothing had ever taken that branch. It has
two cases now, because the function picks between two values and the terminal's
column decides which.

**And a delay flag named for the vertical tab is not triggered by a vertical
tab.** The parity table classifies that character as merely non-printing; it is
the *form feed* that gets the treatment — 127 ticks of silence, the largest
number in the file, because ejecting a page is the slowest thing a printer does.

### The bug that was three cases deep

The last of the six produced the most useful mistake of the whole exercise, and
it was mine twice over.

The case was meant to prove that an internal 256-byte buffer is invisible to a
program reading a longer line. The first version expected more than 255 bytes
from one read. It got 145. So the second version concluded the line must arrive
in *pieces*, and asserted that instead.

145 was not a piece. It was the leftover from a *different case*, three sections
earlier, which had sent a 401-character line and read it into a 256-byte buffer.
The 145 bytes it never collected sat in the stream until the next case read
them — which then reported a plausible wrong answer, and made the case after
*that* look broken for reasons that had nothing to do with it.

This project already knew this lesson in another form: an earlier tool that ran
every program in one directory kept "finding" crashes that were really programs
tripping over files their predecessors had left behind. A test has to be a pure
function of its own inputs. The same thing happens between cases inside a single
process when they share a stream, and it is harder to see, because there is no
directory to look in.

The fix was a helper that reads whole lines rather than buffers-full. With it,
the answer is 498 characters in a single read — which is the property the case
was after all along, and a better one than either guess: the buffer really is
invisible, because the read loops on the line delimiter rather than on message
boundaries.

### A filesystem image, and checkers that cannot check each other

`mkfs` writes a real V7 image; `icheck`, `dcheck`, `ncheck`, `clri`, `quot`,
`fsck`, `dump`, `restor` and `dumpdir` read and repair it. Ten programs that
were once written off as "raw VAX disks".

Getting there needed the on-disk widths fixed first (above), and it produced the
sharpest testing lesson in the project. Built without its `DIRSIZ` flag, `mkfs`
writes a **wrong image that every one of those checkers pronounces clean** — so
the group's own tools cannot guard the group, and the only real guard is
asserting bytes at known offsets in a generated image.

Verifying that the fix was a no-op needed three checks, and the middle one is
the reusable trick: layout measured from the V8 side; the generated image
compared byte-for-byte against **a same-binary, 1.2-seconds-apart noise floor**,
which came out the identical seven offsets, so nothing but the clock had moved;
and every differing tape byte classified by its position within the record — 14
in the date field, 14 in the checksum, zero elsewhere. **Compare artefacts
against what the clock alone does**, not against each other.

### Costing the next step before doing it, and finding it was the wrong shape

The plan's next step was one sentence: *the filesystem server — V8's own
`alloc.c`, `iget.c`, `nami.c`, `rdwri.c` over that image*. Four files. The way
this project sizes such a thing is to count the names a file calls but does not
define, because that number is what made the stream engine affordable — nine
names for 483 lines — and what made the tty discipline affordable at fifteen.

The four files call **47 names they do not define**. Six already exist.
Following the rest to whichever file defines them, and then following *those*,
converges at **42 files and 17,393 lines** — the virtual memory system, the
swapper, the trap handlers, the Unibus adapter, down to the registers of two
particular VAX memory controllers. That is not an import. That is the kernel.

But a transitive closure follows every call, including calls a filesystem never
makes. The explosion turned out to run through **exactly two functions**. The
biggest single dependency is the buffer cache, and classifying each of its
twenty-three functions by whether it touches the VAX's page tables gives
twenty-one that touch nothing but the interrupt-priority calls the shim already
has — and all eight VAX-memory names living in `swap` and `physio`, which a
filesystem server never calls. They have to *link*. They never have to *run*.

With those two treated as dead weight the real unit is **six files and 2743
lines**, needing nineteen new names, of which four are the panic stubs for the
dead pair. A week of work rather than a month, and the difference was an
afternoon of counting.

Two things the survey turned up that the one-sentence version could not. The
import **retires** hand-written code rather than only adding: three functions
the shim currently spells by hand are defined by one of the six, and a
thirty-line stand-in header exists whose own comment reads *"there is no buffer
cache here and no disk driver, so importing it would put a description of
hardware in the tree to obtain two constants"* — true when it was written, and
this step is precisely the thing that falsifies it.

And then the auditor found the one that would have cost days. `nami.c` compares
a directory entry's name against the name being looked up, and under
`#if DIRSIZ == 14` it does not call `strncmp` — it hand-unrolls the comparison
as four-plus-four-plus-four-plus-two bytes, spelled in terms of `long`. That is
exactly fourteen **because V8's `long` is thirty-two bits**, the same fact,
recorded in one line of the original compiler's machine description, that had
already made every on-disk structure in this port the wrong size. Here it is
eight-plus-eight-plus-eight-plus-two: it reads past both names, fails to match
a name that is present, and **every path lookup in the filesystem returns "no
such file"**. Total failure, from a comparison that looks like an
optimisation — and the tempting fix, changing `DIRSIZ`, makes the symptom
vanish by silently changing the on-disk format.

### Two sentences in the documentation, checked

Neither of the last two findings came from reading code. They came from running
an assertion the project's own notes make about the tree, which nobody had run.

The notes document half a dozen `grep` sweeps — the ways to re-find each bug
class after importing something new. Every one spelled its separator `[ \t]`,
which looks like "space or tab" and is a GNU extension. The POSIX reading is
three literal characters: space, **backslash**, and the letter `t`. Which you
get depends on which `grep` is installed. On this machine, one thing; on a
stock Mac and on the CI runner, another:

| the sweep | stock `grep` | this machine |
|---|---|---|
| typed yacc tokens | **3** | 62 |
| `#include` of a non-header | 71 | 69 |

Fifty-nine of the sixty-two typed-token declarations in the tree put a *tab*
after `%token`. So the sweep written to catch a specific pointer-truncation bug
would, on a stock Mac, find three of sixty-two — and the file it would miss
includes the exact line where that bug was found. The other sweep fails in the
opposite direction, and shows the backslash doing the damage: it matched the
escaped quote inside `printf("# include \"mfile2.h\"")`, and that same escaped
quote is why the filter meant to discard such lines did not discard them. One
stray character, two faults cooperating, and a plausible number.

The second sentence read: *six spellings of one number*, about `DIRSIZ`. There
are ten and they are four numbers. One of the disagreements is correct and
interesting — the kernel header says 14 on purpose, because it describes a disk
record. One was a live bug.

`sh` has a spelling corrector: mistype a directory in `cd` and it offers the
closest name. It keeps the best candidate in a buffer sized `DIRSIZ+1`, filled
by a copy with no bound — exact on a system where a name cannot exceed
fourteen characters, and an overflow here, where this port raised the limit to
254. Measured under a sanitizer against the untouched source: a one-byte write
past the end.

One byte, and the reason it is only one byte is the interesting part: **the
bound is in a different function**. The scoring routine that decides whether to
copy will not score a name more than one character longer than the guess. So
reasoning about the copy alone gets the severity wrong in both directions — it
is not unbounded, and it is not harmless. The copy that fills the *other*
buffer **is** bounded, which is why anyone auditing this function for exactly
this bug class finds a bound and stops looking.

It is also easier to hit than "one byte" suggests, because the corrector walks
every component of the path and an exact match scores zero and copies too. The
first probe run tripped on a path nobody had constructed for it: a
fifty-two-character directory in the temporary tree, truncated to a
fourteen-character guess, sitting beside a real fifteen-character directory
that differed by one character.

And the fix carries a trap the project has met before. The path buffer is 128
bytes, and 128 is not an independent number — it is written into the code as
`128 - DIRSIZ - 2`. Raise `DIRSIZ` alone and that becomes negative, the guard
fires on the first pass, and the corrector returns "no suggestion" forever: no
crash, no message, a feature silently gone. That is exactly what happened to
`mv` when the same constant relationship was broken there. Both numbers move or
neither does.


### One file, two paths, and a measure that could not see it

With the survey done, the import began: six authentic kernel files, 2743 lines,
into `src/sys/`. The survey had costed the twenty headers they need by line
count and by how many times each mentions the VAX — sensible measures, and
between them blind to the thing that mattered.

Fourteen of the twenty are **the same file the port has already imported**.
V8 ships each kernel header at `/usr/sys/h/` *and* `/usr/include/sys/`, byte
for byte; `git hash-object` gives one hash for both. So the question was never
"what do these headers cost" but "the port patched some of these years ago, for
the userland — which copy should the kernel see?"

Three of them are on-disk records, patched to exact widths because a VAX wrote
four bytes where this machine writes eight. Importing the pristine kernel copies
would have given the kernel an eight-byte timestamp in a superblock that `mkfs`
writes with four — the same bug an earlier phase had already found and fixed,
reintroduced on the *other side of one disk*, where nothing could have caught
it: every checker in the tree would have used the patched header and only the
kernel the pristine one. Two self-consistent halves disagreeing by hundreds of
bytes.

The rule that came out of it is stronger than "patch it in both places", because
that is a rule vigilance has to keep: **a record written to a disk gets exactly
one declaration in the tree.** The kernel side now holds three headers that
contain one `#include` each.

### The instruments, again — and one of them was the compiler

Then the six were compiled, to find out what they actually need rather than what
a name count predicted. The first run said every file had exactly **one error**.
Uniform, plausible, and entirely an artefact: the shell had passed the whole
flag string as a single argument. The second run said 19, 20, 20, 20, 20, 5 —
which reads like a shared cause and is `clang`'s default ceiling of twenty. Two
of the 20s were really 45 and 49.

Both faults point the same way, and it is worth stating as a habit rather than a
war story: **a set of measurements that clusters at a round number is telling
you about your instrument, not your subject.** The real figure was 231, and it
collapsed to four causes, of which importing one authentic header removed 57.

Verifying *that* needed a rule the project had already paid for twice. The new
header wins an include that three working files resolve, but it appears in no
dependency file — so `make` has no reason to rebuild them, and reports success
having compiled nothing. A stale object does not look like a build problem. The
three were touched and rebuilt explicitly before the claim was made.

### Nineteen names, and the twentieth was hiding in a macro

A subagent was set to specify each of the nineteen kernel services the import
needs. Its central claims were then re-read at source — the project's rule, and
the report invited it by citing everything. All six checked held, and four
changed the plan.

The best of them: there are **twenty**, not nineteen. `plock` is called three
times by one of the six files and defined in a seventh, and it was invisible
because a header defines it as a *macro* — and one of the six includes that
header while another does not. The same name is a macro in one file and a real
function call in the next. A survey that reads them in the wrong order sees no
symbol at all.

Two others are the sort that would have cost a day each. `fubyte`, the primitive
that fetches one byte from user space, is not a C function on a real V8 at all —
a `sed` script rewrites it into VAX instructions before the assembler runs — and
the instruction it becomes is `movzbl`, which **zero-extends**. Implement it the
obvious way, with a signed char, and byte `0xFF` becomes `-1`, which the caller
reads as a fault: a write that silently truncates at the first high byte. And
its sibling `subyte` must return exactly zero on success, because upstream
writes `if(id ? suibyte(p,c) : subyte(p,c) < 0)` and `?:` binds looser than `<`
— so on one of the two branches the raw return value *is* the error test.
Upstream is correct given its own convention. The convention is the thing to
preserve, not the parenthesisation to fix.

And a footnote that is the class this project collects, in its purest form.
Sweeping the six files for signal names returned the word SIGNAL out of an
English comment — plus two hits inside the header that documents this exact
trap, one of them the sentence *"grep over it yields exactly those two plus the
word SIGNAL"*. The sweep matched a comment warning that the sweep matches
comments. Knowing the rule does not filter the output. Only filtering it does.

### Then it built, and the build had opinions

Compiling those six files was supposed to be the mechanical part. It found the
second-worst bug of the phase and it found it as a *warning*.

`alloc.c:34` — upstream's numbering; the port's comment moved it — declares
`register long *p`, and `p` walks `s_bfree` — the
superblock's free-block bit map. A year earlier, a different phase of this port
had narrowed that array to four-byte words, because it is written to a disk and
a VAX wrote four bytes there. The array narrowed. The pointer did not. So
clearing one block's bit is an eight-byte read-modify-write over a four-byte
word, which rewrites the next thirty-two blocks' word with whatever it read;
and the scan loop strides eight, covering half the map before running nine
hundred and sixty-one words past the end of the buffer.

It is the same one line of cause as the name-compare bug — Bell Labs' own
compiler defining `NOLONG`, "map longs to ints" — and Bell Labs say so five
lines below the declaration, in a comment reading `BITS PER LONG`. The
difference between the two is only how loudly each announced itself. The name
compare was an error and nothing worked. This one was two warnings in a build
that succeeded, and it would have corrupted a free-block map on the first write
and been blamed on the program that made the filesystem.

Then the linking. The plan costed four name collisions between the kernel and
the C library. There are nine, and the five extra ones split cleanly by kind.
Six are function-against-function, and the linker catches those the first time
you link — that is what a duplicate symbol *is*. Three are a **variable against
a function**: the kernel's clock variable `time` against the `time(2)` stub, an
`int timezone` against a function returning the zone's *name*, a mount table
against the call that fills one. In C, a variable declared the 1985 way is a
"common" symbol, and resolving a common against a real definition is exactly
what a linker is supposed to do. So those three are silent. The kernel's clock
would simply have become the address of the `time()` function, and every
timestamp written to disk would have been a code address.

Nothing in the build could have told me. `nm -g` on the archives did.

And the same instrument caught something worse, which I had caused myself. The
build flags turn off "implicit function declaration" warnings, because the
imported code is 1985 C and every line would trip them. That suppression was
argued for *declarations*. It also covers a missing **macro** — so when I gave
the header its constants and forgot its geometry macros, `BSIZE(dev)` did not
fail to compile. It became a call to a function named `BSIZE`. Fourteen of
them, sitting in the object file as undefined symbols, in a build with no
errors and no warnings. I found them by subtracting what the archive defines
from what it undefines, which is the only instrument left when you have told
the compiler not to speak.

There is a lesson in the shape of that and it is not "be careful". It is that a
suppression is a *scope*, and the scope you argued for is not the scope you
get.

### A file that answers your question, and has been dead for years

The kernel's filesystem switch is a table of function pointers, one row per
filesystem type, and its obvious source is a file called `dev/conf.c`. Row zero
names a function called `rnami`.

`rnami` is not defined anywhere in the V8 kernel. Not in that directory, not in
the tree, nowhere. What *is* there is a comment in `nami.c`, directly above a
function called `fsnami`, reading: `USED TO BE rnami`.

I went looking for how the file could still name it, and found Bell Labs had
written the answer down. There is a note in the configuration directory listing
the differences between the old build system and the new one, and its eleventh
line is: *"dev/conf.c is no more. config makes a conf.c for each machine."* Two
lines later it lists the source files "changed a little to make names regular"
when that happened, and `nami.c` is on the list.

So `dev/conf.c` is a fossil. It predates the rename, it is generated per-machine
now, and every citation to it — including the one in my own plan — is a
citation to dead code. The live description is a different file entirely, one
this project already relies on for the terminal driver, and reading its
filesystem section gives `fsnami`, which is the name that exists.

This is the second fossil to cost me. There is a V7 device driver still sitting
in the kernel source that cannot compile and is in no build list, and the note
I wrote about it a while ago says a vestigial file that answers the question you
are asking is the worst kind of evidence. It was right, and the rule needed
sharpening rather than repeating: when two upstream files disagree, do not
reason about which looks more current. **Find out which one the build reads.**

### And then it ran

The six files compiled and linked, and that felt like the end of something. It
was not. A build proves the declarations line up. It says nothing about whether
`bmap` can walk an indirect block, or whether `namei` can find a name — and
until this week not one line of that code had executed.

Making it execute needed three things. A block driver, which went into the test
probe rather than the shim, because nothing in the port consumes one and a
component with no caller invents a difference the kernel does not have. A place
for the kernel's tables and its startup, which upstream splits across three
files — one holding size formulae compiled with a `MAXUSERS` that was never
shipped, one carving storage out of a VAX's virtual address space, and one
holding the code. And a mount, which meant transcribing `allocmount` from a
system-call file, including a line where Bell Labs wrote `!mp->m_flags &
M_MOUNTED` and got the precedence backwards — correct only because the flag they
meant is 1 and it is the only flag the structure has.

Then `mkfs(8)` wrote a 2000-block image with a 28000-byte file two directories
down, and V8's kernel opened it by name and handed back the bytes. `cmp` says
they are identical to the file mkfs was given. Twenty-eight blocks is past the
ten addresses that fit in an inode, so the walk went through the indirect block
too. The writer is 1985 code; the reader is different 1985 code; neither knows
the other exists.

### A header died and nothing said so

While wiring that up I went looking for `struct buf` and found two headers with
the same name — the authentic one, and a small one this project wrote to give
the stream code two constants without importing a hundred lines about a VAX
buffer cache. Its comment explained itself clearly and named its consumer.

The comment was false, and had been for exactly one commit. When `bio.c` was
imported it brought the real `buf.h` into the tree — and a quoted include tries
the including file's own directory first, so the stream code's unchanged
`#include "../h/buf.h"` silently began resolving to the authentic header
instead. No line in either file changed. A **third** file arriving did it.

What makes this one worth telling is that there was a test. A dependency case
named `our buf.h -> streamio.o`, green every run. It was green because the
*build* edge was real — the makefile did list our header as a prerequisite — and
the case never checked that the header it named was the header the compiler
opens. The only instrument that can tell the difference is `clang -M`, because
the source line reads the same either way. Both remaining includers turned out
to use neither constant. The file was dead, and the guard over it was
auditing nothing.

### Bell Labs' comment was stale against Bell Labs' code

There was a note in this project's own shim saying that `getfs()` panics with
the message `no fs`. The code says `panic("getfs")`.

I assumed I had invented the wrong string. I had not. `no fs` is upstream's
own words, in the comment block twelve lines above the function, listing the
panics it can raise — a comment that stopped matching the code beneath it
somewhere between V7 and V8, and that this project read and wrote down as
behaviour.

The rule about recorded diagnoses being hypotheses is one I already had. What I
did not have is that it applies to the imported half's comments as well, and
that those are the *most* dangerous instance of it: the fidelity contract
forbids editing them, so they are the one body of prose in the tree that nobody
ever reads with an eye to whether it is true.

### The citation that invalidates itself

The same day, a subagent audit turned up sixteen stale line-number citations
across the tree. Eight had one cause. When I documented the free-map bug in
`alloc.c` I wrote a forty-three-line comment above the declaration — and every
line I had cited moved down by forty-three. The first draft's `:70` now pointed
into the middle of the comment doing the citing.

Correcting it moved them twice more, because each correction added lines, and
only the third measurement converged. Which is the tell that prose was the wrong
container: a line number written inside the file it describes is invalidated by
the act of writing it. Those five citations are a test now, mutation-verified —
insert a line anywhere above the code and all five go red.


### The write half, and a mutation that killed the shell

Reading proves the layout is understood. Writing proves the *bookkeeping* is,
and it reaches code no reader can touch: `bmap`'s allocating arm, `alloc()` and
`free()`, `ialloc()` and `ifree()`, `itrunc`, and `namei` with a create flag.
So the probe grew a second half — overwrite a block that exists, extend into one
that does not, extend past block 9 so the indirect block has to be made too,
create a file *by name*, write several hundred blocks into it, delete it.

The instrument is the superblock's own accounting rather than a count of disk
writes, because a disk-write count depends on when a thirty-two-buffer cache
happened to evict something, and that is a fact about the machine. `s_tfree`
and `s_tinode` are what the allocator maintains. The strongest single assertion
is that after the delete they are exactly what they were before the create: an
off-by-one anywhere in five functions moves one of them.

The case I most wanted was the free list. `alloc()` hands out blocks from a
cache of numbers in the superblock, and only when that cache empties does it
follow the chain — read the block it just gave away, and find the next batch of
free block numbers written *inside* it. That is V7's on-disk format, a 716-byte
record, and `mkfs` wrote one in 2025 for a 1985 kernel to walk. Reaching it
means allocating more blocks than the cache holds, so the probe reads the count
at run time and asks for that many plus twenty-four. Every one of those writes
succeeding is the proof, because without the refill the allocator returns "no
space".

Then the first write failed. All of them did, with `EMFILE` — "too many open
files", which is not what a size limit sounds like, and which sends you to the
file table. `writei` compares the write against `u_limit[LIM_FSIZE]`, and a
u-area that has never been initialised has that array all zero, so `0 + 1 > 0`
and every write to a regular file is refused. The read half had never touched
it. The fix is thirty lines transcribed from Bell Labs' `main()`, in the file
that already stands in for `main()`.

And then the mutation that was supposed to prove the new test could fail killed
my shell.

Not "failed the suite" — killed it. No failing case, no summary line, no output
at all, because the harness had been piping to `grep '^FAIL'` and there was
nothing to grep. What happened is that `writei`'s refusal is not silent: it
sends `SIGXFSZ` first. On a VAX that sets a bit in the process table. Here
`psignal` is a real `kill(2)`, because there is one process and it is a Mac
process. The probe stands up a filesystem, not a process description, so it
never calls the routine that records which host process it is — leaving the pid
zero. The map returned zero. The guard was `if (hp < 0) return`, which zero
passes. And `kill(0, sig)` is not a no-op. It signals every process in the
group.

Eleven lines below the function that did this, its sibling `gsignal` carries
exactly the guard that would have prevented it, with a comment explaining that
group zero is not a group. This project's most repeated failure, in its purest
form: the fix landed on one line, and the line beside it kept the assumption.

The case that guards it now asserts that the process is still alive after an
over-limit write. That is a strange thing to assert and it is the only thing
you *can* assert about a signal that must not arrive.

### Three programs that do not share the probe's beliefs

A probe that writes and then reads back is one program agreeing with itself.
That is worth something and it is not worth much, because a misunderstanding
lands on both halves.

So the last thing the suite does is hand the finished image to `icheck`,
`dcheck` and `fsck` — three programs Bell Labs wrote to inspect filesystems,
which know nothing about any of this. `icheck` walks the image and recomputes
the block accounting. `dcheck` walks the directory tree and recomputes the link
counts. `fsck` does both and *repairs*, so its silence is the strongest of the
three, and it is asserted twice: once on what it printed and once by `cmp` on
the bytes, because a checker saying nothing and a checker changing nothing are
different claims.

The mutation that justified all of that broke the free-list refill so that
handed-out blocks repeat. **Every case in my probe stayed green.** The writes
succeeded, the accounting looked right from inside, the bytes read back. Only
`icheck` and `fsck` caught it — a duplicate-block bug is invisible to a program
that is both the writer and the reader, by construction.

Seven mutations in all. Two of them turned my own comments red rather than my
code. One said the zeros in a file's hole come from a cleared disk block; they
come from a buffer attached to no device at all, because `bmap` answers minus
one for an unallocated block and `readi` has a whole arm for that. The other
said the read path dirties no buffer; it dirties one per lookup, because `readi`
sets the access-time flag and `iput` writes the inode back. Both claims were
plausible, cited nothing, and were false. Making a mutation fire is how you find
out which of your sentences were decoration.

### The auditor read the sentences, not just the code

I then sent the LP64 auditor at the diff — the subagent this project keeps for
the width and pointer hazards. It came back clean on every one of them, with
the measurements rather than the verdicts, and two of those are worth keeping:
that `writei`'s size check is *stricter* here than on a VAX, because upstream's
arithmetic was 32-bit unsigned and wrapped where this is 64-bit signed and
cannot; and that `u_error` is one signed byte with room for exactly the errnos
the kernel can put in it, the largest being 62.

What it found instead were five defects, and **four of them were sentences.** A
comment describing `update()` and a `bflush()` as a sequence, when `update()`
*ends* with `bflush` — so the second call was dead and the prose was what named
it. A claim that three checkers were "bounded in time by their deadlines" when
only one of them had one. A count of three that was one, because it counted
renamed names while describing declarations. Two declarations with no call
site. And one real bug: a `long` read uninitialised on the path where two
things fail at once, so the strongest case in the section would report an
arbitrary answer on precisely the run that needed a diagnosis.

Its best observation was not a defect at all. The read-only check I had
restored that morning is *read* on every create and can never be *taken*,
because the mount code sets the flag to zero and nothing else ever sets it. A
guard that has never been seen to fail. Making it fire meant setting the
superblock field by hand — legitimate for the same reason the probe registers
its own driver, since the code that would normally set it is a mount call this
port has not imported — and asserting both halves: refused with `EROFS` when
the flag is on, and *succeeding* when it is off, because otherwise the case
passes against a permission check that refuses everything.

And then the cleanup for that new case created a dangling directory entry — I
freed the inode and left the name pointing at it. All three new cases went
green. `fsck` did not: *FILE SYSTEM WAS MODIFIED*, and a byte difference at
180289.

Which is the whole argument for the acceptance test, arriving from the
direction I had not expected. I put those three programs there to catch the
kernel lying. Within an hour they caught the probe.

### `cat` has a buffer called `buf`, and so does the kernel

The last piece was supposed to be small. Everything above is reached by calling
the kernel's functions directly from a test harness; what was missing was
*dispatch* — a fourth filesystem type in the shim, so that an ordinary V8
program's `open("/mnt/something")` lands in `namei` instead of on the Mac.
Three types already existed. This was going to be the fourth.

The fourth type has to call `namei` and `iget` and `readi`, so whichever
program opens the file has to be linked against the kernel. I tried it on the
smallest program I could think of. `cat`.

```
ld: warning: tentative definition of '_buf' with size 4096
    from bin/cat.o is being replaced by real definition of
    smaller size 8 from libv8kern.a[18](main.o)
```

`cat.c`, line 10, is `char buf[BLOCK]` with `BLOCK` defined as 4096. The
kernel's `main.c` has `struct buf *buf`, a pointer to the head of the buffer
cache. In K&R C, `char buf[4096]` with no initialiser is a *tentative*
definition — a common symbol — and a linker presented with a common on one side
and a real definition on the other is supposed to prefer the real one. That is
not a bug in `ld`. That is `ld` working.

So `cat`'s four-kilobyte buffer became an eight-byte pointer, and
`read(0, buf, 4096)` began writing four kilobytes into it.

I ran it. It printed its input back, byte for byte, and died of SIGSEGV.

Then I linked it a second way — not forcing the whole archive in, just letting
one undefined reference pull the kernel object in the way a real build would.
Same warning, same eight-byte `buf`. This time it printed its input back, byte
for byte, and **exited 0**.

The difference is where the other 4088 bytes landed. `nm -n` on the binary:

```
000000010001d588 D _buf
000000010001d590 D _buffers
000000010001d598 D _nbuf
```

Eight bytes past `buf` is `buffers`. Sixteen bytes past is `nbuf`. `cat`'s first
read overwrites the buffer cache's own pointers and returns success.

I swept the rest of the world: 297 program objects against the archive's 266
names. **Fifty-six collisions, across twenty-nine programs, on twenty-seven
distinct names — and twenty-five of the fifty-six are silent.** The names are
not exotic. They are `buf`, `bread`, `alloc`, `bmap`, `tty`, `file`, `bwrite`,
`getblk`, `iput`, `itrunc`, `panic`, `copyin`, `copyout`. `buf` alone hits eight
programs. The filesystem checkers are the worst affected, for a reason that is
almost funny: `icheck`, `dcheck`, `fsck`, `mkfs`, `clri` and `restor` are
programs that reimplement the kernel's algorithms, so of course they use the
kernel's variable names.

There is a way to hide symbols — merge the archive into one object and export
only the handful of names beginning `v8k_`. It works for twenty-two of the
twenty-seven. The other five are common symbols, and `ld` will not make a common
symbol private. Two commons of the same name merge silently, taking the larger
size, with no warning at all. So the mitigation converts the loud half of the
problem into the quiet half of the problem.

And then, reading back through the shim for something else, I found this comment
sitting in `vfs.c`, written months earlier for a different reason:

> IT DOES NOT SURVIVE A PROGRAM REPLACING ITSELF, and that is fine today and
> will not be later.

Which settles it independently of every symbol. A descriptor into a mounted
image would be an inode pointer and an offset held in the program's own memory.
`exec` throws that memory away. So `cat /mnt/a > /mnt/b` — where the shell opens
the destination and `cat` inherits it — cannot work in the client, no matter how
the linker is persuaded to behave.

Two roads, and they arrive at the same place: the kernel has to be in a
different process from the programs, which is what the plan said the wire
protocol was for, filed under "eventually". It is not eventually. It is the
requirement, and the mount is the thing that forces it.

I did not build the mount. What I built instead is the sweep that killed it,
as a test — because the argument for the server is a measurement, and a
measurement nobody re-runs is just a story I told myself in August. It asserts
two things: that no V8 binary links the kernel archive, and, more generally,
that no program's common symbol has resolved into a library's text. Six such
pairs exist in the live build today — `od`'s `max`, `dc`'s `log10`, `mkfs`'s
`utime`, `nroff` and `troff`'s `nlist`, `sh`'s `tmpnam` — and all six are
correct, purely because nothing currently pulls the library member in. I proved
the guard can fail by relinking `od` so that the library's `max()` wins, and
watching it go red.

The thing I keep having to relearn is that the interesting output of costing a
step is not always the step.


### And then the server, which the costing had already designed

Once the mount has to be a separate process, most of the design is settled by
things already measured. The kernel is single-threaded because it keeps its
state in a global u-area, so there is no choice about threads. It is the sole
authority for the buffer cache, so there is no choice about forking per
connection either. What makes one process *sufficient* rather than merely
necessary is a property the probe had relied on for two steps without anyone
naming it: the image driver is synchronous, `iodone()` runs inside `strategy`,
so `iowait()` finds `B_DONE` already set and never sleeps. Every request can be
carried to completion between two `poll()` returns.

So: `v8fsd`, a host binary that links the kernel archive, holds a disk image
open and speaks plain 9P2000 on a Unix socket. Attach, walk, open, read, stat,
clunk. The write half answers `EROFS` for now — the kernel underneath it works,
step 5d did all of it, so that is a boundary in the server rather than a gap in
the port, and splitting it the way the read and write halves were split keeps a
failure attributable to one of them.

It reads a file two directories down, 28 blocks long, through `namei` and
`bmap`'s indirect arm, over a socket, and `cmp` says the 28000 bytes are the
ones that went in.

Three things surprised me, and only one of them was about 9P.

**A driver set belongs to a configuration, not to a library.** Moving the block
driver out of the probe was overdue — it finally has a consumer — and the
obvious place was the kernel archive. That instantly broke a test asserting the
archive imports exactly `memcpy`, `setjmp` and `longjmp`, because a block
driver does host I/O by definition and brought `pread` and `pwrite` with it.
The guard was right and my placement was wrong: `config(8)` is what chooses a
driver set on a real V8, and the object belongs on the link line of whatever is
being configured. The nice consequence is that the probe and the server now
share one driver, so the probe's 236 cases are coverage for the server's block
layer.

And then removing it from the archive didn't remove it from the archive.
Dropping an object from a list leaves the target newer than everything left in
it, so the rule never re-ran. Make has told me this before, in the other
direction, and the Makefile even has a comment about it.

**A trap that a comment predicted, walked into for the first time.** The kernel
headers rename thirteen names aside — `access` becomes `v8k_access` — and there
is a header that undoes all thirteen so a file can also have the host's
`<unistd.h>`. The probe's comment warns that this makes `free` and `ialloc`
belong to libc, and that calling `free(dev, bno)` "would compile and hand a
device number to the C library's allocator". I wrote `access(ip, IREAD)`. It
compiled, because K&R, and asked the operating system whether a path built out
of an inode pointer was readable.

**And a case that could not see what it was named for.** The stat case checked
that the server reported the file's name, and it did — `hello`, every time,
including while the length came back as 10248 against the 27 bytes `mkfs` wrote.
9P carries the name out of the fid, which is the name *the client sent in the
walk*. A server that had walked to entirely the wrong inode would still print
it. The field that identifies the file is the qid path, which is the inode
number out of the directory entry, and `ncheck` will say independently what it
ought to be.

The 10248 turned out to be the same lesson a third time. The 9P cases run at the
bottom of a suite whose top hands the image to `fsprobe`, and `fsprobe` writes
to it — that is what step 5d does. So the server was reading a filesystem an
earlier section had modified. I have now had this bug between two programs
sharing a directory, between two cases sharing a stream, and between two
sections sharing a file.

### `cat /mnt/hello`

The server answered a probe. Making it answer `cat` took one idea, and the idea
arrived as a problem: where does the file offset live?

Not in the program. A file offset in the client's memory is wrong three
different ways, and all three are ordinary Unix. `dup` gives you two
descriptors and *one* offset, so two rows in a client table would drift apart.
`fork` gives parent and child one offset, so a copied table drifts too. And
when a program replaces its own image the offset survives while every table in
its address space is destroyed — which is the thing a comment in the switch had
warned about a year earlier, in the words "that is fine today and will not be
later".

They are the same fact three times. The offset does not belong to the
descriptor or to the process; it belongs to the open file description — the
`struct file` a kernel keeps. And there is an object here with exactly those
properties, which I had been looking straight at: a socket is shared by `dup`,
shared by `fork`, and survives the image being replaced.

So: **one connection per `open`**. The connection *is* the open file
description. The fid becomes a constant, because a connection carries exactly
one open file. The offset goes on the server, where the open file description
already is. And the client ends up holding no per-file state at all — which
means an inherited descriptor works in a program that knows nothing about it,
not because anything was arranged, but because there is nothing to arrange.

That immediately ran into the one thing 9P does not have. There is no seek
message. There is no seek message because Plan 9's *kernel* held the offset, in
the channel behind the descriptor, and handed every read an absolute one. 9P is
a `pread` protocol because the thing above it is a kernel — and the thing above
it here is a C library linked into `cat`. This is a specific and slightly
delightful kind of discovery: not that the protocol is wrong, but that it
encodes an assumption about its own environment which this environment does not
satisfy.

The extension is one concept. A fid has a cursor; a read at the all-ones offset
uses and advances it; any other offset is 9P's own `pread` and does not touch
it. I wrote in the header that a conforming client therefore could not tell
this server from a conforming one. The existing probe refuted that inside the
hour: it reads a directory at 2^64−1 on purpose, to prove an unsigned-offset
crash guard is still there, and 2^64−1 is now the sentinel. The claim I can
actually make is narrower — invisible at every offset from which a conforming
client could read a byte, and 2^64−1 is not one of those. It is a better
sentence, and I did not write it; a test written weeks earlier did.

Then `cat /mnt/hello` printed the file, `cat /mnt/sub/deep` returned 28000
bytes that `cmp` said were the ones `mkfs` was given, and
`sh -c 'cat < /mnt/hello'` printed **nothing**.

That last one is the case the whole design exists for — the shell opens the
file, dups it onto standard input, and replaces itself with `cat`. And the bug
was mine, three lines above a comment that had already diagnosed it. Closing a
descriptor sent a `Tclunk`. But a clunk destroys the fid, and the fid belongs
to the connection, which every dup shares — so the shell, doing the ordinary
thing, clunked the file out from under the descriptor it had just made. A clunk
is not `close(2)`; it is the *last* close. The right number of them is zero:
the kernel drops the connection when the last descriptor goes, because the
kernel is the thing that knows the reference count, and the server frees
everything when the connection ends.

The comment beside the call said so. It said "dropping the connection is what
actually releases the server's fids" — and then called the clunk "politeness".
The bug was inside the sentence explaining why it was unnecessary. I have
written that shape into the project notes perhaps a dozen times now, always
about someone else's code or my own from months ago, and it turns out to be
just as easy to do while typing the explanation.

Two smaller ones came out of the same run, and both are the same species. `ls`
said `/mnt unreadable`, which is what you get when `open` fails, which is what
happens when the directory snapshot cannot be built: a directory read carries
bare stat structures, and only `Rstat` wraps its single stat in a second length
field. I had put the second length in both places. And a directory's reported
size has to be the length of the V7 records the shim will hand out, not the 64
bytes the image charges — a rule this port learned once already, wrote down,
and applied to exactly one filesystem type. A new type implementing the same
interface does not inherit the fix.

The last piece was containment, and it is where I nearly wrote a test that
proved nothing. Eleven syscalls — `unlink`, `chmod`, `mkdir`, `utime` and the
rest — have no slot in the switch at all; they were always passthrough, which
was survivable while every filesystem answered out of the jail directory. With
a mount they stop being containable: the path reaches the host verbatim, so
`rm /mnt/x` asks macOS to unlink `/mnt/x`. They refuse now. But my test asserted
that `chmod 777 /mnt/hello` exits 1 — and it exits 1 either way, because this
machine has no `/mnt` and the host's `chmod` fails too. The guard and the
absence of the directory were indistinguishable. The fix was to mount over a
directory the host really does have, holding a file with different contents,
and assert two things the absent directory cannot fake: that the read is
shadowed by the image, and that the host file's mode is unchanged.


### The probe, and the mutation that would not fire

All of those cases drive the client through the shipped binaries — `cat`, `ls`,
`tail`, `wc`, `sh`. That is the claim worth making, and it is also the reason
three things had no test at all: nothing in a 1985 userspace calls `fstat` on a
directory descriptor, nothing seeks backwards from the current position, and
nothing dups a descriptor purely to prove two of them share one offset. The
last of those is the *central* claim of the design. It followed structurally
from the offset living on the server, and following structurally is not the
same as being true.

So I wrote a probe: the shim's own source compiled into an ordinary host
binary, calling `v8s_open` and `v8s_read` directly, run against the same server
under the same mount. It found four things I was not looking for.

The first was the guard I built it for. Deleting the line that fixes a
directory's reported size had left the suite green — and the code is not dead,
it is a rule this port learned the hard way. What was missing is that nothing
had ever asked *both* questions. `stat` reports what the image charges for a
directory; `fstat` reports what `read(2)` will actually produce. They
deliberately disagree, and the disagreement is the observable. Nobody had put
them side by side.

Which is how I found that the pair of numbers in my own comment — "64 bytes on
the image against 768 of records" — describes no directory in existence. 768 is
three records and belongs to the subdirectory; the root has four entries, so
its pair is 64 and 1024. Neither number is wrong on its own. The sentence is
arithmetically impossible, because 64 bytes of 16-byte entries cannot be three
records, and I had never done that arithmetic because two plausible numbers
side by side read as one measurement. The test now asserts the *ratio* over two
directories of different sizes, which no transcribed pair can satisfy by
accident.

The second was an errno. V7's `namei` distinguishes "no such file" from "not a
directory" — two answers, one line apart — and so does my server. But 9P's
reply for a walk that stops part way is silent about *why*, so the client
flattened both to `ENOENT`. The reason was in the message all along, in the
qids I was throwing away: the last one describes what the failed component was
looked up in, and if that is not a directory the answer is `ENOTDIR`. Fixing it
needs three test cases rather than two, and the middle one is the whole point —
a missing name inside a *real* subdirectory takes the same code path as a walk
through a file, so a client that simply always said `ENOTDIR` would pass the
obvious case and fail nothing.

The third I found by accident, and the accident is the fourth.

I had a mutation that would not fire. The server's seek code checks for
overflow before adding rather than after; reverting it to the broken form left
all 525 cases green. This project has a rule for that — a mutation that does
not fire means the test is vacuous or the code is dead — and here it was
neither. Every overflow reachable in that function wraps to a negative number,
so the broken version arrives at the same refusal. It gets there by executing
undefined behaviour, which is precisely the problem: a compiler is entitled to
assume the overflow cannot happen and delete the check. No behavioural test can
see that, ever.

The instrument for it is a sanitizer, and it turned out to be cheap enough to
keep: the same server built a second time with UBSan, with the suite's traffic
run through it. Silent today; with the guard reverted, it prints the overflow
and dies. And because it dies, the client on the other end came back with exit
status 141 — which is 128 plus 13, which is SIGPIPE.

That is the fourth finding, and it is the one I would not have reached any
other way. The connection is a socket. The program using it is `cat`, which has
no idea it is talking over one. When the server died, the next request raised
SIGPIPE and *killed* the program — where a V8 machine whose disk stops
answering gives you an I/O error. A transport had leaked its signal semantics
into a filesystem. The fix is one socket option, and it has to be the
per-socket one rather than ignoring SIGPIPE process-wide: a V8 program in a
pipeline must still die when its reader goes away, because that is how
`yes | head` terminates.

Nine mutations now, and nine of them fire.

And an auditor read the result and found that in moving the legitimate case out
of one line, I had left its errno behind: the two protocol faults that remain
on that line were still being reported as "no such file", four lines above a
new line correctly calling the same class an I/O error. The rule it violates is
stated in the same file, in a comment I wrote. The fix lands on one line and
the line beside it keeps the assumption — I have recorded that shape a dozen
times, and it is no less easy to do while typing the thing that documents it.


### A predicate that answered a different question

The jail resolves a path by asking whether the rootfs has the name, and it
asked with `access(path, F_OK)`. But `access` follows the last component — so a
symlink inside the jail whose target does not exist reads as *absent*, the path
falls through unresolved, and every operation on it goes to the host. That is
the wrong direction for a chroot to fail in. It had been found months earlier,
written down honestly as a limit with three test cases asserting it, and left
alone, because it is the one function every path in the world passes through.

The right predicate is `lstat`, which answers what the union rule actually asks
— does the rootfs have this *name* — rather than "is there a reachable object
at the end of it". What made it safe to change was not the argument but three
measurements.

The two predicates disagree on exactly four things on this host, and all four
are the same shape: a dangling absolute link, a dangling relative one, a loop,
and a link whose target sits behind a directory the process cannot search. The
errnos are `ENOENT`, `ENOENT`, `ELOOP` and `EACCES` — so a fix keyed on "access
said the file was missing" would have covered half the class. Everywhere else
they agree, including the cases that look like they should not: a file behind
`chmod 000` fails both, because `lstat` needs search permission on the prefix
too.

Second, the change is monotone. There is no input where `access` succeeds and
`lstat` fails, so the union can only ever resolve *more* names into the jail,
never fewer. That is what bounds the blast radius, and it is a fact about the
two syscalls rather than about my code.

Third, the three suites that exercise the union rule hardest came back at
exactly the counts they had before — and the suite that owns the limit failed
precisely the two cases written to state it. That is what a correct fix to a
documented limitation looks like from the outside.

Two things I got wrong along the way, both recorded. The note I had left for
this fix said only the reading mode would change, "because the parent case is a
directory and cannot be a dangling link" — nothing stops it being one, and with
`access` a dangling parent sends a file creation to the Mac, which is the
escape direction in the mode that exists to prevent it. And my first test for
that half was vacuous: written as a create, it passed with the fix reverted,
because this machine has no `/usr/src` and the create fails whichever world it
lands in. The guard and the absence of the directory were indistinguishable —
the same trap I had walked into a week earlier with a `chmod` on a mount point.
The answer was to stop asserting the consequence and assert the resolution: ask
the function directly which path it returns.


### "Read only" was a claim about the protocol, not about the filesystem

The server had refused every write since it was built, and I had written the
reason down twice: `Twrite`, `Tcreate`, `Tremove` and `Twstat` answer an error,
and the kernel underneath them is finished and tested. What I had also written
down, in the task for the next step, was a warning that this was not the same
thing as the filesystem being read-only. Bell Labs' `readi` sets an
"accessed" flag on the inode; releasing that inode writes the flag back to
disk. So every read through the mount was dirtying a disk inode. Two accidents
hid it, the note said: the image was opened read-only, and nothing ever flushed
the buffer cache.

I re-measured before building on it, and the note was wrong in a way that
mattered. Building three copies of the server that differed only in those two
details, and instrumenting the block driver to print every write along with a
flag saying whether the bytes about to be written differed from the bytes
already there, produced this: with the image open for writing and no flush, a
small read, a 28000-byte read and twenty more reads produce **zero** writes.
The dirty buffer simply sits in the cache; the cache is never under enough
pressure to recycle it. Opening the file read-only had been doing nothing at
all. There was one accident, not two.

And there was a third that nobody had named, which is the interesting one. The
kernel's clock is set exactly once, by the mount code, from a timestamp in the
superblock — because upstream advances it from a clock interrupt, and the clock
interrupt lives in a file about a VAX interval timer that this port has never
imported. `mkfs` writes that same timestamp into every inode it creates. So
when the read path stores the access time, it stores *the value that is already
there*. The driver prints the write. `cmp` on the image prints nothing.

I confirmed it the only way that distinguishes "no write" from "a write you
cannot see": perturb one inode's access time to a distinctive value first, and
run the same read. Exactly four bytes move, back to the superblock's timestamp.

This is a shape the project has hit twice before — a line discipline writing
sixteen bytes into a six-byte structure where the extra ten round-tripped
through the caller's own memory, and a string function reading one byte past
its bound and then overwriting it. The write is real, the result is correct,
and no comparison of the artefact against itself can see it. What is new is
that this time the artefact was a file on disk rather than memory, which felt
like it ought to be easier to inspect and was not.

The frozen clock stops being invisible the moment anything writes on purpose:
every modification time a new file gets would be the moment the image was
manufactured. So the first change of the step was not a protocol message. It
was giving the kernel a clock — which is machine-dependent code, so it belongs
in the shim, and it reads the time through a raw system call rather than the C
library, because the kernel archive is asserted to import exactly three
external names and `time` is not one of them.

The second change is the one I liked best, because Bell Labs had already
written it. Making the mount read-only *properly* — so that not even an access
time reaches the disk — is not a new flag. It is `mount(2)`, whose
implementation opens the device with a read-write argument of `!ronly` and
stores `ronly & 1` in the superblock. Two lines, transcribed. With the bit set,
the inode-update routine returns before it reads anything, and the permission
check refuses writes through an arm that had been restored two steps earlier
and never yet been able to fire in anger. The three servers the test suite
already ran on one shared image now run with that flag, which turned a standing
contamination hazard into a guarantee and changed not one existing expectation:
a genuinely read-only mount refuses exactly what the protocol used to.

Then the messages, and four defects that only running it could have found.

The first was mine within minutes of writing it. Transcribing upstream's
`mkdir` — eleven lines that write `.` and `..` into a new directory, because
the syscall that allocates it does not — I copied its call to the
inode-update routine verbatim, including `&time`. In this particular file,
`time` is the C library's function, because a header deliberately hands
thirteen kernel names back to libc so the file can also use host headers. So
that line passes **the address of a function** where a pointer to a timestamp
is wanted, and the routine would have written four bytes of machine
instructions into an inode as a date. It compiles, because the function is
declared in 1985 style and takes anything. The file's own comment block records
this trap for three other names; this is the fourth, and the first where the
wrong thing is a variable rather than a call — there is no call site for a
reader to be suspicious of.

The second was a comment I wrote confidently and then disproved by reading. I
implemented open-with-truncate as "free the blocks, then zero the size and mark
the inode dirty", on the reasoning that the truncation routine only handles
blocks. It does not. It writes a zeroed copy of the inode *synchronously*
first — so that a crash part-way through freeing leaves harmless orphaned
blocks rather than blocks claimed twice — and then clears the dirty flags on
purpose. My two extra lines would have undone exactly that reasoning. Upstream's
own caller is one line: truncate, and nothing else.

The third and fourth were found by running the thing rather than reading it,
which is the honest summary. 9P's remove message carries a file handle and
nothing else. V7's `unlink` names a *directory* and an *entry* — and there is
no parent pointer in an inode, because `..` is a directory entry, so it exists
for directories and not for plain files. My first version used the filesystem
root as the parent for a plain file, which is correct only for names directly
under the mount point. `rm /mnt/newdir/f` asked the server to unlink `f` from
the root, found nothing, and reported a failure that `rm` swallowed. The walk
and the remove are in different functions and each reads correctly on its own.
And once that worked, the file's blocks leaked on every delete, because the
handle still held the inode: upstream's unlink code takes its own reference and
it is *that* release, at a count of one, which notices the link count has
reached zero and frees the blocks. Neither would have been visible from inside
the probe. The first showed up as a shell command that quietly did nothing; the
second would have shown up only in `icheck`.

There is a fifth thing, and it is a small argument I enjoyed. 9P has no
`access` message, for exactly the reason it has no seek message: Plan 9's
kernel decides permission when it opens a file, and has no `access` system call
to ask in advance. V7 does, and `test -r` is an ordinary program in this world.
The client had already been wrong about this twice — first computing the answer
from the image's permission bits against the *host's* user id, which was wrong
on every file of every image; then reporting what would actually happen, which
was right only while every write was refused. Both of those explanations were
still sitting above the function, one contradicting the other, because the
rewrite had added its reasoning without removing what it replaced. The third
answer is to ask, over a new message numbered beside the seek one, and let Bell
Labs' own permission routine answer with the server's identity on both sides.

It immediately told me something I had got wrong. `test -x` on a file with mode
0644 returns **yes**, because upstream's permission check grants root
everything with no special case for the execute bits — that refinement is
BSD's, not V7's. I had written a test case expecting "no" and the code was
right.

The eight mutations all fire, but one of them needed a new test case first, and
its reason is worth recording. Deleting the line that advances the server-side
file offset after a write left the entire section green — and for none of the
three reasons this project has previously catalogued. The guard was not
vacuous and the code was not dead. Every write in the suite was a *single*
write through one open file, because `echo` writes once, so an offset that
never advances is never asked to. Copying a 28000-byte file within the mount is
seven writes through one descriptor, and the second one lands on top of the
first.


### Three syscalls that are one message

`chmod`, `chown` and `utime` were the last of the fourteen path-taking calls
with a 9P answer, and they turned out to be a single message. 9P's `Twstat`
carries an entire `stat` structure, and the rule is that any field the client
does not mean to change is sent as all ones — so a chmod is a wstat that sets
the mode, a chown one that sets the owner, and a utime one that sets the times.
Three syscalls, one wire format, three different fields filled in. What is left
refusing a mounted path is `link` and `symlink`, and neither is deferred work:
9P2000 has no message for either, and a V7 image holds no symbolic link to read
back.

The server had already had a `Twstat` handler for a step. It had never been
called. And reading Bell Labs' own `chmod`, `chown` and `utime` before writing
the client half found that three of its arms disagreed with them — in each case
with a comment directly above claiming the rule the code did not implement.
`chmod` and `utime` gate on `owner()`, which is *ownership or superuser*, and
the code tested superuser alone while the comment said "ownership for
everything else". It cited the wrong file, too. None of it was observable,
because the server runs as uid 0 and both rules therefore always permit — which
is exactly why the sentence and the line could disagree for a whole step
without anything going red.

The missing arm is the more interesting one, because it had been declined on
purpose and the reason given was true. The server honoured the modification
time and not the access time, and the note said: *nothing in this world sets
atime alone, and an unexercised arm is a claim nothing can check.* Still true.
It simply never covered the case that turned up. `mv` sets **both** — line 129
of `mv.c` is `utime(target, &s1.st_atime)`, which takes the address of one
field of a `struct stat` and relies on the next field being adjacent to pass a
two-element array — and on a mount that line is not an unusual path but the
only path, because `link` is refused, so `mv` always falls through to forking
`/bin/cp` and then stamping the copy. Declining an arm because nothing sets a
field *alone* is a different claim from nothing setting it at all, and only the
second one would have been a reason. Before the arm existed, moving a file
within a mount copied it correctly, removed the original correctly, and lost
both timestamps without a word.

Testing that needed a moment's thought about the instrument. `ls -l` prints
minutes for a recent file, and the test writes and reads within the same
second, so a comparison of "before" and "after" could not distinguish a working
`utime` from a broken one. `ls -l` prints a *year* for an old file. Giving the
source file a 1991 date makes the difference one the instrument can actually
show — the same lesson, in a different key, as the earlier discovery that a
minute-granular listing could not see the clock start moving.

### The two ends of one wire, an hour apart

The auditor ran on this the hour it was written, for the fifth consecutive
unit, and its best finding was in the code I had just made reachable.

9P specifies a file's owner as a *name*. This server sends a number, and says
why: turning an inode's numeric owner into a login name means reading a
password file, and there are two of them here with no principled way to choose.
So `statof` writes `"0"`, and the handler read it back with `atoi`.

`atoi` has no error return. `atoi("nobody")` is 0, and so is `atoi("0")`. The
auditor drove a real server with a raw client and measured it: an owner of
`"nobody"` set the file's uid to 0 and the server replied with a success. So
did `"--"`. `"12x"` set it to 12. And 0 is root — the one identity the kernel's
`access()` bypasses entirely.

What makes it worth writing down is not the bug but where the guard already
was. The *client* end of this same field had been given exactly this check by
an earlier audit, with the contract spelled out beside it: root maps to root,
and non-root never maps to root. The reading end had it. The writing end,
added an hour before, did not. This project's most repeated shape is "the fix
landed on one line and the line beside it kept the assumption"; this is that
shape with a process boundary and a protocol in between.

The fix has to separate two properties that look like one. `"65536"` is
accepted, and truncates to 0 — because that is V7's own answer, `ip->i_uid =
uap->uid`, an `int` assigned into a `short` with no check. A string V7 could
have produced keeps V7's behaviour. A string it could not produce is refused.
Range is not parseability.

And nothing could have caught it, which is the reason it existed: the handler
had no caller until the step that added one. The test for it is a new mode in
the wire-level probe, because no V8 program can reach it — the client formats
an integer and is structurally incapable of sending a name. Only a foreign
client, or a probe, can ask the question.

### A case that was vacuous twice over

Eight mutations, eight fire — but only after mutation testing revealed that one
of the new cases asserted nothing at all.

The case was "chmod cannot change a file's type", and it survived every attempt
to break the code it was supposed to guard. The first reason is that `ls`
chooses its type character with a `switch` whose default is `-`, so a mode word
that had lost its type bits *entirely* still prints as an ordinary file. The
second, discovered while fixing the first, is that the client reconstructs the
type from a single protocol flag rather than passing the server's mode word
through — so `ls` on a mount cannot see that field even in principle. Either
reason alone would have kept the case green after the other was fixed.

The type is observable on a **directory**, because the flag the client trusts
is itself derived from the server's type bits. Re-aimed at a directory, the
case fires — along with all five independent checkers, which is the other
lesson repeating: `icheck`, `dcheck` and `fsck` know nothing about 9P, and they
caught this before the re-aimed case did.

The mutation harness contributed a finding of its own, in the same family as
several before it. This project has a documented rule that a mutation run must
`touch` the source after restoring it, because the whole cycle can finish
inside one second and `make` compares timestamps at whole-second granularity.
That rule covers the *restore*. It does not cover the *apply*: the previous
mutation's restore-rebuild finished in the same second the next mutation was
written, `make` declared the binary current, and two runs measured a program
that had never been changed. They were caught only because the harness hashes
the artefact and checks it moved — which is the difference between "the
mutation did not compile" and a comfortable, false "the guard did not fire".

### And then the disk filled up

The auditor's other finding was not a wrong answer but a dead server. Writing
until a mounted image runs out of space printed `file system full` and then
`panic: tsleep: no device below, and no timeout`, and the process exited —
dropping *every* client's connection, not just the writer's. An ordinary
program with no privileges could do it.

Every link in that chain was correct for the caller it was written for, which
is what makes it worth describing. Bell Labs' out-of-space path is a kludge and
they say so in capitals: print a message, sleep five clock ticks in the hope
that some other process frees a block, then fail with ENOSPC. This port maps
`sleep` onto its own `tsleep`, which panics when there is no device underneath
and no timeout — and the comment above that panic reasons, carefully and at
length, entirely about *streams*. It was a complete account of every caller
that existed when it was written. Then a filesystem arrived.

The fix turned out to be provable rather than a judgement call, and it took two
greps. The channel being slept on is `lbolt`, the clock tick. Exactly one line
in the imported tree sleeps on it — the out-of-space kludge — and exactly one
line in the entire eighteen-thousand-line kernel wakes it: the clock interrupt
handler, in a file this port does not have and does not import. So a sleep on
that channel can never wake. That is the same shape of argument the panic
itself makes about a stream with nothing below it, reaching the opposite
verdict because the caller is different. And returning immediately changes no
observable, because the wait is futile by construction: upstream is waiting for
*another process*, and in a single-threaded file server the caller is the only
thing running, so the loop was always going to fall through to ENOSPC.

Behind it was a second defect that the first one had been hiding, which the
audit predicted in so many words. The server's directory-creation wrapper calls
`writei` to lay down `.` and `..` and never looked at the error. Upstream's
`mkdir` ignores the same return value and can afford to, because upstream's
*is* the system call — the error lands in `u_error`, which is where the user's
`mkdir` reads it from. Here it died in the wrapper, and the server replied
success. With the panic gone, this became reachable for the first time: `mkdir`
on a full image exited 0, and `fsck` found a parent whose link count had been
incremented for a `..` that was never written, and a directory of size zero.

The damaged directory is not the bug. It is precisely what a V7 kernel leaves
behind when that write fails, and repairing it is what `fsck` is for. The
success reply was the bug.

One more thing about the test for it, because the obvious case is worthless.
"Writing until full fails" passes whether or not the server survives — a
process that dies mid-write looks exactly like a failed write from the other
end of the socket. What discriminates is asking whether the server is *still
there*, and then whether it can still answer a read; mutation confirms that
reverting the fix turns those two red and leaves the write case green. And the
image is not asserted clean afterwards, only still readable, because a clean
image is not what a mid-write death would have taken away.

### Two smaller ones, and an honest nothing

The audit's remaining two are worth a paragraph each, for opposite reasons.

V7 re-checks what a file was opened for on *every* transfer, in a single line
of `rdwr()`, and the server had a third of it: the read path checked only that
the file was open at all, and the write path refused a read-only handle but not
an execute-only one. So a handle that had proved nothing but execute permission
could write to the file, and a program that opened a file write-only could read
it. The interesting part is the shape of the fix. 9P defines its execute mode
as "read, but check execute permission" — Plan 9's kernel has to read a binary
in order to run it — so the two gates are *not* complements, and each direction
needs a refusal case and a success case. A server that simply refused the
execute mode outright would sail through a suite made only of refusals.

It also needed a note about what it does *not* fix. The server runs as uid 0,
so the kernel's own `access()` takes its root bypass and grants write
permission on everything; the truncation the auditor measured happens before
the change and after it. Only the gate is live, because a gate is a property of
the handle rather than of the identity. Writing that down was more work than
the fix, and it is the part that will still be true in a year.

The last finding had no observable at all: one path-taking call still
special-cased mounted paths inline instead of going through the switch, which
left part of the switch with no caller and sent one operation to the host where
its sibling went to the filesystem type. Nothing misbehaved, because the two
roads happened to meet — which is exactly why an auditor found it and no test
did. It gets no new test case, because a case for a change that alters no
behaviour cannot fail, and this project's own rule is that a mutation which
does not fire means the case is vacuous. What *does* have coverage is the one
arm the move brought to life, and mutation says so: delete it, and a case about
`rmdir` written years ago goes red, because V7's `rmdir` ends by unlinking a
directory and macOS will not do that.


### The cast that made a stranger root

The last thing this session did came out of a sweep rather than a bug report,
and it is the clearest example I have of why a repeated bug class deserves a
sweep instead of a fix.

Some months ago an auditor found the shim folding a host process id into V8's
sixteen-bit field correctly, and then narrowing a *user* id with a plain cast on
the next two lines — directly beneath the file's own paragraph explaining why a
truncation there is dangerous. That got fixed. What nobody did was ask where
else the same cast lived.

It lived in two more places, and a third turned up while I was fixing those. The
worst is the one in `stat` — the path every `ls -l` goes through. A user id on
this Mac is thirty-two bits; V8's field is sixteen; and a plain cast maps every
multiple of 65536 onto **zero**. Zero is root. So on a machine with
directory-issued accounts, a file belonging to an ordinary user reads as
belonging to root, and the kernel's permission check has a bypass for exactly
that identity. The cast does not produce a wrong number. It produces a
privilege.

The rule that replaces it is two sentences — root maps to root, non-root never
maps to root, everything that fits stays exact — and it lives in a header rather
than a library, because no two of the four components that need it are allowed
to share an archive. A pure piece of arithmetic needs no link edge.

What it deliberately does *not* touch is `getuid` itself, and the tree settles
that in a single line: `mv` contains `setuid(getuid())`, and that value goes
straight back out to the real kernel. A folded id there would try to become a
user who does not exist. The honest consequence, which belongs in the record
rather than in a footnote, is that comparing a file's owner against your own id
still disagrees on such a host — but it disagreed before too, for exactly the
same values. The fix is to the contract, not to the identity map. A colliding
user id is a wrong name; a promotion to root is something else.

And there is no test on this machine that can reach any of it, because the uid
here is 501 and a CI runner's is lower still. So the guards are two, for two
different relations: the arithmetic, checked against a table that includes the
values which broke it, and the call sites, checked by a sweep of the source
asserting that nothing narrows an id with a cast again. Neither can see what the
other sees. The sweep also matches its own explanation — four files now discuss
the bad cast in prose — so it excludes comments and *prints how many it
excluded*, because a filter that silently removes things is the next bug.

### A reason that had already been overruled twice

Two operations were still refused on a mount, and one sentence explained both:
*"Neither is deferred work: 9P2000 has no message for either."*

That sentence is in two files, and it is wrong in a way worth dwelling on,
because nothing about it looks wrong. It is accurate — 9P2000 really has no
message for `link` — and it is cited, and it reaches a confident conclusion.
What it does not notice is that the very same fact had already been ruled the
other way, twice, in the same file. 9P has no seek, because Plan 9's kernel
held the offset; this port added `Tseek`. 9P has no `access`, because Plan 9
has no `access(2)`; this port added `Taccess`. Both times the reasoning was
written out at length: V7 has the concept, the protocol does not, so number a
message outside the range a conforming client can use and say why.

The third time, the identical observation became grounds for declining. A
reason that has already been overruled twice by the file stating it is not a
reason; it is a habit.

And the two operations were not alike. A V7 filesystem cannot *represent* a
symbolic link — there is no mode bit for one, which is why `readlink` on a
mount answers `EINVAL` — so that refusal is permanent and no protocol could
change it. A V7 filesystem is *built on* hard links: `i_nlink` is a field in
the inode, `link()` in `sys2.c` is one increment and one `namei`, and the
kernel arm that does the work had been sitting in the imported tree since the
import, unreachable only because nothing had ever sent it a request. One of the
two was a permanent fact about a filesystem format. The other was a gap. The
sentence had flattened them into a pair.

What kept it flattened is that the loudest consumer fails quietly. `mv` of a
file, when `link` is refused, forks `/bin/cp` and exits 0 — so the gap reads as
a slow path rather than a missing feature. `mv` of a *directory* has no such
fallback: it must relink, and it printed "cannot link" and left the directory
where it was. The measurement that settled it took ten seconds: create a file
on the mount, create a directory on the mount, then ask for a link — and get
back **"Read-only file system"** from a filesystem that had just accepted two
writes.

### And then link exposed something older

Adding it broke `rmdir`, which is the interesting part.

9P has one `remove` and V7 has two calls. Plan 9 has no `rmdir(2)` at all — its
`remove` takes anything — so the message carries no flag and the server had
been deciding from the inode: if it is a directory, do the directory thing.
That is the right reading of `Tremove` for a foreign client, and the wrong
answer for V7's `unlink(2)`, which removes a *name* whatever the name points
at.

It had been wrong from the day the server was written and nothing could see it.
With no way to give a directory a second name, every directory a client could
remove had exactly two links, and the two kernel operations differ only above
two. Adding `link` made the difference reachable, and three separate
disagreements arrived in the same afternoon: the wrong errno, a directory
destroyed where V7 would have decremented, and — the one that stings — a
*failed* removal that decrements the parent's link count on its way to the
error and never puts it back. That last is Bell Labs' own bug, sitting in
imported source, and it stays: the rule is that changes to `src/` must be
forced by the target, and a 1985 bug reproduced faithfully is the point of the
exercise.

The fix needed a fourth message, and it could not be a plain "remove this
name", because of a fiction the port had already told. `rmdir(1)` in V7 takes a
directory apart by hand — unlink `..`, unlink `.`, unlink the directory — and
this port swallows the first two, because neither macOS nor this server will
perform them. So by the time the third arrives, two decrements that should have
happened have not, and a faithful unlink leaves a directory attached to
nothing. The server therefore asks the only question the two callers actually
differ on: *does another name reach this directory?* If not, this was its last
name and removing it destroys it. If so, removing one name is all that was
asked.

The regression that forced all of this was visible to exactly one instrument.
The directory listing was right. `icheck` was silent. `dcheck` was silent. Only
`fsck` said `***** FILE SYSTEM WAS MODIFIED *****`. Three independent readers
of the same image, and they do not agree about what they can see.

### Two mutations that would not fire, and both were right not to

Nine mutations, seven red. The two that stayed green were the useful ones.

Both had deleted a guard I had written in the new code, and in both cases
nothing changed because Bell Labs already refuse the same thing one layer down
— a duplicated `ENOTDIR`, and a rejection of `.` and `..` as a new name. The
second is the better story, because the comment justifying it was not merely
redundant but *false*: it warned that without the guard the kernel would
happily overwrite a live directory's parent pointer. It would not. Upstream
returns `EEXIST` the moment the name is found, and `..` is always found. The
guard I had written was less faithful than no guard, and the case I had written
to cover it was really testing something else entirely.

That is the same shape as the sentence this section opened with — a confident,
cited claim that nobody had run — arriving three hundred lines and one hour
later, in code written by someone who had just finished writing about it.

### Two copies of a wrong list agree

One more, because the guard that missed it is a good one. The server turns an
errno into a name for the wire and the client turns it back, and the suite
compares the two tables in both directions and requires them to match. They
matched perfectly. They were both missing the same seven names, so every one of
those errnos reached the client as a generic I/O error — including the one that
made a failed unlink report the wrong reason.

Two hand-written copies of one list will agree with each other forever. What
they cannot do is notice that the list is short. The check is now against a
third thing that is neither table: a sweep of every errno the imported kernel
can actually assign. Twenty-two names; the tables had fifteen. And the
near-miss is instructive on its own — an earlier fix to this exact code had
found one missing name, added it, and moved on. Auditing the set instead would
have found the other seven a year ago.

### A compensating error reads as a design

The step after that one made `mv` of a directory work across directories, and
it did so by retracting the previous step's fix.

That fix had a heuristic: when the server was asked to remove a name, it
decided whether the name was a directory by looking at its link count. The
comment explaining it was careful and correct about the mechanism — it
described a fiction the client was telling somewhere else, where `rmdir(1)`'s
three unlinks were being absorbed down to one, so the third had to do all three
jobs. Every clause of that was true. The conclusion was wrong, because what it
described was **one workaround split across two files**, and the comment read
it as a design.

The real defect was in the shape of the protocol message. The port had added a
`Tunlink` and given it a file handle, copied from the existing `Tremove`. But
those name different things: Plan 9's `remove` names a *file*, and V7's
`unlink` names a *directory and an entry*. `..` is where the two visibly come
apart — it is an entry that exists only from the directory's side, so no handle
can name it, which is precisely why the server already zeroed a field when a
handle was walked to `sub/..`.

The diagnosis was sitting in the codebase, in the header comment of the
function it indicted, phrased as an explanation rather than a complaint. It had
been read past for a whole step.

Reshaping the message to carry a directory and a name let both halves go, and
they had to go together: keeping either alone decrements the parent's link
count twice. And the mutation testing said something sharp about the guards.
Restoring the client's absorption alone turns **`fsck` red while `icheck`,
`dcheck` and the block-count identity all stay silent** — the third time in
this project that an independent reader caught something the probe's own
writer and reader, sharing one program's beliefs, could not.

### A mount becomes somewhere you can stand

Until this point a mount was a place you could *name*. `cat /mnt/sub/deep`
worked; `cd /mnt` was refused, and the refusal was honest — nothing in the shim
tracked a working directory, so a program that got inside would have found
every relative name resolving against the Mac.

Closing it is one function, hooked in at three places, and it is the identity
whenever no mount is configured — which is why sixteen of the seventeen test
suites did not move by a single case.

The design problem is the one that made the first attempt get thrown away. The
fold has to resolve `..` **lexically**, because at a mount root the server
cannot help: a V7 filesystem's root points at itself, and on a real Unix it is
the kernel's mount table that fixes up a walk crossing a mount upward. There
is no kernel here. But folding the *last* component destroys the one thing the
previous step's `Tunlink` exists to name — `..` as an entry — so
`unlink("/mnt/d/..")` folded whole becomes `unlink("/mnt")`. Thirteen failures,
and `fsck` reporting that it had modified the filesystem.

The obvious compromise is to fold all but the last component everywhere, and it
is also wrong. `getwd(3)` opens `..` and then chdirs to `..` **in the same
loop** and requires the two to agree; fold all but the last and one reaches the
image root while the other reaches the jail root, and the walk covers two
different trees.

So the mode is threaded rather than inferred, and the split turns out to be one
the code already drew elsewhere: *reach an object* folds the path whole, *name
an entry* folds all but the last. The dispatch needs the mode as much as the
resolution does — a fully folded `/mnt/..` is the jail's own root and belongs
to the host filesystem, so an unlink meaning "remove the `..` entry of a
mounted image" would otherwise ask the Mac to remove the jail.

`pwd` works inside a mount with `getwd.c` **unmodified**. It takes that
function's own different-device fallback, because a mounted file reports device
zero while the jail root reports the host's — so the inode comparison the
same-device arm would use is never reached.

Three things came out of it that were not about the feature.

A fold introduces a **normal form**, and everything compared against a folded
path has to be in it. The static mount table is normalised because it was
written out by hand. The one that comes from an environment variable was not —
and `$TMPDIR` ends in a slash on a Mac, so the test suite's own mount carried a
double slash and stopped claiming its own files. `cat` read the host directory
the mount was covering, in the case written to prove that it could not.

**Pointer identity stopped meaning "resolved", and that one was an escape.**
Three callers asked "did the jail claim this name?" by comparing the answer
against the argument, which worked because a path resolver that did not
redirect handed back the very pointer it was given. With a fold running first
it hands back a pointer into the fold's buffer, so the test is true for every
path — and one of those callers would then never reach the create-a-new-name
rule, so `creat("/etc/./x")` inside the jail would write to the Mac.

And moving a buffer from function scope to file scope silently changed
`sizeof` from 1024 to 8. The jail's root path was truncated to seven characters
and every binary lost its jail. It failed loudly, which is luck rather than
design.

## Part 11: A world you can live in

Everything to this point is a **depth** claim: an authentic compiler, a
pseudo-kernel, a real filesystem. Then the goal changed to *"install a usable
V8 world"*, which is a **breadth** requirement, and a measurement that had
never been taken became the important one.

V8 shipped **286** executable commands across `/bin`, `/usr/bin` and `/etc`.
This port installed **91**.

Of the 195 missing, 43 have no source at all — VAX firmware, data files, and
the handful that shipped as binaries only. About another 96 are out of scope
for reasons already recorded: PDP-11 cross-tools, the deliberate host-toolchain
exceptions, the `uucp` suite with no network under it, the Blit graphics
programs, the kernel grovelers. So the honest denominator is around **210**,
and the port was at less than half of it.

**The gap was entirely at import, and nothing was stuck.** Ninety-one programs
had been imported and ninety-eight installed; the only ones imported and not
installed were toolchain. There was no queue of programs that had failed to
build, or built and failed to install. The pipeline was empty because nothing
had been put into it. Two existing measurements say it was never a difficulty
problem: a survey had already found that **156 of 163** single-file commands
compile under the ported compiler, and Bell Labs' own build script had already
built, stripped and installed **fifty** of them in one invocation.

So: import in bulk, and let the test suites do the triage.

Thirty-seven single-file commands went in at once and **all thirty-seven
compiled and linked with no failures at all**. Then the suites found three real
bugs the compiler had not.

The rootfs-wide sweep for truncated pointer returns caught `last` calling two
undeclared time functions, one nested inside the other — two truncations in a
single expression. The sweep that asserts no binary takes anything from the
host's C library caught **five** programs quietly resolving three functions
from it, which meant `id` was reading the **Mac's** group database from inside
the jail. All three functions existed upstream and had simply never been
imported. And the linker caught `cb`, whose first lines include a `.c` file as
if it were a header — the fourteenth member of a list this project has been
keeping, and upstream's own makefile says so outright.

### One program had eleven of the same bug

`find` builds an expression tree, and each predicate reads its own operands by
casting the tree node to a small local struct of integers. On the VAX every
member was four bytes and the cast was exact. Here the node's members are
**pointer-sized**, so the second field lands at offset four — the upper half of
a function pointer.

Every predicate that reads its second field was **silently false**: file type,
permissions, link count, size, owner, group, all three time tests, and both
forms of running a command. Eleven of them.

`-name` works. Its cast happens to be an integer followed by a *pointer*, and
the pointer's own alignment pushes it to offset eight, where the real field is.
So the one predicate anybody tries first is the one that works by accident,
which is why this survived import, compilation, and a smoke test that printed
file names.

It had three more problems, and one is a good illustration of a rule this
project keeps rediscovering. Raising a path buffer to hold a modern filename
broke a *relationship*: the archive-writing code copied that buffer into a
fixed 256-byte header field, safe by construction while the buffer was 200
bytes and an overflow the moment it was not. The field is an archive format and
cannot grow, so a name that will not fit is now reported and skipped rather
than truncated — a short name in an archive names a different file.

`find /usr/lib -type f` now returns 74, which is what the Mac's own `find`
returns for the same tree.

### The world is a working copy of a golden image

A world you cannot write is a demo. The install now writes a **pristine** tree
and never touches it again; the first launch copies it to the user's own home,
synthesizes a password file for whoever is running, creates their home
directory and logs them in there. Everything after that persists — a file, a
program they built and installed into `/bin`, an edited `/etc` — across
launches, across the next install, and across a `make clean` in the build tree.
A reset exists and destroys the working copy, and it is the only thing that
does; it requires typing the word.

It cannot be the installed tree itself, and the first reason is fatal on its
own: the default prefix is root-owned on macOS, so the world would be read-only
to the person using it — and this project's central claim is that V8 rebuilds
V8, which means Bell Labs' build script has to be able to copy a program into
`/bin`.

Giving the user a home directory needed one more row in the mount table, for
`/usr` itself. That is normally the cheapest way to break everything, and it is
safe here only because the rule is a **union rather than a capture**: a path is
redirected into the world only if the world actually has that name, so
`/usr/include` and `/usr/local` still fall through to the Mac.

And the login needed **no change to V8's source at all**. A first draft taught
the launcher program to read `$HOME` itself; the jail test suite caught it in
the same run, because a bare invocation inherits the *Mac's* `HOME` and
chdirs straight out of the world. V8's own program already takes the directory
as its argument. The environment is the launcher's to set and the argument is
the program's to take; crossing them turned a host variable into a jail escape.

### Sixty-two names, and what that means

The world provides 81 commands, and **62 of them share a name with one the Mac
also has** — `make`, `cc`, `sed`, `sh`, `grep`, `sort`, `ls`, `cp`, `test`. The
launcher puts V8 first, which is the entire point, so a native build started
from inside finds a 1985 `make` and a 1985 compiler.

That is not a defect to fix. It is the price of the world looking like V8, and
lowering it means giving up the premise. What was missing was a way to *say*
you meant the other one, so there is now a one-word prefix that switches worlds
— and it restores the host's search path rather than merely locating a binary,
because a wrapper that only found the Mac's `make` would still hand it a path
whose first entries are 1985's, and `make`'s own children would be wrong again.

The useful formulation is that **native apps work and native builds do not**.
Running an app is one exec, and the tools people actually reach for — `python3`,
`git`, `node` — collide with nothing. Building software is hundreds of path
lookups against exactly the 62 names V8 owns. So you build on the Mac and work
in V8.

### And `su` answers a question that had two bad answers

Whether privilege inside this world should be *real* root or a pretend root
over the jail is a question with two unattractive answers. Real root is
catastrophic: the jail is a string operation in process memory, so every
path-resolution bug becomes an exploit, and this very session produced two such
bugs. A faked root is a costume that the host does not honour, so it would
split into two regimes behind one call.

V8's own `su` resolves it without choosing either. It is authentic source, it
installs where Bell Labs' tables say, and it needed nothing new — the three
library functions it wanted arrived with the `id` fix. Ask it for root and it
says `Password:` and then `Sorry`, because the synthesized password file
carries `*`, which no encryption of any password can equal. That is 1985's own
convention for an account that cannot be logged into, not a refusal this port
invented.

There are three different uid-zeros in this system and none of them is a login:
the host's, which is real and refuses; the one folded from the host identity in
the process-table half, which maps root to root and **never** maps a non-root
user to root; and the filesystem server's, which is deliberately zero and
argued for in the file that sets it.

### A terminal library, and two copies of one file

The world could describe a filesystem in detail and could not describe a
terminal at all. V8's `libtermlib` was sitting unimported, there was no
`/etc/termcap`, and every screen-aware program in the tree wants both. It is
three files and a 44-kilobyte database, and importing it turned mostly on
questions about *which* three files.

There are **two** copies of `termcap.c` upstream, differing by eleven lines,
and this repository's rule for that is to find out which one the build reads
rather than which one looks tidier. Two measurements settle it and they agree.
The eleven lines are a block that asks a Blit terminal its window size, and
`ioctl` appears nowhere else in the library — so an undefined `_ioctl` symbol is
a *fingerprint* of that source, and the archive V8 shipped has one. The other
copy installs to `/lusr/ucb/lib`, a directory that does not exist anywhere in
the distribution. So the shipped library is the one with the Blit block, and the
`ex` copy is the Berkeley staging area.

That block opens with an include this port has met before:

```c
	if (strcmp(id, "co")==0 || strcmp(id, "li")==0) {
#include "/usr/jerq/include/jioctl.h"
		struct winsize jwin;
```

An absolute path is the one form of include no `-I` can redirect, and it is
*inside a function body*, which is legal and startling. My first reading was
that the header simply was not in the distribution. That was wrong in the way
this project keeps being wrong: `jioctl.h` is in the same archive under
`jerq/`, the import tool already had a case for it, the Makefile already copied
it into the rootfs, and `ls.c` already carried the identical one-line change
for the identical reason. The precedent had been in the tree for months. The
correction cost nothing here and it is the same shape as four of the seven
"blockers" that turned out to be false — something described as missing that
was sitting in the tree, unread.

The library's four `-D` flags looked like decoration and are not. Each guards
one case in `tgoto`'s cursor-addressing interpreter, and a missing one is not a
compile error: the case falls through to a default that returns the string
`"OOPS"`, so a terminal whose cursor sequence uses that escape simply never
moves its cursor. Dropping all four takes the object from 2376 bytes to 2096.

Which is how the interesting failure arrived. Deleting those flags and running
the suite produced **no failures at all**. The mutation was strong and the code
was live — neither of the two reasons this project had recorded for a mutation
not firing. The third reason turned out to be simpler than both: `ul`, the only
program that links the library, never does cursor addressing, so the largest
member of the library had nothing looking at it. A probe now calls `tgoto`
directly, and with the flags removed it names each missing one — four lines
each reading `want [37] got [OOPS]` — while the unguarded case keeps passing,
which is what says the probe discriminates instead of merely failing.

`ul` itself contributed the tenth member of the address-0 class, and the
trigger was the same as the other nine: the option is the last thing on the
command line. `ul -t` reads the null after the last argument and the library
dereferences it. The fix is `""`, and `""` is not a guard but the VAX's own
answer — measured, because this project recorded that byte wrongly for months.
V8's binaries are ZMAGIC, so text begins 1024 bytes into the file and virtual
address zero is the first byte of `crt0`, which is `0x00`, identical in every
shipped binary. So a VAX read the empty string there. What the empty string
*does* is the part worth keeping: `/etc/termcap` has exactly one blank line,
`tgetent` reads it as an entry whose first character is a NUL, `""` matches it,
and the library prints `Bad termcap entry` and falls back to a dumb terminal.
All of that happened in 1985 too. The port reproduces it rather than
short-circuiting it with a null check that would have returned earlier and
differently.

Two upstream bugs are deliberately left in place, because a VAX had them
identically and the contract says record rather than patch. `ul -t vt100`
leaves `vt100` as a filename, because the option loop consumes the value and
not the option. And `ul -tvt100` — the spelling `ul`'s own usage message
documents — **hangs**, because the loop never advances at all. Measuring that
one needed care: a deadline wrapper written minutes earlier reported the hanging
case as a clean exit, which is this project's own rule about instruments
arriving on schedule.

The last thing was a test that could not be written. `libtermlib.a` is a hard
link to `libtermcap.a`, because that is what upstream's install does. The
obvious dependency case — touch the prerequisite, require the target to go
stale — is not difficult but *impossible*: they are one inode, so touching
either moves both timestamps and the prerequisite can never be newer than
itself. The case was deleted and the reasoning written where it stood. What
guards the link instead is an inode comparison, which is also the only thing
that would notice the failure that matters, since two identical copies would
pass a byte comparison and would not be what V8 shipped.

### The editor that was filed under the wrong name

`vi` was recorded in this project's own notes as having no source at all, next
to `more` and `pg`. `more` and `pg` really did ship as binaries with nothing
behind them. `vi` did not: **`vi` is `ex`**, the two shipped binaries are
byte-identical, upstream's makefile hard-links `vi`, `view`, `edit` and `e` to
it, and there are sixty-one files of source sitting in the tree. It was filed
with the sourceless because `usr/src/cmd` has no directory called `vi` — the
sweep looked for the program's name, and the source is under the name of the
program it is a link to. The most valuable interactive program in the system
was written off by a listing that could not have found it.

Importing it went better than anyone had a right to expect. **All twenty-eight
objects compile under the 1985 compiler with no source change at all**, using
upstream's own flags; the program links with nothing imported from the host C
library; and `ex`, `vi`, `view` and `edit` are installed as one inode, the way
V8 shipped them. It edits: print, substitute, append, delete, write, with the
file coming out exactly right each time. A nineteen-thousand-line BSD editor
from 1981 needed nothing.

The port needed three things, and that asymmetry is the whole story of this
section.

Getting that far turned up three bugs, and none of them is in `ex`. All three
had been sitting in the port waiting for a consumer.

The first is the one worth the space. `ex` ships its own `printf`, written
against `<varargs.h>`, and that header was **still 1985's** — never imported,
never patched, byte-identical to the vendor copy. Its `va_arg` strides
`sizeof(mode)`, four bytes for an `int`, while this port's compiler spills
arguments into eight-byte slots. So every argument after the first is a splice
of two halves, and the first is always right — which is exactly what the
failure looked like: the file name printed and the line count segfaulted.

What makes it a good example is that the *class* was never unknown. It is this
port's dominant bug, and the forward spelling of the very same idiom — V8's
`printf(fmt, args)` walking `&args` — had been fixed years earlier in seven
separate files, every one of them taught to stride eight bytes. What nobody
noticed is that `<varargs.h>` says the same thing a third way. Measured:
`ex/printf.c` is the only file in the entire tree that uses `va_alist`, so the
header had no consumer, and a header with no consumer cannot be seen to be
wrong. The same shape as the on-disk `fblk.h` that had never been imported and
was right only by coincidence.

The second is smaller and the same shape from the other side. `ex` defines its
own `exit()` — the ordinary 1980s way to clean up before leaving — and finishes
it with `_exit()`. Both wrappers lived in one member of the stub archive, so
the linker pulled that member to satisfy `_exit` and its `exit` collided with
the program's. V8 keeps `sys/exit.s` and `sys/_exit.s` as separate files for
precisely this reason, and the stub file's own comment said so, three lines
above code that did the opposite. The archive's granularity rule had been
stated, understood, applied to `signal`, and not applied here.

The third was a recorded blocker that dissolved on reading. `ex` wants
`ntty_ld`, and the notes said `tty_ld`/`ntty_ld` were "genuinely kernel state
rather than something to emulate". They are twenty-four initialised integers in
`libc/gen/linedis.c`, a file that had simply never been imported. Cross-checked
against `conf/devices` — the file the kernel build actually reads — which
agrees row for row, discipline 6 being `ntty` in both.

### The bug that was in the ruler

There is a fourth thing to report, and it is the most useful one, because for
several hours this section said something else. It said `ex` was imported and
deliberately **not installed**, because it opened a file, reported it
truthfully, and then executed no command and exited zero. It offered a clue
that felt like a real finding: an instrumented build — the same sources plus
three `write` calls — *did* execute commands, and behaviour that changes when
you add debug output is the signature of memory corruption moving under a
changed layout. That went into four documents, a test exclusion, and a commit
message.

None of it was true. `ex` had worked the entire time.

The deadline wrapper I was running it under executed its argument as `"$@" &`,
and **a backgrounded job in a non-interactive shell gets its standard input
from `/dev/null`**. So `ex` read end-of-file immediately, did nothing, and
exited zero — which is indistinguishable from a broken editor. The control is
three lines and takes a second: `printf 'X\n' | sh -c 'cat & wait'` prints
nothing at all.

The clue was worse than useless, because it was a comparison across two
variables rather than one: the instrumented build had been run *directly* and
the clean build *through the wrapper*. The instrumentation was never the
difference. And the cheap check that would have killed the theory outright went
unrun — ten invocations of the same binary produce byte-identical output, and
memory corruption is rarely that well-behaved.

This project has a rule that an instrument you wrote is a suspect, and it earned
it three separate times in one sitting: the same wrapper had earlier reported a
genuine infinite loop as a clean exit, and a shell variable holding seven
compiler flags had gone through unquoted — `zsh` does not word-split — making
"twenty-seven of twenty-eight objects compile" a measurement of compiling the
editor with no options at all. Three instruments, three false readings, and the
one that cost real work is the one that got written down before it was checked.

The honest version of the rule is not "be careful with harnesses". It is:
**a recorded diagnosis is a hypothesis, including the one you wrote an hour
ago**, and the moment a finding is surprising enough to be worth documenting is
exactly the moment to spend one more command confirming it.

What survived the correction is worth noting too. The three port bugs above are
real and were found on the way; they are fixed and tested regardless of how the
ruler was bent. And the carriage returns `ex` emits after each printed line —
which looked like a fourth defect — turned out to be its own `optimize` option:
`pstart()` clears `CRMOD` on the output descriptor so the kernel stops
supplying the return and the editor does it itself. That is reachable from the
user side, which is what makes it a test rather than a paragraph: `set
nooptimize` puts it back, and both halves are asserted now.

### The guard against a stale number was a number nobody guarded

One more thing came out of installing the editor, and it is about the project
rather than about `ex`.

There is a crash probe here that runs every installed binary against every
single-letter option and counts the ones that die on a signal. Its expected
output is not zero — two programs have defects that a VAX had too, and this
project's rule is to record those rather than patch them — so the probe has a
*floor*, written down beside it: fifty-four, being fifty-three from `lex` and
one from `bcd`. The note ends with a sentence I had read several times: *if this
ever reads fifty-five, the new one is the finding.*

I ran it after installing the editor, mostly as hygiene. It read **one hundred
and sixty**, across six thousand three hundred and sixty invocations.

None of the new ones is `ex`. All hundred and six arrived with an earlier batch
of imports, and nothing had measured them, because the probe is not part of the
test suite. It is a manual instrument whose expected output is a number in a
prose file — and a number only a human checks is a number that rots. That is
precisely the failure the note itself was written to prevent, one level further
out: the guard against a stale count was a count with no guard.

Three of the four new programs are this port's most familiar bug, the one where
1985 could read address zero and this machine cannot, triggered as it always is
by an option arriving last on the command line. The nicest of them is `cb`,
whose complaint about an unrecognised flag reads `*argv[1]` — which parses as
the first character of the *next* argument, where the second character of *this*
one was meant, exactly as the switch three lines above it writes. So `cb -a
file.c` has always reported "illegal option f"; only the crash, when there is no
next argument to misread, belongs to this machine. The fourth turned out not to
be that class at all: `cpio -i` reads a valid archive perfectly and faults only
on empty input.

A smaller lesson came with it. I read the probe's log three times while it ran,
and each time it named three programs. The fourth, `diffh`, sorts after `lex`
and simply had not been reached. A partial log is not a population, and the only
honest moment to count is when the thing stops.

### Fixing them, and the one I decided to leave broken

Going back to fix the four produced one genuinely interesting decision, and it
turned on a detail I would not have predicted mattered.

Three of them were the familiar shape and the fixes are one line each. What
makes them more than typo repair is the rule this project applies to them: the
fix has to reproduce **what the VAX did**, not merely stop the crash. Address
zero on a VAX was not a void — V8's binaries put the text segment there, so
reading it returned the first byte of the C runtime startup code, which is
`0x00`. That byte is not a hyphen and not a digit, so each program had a defined
behaviour, and the job is to restore *that*. `diffh` printed "must have 2 file
arguments". `tar` read the empty string as a block size of zero and rejected it
on the very next line, with "Invalid blocksize". `cpio` printed its usage and
exited 2. Each fix is paired with a test that the option still *works*, because
an early `return` bolted in front of the fault would satisfy every other case
while quietly breaking the flag.

`cb` is the one worth dwelling on, because the fix had to *preserve* a bug. Its
complaint about an unknown flag reads the wrong argument, as described above —
and that misreading is upstream's, visible on upstream's hardware, so the
contract forbids correcting it. What the VAX printed, when there was no next
argument at all, was that byte from address zero: a NUL, in the middle of the
message. So the honest repair prints a NUL, and the test asserts the message is
twenty-one bytes long with a zero byte in it. There is something bracing about
writing a test whose purpose is to insist that a bug is still present, and
another asserting an output nobody would ever want.

The fourth I left alone, and my earlier account of it was wrong. I had written
that `cpio -i` was "EOF handling"; it is not. It is an unchecked
`fopen("/dev/tty")` in the change-the-tape prompt, and V8's `/dev/tty` is a link
to `/dev/fd/3` — file descriptor three — so whether it succeeds depends on
whether the program was started with a terminal on that descriptor, which the
launcher arranges and a test harness does not. Then it calls `fgets` on the null
result. And `fgets` is a `getc` loop, and `getc` is a macro that *decrements a
counter inside the structure* — so it does not read address zero, it **writes**
to it. A VAX's text segment was read-only shared. It faulted there too.

That distinction — read or write — is the whole verdict, and it is invisible
unless you go and look at the macro. A read of address zero has an answer worth
restoring. A write to it never had one, so there is nothing to restore and the
port should not invent something. Two other programs here were left alone years
ago for exactly that reason, and this is the third.

The measurement that settled it is my favourite kind, because it turned a
puzzle into a confirmation. Running `cpio -i` under a debugger did not crash —
which looked at first like the bug being unstable. It was not: the debugger
leaves a descriptor three open. The thing that made the crash disappear is the
same thing the diagnosis said causes it.

And the floor is a test now rather than a sentence, which was the actual lesson
of the previous section. A single loop runs those four programs against every
option with descriptor three explicitly closed, and asserts that the surviving
crash list is exactly one entry long and reads `cpio -i`. When I mutate the
source to "fix" that last one, the test goes red — which is what tells me it is
guarding the decision and not just describing it.

The probe, re-run over the same six thousand three hundred and sixty
invocations, went from a hundred and sixty to **fifty-five**. It is worth being
precise about that number, because the obvious thing to write is that it is back
to fifty-four and it is not. Fifty-three of them are `lex` and one is `bcd`,
both long-standing; the fifty-fifth is `cpio -i`, which is new and permanent.
Recording it as fifty-four would leave the next person reading the extra one as
a regression to hunt — which is exactly how the previous number came to be
wrong. A floor is only useful if it says what it is made of.

### The probe had no exit status

Then I put it in CI, and discovered the thing that had really gone wrong.

In its entire life this script had never returned a failure. It printed its
findings and exited zero. Every time. So "the floor" was never a check at all —
it was a person running a command, reading a number off the terminal, and
comparing it to a sentence in a different file from memory. Of course it rotted.
The surprise is not that it drifted; it is that anyone thought of it as a guard.

Giving it one turned out to be more interesting than plumbing. The expectation
is a **list**, not a number — every invocation expected to die, by name — because
a count lets two changes cancel out: fix one crash, introduce another, total
unchanged, nothing to see. And it is checked in **both directions**. A new crash
failing is obvious. A crash that has *stopped happening* also fails, which is
the part worth arguing for: a floor that claims more than it should is exactly
where the next regression will hide, unnoticed. So fixing something now requires
deleting its line from the expectation file in the same commit. That is not
bureaucracy — it makes the removal a reviewable decision instead of a silent
one, and this project's whole method is that decisions should leave marks.

Three smaller things, each a rule this repository already had, arriving somewhere
new. A missing expectation file is a *failure*, not a skip — a test suite here
once reported "12 passed" from the wrong directory because its best case was
wrapped in an existence check. The comparison forces a fixed collating order,
because `comm` needs both sides sorted the same way and a locale belongs to the
machine, not to the project. And a run that was disturbed — something rebuilt
the tree underneath it, which shows up as processes killed from outside — no
longer reads as a pass; it cannot testify either way, so it says so.

It runs as its own job, not appended to the existing one. Thirteen minutes
against well under one: in a single job every push would wait thirteen minutes
for its first signal, and a slow check is a check people stop reading. That is
the same failure as the rotted number, just wearing different clothes.

And I managed to demonstrate one more hazard while building it. I edited the
script eight minutes into a thirteen-minute run of that very script. `sh` does
not read a program into memory and then run it; it reads as it goes. Inserting
lines below the point it has reached shifts everything under its feet and it
resumes mid-token. The run had to be thrown away. There was already a rule here
about not editing things while tests are running — I had thought of it as being
about build artifacts. It is also about the interpreter.

### The first CI run found a crash no local run has ever shown

Which is, of course, the entire argument for putting it there — and it promptly
broke the design I had just finished.

`tar -u` died of a segmentation fault on the GitHub runner. On this machine it
exits cleanly, with a sensible error, every single time. I have run it about
forty times under exactly the conditions the probe uses, and tried to force the
temporary-file collision that looked like the obvious candidate. It will not
reproduce here.

The consequence for the expectation file is not a nuisance, it is arithmetic. I
had built a floor that is checked in both directions, and a crash that does not
happen every time cannot be written into such a thing at all: include it and it
fails wherever it does not fire; leave it out and it fails wherever it does.
There is no entry that is correct on both.

I wrote that up as a *host-dependent* crash — dies on a runner, not on the
development machine — and the very next CI run falsified it inside the hour.
That run exercised the same invocation three more times on a runner, and it
behaved perfectly in all three. So I relabelled it: not host-dependent,
intermittent, cause unknown, filed with the project's other unreproduced
flickers.

That was wrong too, and it was wrong in the more comfortable direction.
"Intermittent, cause unknown" is a diagnosis that asks nothing further of you.

The cause was containment, and it was in my test rather than in the port. When
`tar` is given no archive name it falls back to its built-in default, the tape
drive `/dev/rmt1`. Opening it fails, so — when creating — tar creates it
instead. And creating a file inside the jail resolves against the parent
directory, which is `$V8ROOT/dev`, which *exists*. So `tar -c` quietly writes a
ten-kilobyte tape into the root filesystem, and every subsequent `tar` in the
sweep finds a tape that opens and takes an entirely different path through the
program. Measured: the same invocation exits 1 without the leftover file and 0
with it.

Which means my sweep's results were a function of the order it happened to run
things in. That is precisely the rule the crash probe was rewritten for years
ago — programs reading each other's litter — arriving through a door I had not
thought to close. The old fix gave every invocation a fresh working directory.
A fresh working directory does not contain a program that writes to an absolute
path.

The list of programs needing containment already had `dump` and `restor` on it,
which are the same shape: archivers with a default tape. `tar` is the third
member of that family and it was simply missed, because the list had been
assembled by thinking about which programs obviously write. The check that
actually finds this is not a reading of the source at all — it is hashing the
root filesystem before and after a run and seeing whether it changed. That is
how the classification is verified now, rather than asserted.

Two red builds, then, and both of them were my instrument rather than the
thing it was pointed at. The escape hatch I had just built for the "unexplained"
crash is now empty, because the crash was explained. I have left the mechanism
in place — it is tested, and the next genuinely flaky thing should not have to
reinvent it — but the fact that its only occupant turned out to have an
ordinary cause is worth remembering the next time something looks unknowable.
The bar for declaring a thing unexplainable should be higher than two wrong
labels and an afternoon.

The fix, naturally, was not to add a special case for tar. It was to make the
probe check the thing it had been assuming: take the list of files in the root
filesystem before the sweep and again after, and treat any new path as a
program that got out. That check is three lines and it immediately found a
second one — `/dev/null`.

A program asking to create `/dev/null` inside the jail gets an ordinary empty
file, for the identical reason tar got a tape: creating a file resolves against
its parent directory, and the jail has a `/dev`. This had been happening
forever and was invisible here, because my working tree had had such a file for
so long that it was no longer *new*. Only a machine that had never run the
system could see it — and the fix turned out to be that Bell Labs shipped a
`/dev/null` and we had simply never made one. The build creates it now, beside
the four sibling device names it was already creating. That fix was right about
the name and wrong about everything else, which took another sitting to find;
it has a section of its own further down.

So a containment check turns out to double as a completeness check on the
world: two of the things it caught were not programs misbehaving but nodes
missing from `/dev`. And both were only visible on a machine with no history,
which is a question worth asking of any green test — would this still pass on a
tree that has never been used?

The sharpest version of that question arrived later, and the answer was no. A
build failed on CI with `No rule to make target …/plot.c.a`, and the file was
sitting on my disk. `usr/src/libplot` stores each library's C sources **inside
an `ar` archive** — `tek.c.a`, `plot.c.a` — and `.gitignore` has carried `*.a`
since the first commit, for build outputs. So `git add -A` had silently added
nothing, and 937 lines of Bell Labs source were never committed. Every suite,
and a 7579-invocation crash probe, had passed against source that did not exist
in the repository.

The wider version is worse and is not really about me. Ten `.c.a` bundles are
affected and eight of them are in the *vendored upstream*, so `third_party/` —
the pristine archive the whole fidelity claim rests on — has been incomplete in
git since the day it was vendored. `git log --all -- '*.c.a'` is empty; they
were never tracked once. On a fresh clone, `tools/import.sh` could not have
imported any plot library at all, which is part of why nobody had ever looked
at that tree.

And 190 files, 2.6 MB, are still ignored there by the same three rules: every
shipped library, the troff font tables, the object files. None of them is a
build input, which is exactly why this stayed green for so long — but several
*measurements* in these notes cite them. "The shipped `libm.a` is 216 bytes, one
member, whose entire symbol table is a row of underscores" is a real finding and
it is not reproducible from a clone. A repository that cannot reproduce its own
evidence is a weaker artefact than one that can, and that is now written down as
work rather than as a footnote.

**And the four rules were hiding something better than shipped binaries.**
Dropping them made 346 files trackable, and four of those are already in `src/`,
imported alongside their programs and silently discarded ever since:
`eqn/eqntest.a`, `tbl/samples.a`, `pic/pictest.a`, `hoc/tests.a`. They are `ar`
bundles of **137 test cases written by the programs' own authors** — 37 for eqn,
54 for tbl, 36 for pic, 10 for hoc — sitting in the tree, uncommitted and unrun,
while this port tested those four programs against cases it had written itself.

**Running the other three found two live crashes in `tbl`.** Sixteen of its 54
cases died on SIGSEGV — 30% — while `tests/wavec` was green and had been for the
life of the port. Both are the same one-word defect: an `int` holding a pointer.
`maknew`'s `dpoint` carries the address of a decimal point, and `leftover`
carries the address of a line that would not fit; each is also used as a
boolean, which is why upstream declared them `int` and why it was exact on a
VAX. Here the cast keeps the low 32 bits, and the crash address gives the class
away — `0x1e6058a9`, `0x4ac8141`, both plainly 32-bit values.

The first is the `n` column, which is *tbl's characteristic feature* — numeric
alignment on the decimal point — and twenty bytes reproduce it. `l`, `c`, `r`
and `a` columns are all clean, which is exactly why it survived: the hand-written
tables in `tests/wavec` had never once used an `n`. That is the whole argument for
an independent suite, and it took one run to make it.

Two details worth carrying. Widening the declaration alone fixes neither, because
`(int)str` truncates *before* the value is widened into the `long` — the explicit
cast is the truncation, not the storage. And `leftover` is declared in three
places, one of them an `#include`d non-header, so all three had to move together.
Sixteen failures became one; the survivor is a different class, a null row
pointer, and the suite now asserts that it *still* dies so that fixing it has to
be a decision.

`hoc`'s ten went straight into the suite and it passes them, which is a better
result than the ones I wrote because they are adversarial in a way an author's
own cases are not. `ack` is Ackermann's function: `A(3,3)` is 61, reached in
2432 recursive calls, and it exercises the interpreter's argument stack far
harder than anything I would have thought to write. `fac1` checks 0!, 7! and
10!; `fib2` runs to 14930352.

One of the ten is **not** run, and finding out why was worth the time. `ack1` is
`while (read(x)) { read(y); print ack(x,y) }`, and hoc's `read` is
`fscanf(fin, ...)` where `fin` is the *program* stream rather than standard
input. So `read()` competes with the parser for the same bytes, and stdio's
block buffering means the parser has already swallowed the data by the time
`read` executes. Three invocations were tried — data piped after the program,
data as a second file argument, program as a file with data on stdin — and all
three give the same error at *the program's last line*, which is the tell. It
needs a terminal, like `ex`'s visual mode. Upstream's design, not a port defect,
and the sort of thing that would have been recorded as a bug if I had stopped at
the first failure.

The preventive half is three lines in the import tool, which for its whole life
had never asked git anything: it copies the file, records the hash, prints a
success line, and stops. It now runs `git check-ignore` on the destination and
warns. Writing that guard produced one more instance of the session's most
frequent lesson — the first draft omitted `--no-index`, and plain
`check-ignore` answers about the *index*, so a path that is already tracked
reports "not ignored" whatever the patterns say. The guard was therefore silent
on exactly the invocation I used to test it, and only firing it deliberately,
with the exception line commented out, showed that it did nothing.

So there is a third category now: entries marked as tolerated, removed from both
sides of the comparison, neither required nor forbidden. That is a hole in an
otherwise strict check, and holes like it are how the original number rotted, so
it is kept small and noisy on purpose — the count is printed on every run, the
entries are listed by name, and a separate test asserts that there is exactly
one of them. Adding a second requires editing two files deliberately. An
exemption nobody can see is the failure mode; an exemption that announces itself
every run is a note-to-self with teeth.

What I did not do is explain it. The file records what is *known* — that the
dash is a no-op case so the option really does reach the temporary-file arm,
that the `fopen` there is null-checked, that no filesystem redirection is
involved — and then stops, with a task number. This project has a scar from the
last time I wrote a mechanism down for something I had not reproduced: it went
into four documents and a test exclusion, and there was no bug. An honest "here
is what happens, here is what I checked, I do not know why" is worth more than a
plausible story, and it is much easier to correct later.

### Making the name authentic made the object wrong

The `/dev/null` fix above was: V8 shipped one, we had not, so build one. That is
correct about the name and it broke the device.

Before it, the jail had no `/dev/null` of its own, so the path fell through to
the Mac's — reads returned end-of-file, writes vanished, everything worked. What
the containment check had actually caught was a program *creating* the file and
poisoning that fall-through from then on. Materialising it as a build product
did not prevent that; it did it once and for all, at build time. The breakage
moved from "after the first write" to "always", and the guard that had found it
went blind, because a path already in the list can never be reported as new.

Four things were wrong, measured at the prompt:

```
$ echo hello > /dev/null        # 14 bytes appear in the file
$ echo more >> /dev/null        # 21 bytes now
$ cat /dev/null                 # prints hello / more
$ test -c /dev/null; echo $?    # 1 — it is not a character device
$ ls -l /dev/null               # -rw-r--r--  1 christie 20  21
```

The accumulation is the one the task description named, and it is the least
interesting of the four. The sharp one is the read. `prog < /dev/null` is how
every Unix programmer hands a program empty input, and here it was handing it
whatever last wrote — which is the crash probe's own founding lesson, programs
reading each other's litter, arriving *inside the device whose entire job is to
have nothing in it*.

The fix is the fifth filesystem type and it is two functions. One returns the
path unresolved, so every other operation reaches the Mac's device instead of
our node; everything else is inherited. That is not laziness, it is the rule the
`/dev/fd` type established — Bell Labs' null is two arms of the memory driver,
`case 2: return;` for read and "consume the count, keep none of it" for write,
and the host's device is exactly that, so giving it read and write slots of our
own would invent a difference the kernel does not have. The rootfs node stays,
because `ls /dev` has to list what V8 listed. It is simply never the object any
more.

The second function is one line, and it is there for a reason that is not about
`/dev/null` at all. The two machines agree about this device completely: Bell
Labs' device table says major 3, minor 2, and so does macOS. They disagree about
how a major and a minor are *packed* — Darwin puts the major at bit 24, V8 puts
it at bit 8 — and the shim narrows to V8's sixteen bits with a mask. A mask
cannot preserve a field at bit 24 no matter how wide the destination is, so the
major is not truncated, it is deleted. Every device node the jail reports has
been wrong this way for the life of the port: `/dev/zero` is 3,3 and the jail
says `0, 3`; `/dev/random` is 17,0 and the jail says `0, 0`. For `/dev/null`
that would have meant major 0, which is the *console*. Not a failure — a
plausible wrong answer, which is the kind this project is most afraid of.

I did not fix that generally, and the reason is a measurement rather than a
preference. Once this row exists, every device node V8 actually shipped reports
V8's numbers, from one synthesizer or another. What is left mis-encoded is
exactly the set of nodes V8 never had, where there is no 1985 answer to restore
and the host's own numbers are the honest report. Fixing it properly needs a
rule for a major above 255, which is a different problem with a different
answer. It is written down as its own item rather than folded in here.

Then the tests found something in the tests. Four mutations, and the second one
— make the path resolve through the jail again, i.e. reintroduce the bug — fired
two cases but *not* the one written for it, the case asserting that bytes
written through the jail never reach the node. A mutation that does not fire is
a finding, so I measured instead of theorising, and the node held three bytes
when the run finished: the write had escaped exactly as intended and the case
had passed anyway. The first mutation's run had left its own bytes in the file,
so the second run's "before" reading was already three, and its truncating
`creat` landed back on precisely three.

That is the litter shape again, one level further out than I had ever seen it:
not between programs sharing a directory, nor between test cases sharing a
stream, nor between suite sections sharing a disk image, but **between two runs
of a single case, through the artefact the case is about**. Writing the
assertion as a relation rather than a value — which is this project's standing
rule, and normally the right one — was not enough, because the run that fails is
the run that contaminates the next one's baseline. The case establishes its
baseline now instead of observing it.

And because materialising the node blinded the containment check for that one
path, the probe measures its size before and after the whole sweep. Six thousand
invocations of every installed program against every option is the widest net
in the tree for "does anything write here", and it costs one `stat`.

Then, checking that nothing else was missing from `/dev`, I found the directory
almost empty — on a tree where the tests had just passed. One of the suites
deletes `/dev` on purpose, to prove that the load-average program manufactures
its fake kernel on demand, and then puts back what the build owns. It put back
three names. By now the build owns a hundred and thirty-six, so every full test
run had been leaving the build tree's world without a `/dev/null`, a `/dev/tty`
or a `/dev/fd` until the next build silently rebuilt them. Three things hid it:
every suite that reads those runs earlier in the sequence; running one suite on
its own rebuilds what it needs first; and the *installed* world was never
affected, because `make install` begins with a clean for an unrelated reason. So
the artefact anyone would have thought to inspect was the one that was fine, and
the broken one was the tree you actually work in — which is the direction that
keeps a defect alive longest.

Sitting immediately above that three-name restore was a loop asserting that
`/dev/null` and `/dev/tty` *do not exist* — word for word the claim I had just
corrected in the shim, the same sentence having gone stale independently in two
files. It passed for the most circular reason available: the deletion four lines
earlier had made it true. It was testing its own cleanup.

The restore is a snapshot now, so the suite does not need to know what the build
owns. And the third member of that loop, `/dev/console`, I kept for one draft,
reworded — and left sitting after the deletion, where it could only ever be true.
I had reproduced the defect I was in the middle of diagnosing, inside the fix for
it, and only a mutation caught it: creating the file changed no verdict, and the
new snapshot then dutifully restored my litter. The case is gone rather than
moved, because before the deletion it would assert a gap nobody chose — nothing
builds `/dev/console` by accident rather than by decision, and nothing opens it.
The person writing a fix is the last one who can see it repeating the bug.

### The deferral that lasted one measurement

I had left the device-number encoding alone deliberately, and written down why:
once `/dev/null` had its own row, every device V8 actually shipped reported V8's
numbers from one synthesizer or another, so the only things still mis-encoded
were host devices V8 never had — where there is no 1985 answer to restore. That
argument has a hole in it, and the hole is the word *reported*. I was thinking of
the number as something a program **prints**. Mostly it is something a program
**compares**.

Fifteen places in the tree read it. The ones that matter do not display it:
`fsck` twice, and `df`, ask whether a block device's number equals the number of
the filesystem mounted on it — that is how they decide "this is the root device".
`diff` compares two of them to decide two files are the same device. So two
devices that translate to the same number are two devices the caller cannot tell
apart, and the mask was collapsing them wholesale:

| over | truth | `& 0xffff` | proper re-encoding |
|---|---|---|---|
| this Mac's `/dev` (403 nodes) | 345 distinct | **131** | **345** |
| the mount table | 14 filesystems | **13** | **14** |

The second row is the one that stopped being theoretical. `/Volumes/Cloud` is
major 54 minor 7; an APFS volume is major 1 minor 7; the mask kept only the
minor, so both were device 7, and every V8 program on this machine was seeing one
filesystem where the host sees two. The shim's notes say, in as many words, that
the device number is **the only thing separating** eight mount points that all
share host inode 2. That is the `pwd`-names-the-wrong-directory bug from earlier
in this article, arriving through the other half of the same pair.

What I want to record is not the fix, which is one function. It is that the
paragraph asserting the pair was safe **contained its own refutation**. It read:
*"15 mounted filesystems, whose truncated devs are `0003 0005 0007 0008 000e 0010
0011 0012 0017 001b 001f 0023 0026 88d9` — all distinct, so the pair is injective
today."* Count the values. There are fourteen, for fifteen filesystems. Fourteen
numbers cannot be the distinct images of fifteen inputs; the sentence is
impossible on its own terms, and it had been sitting there being read.

It even carried the correct warning — *"distinct because macOS keeps APFS volume
minors small, not by construction"* — which is exactly right, and is precisely
why someone should have re-run it. A hedge is not a guard. If a claim is only
true by luck, the thing to write down is the test, not the caveat.

And then, having convinced myself by arithmetic, I ran the program. `df`, built
the old way, prints this:

```
/System/Volumes/iSCPreboot  disk1s1  -1968638524 -1014876880 -953761644  52%
/System/Volumes/iSCPreboot  disk1s1       512000       18036      493964   4%
```

The same volume twice, once with negative free space — and `/Volumes/Cloud`
missing from the listing altogether. That is the collision, rendered: `df` found
a device number matching the cloud mount, decided it was `disk1s1`, and read the
block counts out of the wrong superblock. With the encoding fixed, each appears
once and the cloud mount says `mounted on unknown device`, which is simply true —
it has no local block device to report.

Negative free space is not a subtle symptom. It had been in `df`'s output on this
machine for the life of the port, and I found it by running the program *after*
I had already worked out from first principles that it must be wrong. The
arithmetic was the harder route and I took it first. Run the consumers.

### The one red CI run, and the log I had thrown away

Three commits went out in this sequence and the middle one failed. Its
functionally identical successor passed, which is the signature of a flaky test
rather than a broken one — and it was the same failure I had seen locally an hour
earlier and been unable to diagnose, because I had piped the run to `tail -1` and
kept only `make: *** Error 1`. That is the third time in this project that a
filter has destroyed a diagnosis, and the second in one sitting.

CI keeps whole logs, so this time the evidence existed:

```
FAIL ps -T lists the same processes as ps
  want [65603ba829b54599b766e1814be884f4]
  got  [d069892fa093e6a273b65509293cce80]
```

It ran `ps`, then ran `ps -T`, hashed the first twenty process IDs of each, and
required the hashes to match — which is only true if nothing on the machine
started or exited in between. A test asserting a property of the host, in a suite
that has been swept for exactly that three times, surviving in the one place
nobody looked. What made it fire locally was a background loop I had started to
poll CI: it was spawning a process every thirty seconds.

Two things beyond the fix. Hashing a set you cannot then diff is a defect of its
own — two differing MD5s tell you nothing about *which* process moved, and the
replacement prints it. And the stable relation was cheap and sitting right there:
sample `ps` again *after* the `-T` run and intersect the two. Anything alive
across both plain samples was alive during the one between them, so churn can
only shrink that set, never invent a missing entry. It now checks six hundred and
fourteen processes where the old one checked twenty.

### awk, and a program that carried addresses in four kinds of integer

`awk` was the largest single thing missing from the world, and it is the first
program here whose *value stack*, whose *regular-expression table* and whose
*scanner* all keep pointers in objects declared as integers. On a VAX that was
not a design decision; it was the same thing. `int`, `char *` and yacc's
`YYSTYPE` were all four bytes, so writing a symbol-table address into `yylval`
and reading it back as a `Node *` was exact. On this machine each of those
crossings loses the top half of an address.

The one that would have destroyed everything is a single line:

```c
extern int	yylval;
```

That object belongs to yacc, and this port's yacc defines it as a `long` —
which was itself a fix, made years ago, for a token-typing bug found in `pic`
and then swept for across the tree. So the lexer's declaration described four
bytes of an eight-byte object. That alone is a width bug. What makes it fatal
is what gets stored: seven of the assignments are pointers, cast down through
`int` on the way in —

```c
<A>"$0"		{ yylval = (int) lookup("$0", symtab); RET(FIELD); }
```

— covering every variable, number, string constant, field reference and regular
expression an awk program can contain, and the grammar reads each of them back
as `(Node *) $1`. Two more of the same shape sit in the regex engine: `rlxval`,
which holds a character for `/a/` and a heap pointer for `/[abc]/`, and the
`lval` field of the DFA's transition table, which is written as an `int` and
read back as a `char *` about four hundred lines later. Bell Labs wrote the
comment that gives that one away, at the top of the file: *"right contains
value or pointer to value."*

None of the four is a cast that loses a pointer. They are all **declarations
that describe the wrong type**, which is the class the compiler cannot help
with, because a declaration is what it has been told to believe.

The build is unusual for a different reason. awk has nine translation units and
one of them does not exist: `proctab.c` is a table of function pointers, one per
grammar token in token order, written at build time by a program called
`maketab` — which itself has to be compiled and run, and which reads the header
`yacc` produced. Two generators in series, which nothing else in this tree has.
Upstream's makefile opens by admitting theirs does not get the dependencies
right:

```
# This makefile is wrong -- it doesn't properly
# recompile everything when a new token is added
# to awk.g.y.
# Watch out!
```

Ours does, and eight cases in the dependency suite say so — the one that
matters walking the whole chain in a single assertion, from the grammar to
`proctab.o`. `maketab` is compiled by V8's `cc` and run, because that is
literally what upstream's makefile does and because `yacc` and `lex` are already
V8 binaries this build executes. And its output goes through a temporary file
before being moved into place: `> proctab.c` creates the file *before* the
program runs, so a `maketab` that died would leave an empty, freshly-dated
`proctab.c` and the next build would call it current. That is the stale-object
failure this project has already paid for four times, arriving through a shell
redirect instead of a rule.

#### The change I made, measured, and then took back out

`maketab.c` calls `malloc` without declaring it and casts the result to
`char *`. That is precisely the shape a sweep in this tree exists to catch — an
undeclared function returns `int`, so a heap address loses its top half — and
that sweep structurally cannot see this one, because `maketab` is a build tool
and never gets installed. So I added the declaration and wrote a paragraph of
comment explaining why it was forced.

Then I measured it, because the rule here is that a change to Bell Labs' source
has to be forced by the target rather than by a plausible story. The generated
`proctab.c` is byte-identical with and without the declaration, and so is the
emitted assembly, instruction for instruction:

```
	bl	_malloc
	ldr	x9, [sp, #256]
	mov	x10, x0        <- the whole register
	str	x10, [x9]
```

The distinction turns out to be narrower than the rule I had in my head. An
undeclared pointer-returning function is truncated **where the `int` type is
used** — and an explicit cast applied directly to the call is not a use. The
value never has to be materialised as a 32-bit quantity, so there is nothing to
cut. Compare the real instance the sweep found, in `last`:
`asctime(gmtime(&delta))+11` does arithmetic on the int, and there the int-ness
is load-bearing. The declaration came back out and `maketab.c` is byte-identical
to 1985. The measurement is the record.

### The number that was right for nine years and wrong here

awk printed this:

```
3.0000000000000000000 15.000000000000000000
```

where every awk since 1977 prints `3 15`. It is not awk's defect. `tran.c` does
what awk has always done — if a value is integral, print it with `%.20g` and let
the conversion drop the fraction — and our `printf` never dropped it. The
comment directly above the code says the rule out loud:

```c
/*
 * %g: %e if the exponent is below -4 or at least the precision,
 * %f otherwise, with trailing zeros removed.
 */
```

and the twenty lines under it never remove one. A comment stating the rule while
the code beside it implements a different one is the single most repeated shape
in this project's own notes, and here it was sitting in a file I wrote.

There was a right answer to restore rather than a decision to make, and it is in
Bell Labs' assembler:

```
g1:	jbs $numsgn,flags,g2	# `#' given: keep them
	jbs $dpflag,flags,g2	# dont strip if no decimal point
g3:	cmpb -(r5),$'0		# strip trailing zeroes
	jeql g3
	cmpb (r5),$'.		# and trailing decimal point
	jeql g2
	incl r5
```

Seven instructions, including the `%#g` exemption — ANSI's rule four years
before ANSI. Berkeley's `gcvt.c`, sitting in the same directory, strips them
too. This port's C rewrite of that file dropped both.

Fixing it turned up a second defect standing next to the first. `%g`'s precision
counts *significant* digits and `%e`'s counts digits *after the point*, so the
scientific form of `%.Pg` is `%.(P-1)e`. Ours passed the precision straight
through, and `%g` of 1234567 came out `1.234567e+06` — seven significant digits
from a conversion that asked for six. The VAX makes exactly that distinction, in
the other direction: the `%e` entry point increments the digit count on the way
in and the `%g` entry point jumps over that instruction.

Neither defect is visible unless a value's last significant digit happens to be
a zero, or the exponent form is reached. That is why thirty-eight library cases
and every Wave C program had walked past both. Forty combinations now agree with
the host's `printf` character for character, and five of them are cases.

#### One test went red, and it was the test that was wrong

`grap` prints coordinates with `%g`. So every graph this port has ever produced
carried tick labels reading `1.00000`, `1.50000`, `2.00000` where V8 prints `1`,
`1.5`, `2` — and a case in the document-preparation suite had frozen that:

```sh
check 'grap transforms the last point' '1' \
    "$(printf '%s\n' "$grapout" | grep -c 'xy_gg(10.0000,100.000)')"
```

That is the third time in this project a test has been calibrated against broken
output and gone red when the program was fixed. The first counted drawing
commands that only matched because every coordinate was zero; the second
expected a full-width answer from an arithmetic expression that should have
wrapped at 32 bits. The discipline that comes out of it is short: when a library
fix turns a test red, find out which of the two was wrong before assuming it was
the fix. Here what the case *discriminates* has not changed at all — those
numbers travelled through argument slots as a struct passed by value, and a
broken struct-by-value still shows up as wrong numbers. Only the spelling of the
right answer moved.

### The set that was recorded as finished, and was not

The single-file commands — the ones that are one `.c` in `cmd/` and nothing
else — were done in a batch of thirty-seven, and the note I wrote afterwards
said so: batch one imported "all 37 single-file commands", and the next batch
was to be the directory programs. Re-measured before starting that next batch,
**thirty-six of the still-missing programs have a bare `.c` sitting in `cmd/`**.

Most of them are genuinely out of scope, and it takes reading each one to say
so: `ar`, `ld`, `nm`, `ranlib` and `size` are the object-format tools this port
deliberately borrows from the host; `halt`, `reboot`, `init`, `mount`,
`swapon`, `getty`, `savecore` and a dozen more act on a machine rather than on
files. Five are not. So the honest position was never "the single-file set is
exhausted" but "the single-file set minus the ones nobody triaged."

They are in now. `uuencode` and `uudecode` round-trip a file byte for byte.
`spline` interpolates a hundred points through four, which is also the first
new exercise of the floating-point path since the math library was fixed.
`stty` is here because of something `ex` established: it had been recorded as
blocked on `tty_ld` and `ntty_ld` being "genuinely kernel state", and they are
twenty-four initialised integers in a library file nobody had imported. With no
controlling terminal it says `stty: can't open /dev/tty`, which is the truth —
this world's `/dev/tty` is a link to `/dev/fd/3`, so with no fd 3 there is
nothing to open.

`mc` — columnate — carried a bug this project has now fixed eight times:

```c
while(*argv[1]=='-'){
	--argc; argv++;
```

Every arm of the switch inside consumes an argument, so once the loop has eaten
the last option `argv[1]` is the vector's null terminator. For a program whose
entire interface is `mc [-][-WIDTH][-t] [file...]`, an option in the final
position is the ordinary invocation, not an edge case. `mc -20 < file` crashed
after reading the width and before reading a byte of input. A VAX mapped
address 0 — crt0's first byte, `0x00` — which is not `'-'`, so the loop simply
ended and `mc` read standard input. That is what the guard restores.

Its other change is an include: `#include "/usr/jerq/include/jioctl.h"`, an
absolute path into the 1985 filesystem, which is the one kind of include the
preprocessor cannot be told to resolve elsewhere. The header is authentic and
the rootfs already carried it — `ls(1)` needed the same thing, and the rule
that copies it named `mc(1)` in its comment as the other consumer, written
before `mc` existed here.

I spent two experiments on the wrong suspect first. The build failed on that
include, and the include is inside `#ifdef JERQ`, so the natural reading was
that V8's 1985 preprocessor evaluates `#include` inside a false conditional.
Tested with a space after `#ifdef` and again with a tab, against a path that
does not exist: both compile clean. The preprocessor is fine. `mc.c`'s **first
line** is `#define JERQ` — the file turns the Blit code on itself, where the
other two consumers leave it to the compile line and nothing defines it. One
command reading the top of the file would have settled what two experiments
could not.

### Four programs chosen for four build idioms

The fifty-five remaining programs live in directories rather than as a single
`.c`, and that is the whole difficulty: each one has a shape. So the next four
were picked for their shapes rather than their size, because those shapes are
what the other fifty-one will need.

`expr` is a grammar and nothing else — one yacc file, one object, and the
scanner is C in the third section rather than a lex file. `m4` is three sources
and a grammar. `pack` builds two programs from one directory and makes the third
name, `pcat`, a hard link to `unpack` — which is `ex`/`vi` at a smaller scale,
and measured the same way: the shipped `pcat` and `unpack` are byte-identical at
10240 bytes with the link lost when the archive was extracted, so the inode is
what gets asserted rather than a `cmp`.

`diff3` is the interesting one. The command in `/usr/bin` is a **shell script**
and the binary it runs lives in `/usr/lib` — upstream's install arm is
`mv diff3 /usr/lib; cp diff3.sh /usr/bin/diff3`. The script has no `#!` line,
V8's `sh` runs it anyway, it calls V8's `diff` twice, and it execs
`/usr/lib/diff3` by absolute path. So a single invocation of `diff3 a b c`
exercises the shell, the diff, the jail's `/usr/lib`, and the helper — and it
produces a correct three-way merge:

```
====
1:2,3c
2:2,3c
3:2,3c
```

#### The case that went red on the import, which is the case working

Importing `diff3` turned a test red immediately, and the right one. There is a
case here that compares three independent sources for *where a command
installs* — Bell Labs' `Admin/dest` tables, the shipped directory tree, and each
program's own makefile — and asserts that the makefiles and the tables disagree
about exactly two programs. `diff3` is a third: its makefile says `/usr/lib`,
the shipped tree has `/usr/lib/diff3`, and `Admin/dest` answers `/usr/bin` by
fall-through because `diff3` is in none of its tables. Two sources against the
fall-through, the same pattern `cpp` established. The case existed to notice
exactly that, and it did.

It also flushed out a latent bug in its own parser. That sweep reads each
makefile's install arm and takes the destination from the third field —
correct for `cp sh /bin/sh`, and wrong for `pack`'s
`cp pack unpack /usr/bin`, where the third field is the *second program*. So
the sweep reported pack's destination as "unpack" and called it a
disagreement: a false positive in the one place whose entire job is finding
real ones. The destination is the last field, and the program may be any of the
sources.

And writing that fix broke the suite in a way the file had warned about, twenty
lines above where I was typing. The parser is an `awk` program inside a
single-quoted shell string, and there is a note in it that reads *"No
apostrophes in here: the whole program is inside a single-quoted shell
string."* I wrote a comment containing `pack's`, which closed the string, and
the shell reported a syntax error on a `for` loop that was perfectly valid awk.
The note is now longer by a sentence saying that a comment closes it just as
well as code does.

`diff3` also carried the argv bug for the ninth time — `if(*argv[1]=='-')` as
`main`'s very first statement, so a bare `diff3` dereferenced the null
terminator before anything was checked. Unlike the others this one is an `if`
rather than a loop, so only the bare invocation was affected. A VAX read `0x00`
there, the test failed, and the argument-count check below it printed
`diff3: arg count`. That message is the answer; the guard restores it.

### `struct`, where one `#define` was the whole fix — and a cost estimate that
### was far too large

Brenda Baker's `struct(1)` turns Fortran into Ratfor: 40 files, 5695 lines. It
compiled and linked immediately and then SIGSEGV'd on the first line of real
Fortran it was given. The first defect was the familiar one — five allocators
returning pointers through an implicit `int` — and fixing it moved the crash
into `fixvalue`, whose parameter for a pointer is declared `int`.

I wrote that up as "not one more line but a pass over the module, and an
unknown number of the other 39 files may do the same." That estimate was wrong,
and wrong in the direction this project rarely errs in: **too large**. What
`1.hash.c` is doing is not a style of writing `int` for pointer. While a
Fortran label is still unresolved it threads a *fixup chain through the graph's
own cells* — one function stores the address of a cell, each cell holds the
address of the next, and a later pass walks the chain overwriting every one
with the vertex number that finally resolved. So a cell means **a vertex number
or a pointer**, chosen by whether the label has been seen yet, and the only
thing that has to change is that the cell be pointer-sized. `#define VERT int`
became `#define VERT long`, and that is the fix.

Widening it broke 13 of 38 objects, and both classes turned out to be
contradictions that were already in the source:

One array was **declared at two different widths in two headers** — `VERT
*after` in one, `int *after` in another — while being *defined* as `VERT *` in
a third file. `#define VERT int` had made the two spellings the same type, so
no compiler in forty years had cause to speak. Widening VERT is what made them
disagree, and seven translation units refused at once.

The other was a table that has to share a width with a graph cell, because the
macro reading it is a ternary yielding *either* a table entry *or* a cell: a
non-negative entry is the arc count, a negative one is the offset of the count
inside the node. The two arms are interchangeable by construction, so the
ternary has no type unless they agree.

With those two lines, all 38 objects compiled — the same count as the
pre-change baseline, rebuilt in a scratch tree as a control.

Then the crash moved again, into `printf`, and the third defect is **the line
beside it in the most literal form this repository has recorded**. One line of
`def.h` declares two functions returning `VERT *`. The very next line declares
six more as `int *`. All eight return the address of a graph cell. Upstream had
spelled one type two ways on two adjacent lines, and a VAX could not tell them
apart. The consequence was not a warning but silent half-width access: a macro
that dereferences one of those six stored only the *low half* of a string
pointer, so `%s` was handed a truncated address.

What identified it was that the faulting address **changed between runs** —
`0x4b4686c`, then `0x1de0686c`. ASLR moving it is what says the value is a real
heap pointer with its top half gone, rather than a constant. A stable address
would have meant something else entirely.

Two instruments were silent throughout, and it is worth saying exactly why,
because between them they cover one direction twice and the other not at all.
The compiler warns on a pointer mismatch in an *assignment* and not in a
`return`, so six functions returning the wrong pointer type produced no
diagnostic — a whole-module warning diff against the baseline named exactly one
new site, and it was a different bug. And the truncation sweep this project
uses reads *call sites*, so a function narrowing its own result leaves nothing
at the call to match; it reported zero hits over this binary throughout, which
was correct. What found the bug was reading two adjacent declarations.

The fourth defect is the same class pointing the other way, and it is the one
that does not crash. **Eleven functions return a vertex and were declared
implicit `int`.** `UNDEFINED` is −1; returned from an implicit-`int` function
it comes back in the 32-bit half of the register, a write to which zeroes the
top half architecturally, so the caller reads 4294967295. The test for
"is this defined" is `v >= 0`. After widening the cell type, **every undefined
value returned by one of those eleven tested as defined.**

I found five of them by sweeping for `return(UNDEFINED)`, which was the wrong
question, and six more by asking the right one: which functions return a
variable *declared* as a vertex while carrying no return type of their own.
Two of the six that only the second sweep could find do not set a field, they
set the shape of the output — they fill the dominator and loop-header tables
that the tree builder branches on. A truncated value there does not crash. It
compiles a different program.

The last defect took three wrong turns to reach, and the route is worth more
than the fix. First I dumped the whole graph row for the failing node, which
corrected me twice at once: one cell read as `4425033912` and looked like
garbage, but it is a string pointer, and the tell that it is *correct* is that
it **changes between runs** — ASLR moving it is what proves a genuine 64-bit
pointer, which is exactly what widening the cell was for. And the two cells
that really were wrong turned out to be provably right when the tree builder
finished writing them.

So I stopped reasoning about mechanism and bisected by phase instead — three
print statements in the driver, one run. The cells were clean after every
phase up to the last one. The corruption was a single line inside it:
`negate()`, which flips an `IF`, does its work by calling a small generic
`exchange(p1, p2)` helper. That helper is declared to swap two `int`s. Its four
callers pass three different kinds of thing — two of them pass *pointers* — and
on a VAX all three were four bytes wide, so one helper covered all of them.
Under LP64 it swapped the low half of each object and left the high halves
where they were, which is why each cell ended up holding its own correct low
word and the other one's high word.

**And it was in my own sweep output.** I had run a search for `int *`
declarations and printed a line reading `2.dfs.c:148: int *p1,*p2;` under a
heading I had written as "int\* locals" — then scanned past it, because I was
looking for locals. It is a K&R parameter list, which is the one place a
declaration sits alone on a line and is not a local. Two further rounds of
instrumentation went into re-deriving what that line had already told me.

With that one word changed, `struct` does the thing it exists to do:

```
      SUBROUTINE D(N)              subroutine d(n)
   10 N = N - 1                    REPEAT
      IF (N .GT. 0) GOTO 10   -->  	n = n - 1
      RETURN                       	UNTIL(!(n .gt. 0))
      END                          return
                                   END
```

A `DO` loop with a `GOTO` out of it becomes `DO … { break }`. A three-way
`GOTO` branch becomes `IF / ELSE IF / ELSE`. A computed `GOTO` becomes a
`SWITCH` with `CASE`s. Every `GOTO` is gone, replaced by the structured
construct it was standing in for — which is Brenda Baker's whole point, running
on hardware built forty years after she wrote it.

That was still not the command working, though. `struct` is a shell script
that pipes the restructurer into a second program, `beautify`, which
pretty-prints the result — and `beautify` crashed on the first real input
while exiting cleanly on an empty one, because an empty file never reaches its
lexer. It had the same bug `awk` had, in the same place: the lexer declared
`yylval` as an `int` while the grammar side had it as a `long`, and every value
the lexer puts there is a string it later frees. The sweep that found it is one
this project wrote down after `awk`, which is the first time one of these
documented sweeps has caught a second instance on its own.

The last one is the nicest, because the fix touches no source at all. The
parser is *checked in* — generated by 1985's yacc and shipped as C — so it
carries that yacc's default, `#define YYSTYPE int`, and the whole value stack
was therefore 32 bits wide. But the line above it is `#ifndef YYSTYPE`, an
escape hatch upstream shipped for exactly this, so one `-D` on the compile line
fixes it and the 1985 parser stays byte-identical.

**And the first attempt did nothing, silently**, for a reason this project had
already written down about something else: make does not track a change to a
recipe. Adding the flag left every object up to date, so the program was
relinked from the old one and crashed in precisely the same way. What caught it
was compiling the same three-line fragment standalone — the flag worked there
and not in the build, which points at the build system rather than the
compiler. The instrument that settled it was `nm`: the symbol went from 4 bytes
to 8 and the value stack from 600 to 1200. Nothing in the source had changed,
so no diff could have told me either way.

`struct(1)` is installed now, the way V8 installed it: two binaries in a
`/usr/lib/struct` directory of their own and the script as the command, with
eight tests covering the four structured constructs separately so that a
regression in one cannot hide behind the others.

There is also a hazard I found and deliberately did not fix, because it is not
reachable by anything I can test: the graph is allocated by the program's own
arena allocator, which sub-allocates inside a larger block, and then grown with
the C library's `realloc` and `free`. Neither may legally be called on that
pointer, and the `free` comes *after* the `realloc` that already released it.
It fires at 400 nodes — a large Fortran routine, which is precisely the input
nobody tries until the small ones work.

It is deliberately still not installed. `struct` exists to turn `GOTO`s into
loops, so a `struct` that fails on a `GOTO` fails at the one thing it is for,
and a command in the world that dies on its own primary input is worse than a
command that is not there.

### csh, and a bug that half the world's machines cannot see

The C shell is 12,524 lines and it carried four apparent blockers. None of them
survived measurement: `xstr` was already ported, `-ljobs` turned out to be a
real 6,348-byte archive rather than another empty stub like `libm`, and four of
libjobs' seven files are VAX assembly whose contents — `killpg`, `setpgrp`,
`wait3`, `getwd` — this port already had. The library reduced to one 185-line C
file, and the link came down to exactly two undefined symbols.

One of those two is the interesting one, and it is **not a missing symbol but a
missing memory model**. csh asks "is this pointer heap or static" by comparing
against `end`, the linker's end-of-BSS marker, because a VAX laid text, data,
bss, heap and stack out in that order. This shim takes its heap from an
anonymous `mmap`, so the arena sits wherever the kernel puts it and is not
contiguous with anything. Defining *some* `end` symbol would have compiled and
answered plausibly and been wrong, which is worse than not linking. The shim
knows the arena exactly, so the fix is a range test — which needs no ordering
assumption at all and comes out *stronger* than the original, because it also
refuses to free a stack pointer.

Getting that far turned up three defects in code csh merely reached first. The
signal header had never been imported, so it was still 1985's, and two of its
three macros truncated a function pointer through an `int` — the third read only
bit 0 and was accidentally correct, which is the same distinction that made `~x`
safe in an earlier sweep. `getpgrp` was missing while `setpgrp` was present.
And `wait3` handed the kernel the wrong struct: V8's third argument is a
40-byte `struct vtimes`, Darwin's `wait4` writes a 144-byte `struct rusage`, so
it wrote 104 bytes past a stack variable and over the return address. csh ran a
command, printed the output correctly, and jumped to address zero.

With those fixed, csh links with nothing from the host and runs the entire
language — and hung on every external command. The output was right and arrived
first; only the exit never came. I shipped it that way for exactly one commit,
built but deliberately not installed, with a stack sample that named the
function it was stuck in and two eliminated hypotheses beside it.

**The stack sample was true and the conclusion drawn from it was wrong.** It
showed `pjwait` asleep waiting for a `SIGCHLD`, so I spent the session on the
signal machinery — and the signal machinery had been correct the whole time. A
probe reproducing `pjwait`'s loop outside csh worked perfectly on the first run,
which is what finally pointed somewhere else.

The actual bug is one word. `struct process` declares `short p_pid`. csh stores
what `fork` returned and later compares it against what `wait3` returns, and on
this machine pids run to 99,998 where a VAX wrapped at 30,000 — so 45,267 went
into the field and −20,269 came back out, the same sixteen bits read as signed.
The comparison never matched, the "still running" flag was never cleared, and
the shell waited for a child it had already buried.

What makes this one worth writing down is not the fix but **who could have seen
it**. A freshly booted machine hands out low pids. Every one of these cases —
run a command, run a pipeline, run twenty in a row — would have passed against
the broken shell on a continuous-integration runner, because a runner is always
freshly booted. That is the same property that let a 16-bit process id survive
elsewhere in this port for months. So the test that actually guards this is not
any of the behavioural ones: it is a check on the *width of the field*, which is
wrong at every pid, next to a line that prints how high this host's pids go and
says out loud when the behavioural cases cannot see anything.

The sweep that followed found the same class one more time, in `w`, where a
`short` copy had been left behind when this port widened the process structure
it is copied from.

### And the shell had a second bug, which CI found and I could not

Installing csh turned the build red on the next push, with one case failing:
backquote substitution had produced nothing. It passed a hundred times out of a
hundred on my machine.

Running it eight ways at once found it — two failures in four hundred and
eighty. The measurement that actually located the bug was not a debugger but a
comparison: V8's Bourne shell, doing exactly the same thing under exactly the
same load, failed zero times in four hundred and eighty. The two shells differ
in one relevant way. csh installs a handler for the "child has died" signal;
`sh` installs none. So only csh can have a blocking read interrupted — and
csh's backquote reader treats an interrupted read as end-of-input. A child
dying at the wrong microsecond ended the substitution early, and the shell
quietly produced an empty string instead of the answer.

The interesting part is whose bug it is. On a VAX that read is *restarted*
rather than interrupted, so the code is correct on its own hardware. This port
had a note recording the opposite — "no automatic restart; V8 programs expect a
slow read to fail and check for it" — and that note was **right, and
incomplete, which is worse than wrong**. V8's kernel decides in a single flag:
a process that installs a handler through the older `signal` call gets the
interrupt, and one that uses the newer reliable interface gets its read
restarted, in the same kernel, three files apart. csh uses the newer one. The
note had been written when nothing in the port used that interface at all, so
it had stayed plausible for years.

One line of shim, and the failure rate went from two in four hundred and eighty
to zero in twelve hundred. What guards it now is not a repeat of the failing
scenario — at four tenths of a percent no such test is worth anything — but the
underlying property, asserted as a *pair*: one case per interface, because a
blanket rule in either direction satisfies half of them, and a single case would
have been passed by the bug. The case is made deterministic by a small trick:
the signal handler is the thing that writes to the pipe, so the read is
interrupted, the handler supplies a byte, and a correct restart returns it.

It also refuted something I had written a few hours earlier, in the same
document, about the previous bug: that a continuous-integration runner hands out
low process ids. This one reached 98,638. A runner is a machine with no
*history*, which is not the same as a machine with a low counter — the counter
climbs with everything the job has already spawned, and a full build plus
seventeen test suites spawns a great many.

### Moving the tree to another disk, which found two more

Halfway through that work the repository moved to a different machine and a
different physical volume. The tree had been green minutes earlier — 2,451
assertions across seventeen suites — and the move broke two of them.

The first was a test. A case linked `/bin/cat` into the suite's temporary
directory to prove that `ln` resolves its argument inside the jail rather than
reaching the Mac's `/bin`. A hard link cannot cross a filesystem, and the
temporary directory and the repository were now on different disks, so it
failed with a cross-device error and reported that `ln` had produced nothing.
The case had also only ever checked the *first* of the two names — the second
was a relative path in a host temp directory that never went near the jail. It
now links to a path inside the rootfs, which is on the same disk as `/bin/cat`
by construction, and asserts by inode number rather than by file size, because
a hard link is one inode with two names and two identical copies would pass a
size comparison while being exactly what a broken link leaves behind.

The second was not a test. `df` had been reporting the wrong number for the
root filesystem since the day it was written, and only a tree on a non-root
volume could show it. The program finds which device a mount point lives on,
takes its device and directory columns from that entry — and took its *numbers*
from the path it was originally handed. Inside the jail those are different
things: `/` resolves to the port's own root directory, so the row was correctly
labelled with the volume holding the rootfs while reporting the block count of
the machine's real root. Two rows naming one filesystem and disagreeing by a
factor of two. The fix is one line; the interesting part is that the test
standing next to it had the same assumption from the other end, comparing a
single row against the host's figure for that row's device — which agrees
exactly when the bug is invisible. When a test and the code it checks share an
assumption, neither can catch the other. It compares every row by mount point
now, and replaying the old defect against it names two bad rows where the
previous form saw one.

A second machine is a cheap audit. Continuous integration only ever runs on its
own disk layout, so it cannot ask this question at all.

## What is left

Phases 0 through 4 are done, Phase 6 is done, and **Phase 5, the Blit terminal,
is dropped** — `sam` and `acme` came to macOS natively through Plan 9 from User
Space, so the software the Blit is remembered for is already here.

The filesystem work is finished. `mkfs(8)` writes an image; V8's own kernel
opens a file in it by name and hands back bytes that `cmp` says are identical;
the write half creates, grows past the superblock's cached free list, deletes
and restores the accounting exactly, with three of Bell Labs' own checkers
pronouncing the result a filesystem. The mount is a **server**, because putting
the kernel in the client collides with twenty-nine programs and could not
survive `exec` even if it did not — one connection per open file, so the socket
itself is the descriptor and nothing has to be inherited through a table. It
reads, it writes, `mv` of a directory works across directories, and you can
`cd` into it and `pwd` from inside with `getwd.c` unmodified.

What remains is breadth, and it is now the main line rather than a coda. The
port installs **174** of the 286 V8 shipped, and the ones still missing mostly
have source sitting in the tree.

### struct, which turns GOTOs back into loops

The last of the batch went the other way, and the decision is the interesting
part. `struct` is Brenda Baker's Fortran-to-Ratfor restructurer — 40 sources,
5695 lines. It compiles: 37 objects, no failures. Both its binaries link with an
empty undefined-symbol list. And then it dies on its first line of Fortran.

The reason is the class this whole port opens with. Its five allocators return
pointers through an implicit `int`, and `challoc` stacks two truncations in four
lines: `int i; i = malloc(n); return(i);` — one into the local, one by returning
from a function nobody declared. It fires at the program's first real work,
`hashtab = challoc(...)` followed by a write through `hashtab`. Declaring what
the allocators return fixes it without touching a statement, and the header to
put them in is the one upstream's own makefile already names as every object's
dependency.

Fixing it **moved** the crash rather than removing it — to a function whose
pointer parameter is declared `int`, in a file that uses `int` for pointers as a
matter of style. That is not one more line; it is a pass over the module, and
possibly over the other 39.

So `struct` sits in `src/` with the allocator fix, out of the Makefile, exempted
by name in the suite that would otherwise notice, and with a `PORTING.md` saying
where the next person should start. A program installed into the world that dies
on its own primary input is worse than one that is not there: the crash probe
would gain a floor entry, the imported-equals-installed guard would go green on a
lie, and the world would grow a command nobody can use. Stopping at a measured
boundary is a result; shipping past one is not.

### qed, and a signal handler that lost half its address

Thompson's editor — `ed`'s ancestor, fourteen sources — went in next, and twelve
of its fifteen files are byte-identical. The three that changed are one defect
with five faces.

`signal(2)` returns the *previous* handler, a function pointer, and `qed` saves
it across a shell escape to put back afterwards. Upstream stores it in an `int`,
which is exact on a VAX and keeps the low 32 bits here. It is not latent:
`main.c` installs the real function `interrupt` for SIGINT, so by the time a
`!command` saves the handler the value being truncated is a text address, and
the next `^C` after that escape jumps to half a pointer. Measured with a program
that does exactly what `qed` does — a handler at `0x100ecc660` comes back as
`0xecc660`.

Two details are worth more than the fix. The variable had to become `long`
rather than a function-pointer type, because it doubles as a **sentinel**: the
restore is guarded by `if(savint>=0)` with −1 meaning *nothing saved*, and a
pointer type makes that comparison meaningless. And the same variable is
declared in **two files** — defined in one, re-declared in another as an
implicit-int `extern` inside a function, with no header between them. Widening
the definition alone would have left that second unit reading four bytes of an
eight-byte object: silently, on a little-endian machine, and correctly for every
value below 2³¹. The sentinel test would have kept working while `signal` got
half an address.

**And the compiler's diagnostic did not change.** v8cc warns
`illegal pointer/integer combination` at all five sites before and after,
because the warning is about the *kind* mismatch and not the width. A build that
counted warnings would have called the fix a no-op.

### Seven more, and the two most useful things were not in any of them

The latest batch was scoped as ten small directory programs and turned out to be
seven, a library, and two findings that had nothing to do with the programs being
ported. That ratio is now the normal one.

`hoc` is Kernighan and Pike's calculator — a grammar, four sources, and the first
program here to reach V8's own maths through a function pointer stored in a symbol
table. `sqrt(2)` returning `1.4142136` is a narrower claim than it looks: two
separate defects had to already be fixed for it, a declaration that promised
`float` where a `double` comes back in a different register, and a calling
convention that put doubles in the integer registers. Neither had ever been
exercised through an indirect call.

`p`, a pager, turned out to carry **the third copy of `spname` in the tree**, and
it is not the one that was already repaired. `sh`'s copy is the later rewrite,
with a bound test; this is the original, without one. Raising `DIRSIZ` from 14 to
254 — a change this port made years ago — broke both, and the difference is
instructive. In `sh` the guard was written as `newname[128-DIRSIZ-2]`, which went
*negative*, so the function returned "no suggestion" on the first pass and `cd`
stopped correcting spellings: loud, immediate, and found. Here there was no guard
to go negative, so the same change quietly turned an 80-byte path buffer into one
a single 254-character filename overruns. **A missing guard is harder to find than
a wrong one**, because the wrong one fails visibly the moment its premise moves.

The two findings outside the programs:

**`usr/src/libplot` exists, and nothing had ever looked at it.** Three of the
original ten — `graph`, `plot`, `prof` — appeared to share one blocker, and it
is a tree of seven plot libraries sitting a directory over from the one every
previous survey swept. That is `vi` all over again: something recorded as
missing that was in the tree, unread.

What I then concluded about those libraries was **wrong**, and the correction is
the more useful half. I measured what `libplot.a` defines — `subr.o` and
`whoami.o`, 1008 bytes — grepped `graph.c` for the six primitives it calls,
found none of them defined, and wrote down that `-lplot` would not have linked
on a VAX either. Every step of that is accurate and the conclusion is false.

`graph.c`'s fourth line is `#include <iplot.h>`, and `iplot.h` is a file of
**macros**: `#define erase() printf("e\n")`, `#define line(a,b,c,d) printf("li
%g %g %g %g\n", …)`. The plot interface is a header, not a library. A program
that includes it emits `plot(1)`'s textual command language and calls nothing at
all — measured, `graph.o`'s undefined symbols are `atof`, `ceil`, `fabs`,
`floor`, `log10`, `malloc`, `printf`, `realloc`, `scanf`, `sprintf`, `strcat`,
`strcpy`, `strlen`, `ungetc`, and not one plot function. So `libplot.a` is
*complete* at two members: `putnum()` is the one thing the macros cannot express
inline, because the spline and fill macros pass arrays, and `whoami()` names the
device. `whoami` is in fact what reveals the design — it returns `"general"` in
`libplot`, `"tek"` in `lib4014`, `"hp"` in `lib2621`. Seven interchangeable
devices, one per terminal.

**A grep cannot tell a macro invocation from a call**, and that is the whole of
the error. `line(` matches both. The same shape had already cost this port a
crash: `awk`'s `execute` is a macro that dereferences its argument in front of
the null check one call deeper, so reading `real_execute` proved nothing about
what `execute` does.

`graph` and `prof` were therefore never blocked and needed no source change at
all. What genuinely needs a device library is `plot(1)` itself: `driver.c`
parses the language and dispatches through a table of function pointers, so it
references 28 primitives for real, and `lib4014` defines all 28. That makes the
Tektronix renderer buildable on upstream's own link line, and `graph | tek` now
produces real 4014 escape sequences. `hpplot`, its sibling, wants `libcurses` —
43 unported files — and is the honest remainder.

**And `nlist(3)` had a null dereference that no previous consumer could reach.**
`dmesg` and `showq` are grovelers: they look a symbol up in a kernel's name list
and read it out of `/dev/kmem`. Neither can answer here — the shim manufactures a
name list holding two symbols and neither program's are among them — and saying
so honestly is the whole reason to port them, following `w`, which says `No mem`.
`dmesg` crashed instead. The bug is in libc, in a file byte-identical to
upstream's: the loop that matches a symbol against the caller's list walks to the
list's terminator and reads through a null pointer. On a VAX address 0 held a
zero byte and the loop stopped; macOS leaves that page unmapped.

The guard is **at the top of the same function** — the counting loop
over the same array has the null test that the matching loop lacks. And nothing
had reached it because the matching loop breaks at the symbol it wants: every
caller the port had asks for symbols that are *present*, so none of them ever
walks as far as the terminator. `dmesg` is the first program here to ask for one
that is absent, which is precisely the honest-refusal path, and it faulted on the
way to reporting it. The fix is one `&&`; the test that matters is the control,
because a repair that made the function return early would silence the crash and
break `load`, which reads a symbol that *is* there.

- **Visual mode.** `ex` edits; invoked as `vi` it correctly answers that open
  and visual must be used interactively, which is right and also the limit of
  what can be tested without a pseudo-terminal. Driving the screen half — and
  with it `tgoto`, the cursor-addressing code that has a probe but no real
  consumer — needs a pty in the suite.
- **The rest of the terminal stack.** `libcurses` is 43 files and now has the
  library it sits on. The launcher still sets no `TERM`, which is a one-line
  decision that has to be made honestly rather than defaulted.
- **The remaining programs with grammars** — `expr`, `m4`, `bc`, `qed`,
  `struct`. `awk` is in, and it turned out to be the interesting one: the
  build now runs the ported `yacc` and `lex` per program, and in awk's case a
  third generator that the build has to compile before it can run.
- **The rest of the plot family.** `hpplot` — `plot -Thp` — wants `lib2621`
  and `libcurses`, which is 43 unported files; the pen plotter, the troff
  renderer and the two Blit back ends are the remaining four device libraries.
  And `/usr/bin/plot`, the dispatcher that picks a renderer from `-T`, is a
  16-line shell script V8 ships with **no source**, so installing it would make
  it the first program in this world the port did not build.
- **The language systems**: Fortran with its two runtime libraries, both
  present upstream and unimported, plus `efl`, `ratfor`, the C++ front end and
  `lcomp`. These are the largest ports left and the most interesting,
  because a Fortran compiler that runs on a Mac is a claim of a different
  order from a filter that does.
- **Two small honesty items.** The world installs Bell Labs' `/etc/ttys`
  unchanged, and it describes a VAX's serial lines on a machine that has none —
  the synthesized password file is the precedent and the argument is identical.
  And a package doing `chmod 4755` currently sets a real setuid bit on a real
  Mach-O binary: harmless while the file is user-owned, and an escalation the
  moment anything is root-owned, and impossible to make mean "root over the
  jail" because the host decides what a setuid exec does.
- **The SIMH cross-check** — described below, and still the best test available.
- **An FSKit host client**, so macOS can mount the V8 world, alongside the Blit
  terminal app.
- **V9**, which needs `mk` first, and a test already fails the day the first
  `mkfile` appears so that the make-guarding hook cannot wave a whole new build
  system through while reporting success.

Two things are open and honest about it: one unexplained 70-byte stderr write
from `refer`, seen once and never reproduced in fifty runs — the case now
captures content so the next occurrence diagnoses itself — and `w`'s full form,
which needs a `/dev/mem` that does not exist and says `No mem` rather than
guessing.

---

### Three commands that cost nothing, and a number that disagreed with its own command

Re-measuring the breadth figure was meant to be bookkeeping. It produced two
corrections and three free programs.

The figure said V8 shipped **286** commands. Re-running the command written
down beside it gave **287**. Neither is wrong: `find … -type f -perm -u+x` is a
count of *files*, and `/usr/bin/procmount` and `/etc/procmount` are two
different binaries — different inodes, 9216 bytes against 7168 — sharing one
name. So 287 files, 286 names. What was actually broken is subtler than a stale
number: **the recorded command does not produce the recorded number**, and the
gap is exactly one duplicated name that nobody had tripped over. Re-measuring
alone would have "corrected" 286 to 287 and silently redefined the quantity;
trusting the prose alone would have missed the disagreement. Only running both
and reconciling them finds the fact about V8 that neither number holds.

The same paragraph had a second defect that no guard could see. `tests/wavea`
checks the article's command count against one derived from the rootfs, and it
does it by capturing the first number out of *"the port installs **163** of the
286 V8 shipped"*. Both numbers are in one sentence, four words apart. The regex
takes the first and walks past the second — so the guarded half stayed exact
for months while the unguarded half sat wrong in the same line of prose.
Proximity to a test is not coverage by it.

Then the useful part. Comparing what V8 shipped against what the port installs,
three names were missing that cost no source, no compile and no decision,
because Bell Labs' own install arms make them as hard links from binaries
already in the tree: `cmd/ed/Makefile` does `ln /bin/ed /bin/e`, and
`cmd/compress/Makefile` does `ln $D/compress $D/uncompress` and then
`ln $D/uncompress $D/zcat`. Verified the way the `pcat` link was — the shipped
copies are byte-identical with *different* inodes, because the tarball lost the
links on extraction.

`e` is the one worth the paragraph, because the Makefile had already reasoned
about it and reached a **correct conclusion about the wrong program**. Its
comment says `e` is an `ex` arm that was never run — and that is true:
`ex/makefile` links `e` under `BINDIR`, `BINDIR` is `/usr/bin`, and there is no
`/usr/bin/e` in the shipped tree. But `e` *is* shipped, in `/bin`, at 13312
bytes against `ex`'s 116736. It is `ed`'s link, from a different makefile
entirely. The project's own rule — when a program is absent from `cmd/`, check
whether it is a link before calling it sourceless — had been followed, with the
check aimed at the wrong parent. A correct answer to the wrong question is more
durable than a wrong one, because nothing about it invites re-checking.

The two link families needed different tests, and the difference is in the
source: `compress.c` strips the directory from `argv[0]` and compares it
against `"uncompress"` and `"zcat"`, so those links are load-bearing and an
inode comparison would not prove the link reached the program — each name gets
a behavioural case, including that `zcat` leaves the `.Z` in place. `ed` reads
`argv[0]` nowhere, so `e` is a pure alias and the inode *is* the claim.

163 to 166, with nothing compiled.

### And five more, where correctness became a property of $PATH

The other cheap remainder was five commands V8 shipped as **shell scripts**, so
there is nothing to compile and the installed file is the source: `true`,
`false`, `dirname`, `nohup`, `whois`. Their whole dependency set — `expr`,
`nice`, `test`, `grep`, `sh` — was already installed.

`true` is worth a sentence. It is **zero bytes**: not a program that exits 0,
but the original empty file, which is what an empty script does. That is
upstream's, byte for byte.

Where each one came from is recorded by where it sits. `false` has upstream
source at `usr/src/cmd/false.sh` and lands in `src/cmd` like every other
imported program; the other four have no `usr/src` copy at all, so what was
imported is the shipped artefact itself and the import tool maps it to
`src/bin`. That asymmetry is information rather than untidiness — it says at a
glance which four are files Bell Labs shipped without shipping a source for.
It is the `more`/`pg` situation with the opposite verdict: those are opaque
binaries and stay out, these are readable text, so installing upstream's own
bytes is strictly more faithful than writing a replacement.

Then the part that was new. Every command installed before these is a
self-contained binary whose behaviour is fixed at link time. **A script's
correctness is a property of the PATH it runs under**, and the first test run
proved it the hard way: invoked with a developer's host PATH, `whois root`
resolved `grep` to a Homebrew binary the rootfs does not have. The jail lets
that through by construction — a host binary is not jailed — so it grepped
**macOS's** `/etc/passwd` and printed a completely plausible wrong answer,
`System Administrator:/var/root` where the jail says `Superuser:/`. Nothing had
escaped, and `V8JAIL=warn` correctly reported no escape: a host binary was
asked, and host binaries see the host.

The independent tell was the error text. Both runs complained about the missing
`/usr/adm/usrlist`, but BSD grep says `No such file or directory` and V8's says
`can't open` — the wording is what proves a different binary ran.

So the cases run under the PATH `v8(1)` actually gives the world, and the
`whois` case asserts its answer against a line **derived from the jail's own
passwd** rather than a transcribed string: the host's root line is a property
of whoever's Mac this is, and the jail's is a property of the port.

166 to 171.

---

## A language system, and the sweep that could not have found its bug

The next batch is the language systems, and they were picked by size. Counting
source lines settles it and is not close: `ratfor` 1263, `lcomp` 1925, `efl`
10089, `f77` 16140 — plus another 5165 in `libF77` and `libI77`, so 21305 —
and `cfront` 22442.

`ratfor` is Kernighan's Rational Fortran preprocessor. It reads C-like control
flow and writes Fortran 66, and it has a consumer already installed, because
`struct(1)` is its inverse: `struct` reads Fortran and writes Ratfor. So the
pair round-trips, which is an end-to-end assertion that needs no `f77` at all:

```
      i = 0                       i = 0                    i = 0
10    i = i + 1      struct →     repeat      ratfor →     23000 continue
      if (i .lt. 10) goto 10        i = i+1                i = i+1
      call done(i)                  until(i>=10)           23001 if(.not.(i.ge.10))goto 23000
      end                        call done(i)              call done(i)
```

`struct` recognised the backward `goto` as a `repeat/until`; `ratfor` turned it
back into a labelled one. Two Bell Labs programs from the early eighties,
inverses of each other, both running on Apple Silicon.

**The port needed two words changed, in one file.** `r.h` declares `yyval` and
`yylval` `int`; this port's yacc emits `#define YYSTYPE long` and defines both
objects itself, so the declarations described four bytes of an eight-byte
object. Every other file — six `.c`, the grammar, the makefile — is
byte-identical to pristine V8.

The interesting part is not the fix. It is that **this project already
documents this exact bug class, prescribes a sweep for it, and the sweep could
not have found either instance.** Run over the tree with `ratfor` imported, its
only hit is the comment describing the *previous* instance. It is narrow three
ways at once: it globs `*.l` and `*.c`, and ratfor's grammar is `r.g` while
both declarations are in a **header**; it matches `yylval` only, and `yyval` is
the same lie about the same symbol one line away; and it matches its own
documentation, which is the shape where writing a finding down grows the
population being counted.

**The two lies cooperated, which is why fixing one would not have been
enough.** `yaccpar` contains `yyval = yylval`, so once a token carrying a
pointer has been read, `yyval`'s upper half holds that pointer's upper half —
and a later `yyval = genlab(3)` written through an `int` declaration replaces
only the lower half.

Measured separately, the two halves fail in completely different registers:

| reverted | result |
|---|---|
| `yylval` | **SIGSEGV** on the first line of input |
| `yyval` | **exit 0**, plausible Fortran, and a label printed as `4294990298` |

`4294990298` is `0x100005ADA` — label 23002 with a 1 sitting in bit 32. The
segfault announces itself. The other one would have shipped, which is why the
test for it asserts a *relation* — every label V8 generates fits in five digits
— rather than the statement text or the exit status, neither of which can see
it.

**And one of the three changes was reverted after being measured.**
`rlex.c`'s `yylval = (int) str` looks like a second truncation. It is not: with
`yylval` declared `long`, v8cc emits `adrp`/`add`/`str x10,[x9]` — a full
64-bit store with no narrowing instruction — for `(int)` and `(long)` alike,
and the two `.s` files are byte-identical at 19066 bytes. A change to authentic
source has to be *forced by the target*, and one that emits the same
instructions is not. So `rlex.c` stays as Bell Labs wrote it and the
measurement goes in the porting notes instead — the same call, settled the same
way, as `awk`'s `maketab.c`.

### The third bug in one test's parser

`ratfor`'s install line is `cp a.out /usr/bin/ratfor`, and that shape broke
something. A test compares each imported makefile's stated destination against
Bell Labs' own `Admin/dest` tables, and its parser required the program's name
to appear among the **sources** of the `cp`. Here the source is `a.out`.

That was not a bug `ratfor` introduced. **Nine of the imported makefiles were
already unreadable to it** — six with `a.out` targets, two installing a shell
script under a different name, one installing a variable — a fifth of the
population, silently reported as stating no destination at all. One program,
`grap`, escaped only because an earlier line in its makefile happened to spell
its own name.

The guard against exactly this was in place and passed throughout: the sweep
must find at least ten makefiles stating a destination. It did, because the
readable ones kept the count up. **A threshold bounds how wrong a sweep can be
and never how incomplete it is.** It is a derived relation now — every makefile
installing under its directory's own name must resolve — plus cases aimed at
the two halves of the fix.

Fixing it immediately produced a false positive of its own, which is the same
shape as the bug it sits beside: `cp structure beautify /usr/lib/struct`
installs two programs *into a directory named* `struct`, and a basename test
read that as `struct` installing to `/usr/lib`. The discriminator is `cp`
semantics rather than a heuristic — with more than one source the destination
must be a directory — and it surfaced a genuine second reading in `ex`, whose
makefile states two destinations, BSD's `/usr/new` staging directory first and
the real `/usr/bin` second.

**And ratfor reproduces a bug reported in 1981.** Its `BUGS` file is a mail
message from Rick Becker to Brian Kernighan, dated 4 March 1981: an unclosed
`for(` sends ratfor into a loop, and it shipped unfixed. Fed the same four
lines, this build hangs — exit 142, zero bytes of output, forty-five years
later on hardware that did not exist. There is a test asserting it still does,
because repairing it would have to be a decision rather than a tidy-up.

171 to 172.

### efl, where the program was innocent and the compiler was not

The next one by size is `lcomp` at 1925 lines; the owner picked `efl` instead,
at 10089, and it turned out to be the right call for a reason nobody could have
predicted. Feldman's Extended Fortran Language gives Fortran 66 structured
control flow, C-like data structures and generic procedures. Twelve thousand
lines over twenty-five objects — the largest thing in this port after the C++
front end.

**Every one of its thirty-four files is byte-identical to pristine V8.** The
port needed no source change at all. What it needed was a fix to our compiler.

That the LP64 class was absent is down to a single line. `defs` opens

```c
typedef int *ptr;
```

which is a generic pointer type that is *already pointer-sized*. Compare
`struct(1)` one batch earlier, whose `#define VERT int` stored pointers in an
`int` and cascaded into two headers. Every node-returning function in efl is
declared `ptr` and the allocators are declared in `defs`, so the rootfs-wide
truncation sweep found exactly one narrowed call across twelve thousand lines —
`conval`, the function whose entire job is to extract an integer. A program can
be enormous and have almost no LP64 surface, and one typedef decides which.

It still crashed on `procedure main`. The address was **0x80**, and that
address was the whole diagnosis.

`misc.c` contains `p->vproc = q->vproc = v` — a chained assignment to two bit
fields. `vproc` is a 2-bit field at bit 6, and the value being stored was 2, so
`2 << 6` is 128 is 0x80. The compiler was storing the spliced field value *as
an address*:

```asm
ldr  w12, [x10]          ; first insert -- value in its own register
bfi  x12, x11, #6, #2
str  w12, [x10]

ldr  w10, [x10]          ; second -- value loaded INTO the address register
bfi  x10, x11, #6, #2    ; x10 is now 0x80; the address is gone
str  w10, [x10]
```

Underneath was a static scratch pool released one scope too early. The back end
describes a bit field's containing word in a slot of `contbuf[]`, and freed the
slot when the function returned rather than when the lvalue died. An lvalue
stays live across the evaluation of the right-hand side, and here the
right-hand side was *another bit-field assignment*, so both got slot zero. Two
symptoms, one cause: the outer store went to the wrong object — the correct
address was computed into `x9` and never used — and it went there through a
register the inner cleanup had already returned to the pool, so the allocator
handed that same register straight back.

The author's own error message names the misconception. It reads `"bit fields
nested too deeply"`, and C has no bit field inside a bit field: the recursion is
always exactly one deep. The quantity that needed bounding was never nesting
but *bit-field lvalues live at once*, and `a.f = b.f = v` has two. One hundred
and seventy-two programs had never produced that shape.

The build is the other reason efl was worth taking. It has three generated
inputs made by three different V8 programs, and one of them is a text editor.
`fixuplex` is an `ed` script that patches the scanner lex just generated:
one substitution to route character input through efl's own pushback macro, and
a twenty-seven-line append of global-flag checks the lex skeleton has no way to
express. `ed` is a compiler pass. It works unchanged, V8's `ed` on V8's `lex`'s
output, 1455 lines in and 1482 out — and it is not idempotent, because V7 `ed`
prints `?` on a failed substitute and keeps reading commands, so a second run
would append the block twice and exit zero.

The third input is the one that is *not* generated. `gram.c` is checked in, and
upstream's yacc rule is commented out with the reason attached: *"gram.c can no
longer be made on a pdp11 because of yacc limits."* So the eighty-three token
numbers baked into the parser and the eighty-three the build derives from the
`tokens` file are two hand-maintained copies of one list, with no step left
that would notice them disagreeing — and token numbers are *line numbers*, so
inserting one line renumbers everything below it and the lexer starts returning
numbers the parser reads as different tokens. Both halves still compile. This
is the shape that let two errno tables agree perfectly about a set missing seven
names, so the test compares them as sets in both directions: two lists of
eighty-three can differ and still both be eighty-three.

And it works. Given a `struct point {real x, y; integer tag}` and an array of
eight, efl emits

```fortran
      integer p(3, 8), i
      real p1(3, 8)
      equivalence (p(1,1), p1(1,1))
```

— a two-dimensional array with an `EQUIVALENCE` aliasing the same words at a
second type, because there is no other way to say "structure" in Fortran 66.

172 to 173.

---

## Fortran, and a library that never linked

`f77` was the next language system, and the first thing the survey found was
that f77 is not one program. It is four.

The driver at `/usr/bin/f77` is an `exec` pipeline, the same shape as `cc`.
`/usr/lib/f77pass1` is the compiler front end. And then there is a line in a
twenty-line file called `drivedefs`:

```c
#define PASS2NAME	"/lib/f1"
```

`f77pass1` does not emit assembly. It emits **pcc intermediate code**, and
`/lib/f1` is a separate compiler that turns it into assembly. Its source is not
under `cmd/f77` at all; it is `cmd/pcc1`, built with `-DFORT`, and the install
arm of its makefile is `mv fort ${DESTDIR}/lib/f1`.

The obvious move was to point it at the ARM64 back end this port already has.
`compiler/ccom-arm64/` is a pcc-family code generator written inside pcc's own
architecture — `local2.c`, `macdefs.h`, the same file names and hooks. It has
been compiling the whole tree for a year.

It cannot be done, and the two `mfile2` files say why in their first forty
lines. pcc1 matches on **shapes and cookies** — `SAREG`, `SOREG`, `SNAME`,
`OPSIMP`, `INTAREG`, nine of them. pcc2, which is V8's ccom and therefore ours,
matches on **types** — `TCHAR`, `TSHORT`, `TSTRUCT`, three cookies, an entirely
different `optab`. Same filenames, same ancestry, different compilers.

f77's own `README` says it in one line, and I had read that line an hour
earlier without hearing it:

> f77 is a pcc1 compiler. c is a pcc2 compiler these days.

I had filed it as a remark about the debugger symbol format, because that is
what the paragraph around it is about. It is a remark about the whole back end.
So `/lib/f1` is a second machine-dependent code generator, roughly 2,500 lines
of new ARM64, and f77 became a four-stage job instead of a step.

### The stage that was portable

The two runtime libraries — `libF77` and `libI77`, 154 files and 5,607 lines —
have no assembly and four sites in the whole set that mention a machine name.
They are the only part buildable with the tools that exist today.

They are also, it turns out, the part that never worked.

`libI77` is a System V library that Bell Labs dropped into V8 and never
reconciled. Two of the things it calls are System V libc internals: `setvbuf`,
and a table called `_bufendtab` that holds the end of each stream's buffer. I
searched for them across every `.a` in the distribution:

| symbol | found in |
|---|---|
| `_sobuf` | `lib/libc.a`, `usr/lib/11libc.a` — resolves |
| `setvbuf` | `usr/lib/libI77.a` **and nowhere else** |
| `_bufendtab` | `usr/lib/libI77.a` **and nowhere else** |

Two undefined symbols. **No Fortran program on a real V8 could reach the end of
`ld`.** And the library knew: four lines above the first `setvbuf` call there is
a comment reading *"IOLBUF and setvbuf only in system 5+"*, sitting directly
above the unguarded call it describes.

Reproducing that faithfully would mean shipping something unusable. So the port
supplies the two missing pieces in eighty lines of layer-2 code, archived
**into `libI77.a`** — not into libc, because putting `setvbuf` in libc would
invent a C library V8 never shipped, and because the driver's library list is a
fixed four names with no room for a fifth.

`_bufendtab` needed no table. Every stdio buffer in V8 is exactly `BUFSIZ`
bytes — `flsbuf.c:25` is the literal `base+BUFSIZ` — so the answer is
computable from the stream, and being computed it cannot go stale the way a
table can.

### The header that had to be kept off the include path

`libI77` also ships its own `stdio.h`. It is System V's, and it disagrees with
V8's about where the fields are:

| | V8 | libI77 |
|---|---|---|
| `_flag` | `short` | `char` |
| offset of `_file` | 26 | **25** |
| `_NFILE` | 120 | 128 |
| `_IOLBF` | 0200 | 0100 — which is V8's `_IOSTRG` |

One byte. `fileno()` would read the high half of `_flag`, and `libI77.a` and
`libv8c.a` would disagree about `FILE` in a program that links both. Upstream's
makefile says `CFLAGS = -I. -g`, so it *did* compile against that header, and
the shipped archive proves it by carrying `_bufendtab`.

The interesting part is that the `-I` turns out not to be needed. `fio.h`,
`fmt.h` and `lio.h` are quoted includes and resolve by the includer's own
directory. Exactly one file wants anything else — `ecvt.c`, which needs
`<nan.h>` and `<values.h>` — and `ecvt.c` is also the only file in the library
that mentions `FILE`, `printf`, `getc`, `stdout`, `stderr` and `stdin` exactly
zero times. So it is the one file that can safely be handed a flag.

It gets a staging directory holding **one header**, and `<values.h>` falls
through to ours. Which mattered, because ours had to be written.

### A floating-point format with no arm for this machine

`values.h` describes a floating-point format and picks one from a macro the
compiler predefines. Upstream has three arms: `u3b`, `vax`, `gcos`. This port's
`cc` predefines `unix` and `arm64`. So none of them is taken, `_DEXPLEN` comes
out undefined, and the compile stops — loudly, which is the good direction.

Adding an IEEE arm is four constants. What was not four constants is the line
underneath it, which upstream writes unconditionally for every machine it knows:

```c
#define DMAXPOWTWO	((double)(1L << BITS(long) - 2) * \
				(1L << DSIGNIF - BITS(long) + 1))
```

That needs the mantissa to be **wider than a long**. On a VAX, `DSIGNIF` is 56
and `BITS(long)` is 32, so the second shift is 25. Under LP64 the mantissa is
53 bits and a long is 64, so the shift is **minus ten** — undefined behaviour,
evaluated at run time. The quantity meant is `2^DSIGNIF`, which is directly
expressible here *precisely because* the mantissa is now the smaller of the two.

### Proving it, with no compiler to prove it with

There is no Fortran compiler in this tree, so the runtime has no caller — and a
component with no consumer is one this project has learned not to trust. The
answer is a probe: a C program that supplies `MAIN__` and lets `libF77`'s own
`main()` call it, which is exactly the shape of a Fortran program.

Most of it checks intrinsics — that `s_cmp("ab", "ab  ")` is zero because
Fortran blank-pads, that `MOD(-7,3)` is `-1` because Fortran keeps the sign of
the dividend, that `NINT` rounds half away from zero. One check found a bug in
the probe rather than the library: `i_nint` takes a `REAL` and `d_nint` takes a
`DOUBLE PRECISION`, and the prefix letter is the only thing that says so, so
handing `i_nint` the address of a double reads its low four bytes as a float —
which for 2.5 are zero.

But the load-bearing part is one two-letter token. The format is `(A2,T1,A1)`:
write `AB`, then `T1` to move the cursor **back** to column one, then `X`. The
answer is `XB`.

It has to be backward. `_bufend`'s three call sites are all inside the same
question — *can I skip inside the buffer, or must I seek?* — and a purely
forward write never reaches any of them. A probe that just printed a line would
have proven nothing at all about the one thing this build invents.

### And the guard that turned out not to be one

The shim's `_bufend` returns `_base + BUFSIZ`, except when there is no buffer,
where it returns `_base` so the comparison fails and the caller seeks instead.
I wrote a paragraph explaining that the unguarded version would let an
unbuffered stream advance its pointer off NULL into low memory.

Then I removed the guard, and every test stayed green.

The first half of the claim was true and the consequence was not. With no
buffer, every character goes through the unbuffered arm of `_flsbuf`, which
writes it directly and never touches the pointer. Measured with a probe that
unbuffers stdout first — which is what the library itself does on a terminal —
both spellings print the same bytes.

The arm stays, because advancing a pointer off NULL is undefined whether or not
anything reads it. But the comment now says it is defensive rather than
load-bearing, and **no test is written for it**, because there is nothing
observable to assert. A case with no difference to detect is the kind this tree
keeps finding by accident; writing one on purpose would be worse.

That is what the non-firing mutation is for. A green run can never tell you a
guard is empty.

173 to 173 — no new commands, because a library is not a command. What moved is
that `-lF77 -lI77` now resolves, which is something V8 itself could not say.

### The driver, and a number pinned from an unexpected direction

Stage 2 is `/usr/bin/f77` itself, and the first surprise is that it is not only
a driver. Upstream links it from two objects — `driver.o vaxx.o` — because from
line 1159 `driver.c` holds the **DATA statement emitter**: it sorts the
intermediate file the compiler wrote, walks it, and lays initialised variables
out as assembler directives. So the driver carries real machine-dependent
output, and `vaxx.c` and `pdp11x.c` are each one machine's four directive
printers. Ours is `arm64x.c`, and three of its four functions are what the VAX
did verbatim. Only one differs, because clang's assembler reads `0101` as
decimal 101 where the VAX read octal — a plausible wrong answer rather than an
error.

Then the machine description had to be written, and it produced the most
consequential number in the port from a direction I was not looking.

LP64 says `long` is 64 bits, so the instinct is that Fortran's default INTEGER
should follow. It cannot, and the reason is nothing to do with integers.
`typesize[]` — which exists twice, identically, in the driver and in the
compiler — is

```c
{ 1, SZADDR, SZSHORT, SZLONG, SZLONG, 2*SZLONG,
  2*SZLONG, 4*SZLONG, SZLONG, 1, 1, 1 }
```

indexed by type. The fourth entry is INTEGER and the **fifth is REAL**, and they
are the same expression. libF77's `r_nint` takes a `float *` and `d_nint` takes
a `double *`, so a Fortran REAL is four bytes and a DOUBLE PRECISION is eight —
which forces `SZLONG` to 4, and with it INTEGER, LOGICAL, and the hidden length
that rides along behind every character argument. f77 has exactly two integer
types and no way to give the integer and the float different sizes.

And that collides with the library I had just finished. `libI77`'s `fio.h`
spells the hidden length `typedef long ftnlen`, and 37 libF77 files spell
Fortran INTEGER as `long` too. Under this compiler that is eight bytes. On a
VAX it was four — V8's own compiler says so in one line, `# define NOLONG`,
*map longs to ints* — so the sources are correct for the compiler they were
written for and wrong for this one.

Stage 1's write-up had predicted exactly this, in a sentence ending *"the first
thing to check in stage 3"*. It was worth writing down: the answer is that 38
files of authentic source need reconciling, and the cost is visible enough that
it is the owner's call rather than mine. Stage 2 does not touch it — the driver
never calls the runtime — so it is costed, recorded, and left standing.

### `HERE` meant two different things

The driver has six machine conditionals, and porting it turned on noticing that
they are not all the same kind of question. Three ask *"is this a VAX"* — the
one-input-file assembler workaround, the PDP-11 cross-compile cleanup, the
Interdata optimiser. Three ask *"is this a Unix"*, and spell it as a list of the
three Unixes V8 knew:

```c
#if HERE==PDP11 || HERE==INTERDATA || HERE==VAX
	if( (waitpid = fork()) == 0)
```

That one guards the entire fork-and-exec of the link editor. Without this
machine's name added to it, the driver builds, runs, reports no error, and never
invokes the linker at all.

There was a shortcut available: compile with `-DHERE=VAX -DTARGET=VAX` and every
arm comes out right. I checked all six; it genuinely does. It is still a lie —
`HERE` means the machine the compiler runs on — and the cost of the lie is that
the next person reads `-DTARGET=VAX` in a recipe and believes it. Naming the
machine is one line in a file that already numbers seven of them.

### Correcting a flag's spelling was not the fix

The link then failed on the loader flags, and the sequence is worth keeping
because the middle step looked like success.

Upstream passes `-X` to `ld`, meaning *discard local symbols*. ld64 answers
`warning: -X is obsolete` on every link. Mach-O's spelling is `-x`, so I changed
one character — and the link died with

```
clang: error: language not recognized: '-u'
```

which names neither the flag I changed nor anything obviously related. `-x` is
*clang's* option for specifying a source language, and it had eaten the next
argument as its operand. The flag was never reaching `ld` at all: this port's
assembler and linker are the host's, reached through clang, so every loader flag
needs `-Wl,`.

**Read which program parses a flag before correcting its spelling.** Three
drafts of three lines, and only the third was about the right program.

With that, `f77 prog.o -o prog` links an object against `libF77` and `libI77`
and the result runs — a Fortran-shaped program, built by V8's own driver, on
hardware V8 never saw. What is missing is the compiler in the middle: stages 3
and 4, one of which is a second code generator.

173 to 174.

### And then the width change found a compiler bug

The INTEGER-width problem looked like a judgment call with a visible cost, so I
wrote it up as one and moved on. Then I went back to check whether `SZLONG`
really could not be 8, and found it is pinned **twice**:

```c
case TYLONG:
	if(length == 0)  return(tyint);
	if(length == 2)  return(TYSHORT);
	if(length == 4)  goto ret;      /* INTEGER*4 -> TYLONG */
```

The literal `4`. `lengtype()` hardcodes the length constants, so
`typesize[TYLONG]` must be four bytes whatever the float layout said. There is no
value of `SZLONG` other than 4 that satisfies both, which means it was never a
decision — the runtime's `long` had to narrow, and V8's own compiler agrees it
always meant 32 bits.

So 39 files in `libF77` and four sites in `libI77` changed, one token each, and
the byte-identical count dropped from 153 of 154 to 112. Worth stating plainly:
that is the cost, and the `PROVENANCE` hashes keep the diff reconstructible,
which is what the mechanism is for.

The half that needed judgment was what *not* to change. Thirteen `libI77` files
use `long` for a file offset, and those must stay 64-bit; the `long x` locals
that widen a value for `icvt` are correct as they are. A blanket `-Dlong=int`
would have broken every one. Which end supplies the operand decides the width —
the same question as the permission bug three sections up, in a typedef.

And then the first run of the probe said:

```
f77probe: i_nint -2.5: want -3 got -3
```

Both print as −3 and they are not equal. That is this port's signature failure,
and it means the comparison is happening at 64 bits while the print reads 32.
Ten lines reproduced it, and the emitted code named the cause:

```
	fcvtzs	w9, d16
	mov	x0, x9
```

`fcvtzs Wd, Dn` writes a **w** register, and any write to a w register zeroes
bits 63:32. This back end keeps a signed `int` sign-extended, and every
arithmetic site restores that with an `sxtw` — the fix for a bug found in a dump
tape's checksum two years of this project ago. The *conversion* had none. So a
negative float converted to an integer came back as `0x00000000fffffffd`.

It is a sixth site for that fix, and the only one that is a **conversion** rather
than an operator — which is exactly why the sweep that found the unary `-` and
`~` could not reach it: that sweep switches on an operator, and a conversion is
not one. And nothing had ever observed it, because `libF77`'s intrinsics returned
`long` until that afternoon. There was no high half to get wrong.

**A width change is a way of asking a compiler questions it has never been
asked.** That is an argument for making one on purpose rather than avoiding it.

Three of the four tests I wrote for the fix were vacuous, and the mutation said
so: they used an automatic, which is stored back through `str w` and re-narrowed
by the store. Only `register` keeps the value in a register long enough to be
wrong — which is why the four operator cases sitting immediately above them all
say `register`, a detail I had read past twice.


---

## Stage 5: finishing the compiler, and what a refusal cannot see

`/lib/f1` shipped implementing nineteen operators and **refusing everything else
by name**. That was a deliberate design: a code generator that silently skips an
operator emits a program that links and computes the wrong answer, so the
`default:` arm errors out with the opcode in it. I described that as the thing
keeping the gap safe.

Four probes measured it. Arrays refused, naming `operator 64` — a left shift,
which is how a subscript is scaled. An `INTEGER FUNCTION` refused. And then the
other two:

| a subroutine `CALL` | compiled, linked, **SIGSEGV** |
| `REAL` arithmetic   | compiled, linked, **hung**    |

**A guard on the vocabulary is not a guard on the grammar.** The refusal can only
fire on an operator the pass does not *recognise*. Those two programs used only
operators it did recognise, and it mishandled them — the exact failure mode the
refusal existed to prevent, arriving through the door it does not watch.

One of the refusals was worse than useless. `f(n)` in an expression was rejected
as *"indirect call not implemented"*. There is no indirect call in that program.
The callee was being read from the wrong stack slot, because a call in an
expression pushes its result temporary first. **A wrong answer in a diagnostic is
worse than none** — it sends the next reader after a missing feature instead of a
bug.

### The comment that described a contract nobody implemented

The subroutine crash was two files agreeing with each other about nothing.
`arm64.c`'s header said — twice, in two separate entries — that the argument copy
was "pass 2's business" because "this port's own prologue spills x0-x7". No
prologue spilled anything. `mvarg()` was an empty function, and the claim was
duplicated in the one file that could have refuted it, which is why neither copy
read as wrong.

Underneath it was a second fault that only a particular *shape* of program can
show: a parameter and a temporary at the same address. Upstream keeps two frame
registers, `ap` and `fp`; I had set both to x29 with no bias between them. It
takes a procedure holding **both** a parameter and a compiler temporary to make
the two collide, and nothing had had both. The fix is one constant — and it needs
no second register, because the machine difference *removes* the requirement:
the VAX's `ap` points into the caller's frame and never copies arguments at all,
while arm64 must spill its argument registers regardless. Once both regions are
in one frame, arithmetic separates them.

### Correct code on a VAX

The best bug of the session was not in the compiler. `write(6,*) 0.375` printed
`3.750000000e+00`. So did `37.5`. So did `375.0`. But `0.0375` printed perfectly,
and so did `1e10` and `1e-10`.

`wrtfmt.c` picks the value by length and then asks `if (p->pf != 0)` — the
**float** arm of a union holding a double. On a VAX that is exact: D_floating's
leading 32 bits have the identical layout to F_floating, so a double read as a
float is the same value, nonzero exactly when the double is nonzero. On IEEE
little-endian those four bytes are the **low mantissa bits**, which are zero for
any round number.

So it failed for tidy values and worked for untidy ones. That is the worst shape
a numeric defect can have: the values anyone would write a test with are exactly
the ones it breaks. It is Bell Labs' own line, it had been there since the
runtime libraries landed, and nothing had noticed because nothing had ever
printed a `REAL` through this port.

The guard is two cases, and the untidy one is the control rather than a
duplicate: a "fix" that simply always applied the scale would pass `0.375` and
break `0.0375`. Reverting the line fires the first and leaves the second green.

### Reading the caller instead of inventing it

A computed `GOTO` crashed the compiler's own front end. `prcmgoto` had a
signature I had made up, because `vax.c` does not define it at all — the VAX
takes a different branch entirely, so there was no neighbour to copy. `pdp11.c`
is the only other implementation in the tree and has it written out: the fourth
argument is an `int` label for a table the caller has **already emitted**. Mine
took an array of pointers and dereferenced the label number. Pass 1 died at
address `0x13`, which is 19, which was the label.

The macOS crash report named the three frames in one command. Reading the
caller's own argument list would have cost none.

And when that was fixed, the link failed instead — a jump table is a table of
addresses, and arm64 Mach-O forbids a relocation in `__TEXT`. `f77` reported
success over the top of it, because upstream's `doload` ends with a bare
`await(waitpid)` and discards the status. The observable was a program that
compiled, said nothing, and did not exist.

Fortran now does arrays, subroutines, functions, recursion, `REAL` and
`DOUBLE PRECISION` arithmetic, mixed-type expressions, `.AND.`/`.OR.`, `MOD`,
and computed `GOTO`s. Eight defects, in four different components, and only two
of them were in the code I set out to change.

---

## The ninth argument, and the silence a refusal was hiding

Four limits were left, each refusing by name. The first was the ninth argument:
arm64 passes eight in registers and the rest on the stack, and `/lib/f1` would
not emit a call that wide. A subroutine with nine parameters is not exotic
Fortran, so this was the one worth closing.

The interesting part was not the fix. It was what the refusal had been hiding.

f77's front end addresses parameter *n* at a fixed stride from the frame
pointer, for every *n*. The prologue spilled the eight register arguments and
stopped. So a nine-parameter subroutine addressed its ninth parameter at an
offset nothing had written — and that offset, measured, was exactly the one the
epilogue unwinds the frame with. It was the slot holding a saved
floating-point register. The subroutine loaded `d14` as a pointer and stored
through it.

That compiled without a warning, and no test could reach it, because nothing in
the world could emit a call with nine arguments. A guard on the caller is not a
guard on the callee. The refusal at one end was the only reason the other end's
silence had never been observed — and closing the first is what made the second
observable at all.

Then raising the bound made a second one bind. The value stack in `/lib/f1` was
`64`, and the new argument limit was `64` — the same number by coincidence, not
by relation. A call needs one slot per argument plus one for the callee, so 64
arguments needed 65 and the compiler refused with `expression stack overflow`
for a program whose expressions are all single terms. The diagnostic named the
wrong resource because the two numbers had never been connected. They are now:
one is written in terms of the other, so they move together.

And a case I wrote to guard the fix turned out to guard something else. I had
added a structural check — no parameter may be addressed above the offset the
epilogue unwinds from — and explained it by saying the value cases could not
see the defect, since a wild pointer crashes rather than computing a wrong
number. Mutation testing disagreed: deleting the argument copy fired all three
value cases and left the structural one green. The two check different things.
The values check that the arguments are *placed*; the structure checks that the
frame is *big enough to hold them*. Both are needed, and my explanation had
credited one with the other's job. The comment now says what was measured.

The second limit went the same way — by noticing that the refusal contained an
assumption nobody had examined.

Fortran's `ASSIGN 20 TO lbl` puts a code address into an integer variable, and
then `GOTO lbl` branches to it. On a VAX an address and an `INTEGER` were both
four bytes, so this cost nothing and needed no mechanism. Here `INTEGER` is
still four bytes — it has to be, for reasons pinned three ways — and macOS loads
program text above the 4GB line. The address does not fit. I had written the
refusal myself, and it is accurate: measured, storing one drops the `1` in bit
32 of `0x10001f140`.

What it assumed is that the four bytes had to hold *the address*. They only have
to hold something the branch can turn back into one. So the compiler now emits a
label at the head of each procedure, stores the **distance** from it, and adds
it back at the branch. Both ends compute that distance at run time from two
address-forming instruction pairs, which means it does not depend on the two
labels living in the same section — and they might not, because the front end
interleaves its constant pool with the code the second pass produces.

There is a tidier-looking version where the assembler computes the difference,
and it is the one that would have broken. There is also a heavier version — an
index into a table of addresses, which is what the *computed* `GOTO` already
does — and it needs per-procedure bookkeeping and a second relocatable table.
The subtraction needs neither.

Then two of my own test instruments were wrong before any of the code was, and
both failed by printing nothing. One case called a shell function defined later
in the same file, so the call expanded to the empty string. The other counted
into an awk variable named `sub` — awk's own substitution function — so the
program was a syntax error and awk printed nothing. Empty output reads exactly
like a compiler that produced nothing. What separated *my instrument is broken*
from *the compiler is broken* was running the program by hand and watching it
print the right answer.

The third limit was a refusal about registers that had nothing to do with
registers. A character concatenation of five pieces was rejected for want of an
eleventh register, and the fix added none.

Fortran builds `a//b//c` by writing a length and a pointer for each piece into
two arrays and calling one library routine over them — so an *n*-way
concatenation is one statement containing 2*n* assignments. The compiler's
second pass returned, as the value of each assignment, the register the value
had passed through, and nothing releases that until the statement ends. So it
held two registers per operand and used none of them. An assignment's value can
just as well be *the thing assigned to*: read back, it is the same number, and
it holds no register. Measured, distinct registers used: 1, 6, 8, 10, refused —
and afterwards a flat 2 at every width, out to eight pieces and beyond.

The fourth limit — a second `ENTRY` point — turned out to be two mechanisms, and
only one of them was the one the refusal named.

`ENTRY` lets one Fortran procedure have several names with different argument
lists. The compiler numbers the parameters as a union across all the entries, so
a later entry's first argument may belong anywhere in the frame — and if two
entries name the same parameters in a different order, the mapping is a
permutation. The VAX version copies arguments through a staging area, which it
must, because on a VAX they were never in registers. Here they still are when
the prologue runs, and a register is not one of the destinations, so the
prologue can simply store each one where that entry wants it. The extra
mechanism was solving a problem this machine does not have.

The other half was a line that was simply absent. Entries can differ in *type*,
so each type gets its own epilogue and every `RETURN` jumps to a shared exit
that branches indirectly to the right one — through a variable the entry is
supposed to fill in. The VAX file fills it in; this one never had that line.
With a single entry the exit is not indirect and the variable is unused, so
nothing had ever reached it. An integer function with an entry compiled without
a word, printed nothing, and exited 0.

And that indirect exit is the assigned `GOTO` from the previous limit, arriving
with a second producer one refusal later. My branch added the base offset
unconditionally, which is right when the variable holds a distance and wrong
when it holds a whole address. The two mutations are exact complements: forcing
it on breaks the entry case alone, forcing it off breaks the assigned-`GOTO`
cases alone.

Three of the test programs were mine again, and all three were implicit typing —
`entry g(...)` inside an integer function is a *real* function, because `g` is
not in `I` through `N`. The standing rate is about three per corpus.

---

## The thing this project keeps teaching

Almost nothing here failed loudly.

The compiler bug printed fifteen correct hex digits and dropped the sixteenth.
The truncation bug computed a checksum, printed it correctly, and took the
not-equal branch. The signal bug returned success and hung an hour later. The
group-database escape printed a perfectly plausible list of group names — from
the wrong machine. The buffer overrun corrupted output several rows after the
row that caused it. `pwd` printed another directory's path and exited 0. `mkfs`
built with a missing flag wrote a wrong filesystem that all three checkers
pronounced clean. Five missing libc functions gave *right answers* for months,
from Apple's implementations, in a project whose entire premise is that the code
is Bell Labs'.

So the discipline that matters is not care while writing. It is building things
that fail loudly on your behalf: mutation-tested guards, provenance hashes,
`nm -u` sweeps over the whole world rather than one sample, hooks that refuse
the plausible mistake, and a habit of measuring the actual path rather than a
convenient proxy for it.

**And then not trusting those either.** The instruments were wrong more often
than the code was. A crash probe reported four different numbers before it
reported the true one, each time authoritatively. A sweep counted the
*documentation* of a rule as an instance of the rule, three separate times. A
"verify the object rebuilt" check watched an object the suite does not link, and
reported three real mutations as meaningless. And the most expensive one was a
sentence: a recorded constraint saying a fix was impossible, whose every clause
was false — citing a file that says the opposite, about a field that does not
exist, in a struct with no such member. It stood for months. **A wrong cause
eventually trips a test; a wrong constraint never does, because the code it
forbids was never written.**

The habit that comes out of that is narrower than "be rigorous". It is: when a
note tells you something is impossible, go and read the thing it cites. And when
you write a second instance of anything — a second function beside an existing
one, a second filesystem type, a second checker — expect it to expose that the
first one's stated reasons were wrong. It did here, every time.

### Stage 6: what twenty ordinary programs found

Stage 5 ended with a claim I had written carefully and still got wrong: that
`/lib/f1` refuses by name anything it cannot do, so an unfinished pass is a safe
one. A guard on the vocabulary is not a guard on the grammar, and I had already
recorded that once. Twenty ordinary Fortran programs — character variables,
FORMAT, DATA, COMMON, EQUIVALENCE, SAVE, arithmetic IF, internal I/O — found
eight more defects, and only one of them announced itself.

The worst was silent. `flushcc()` spills a pending comparison into a register,
and its comment said it runs at "the only thing that writes flags". That was a
true and complete survey of the code that existed when it was written; a `bl`
writes the flags too, and calls arrived later. So `if (i .lt. 3 .and. fn(i) .gt.
1)` compared, called, and then ran `cset` on flags the callee had overwritten —
and `.FALSE. .AND. .TRUE.` came out **TRUE**. Nothing crashed. The value tests
for it are not the guard, because they depend on what the callee happens to
leave behind; the guard is structural, that between a `cmp` and the `bl` that
destroys it there must be a `cset`, which is true at every callee.

Two more were the same fact wearing different clothes. `abs` — the commonest
intrinsic in the language — was refused as "operator 22 (COLON)", because f77
expands it as `0 <= t ? t : -t` and Fortran's conditional lives in the compiler
rather than in the language. And `x ** 0.5` printed `3.01e+23` for the square
root of two, because K&R C has no float return: every one of the 66 typed
functions in the Fortran runtime returns `double`, f77's own table calls those
results `real`, and on a VAX that disagreement cost nothing — D_floating's
leading word has F_floating's exact layout, so reading the first half of a
returned double as a float is the same number. On IEEE it is the low mantissa.
That is the second time this port has been bitten by that one coincidence, in a
second component.

My favourite is the one that took two ports of a single fact to reach. `assign
20 to lbl`, with no GOTO at all, crashed the front end at address 1. `putop`
walks a chain of type conversions and refreshes its operand pointer before
re-checking what it is looking at, so at a leaf it read a pointer field off the
end of a constant block — on a VAX, a garbage byte from the next heap object,
into a variable the loop then stopped using. Here the block is eight bytes
longer, the field is real and zero, and the read faults. And it was reachable
only because `SZADDR` stopped equalling `SZLONG`: assigning a label to an
INTEGER skips its conversion when the destination is no narrower, which was `4
>= 4` on a VAX and is `4 >= 8` here, so the node that trips the walk had never
been built. Two consequences of one width change, meeting in a function neither
of them is about.

The register allocator turned out not to need a bigger pool so much as an
honest reading of the record it was already given: pass 1 announces how many
registers it took, that number is zero in every program measured, and four
callee-saved registers had been sitting idle in every procedure the pass had
ever compiled while it refused `a // b` for want of a sixth.


And the twenty were not exhaustive — they were where I stopped looking. Ten
more programs found four further defects, three of which are the same width
change arriving in a *layout* rather than in a value: an argument list that
packs by size where the machine slots by register, a data block whose alignment
is taken from its first member's type, and a control block whose fields f77 and
its own runtime disagree about. The fourth was `EXTERNAL` — passing a procedure
as an argument, which Fortran has had since 1966 — refused by a diagnostic that
described it accurately as "a call through a value rather than a name".

The one I keep thinking about is the deferral. `io.c` had a note saying the
OPEN, CLOSE and INQUIRE control blocks still had the alignment defect, that
fixing them unexercised would be a claim nothing could check, and that the first
program to OPEN a file would refuse to link. Every clause of that was written in
good faith and the last one was false: a READ/WRITE block is initialised data,
so its pointers are relocations the linker checks, but an OPEN block is filled
in at run time, so there is nothing to check and the program links cleanly and
crashes. An expiry condition that names the wrong instrument is not a tripwire —
it is a note that will never come due.


And then a review of the finished thing found the largest gap of the lot, in a
sentence rather than in code. `prolog()` — the entry sequence — carried a
comment saying that the VAX walked its argument list here and that this target
did not need to, because the register spill had moved elsewhere. That was true.
What it missed is that the VAX walked that list for *two* unrelated reasons:
copying arguments into the frame, and evaluating each dummy array's run-time
dimensions. The first had indeed moved. The second had never existed here at
all. `proc.c` allocates a temporary per adjustable dimension and leaves the
machine-dependent file to store the expression into it; ours stored nothing, so
the temporary was read and never written, and `integer a(m,n)` — the standard
way to pass a matrix in Fortran 77, the shape all of LINPACK is written in —
compiled without a diagnostic and died. A one-dimensional adjustable array
works fine, because nothing multiplies by the first extent, which is why a
thirty-program corpus walked past it. The fix is eight lines transcribed from
`pdp11.c`, which had them all along, twelve lines above the two statements this
file *did* transcribe.

The variable lower bound is the worse half of that one. A missing extent faults;
a missing base offset does not. It gives exit 0 and a plausible wrong number,
and it is right by coincidence for some bounds — so only a case that names the
values can see it at all.

The same review found a silent wrong answer in the multiple-`ENTRY` work
finished the day before. Deciding which hidden result slots an entry gets, the
new code asked the *procedure's* type where the allocator had asked each
*entry's* — the same question, one word apart, indistinguishable in every
program with one entry and in every program whose entries agree. A `REAL`
function with a `COMPLEX` entry crossed two operands and printed a pair of
denormals, exit 0. What had been catching it was the refusal deleted to make
multiple entries work in the first place: closing a limit removes the thing that
was standing in front of the code behind it, which is the same shape as the
ninth argument, one step along.

And one of the cases written to guard the register-pool fix was itself vacuous,
which only mutation said. It summed thirty array elements at *constant*
subscripts — and a constant subscript needs no computed base register, so the
leak it was written for could not occur in it. The program passed against the
bug. One character, `a(1)` to `a(i)`, is the difference between a test and a
decoration.

The 1985 code, for its part, has been almost entirely correct. Where it was
wrong, it was wrong about the machine — that address 0 is readable, that a
pointer fits in an `int`, that a `long` is four bytes. It was right about
everything it could control.

---

*Source: [github.com/ChristineTham/v10-unix-userspace](https://github.com/ChristineTham/v10-unix-userspace).
Research Unix editions 8, 9 and 10 are made available by Nokia Bell Labs and
Alcatel-Lucent for non-commercial use, via The Unix Heritage Society.*
