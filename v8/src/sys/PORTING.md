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

## `ttyld.c` and `partab.c`: the tty line discipline, and one number that is ours

Third and fourth authentic kernel files, both imported **byte-identical** and
both guarded by hash in `tests/streams` — which is the point of how `NTTY` was
handled, below.

`ttyld.c` is 596 lines and is **line discipline 0**, not a device:
`conf/devices:75` reads `standard line-discipline 0	tty	tty	info`, and what
pushes it is `init.c:377`, `ioctl(0, FIOPUSHLD, &tty_ld)`, immediately before
the three `dup(0)`s that make fds 1, 2 and 3. `partab.c` is 51 lines of pure
data — the parity-and-class table `ttyld.c` declares as `extern char partab[]`
— and lives in `sys/`, not `dev/`, which an earlier survey had wrong.

### It cost one function, and the survey was right about that

Fifteen external names, fourteen already in `libv8kern.a`. The one missing was
`max()`, eight lines at `sys/rdwri.c:236`, and it is in `shim/kern/sys/subr.c`
beside `min()` rather than imported, because `rdwri.c` is the file I/O layer —
`readi`, `writei`, `iomove` — and taking sixteen lines of arithmetic would mean
taking all of it. Same judgement as the `printf`/`bcopy`/`uballoc`
redirections in `param.h`.

`outconv` looks like a sixteenth callee and is `ttyld.c`'s own (`:351`). After
the import the archive's externals are still exactly `_longjmp _memcpy
_setjmp`, unchanged, which is the property that says nothing new leaked in.

**AND WRITING max() FOUND min() MISDECLARED, WHICH IS THE USEFUL PART.** Both
`param.h` and `subr.c` recorded that upstream's `min` has "no declared return
type, so int(unsigned, unsigned)". `rdwri.c:249` is the word `unsigned` on a
line of its own and `:250` is `min(a, b)`; there is exactly one `min` in the
kernel and no `min` macro in `h/`. Both are `unsigned` now, which is
upstream's. Nothing observable changed — `register n` in `streamio.c` is an
implicit int and every call is bounded by a 1024-byte block, so bit 31 is
clear and the two types have the same bits — and that is exactly why a wrong
note survived beside a working function. It would have been copied into `max`.

### `NTTY` is this port's decision, and it is derived rather than picked

`ttyld.c:6` is `#include "tty.h"`, and there is no such file to import.
`h/tty.h` upstream is **zero bytes**, a make timestamp node
(`conf/makefile:61-62` touches it to mean "sgtty.h and ioctl.h are current"),
and it is not what `ttyld.c` gets anyway — a quoted include tries the
includer's own directory first, and `dev/` has no `tty.h`. What `ttyld.c` is
asking for is the per-configuration header `config(8)` generates:
`conf/files:98` marks the file `optional tty pseudo-device`, so
`pseudo-device tty N` in a machine description becomes `#define NTTY N`. That
description is not shipped, `conf/config` is a VAX `a.out` binary rather than
source, and there is **no `#define NTTY` anywhere in `third_party/`**.

So it is ours, and it goes in `shim/kern/dev/tty.h` — reached by exactly the
fall-through that turns `"../h/param.h"` into the stand-in beside it, with
`-Ishim/kern/dev` already in `KERNFLAGS`. **No edit to Bell Labs' source was
needed to supply it, which is what keeps the hash guard available.**

The value is **128 = NSTREAM** (`src/sys/research/sparam.h:6`, authentic), and
it is derived: `NTTY` bounds `struct ttyld tty[NTTY]`, the pool `ttyopen()`
allocates from, and a slot is one discipline **attached to a stream** rather
than one terminal — `ttyopen` returns 1 immediately when `qp->ptr` is set, so
it is one slot per stream, and a process cannot hold more streams than
`NSTREAM`. On a VAX `NTTY` counted configured terminal lines and was far
smaller; it cannot mean that here, because the shim is per-binary and `tty[]`
is per-process, so "how many terminals has this machine" is not a question one
process can answer. "How many streams can this process hold" is, and it is the
tight bound. Cost is not the argument either way: `struct ttyld` is 14 bytes,
so the array is 1792 bytes, and the archive's zero-initialised storage went
from 96332 to **98124 bytes** — a figure a correct import increases.

### What is exercised, and why the open path is not blocked by the missing driver

There is no hardware driver under it, and an earlier survey concluded from
that fact that `ttyld` "cannot be exercised". False at the open path:
`ttyopen` (`:41-63`) never dereferences `q->next` and sends nothing
downstream, so it runs on a bare queue pair. Only *traffic* needs a bottom
end. `tests/streams/ttyprobe.c` is a third probe beside `probe.c` (the engine)
and `sioprobe.c` (the syscall side), and it pushes the discipline through
`qinfo->qopen` rather than calling `ttyopen` by name, so the `long (*)()` slot
the Makefile suppresses a warning about is the thing under test.

27 cases: the default terminal `ttyopen` builds (ECHO|CRMOD, erase `^H`, kill
`@`, intr DEL, quit FS), idempotence on a second push, `ttyclose` releasing the
slot, `partab` content at three points, and exhaustion.

**Exhaustion is the case that matters, and it is the `qopen` rule.** CLAUDE.md
records that a `qopen` must never return a negative int, because `return -1`
becomes `0x00000000ffffffff`, which `stopen:124` does not see as NULL and
`:131` does not see as 1 — so a refusal would read as SUCCESS and hand back an
inode pointer of `0xffffffff`. `ttyopen` returns exactly 0 and 1 on every
path. Measured, not read: 128 disciplines fit, the 129th is refused, and the
refusal is 0. Mutating `return(0)` to `return(-1)` turns that case red (and
the hash guard with it).

The one flag beyond `KERNFLAGS` is `-Wno-incompatible-function-pointer-types`,
for `ttyld.c:34-35` initialising `struct qinit`'s `long (*)()` slots with `int`
functions — identical to `streamio.c`'s case and spelled as its own
`TTYLDFLAGS` rather than folded into `KERNFLAGS`, because `stream.c` compiles
clean without it.

### The traffic paths — DONE, and it took a driver rather than a module

This section used to say the five functions below the open were compiled,
linked and undriven, and that reaching them needed "something under the
discipline to send to". That is now built. `tests/streams/ttyprobe.c` carries a
driver — 83 lines of code, measured — and drives **two** stacks, because the
open path and the
traffic paths want different ones:

| stack | for | why |
|---|---|---|
| a bare queue pair | `ttyopen`, `ttyclose` | `ttyopen` never dereferences `q->next`, and exhaustion needs `NTTY+4` opens — 132 whole streams would be absurd |
| stream head / `ttyld` / driver | everything else | every other function reaches past its own queue |

The real stack is built the way `init.c:368-382` builds one — `stopen()` the
driver, `v8k_stconf()` the discipline, `FIOPUSHLD` to push it between them —
so the only new code is the bottom layer. The middle is `ttyld.c`
byte-identical and the top is `streamio.c`.

**Why one function needs both ends, which is the whole argument for a driver.**
`ttyldin` sends data **up** through `q->next` and flow control **down**
through `WR(q)->next`, in the same loop. A second module stacked above would
see the first and never the second, so it cannot distinguish a discipline that
sends `M_STOP` downstream from one that does not. Mutating
`putctl(wrq->next, M_STOP)` to `putctl(q->next, M_STOP)` turns exactly one case
red — *"and the device is told"* — and that case is unreachable without a
bottom end.

