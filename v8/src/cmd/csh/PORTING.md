# csh(1) — porting notes

Task #93.  Bill Joy's C shell, 4.1BSD vintage: 31 files, 12524 lines.
Upstream `v8/usr/src/cmd/csh`, installs to `/bin` (`Admin/binfiles:10`, and
the shipped tree has `/bin/csh`).

**BUILT AND INSTALLED.**  It links with an **empty `nm -u`** and runs the whole
language, including external commands, pipelines and backquotes.

## The defect that mattered: a `short` holding a host pid

It stood built-and-not-installed for exactly one commit, because it produced
correct output and then hung on every external command.  The recorded diagnosis
was a `SIGCHLD` that never woke `pjwait`, taken from a stack sample.  **The
sample was true and the inference from it was wrong**: the signal machinery was
correct throughout.

`sh.proc.h` declared (now `:39` after the PORT comment)

```c
	short	p_pid;
	short	p_jobid;		/* pid of job leader */
```

`palloc` stores what `fork` returned; `pchild` compares it against what `wait3`
returns (`sh.proc.c:51`, `pid == pp->p_pid`).  V8 pids wrapped at 30000 and a
VAX `short` held every one of them.  macOS hands out pids to 99998, so above
32767 the stored copy is negative, the comparison never matches, `PRUNNING` is
never cleared, and `pjwait` pauses forever for a child it has already reaped.

Measured, with the two ends printed side by side:

```
palloc: recorded pid 45267
pjwait: pp->p_pid    -20269       <- 0xB0D3 read as a signed short
pchild: wait3 ->      45267
pchild: NO MATCH for  45267
```

It is the **identity** half of the 16-bit-range class, the same shape as
`d_ino`: the field is not a quantity being approximated, it is a key, and a key
that does not compare equal is not a rounding error.  Widening is safe for the
reason `awk`'s `struct rrow` was — the struct has **one end**.  It is csh's own
in-core list: not on disk, not across the shim seam.

`p_jobid` goes with it and is not decoration: it is a pgrp, and `pkill` hands it
to `killpg` (`sh.proc.c:860`).

### A second defect rode with the same fix

`sh.h`'s `shpgrp`, `tpgrp` and `opgrp` were `short` too, and every one of them
has its **address taken** for `ioctl(TIOCGPGRP/TIOCSPGRP)` — an `int *` in V8's
own header and in this port's shim (`shim/v8sys/ioctl.c:225`, `*(int *)arg = n`).
So the get wrote four bytes into a two-byte global and the set read two bytes of
whatever followed it.  Widening is what makes the declaration agree with the
ioctl it is passed to.

`nfunc.c` carries the identical `short ctpgrp` and is **deliberately not
changed**: upstream's own makefile builds `sh.func.o` and never names it, so
nothing forces a change to a file no build reads.  Both guards are scoped so
that this is a consequence rather than an exception — `tests/wavea`'s width
sweep derives its file set from the **built objects**, and `tests/deps` asserts
from the other end that `nfunc.c` is a prerequisite of nothing.

## How it was found, and what the wrong turn cost

The recorded stack sample sent a whole session at the signal layer.  What ended
that was a **probe**: `pjwait`'s loop — `sighold`, read the flag, `sigpause` —
written as forty lines outside csh and linked against the same `libjobs`.  It
worked on the first run, which said the defect was in csh and not underneath it.

Then instrumentation, and **the instrument was wrong before the program was**:
the first trace wrote to fd 2 and printed nothing at all, which reads exactly
like "these functions are never called".  csh reshuffles 0/1/2 to high
descriptors at startup (`sh.c:861-864`).  The second version opened its own file
and the answer arrived immediately.  *An instrument you wrote is a suspect*, for
the fourth time in this tree.

## Why no behavioural test can guard it, and what does

**A freshly booted host hands out low pids**, so every behavioural case here —
run a command, run a pipeline, run twenty in a row — passes against the broken
shell on a CI runner.  That is the same property that let the 16-bit `p_pid`
survive in `tests/kmemu`, and it is why this reached a commit.

So `tests/wavea` carries three kinds of case:

- the **value** cases (six of them, one per fork path plus `$status`), which are
  the demonstration and are live only above 32767;
- the **width** case, which is wrong at every pid — no built csh source may
  declare a pid in a `short`, over a file set derived from the built objects;
- a line that prints how high this host's pids actually go, and says out loud
  when the value cases cannot see anything.

## And a second defect, found by CI: backquotes lost their output ~0.4% of the time

The commit that installed csh went red on the runner with `csh: backquotes
finish / want [inner] / got []`.  Reproduced locally under eight-way
contention: **2 failures in 480**, where V8's `sh` doing the same thing was
**0 in 480**.

The difference between the two shells is the whole diagnosis.  csh installs a
SIGCHLD handler (`sigset(SIGCHLD, pchild)`); `sh` installs none, so only csh
can have a blocking read interrupted.  And `backeval` reads the backquote pipe
like this (`sh.glob.c:690-691`):

