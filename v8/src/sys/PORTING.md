# V8's kernel, in the shim

`src/sys/` is the first Bell Labs **kernel** source in this port. Everything
above it — 58 programs, a C library, a compiler — is userspace. This is the
other side of the seam.

Today it holds one thing: **Dennis Ritchie's stream machinery**, PLAN.md §8a
step 1.

| File | What it is |
|---|---|
| `dev/stream.c` | 483 lines, 19 functions: block allocation, queues, the service-procedure scheduler. **Byte-identical to upstream.** |
| `h/stream.h` | `struct queue`, `struct block`, `struct qinit`, `struct stdata`, the `M_*` message types. Unmodified. |
| `research/sparam.h` | The stream configuration for `research`, the machine V8 was developed on. Unmodified. |

The machine-dependent half is `shim/kern/`, and `shim/kern/NOTES.md` is the
companion to this file.

## Not one line of `stream.c` was changed

`tests/streams` asserts it, by comparing `git hash-object` against
`src/sys/dev/PROVENANCE`. That is a stronger claim than any of the userspace
ports can make, and it was worth some effort to keep.

**It is possible because the footprint is tiny.** Measured before anything was
written — the whole of `stream.c`'s dependency on the kernel around it:

| Name | Uses | Where it comes from now |
|---|---|---|
| `NULL` | 35 | `shim/kern/h/param.h` |
| `splx` | 13 | `machdep.c` — a nesting counter |
| `spl6` | 10 | `machdep.c` |
| `panic` | 5 | `machdep.c` |
| `printf` | 4 | `machdep.c`, redirected away from the program's |
| `u_char`, `caddr_t` | 3 | `param.h` |
| `mtpr` | 1 | `mtpr.h`, honouring `SIRR` only |
| `uballoc` | 1 | a macro; there is no Unibus |

Nine names. A 483-line message-passing engine that barely knows what machine it
is on — which is exactly why §8a puts streams first, and it is *why* streams
were the piece of V8 that survived into System V and SunOS while the rest of the
VAX kernel did not.

Two of those nine deserve their own note, because in both cases the alternative
was a source deviation and the header won.

**`uballoc`.** `qinit()` maps the block pool onto the Unibus for DMA and panics
if it cannot. There is no Unibus. A three-line deletion would have been the
obvious fix; a macro returning a non-zero token costs nothing and keeps the blob
hash. Nothing observes the value — `blkubad` is read in exactly two places in
the whole V8 kernel, `dev/ill.c` (Interlan Ethernet) and `dev/kdi.c` (Datakit),
both converting a block address into a bus address for a board that is going to
DMA out of it. If a driver ever wants a real one, the macro stops compiling.

**`printf`.** Redirected to `v8k_printf`, which writes to fd 2 with a raw
`write`. `libv8c` also defines `printf`, so without the redirect the kernel's
four diagnostics would go through the *V8 program's* stdio — buffered with its
output, and gone entirely if it had closed or redirected stdout.

## The include path is doing something deliberate

`stream.c` says `#include "../h/param.h"`, and a quoted include is resolved
against the including file's directory **first**. From `src/sys/dev/` that is
`src/sys/h/param.h`, which does not exist, so it falls through to `-I`, and the
build passes `-Ishim/kern/dev`.

The result is the property worth having: **where an authentic header exists it
wins, and ours fill only the gaps.** `src/sys/h/stream.h` is real V8 and is
found by the same rule from the same `#include` syntax, with nothing marking
which is which. Importing an authentic `param.h` later would displace ours
without touching a build rule.

## Two dialects, and a build flag rather than an edit

`stream.c` is 1985 K&R: implicit `int`, implicit function declarations, no
prototypes. Clang rejects all three by default. The fix is `-std=gnu89` with
those diagnostics off — **a build flag, not a source change**, which is the
fidelity contract working rather than being worked around. CLAUDE.md says do not
modernise K&R declarations; the way to obey that is to compile the dialect the
code is written in.

Two suppressions in `KERNFLAGS` are *not* about K&R syntax and were checked
rather than waved through. `-Wchar-subscripts` fires on `bsize[bp->class]` and
`char` is signed on this target — but `class` is assigned 0..3 by `allocb()` and
`qinit()` and nowhere else, so it cannot index backwards. `-Wunused-variable` is
`freeb()`'s `bp1`, used only under `#ifdef CAREFUL`, V8's own debug switch.

