# csh(1) — porting notes

Task #93.  Bill Joy's C shell, 4.1BSD vintage: 31 files, 12524 lines.
Upstream `v8/usr/src/cmd/csh`, installs to `/bin` (`Admin/binfiles:10`, and
the shipped tree has `/bin/csh`).

**IMPORTED AND DELIBERATELY NOT BUILT.**  It is not in the Makefile and
`tests/wavea` exempts it by name, with the reason quoted there.  What follows
is exactly how far it got and what the two remaining symbols are.

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
