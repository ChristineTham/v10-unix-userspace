# Porting Research Unix V8 Userspace to macOS (ARM64)

**Status:** Phases 0 through 2b complete and tested; **Wave A done** — 156 of 163 single-file commands in `usr/src/cmd` build, and the seven that do not are accounted for individually (see the foot of `tests/wavea/run.sh`); none is a compiler defect. 278 tests. **Wave B done**; **Wave C in progress** — nroff, troff, tbl and eqn all work, and V8's own yacc builds V8's grammars. Phases 4 and 5 not started.
See "Current state" at the bottom.
**Project arc:** V8 first (this plan), then V9, then V10 — restoring the original project goal of the last Research Edition, with V8 as the beachhead where the lessons are cheapest.
**Targets:** macOS on Apple Silicon (primary), Linux on ARM64 (secondary).
**Source of truth:** `third_party/Research-Unix-v8/` (TUHS release, Alhadis git mirror; case-collision recoveries documented in `third_party/Research-Unix-v8/CASE_COLLISIONS.md`).

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

3. **`$V8ROOT` must not depend on the environment.** It is `getenv` today, and
   when unset `rootpath()` silently returns the host path — so a V8 binary run
   outside the launcher quietly operates on the real filesystem. Same silent
   fall-through class as the variadic libc gaps. Needs a compiled-in default
   stamped at install (`-DV8ROOT_DEFAULT`), with the env var as override.

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
| Emulate | `sbrk/brk` (reserved anonymous mmap arena, monotonic break), `signal` (V8 reset-on-delivery semantics + `SIGDOPAUSE`/`SIGDORTI` packing over sigaction; number translation ≈ identity except 16/23), `ioctl` (sgtty `TIOC*` ↔ termios; `FIONREAD`; `TIOC[GS]PGRP`), `nap` (ms sleep), `syscall()` (dispatch into this table), `#!` handling in execve if needed |
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
| 1b ARM64 back end | done | `v8ccom` 62/62 — arithmetic, control flow, pointers, arrays, globals, statics, recursion, structs, bitfields, floats, 12-argument calls |
| 1c driver | done | `v8cc` 8/8, `make rootfs` |
| 2a libv8sys | done | `v8sys` 44/44 |
| 2b V8 libc | done | 89 objects, compiled by v8cc: stdio (incl. `%f`/`%e`/`%g`), the string family, malloc, ctype, qsort, getenv, the directory routines, `setjmp`/`longjmp`, perror and IEEE floats (`libv8c` 19/19) |
| 3A Wave A | done | **156 of 163** single-file commands in `usr/src/cmd` build, including `ls`. 29 of them are exercised with golden output (`wavea` 62/62): `cat`, `cmp`, `col`, `comm`, `cut`, `deroff`, `echo`, `expand`, `fgrep`, `fold`, `grep`, `head`, `join`, `look`, `number`, `od`, `paste`, `pr`, `printenv`, `pwd`, `rev`, `seq`, `sort`, `split`, `sum`, `tail`, `tee`, `tr`, `uniq`, `unexpand`, `vis`, `wc`, `yes`, `ascii`, `basename`, `cal` |
| 3B Wave B | done | The **Bourne shell** runs (`sh` 21/21) — see `src/cmd/sh/PORTING.md`. The file and process tools run too (`waveb` 21/21): `cp`, `mv`, `mkdir`, `rmdir`, `sed`, `ed`, `dc`, `factor`, `primes`, `tsort`. **38 multi-file command directories** compile and link, including `ps`, `w`, `df`, `tbl`, `qed`, `adb`, `yacc`, `man`, `diff3`, `dump`, `su`, `cron`, `compress` |
| 3C Wave C | in progress | **nroff, troff, tbl and eqn all run** (`wavec` 16/16). `tbl \| nroff` formats a table and `eqn \| nroff` sets an equation. eqn is built with **V8's own yacc**, itself compiled by v8cc. nroff fills and honours `.br`, `.ll`, `.ce`, `.sp`, `.na`; troff emits the device-independent stream for the 202 typesetter, with its tables compiled by `makedev` at build time. See `src/cmd/troff/PORTING.md` and `src/cmd/tbl/PORTING.md`. `eqn` and `pic` need their yacc grammars generated; `refer` has six files using the pre-C89 `int x 5;` form V8's own grammar rejects |
| 4 grovelers | not started | |
| 5 blitterm | not started | |

`make test` runs everything.

### What actually works today

V8's own preprocessor and compiler, with a new ARM64 back end, driven by V8's
own `cc`, produce object code that assembles, links and runs correctly on Apple
Silicon. Everything above the code generator is untouched 1985 Bell Labs source.

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

None of them is a bug in what Bell Labs wrote. All of them are the port's to
absorb, and the rule that has held is: **fix it where the width is decided —
the target model, the seam, or the one conversion routine — never per program.**

### Deliberate gaps in the back end

Switches are linear compare chains (correct, but dense switches in troff and
the shell want a jump table). Debug symbols are stubbed; when they arrive they
should be DWARF through the host assembler, not VAX stabs. The ELF/Linux path
is written but untested.