60 new cases. What they cover, and the three worth knowing about:

- **The read path needs `ttyldin` AND `ttyinsrv` for one read.** `ttyldin`
  queues bytes and, on the newline, enqueues an `M_DELIM` and `qenable`s;
  `ttyinsrv` runs at `splx(0)` and gathers the line. Erase and kill are
  `ttyinsrv`'s alone — `ttyldin` queues `\010` verbatim.
- **ECHO closes a loop through 1985 code.** The bytes go back out the write
  side as they arrive, through `ttyosrv` and `outconv`, and the driver sees
  them CRMOD'd. A terminal shows you what you typed because the kernel sends
  it back, and that is now measured rather than assumed.
- **`ttysig` ends in a real signal.** DEL is `t_intrc`, `ttyldin` recognises
  it, `ttysig` flushes both queues and sends `M_SIGNAL` up, `streamio.c:379`
  turns that into `gsignal`, and the shim's `gsignal` is a `kill(2)` to the
  probe. The assertion is that a **handler ran**. Six layers, one keystroke.

**THE TAB DOES NOT EXPAND BY DEFAULT, AND THE FIRST DRAFT OF THAT CASE READ THE
LOOP WITHOUT ITS GUARD.** `outconv`'s expansion is behind
`(tp->t_flags&TBDELAY)==XTABS` (`:385`) and `ttyopen` sets `ECHO|CRMOD` only —
`XTABS` means *this terminal cannot do tabs itself*, a fact about hardware
rather than a default. So there are two cases: the literal tab that a default
terminal gets, and `a` + seven spaces once the flag is set. Same shape as
`min()` being found by writing `max()`: the expected value was wrong, the
measured one was right, and the guard was one line above the code being read.

**The ioctl arms differ in a way the return value cannot show.** `ttldioc`'s
`TIOCSETP` passes the block **down to the device** and the acknowledgement that
wakes `stioctl` is the *driver's*; its `TIOCSETC` arm is `qreply(q, bp)` with
`fromdev` 0, which turns the block round **at the discipline**. Both make
`stioctl` return 0. The only way to tell them apart is to ask the driver
whether it saw anything, which is what the pair of cases does. Mutating the
`TIOCSETC` arm to pass down turns exactly one case red.

And the `fromdev` **1** arm had never been taken by anything in this port: an
`M_IOCTL` sent *up* reaches `ttyldin`, which calls `ttldioc(WR(q), bp, q, 1)`,
and every arm then ends in `qreply(rdq, bp)` — back down as an `M_IOCACK`. A
modem asking the discipline for the line settings is answered without the
process being involved.

**The driver's one hard requirement is the one that hangs.** `stioctl`
(`streamio.c:759-786`) sends an `M_IOCTL` down and then `tsleep`s on
`stq->iocblk` for **fifteen seconds**. A driver that frees an `M_IOCTL` instead
of acknowledging it does not fail — it stalls. Measured by mutation: 30
failures and the probe killed by its 60-second alarm. That is also why
`ttyprobe` now runs under a deadline; the comment saying it needed none was
true of the open path and stopped being true the moment a driver went under.

### Where the driver lives, and why it is not in `shim/kern/`

With the probe. Nothing in the port consumes a tty driver: PLAN.md §8a step 1b
costed a host-fd driver to sit under `/dev/tty`, and that was measured wrong
four ways — V8's `/dev/tty` is a hard link to `/dev/fd/3` and opening it is
`dup(2)`. A driver in `shim/kern/` would therefore be a component with no
caller, which is the mirror of this port's recurring lesson: an unexercised
rule cannot be seen to be incomplete, and an **unconsumed component invents a
difference the kernel does not have**. `sioprobe.c`'s loopback and pipe drivers
are the precedent — scaffolding lives with the probe.

### The four flag-gated arms — also done, and one of them is pure 1970s

`LCASE`/`maptab[]`, escape handling, `TANDEM`, and `outconv`'s delays were
listed here as "cases to add rather than machinery to build", and they were.
16 more cases; streams 200 → 216.

- **`maptab[]` is a Model 33 Teletype.** No lower case and no braces, so V8
  lets you type them: `\a` is `A`, `\(` is `{`, and a bare `A` is folded
  *down* to `a`. The split between the two functions is the interesting part —
  `ttyldin` marks an escaped character by setting bit 7 and never consults the
  map; `ttyinsrv` sees the marked byte and does the lookup. So the map is
  applied one queue later than the escape that asked for it. `A\a\(` reads
  back `aA{`, and this is the only thing in the port that reads those 128
  bytes.
- **The escape arm has three outcomes and only one is obvious.** With `LCASE`
  off, an ordinary escaped character *keeps* its backslash (`\z` is two
  characters) but one that **is** the erase, kill or eof character is emitted
  alone — dropping the backslash is how you type a literal `@`.
