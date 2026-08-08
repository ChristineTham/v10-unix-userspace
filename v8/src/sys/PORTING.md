# V8's kernel, in the shim

`src/sys/` is the first Bell Labs **kernel** source in this port. Everything
above it — 58 programs, a C library, a compiler — is userspace. This is the
other side of the seam.

Today it holds one thing: **Dennis Ritchie's stream machinery**, PLAN.md §8a
step 1 — now both halves of it, the engine and the syscall side.

| File | What it is |
|---|---|
| `dev/stream.c` | 483 lines, 19 functions: block allocation, queues, the service-procedure scheduler. **Byte-identical to upstream.** |
| `sys/streamio.c` | 1093 lines: `stopen`, `stread`, `stwrite`, `stioctl`, `stclose`, the module stack, file passing. **Two recorded deviations, both LP64** — see below. |
| `h/stream.h` | `struct queue`, `struct block`, `struct qinit`, `struct stdata`, the `M_*` message types. Unmodified. |
| `h/dir.h` `h/inode.h` `h/ioctl.h` `h/ttyld.h` `h/file.h` `h/inline.h` | The six authentic headers `streamio.c` needs. Unmodified. |
| `research/sparam.h` | The stream configuration for `research`, the machine V8 was developed on. Unmodified. |

The machine-dependent half is `shim/kern/`, and `shim/kern/NOTES.md` is the
companion to this file.

**ELEVEN HEADERS, SIX THEIRS AND FIVE OURS, AND THE SPLIT IS THE RULE WORKING
RATHER THAN A COMPROMISE.** `streamio.c` includes eleven, and which side each
lands on was decided one at a time by asking whether the header describes V8 or
describes a VAX:

| ours, in `shim/kern/h/` | why |
|---|---|
| `param.h` | VAX page sizes, cluster counts, the u-area's virtual address. We take the six constants that are pure numbers and leave the rest |
| `user.h` | hazard 4: three pointer-shaped fields the /proc ABI freezes at VAX widths |
| `proc.h` | opens by including `pcb.h`, `dmap.h` and `vtimes.h` — VAX virtual memory, to obtain four fields |
| `buf.h` | 107 lines of buffer cache and disk-driver state, to obtain `B_READ` and `B_WRITE` |
| `conf.h` | the driver switch tables, one row per peripheral on a machine that is not here |

The other six are imported unchanged, because a `struct inode`, a `struct file`
and an ioctl number are V8 rather than DEC. Nothing in the `#include` syntax
says which is which — a quoted `"../h/x.h"` finds `src/sys/h/` first and falls
through to `-Ishim/kern/dev` — so **importing an authentic `user.h` later would
displace ours without touching a build rule.**

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

`tests/streams` began at 43 cases -- it is 111 now, the syscall side having
brought 68 more -- and three of the original 43 were wrong before the code was.
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

## The syscall side, and what it cost

`sys/streamio.c` is **in**. What follows is the record of the survey that
preceded it — kept because every number in it was checked against the file
before anything was written, and three of the four hazards it names were
settled by reading *upstream* rather than by reading us — followed by what
actually happened.

**The headline is that the survey was right about the shape and generous about
the cost.** Thirty-four external names, of which nineteen already existed; the
fifteen left came to four small files in `shim/kern/sys/`, named after the ones
V8 keeps them in — `slp.c` (tsleep, wakeup), `fio.c` (the u-area, the file
table, the inode edge), `subr.c` (the twelve mechanical names), `ioconf.c` (the
configuration table `config(8)` would have generated). `tests/streams` went
from 43 cases to 111.

### Two deviations, both LP64, and they are not the same shape

`stream.c` can be guarded by its hash because nothing in it changed.
`streamio.c` cannot, so `tests/streams` diffs it against `third_party/` and
asserts that upstream lost **exactly one line** and that it is the one below.
The second deviation *adds* a line, which is why the test counts removals and
additions separately — a first draft asserted two removals and failed,
correctly.

**1. `:713`, a replacement.** `copyout((caddr_t)&fmt, arg, sizeof(arg))` where
`fmt` is `int` (`:549`) and `arg` is `caddr_t` (`:543`) — eight bytes read out
of a four-byte object and written to user memory. Four by coincidence on a VAX.
Argued from eight controls: `stioctl` copies at nine sites and every one but
this names the *object*.

