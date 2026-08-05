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

Today the world has 58 installed binaries, including the Bourne shell, `troff`,
`nroff`, `tbl`, `eqn`, `pic`, `grap`, `refer`, `spell`, `make`, `yacc` and
`lex`. `grap | pic | troff` draws a graph end to end. `refer` resolves citations
against an index its own tools built. The compiler reproduces itself: the ccom
built by ccom, built by ccom, generates byte-identical assembly. 649 tests
across 15 suites guard it.

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

Rung 5 is demonstrated on seven programs chosen for their *makefile idioms*
rather than their size: `lex` (dependency lines on `#include`d non-headers),
`sed` (one-line rules and `*.o` globs), `fmt` (macro expansion), `tsort`
(suffix rules), `tbl` (a 22-target dependency line), `yacc`, `spell`. V8's make
handled every one unchanged.

And it taught us something our own rules could not have: `tbl` and `yacc` prove
V8's make gets `#include`d-non-header dependencies right — meaning **the
knowledge our Makefile had to be told was in the tree the whole time.**

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

What is lost is documented rather than hidden: names longer than 14 characters
are truncated (**and truncation can alias two entries onto one name**), and
inode numbers are folded to 16 bits (**and can collide**). Both are authentic
V8 limits — a V8 program could not have seen more either — but both are real.

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

### V8 assumes address 0 is readable

The VAX put the text segment at 0, so `*(char *)0` returned a byte of the
program rather than trapping. macOS keeps page 0 unmapped.

`refer` hit it at end of input, so a test with one citation would not find it.
`df` hit it in `while (argc >= 1 && argv[1][0]=='-')` — which dereferences the
NULL terminating `argv` — so **every invocation of `df` crashed before printing
anything.**

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

---

## Part 9: What we have just worked out

The plan says V8 first, then V9, then V10. Before building the next large piece,
we went and read V9 and V10. Several assumptions did not survive.

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

## What is left

Phase 4 has `w` and `ps` outstanding. Then, in order: streams; the 9P switch
carrying only a passthrough server, so the suites stay green while the floor is
replaced; `/proc`; `mkfs` and a raw image; the V8 filesystem server over it, and
with it the ten programs currently written off as "raw VAX disks"; the SIMH
cross-check; an FSKit host client alongside the Blit terminal app; and then V9,
which needs `mk` first.

---

## The thing this project keeps teaching

Almost nothing here failed loudly.

The compiler bug printed fifteen correct hex digits and dropped the sixteenth.
The signal bug returned success and hung an hour later. The group-database
escape printed a perfectly plausible list of group names — from the wrong
machine. The buffer overrun corrupted output several rows after the row that
caused it. Five missing libc functions gave *right answers* for months, from
Apple's implementations, in a project whose entire premise is that the code is
Bell Labs'.

So the discipline that matters is not care while writing. It is building things
that fail loudly on your behalf: mutation-tested guards, provenance hashes,
`nm -u` sweeps over the whole world rather than one sample, hooks that refuse
the plausible mistake, and a habit of measuring the actual path rather than a
convenient proxy for it.

The 1985 code, for its part, has been almost entirely correct. Where it was
wrong, it was wrong about the machine — that address 0 is readable, that a
pointer fits in an `int`, that a `long` is four bytes. It was right about
everything it could control.

---

*Source: [github.com/ChristineTham/v10-unix-userspace](https://github.com/ChristineTham/v10-unix-userspace).
Research Unix editions 8, 9 and 10 are made available by Nokia Bell Labs and
Alcatel-Lucent for non-commercial use, via The Unix Heritage Society.*
