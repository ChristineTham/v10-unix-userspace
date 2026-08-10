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
assembly. **1767 tests across 17 suites** guard it.

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

## What is left

Phases 0 through 4 are done, and so is Phase 6 — `make install` stamps a prefix
into every binary and writes a launcher that drops you at `/` in a world whose
`/bin`, `/etc`, `pwd` and compiler are all V8's, with the Mac still reachable
through `PATH`. The filesystem switch, `/proc`, `/dev/fd`, `mkfs`, the raw
image and its ten tools, the stream engine and the tty line discipline are all
in.

What remains, in rough order:

- **The V8 filesystem server over the raw image**, which is what turns the image
  from something the tools inspect into something the world can mount. The
  reading half is done and it is the part that could not be faked: `mkfs(8)`
  writes an image, and V8's own kernel opens a file in it by name — `namei` to
  `fsnami` to `dsearch` to `iget` to `bmap` to `readi` to `bread` to a block
  driver — and hands back 28000 bytes that `cmp` says are identical to the file
  mkfs was given. The file sits two directories down and spans 28 blocks, so the
  walk crosses a subdirectory and goes through the indirect block, not just the
  ten addresses in the inode. The writing half is done too — a file created by
  name, grown past the superblock's cached free list so the on-disk chain has to
  be followed, deleted, and the block and inode accounting exactly restored,
  with `icheck`, `dcheck` and `fsck` pronouncing the result a filesystem. What
  remains is the **mount** — and costing it is what produced the section above.
  It cannot be a fourth type in the shim's `vfs.c`, because that puts the kernel
  in the client and twenty-nine programs share a global name with it; and it
  could not survive `exec` even if they did not. So the mount is a **server**,
  one connection per open file so that the socket itself is the descriptor and
  nothing has to be inherited through a table. **That server now exists** — it
  reads a file out of an image over 9P and `cmp` agrees — and what is left is
  the client half, plus the writes. **The client now exists too**: with one
  environment variable set, `cat /mnt/sub/deep` returns those same 28000 bytes,
  and `ls`, `tail`, `wc`, `grep` and shell redirection all work against the
  image unmodified, and a probe compiled against the shim reaches the three
  paths no 1985 program does — `fstat` on a directory descriptor, `lseek` in
  all three whences, and two descriptors sharing one offset. What is genuinely
  left is the **write half of the server** — `Twrite`, `Tcreate`, `Tremove` and
  `Twstat` answer `EROFS` today, while the kernel code underneath them is
  written and tested — and the one gap the client cannot close on its own,
  which is `chdir` into a mount: nothing in the shim tracks a working
  directory, so a relative name inside a mount would resolve against the host.
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

The 1985 code, for its part, has been almost entirely correct. Where it was
wrong, it was wrong about the machine — that address 0 is readable, that a
pointer fits in an `int`, that a `long` is four bytes. It was right about
everything it could control.

---

*Source: [github.com/ChristineTham/v10-unix-userspace](https://github.com/ChristineTham/v10-unix-userspace).
Research Unix editions 8, 9 and 10 are made available by Nokia Bell Labs and
Alcatel-Lucent for non-commercial use, via The Unix Heritage Society.*