## Why it is a separate archive

`libv8kern.a`, not part of `libv8sys.a`. Two reasons, one of them new:

- **The libkmemu reason.** A facility with a cost should be linked only where it
  is used, and `nm` should be able to prove it.
- **A storage reason.** `NBLOCK` blocks plus `NQUEUE` queues are about 85 KB of
  bss, and `qinit()` threads every block onto a freelist, dirtying roughly 60 KB
  of otherwise-clean pages. `cat` should not carry the storage of a
  message-passing engine it never opens. `qinit()` is therefore called through
  `v8k_streaminit()` on demand, not by a constructor.

`tests/deps` asserts both edges: the kernel archive depends on all six inputs
including the three stand-in headers reached by `-I`, and it does **not** reach
`cat` or `libv8sys.a`.

## What the tests found

`tests/streams` is 43 cases and three of them were wrong before the code was.
Worth recording, because each was a misunderstanding of the engine rather than a
typo:

- **`putq` coalesces.** Three one-byte writes are *one block* on the queue, so a
  FIFO test that pushed `A`, `B`, `C` and read the first byte of each message
  got `A` and looked like a failure. It is the whole reason a queue of terminal
  input does not cost a block per keystroke. The test now uses full blocks; the
  coalescing has a case of its own, and it is the only path through `bcopy`.
- **`putctl` and `putcpy` do not call `putq`.** They call
  `(*q->qinfo->putp)(q, bp)` — the queue's own put procedure, because a module's
  job is to decide what happens to a message rather than to assume it is
  enqueued. A probe whose `putp` discarded blocks made both look broken.
- **`backq` panics rather than returning null** when the link is missing, so
  getting the direction backwards in a test is a crash and not a wrong answer.

And two real defects, both in the new code:

- **`panic` did not format.** Every panic in `stream.c` is a plain string, so a
  version that printed its format verbatim looked correct against every
  authentic caller — until `v8k_mtpr`, written afterwards, panicked with a
  register number and printed a literal `%x`. Now `kvprintf` is the shared core.
- **Dropping `bcopy` does not leak `bcopy`.** Mutation-testing the guard by
  removing the `#define` showed the archive importing **`_memmove`**: clang
  recognises the pattern and lowers it. So the assertion is written as "the only
  external is `_memcpy`" rather than as "bcopy is absent", which would have
  missed it.

`_memcpy` is the one permitted external, and nobody here wrote it — clang emits
it for the struct assignments in `allocq()`. It resolves to **V8's own**
`memcpy`, which `libv8c` defines, and that is asserted rather than assumed,
because "it links" does not say whose.

## What is not here yet

`sys/streamio.c` — the syscall side, 1093 lines: `stopen`, `stclose`, `stread`,
`stwrite`, `stioctl`, and the `I_PUSH`/`I_POP` module stack. It needs inodes,
`u.u_error`, `tsleep`/`wakeup` and a file table, so it is entangled with the
process model in a way `stream.c` is not — and the design question it raises is
a real one, because this port's shim is **per-binary**, while a stream between
two processes is not.

That is the same question §8a step 2 answers for filesystems, so the two should
be answered together rather than twice.

### Measured, so the cost is a number rather than a feeling

The footprint survey that made `stream.c` affordable — **nine names** — was run
again against `streamio.c` before deciding anything. It is not nine.

| | `stream.c` (in) | `streamio.c` |
|---|---|---|
| external names needed | **9** | **~34** — 10 types, 14 functions, 6 globals, ~40 constants |
| authentic headers to stand in for | 3 | **11** |
| of those, mechanical translations | 9 of 9 | ~20 of 34 |
| of those, *design decisions* | 0 | `tsleep`, `wakeup`, `longjmp`, `u`, `copyin`/`copyout`, `iomove`, the file table |

Four headers drag in the entire process model — `user.h`, `proc.h`, `inode.h`,
`file.h`. `dir.h` and `buf.h` come along innocently as prerequisites of
`user.h` and `iomove`. `struct user` is referenced **69 times** across 10
distinct fields, 49 of them `u.u_error` alone.