```c
icnt = read(pvec[0], ip, BUFSIZ);
if (icnt <= 0) { c = -1; break; }
```

**EINTR is indistinguishable from end-of-file there.**  So a SIGCHLD landing
inside that read ended the substitution early and `set v = \`echo inner\``
quietly produced the empty string.

**IT IS NOT csh's BUG AND THE FIX IS NOT IN csh.**  On upstream's kernel that
read is restarted, and V8 decides it in one flag:

| | |
|---|---|
| `sys4.c:318-320` | `ssig()` sets `SNUSIG` when the action is `SIGISDEFER(f)` -- exactly what `sigset()` passes |
| `sys2.c:55,103` | `read`/`write`: `if ((p_flag&SNUSIG) && setjmp(u_qsav)) if (u_count == uap->count) u_eosys = RESTARTSYS` |
| `trap.c:184` | `if (u.u_eosys == RESTARTSYS) pc = opc` -- back the PC over the chmk and re-issue |

So V7's `signal(2)` yields EINTR and the reliable interface **restarts**, in the
same kernel, and the shim was applying the first rule to both.  `v8s_sigsys`
sets `SA_RESTART` on its deferred arm now -- XNU's flag carries the same
proviso as `u_count == uap->count`, so it maps exactly rather than
approximately.  Measured after: **0 failures in 1200**.

Three things generalise:

- **A RECORDED DECISION WAS RIGHT AND INCOMPLETE, which is worse than wrong.**
  `shim/v8sys/signal.c` and `shim/NOTES.md` both said "no SA_RESTART -- V8
  programs expect a slow read to fail with EINTR".  True of `signal(2)`, silent
  about the interface that had no caller until csh arrived.  The unexercised-rule
  shape, in a *note* rather than in code.
- **THE GUARD CANNOT BE THE SYMPTOM.**  At 0.4% no behavioural case is worth
  anything.  `tests/v8sys` asserts the PROPERTY, as a pair -- one case per arm,
  because a blanket rule in either direction passes half of them -- and it is
  deterministic and cannot hang, because **the handler is the writer**: the
  read is interrupted, the handler puts a byte in the pipe, and the restarted
  read returns it.
- **CI FOUND IT AND LOCAL RUNS DID NOT**, twice over: the runner's pids reached
  98638, so it was not the "freshly booted, low pids" machine the width guard
  was written for -- the assumption in that guard's comment was about runners in
  general and is now measured to be false for this one.

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

Two more were found while chasing the hang, and are real bugs that were simply
not the cause: an unblock/suspend race in `v8s_sigsys` (`sigsuspend` does the
unblocking now, because it installs its mask and waits as one operation), and a
`sigprocmask` called with a `how` of 0, which is not a valid operation.  Both
are kept.  **Neither changed the hang**, which is what should have been read as
evidence sooner: two correct fixes to a layer, neither moving the symptom, is
the layer telling you it is not the layer.

`v8s_sigsys` and `v8s_sigpeel` are syscall-table entries rather than library
code, which is why they are in the shim beside `killpg` and `setpgrp`.

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
sweeps finds anything.  csh is BSD code and better declared than V7's.  **The
sweep that was missing is the one that found the bug**, and it is now written
down beside the others: a `short` holding a host pid.

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

`onlyread` and `xfree` (`sh.set.c:276` and `:284`) each used `extern char
end[]`, and reduce to

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

- `onlyread` has **one** caller (`sh.set.c:441`, `onlyread(value) ?
  savestr(value) : value`) where answering "yes" merely copies.
- `xfree` has **87**, where freeing a non-heap pointer is the crash.

A shim predicate is therefore *stronger* than upstream, which would happily
free a stack pointer if one ever reached it.

## Do not link it without sigsys, and here is why

Only `sigsys` errored.  `sigset`, `sighold` and `sigrelse` **resolved
silently from `-lSystem`** — measured; macOS ships System V compatibility
versions of all three.  That is the host leak `tests/kmemu` exists to catch,
and it would have been invisible in a build that "worked".  `tests/deps`
asserts the archive is a real prerequisite of the binary for that reason: the
failure it guards is not a link error but a silent substitution of macOS's
signal semantics for V8's.

## Three decisions, each with a precedent here

- **`sigset.c` defines `signal()` and so does libv8stubs.**  The
  duplicate-definition class.  Upstream resolves it by link order (`-ljobs`
  before libc) and so does this port, and `tests/kmemu`'s collision sweep is
  what keeps it honest rather than an assumption.
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

## Still open

- **Job control is untested.**  `csh -c` and script execution are exercised;
  `%1`, `fg`, `bg`, `stop` and `^Z` need a controlling terminal, which is the
  `/dev/fd/3` arrangement the `v8` launcher makes rather than something a
  suite can set up cheaply.  `tpgrp` is `-1` in every case run here, which is
  csh's own "no job control" value, so the widened pgrp variables are
  exercised for width and not for job control.
- **`nfunc.c`** is the alternate `sh.func.c` and is not built; see above.