**2. `urcvfile`, an addition.** Upstream declares only `stq`, so `arg` is an
implicit `int` — and `stioctl:614` calls it with its own `caddr_t arg`. On LP64
the user address is truncated to 32 bits on entry and sign-extended again at
the `copyout`, which is a wild pointer write from `FIORCVFD`. The twin one
function up, `usndfile`, declares `caddr_t arg` for the same argument from the
same caller. **One of a matched pair has it and the other does not** — the
shape `unexpand` and `expand` already produced in this tree.

It was found by the compiler rather than by the survey, which is worth saying:
`v8cc` widens undeclared K&R parameters on purpose (`acctype()` in
`compiler/ccom-arm64/gencode.c`), and `streamio.c` is compiled by **clang**, so
the widening this port relies on everywhere else is simply absent here.

### What the lp64-auditor found afterwards, including in our own code

Run against the imported file, and the dominant class came back clean — no
other undeclared parameter holds a pointer, no pointer is stored in an `int`,
every pointer-returning callee is declared. Four things it did find:

- **`u_uid` and `u_gid` were a bare cast to `short` in `fio.c`, one line below
  the paragraph arguing why a bare cast is wrong for pids.** CLAUDE.md's own
  warning — *the fix lands on one line and the line beside it keeps the
  assumption*. The magic value is 0-means-root, `streamio.c:44` lets root
  bypass a stream's exclusive-use lock, and `sndfile:951` copies `u_uid` into
  the credentials `FIORCVFD` hands the far end. Fixed: root maps to root,
  non-root never maps to root. Latent exactly as `p_pid` was latent — 501 here,
  and uids past 100000 are routine on a directory-bound Mac.
- **Two of the reasons written down were wrong while the code was right.**
  `subr.c` said `nulldev` was safe because "every call site discards the
  result": false, all three `qopen` sites consume it. The real reason is
  topological — `strdata` is the *head's* qinit and `qopen` is only invoked
  below the head. And the Makefile said an `int` return leaves x0's top half
  "unspecified": measured, any write to `w0` zeroes bits 63:32, and an `int`
  return that forwards another call's result emits **no truncation at all**, so
  a 64-bit pointer survives intact. Both corrected in place; the second gave a
  sharper rule for the first driver (a `qopen` must never return a negative
  int, because `-1` becomes `0xffffffff`, which is neither NULL nor 1).
- **Two latent upstream defects that are not LP64 but whose failure mode
  changed.** `:61` `flushq(RD(qp))` passes one argument to a two-parameter
  function, so `flag` is register litter — garbage on the VAX too, but now
  unpredictable per path rather than per call site, and a nonzero `flag` frees
  queued `M_PASS` blocks instead of requeueing them. And `:796` computes
  `bp->wptr - bp->rptr` *after* advancing `rptr` by four, having range-checked
  before, so a short `M_IOCACK` makes the length negative — which our
  `copyout(…, unsigned long)` prototype turns into 1.8e19. Both need a driver
  that does not exist. Recorded rather than fixed: an unexercised fix cannot be
  seen to be right either.
- **The seven entry points had no declaration anywhere**, and `stopen` returns
  `struct inode *`. A future caller writing `int stopen()` would lose the top
  half of that pointer silently. They are now prototyped in
  `shim/kern/h/param.h` — which is `streamio.c`'s *first* include, so the
  prototypes are checked against the definitions on every build. A header
  nobody includes would have recorded the types without ever verifying them.

### The survey that preceded all of it

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
`user.h` and `iomove`. `struct user` is referenced **71 times on 70 lines**
across 10 distinct fields, 49 of them `u.u_error` alone. (This line said 69
until hazard 4 below needed the fields enumerated rather than counted; `:573`
names `u.u_procp` twice, which is where a line count and an occurrence count
part company.)

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

**THE `wakeup` HALF OF THAT IS WRONG, and it was wrong in the direction that
throws an answer away.** It is true of what `wakeup` must *do* — there is no
sleeper to release — and false about what the call is worth. Written as an
empty function it leaves `tsleep` unable to answer its own first question:
`queuerun()` has just run some service procedures, did any of them produce
anything? A counter answers it exactly, because every `wakeup` in `streamio.c`
sits on a path where a producer has just made progress. So `wakeup` increments
and `tsleep` compares across `queuerun()` — the whole of sleep/wakeup, reduced
to the one bit a single-threaded kernel can use. `shim/kern/sys/slp.c`.

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
imported. The 33 other names are mechanical, and the four hazards below were
the real cost — **all four are now settled too**, so what is left of this step
is writing code rather than deciding anything.

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

