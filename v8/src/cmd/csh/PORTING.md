# csh(1) — porting notes

Task #93.  Bill Joy's C shell, 4.1BSD vintage: 31 files, 12524 lines.
Upstream `v8/usr/src/cmd/csh`, installs to `/bin` (`Admin/binfiles:10`, and
the shipped tree has `/bin/csh`).

**BUILT AND DELIBERATELY NOT INSTALLED.**  The Makefile has its rules and
they run; `$(CSH_BUILT)` is deliberately absent from `$(V8DIRBIN_BUILT)`, and
`tests/wavea` exempts it by name.

It links with an **empty `nm -u`** and runs the whole language:

| | |
|---|---|
| `@` arithmetic | `set x = 6; @ y = $x * 7` → **42** |
| `foreach` | `foreach i (p q r)` → **pqr** |
| `while` | counts **321** |
| `switch`/`case` | **matched** |
| globbing | `gt/*.txt` expands; `[nosuch]` is correctly an error |
| `if ( -e /bin/sh )` | **present** |

**And it hangs on every external command**, which is why it is not installed.
A shell that prints the right answer and then waits forever is worse in the
world than no `csh` at all — the same rule that kept `struct` out until it
worked.

The output is CORRECT and arrives first; only the exit never comes.  Sampled,
the stack names one function and no guessing is required:

```
process -> execute -> pwait -> pjwait -> sigpause -> sigsys
        -> v8s_sigsys -> v8s_sigpause_wait -> sigsuspend
```

so `pjwait` blocks for a `SIGCHLD` that never wakes it.  **Two candidate
causes were measured and are not it**, which is worth recording because both
were plausible:

- **An unblock/suspend race.**  `sigset.c` spells sigpause as ONE call
  (`sigsys(n|SIGDOPAUSE, act)`) because the VAX set the action and slept
  atomically, so unblocking and then suspending would lose a signal arriving
  in between.  Real, fixed (`sigsuspend` does the unblocking now) — and **not
  the cause**: the hang is deterministic, 12 runs of 12.  The intermittency
  that suggested a race was my own harness, an 8-second deadline in one run
  and 6 in the next.
- **An invalid `sigprocmask` `how` of 0.**  A genuine bug — SIG_BLOCK is 1, so
  the mask query silently failed and returned an empty mask.  Fixed.  No
  change to the hang.

Also ruled out by measurement rather than argument: the shape of stdin (file,
pipe and detached all hang identically), and a missing `/dev` from
`tests/kmemu`'s delete-and-restore (`/dev` is intact and a rebuild changes
nothing).

## What was fixed getting here, and all of it is independently right

Four of these are defects in code csh merely *reached* first:

- **`signal.h`'s `DEFERSIG` and `SIGUNDEFER` truncate a function pointer**
  through `int`.  The header had never been imported, so it was still 1985's —
  the `sys/fblk.h` shape.  `SIGISDEFER` is the third of the trio and was
  **accidentally correct**, because it reads bit 0 and bit 0 survives a
  truncation; changed anyway rather than left as the odd one out.
- **`v8s_wait3` handed the kernel the wrong struct.**  V8's third argument is
  `struct vtimes *` (10 ints, 40 bytes); Darwin's `wait4` writes `struct
  rusage` (144).  So it wrote 104 bytes past a stack automatic and over the
  return address, and csh ran a command, printed its output, and jumped to
  address 0.  Third instance of the struct-at-the-seam class after `sigaction`
  and `stat`.
- **`v8sys_isheap`**, and `sh.set.c`'s two users of `extern char end[]` — the
  memory-model finding described below.
- **`getpgrp` was missing while `setpgrp` was present**, which is how one half
  of a pair sits absent with nothing to say so.

New and awaiting their consumer: `v8s_sigsys` and `v8s_sigpeel`.  They are
syscall-table entries rather than library code, which is why they are in the
shim beside `killpg` and `setpgrp`.

## Four apparent blockers, none of them real

Measured before writing any rule, because guessing from a `-l` name is the
mistake `src/libplot/PORTING.md` records:

| | measured |
|---|---|
| `xstr` string sharing | **already ported** — `v8/src/cmd/xstr.c` |
| `-ljobs` | a **real** archive, 6348 bytes with source, not `libm.a`'s 216-byte stub |
| libjobs is 7 files | 268 lines, and **four are VAX `.s`** |
| those four | `killpg` `setpgrp` `wait3` already in `shim/v8sys`; `getwd.c` already in `src/libc/gen` |

So **libjobs reduces to `sigset.c`, 185 lines of C** — `sigset` `sighold`
`sigpause` `sigrelse` `sigignore` and its own `signal`, the System V reliable
signal layer csh calls **88 times** (`sigrelse` 30, `sigsys` 22, `sighold` 21,
`sigset` 10, `sigignore` 3, `sigpause` 2).

## It compiles, and the link leaves exactly two symbols

All **19 C objects** compile under v8cc with upstream's `-DTELL -DVMUNIX
-DVFORK`; the only diagnostic is `NCARGS redefined`, csh's `sh.local.h`
against `sys/param.h`, which is the DIRSIZ multi-spelling shape and a warning.

The LP64 audit is **clean** — none of the undeclared-pointer-return, the
`int`-holding-`malloc`, the `#include`d-non-header or the address-0 argv
sweeps finds anything.  csh is BSD code and better declared than V7's.

