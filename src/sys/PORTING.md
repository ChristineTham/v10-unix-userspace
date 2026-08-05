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