**That is what was done, and the order paid off exactly where it was supposed
to.** `tsleep` was written once, from the settled design, and its hardest
decision was one the survey had already framed: what to do when there is no
device below and no timeout. `shim/kern/sys/slp.c` panics there, because
returning `TS_OK` would spin, `TS_TIME` would invent a timeout nobody asked
for, `TS_SIG` would invent a signal, and `poll(NULL, 0, -1)` would hang with no
message attached. `tests/streams` asserts the panic, in its own program,
alongside the mtpr one.

**And `wakeup` did NOT end up a no-op, which the survey got wrong in the
harmless direction.** It said there is no second thread to release, and that is
true of what `wakeup` must *do*. It is false about what the call is worth: it
leaves `tsleep` unable to tell whether the `queuerun()` it just ran produced
anything. A counter answers that exactly — every `wakeup` in `streamio.c` sits
on a path where a producer has just made progress, so incrementing a counter
and comparing it across `queuerun()` **is** the wakeup, reduced to the one bit
a single-threaded kernel can use. No polling of queue state and no guessing.

### Four hazards to settle BEFORE importing, not after — ALL FOUR SETTLED

*(A fifth arrived at import time and is not in this list, because the list is
about what a survey could see in advance and that one could not: `urcvfile`'s
missing `caddr_t`, found by the compiler. "Two deviations" above has it.)*

Found while surveying. What settled every one of them was reading the
**upstream** declarations rather than only ours — `h/proc.h`, `h/user.h`,
`h/param.h` and `streamio.c`'s own call graph. One is a typo of upstream's, one
dissolves on counting, and **two are conflicts between two of this port's own
commitments**, which is precisely the kind that cannot be seen from our side of
the seam alone: from here they look like V8 being too small for the machine,
and upstream says the field was exactly the right size all along.

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

2. **`stream.h:67` is `short pgrp` — AND THE AUTHENTIC FIELD IS NOT THE NARROW
   ONE.** The hazard was recorded as "16 bits cannot hold a macOS pid, and the
   header is byte-identical so the `p_pid` fix is unavailable". Upstream
   contradicts the first half: `h/proc.h:28-29` declares `short p_pgrp` and
   `short p_pid`, so on a VAX `stream.h`'s field is **exactly as wide as a
   process id**, and `:573` — `stq->pgrp = u.u_procp->p_pgrp = u.u_procp->p_pid`
   — loses nothing at all.

   The narrowing is entirely this port's. `p_pgrp` and `p_pid` were widened to
   `int` at `src/include/sys/proc.h:36-37` because a macOS pid above 32767 reads
   back negative and **`ps` has to print the real one**. So this is not V8 being
   too small for the host; it is our widening meeting an authentic field that
   never needed it — which makes the answer forced rather than chosen, and it is
   the sentence `daddr_t` already gets: **a struct that crosses a seam keeps
   that seam's width.**

   `src/include/sys/proc.h` crosses to `ps` and to the /proc ABI, so it stays
   `int`. A kernel-side `struct proc` in `shim/kern/` crosses nothing —
   `streamio.c` is its only reader — so it keeps **upstream's `short`**, and the
   shim hands it ids in that range rather than raw host pids, mapping to the
   host's where a signal is actually delivered. The authentic header is then
   neither edited nor worked around: every store, the `:45` comparison and both
   `gsignal`s (`:371 :379`) are exact for every value the port can produce.

   One thing that is inherited rather than fixed. `TIOCGPGRP`/`TIOCSPGRP`
   through a stream (`:567 :576`) copy `sizeof(stq->pgrp)` — **two** bytes — to
   and from a user `int`, leaving its top half stale. The VAX copied two bytes
   there too, so unlike `:713` above there is no coincidence to undo and S1 says
   reproduce it.