**The decisive name is `tsleep`** — it is the first compile error, and the
per-binary question above is not a caveat on this work but its precondition.

### SETTLED, and the answer is milder than this section used to claim

This paragraph used to say `tsleep` "can only mean run `queuerun` and re-poll —
a change to the engine's semantics". Measured against the file, that overstates
it. Three facts, none of which needs the import to establish:

**1. Every `tsleep` sits inside a condition re-test loop.** `stopen:51` is
`while (sp->flag&STWOPEN) { tsleep(...) }`; `stread:217` is `for (;;) { ... if
(getq(...) == NULL) { ... tsleep(...); continue; } ... }`. That is ordinary
kernel discipline — sleep/wakeup is *advisory*, and a `tsleep` that returns
without the condition holding is harmless because the caller loops. So a shim
`tsleep` is not obliged to reproduce "block until exactly this channel is
signalled"; it is obliged not to spin and not to miss a wakeup.

**2. Every `wakeup` that can release a sleeper is in this same file**, and all
nine are in four functions:

| waker | what it is |
|---|---|
| `strput` (5 of them) | the stream head's read-side **put** procedure |
| `stwsrv` (1) | its write-side **service** procedure |
| `stopen` (1), `stioctl` (1) | self-wakes, same thread, same syscall |

`strdata = { strput, ... }` and `stwdata = { nulldev, stwsrv, ... }` register
the first two as the head's `qinit` procedures, so they are reached by
`putnext` and by `queuerun()` — not by an independent thread.

**3. The engine calls neither.** `grep tsleep\|wakeup src/sys/dev/stream.c`
returns nothing: `stream.c` is pure message passing and never blocks.

So the only producer that is genuinely "another process" is the **driver at the
bottom of the stack** — and what sits at the bottom of a stack is a question
this port has already answered once, for filesystems, in §8a step 2: the host.
A V8 stream's driver end is a host descriptor (a tty, a pipe, a socket), which
makes the shim's `tsleep`

```
	tsleep(chan, pri, timo):
		queuerun();                    /* anything already in the stream */
		poll(driver fds, timo);        /* the host kernel IS the other process */
		return TS_OK / TS_TIME / TS_SIG
```

and `wakeup` a no-op, because there is no second thread to release and the
re-test loop plus `queuerun()` covers every in-stream case. **That is faithful
rather than a semantic change**: in the kernel `tsleep` waits for the driver to
interrupt, and here it waits for the host fd that stands in for the driver.

`shim/kern/dev/machdep.c` already has the first half — `splx()` runs
`queuerun()` when the level returns to 0 — and its own comment anticipates the
second: *"When a signal-driven source arrives (a tty, a socket), spl6 gains the
mask and the counter stays exactly as it is."*

### And the piece that looked left over belongs to a different file

The obvious remaining worry is a stream between two *V8* processes, where the
producer really is another process and no host fd backs it. **In V8 that is
`pipe(2)`, literally** — `sys/pipe.c:16` says "Allocate 2 open inodes, stream
them, and splice them together", and `:67-70` cross-connect each stream's write
queue to the other's read queue. So it is not a corner case; it is the most-used
IPC in the system.

It is also **not `streamio.c`'s problem**, which is the measurement that matters
here. `pipe.c` is a separate 129-line file, the dependency runs *pipe.c →
streamio.c* (it needs `nilinfo`, the black-hole `streamtab`), and `streamio.c`
mentions pipes exactly **once**, in a comment. Every stream `streamio.c` itself
opens is attached to an inode's `i_sptr` with a *device* at the bottom — which
is the single-ended, driver-backed case the `tsleep` answer above covers.

And this port has already answered the pipe question a different way:
`v8s_pipe` in `shim/v8sys/syscall.c` is the host's `pipe(2)`, and every V8
program has had working pipes throughout. Importing `pipe.c` would mean
*replacing* something that works with something that needs two-ended streams —
a decision worth taking on its own merits, and not a prerequisite for anything.

**So the per-binary question is answered for this step.** What was recorded as
its remaining content turns out to be scoped to a file that is not being
imported. The 33 other names are mechanical, and the four hazards below are the
real cost.