- **A doubled backslash leaves the escape latched, and that is measured.**
  `ttyldin:171-175` strips bit 7 off an escaped backslash and **sets `TTESC`
  again**, so `\\z` yields `\` then an escaped `z`. Recorded as behaviour, not
  intention.
- **`TANDEM` is XON/XOFF seen from the receiving end** — the discipline
  noticing its *own input queue* filling and sending a stop character back.
  The threshold is upstream's arithmetic on `ttrinit`'s own numbers,
  `(600+60)/2` = 330, and the release is `ttyinsrv`'s tail rather than
  `ttyldin`'s, so it only happens because something *read*.
- **`outconv`'s delays are padding for four terminals that existed.** A
  carriage return on a tn 300 took longer than the next character took to
  arrive, so the discipline emits an `M_DELAY` the driver turns into silence.
  `CR1` selects it, the count is 5, and a negative control with the bits clear
  emits nothing — so the case measures the *algorithm* and not the presence of
  a return.

### THE AUDITOR FOUND A LIVE BUG IN THE DRIVER, AND V8 SHIPPED THE FIX

Run on the new code per CLAUDE.md's rule — *on the shim code written to make
that program build, which is where it has actually found things.* It came back
clean on width narrowing, K&R argument slots, `qopen` return values and address
0, and found this:

**The length of an acknowledgement is part of the acknowledgement.** `stioctl`
builds every `M_IOCTL` with `wptr += sizeof(union stmsg)` — **20 bytes,
unconditionally, whatever the command** (`streamio.c:755`). `ttldioc`'s
`TIOCSETP` arm does not touch `wptr`. And `streamio.c:793-798` bumps `rptr`
past the 4-byte command and copies `wptr - rptr` back to the caller's `arg`.
So an ack passed through unchanged copies **16 bytes into a `struct sgttyb`
that is 6 bytes long** — ten bytes past the caller's object, on every set.

**Bell Labs spend one line on it, in two drivers.** `dev/cons.c:56-58` is
`case TIOCSETP: case TIOCSETN: bp->wptr = bp->rptr;` and then **falls
through** to `TIOCGETP`, which must *not* reset it because it has a payload to
return; `dev/dz.c:229` is the same. Their default arms are `M_IOCNAK` with
`wptr = rptr`, which `streamio.c:803-809` turns into `ENOTTY`. The driver here
now reproduces that switch exactly, including the fall-through.

Three things worth carrying:

- **A value sentinel cannot see this class.** The ten bytes written are the ten
  `copyin` read from that same address moments earlier, so the write
  round-trips and every byte of memory ends up correct. The suite was green
  throughout. **The only observable is the fault.**
- **So the guard has to be a page, and it has to be `PROT_READ`.** `sg` sits at
  the last six bytes of a writable page with the next page readable but not
  writable — readable because `copyin`'s authentic 20-byte over-*read* must
  still succeed, so that the only thing which can fail is the write. In a
  child, because the failure is a signal. Measured: **0 with Bell Labs' line,
  SIGBUS without it.** That makes the convention a test rather than a comment.
- **The comment recorded the wrong conclusion.** It said "streamio.c copies
  `wptr - rptr` either way", which is true and is the reason **to** shorten the
  ack, written as though it were a reason not to. Same shape as the recorded
  constraint that blocked the inode fix for months: a sentence that is
  *accurate* and points the opposite way from what it concludes.

The `TIOCGETP` ack still copies 8 bytes into the same 6-byte struct, and that
one **is upstream's** — `cons.c` leaves `wptr` alone there too, so a VAX did
exactly this. Reproduced, not repaired.

One latent finding was taken as well: `allocb(n)` answers a request above 64
with a **64-byte** block when class 3's freelist is empty and class 2's is not
(`stream.c:44-46`), so the argument bounds the *request* and not the
*capacity*. Unreachable here — the largest call is eight bytes — and bounded
anyway, because this driver is the port's only worked example of one and a
missing bound is what the next driver would inherit.

**Streams is 220 cases**, from 140 before the driver: 60 for the traffic paths,
16 for the flag-gated arms, 4 for the acknowledgement conventions the audit
found.

### The six remaining arms — also done, and the list itself was wrong twice

**Streams is 236 cases.** All eight functions and every arm inside them that
this port can reach. Writing the list out was worth it on its own, because two
of its six entries were wrong:

- **`ttyosrv`'s `M_FLUSH` arm was described as unreachable, and the citation
  said the opposite.** The note read *"needs `TIOCFLUSH`, which `stioctl`
  handles itself at `:588-594`"* — as though `stioctl` handling it were the
  obstacle. It is the mechanism: `:594` is
  `putctl(stq->wrq->next, M_FLUSH)`, and `stq->wrq->next` **is** ttyld's write
  queue, so the block lands on it and `ttyosrv` runs. Accurate line number,
  opposite conclusion. Third instance of that shape here after the inode
  constraint and the "streamio copies `wptr - rptr` either way" comment, and
  the tell is the same each time: **the sentence cites something true and then
  draws the reverse inference from it.**
- **`BSDELAY` was listed as an unexercised delay and `ttyld.c` does not mention
  it.** `grep -c BSDELAY src/sys/dev/ttyld.c` is **0** — the flag exists in
  `ioctl.h` and this file has no arm for it. A name copied from the header into
  a list of "things in the file".

What the four real ones turned out to be:

- **`max()` had never executed.** It was written for this import — the one name
  `ttyld.c` needed that the shim lacked — and writing it is what found `min()`
  misdeclared in two files. It has **exactly one call site in the whole tree**,
  `ttyld.c:439`, in the tty 37 newline delay. Two cases now, because `max` has
  two branches and the column picks one: `abc` takes the constant 6, sixty-four
  characters take the computed 7.
- **`VTDELAY` is not reached by a vertical tab.** `partab.c:12` gives 013 (VT)
  class **1**, *non-printing*; it is 014, **form feed**, that is class 5. So the
  flag named for a vertical tab is a form-feed delay — 127 ticks, the longest
  number in the file, because ejecting a page is the slowest thing a printer
  does. Both are asserted: VT produces no delay, FF produces 127.
- **`ttyhog` caps the queue at 512 and makes the terminal beep.** A character
  that is not a newline is replaced by `\007` and never queued. 600 sent, 512
  counted, bells observed.
- **`canonb` is invisible to the reader, which is the better property.** 498
  characters cross the 255-byte flush at least twice and come back in **one
  read**, because `stread` loops on the delimiter rather than on a message
  boundary.

**AND THE CANONB CASE WAS WRONG TWICE BEFORE IT WAS RIGHT, both times from
reasoning instead of asking.** The first draft expected ">255 bytes in one
read". The second saw 145, concluded the line arrives in *pieces*, and asserted
that — and 145 was not a piece at all: it was the TANDEM case's unread
remainder, left behind because that case read a 401-byte line into a 256-byte
buffer. **A short read leaks into the next case**, which then reports a
plausible wrong answer and makes the case after *that* look broken. Same shape
as `tests/crash-probe.sh`'s programs reading each other's litter, arriving
inside one process instead of one directory. `ttyprobe.c` grew a `readline()`
that reads whole lines, and every canonical read goes through it.

**A footnote on the mutation harness, because it under-reported twice.** Two
mutations (`count = 6` in the CR delay, forwarding `M_DELIM`) were scored at one
failure by a batch runner and at two — the right two — when re-run alone. The
guards are fine; the batch reading is not. **Measure a mutation in isolation
before believing its count**, which is the same rule as validating a prober
against a known crasher before believing its number.

## `v8fs`: the six files, and what the headers turned out to be

PLAN.md §8a step 5 names four files -- `alloc.c`, `iget.c`, `nami.c`,
`rdwri.c`. The survey that preceded this import established that the unit is
**six**: those four, plus `sys/subr.c` (239 lines, for `bmap`) and
`dev/bio.c` (783, the buffer cache), 2743 lines in all. All six are imported
here; `nami.c` carries the only deviation.

### THE HEADERS ARE NOT SIXTEEN NEW FILES. THEY ARE FOURTEEN FILES ALREADY IN THE TREE

The survey costed the headers as *"20, and only one is a VAX document ... the
other sixteen are 1286 lines of ordinary structure"*, and treated that as a
size. It is not a size, it is a **collision**, and the measurement that shows
it is one line:

```
$ cd third_party/Research-Unix-v8/v8/usr
$ git hash-object sys/h/param.h include/sys/param.h
5409ff3980c6c4edb6d48503bdb301383475de3b
5409ff3980c6c4edb6d48503bdb301383475de3b
```

Every one of the fourteen headers that exists at both paths is **the same
upstream blob** -- `param.h dir.h inode.h filsys.h ino.h fblk.h buf.h proc.h
conf.h user.h systm.h mount.h acct.h vlimit.h`, checked individually. V8 ships
one file at `/usr/sys/h/` and `/usr/include/sys/`, byte for byte. So the
question step 5 actually asks is not *"what do these sixteen headers cost"*
but **"the port has already imported most of them once and patched some of
them; which copy should the kernel side see?"** -- and the answer differs per
header, because the patches were made for the userland layer:

| header | the port's local copy | what the kernel needs |
|---|---|---|
| `filsys.h` `ino.h` `fblk.h` | `src/include/sys/`, **patched** to `v8_i32`/`v8_u16` per field (§8a step 4a) | the SAME patch -- it reads the same disk |
| `dir.h` | both sides already present: `src/sys/h/dir.h` **pristine** (upstream says 14), `src/include/sys/dir.h` patched to 254 | 14, and it is already there |
| `param.h` | `src/include/sys/param.h`, patched to DIRSIZ 254 + an `#ifndef` guard | 14 -- see below |
| `inode.h` | `src/include/sys/inode.h`, **pristine** | the same; `src/sys/h/inode.h` is already imported |