Linking gives `_sigsys` and `_end`, and nothing else.

### `_sigsys` — a syscall whose source is VAX assembly

`signal.s` provides `_sigsys` (4.1BSD call 48) and `_sigpeel` (the
signal-return trampoline).  Its contract is in the file's own header:

```
sigsys(n, SIG_DFL)          default action
sigsys(n, SIG_HOLD)         block the signal temporarily
sigsys(n, SIG_IGN)          ignore
sigsys(n, label)            handler
sigsys(n, DEFERSIG(label))  handler, entered with the signal held
returns the old label
```

That is `signal(2)` plus a hold bit, and every clause maps onto the shim's
existing `sigaction`/`sigprocmask` machinery — `DEFERSIG` in particular is
just sigaction's *default* (the signal masked during its own handler).
`_sigpeel` has a precedent too: `shim/v8sys/sigtramp.s` exists because the
kernel's `sigaction` wants a trampoline, which is the same job.

### `_end` — NOT a missing symbol, a missing MEMORY MODEL

`sh.set.c:251,259` is `extern char end[]`, and the two users are

```c
onlyread(cp) { return (cp < end); }
xfree(cp)    { if (cp >= end && cp < (char *) &cp) cfree(cp); }
```

Both ask one question — *is this pointer heap-allocated* — and answer it from
the VAX's contiguous text/data/bss/heap/stack ordering.  **This target does
not have that ordering.**  `shim/v8sys/mem.c` takes its heap from
`mmap(0, 1GB, ...)`, so the arena sits wherever the kernel put it and is not
contiguous with `__DATA`.  Defining some `end` symbol would produce a
plausible wrong answer rather than an error, which is the worst outcome
available.

**So the fix is a shim predicate, not a constant**, and the shim can answer
exactly: the arena is `[arena_base, arena_brk)`.  Both call sites also have a
safe direction, which bounds the risk:

- `onlyread` has **one** caller (`sh.set.c:413`, `onlyread(value) ?
  savestr(value) : value`) where answering "yes" merely copies.
- `xfree` has **87**, where freeing a non-heap pointer is the crash.

A shim predicate is therefore *stronger* than upstream, which would happily
free a stack pointer if one ever reached it.

## Do not link it until sigsys exists, and here is why

Only `sigsys` errored.  `sigset`, `sighold` and `sigrelse` **resolved
silently from `-lSystem`** — measured; macOS ships System V compatibility
versions of all three.  That is the host leak `tests/kmemu` exists to catch,
and it would have been invisible in a build that "worked".

## Three decisions left, each with a precedent here

- **`sigset.c` defines `signal()` and so does libv8stubs.**  The
  duplicate-definition class.  Upstream resolves it by link order (`-ljobs`
  before libc) and so can this port, but it must be **asserted** rather than
  assumed: a stub member already pulled in for another reason wins instead.
- **csh builds its own `printf.c`, `doprnt.c` and `alloc.c`** against libv8c.
  That is population #3 of the same sweep — a program's own objects against
  an archive — where 25 of 56 collisions were silent.  Note `doprnt.c` **is
  not C**: it is VAX assembly wrapped in cpp macros (`.globl __doprnt`,
  `#define flags r10`), which is why upstream's rule is `cc -E | as`.  So
  csh's own printf cannot be built here and libv8c's is the answer.
- **`xstr` cannot be used end to end.**  Its point is `cc -c -R`, putting the
  shared string table in read-only TEXT, and `-R` is the arm64 shared-text
  stop that already blocks rung 5 for `cpp` and `sh`.  Building without the
  pass is an optimisation lost, not semantics — the same call already made
  twice.