**A pure stratum exists and is too small to justify the rest.** Five functions —
`qattach`, `qdetach`, `streadable`, `nilopen`, `nilput`, 86 lines, 7.9% of the
file — need nothing beyond a two-line `nulldev()`. `qattach`/`qdetach` are the
module push/pop primitives, which is exactly what `tests/streams` cannot reach
today. But a byte-identical import must *compile and link whole*, so all ~34
names must exist before the first of those five can run. Two-thirds of the file
would be provably unreachable at the end of it.

`strput` is **not** pure, despite looking it: `streamio.c:372` calls
`forceclose()`, which walks `file[0]`…`fileNFILE`. `stexit` is 12 lines and
calls `stclose`, which reaches `tsleep` and `closef`. Most functions inherit the
file table through the `stenter`/`stexit` edge.

**Conclusion: answer the per-binary question first, then import once.** Not
because the work is large, but because importing first would mean writing
`tsleep` twice and deciding its semantics under the pressure of a build that
does not link.

### Four hazards to settle BEFORE importing, not after

Found while surveying, and the second is a conflict between two of this port's
own commitments rather than a bug in anything.

1. **`streamio.c:713` is an upstream LP64 bug.**
   ```c
   if (copyout((caddr_t)&fmt, arg, sizeof(arg)))     /* fmt is int, arg is caddr_t */
   ```
   **VERIFIED, and the decision is taken: it becomes `sizeof(fmt)`.** The
   declarations are `caddr_t arg` (`:543`) and `int fmt, nld, ioctime` (`:549`),
   so on LP64 this reads **8 bytes out of a 4-byte object** and writes them to
   user memory. On the VAX `sizeof(caddr_t)` was 4 and so was `sizeof(int)`, so
   upstream's line computed the right number *by coincidence* — which makes the
   change target-forced under S1 rather than a matter of taste, exactly like
   `strncat`'s and `troff`'s.

   The survey compared it against one sibling. **There are nine**, and that is
   what settles it — `stioctl` copies at `:562 :567 :576 :623 :665 :690 :713
   :732 :779`, and every one but `:713` names the *object* (`nld`,
   `stq->pgrp`, `ld`, `sizeof(union stmsg)`, or a computed
   `bp->wptr - bp->rptr`). One of nine names the pointer. A typo, not a design,
   and now argued from eight controls rather than one.

   Reached by `FIOLOOKLD` with a non-null argument. A byte-identical import
   would inherit it, so this is a recorded deviation to apply at import time —
   `tools/import.sh` keeps the upstream hash, and this paragraph is the diff's
   justification.

2. **`stream.h:67` is `short pgrp`, and it cannot be widened.** `streamio.c:573`
   stores a pid into it (`stq->pgrp = u.u_procp->p_pgrp = u.u_procp->p_pid`),
   and `:371` then does `gsignal(stp->pgrp, SIGHUP)`. This port widened
   `p_pgrp`/`p_pid` to `int` in `src/include/sys/proc.h:36` precisely because a
   macOS pid above 32767 reads back negative — but `src/sys/h/stream.h` is
   **byte-identical and asserted so by `tests/streams`**, so the same fix is not
   available. A pid over 32767 truncates negative here and signals a negative
   process group. Exactly the `p_pid` class, including the part where a freshly
   booted CI runner never reaches it.

3. **`stream.h:69` is `char count`**, signed on ARM64 macOS, incremented
   unbounded by `stenter` (`:890`) and tested `--stp->count==0` by `stexit`
   (`:904`). 128 nested entries wraps negative and the stream never closes.

4. **`struct user` would then exist twice in this port, under two rules.**
   `shim/libkmemu/procfs.c` already synthesises one declared *by byte offset*
   with `_Static_assert`s, because `/proc`'s ABI is the real 4016-byte VAX
   layout. A kernel-side `struct user` for `streamio.c` needs no particular
   layout — nothing outside the file reads it. Two structs of one name with
   opposite constraints is worth naming here, before it is created.

One smaller thing worth knowing: the 13 ioctl codes `streamio.c` needs
(`FIONREAD`, `TIOCGPGRP`, `FIOPUSHLD`, …) are **already spelled with identical
values** in `shim/v8sys/ioctl.c` as `V8_*`. A second spelling would be the
`DIRSIZ`-in-three-headers mistake starting over.