That `filsys.h` row is the one worth stopping on. Importing the pristine
kernel copy into `src/sys/h/` would have given the kernel a `struct filsys`
whose `s_time` is 8 bytes and whose free-block map is 8-byte entries, over
images `mkfs` writes with 4 -- **reintroducing the exact bug §8a step 4a
fixed, on the other side of the same disk**, where no reader we have could
see it, because every reader would be using the patched header and only the
kernel the pristine one. The survey did not flag it because it counted the
headers by line and by VAX-reference, and an on-disk record has neither
property.

### And `param.h`: the DIRSIZ worry evaporates, and a different file decides it

The survey's caution was that a kernel-side `param.h` would be *"a third
spelling"* of DIRSIZ. There is no third spelling, and the reason is simpler
than the blob identity above: **upstream's `param.h` already says 14**
(`h/param.h:75`), which is exactly what the kernel side wants. A pristine
import would have been correct on the number the caution was about.

The precedent is likewise not the obstacle, and the first draft of this
section got it wrong in a way worth correcting rather than deleting. It said
`src/sys/h/dir.h` and `src/include/sys/dir.h` are *"two locally-divergent
copies"* -- implying both are patched. Measured, `diff` against upstream:
**the kernel copy is byte-identical and only the userland copy is patched.**
That is a *better* precedent than the one claimed, and the same is true of
`inode.h`. The rule the tree actually follows is **kernel side pristine,
userland side patched where the port had to widen something.**

What rules `param.h` out is neither of those. It is two concrete things:

- **`param.h:169-171` includes `"../h/types.h"` under `#ifdef KERNEL`,** and
  upstream's `h/types.h:23` is `typedef long daddr_t;`. Importing `param.h`
  therefore drags in a pristine `src/sys/h/types.h` with an **8-byte
  `daddr_t`** -- the `filsys.h` hazard above arriving by a second route, and
  this time through a header nobody would have thought to inspect, because
  the file being imported is `param.h`. (`:48` also pulls `<signal.h>`, which
  in a kernel compile with no `-Isrc/include` is the host's.)
- **`shim/kern/h/param.h` holds the `_OFF_T`/`_INO_T`/`_DEV_T` guards** that
  stop Darwin's typedefs silently redefining `struct inode`'s layout, plus
  the `printf`/`bcopy`/`uballoc` redirections that keep `stream.c`
  byte-identical. An authentic `src/sys/h/param.h` **wins the quoted
  include** and takes all of that away from `stream.c`, `streamio.c` and
  `ttyld.c`, which compile against it today.

Upstream's `param.h` is also headed `"Tunable variables"` and carries `NBPG`,
`PGSHIFT`, `CLSIZE`, `CLOFSET`, `UPAGES`, `clbase`, `clrnd` -- a machine
description by this tree's own test. But that was the weakest of the three
arguments and it is worth saying so: `CLSIZE 2` is what selects the 1024/4096
geometry, so it is as much a disk fact as a machine one.

So: **not imported.** The filesystem geometry it holds (`BSIZE(dev)`,
`INOPB(dev)`, `BMASK`, `BSHIFT`, `NMASK`, `NSHIFT`, `itod`, `itoo`, `fsbtodb`,
`dbtofsb`, `NINDIR`, `BITFS`, `BUFSIZE`, `NICINOD`, `NICFREE`) goes into
`shim/kern/h/param.h` at upstream's values, which is what that file's own
header comment already says the policy is. The values stay Bell Labs' and a
test **compares them against the authentic `src/include/sys/param.h`** rather
than trusting the transcription -- the same discipline as making the header
test compare `NMASK(0)` against the `sizeof`-derived `NINDIR` rather than
against a number someone typed.

### `nami.c`: the name compare, and `NOLONG` for the second time

The one deviation, and it is total rather than partial -- with it, no path
resolves at all. `nami.c:145-147`, inside `#if DIRSIZ == 14`:

```c
if (*(long *)&nm[0] == *(long *)&dp->d_name[0] &&
    *(long *)&nm[4] == *(long *)&dp->d_name[4] &&
    *(long *)&nm[8] == *(long *)&dp->d_name[8] &&
    *(short *)&nm[12] == *(short *)&dp->d_name[12]) {
```

A hand-unrolled 14-byte compare, and the offsets are the argument: 4+4+4+2 is
14 **only because V8's `long` is 32 bits**. That is
`cmd/ccom/vax/macdefs.h:20`, `# define NOLONG`, "map longs to ints" -- the
same line that made every on-disk struct in this port the wrong size. Note
what the `#if` one line above is doing: upstream is stating the arithmetic it
depends on, and stating it in `DIRSIZ`, the one term that *does* travel, while
the term that does not travel is invisible.

Here `long` is 8, so the three compares read 24 bytes where 12 were meant.
`d_name` is 14 and `struct direct` is 16 (kernel `ino_t` is `u_short`), so
bytes 14-15 of this entry and the first ten of the *next* decide an equality
that is supposed to be about the name -- and the last entry in a block reads
past the buffer. `dsearch()` fails to match a name that is present, so
`namei()` returns ENOENT for every path.

Fixed to `int`, which is 32 bits here and reproduces the VAX exactly. `short`
is 2 on both and is untouched. Measured: **3 lines removed, 37 added** (the
comment).

**The wrong fix is `-DDIRSIZ=254`**, which reaches the `strncmp` arm and makes
the symptom vanish by changing the size of an on-disk record. DIRSIZ is 14 in
`src/sys/` because that is what a V8 disk holds.

### What the `NOLONG` sweep eliminated, said out loud

A sweep that only reports hits cannot be audited. Run over the six files:

```bash
grep -rnE '\*\([[:blank:]]*(unsigned[[:blank:]]+)?long[[:blank:]]*\*\)' src/sys
grep -nE 'sizeof[[:blank:]]*\([[:blank:]]*(unsigned[[:blank:]]+)?(long|daddr_t|time_t|off_t)' ...
grep -nE '(^|[^_a-zA-Z])(unsigned[[:blank:]]+)?long[[:blank:]]+[a-z_]' ...
```

- The pointer-cast shape is a **singleton across the entire upstream kernel**,
  not just the six -- the three `nami.c` lines and nothing else in
  `v8/usr/sys/`.
- `sizeof(long)` and friends: **zero**.
- `alloc.c:416` is the word "long" in an English sentence in a comment. Not an
  instance -- the sweep matching prose, which this project has now been caught
  by three times.
- `bio.c:62-66` -- `nread nreada ncache nwrite bufcount[64]` -- are statistics
  counters with no on-disk or ABI contact. Wider is harmless.
- `rdwri.c:85` and `:179`, `if ((long)bn<0)`, are **correct**: `bn` is
  `daddr_t` (`rdwri.c:33`, `:133`), and this port narrowed `daddr_t` to signed
  `int`, so the cast sign-extends and the hole test still fires. It would be
  wrong if `daddr_t` were unsigned, which is the one thing to re-check if
  §4a's narrowing is ever revisited.
- `nami.c:183`'s `extern long cdevpath` is **dead**: both it and its only use
  (`:410`) are inside `#ifdef CHAOS` (`:182-184`, `:408-422`), and `CHAOS` is
  not defined here. So it is neither a hazard nor an external name to supply
  -- which a `long` sweep flags and a reader has to go and check.

### What the six actually need, compiled rather than surveyed

The survey costed step 5 by counting external names. That is the right first
measure and it is not the last one: a name the linker will want is not the
same set as a name the *compiler* will want, and only the second stops a
build. So the six were probe-compiled with `$(KERNFLAGS) -DKERNEL -fcommon`
before any shim code was written.

**Two instrument faults first, because both produced a confident wrong
number and one of them is in this project's own memory.**

- `clang $KF -c ...` under **zsh does not word-split `$KF`**, so the whole
  flag string arrived as one argument to `-std=`. Every one of the six
  reported *"1 error"* -- a uniform, plausible, entirely fictional result.
  Re-run under `sh`. This is the identical fault as passing a
  newline-separated symbol list unquoted to a probe, which is already
  written down; **one instance recorded is one instance fixed.**
- clang's **default `-ferror-limit=20`**. Five of the six then reported
  exactly 19 or 20, which reads like a shared cause and is an artefact of
  the cap. `-ferror-limit=0` turned 20 into 45 for `alloc.c` and 49 for
  `nami.c`. A number that clusters at a round value is a number to distrust.

Measured properly: **231 errors across the six.** `struct buf` incomplete
accounted for 39 of them directly, plus 18 more as implicit-`int` returns
being assigned to `struct buf *` -- because the declarations that fix those
live in `h/buf.h` under `#ifdef KERNEL`, and the `buf.h` in scope was the
30-line stand-in.

**So the authentic `h/buf.h` is imported, and the stand-in's retirement is
verified rather than assumed.** The survey predicted this and was right on
both counts: the file has **zero** VAX references, and its own header comment
-- *"there is no buffer cache here and no disk driver, so importing it would
put a description of hardware in the tree to obtain two constants"* -- was a
prediction that step 5 falsifies.

Verifying it needed care, and the shape is one this file has recorded twice.
`src/sys/h/buf.h` is a **new** file that now wins the quoted include for
`stream.c`, `streamio.c` and `ttyld.c` -- so it appears in no `.d` file, make
has no reason to rebuild those three, and a plain `make` reports success
having compiled nothing. **A stale object does not look like a build
problem.** The three sources were touched and rebuilt explicitly: all clean.

231 -> **164**. What is left is a precise shopping list rather than a risk,
and it is four things:

| | count | where it goes |
|---|---|---|
| `fstypsw` (13), `cdevsw`, `nfstyp` | 16 | the filesystem switch, into `shim/kern/h/conf.h` |
| `ROOTINO NICINOD NICFREE CANBSIZ NMOUNT PINOD SUPERB NODEV MSWAPX BUFSIZE` | 43 | geometry and tunables, into `shim/kern/h/param.h` at upstream's values |
| ten `struct user` members | 43 | `shim/kern/h/user.h` |
| `proc`, `pidhash`, `p_idhash`; `swblk_t`, `time_t`, `SIGXFSZ` | ~12 | `shim/kern/h/proc.h`, and the `time` collision below |

**AND THE `struct user` ROW IS THE SURVEY'S THIRD CORRECTION.** It said
*"`shim/kern/h/user.h` has no `u_dbuf` or `u_dent`, both of which `nami.c`
needs"*. Measured, the six want **ten** members that are not there:
`u_acflag u_cdir u_cmask u_dbuf u_dent u_dirp u_limit u_nbadio u_rdir u_vm`.
The survey found the two that `nami.c` needs because it was reading `nami.c`;
`alloc.c`, `iget.c` and `rdwri.c` want the other eight. Same shape as the
`atol(argv[1])` loop that existed three times and was fixed once: **a finding
filed under the file it was found in is not filed under its shape.**

The remaining `v8_i16`/`v8_u16`/`v8_i32` errors were **ours**, not upstream's
-- introduced an hour earlier by the three forwarding headers, which point at
`src/include/sys/` copies spelled in this port's width names. Closed by
declaring the four in `shim/kern/h/param.h`, with `tests/streams` comparing
their `sizeof` against `src/include/sys/types.h` rather than against a number
typed into a comment, because that is now the same name declared in two
files.

### The nineteen names are TWENTY, and five of them are not C functions

The survey listed 19 external names to be written in `shim/kern/`. A
subagent was set to specify each one against upstream; its central claims
were then re-read at source rather than taken on report, which is this
project's rule and which the report itself invited by citing everything.
Six were checked and all six held. The findings that change the plan:

**`plock` is a twentieth name, and the reason it was missed is a header.**
`nami.c` calls it three times (`:74`, `:389`, `:433` after this port's
comment; `:74`, `:355`, `:399` upstream) and upstream defines it at
`sys/pipe.c:105-115`. It was invisible to the survey because `h/inline.h`
defines it as a **macro** under `#ifndef UNFAST` (`:6`, `:8`) -- and
`iget.c:13` includes `inline.h` while **`nami.c` does not**. So the same
name is a macro in one of the six and a real call in another, and a survey
reading `iget.c` first sees no symbol. It appears **zero** times in PLAN.md
and zero times in this file. (`prele` is a twenty-first the day anyone
defines `UNFAST`, which nothing does.)

**Five of the twenty are inlined by `sed` before the assembler sees them.**
`sys/asm.sed` is a peephole pass in the kernel build (`conf/makefile:103`,
`${C2} ioconf.s | sed -f ../sys/asm.sed | ${AS}`) that rewrites
`calls $N,_name` into VAX instructions. `fubyte`, `fuibyte`, `subyte`,
`suibyte` and `spl0` are never C calls on a real V8, and `fuibyte` **has no
definition anywhere in the archive** -- it exists only as `asm.sed:36`,
which sends it to the same `jsb _Fubyte` as `fubyte`. Two consequences the
shim must honour, both measured:

- **`fubyte` zero-extends.** `locore.s:773-777` is `prober` / `beql fserr` /
  **`movzbl (r0),r0`** / `rsb`. So it returns 0..255, and failure is `-1`.
  A sign-extending implementation turns byte `0xFF` into `-1`, and
  `subr.c:188`'s `if((c = ...) < 0)` reads that as EFAULT -- a write that
  silently truncates at the first high byte. Same class as `sed -n l`'s
  `trans[*p1]`, on the return side instead of the index.
- **`subyte` must return exactly 0 on success, because of a precedence
  quirk in upstream.** `subr.c:162` is
  `if(id?suibyte(u.u_base, c):subyte(u.u_base, c) < 0)`, and `?:` binds
  looser than `<`, so it parses `id ? suibyte(...) : (subyte(...) < 0)`.
  For `id != 0` the condition is the **raw return value**. Upstream is
  correct because that value is 0 or -1; a shim returning 1 for success
  gives a spurious EFAULT on every `passc` in kernel-space mode. Do not
  reparenthesise it -- the convention is what to preserve.

**`mfind` must be declared returning `struct cmap *`.** The survey's claim
that returning NULL is *correct* rather than a stub holds -- `vmmem.c:418`
is upstream saying so -- but the width analysis inverts: `h/cmap.h:36`
declares `struct cmap *mfind();` inside `#ifdef KERNEL` (`:32`), and
`rdwri.c:10` includes it. **The prototype is in scope**, so the pointer
survives and an `int mfind()` in the shim would conflict with the header its
caller reads. `pte.h` carries the same shape for `vtopte` and is the one
header `bio.c` needs that exists in neither `src/sys/h/` nor
`shim/kern/h/` -- so whoever writes it must carry that declaration, or a
pointer is truncated in dead code that comes alive the day a raw device
lands.

**Four names collide at link time, and this is the largest unrecorded item.**
Measured with `nm -g` against the built archives rather than by reading:

| name | ours | theirs | why it matters |
|---|---|---|---|
| `access` | `alloc`'s caller -- `fio.c:174` defines the kernel one | **`libv8stubs.a`**, the userland `access(2)` | signatures differ AND the returns are inverted: kernel is **0 permitted / 1 denied**, access(2) is 0/-1. A link resolving the wrong way gives a `namei` that mis-decides permission |
| `free` | `alloc.c:205` `free(dev, bno)` | **`libv8c.a`** `free(ap)` | a block allocator handed a heap pointer |
| `sleep` | the kernel primitive, `sleep(chan, pri)` | **`libv8c.a`** `sleep(unsigned)` | a program calling `sleep(3)` would `tsleep` on channel 1 |
| `ialloc` | `alloc.c:281` `ialloc(dev)` | **`libv8c.a`**/`malloc.o` | pure intra-port; not a libSystem name at all |

That is the shape `PLAN.md`'s `min`/`max` note found and the pairwise
archive-overlap guard in `tests/kmemu` was written for -- and note it is
`nm -g`, not `nm -u`, that sees any of it, for the reason that guard
records.

**And two signal names are missing from `shim/kern/h/param.h`**: `SIGKILL`
(`bio.c:628`) and `SIGXFSZ` (`rdwri.c:166`). That file's own rule is to keep
the list minimal rather than complete, so both need adding *and* need the
`_Static_assert` in `shim/kern/sys/subr.c:51-58`.

**A footnote, because it is the funniest instance of a class this file
collects.** Sweeping the six with `grep -oE 'SIG[A-Z]+'` returns a fifth hit,
`SIGNAL` at `alloc.c:190` -- the word in an English comment. It also returns
two hits inside `shim/kern/h/param.h` itself, and one of them is `:231`,
which reads *"`grep -oE SIG[A-Z]+' over it yields exactly those two plus the
word SIGNAL"*. **The sweep matched a comment warning that the sweep matches
comments.** The rule was already written down; running it again still
produced the noise it predicts.

## The build: nine link collisions, fourteen phantom functions, and a second NOLONG

§8a step 5's six files now compile and link. `libv8kern.a` has seven new
members and imports exactly `_longjmp _memcpy _setjmp` — V8's own three.
`tests/streams` went 247 → 262 and `tests/kmemu` 133 → 136.

The import was the easy half and it was done in the previous session. This is
what the *build* found, and almost none of it was in the survey.

### alloc.c is the SECOND deviation, and it arrived as a warning

The import landed with one recorded deviation, `nami.c`'s hand-unrolled name
compare, whose cause is one line of Bell Labs' compiler: `# define NOLONG`,
"map longs to ints" (`cmd/ccom/vax/macdefs.h:20`). A VAX `long` was 32 bits.

`alloc.c` has the same bug and it is worse, because it did not stop the build.
`alloc.c:34` is `register long *p`, and `p` walks `s_bfree` — the superblock's
**free-block bit map**, which upstream declares `long S_bfree[BITMAP]`
(`h/filsys.h:31`) and which §8a step 4a narrowed to `v8_i32 S_bfree[961]`
(`src/include/sys/filsys.h:47`) precisely because a VAX wrote four bytes per
word there. The array narrowed; the pointer that walks it did not.

Four uses, wrong three different ways:

| site | what it does | what goes wrong |
|---|---|---|
| `:70`, `:96` | `*p &= ~(1 << (j&31))` | an 8-byte read-modify-write on a 4-byte word, so clearing one block's bit rewrites the *next* 32 blocks' word |
| `:83` | `for(i = 0; i < BITMAP && !*p; i++, p++)` | strides **eight** bytes for BITMAP iterations — scans half the map, then runs 961 words past the end of the superblock buffer |
| `:89` | `*p & (1 << j)`, `j < 32` | reads the right bits of an 8-byte load: right by accident, and only here |

Upstream states the assumption five lines below the declaration, and that
comment is the tell — `:88` is `for(j = 0; j < 32; j++)` with the trailing
comment `BITS PER LONG`. Bell Labs wrote down that a long is 32 bits, in a
comment, next to the loop that depends on it.

Three things generalise:

- **The loud one and the quiet one are the same bug.** `nami.c`'s deviation
  made `namei()` return ENOENT for every path — nothing worked. `alloc.c`'s is
  two `-Wincompatible-pointer-types` warnings in a build that *succeeded*, and
  it would have corrupted a free-block map on the first write and been blamed
  on `mkfs`. The survey found neither; the compiler found both, and only
  because one of them was an error.
- **The guard is shaped to the deviation, not copied from the other one.**
  `nami.c` lost three lines and gained three casts; `alloc.c` loses exactly one
  declaration and gains one. Counting removals and additions separately is what
  lets the two be different.
- **The guard asserts what the deviation DEPENDS on.** If
  `src/include/sys/filsys.h` ever widened `S_bfree` back to `long`, this
  deviation would become the bug instead of the fix — so `tests/streams` checks
  the *narrowed field* as well as the changed line.

### Nine link collisions, and the three nobody predicted are one class

The task was costed at four. `nm -g` over the archives found seven; extending
`tests/kmemu`'s cross-archive sweep to the archives it had never opened
found two more.

| name | the other definition | the kernel's |
|---|---|---|
| `free` | `libv8c.a(malloc.o)` | `alloc.c:205` frees a disk block |
| `ialloc` | `libv8c.a(malloc.o)` | `alloc.c:281` allocates an inode |
| `min`, `max` | `libv8c.a(min.o, max.o)` | `rdwri.c:250, :236` |
| `sleep` | `libv8c.a(sleep.o)` | the kernel's, `v8fs.c` |
| `access` | `libv8stubs.a(access.o)` | inverted polarity: 0 = permitted |
| **`time`** | `libv8stubs.a(time.o)` — a **function** | `systm.h:12` — a **variable** |
| **`timezone`** | `libv8c.a(timezone.o)` — a **function** | `systm.h:7` — a **variable** |
| **`mount`** | `libv8stubs.a(mount.o)` — a **function** | `mount.h:21` — an **array** |

**Six are function-against-function and three are variable-against-function,
and that split is the whole reason the count kept moving.** A duplicate
function is a duplicate-symbol error — the linker refuses and you find it the
first time you link. A **common** symbol resolving against a text definition is
what a linker is *supposed* to do, so it happens in silence. Every one of the
three nobody predicted is in that class:

- the kernel's clock variable would have become the address of `time()`, and
  `iget.c:276`'s `dp->di_ctime = time` would write a code address into an inode
  as a timestamp — no diagnostic, wrong dates on disk;
- `timezone` is not even the same idea twice: `int timezone` (minutes west) against
  `char *timezone(zone, dst)` (the zone's *name*);
- `findmount()` would have walked the text segment.

All nine get the `psignal` treatment — a macro in `shim/kern/h/param.h`
renaming both definition and calls, so Bell Labs' source keeps its spelling.

Two things came out of it for the test suite. **`tests/kmemu`'s pairwise sweep
was reading three archives and the build makes five** — and the first
correction to that sentence said *four*, which is the more useful half of the
story. The archive that mattered most was `libv8stubs.a`, holding the syscall
stubs, i.e. exactly the names a kernel is most likely to also define: it
structurally could not have reported `access` or `time`. The one the correction
itself then missed was `libkmemu.a`, and adding it found `kmemu_procfs` and
`kmemu_synth` duplicated against `libv8sys.a` — deliberately, by an arrangement
`shim/v8sys/noprocfs.c:10` documents (it quotes the duplicate-symbol error that
forced it) and which nothing had ever asserted. `libm.a` is the sixth and is
excluded on purpose: one member, one symbol named `_________`, V8's own empty
math library reproduced.

**So a fix to a population bug is itself a population claim.** The crash probe
learned this exactly once already — the fix that added `/etc` and
`/usr/lib/refer` to its scan stopped one directory short — and the answer both
times is to *derive* the population (`find build/stage0 -name '*.a'`) rather
than extend a list by hand. And extending it turned `DUPOK` into a **three-way** distinction
rather than a list: `T`-against-`T` is a real collision, `C`-against-`T` is the
silent class, and `C`-against-`C` is deliberate sharing — only `errno`, which
*must* be one object so that a syscall stub and `perror()` agree.

### Fourteen macros compiled as calls to undefined functions

`shim/kern/h/param.h` gained the three CLSIZE==2 constants and stopped, on the
reasoning that a header owes constants and the macros would follow if anything
wanted them. Everything compiled. Everything was wrong.

`KERNFLAGS` carries `-Wno-implicit-function-declaration`, because the imported
half is 1985 K&R and the diagnostic would fire on every line. So `BSIZE(dev)`
with no macro in scope is not an error and not a warning — it is a **call to an
undefined function named BSIZE**, left as an undefined symbol in the object.
Fourteen of them: `BITFS BMASK BSHIFT BSIZE INOPB MIN NINDIR NMASK NSHIFT
dbtofsb fsbtodb itod itoo major`.

Found by subtracting what `libv8kern.a` defines from what it undefines — the
same instrument `tests/streams` already uses for libc leaks, and **the only one
that could see this, because the compiler had been told by us not to speak.**

That is the sharpest instance yet of a rule this tree already states:
**a suppression argued for once covers code nobody argued about.**
`-Wno-implicit-function-declaration` was argued for K&R *declarations*, and it
silently also covers a missing *macro* — which is a different thing, because a
missing declaration changes what the compiler checks and a missing macro
changes what the code **means**.

### dev/conf.c is vestigial, and Bell Labs say so

The obvious source for `fstypsw[]` is `dev/conf.c:602-611`, four rows and
`nfstyp = 4`. Row 0 is `{ 0,…,0, rnami, smount, 0}` — and **`rnami` is not
defined anywhere in the V8 kernel.** The only three occurrences of the name are
that row, the `extern int rnami()` above it, and a comment at `sys/nami.c:167`
reading *"USED TO BE rnami"*, immediately above the definition of `fsnami`.

`conf/config_diff:11` explains it in Bell Labs' own words — *"dev/conf.c is no
more. config makes a conf.c for each machine"* — and `:13-14` lists the files
"changed a little to make names regular" when that happened. `nami.c` is on the
list.

So every citation to `dev/conf.c` is a citation to dead code, and this port has
met the shape before: V7's `syopen` driver still sits in `sys/sys/sys.c`, dead
and uncompilable, and CLAUDE.md already calls a vestigial file that answers your
question the worst kind of evidence. `dev/param.c` is a third instance.

**The live source is `conf/devices`**, which `config_diff:20-21` names as
config(8)'s input and which this port already cites for the tty line discipline
and for `/dev/tty`. Its lines 70-73 are the filesystem handlers:

```
file-system 0	fs  fs  nami mount
file-system 1	na  na  put get free updat read write trunc stat nami mount
file-system 2	pr  pr  … ioctl
file-system 3	mp  mp  … ioctl
```

Type 0's prefix is `fs`, its two members are `fsnami` and `fsmount` — and
`fsnami` is exactly what `src/sys/sys/nami.c:202` defines. `nfstyp` is **1**
here, not 4: types 1–3 are `neta.c`, Killian's `proca.c` and `mp.c`, none
imported. Keeping four rows with a null `t_nami` would turn `nami.c:78`'s
`panic("namei nfstyp")` — a guard against a corrupt `i_fstyp` — into a null
call one line later.

### The twenty services, and the two the survey got wrong

`shim/kern/sys/v8fs.c` implements nineteen; `spl0` is the twentieth and lives in
`machdep.c`, because `splevel` is static there and lowering the level must go
through `splx()` so a deferred `queuerun()` actually runs.

Three kinds, and which kind a name gets is a claim about this port: **REAL**
(thirteen), **ANSWER** (`mfind`, `xrele` — the value V8's own code would produce
here), **PANIC** (five, all VAX virtual memory). A panic is a better stub than a
zero: this port has already counted 42 of `primes`' garbage exit statuses as
signal deaths, and read a qopen's `-1` as an inode pointer of `0xffffffff`.

Two contracts are load-bearing in ways a plausible stub would break:

- **`fubyte` zero-extends** (`locore.s:776`, `movzbl`), and a sign-extending
  version breaks *both* consumers, differently. `subr.c:188` tests `< 0`, so
  every byte ≥ 0x80 would read as EFAULT; `nami.c:571` tests `== -1` **exactly**,
  so byte 0xFF alone would — in a pathname.
- **`subyte` returns exactly 0**, because `subr.c:162` is
  `if(id?suibyte(u.u_base,c):subyte(u.u_base,c) < 0)` and `?:` binds looser than
  `<`. On the I-space arm the **raw** return value is the truth value, so any
  nonzero success return sets EFAULT on every successful store down that arm —
  and only that arm.

And `plock` is the twentieth name the survey missed, for a reason worth keeping:
it is a **macro** in `h/inline.h` and a **function** in `pipe.c:105`. `iget.c`
includes `inline.h` and never calls it; `nami.c` does not include it and calls
it three times. So the macro is dead here and the out-of-line function is
load-bearing — the exact opposite of what reading `inline.h` first suggests.

### Two of three "deliberately minimal" header designs were falsified by the build

`shim/kern/h/pte.h` left `struct pte` **incomplete on purpose**, arguing that a
pointer can be returned and compared while any field access becomes a compile
error naming the file. `bio.c:557` reads `pg_pfnum` and `pg_fod`.

`shim/kern/h/vmparam.h` was written as **intentionally empty**, having checked
what the six files "are about". `bio.c:553` needs `KLMAX`, which is
`vmparam.h:77`. `vmmac.h` claimed one macro was reachable; `bio.c:554` needed a
second.

All three were reasoned from a survey and refuted within the minute by a
compile. That is the same failure at a smaller scale as the header estimate this
step opened with — **a survey of what a file is *about* undercounts what it
*references*, every time** — and the corrections are left in the files rather
than smoothed away, because the reasoning was the good kind and still wrong.

## §8a step 5c: it RUNS — a file read back through Bell Labs' own path

Everything above this heading is a statement about a build. Hashes, diff
shapes, what the archive imports, which header a quoted include resolves to —
not one of them would have changed if `alloc.c`, `iget.c`, `nami.c`, `rdwri.c`,
`subr.c` and `bio.c` had never executed an instruction. **They never had.**

They do now. `mkfs(8)` writes a 2000-block image containing a 28000-byte file
two directory levels down; V8's kernel opens it by name and hands back the
bytes:

```
namei -> fsnami -> dsearch -> bread -> the driver
      -> iget  -> bread -> iexpand
      -> readi -> bmap  -> bread
```

`cmp` says the 28000 bytes are identical to the file mkfs was given. The writer
is 1985 code compiled here; the reader is different 1985 code compiled here;
neither knows about the other. `tests/streams` went 262 → 315.

### What had to exist first, and where each piece went

| what | where | why there |
|---|---|---|
| a block driver | `tests/streams/fsprobe.c` | nothing in the port consumes one, so the shim would hold a component with no caller |
| `bdevsw[]`, `nblkdev`, `v8k_bdconf()` | `shim/kern/sys/ioconf.c` | that file is named for `conf/ioconf.c`, which is where `config(8)` emits exactly these tables |
| `binit`, `iinit`, the buffer and inode tables | `shim/kern/sys/main.c` (new) | `sys/main.c` is where the startup lives and this port has no `main()` |
| `allocmount()` | `shim/kern/sys/v8fs.c` | beside `findmount`, its sibling from `sys3.c` |

**`main.c` stands in for three upstream files and two of them describe a machine
that does not exist.** `sys/param.c` holds the size formulae — and is compiled
`-DMAXUSERS=xx`, a number **not in the shipped tree** (`grep -rn MAXUSERS conf/`
finds nothing), so every formula there is unevaluable. That is the `NTTY`
situation exactly: a per-configuration constant `config(8)` derived from a
machine description Bell Labs did not ship, which makes it a layer-2 decision
under the rule *derive it, do not pick it*. `sys/machdep.c` holds the storage,
carved out of the VAX's kernel virtual address space by `valloc` at `:102-121`
with `nbuf` computed from `physmem` at `:81-84`. `sys/main.c` holds the code.

- **`NINODE` is 80, derived from Bell Labs' own two formulae.** `param.c:29-30`
  sizes the inode and file tables off one quantity, and although the quantity is
  unknown the **ratio survives**: `ninode - 32 = 3/2 * (nfile - 32)`. This port
  fixed `NFILE` at 64 long ago (`shim/kern/sys/fio.c:90`), so the number follows
  and moves only if `NFILE` moves. Do **not** substitute this port's `NPROC`
  into the formula — that 4 is because slot 0 is `pfind`'s chain terminator and
  slot 2 is the pagedaemon's address, which has nothing to do with process
  count. Mixing them would be numerology.
- **`NBUF` is 32, which is upstream's own floor** (`machdep.c:83-84`), because
  the formula above it needs `physmem` in VAX pages. Feeding it this host's
  memory would size a cache for a machine that is neither.

### Two omissions from `binit` that are required rather than tidy

Upstream's `binit` ends with two tails this port must not transcribe:

- **the `bdevsw` counting loop** (`main.c:218-219`) derives `nblkdev` from the
  generated table. Here `v8k_bdconf()` maintains it as drivers register, so
  running the loop as well would count each driver **twice** and put `nblkdev`
  past the populated prefix — creating precisely the hole `ioconf.c` argues
  nothing may create.
- **the swap tail**, which contains `if (nswdev == 0) panic("binit")`. A
  faithful transcription would abort every run. This is V8's buffer cache
  without V8's pager, which is what §8a step 5 imported.

### `bdevsw` needs the same dense-prefix invariant `streamtab` does

`bio.c:352` range-checks a major number, but **five** `d_strategy` calls and
`iinit`'s `d_open` dereference the slot with no null check. So a hole below
`nblkdev` is a null call — the FIOLOOKLD shape again. `v8k_bdconf()` appends and
refuses a row with a null `d_open` or `d_strategy`, and **that rule is
upstream's own**: `main.c:218` ends the table on a null `d_open`, so every row
`config(8)` emitted had one.

### `shim/kern/h/buf.h` died without being edited

It existed to give `streamio.c` `B_READ` and `B_WRITE` without importing 107
lines about a VAX buffer cache, and its header comment said so. Then **bio.c's
import brought the authentic `src/sys/h/buf.h` into the tree**, and because a
quoted include tries the includer's directory first, `streamio.c:4`'s unchanged
`#include "../h/buf.h"` silently started resolving to the authentic header
instead. Measured with `clang -M`; the source line is identical either way. Its
two remaining includers used neither constant, measured too.

`tests/deps` had a case named **`our buf.h -> streamio.o`** pointing at the dead
file. It stayed green throughout, because the *make* edge was real — the
Makefile listed our header as a prerequisite — while the header named in the
case was no longer the one the compile opens. A test whose label and whose check
name different files audits nothing, and here neither the label nor the check
was edited: **a third file arriving made both wrong.**

### The mutation that did not fire, and the case it bought

Seven mutations were run against the new guards and six went red. The seventh
shrank `NINODE` from 80 to 3 and changed nothing — **308 passed either way**. So
did 2. Only `NINODE 1` failed. The read path needs exactly **two** inode slots,
because `rootdir` and `u_cdir` are two `iget`s of the same `(dev, ROOTINO)` and
the second gets the same structure with `i_count` 2 rather than a second slot,
while `fsnami` releases each parent as it descends.

The right reading is not "the mutation was too weak". It is that **no test
constrains the table's size**, which means the derivation above is the only
justification there is — correct for a configuration constant, and now said out
loud rather than implied. What the mutation bought is a better case: the probe
counts inodes still held when it finishes, which catches a **missing `iput`** —
a real bug class that no table size would ever have exposed. Verified by
mutation: dropping one `iput` gives `root-count-final 3` where 2 is required.

And a second reading, from the mutation that broke `bmap`'s indirect arm: the
three scalar checks (`bmap-0-valid`, `bmap-10-valid`, `bmap-differs`) **all
passed** under it, because `i_addr[10]` holds the indirect block's own address —
valid, and different from block 0. Only reading the bytes caught it. That is why
the central claim is a `cmp` against the file mkfs was handed and not a checksum
this port invented.

### Bell Labs' comment was stale against Bell Labs' code

`v8fs.c` recorded that `getfs()` "panics with `no fs`". The source says
`panic("getfs")`. **"no fs" is not invented** — it is upstream's own words at
`alloc.c:414`, in the comment block twelve lines above the code that contradicts
it. This port read the comment and wrote it down as the behaviour.

CLAUDE.md already holds the rule that a recorded diagnosis is a hypothesis until
re-measured. What is new is that it applies to **the imported half's own
comments**, which is the one place the fidelity contract guarantees nobody will
have checked them: we are forbidden from editing them, so they are never read
with a critical eye.

### A line citation inside the file it cites is self-invalidating

The `alloc.c` PORT comment names the four uses of `p` by line. Writing that
comment pushed every one of them down by 43, so the first draft's `:70` pointed
**into the middle of the comment doing the citing**. A subagent audit of the
whole tree found **sixteen** stale citations of this shape, eight caused by that
one comment, plus five description errors where the sentence was wrong
independently of the number.

Correcting it moved them **twice more**, because the correction added lines, and
only the third measurement converged. So the citations are now a **test**
(`tests/streams`, five cases) rather than prose — the same move the deviation
list above already makes — and each is written `ours (upstream)`, the form
`PORTING.md:1186` had already got right for a different file.

### `_memcpy` in the import set was correct, and now it is explained

The task list asked whether tolerating `_memcpy` was deliberate or coincidence.
Measured: **three** archive members import it — `stream.o`, `ioconf.o` (since
this step, from `bdevsw[nblkdev] = *bd`) and **`iget.o`**, which needs it for
`itmp = *ip` at `iget.c:314`. The `bcopy -> v8k_bcopy` redirect does not cover a
compiler-generated structure copy. It is safe anyway, and for a reason the
aggregate guard already asserts rather than one that depends on the list: every
external import resolves to **libv8c**, so the memcpy is V8's own.