3. **`stream.h:69` is `char count`, and 128 NESTED ENTRIES CANNOT HAPPEN HERE.**
   It is signed on ARM64 macOS, `stenter` (`:890`) increments it unbounded and
   `stexit` (`:904`) tests `--stp->count==0`, so a wrap would mean a stream that
   never closes. The field's own comment is the clue — `/* # processes in
   stream routines */` — and counting settles it.

   `stenter` is called from exactly **six** places: `:49 :205 :288 :428 :484
   :553`, which are `stopen`, `stread`, `istread`, `stwrite`, `istwrite` and
   `stioctl`. **None of the six is mentioned anywhere else in the file** — the
   only other occurrences of those names are inside `printf` strings, so none
   is called by another and, the part that actually needed checking in a file
   this full of `qinit` dispatch, **none is stored in a function-pointer table
   either.** They are the kernel's stream-switch entry points, one per system
   call. A process therefore contributes at most 1, and this port runs exactly
   one process per binary, so `count` is 0 or 1.

   That leaves one obligation rather than a fact: a *module or driver* put
   routine runs with a process already inside `stenter`, and re-entering the
   stream from one would nest. Upstream's cannot, because the six are not
   reachable; ours must not either, and that is a rule for `shim/kern/`, not
   something the import guarantees.

   The `longjmp` paths do not break that, which is the half worth checking
   rather than assuming: every `tsleep` returning `TS_SIG` calls `stexit`
   *before* `longjmp(u.u_qsav)` (`:54 :76 :223 :296 :436 :492`), because the
   jump unwinds past the function's own exit. `:977` in `urcvfile` is the same
   idiom on `stioctl`'s behalf — an `stexit` with no `stenter` above it in the
   same function, which is the balance for `:800` and not a double decrement.

   **What would make it reachable is precisely what this port is not:** a shared
   kernel with 128 processes inside one stream. Recorded rather than dismissed,
   because §8a could still grow one.

4. **`struct user` twice — and THREE FIELDS ARE THE WHOLE REASON.** The premise
   as recorded was a name colliding, and that part is already false:
   `shim/libkmemu/procfs.c` calls its one `struct v8user`. The substance is a
   width conflict, and it is now counted instead of feared.

   `streamio.c` touches **ten** `u.u_` fields — `u_error` (49 uses), `u_procp`
   (7), `u_count` (5), `u_qsav` (3), `u_uid` (2), and one each of `u_gid`,
   `u_ttyino`, `u_ttydev`, `u_r`, `u_ofile`. Seven are the same width on a VAX
   and on ARM64: chars, shorts, an `unsigned int`, and a union whose widest arm
   is 8 bytes either way. **Three are not, and all three for one reason.**

   | field | VAX | here | why it cannot be the /proc one |
   |---|---|---|---|
   | `u_procp` | `struct proc *`, 4 | 8 | dereferenced at `:45 :442 :573 :953 :1018 :1027`; the /proc slot at offset 296 is four bytes |
   | `u_qsav` | `label_t` = `long[14]`, 56 under NOLONG | `jmp_buf` = `long[24]`, 192 | `longjmp(u.u_qsav)` has to work, and 56 bytes will not hold an ARM64 frame |
   | `u_ofile` | `struct file *[128]`, 512 | 1024 | `:994` is `u.u_ofile[i] = fp` |

   So the structs are two because **the /proc ABI freezes three pointer-shaped
   fields at VAX widths and the machine needs LP64 ones.** A consequence to
   state once, not a smell to clean up later. The /proc one stays offset-
   declared and `_Static_assert`ed; the kernel-side one goes in
   `shim/kern/h/user.h` beside `param.h`, spells its ten fields honestly, and
   claims no layout, because nothing outside `streamio.c` reads it.

   **And building that table found a live defect in the /proc one.** `u_ofile`
   was `char *u_ofile[16]` — sixteen LP64 pointers, 128 bytes — carrying the
   comment `NOFILE` for a constant that is **128** (`sys/param.h:19`), so barely
   a third of the VAX's 512-byte slot was named. Every `_Static_assert` in the
   file passed. That is the reusable part: **an offset-plus-total-size pair is
   blind to an array's length**, because the pad after it was computed *from*
   that length, so a wrong length with a compensating pad is exactly as green as
   a right one. Harmless only because `synth()` leaves the field zero — and
   fatal the day a kernel-side `struct user` reuses the declaration, which is
   the hazard itself. Fixed, and the guard that can see it is a `sizeof` on the
   member rather than another offset.

### And "33 mechanical names" is a third of what it reads as

The remaining cost was recorded as a count. Enumerated instead — every
identifier `streamio.c` calls and does not define, then classified by who
already provides it — **19 of the 34 are already in the tree**:

| provider | names | n |
|---|---|---|
| `src/sys/dev/stream.c`, imported and byte-identical | `allocb allocq backq flushq freeb getq putbq putctl putq qreply queuerun` | 11 |
| `src/sys/h/stream.h`, macros in the authentic header | `RD WR OTHERQ` | 3 |
| `shim/kern`, already written | `spl6 splx panic` in `dev/machdep.c`; `printf` and `bcopy` redirected in `h/param.h` | 5 |
| **written, in `shim/kern/sys/`** | `slp.c`: `tsleep wakeup longjmp`. `fio.c`: `closef ufalloc iput`. `subr.c`: `copyin copyout iomove gsignal psignal selwakeup min nulldev`. `GETF` came free — it is a macro in the authentic `h/inline.h` | **15** |

(Three more names the extraction offers — `server`, `flag`, `called` — are
inside comments at `:519` and `:842-843`. Worth saying because a grep for
`name(` cannot tell code from prose, and 37 is the number it reports.)

The fifteen are not fifteen problems, and the shape of the work is the point:

- **`copyin` and `copyout` are `bcopy`.** One address space per binary, so
  there is nothing to copy *between*. Two lines each — and `:713`'s recorded
  deviation above is the only subtle thing about either.
- **`min`, `nulldev` and `GETF` are one line each upstream** — `sys/rdwri.c`,
  `sys/subr.c`, and a macro in `h/inline.h`, which is one of the eleven headers
  already on the list.
- **`tsleep` and `wakeup` are settled**: `queuerun()` then `poll()` on the
  driver's host fd. That was this step's blocker and it is answered above --
  though `wakeup` did not stay the no-op predicted there; it counts, which is
  what lets `tsleep` tell progress from deadlock.
- **`gsignal`, `psignal` and `selwakeup` are delivery**, and delivery works
  here — `shim/v8sys/sigtramp.s` and the `struct __sigaction` account in
  `shim/NOTES.md`.
- **`longjmp` on `u.u_qsav` lands exactly on hazard 4's second field.** The
  kernel-side `struct user` needs a jump buffer this machine can actually use,
  which is why it cannot be the /proc one.
- **`closef`, `ufalloc` and `iput` are the real three**, and the survey above
  already named why: `strput` looks pure and is not, because `:372` reaches
  `forceclose()`, which walks `file[0]`…`file[NFILE]`. The file table and the
  inode edge are the design left in this step, and they are three names, not
  thirty-three.

One smaller thing worth knowing: the 13 ioctl codes `streamio.c` needs
(`FIONREAD`, `TIOCGPGRP`, `FIOPUSHLD`, …) are **already spelled with identical
values** in `shim/v8sys/ioctl.c` as `V8_*`. A second spelling would be the
`DIRSIZ`-in-three-headers mistake starting over.

**The import answered that better than avoiding it would have.** `h/ioctl.h` is
one of the six authentic headers now in `src/sys/h/`, so the codes are
upstream's own definitions rather than a transcription — which turns the `V8_*`
list in `shim/v8sys/ioctl.c` from a second spelling into a *checkable copy* of
a single authority.

### And a trap the import walked into, which is the DIRSIZ shape wearing host clothes

`shim/kern/h/param.h` has to typedef `dev_t`, `ino_t` and `off_t`, because the
authentic `inode.h`, `file.h` and `dir.h` are spelled in them. **Darwin owns all
three names, and two are a different width:**

| | ours (upstream's) | Darwin's |
|---|---|---|
| `off_t` | `long`, 8 | `long long`, 8 |
| `dev_t` | `u_short`, 2 | `int`, 4 |
| `ino_t` | `u_short`, 2 | `unsigned long long`, 8 |

So **which definition won would have depended on include order**, and two
objects in one link could have disagreed about the layout of `struct inode` by
twelve bytes with nothing to say so. Not hypothetical: `tests/streams/probe.c`
included `<stdio.h>` first, as any probe naturally would.

The fix is what a kernel header has always done — claim Darwin's own guard
macros, `_OFF_T`, `_INO_T`, `_DEV_T`, so the host's typedefs become no-ops.
And for a file that includes param.h *after* a host header, an `#error` with an
instruction in it, because the alternative is a silent layout difference.
`time_t` is deliberately **not** claimed: no field of any struct here has that
type, so there is no layout to protect, and claiming it would hand the modern C
in `shim/kern` a 32-bit `time_t` for its raw syscalls.

The rule generalises past these three: **a stand-in kernel header that typedefs
anything must ask whether libc owns the name**, and if it does, claim the
guard rather than hope about order.
