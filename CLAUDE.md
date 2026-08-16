# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Research Unix V8 (Bell Labs, 1985) userspace, rebuilt from source on macOS/ARM64 —
not emulated. The authentic Bell Labs C compiler compiles the authentic V8 C
library and the authentic V8 programs, on hardware that did not exist in 1985.

`PLAN.md` is the specification: fidelity contract, target model, phase
breakdown, and the running record of what has been learned. Read §1 (fidelity
contract) and §4a (bootstrap ladder) before making architectural decisions.

## The repo holds a SERIES of ports, and paths below are release-relative

V8 is a rung, not the destination — this repository is named for V10 — so the
tree is split by what varies per release and what does not:

```
v8/          this release: src/ shim/ compiler/ tests/ Makefile, and its own
             build/ and rootfs/.  v9/ and v10/ become siblings.
third_party/ the vendored upstreams, read-only, versioned inside themselves
tools/       import.sh and friends, shared
.claude/     hooks, agents, skills, shared
*.md         the prose, shared
```

**Every path in this file is relative to `v8/` unless it begins with
`third_party/`, `tools/`, `.claude/` or `.github/`.** So `src/cmd/cat.c` means
`v8/src/cmd/cat.c`. This convention exists because spelling `v8/` several
hundred times would bury the content, and because the same sentences will be
true of `v9/` when it lands.

The split is by **what actually varies**, and the compiler is the case that
shows why it is not simply "one directory per release". `compiler/ccom-arm64/`
is about arm64, Mach-O and AAPCS64 — not about V8 — and `shim/kern/` and
`shim/libkmemu/` are about macOS. A V9 tree inherits their *content* even
though it gets its own copy. That is the same authentic-versus-machine-dependent
line `src/sys/h/` and `shim/kern/h/` already draw one level down.

## Commands

```bash
make -j8              # full build (~4s clean) -- dispatches to v8/
make test             # all 17 suites (2225 cases, 2224 on a host whose $TMPDIR
                      # holds under 2 or over 65535 entries -- see wavea's inode
                      # distinctness case).  NOT `make -j8 test': see below
make test-wavec       # one suite: deps jail selfhost cpp v8ccom v8cc v8sys freestanding
                      #            libv8c wavea waveb sh wavec kmemu streams mkfs hooks
v8/tests/deps/run.sh  # a suite directly (same thing, no build first)
make clean            # remove v8/build and v8/rootfs
```

The root `Makefile` builds nothing; it forwards to `$(CURRENT)`, which is `v8`.
`make -C v8 <target>` is the same thing said explicitly, and `make -j8` still
parallelises because the sub-make inherits the jobserver. Each release's
makefile derives its own root from where it sits, which is why moving the tree
under `v8/` changed no path inside it.

Building a single object or program requires an **absolute** path, because
`$(BUILD)` is absolute — and it must be asked of the release, not the root:

```bash
make -C v8 $(pwd)/v8/build/stage0/bin/cat
```

Test suites are shell scripts that print `name: N passed, M failed`. There is no
per-case filter; edit the script or run its commands by hand.

**There are two makes here, and confusing them is easy.** The `make` above is
GNU make, driving *our* top-level Makefile — the seed harness for rungs 0–3, and
the everyday command. `$V8ROOT/bin/make` is V8's own, and it is what reads *Bell
Labs'* makefiles for rungs 4 and 5. The distinction is the bootstrap claim, not a
preference: rung 4 is only meaningful if the build description is theirs. The
`v8-make.sh` hook refuses the host's make on any makefile `tools/import.sh`
brought in, because GNU make would run it perfectly well and nothing would say
the rung had not happened.

There is a **third** build tool coming, and it is not a spelling of these two.
`mk` is Andrew Hume's successor to make (1987), and it arrives with **V9** —
measured, not recalled: V9's README says "all the source and makefiles(mkfiles)",
and V10's kernel is full of them. So it is needed at the *first* upgrade step. V8 has no trace of one: no `mk`, no
`mk.1`, not one `mkfile`, and the only `mk.c` upstream belongs to `efl`. So `mk`
is a non-question today and a port rather than a rename when it lands. PLAN.md
§4a says what it will cost; `tests/hooks` fails the day the first `mkfile`
appears, because `v8-make.sh` would otherwise wave the whole new tree through
while still reporting success.

### Running V8 binaries

Nothing runs without `V8ROOT` — the shim resolves V8 paths inside it, and when
unset it silently falls back to the host filesystem.

```bash
export V8ROOT=$PWD/rootfs
$V8ROOT/bin/cc -o prog prog.c        # V8's compiler
V8JAIL=strict $V8ROOT/bin/make       # build with V8's make, refusing host escapes
V8DBG=1 $V8ROOT/bin/cc -c x.c        # type tracing from the ARM64 backend
```

`V8JAIL=warn` names each host binary reached; `strict` refuses it. Use `strict`
when the claim is "this ran entirely on V8 code".

## `src/sys/` — V8's kernel, and it plays by layer 1's rules

PLAN §8a step 1, and **both halves of the stream machinery are in**:
`src/sys/dev/stream.c`, Dennis Ritchie's engine, **byte-identical to
upstream** — `tests/streams` compares `git hash-object` against PROVENANCE, so
an edit is a test failure — and `src/sys/sys/streamio.c`, the 1093-line syscall
side, which carries **two recorded LP64 deviations** and therefore cannot be
guarded that way. The machine-dependent half is `shim/kern/`, in the same
relationship `compiler/ccom-arm64/` has to ccom.

**A FILE WITH DEVIATIONS NEEDS A DIFFERENT GUARD, AND "it has a PORTING.md"
IS NOT ONE.** `tests/streams` diffs `streamio.c` against `third_party/` and
asserts that upstream lost exactly one line, that it is the `sizeof(arg)`
copyout, and that the second deviation added exactly the declaration it was
supposed to. That makes the deviation *list* a test rather than prose, which is
what a hash gives you for free and a patched file otherwise loses entirely.
Count removals and additions separately — the two deviations here are not the
same shape, and a first draft that assumed "two changed lines" failed.

**AND IT COMPILES BY CLANG, SO v8cc's SAFETY NET IS NOT UNDER IT.** The
compiler widens undeclared K&R parameters on purpose (`acctype()` in
`compiler/ccom-arm64/gencode.c`) because the tree is full of them holding
pointers. Nothing in `src/sys/` gets that: it is host-clang code. `urcvfile`'s
missing `caddr_t arg` was found by the *build*, not by the survey, and it would
have been invisible in a program compiled by v8cc.

Three things about it generalise to the rest of `sys/`, so know them before
importing more:

- **Machine facts go in `shim/kern/h/`, never in `src/sys/h/`.** A quoted
  include tries the includer's directory first, so `"../h/stream.h"` from
  `src/sys/dev/` finds the authentic header and `"../h/param.h"` falls through
  to `-Ishim/kern/dev`. An authentic header always wins; ours fill gaps.
- **K&R gets a dialect flag, not an edit.** `$(KERNFLAGS)` is `-std=gnu89` with
  implicit-int and implicit-declaration off. That is how "do not modernise K&R
  declarations" is obeyed rather than worked around.
- **Prefer a redirection in the header to a deletion in the source.** `printf`,
  `bcopy` and `uballoc` are all `#define`d aside in `shim/kern/h/param.h`, which
  is what keeps the blob hash intact. A three-line deletion would have been
  easier and would have cost the strongest claim available.

**AND THE FIRST RULE HAS NOW PAID FOR A FILE THAT NEEDED A NUMBER V8 NEVER
SHIPPED.** `ttyld.c` and `partab.c` are in (the tty line discipline, line
discipline 0 — `conf/devices:75` — not a device), both **byte-identical** and
both hash-guarded. `ttyld.c:6` includes `"tty.h"`, which is not `h/tty.h` (that
is **zero bytes**, a make timestamp node at `conf/makefile:61-62`) but the
per-configuration header `config(8)` generates from a machine description that
was not shipped — `conf/files:98` marks the file `optional tty pseudo-device`,
and there is **no `#define NTTY` anywhere in `third_party/`**. So `NTTY` is a
layer-2 decision, it lives in `shim/kern/dev/tty.h`, and the quoted-include
fall-through delivers it with **no edit to Bell Labs' source** — which is
exactly what keeps the hash guard available. Derived, not picked: 128 =
`NSTREAM`, because a slot is one discipline *attached to a stream* and a
process cannot hold more streams than that. `src/sys/PORTING.md`.

**AND EXERCISING IT NEEDED A DRIVER RATHER THAN A MODULE, WHICH IS ONE LINE OF
SOURCE AND NOT A PREFERENCE.** `ttyldin` sends data **up** through `q->next`
and flow control **down** through `WR(q)->next`, in the same loop — so a second
module stacked above sees the first and can never see the second, and a
discipline with one end is a discipline that cannot be driven.
`tests/streams/ttyprobe.c` now builds the real thing the way `init.c:368-382`
does — `stopen` the driver, `v8k_stconf` the discipline, `FIOPUSHLD` between —
with `streamio.c` on top, `ttyld.c` in the middle, and **only the bottom layer
ours**. Streams went 140 → 236 cases. Three things generalise:

- **The driver is in the probe, not `shim/kern/`, and that is the rule rather
  than laziness.** Nothing in the port consumes a tty driver — `/dev/tty` is
  `/dev/fd/3` — so one in the shim would be a component with no caller. That
  is the **mirror** of this file's most repeated lesson: an unexercised rule
  cannot be seen to be incomplete, and an *unconsumed component invents a
  difference the kernel does not have*. `sioprobe.c`'s loopback and pipe
  drivers are the precedent.
- **A return value cannot distinguish two paths, so ask the far end.**
  `ttldioc`'s `TIOCSETP` passes the block down and the *driver's* ack wakes
  `stioctl`; its `TIOCSETC` is `qreply` at the discipline and the device never
  sees it. Both make `stioctl` return 0. The pair of cases asks the driver
  whether it saw anything, which is the only observable difference.
- **The failure mode of the thing under it is a HANG.** `stioctl` tsleeps on
  the acknowledgement for **fifteen seconds**; a driver that frees an
  `M_IOCTL` instead of acking stalls rather than failing. Measured by
  mutation: 30 failures and the probe killed by its alarm. `ttyprobe` runs
  under a deadline now, and the comment saying it needed none was true of the
  open path and stopped being true the moment a driver went under it.

**AND THE AUDITOR FOUND A LIVE BUG IN THE DRIVER THAT NO BEHAVIOURAL TEST COULD
SEE, WITH V8's OWN FIX SITTING IN TWO UPSTREAM FILES.** The length of an
acknowledgement is part of the acknowledgement: `stioctl` builds every
`M_IOCTL` with `wptr += sizeof(union stmsg)` — **20 bytes, whatever the
command** — `ttldioc`'s `TIOCSETP` arm does not touch `wptr`, and
`streamio.c:793-798` copies `wptr - rptr` back to the caller. So an ack passed
through unchanged writes **16 bytes into a 6-byte `struct sgttyb`**. Bell Labs
spend one line on it in each of two drivers — `dev/cons.c:56-58` resets `wptr`
for `TIOCSETP`/`TIOCSETN` and *falls through* to `TIOCGETP`, which must not
reset it; `dev/dz.c:229` likewise — and their default arms are `M_IOCNAK` with
no payload, which `streamio.c:803-809` turns into `ENOTTY`.

- **A VALUE SENTINEL CANNOT SEE THIS CLASS.** The ten bytes written are the ten
  `copyin` read from that address moments earlier, so the write round-trips and
  memory ends up correct. 216 cases were green throughout. The only observable
  is the fault.
- **So the guard is a page, and it must be `PROT_READ` rather than
  `PROT_NONE`** — the authentic 20-byte over-*read* has to keep succeeding, so
  that the only thing which can fail is the write. Put the object at
  `page_end - sizeof(object)`, run it in a child, assert the signal is 0.
  Measured: 0 with Bell Labs' line, **SIGBUS without it**. That is how a
  convention becomes a test instead of a comment.
- **The comment recorded the wrong conclusion.** It said "streamio.c copies
  `wptr - rptr` either way" — true, and the reason *to* shorten the ack,
  written as though it were the reason not to. Same shape as the recorded
  constraint that blocked the inode fix for months: **accurate, cited, and
  pointing the opposite way from what it concludes.**

**AND A SHORT READ LEAKS INTO THE NEXT CASE, WHICH IS THE CRASH-PROBE LESSON
ARRIVING INSIDE ONE PROCESS.** `tests/crash-probe.sh` learned that a prober
must be a pure function of the program and its arguments, because programs
sharing one directory read each other's litter. The same thing happens between
*cases in one probe* when they share a stream: `ttyprobe`'s TANDEM case sent
401 characters and read them into a 256-byte buffer, and the 145 left queued
were read by the case after it — which reported a plausible wrong answer and
made the case after **that** look broken. Two drafts of the `canonb` case were
written against those 145 bytes before anyone asked where they came from.
`readline()` now reads whole lines and every canonical read goes through it.
**A case has to be a pure function of what it sent.**

**AND THE THIRD INSTANCE IS BETWEEN TWO SECTIONS OF ONE SUITE, SHARING A FILE.**
§8a step 5e's 9P cases run at the bottom of `tests/streams`, against the image
`mkfs` wrote at the top — and `fsprobe` **writes** to that image in between,
because step 5d creates a file, grows it past the superblock's cached free list
and deletes it. The first run reported `hello`'s length as **10248** against
the 27 bytes `mkfs` put there. So the progression is: between programs sharing
a directory, between cases sharing a stream, and now between sections sharing
an artefact. The fix is the same each time — the 9P section gets `cp img p9img`
taken at the moment `mkfs` succeeds, not a later copy.

**And the case that should have caught it could not, because it asserted a
field the CLIENT supplied.** `stat-name` came back `hello` throughout: 9P's
stat carries the name out of the fid, which is the name the client sent in the
Twalk, so a server that walked to the wrong inode entirely still prints the
name asked for. The qid path is `i_number` and comes from the directory entry,
which is why it is the field the case now checks — against what `ncheck` says
independently. **Ask which end of the wire a field came from before asserting
on it.**

**AND MEASURE A MUTATION IN ISOLATION BEFORE BELIEVING ITS COUNT.** A batch
harness scored two of them at one failure; re-run alone, each produced two —
the right two. The guards were fine and the reading was not, which is the same
class as the crash probe reporting 254 before it reported 96: **an instrument
you wrote is a suspect**, and a batch runner is an instrument.

**AND THE EXPECTED VALUE WAS WRONG BECAUSE THE GUARD IS ONE LINE ABOVE THE
LOOP.** `outconv` expands a tab only when `(t_flags&TBDELAY)==XTABS`
(`ttyld.c:385`), and `ttyopen` sets `ECHO|CRMOD` — `XTABS` means *this terminal
cannot do tabs itself*, a fact about hardware, not a default. A case written
from the loop expected `a` + seven spaces and measured a literal tab. Same
shape as `min()` being found by writing `max()`: **the answer that surprises
you is the one to go and read the guard for**, and the fix is two cases rather
than one — the default terminal's literal tab, and the expansion with the flag
set.

**And writing `max()` found `min()` misdeclared in two places.** Both
`param.h` and `subr.c` said upstream's `min` has "no declared return type, so
`int`"; `rdwri.c:249` is the word `unsigned` on its own line. Nothing
observable changed — every call is bounded by a 1024-byte block, so bit 31 is
clear — which is precisely why the wrong note sat beside a working function for
months, and it would have been copied straight into `max`. **The way to find
this class is to add the sibling**: a second instance forces the declaration to
be read instead of recalled.

- **A stand-in kernel header that typedefs a name libc owns must CLAIM THE
  HOST'S GUARD, not hope about include order.** `shim/kern/h/param.h` has to
  spell `dev_t`, `ino_t` and `off_t`, because the authentic `inode.h`,
  `file.h` and `dir.h` are written in them — and Darwin owns all three, two at
  a different width (`ino_t` 2 vs 8, `dev_t` 2 vs 4). So which definition won
  would have depended on whether a file included `<stdio.h>` first, and two
  objects in one link could have disagreed about `struct inode`'s layout by
  twelve bytes with nothing to say so. It is the DIRSIZ trap arriving through
  a host header. Defining `_OFF_T`/`_INO_T`/`_DEV_T` makes the host's typedefs
  no-ops; an `#error` catches the file that includes param.h too late. Do NOT
  claim a guard for a name no struct here uses — `time_t` is deliberately left
  to the host, because there is no layout to protect and claiming it would give
  the shim's own raw syscalls a 32-bit `time_t`.

**AND IT ALL RUNS NOW, WHICH IS A DIFFERENT CLAIM FROM "IT BUILDS".** §8a step
5c: `mkfs(8)` writes an image, V8's kernel opens a file in it by name through
`namei → fsnami → dsearch → iget → bmap → readi → bread` and a block driver,
and `cmp` says the 28000 bytes match the file mkfs was given. The file is two
directories down and 28 blocks long, so the walk covers a subdirectory and
`bmap`'s **indirect** arm. The driver is in `tests/streams/fsprobe.c` and not in
`shim/kern/`, by the unconsumed-component rule; `v8k_bdconf()` in
`shim/kern/sys/ioconf.c` registers it, under the same dense-prefix invariant
`v8k_stconf` has, because `bio.c:352` range-checks a major number and then five
`d_strategy` sites dereference the slot unguarded. `shim/kern/sys/main.c` is new
and holds what `sys/main.c`, `sys/machdep.c` and `sys/param.c` hold upstream.
Four things generalise:

- **A HEADER CAN DIE WITHOUT BEING EDITED, and a make-edge test will not
  notice.** `shim/kern/h/buf.h` existed to give `streamio.c` two constants.
  `bio.c`'s import brought the authentic `src/sys/h/buf.h` into the tree, and
  because a quoted include tries the includer's directory first,
  `streamio.c:4`'s unchanged line silently started resolving to the authentic
  header. Nothing in either file changed; a **third** file arriving did it.
  `tests/deps` had a case `our buf.h -> streamio.o` that stayed green
  throughout, because the *make* edge was real while the header named in the
  case was no longer the one the compile opens. Measure with `clang -M`, which
  is the only instrument that can see it — the `#include` line is identical
  either way. Both its remaining includers used neither constant; it is deleted.
- **BELL LABS' OWN COMMENTS GO STALE TOO, and the fidelity contract guarantees
  nobody checks them.** `v8fs.c` recorded that `getfs()` "panics with `no fs`".
  The code says `panic("getfs")`. "no fs" is upstream's own words at
  `alloc.c:414`, in the comment block **twelve lines above** the code
  contradicting it, and this port copied the comment down as behaviour. The
  recorded-diagnosis rule applies to imported prose, and imported prose is the
  one thing we are forbidden to edit and therefore never read critically.
- **A LINE CITATION INSIDE THE FILE IT CITES IS SELF-INVALIDATING.** Writing
  `alloc.c`'s PORT comment pushed the four lines it cites down by 43, so `:70`
  pointed into the middle of the comment doing the citing. A subagent audit
  found **sixteen** stale citations tree-wide, eight from that one comment, plus
  five places where the *description* was wrong independently of the number.
  Correcting it moved them twice more and only the third measurement converged.
  So they are a **test** now (`tests/streams`, five cases, mutation-verified),
  and each is written `ours (upstream)` — the form `src/sys/PORTING.md:1186` had
  already got right for a different file.

  **AND "THEY ARE A TEST NOW" WAS TRUE OF FIVE OF THEM, OUT OF ELEVEN HUNDRED.**
  Measured: **1132** citations in the tree, and the five cases covered one PORT
  comment in one file. Sweeping the rest found **nineteen stale**, of which
  **five were corrections made the previous day** — the same commit that fixed
  do_walk's ENOTDIR citation in `v8fsd.c` wrote line 1122 for an arm that had
  already moved to 1124, so the fix shipped wrong and the entry above's "only
  the third measurement converged" repeated itself exactly.
  `tests/streams/cites.awk` is the sweep. Five things generalise, and the last
  is about this paragraph:

  - **THE OBVIOUS GENERALISATION IS A 26% FALSE-POSITIVE RATE, MEASURED.** Pull
    an identifier out of the citing sentence and require it at the cited line —
    and `icheck.PORTING.md` cites `icheck.c:278` from a table row that also says
    "dcheck", which no window or adjacency rule separates from a claim. What is
    checkable without guessing what a citation *means* is that **nobody cites a
    blank line, a `*/`, or a bare `break;`**. Nineteen findings, zero false
    positives, each hand-verified.
  - **SAY WHAT IT CANNOT SEE.** A citation that drifts onto plausible *code* is
    invisible to it — the pre-correction number above had become
    `x[0].d_ino = ip->i_number;` and only reading it caught that. Measured as a
    non-firing mutation rather than claimed, so the blind spot is a fact about
    the guard and not a hope.
  - **THE EXCLUDED COUNT IS PRINTED, because 846 of the 1132 are excluded.** A
    bare `param.h` names ours *and* Bell Labs' — the `ours (upstream)` ambiguity
    above, from the other side — and a resolver that guesses puts the instrument
    back in the business of being wrong. The way to check those is to write the
    directory into the citation, not to widen the sweep.
  - **AND THE VACUITY GUARD IS THE ONE THAT MATTERS, because the stale case
    stays GREEN when the sweep dies.** Measured: break the resolver and it
    reports 0 stale over 0 checked, which reads as a clean tree. Same shape as
    `tests/cpp`'s `if [ -d "$V8INC" ]` skip reporting `12 passed`.
  - **AND THE INPUT IT COULD NOT SURVIVE IS THE ONE IT EXISTS FOR: A FILE THAT
    CITES ITSELF.** The heading this whole entry sits under is *"A LINE
    CITATION INSIDE THE FILE IT CITES IS SELF-INVALIDATING"* — the motivating
    case — and the sweep **spun forever** on it. awk keys `getline < file` on
    the FILENAME, so `cite()` opening the file `scanfile` was mid-read shared
    its stream, and `close()` rewound the outer loop to line 1, which re-found
    the citation, forever. It went a whole step without meeting one only
    because no file in the tree happened to have one; the first that did
    wedged `make test` at 100% CPU with no output. Reproduced in three lines —
    one file, one self-citation. Fixed by loading each file once into a cache
    so nothing is opened twice, which also took the sweep from minutes to
    **0.45s**: it had been re-reading each target file once *per citation*.
  - **THE HANG WAS A PROPERTY OF THE PAIR OF READS, WHICH IS WHY THE FIRST
    MUTATION DID NOT FIRE.** Reverting `cite()`'s reopen alone changed nothing
    — with `scanfile` reading the cache there is no stream left to share — so
    the guard looked vacuous and was not. Reverting the WHOLE fix fires, and
    the observable is worse than a hang: **16MB of spurious STALE lines in
    five seconds**, each claiming "the file has 0 lines", because `cite()`
    drained the shared stream and left `scanfile` at EOF. Two rules. When a
    defect needs two cooperating pieces, a mutation to either one alone is a
    *weaker mutation* and says nothing about the guard — the non-firing rule
    above has a third cause. And **a broken sweep is not one that reports
    something wrong, it is one that reports without stopping**, so the STALE
    capture is `head`-bounded now, for the reason the three checker captures
    already were.
  - **AND A HANG IS THE FAILURE MODE THAT REPORTS NOTHING**, so its guard
    needs a deadline of its own — a hanging case takes the suite down instead
    of failing it. It must also assert the VERDICT rather than termination: a
    sweep that terminated by refusing to resolve self-citations would pass a
    liveness-only case while quietly checking nothing.
  - **AND WRITING THIS PARAGRAPH BROKE THE CHECK IT DESCRIBES**, which is the
    instrument-matches-its-own-documentation shape arriving in the
    documentation *of the instrument*. A prose example of a **stale** citation
    is indistinguishable from a stale citation, so the sweep checks it and
    goes red on a paragraph that is entirely correct. It survived here only by
    two accidents — the pre-correction number happens to land on live code, and
    the post-correction one was written bare as `` `:1122` `` rather than with
    its filename. Neither is a property anyone should rely on: name the line in
    words, never in the matchable form, when the point is that it is wrong.
- **AN INCREMENTAL BUILD HIDES WARNINGS, AND THIS ONE HID 21.** The tree looked
  warning-clean; it is not. Seven objects include the authentic `systm.h` and
  each emits three — one `-Wincompatible-library-redeclaration` for
  `caddr_t calloc()` against the builtin, two `-Wtentative-definition-array` for
  `version[]` and `vmmap[]`. They only appear on a *clean* rebuild of those
  objects. Deliberately **not** suppressed: all three names have zero callers,
  measured, and for `calloc` the warning is the only thing that would speak if
  that changed.

**AND IT WRITES NOW TOO, WHICH IS WHERE THE KERNEL REACHES OUT AND TOUCHES THE
HOST.** §8a step 5d: `writei`, `bmap`'s **allocating** arm, `alloc()`/`free()`
including the free-list chain, `ialloc()`/`ifree()`, `itrunc`, and `namei` with
`NI_CREAT`/`NI_DEL`. A file is created by name, grown past the superblock's
cached free list, deleted, and the accounting comes back exact. Three things
generalise, and the first two are about *notes* rather than about code:

- **A NOTE SAYING SOMETHING IS IMPOSSIBLE DOES NOT NOTICE WHEN IT BECOMES
  POSSIBLE.** `v8fs.c`'s `access()` had dropped upstream's `s_ronly` arm
  because "there is no mount table populated here yet", and said to restore it
  when there was one. Step 5c *gave it one* — `iinit()` calls `allocmount()`
  and sets `s_ronly` — so the arm had been restorable for a whole step. What
  surfaced it is that step 5d is the first thing in the port's history to call
  `access()` with `IWRITE`. Third instance of this shape after `conf.h`'s "the
  switch tables are deliberately absent" and the `shim/kern/h/buf.h` that died
  when a third file arrived: **an unexercised rule cannot be seen to be
  incomplete, and neither can an unexercised excuse.**
- **`hp < 0` IS THE RIGHT TEST FOR "NOT FOUND" AND THE WRONG ONE FOR WHAT
  `kill(2)` DOES.** `psignal` here is a real `kill(2)`, not a bit in `p_sig`.
  A consumer that never called `v8k_procinit()` left `v8k_hostpid` 0,
  `v8k_hostof(0)` matched `p_pid == 0` and returned 0, and the syscall was
  **`kill(0, SIGXFSZ)` — the entire process group.** It killed the test runner
  and the shell above it. `gsignal` **eleven lines away** has carried
  `if (pgrp == 0) return` since it was written, with a comment saying group 0
  is not a group: the fix landed on one line and the line beside it kept the
  assumption, which is this file's most repeated shape. Both refuse 0 now, and
  they are two different claims — "no host process is known" and "0 is not a
  pid". The magic-value rule (`p_pid`, `u_uid`, `FSNMLG`) now has a member
  where the magic value belongs to *the host's syscall* rather than to V8.
- **A MUTATION THAT KILLS THE HARNESS IS A FINDING, NOT A HARNESS BUG** — and a
  harness that filters its output cannot tell you so. The run above produced no
  `FAIL` line and no summary; the first version of the mutation script piped to
  `grep '^FAIL'` and printed nothing at all. Capture whole. Relatedly, an
  unbounded `$(...)` around a checker is a hazard: a corrupted free list made
  `fsck -y` print for its whole 40-second deadline and bash died with
  `xrealloc: cannot allocate 18446744071562067968 bytes`, so the suite could not
  report the failure it had caused. The three checker captures are `| head -200`
  now.

**AND THE AUDITOR CAME BACK CLEAN ON EVERY HAZARD IT EXISTS FOR AND FOUND FIVE
THINGS ANYWAY — FOUR OF THEM SENTENCES.** Run on the step-5d diff: widths at
the u-area seam, the 16-bit ranges, symbol collisions and out-of-bounds all
clean, measured. What it found instead was a comment describing `update()` and
a `bflush()` as a sequence when `update()` *ends* with `bflush` — so the second
call was dead and the sentence named it; a claim that three checkers were
"bounded in time by their deadlines" when only one had one; a count of three
that was one, because it counted renamed *names* while describing
*declarations*; two declarations with no call site; and one real defect, a
`long` read uninitialised on a double-failure path (`-Wconditional-uninitialized`,
which is **not** in `-Wall`).

**Its best finding was not a defect at all: the guard restored that same day
could never be taken.** `access()`'s `s_ronly` arm is read on every create and
`iinit` sets `s_ronly = 0`, so restoring it and exercising it are two different
things. Making it fire needed the superblock field set by hand — legitimate for
the reason `v8k_bdconf` stands in for `config(8)` — and the pair matters: the
same create must *succeed* with the flag cleared, or the case passes against an
`access()` that refuses everything. **And the cleanup for it created a dangling
directory entry that all three new cases were blind to and `fsck` caught within
the hour.** The acceptance test found a bug in the probe, which is M3's
argument arriving from the other direction. So: send the auditor at new shim
code, and read its report for the sentences as well as the code.

`libv8kern.a` is separate from `libv8sys.a` for libkmemu's reason plus a
storage one — **and §8a step 5e found a third that is not a matter of degree at
all: it cannot be linked into a V8 program, at any cost, because 29 of them
share a global name with it and 25 of the 56 collisions are silent.** See the
duplicate-definition entry above and `shim/kern/NOTES.md`. The storage figure:
**240.7 KB** of zero-initialised storage, and `qinit()` dirties
~60 KB of pages. That figure said 85 KB, then 94 KB, then 95.8 KB; it grew when
`streamio.c` brought `_streams`, `_u` and `_file`, again by exactly 1792 when
`ttyld.c` brought `tty[NTTY]`, and now by **145 KB** when §8a step 5c gave the
buffer cache and the inode table real storage — which is a count a correct
import increases, so **re-measure it after every import rather than carrying it
forward**.

**AND IT IS NOW TWO NUMBERS, NOT ONE, WHICH THE OLD MEASUREMENT WOULD HAVE
MISSED ENTIRELY.** Commons are 104242 bytes (`_blkdata` 36736, `_queue` 28672,
`_cblock` 23232 the largest three) — but `shim/kern/sys/main.c` holds another
**142208 bytes of static `__bss`**, which is `NBUF * BUFSIZE` plus
`inode[NINODE]`, and a sweep of `nm`'s `C` symbols cannot see a `static` array.
Measure both: `nm -g` for the commons and `size -m` on `main.o` for the rest —
and **the path is `build/stage0/kern/v8fs/main.o`**, which is worth spelling
out because guessing `build/stage0/kern/main.o` gives `size` no file and it
prints NOTHING, which reads as "no static bss" rather than as a wrong path.
(Same shape: `nm -u` on an ARCHIVE prints `member.o:` headers and BARE symbol
names, so an awk filtering on `/^ *U /` matches nothing and reports a clean
sweep. Both cost a measurement here; both fail silent.) Re-measured and
unchanged: 104242 + 142208 = 246450 = 240.7 KB.

**AND `nm -g` OVER AN ARCHIVE DOUBLE-COUNTS, so dedupe by name.** A common
declared in a header emits a symbol in *every* object that includes it, so
summing all the `C` lines gives **116674** where the truth is 104242 — the same
number, inflated by 12%, with nothing to say so. Re-measured at §8a step 5d:
104242 + 142208 = 246450 = 240.7 KB, unchanged, which is the right answer for a
step that imported nothing. Take the max size per unique name:

```bash
nm -g v8/build/stage0/kern/libv8kern.a | awk '$2=="C"{print $3, $1}' |
  python3 -c 'import sys;m={}
for l in sys.stdin:
    n,s=l.split(); m[n]=max(m.get(n,0),int(s,16))
print(len(m),sum(m.values()))'
```

`cat` does not
carry it. Its externals are `_memcpy`, `_setjmp` and `_longjmp`, all three
V8's own — and `tests/streams` gets that list by **subtracting what the archive
defines from what it undefines** rather than by grepping away a hand-written
list of exported names. The hand-written version would have had to grow by
every name `streamio.c` added, and a name-by-name allow list is exactly how
`tests/kmemu`'s allowed leaks went stale.

**AND THAT GUARD IMMEDIATELY EARNED ITS KEEP, BY REFUSING A PLACEMENT RATHER
THAN A BUG.** §8a step 5e moved the image block driver out of
`tests/streams/fsprobe.c` — the unconsumed-component rule finally has a
consumer, the v8fs server — and the obvious home was `KERN_OBJ`. The archive
then imported `_pread` and `_pwrite`, because a block driver does host I/O by
definition. **A DRIVER SET IS PART OF A CONFIGURATION, NOT OF THE KERNEL
LIBRARY**: `config(8)` is what chooses one on a real V8, `v8k_bdconf` already
stands in for `config(8)`, so `imgdev.o` goes on the link line of whatever is
being configured. Probe and server now share one driver, which makes
`fsprobe`'s 236 cases coverage for the server's block layer.

**And removing it from the archive did not remove it from the archive.**
Dropping an object from `KERN_OBJ` leaves the target newer than every remaining
prerequisite, so the `rm -f && ar rcs` rule never re-ran and `nm -u` still
showed both names. That is the `ar r` note in the Makefile arriving one level
up — there a dropped *source* leaves a stale member, here a dropped *object*
leaves a stale archive — and both read as "the fix did not work".

**AND `hostok.h` HANDS `access` BACK TO libc, WHICH FSPROBE'S OWN COMMENT
PREDICTED AND NOBODY HAD WALKED INTO.** `param.h` renames thirteen kernel names
aside (`#define access v8k_access`) and `hostok.h` undoes all thirteen so a file
can have the host's headers too. `fsprobe.c` records the trap for `free` and
`ialloc` — "calling `free(dev, bno)` would compile and hand a device number to
the C library's allocator". The server hit it with `access`: a K&R
`int access();` declaration plus `access(ip, IREAD)` binds to
`access(const char *, int)`, compiles clean, and asks the *host* whether a path
built out of an inode pointer is readable. Spell the `v8k_` name in any file
that includes `hostok.h`.

**AND THE FOURTH INSTANCE IS A VARIABLE RATHER THAN A CALL, WHICH IS WORSE.**
§8a step 5f transcribed `mkdir()` from `sys/sys2.c:223-257` into the server,
including upstream's own `iupdat(ip, &time, &time, 1)`. In a file that has
included `hostok.h`, **`time` is libc's `time()`** — so that line passes the
ADDRESS OF A FUNCTION as a `time_t *`, and `iupdat` writes four bytes of its
instructions into an inode as a timestamp. It compiles, because `iupdat` is
declared K&R. `access`, `free` and `ialloc` are all *calls*, where a reader
looking for the trap has a call to look at; the kernel's most-read global has
no unqualified spelling in such a file at all, and `&time` reads like the
kernel's clock in every V8 source file ever written. Spell it `v8k_time`.

**AND A K&R FUNCTION-POINTER SLOT WILL NOT TAKE A PROMOTED PARAMETER TYPE.**
`struct bdevsw`'s slots are `int (*)()` and `bio.c` calls them as
`(*bdp->d_open)(dev, rw)`, so the arguments get the default argument
promotions and a `dev_t` (u_short) arrives as an `int`. A driver declaring
`imgopen(dev_t, int)` is therefore describing something the caller never sends,
and clang says so. Declare the parameter `int` — that is the fix, not a
suppression. Worth knowing where it was hiding: `fsprobe.c` had the same two
functions spelled `dev_t` and built fine, because the suite's `KFLAGS` carry
`-Wno-incompatible-function-pointer-types` for the *imported* half's sake. **A
flag argued for 1985 code was covering ours** — the same shape as
`-Wno-implicit-function-declaration` hiding fourteen missing macros.

**AND THE IN-CORE INODE HAS NO TIMESTAMPS, which is easy to assume it does.**
`struct inode` (`src/sys/h/inode.h:15-53`) carries `i_mode`, `i_nlink`,
`i_uid`, `i_gid`, `i_size` and the block addresses — and no times at all. V7
keeps `di_atime`, `di_mtime` and `di_ctime` only in the **disk** inode, and
`iupdat` (`iget.c:250-273`) is what breads it to write them back. Anything that
reports a timestamp has to do that read itself.

**AND THE MOUNT WORKS NOW, ON ONE SENTENCE: THE CONNECTION IS THE OPEN FILE
DESCRIPTION.** §8a step 5e's client is `shim/v8sys/p9cl.c`, a fourth type in
the switch; with `V8MOUNT=/mnt=sock`, `cat /mnt/sub/deep` returns 28000 bytes
identical to what `mkfs` was handed, through Bell Labs' `namei`/`bmap`/`readi`
in another process. **One `connect()` per `open(2)`** is the whole design, and
it is forced rather than chosen: a file offset in client memory is wrong three
ways at once — `dup` shares one offset between two descriptors, `fork` shares
it between two processes, and a program replacing its image keeps it while
every table in its address space dies. All three are the same fact, that the
offset belongs to the **`struct file`** — and a socket is shared by `dup`,
shared by `fork`, and survives the image being replaced. So the fid is a
**constant**, the offset lives on the **server**, and the client holds no
per-descriptor state for a regular file at all. An inherited descriptor works
because there is nothing to inherit.

- **A PROTOCOL CAN ASSUME SOMETHING THIS PORT DOES NOT HAVE, and 9P assumes a
  KERNEL.** 9P has no seek because Plan 9's kernel held the offset in the Chan;
  every Tread carries an absolute one. There is no kernel here. The extension
  is one concept — a fid has a cursor, `P9_OFFCUR` uses and advances it, any
  other offset is 9P's own pread and does not touch it — plus `Tseek`/`Rseek`
  numbered outside 100..127. `tail(1)`, a 1985 program, exercises it.
- **AND A TEST REFUTED THE SENTENCE CLAIMING A CONFORMING CLIENT COULD NOT
  TELL, WITHIN THE HOUR.** `p9probe` reads a directory at 2^64-1 to prove the
  unsigned-offset crash guard is there, and that offset is now the sentinel.
  The honest claim is narrower — invisible at every offset a conforming client
  can read a byte from, and 2^64-1 is not one. Right way round.
- **A `Tclunk` IN close(2) IS WRONG, BECAUSE A CLUNK IS THE *LAST* CLOSE.** The
  fid belongs to the connection, which every `dup` shares — so `sh` doing the
  ordinary thing for `cat < /mnt/hello` (open, `dup2` onto 0, close the
  original) clunked the file out from under the descriptor it had just made,
  and `cat` printed nothing. The right number is **zero**: the kernel drops the
  connection at the last close because it is the thing that knows the reference
  count. **The comment beside it got the mechanism exactly right** — "dropping
  the connection is what actually releases the server's fids" — and then called
  the clunk "politeness". The bug was inside the sentence explaining why it was
  unnecessary.
- **A DESCRIPTOR TABLE IN PROCESS MEMORY BECAME A CACHE, AND NULL NOW MEANS
  *UNEXAMINED*.** `vfs.c:401` had recorded for a year that the table dies when
  a program replaces itself and that this "is fine today and will not be
  later"; later arrived. A server-backed descriptor reading as passthrough gets
  a raw `read(2)` on a 9P socket, which **hangs** — the server sends nothing
  unsolicited — so the authority moved to `getpeername(2)`, which the kernel
  still answers. Measured: a *connected* client reports the bound path (len
  106); an `accept()`ed fd and a socketpair report empty (len 16). The third
  table state is what stops the fallback running on every read of stdin.
- **`dir.c:114`'s RULE HAD TO BE APPLIED A SECOND TIME, IN A SECOND
  FILESYSTEM.** A directory's `st_size` is the length of the V7 record
  snapshot, not what the thing underneath charges — 64 bytes on the image
  against **1024** of records. The port fixed this once for passthrough; a new
  type implementing the same interface does not inherit the fix. That is "the
  line beside it kept the assumption" where the line beside it is a whole second
  implementation.

  **And that pair said 64/768 for a fortnight, which is a measurement of no
  directory at all.** 768 is three records and belongs to the *subdirectory*;
  the root has four entries, so its pair is 64/1024. Neither number was wrong on
  its own and the sentence was arithmetically impossible — 64 bytes of 16-byte
  entries cannot be three records — which is the tell nobody looked for, because
  a pair of plausible numbers reads as one measurement. Found by the client
  probe printing both. The fix is not a corrected constant: `tests/streams`
  asserts the **ratio** (`stat/16 == readable/(V8_DIRSIZ+2)`, the same count in
  two units) over **two** directories of different sizes, so the case cannot
  encode one image's shape.
- **ELEVEN SYSCALLS HAVE NO SLOT IN `struct v8fstyp` AND THAT STOPPED BEING
  CONTAINABLE.** `link unlink rmdir mkdir mknod symlink readlink chmod chown
  utime` are passthrough by construction. Survivable while every type answered
  out of `$V8ROOT`; with a mount, `rootpath()` must *not* prepend the root, so
  `rm /mnt/x` asks the **Mac** to unlink `/mnt/x`. They refuse with `EROFS`,
  one line each, rather than getting slots — a slot claims the operation is
  implemented. `access()` is implemented over `t_stat`; `readlink` is `EINVAL`
  (a V7 image holds no symlink); **`chdir` is the one genuine gap**, since
  nothing tracks a working directory — and the costing for closing it was
  **too small**, re-measured. `..` AT A MOUNT POINT DOES NOT ESCAPE AND THE
  SERVER CANNOT MAKE IT: `ls /mnt/sub/..` correctly gives the mount root, and
  `ls /mnt/..` gives **the image root again**, not the jail's `/`. That is V7
  being right — a filesystem root's `..` points at itself — and on a real Unix
  it is `namei`'s mount table that fixes it when a walk crosses a mount
  upward. There is no kernel here, and the image does not know it is mounted,
  so the client must resolve `..` at the mount point **lexically**. And
  `getwd(3)` is the hard consumer because it *writes*: `getwd.c` opens `..`,
  reads it, **`chdir("..")`s**, repeats, and chdirs back — every level is
  another chdir to intercept, and its loop matches `d_ino` against `stat(".")`,
  which puts the folded-inode machinery on the same comparison. Plus the cwd
  must survive `exec`, which is `vfs.c:401`'s lesson: it has to live in the
  ENVIRONMENT like `V8MOUNT`, and then two things there have to agree.

  **§8a step 5f TURNED FOUR OF THEM INTO SLOTS AND THE COUNT WAS NEVER ELEVEN.**
  An auditor counted fourteen: nine that refuse (ten `MOUNTED()` calls, because
  `link` guards both names), plus `access`, `readlink` and `chdir` which answer
  instead, plus `chroot` — which passes its path **completely unresolved**,
  mounted or not — and `execve`. Counting names while describing calls is a
  shape this file has recorded before. `access`, `unlink`, `mkdir` and `rmdir`
  are slots now, and `mknod`'s directory arm with them; `chmod`, `chown` and
  `utime` are one `Twstat` away and deferred to keep the step reviewable;
  `link` and `symlink` have no 9P2000 message and a V7 image holds no symlink
  anyway; `mknod` for a *device* is meaningless on an image no kernel mounts.

  **AND `chdir` IS NOT A GAP EITHER SINCE §8a step 5i** — the paragraph above
  is left standing because its *costing* is what the step turned out to owe,
  every clause of it, and it is the rare estimate this file records that was
  too small rather than too large. Read it as a bill that was paid: the fold is
  lexical, `getwd` was the hard consumer, and the cwd does live in the
  environment. What it did not foresee is that the fold needs a MODE, which is
  the entry at the end of the mutation-rules section.
- **AND THE CASE FOR THAT GUARD PASSED FOR THE WRONG REASON.** `chmod 777
  /mnt/hello` exits 1 whether or not the guard exists, because this machine has
  no `/mnt` and the host's `chmod` fails too — the guard and the absence of the
  directory are indistinguishable. The host-property trap in a case written to
  prove containment. Fixed by mounting over a directory the host really has,
  holding a file with different contents, and asserting both that the read is
  **shadowed** and that the host file's mode is **unchanged**.
- **MIXING OCTAL AND HEX IN ONE FLAG TEST NAMES THE FLAG NEXT TO THE ONE YOU
  MEANT.** `flags & 01000` was written for `O_TRUNC`; 01000 is 0x200, which is
  `O_CREAT`. `syscall.c` spells both in hex side by side for exactly this
  reason.
- **A TRANSPORT MUST NOT LEAK ITS SIGNAL SEMANTICS INTO THE FILESYSTEM.** The
  connection is a socket and the caller is a V7 program that has no idea it is
  one — so a `v8fsd` that died mid-conversation raised **SIGPIPE** on the next
  request and *killed* `cat`, where a V8 disk that stops answering is `EIO`.
  Found at one remove: the sanitized server above aborts on a broken guard, and
  the client came back 141. The fix is `SO_NOSIGPIPE` in `p9dial`, and **it has
  to be per-socket rather than `signal(SIGPIPE, SIG_IGN)`** — ignoring the
  signal changes the program's own disposition, and a V8 program in a pipeline
  must still die when its reader goes away, which is how `yes | head`
  terminates. The case produces the condition with `shutdown(2)` on the
  client's *own* descriptor: no second process, no timing, and the assertion is
  that the probe reaches its last line.
- **AND `p9walk` ANSWERED ENOENT WHERE V7 ANSWERS ENOTDIR, BECAUSE A SHORT
  Rwalk CARRIES NO ERRNO.** `namei` has two answers one line apart and so does
  the server (`v8fsd.c:1136` sets `ENOTDIR`), but 9P's short reply is silent
  about *why* it stopped — so the client flattened both to `ENOENT` and
  `open("/mnt/hello/beyond")` reported the wrong one. The information is in the
  qids the reply carries and the client was discarding them: the last one
  describes what the failed component was looked up *in*, and if it is not a
  directory the reason is `ENOTDIR`. **The guard needs three cases, not two** —
  a missing name at the top (an Rerror, carrying the server's own errno), a
  missing name inside a real subdirectory, and a walk through a plain file. The
  middle one is the discriminator: it shares its code path with the third, so a
  client that simply always said `ENOTDIR` passes the third alone. Both
  one-sided mutations fire on exactly one case each.

**AND IT WRITES NOW — §8a step 5f — WHERE "READ ONLY" HAD MEANT THE PROTOCOL
AND NOT THE FILESYSTEM.** `sh -c 'echo x > /mnt/fresh'` creates a file on a
disk image through Bell Labs' `namei`/`ialloc`/`writei` in another process;
`rm`, `mkdir` and `rmdir` work; `icheck`, `dcheck` and `fsck` say the result is
clean with the block count back to exactly what it started at. Four things
generalise and three of them are about a claim rather than about code:

- **THE READ PATH HAD BEEN WRITING ALL ALONG, AND THE WRITE WAS INVISIBLE
  BECAUSE THE CLOCK WAS FROZEN.** `readi` sets `IACC` (`rdwri.c:50`), so `iput`
  runs `IUPDAT` and dirties the disk inode on every read. The recorded finding
  named two accidents hiding it — `O_RDONLY` and no `bflush()`. Re-measured
  with an instrumented driver: `O_RDONLY` was doing **no work at all** (O_RDWR
  with no flush = zero pwrites across twenty-two reads; the buffer just sits),
  and there is a **third**: `time` is set once by `iinit` from the superblock's
  `s_time` and nothing advances it, while `mkfs` writes `di_atime == s_time` on
  every inode — so `dp->di_atime = *ta` stores the bytes already there. The
  driver prints the write and `cmp` prints nothing. Perturb one `di_atime`
  first and exactly four bytes move. Round-trip class, third instance after
  `ttldioc` and `strncat`, and the first in an *artefact* rather than in memory.
- **A FROZEN CLOCK IS ONLY INVISIBLE UNTIL SOMETHING WRITES**, at which point
  every `mtime` a create lays down is the moment the image was made — a
  plausible wrong answer. `v8fs_clock()` is the other half of the substitution
  `iinit` already makes for `clkinit`, and it is a **raw `gettimeofday`**
  because `tests/kmemu` asserts `libv8kern.a` imports exactly three names.
- **"READ ONLY" IS A MOUNT FLAG AND BELL LABS WROTE BOTH LINES.**
  `fsmount()` at `sys3.c:299,316` opens the device `!ronly` and stores
  `ronly & 1` in the superblock; `v8k_kinit(dev, ronly)` and `v8fsd -r` are
  those two lines. With it set, `iupdat` returns at `iget.c:248` before it
  breads anything, so not even an atime moves — a guarantee an `EROFS` arm in
  the dispatch never gave. The three servers `tests/streams` already ran on one
  shared image now run `-r`, which turns a contamination hazard into a guard
  and changes not one existing expectation.
- **`do_remove` TOOK THE ROOT AS THE PARENT**, which is right only for names
  directly under the mount: a Tremove carries a fid and V7's unlink names a
  *directory* and an *entry*, and `..` is an entry, so it exists for a directory
  and not for a plain file. `rm /mnt/d/f` asked the server to unlink `f` from
  the root. The fid records the directory each walk stepped through now.
  **Found by running it** — the walk and the remove are in different functions
  and each reads correctly alone.

**AND THE AUDITOR FOUND TWELVE MORE, NOT ONE OF THEM AN LP64 BUG.** Run on the
client the hour it was written — the rule that the subagent earns its keep on
*new shim code* held for the fourth time. It came back clean on every hazard it
exists for; what it found was **a rule stated in one place and not applied in
the one beside it**, twelve times. `shim/kern/NOTES.md` has them all. The four
that generalise:

- **THE GUARD THAT COULD NOT FIRE AND THE CASE THAT DID FIRE RETURNED THE SAME
  VALUE.** `p9uid()` parses an owner and returned 0 — root — on both failure
  paths, against this file's own contract that *non-root never maps to root*.
  Its range test (`v > 32767`) is **dead**, because `"%d"` of a `short` cannot
  exceed it; the live route is that `di_uid` is `v8_i16` and therefore
  **signed**, so uid 40000 renders as `"-25536"` and the `'-'` is a non-digit.
  A written guard next to an unguarded case, both landing on the same return.
- **THE BITS WERE THE SERVER'S AND THE IDENTITY WAS THE HOST'S**, and identity
  is the half that decides. `access()` recomputed permission from the image's
  mode against the host uid while the server takes `fio.c`'s root bypass
  (`u_uid` is 0 and nothing sets it) — so on **every file of every image**
  `test -r` said no and `cat` printed it. When two ends both look right, check
  which end supplies each *operand*, not just each value.
- **A DEADLINE BUILT ON `alarm` DOES NOT BOUND A V8 PROGRAM**, and the suite
  hung twenty-six minutes proving it. Two independent reasons and the alarm
  itself is fine: **V8's `sh` catches SIGALRM on purpose** (`sh/fault.c:123`,
  because it uses `alarm(2)` for `$TIMEOUT` and the fork retry), and *a
  deadline on one end of a pipe is not a deadline on the pipe* — `sh -c 'cat <
  f'` leaves `cat` holding the stdout a `$( )` is reading. Fork, `setpgrp`, and
  kill the **process group**.
- **V8's `cat` IS NOT AN INSTRUMENT FOR READ ERRORS**: `while ((n = read(...))
  > 0)` ends the loop on −1 and exits **0**. Two cases written to assert a
  failing read both passed for that reason — *including the passthrough
  control*, which is what gave it away. Assert the bytes.
- **AND `rm` IS NOT ONE FOR WRITE ERRORS, IN TWO DIFFERENT DIRECTIONS AT ONCE.**
  §8a step 5f asserted that a read-only mount refuses `rm /ro/hello` by reading
  the exit status, and it is **0** whichever way you run it. Plain: `rm.c:101`
  is `if(!fflg) if(access(arg,02)<0) { print the mode; if(!yes()) return; }` —
  so on a file it may not write V7's rm *asks*, gets no answer from a non-tty,
  and returns having done nothing and set no error. With `-f`: the question is
  skipped, the unlink fails with EROFS, and `if(unlink(arg) && (fflg==0 ||
  iflg))` suppresses both the message and the error count. Both are correct rm.
  So the two obvious ways to run it agree on the one answer that means nothing.
  Assert the file.

**AND §8a step 5f-b GAVE chmod, chown AND utime THEIR SLOTS, WHICH ARE ONE
MESSAGE — leaving only `link` and `symlink` refusing.** A Twstat carries a whole
stat and the server applies whichever fields are not the all-ones sentinel. Five
things generalise, and three are about a sentence rather than about code:

- **"NOTHING SETS X ALONE" IS NOT "NOTHING SETS X", and the difference is the
  whole of a missing arm.** `s_atime` was declined with a reason that is still
  true — *"nothing in this world sets atime alone, and an unexercised arm is a
  claim nothing can check"* — and the consumer that arrived sets **both**.
  `mv.c:129` is `utime(target, &s1.st_atime)`, and on a mount it is not an
  unusual path but the **only** path, because `link(2)` has no slot and is
  refused, so mv always falls through to fork, `/bin/cp` and utime. Before the
  arm, `mv` on a mount copied and unlinked correctly and lost both timestamps
  silently. This is the unexercised-rule shape with a **qualifier** doing the
  damage: only the unqualified claim would have been a reason.
- **THE TWO ENDS OF ONE WIRE, AN HOUR APART, AND ONLY THE READING END HAD THE
  GUARD.** `do_wstat` parsed a 9P owner with `atoi`, which has no error return,
  so `"nobody"` and `"--"` both set the inode's uid to **0 — root**, the one
  identity `fio.c:193` lets bypass every permission check, and answered Rwstat.
  The *client* end of the same field had had the guard since an earlier audit,
  with the contract written beside it: root maps to root, non-root never maps to
  root. Plain 9P2000 specifies the field as a **name**, so a conforming foreign
  client sends one. Two properties, and only the second is guarded: **range is
  not parseability** — `"65536"` truncates and must, because `sys4.c:294` is
  `ip->i_uid = uap->uid` unchecked and that is V7's own answer. Nothing could
  have caught it: `do_wstat` had no client caller until the step that added one.
- **A COMMENT CAN CLAIM UPSTREAM'S RULE WHILE THE CODE BESIDE IT IMPLEMENTS A
  DIFFERENT ONE, AND A UNIFORM VALUE HIDES BOTH.** `chmod` and `utime` gate on
  `owner(1)` — `fio.c:215-228`, ownership **or** superuser — and the code tested
  `suser()` alone while the comment said "ownership for everything else" and
  cited `sys3.c`, which is `fsmount`. Not observable, because `u_uid` is 0 here
  so both rules always permit; that is precisely why the sentence and the line
  could disagree for a whole step. Two more of the same shape sat with it:
  upstream strips ISVTX for a non-root chmod, and upstream's `utime` sets
  `IACC|IUPD|ICHG` where the arm set two of three. **Read the syscall, not the
  memory of it** — and note the comment "Can't set ICHG" means the caller cannot
  *choose* a ctime, not that ctime stays put.
- **EROFS IS A CLAIM ABOUT THE MEDIUM AND EPERM IS A CLAIM ABOUT THE OPERATION,
  and one of them stopped being true.** `v8s_mknod`'s device arm said "the
  refusal is the same one the host arm gives" and it was not: a `MOUNTED(p)` made
  the mounted answer EROFS and the host answer EPERM. Since 5f the filesystem
  takes writes, so EROFS is false there and EPERM — this operation is meaningless
  — is true of both worlds. The guard could go because that arm never touches the
  path. `/proc`'s three new slots record it from the other side: EPERM, because
  `/proc` very much does take writes and it is these three operations that mean
  nothing there.
- **FOUR DEAD DECLARATIONS SHARED A LINE WITH TWO LIVE ONES, directly under the
  paragraph forbidding them.** `int iinit(), binit(), bhinit(), ihinit(),
  update(), brelse();` sat below "a declaration with no call site is an
  unconsumed component"; only `update` and `brelse` have call sites. The one that
  mattered is `iinit`, which **5f itself had changed** to `void iinit(int ronly)`
  while the declaration still said `int iinit()`. Deleted rather than corrected:
  the fix for an unconsumed declaration is not a better declaration. A mixed line
  is how the dead ones keep cover.

**AND FILLING THE IMAGE KILLED THE SERVER, WHICH IS A GUARD ARGUED FOR ONE
CONSUMER MEETING A SECOND WITH THE OPPOSITE ANSWER.** `cat big > /mnt/x` on a
200-block image printed `file system full` and then `panic: tsleep: no device
below, and no timeout`; the server exited 2 and **every** client's connection
dropped, not just the writer's. Reachable from an unprivileged program; the
image survives, so it is availability rather than corruption. Bell Labs'
out-of-space path is a kludge they label one in capitals — `alloc.c:185-196`
sleeps five clock ticks on `lbolt` hoping another process frees a block — and
`v8fs.c`'s `sleep()` maps that onto a `tsleep` whose panic comment reasons
**entirely about streams**, which was a true and complete account of every
caller that existed when it was written. Four things:

- **THE SURVEY LISTED EVERYTHING EXCEPT THE ONE THAT CAN FIRE.** `v8fs.c`'s
  comment enumerates the sleepers as `iget.c:93` and `alloc.c:89,215,295`
  "waiting for a locked inode" and attributes the rest to "bio.c's". All four
  of those are unreachable in a single-threaded server — nothing else is
  running to hold a lock — and `alloc.c:194`, the only reachable one, is in
  neither list. A survey that enumerates callers can miss the only live one.
- **THE FIX IS PROVABLE, NOT PROBABLE, AND IT IS TWO GREPS.** `alloc.c:194` is
  the only sleeper on `lbolt` in the imported tree, and `clock.c:290` — the
  clock interrupt — is the only waker in the whole 18k-line kernel, which this
  port neither has nor imports. So that sleep can never wake, which is the same
  *form* of argument `slp.c`'s panic makes about a stream with no device,
  reaching the opposite verdict because the caller is different. Returning
  immediately is not a semantic change: upstream waits for **another process**
  to free a block, and here the caller is the only thing running, so the loop is
  guaranteed to fall through to ENOSPC either way.
- **TWO DEFECTS WERE HIDING EACH OTHER, and fixing the first made the second
  live** — predicted by the auditor and then confirmed. `kmkdir` never consulted
  `u.u_error` after `writei`, so on a full image **`mkdir` exited 0** for a
  directory `fsck` calls damaged. Upstream ignores the same return and **can
  afford to**, because it *is* the system call and `u_error` reaches the user;
  here it died in the wrapper. The damaged directory is not the defect — it is
  what a V7 kernel leaves and what `fsck` repairs — the **success reply** was.
- **"THE WRITE FAILS" IS NOT THE GUARD; "THE SERVER IS ALIVE" IS.** A server
  that dies mid-write looks exactly like a failed write from the client, so the
  obvious case passes either way. Measured: reverting the fix fires *the server
  is still alive* and *it still answers a read*, and leaves the write case
  green. Nor is the image asserted clean afterwards — only still **readable**,
  which is the thing a mid-write death would actually have destroyed.

**AND A HOST id NARROWED INTO V8's 16 BITS IS ONE RULE WITH FOUR SITES, AND THE
FIX HAD REACHED ONE OF THEM.** `shim/v8id.h` is `v8_foldid()`; the contract is
**root maps to root, non-root NEVER maps to root, everything that fits stays
exact**. `u_uid` and `st_uid` are `short` — V8's own widths — so a bare
`(short)` cast maps every multiple of 65536 onto **zero**, and zero is the
identity `fio.c:193`'s `access()` bypasses. Measured: 65536 → 0, 131072 → 0. The
cast does not produce a wrong number, it produces a **privilege**.

- **THE EARLIER FIX STOPPED AT ONE FILE, AND SWEEPING FOUND THREE MORE.**
  `fio.c` was given the fold after an auditor caught it folding `p_pid` and
  casting `u_uid` on the next two lines. Still standing afterwards:
  `stat_translate` (every `ls -l`), `procfs.c`'s u-area (`ps`'s uid column), and
  — found by the next audit — `p9_t_chown`, which writes an id **into a disk
  image**. Same shape as always, with the line beside it *in another component*.
  `p_uid` is deliberately not folded: this port widened it to `int`, so there is
  no narrowing there to guard.
- **IT IS A HEADER BECAUSE NO TWO OF THE FOUR MAY SHARE AN ARCHIVE** — libv8sys
  must not link libv8kern, and libkmemu is the one that may link libc. A pure
  arithmetic rule needs no link edge, so it is `static` in a header. Spelling it
  four times is what `kmem.c`'s one-table rule refuses, and is how the third
  site was missed.
- **getuid(2) IS DELIBERATELY RAW, and the tree settles it in one line.** Every
  16-bit *field* is folded; `getuid` is a value that flows back **out** to the
  host, and `mv.c:56` is `setuid(getuid())`. The cost is that
  `st_uid == getuid()` disagrees above 32767 — **not a regression**, the cast
  disagreed for the same values. Making them agree needs a two-way map, which is
  a design and not a patch. What gets fixed is the contract, because root is a
  privilege and a colliding non-root id is only a wrong name.
- **THE ROUND TRIP IS ITS OWN PROPERTY.** The client sent a raw id, the server
  truncated with V7's own `ip->i_uid = uap->uid`, and `p9uid()` at the *reading*
  end refuses a leading `-` — so `chown(f, 40000)` stored −25536 and `stat` read
  back `P9UID_BAD`. Two ends of one field, each defensible alone. Fold before
  the wire and the number stored is the number rendered is the number parsed,
  **and it is what the jail reports for the same id**.
- **NO END-TO-END TEST CAN REACH ANY OF THIS** (uid 501 here, lower on a
  runner), so there are **two** guards for two different relations: the
  arithmetic, over a table, in `tests/v8sys`; and the call sites, as a source
  sweep plus a **derived** count of the files that call the fold, in
  `tests/kmemu`. The unit test cannot see a missing call and the sweep cannot
  see broken arithmetic. And the sweep **matches its own documentation** — four
  files now discuss the cast in prose — so comments are excluded and the
  excluded count is *printed*, rather than a filter quietly hiding things.

**AND THE OPEN MODE WAS CHECKED AT OPEN AND NEVER AGAIN.** V7 re-checks it on
every transfer, in one line — `rdwr()` is
`if((fp->f_flag&mode) == 0) { u.u_error = EBADF; return; }` — and v8fsd had one
third of it: `do_read` checked only that the fid was open, and `do_write`
refused `P9_OREAD` but not `P9_OEXEC`. So a fid that had proved only execute
permission could write, and `open("/mnt/f", 1)` followed by `read()` returned
the bytes. Three things:

- **THE TWO GATES ARE NOT COMPLEMENTS, and that is 9P's rule rather than a
  choice.** `open(5)` defines mode 3 as *"execute (read, but check execute
  permission)"*, because Plan 9's kernel must read a binary to run it — so
  OEXEC **reads** and does not write. `canread()` is "anything but OWRITE",
  `canwrite()` is "OWRITE or ORDWR", and each direction needs a refusal case
  *and* a success case: a server that refused OEXEC outright would pass a
  refusals-only suite.
- **SAY WHAT THE FIX DOES NOT FIX.** `u_uid` is 0, so `access()` takes
  `fio.c:193`'s root bypass and grants IWRITE on everything — the
  `OEXEC|OTRUNC` truncation an auditor measured happens before the change and
  after it. Folding OTRUNC into `want` is still right (`open(5)`: truncation
  requires write permission "even if the mode is OREAD") and collapses one rule
  that was written twice, but it changes no answer today and the comment says
  so. What is live is the **gate**, which is a property of the fid rather than
  of the identity and is wrong at any uid.
- **SECOND STEP RUNNING WHERE THE GUARD HAD TO BE A WIRE-LEVEL CASE.** The
  client opens with V7's three modes, has no spelling for OEXEC, and would
  never write to a fid it opened for reading — so no shipped binary can ask the
  question. Same as the owner-name guard one commit earlier. When a defect
  lives in what a *foreign* client could send, the probe is the only instrument.

**AND §8a step 5g GAVE link A SLOT, WHICH LEAVES ONLY symlink REFUSING — AND
THE REASON IT HAD BEEN REFUSED WAS THE PORT'S OWN ARGUMENT FOR THE OPPOSITE.**
`syscall.c` and `vfs.h` both said of link and symlink *"Neither is deferred
work: 9P2000 has no message for either."* Every clause of that was doing
damage:

- **A MISSING MESSAGE IS WHAT PRODUCED THE LAST TWO EXTENSIONS.** `Tseek`/
  `Rseek` (128/129) and `Taccess`/`Raccess` (130/131) both exist because 9P had
  no message for something V7 has. Twice the answer was to add one and write
  down why; the third time the identical fact was recorded as grounds for
  refusing. **A reason already overruled twice in the same file is not a
  reason.** `Tlink` is 132/133, and `Tunlink` 134/135.
- **AND THE PAIR WAS NOT A PAIR.** A V7 filesystem cannot *represent* a
  symlink — no `i_mode` for it, which is the same fact `readlink` answers
  EINVAL on — so that refusal is permanent. A V7 filesystem **is built on**
  hard links: `i_nlink` is a field in the inode, `sys2.c:458`'s `link()` is
  `ip->i_nlink++` plus a `namei` with `NI_LINK`, and **`nami.c:484`'s NI_LINK
  arm was already in the imported tree**, unreachable only because nothing
  sent it a request. link was chdir's shape filed under symlink's.
- **IT READ AS COSMETIC BECAUSE THE LOUDEST CONSUMER DEGRADES QUIETLY.** `mv`
  of a FILE falls back to fork-and-`cp` and exits 0. `mv` of a DIRECTORY does
  not — `mv.c`'s `mvdir()` at `:204` has **no fallback** — so it printed
  `mv: cannot link` and left the directory where it was. Measured against a
  server that had accepted `echo > /mnt/f` and `mkdir /mnt/d` seconds earlier:
  `ln` said **"Read-only file system"**. EROFS is a claim about the medium and
  the medium had taken two writes.
- **symlink KEEPS ITS REFUSAL AND LOSES ITS WORD**, which is `v8s_mknod`'s
  device-arm distinction applied to the line beside it, one step late as
  always: EPERM, because the operation is meaningless, is true and permanent
  where EROFS is measurably false. `symlink(2)` documents exactly that errno
  for a filesystem that cannot hold one. And cross-type `link` is **EXDEV** —
  an *answer* rather than a refusal, and the one `nami.c:487` and the host's
  own `link(2)` both already give.

**AND ADDING link IMMEDIATELY EXPOSED THAT `unlink(2)` OF A DIRECTORY HAD BEEN
THE WRONG SYSCALL ALL ALONG.** 9P has one remove and V7 has two calls: Plan 9
has no `rmdir(2)` at all, so `Tremove` carries no flag and v8fsd decided from
the inode — right for a foreign client, wrong for V7's `unlink(2)`, which is
`NI_DEL` whatever it names. **Invisible until link existed**, because with no
way to give a directory a second name every removable directory had
`i_nlink == 2`, and `NI_DEL` and `NI_RMDIR` differ only above that. Three
disagreements arrived at once:

- **the errno** — `nami.c:363` answers EBUSY where V7 succeeds;
- **the on-disk result** — `NI_RMDIR` sets `i_nlink = 0` and frees the inode
  where `NI_DEL` decrements, so even the "working" case destroyed a directory
  V7 would have left for `fsck` as unattached;
- **and the failure path corrupts the parent** — `nami.c:361` decrements
  `dp->i_nlink` *before* the EBUSY test two lines later and the error arm does
  not put it back. Measured with `dcheck` after one failed `mv`: root had 3
  entries and a link count of 2. **Upstream's own bug, deliberately not fixed**
  — `src/sys` is imported.

**THE RECONCILIATION IS ONE LINE AND IT IS FORCED BY A FICTION THE PORT ALREADY
TOLD.** `rmdir(1)` is three unlinks (`rmdir.c:105,108,110`) and `dotlink()`
**absorbs the first two**, so the third arrives with the two decrements that
should have preceded it unperformed — a plain `NI_DEL` then leaves an
unattached directory. `pt_remove` made the same call explicitly for passthrough
on host grounds. So `Tunlink` on a directory asks the only question the two
cases differ on: **does another name reach it?** At `i_nlink <= 2` the entry is
its last and removing it destroys the directory; above 2 another name survives
and `NI_DEL` is the only answer that is not data loss.

**AND NOTHING BUT `fsck` COULD SEE THE REGRESSION.** The listing was right,
`icheck` was silent, `dcheck` was silent, and `fsck` said
`***** FILE SYSTEM WAS MODIFIED *****`. Third instance of the independent-reader
rule, and the sharpest: the three checkers do not agree with each other about
what they can see.

**TWO OF THE NINE MUTATIONS WOULD NOT FIRE, AND BOTH SAID THE SAME THING — THE
GUARD WAS UPSTREAM'S ALREADY.** `do_link`'s draft checked four things and two
were duplicates of a refusal Bell Labs already make, so deleting them changed
nothing:

- **ENOTDIR on the directory fid.** `klink` hands the inode to `namei` as
  `u_cdir` and `nami.c`'s own loop refuses a non-directory with that errno.
  The comment three lines below had already argued for letting upstream's line
  fire — about EXDEV — and **the line beside it kept the assumption**, written
  by the same hand in the same hour.
- **EINVAL for `.`/`..` as the new name, and its stated reason was FALSE.** It
  claimed `nami.c` "would happily write the entry" and replace a live
  directory's parent pointer. It would not: `nami.c:88-95` returns **EEXIST**
  for NI_LINK the moment the name is found, and `..` always is. The guard was
  *less* faithful than no guard, and the case that "covered" it was really
  testing `p9parent` on the client.

**AND THE ERRNO TABLES WERE MISSING SEVEN NAMES, WHICH THE GUARD ON THEM
STRUCTURALLY COULD NOT SEE.** `tests/streams` compares v8fsd's `errnames[]`
with p9cl's `enames[]` and they agreed perfectly — about a set that was too
small. `EBUSY EFAULT EINTR ELOOP ENODEV ENOTTY EXDEV` were in **neither**, so
each reached the client as EIO through a fallback documented for a *foreign*
server's prose. **Two copies of one wrong list agree.** The fix is a third
source that is neither table — the kernel itself:

```bash
grep -rhoE 'u\.u_error = E[A-Z]+' v8/src/sys/ v8/shim/kern/ | grep -oE 'E[A-Z]+$' | sort -u
```

Twenty-two names; both tables had fifteen. `tests/streams` derives that set
every run now, so importing another `sys/*.c` extends the check with nobody
editing the suite. Note the shape: the earlier ENOMEM fix **added one name
instead of auditing the set**, which is exactly how the other seven survived
it.

**AND 9P's STAT CARRIES NO LINK COUNT, so the obvious case would have asserted
a constant.** `p9cl.c:1322` sets `st_nlink = 1` for every file on every mount —
9P2000 has no such field, `.u` and `.L` added one — so `ls -l` can never show
a hard link. The observable is `ls -i`: a qid path **is** `i_number` off the
disk, so two names printing one number is the disk saying they are one file.
The 1 is asserted deliberately, so the limitation is a case rather than a
sentence.

**AND §8a step 5h MADE `mv` OF A DIRECTORY WORK BY RETRACTING 5g's RETRACTION,
WHICH IS A NEW SHAPE: A COMPENSATING ERROR READS AS A DESIGN, AND EVERY CLAUSE
OF ITS JUSTIFICATION CAN BE TRUE.** 5g shipped `isdir = i_nlink <= 2` for
Tunlink and explained it as reconciling V7's unlink with a fiction the port
already told — `dotlink()` absorbing `rmdir(1)`'s two dot unlinks, so the two
decrements that should precede the third had never happened. Accurate about
the mechanism, correctly cited, and wrong in its conclusion, because what it
described is **one workaround split across two files** and it read that as a
design. This is not the recorded-diagnosis rule: the diagnosis was right and
the *inference* was wrong. **When a comment explains a guard by describing a
lie told somewhere else, the fix is usually to stop telling the lie.**

- **A PROTOCOL MESSAGE'S SHAPE IS AN ASSERTION ABOUT WHAT THE OPERATION
  NAMES**, and `Tunlink` had copied `Tremove`'s `fid[4]` — importing Plan 9's
  noun into a V7 verb. `remove(2)` names a FILE; `unlink(2)` names a DIRECTORY
  and an ENTRY. `..` is where the two visibly separate: it is an entry that
  exists only from the directory's side, so **no fid can name it**, which is
  precisely why `v8fsd` zeroes `f_pino` for a fid walked to `sub/..`. Reshaped
  to `dfid[4] name[s]`, matching Tlink. And the three hazards `removeop`
  documents — clunk on every exit, the parent from `f_pino`, the iput of the
  target before `kremove` — turned out to be properties of **carrying a fid**
  rather than of removing a name, so `do_unlink` shares none of them.
- **AND THE DIAGNOSIS WAS ALREADY WRITTEN, IN THE FUNCTION IT INDICTED.**
  `do_remove`'s header said "a fid names a FILE, V7's unlink names a DIRECTORY
  and an ENTRY, and nothing in a struct inode bridges the two" — a complete bug
  report for a *different* message, sitting unread for a step because it was
  phrased as an explanation of `f_pino` rather than as a complaint about
  Tunlink. Same family as the `ttldioc` comment that got the mechanism exactly
  right and called the consequence "politeness".
- **THE TWO HALVES HAD TO GO TOGETHER, AND EITHER ALONE CORRUPTS.** With the
  absorption dropped, `rmdir(1)`'s three unlinks do V7's own arithmetic
  (`d/..` decrements the parent, `d/.` takes d 2→1, `d` takes it to 0 and
  frees it); keep the heuristic and the third call takes NI_RMDIR and
  `nami.c:361` decrements the parent a **second** time. Measured: the mutation
  restoring the absorption alone turns **fsck** red while icheck, dcheck and
  the block-count identity all stay silent — third instance of the
  independent-reader rule, and again fsck is the only one that can see it.
- **A PREDICATE CAN TEST THE WRONG OPERAND AND BOTH CALLERS THEN LOOK THE
  SAME.** `v8s_link`'s dot arm could not distinguish `mkdir(1)` from `mv(1)`
  because it stated `a`, and both pass a directory there. The discriminator is
  `b`: mkdir's target entry already exists (mknod wrote it), mv's was unlinked
  one line earlier at `mv.c:216` for the express purpose of making it name
  something else. Absorption's claim is "the entry you asked for exists with
  the meaning you wanted" and **nothing had ever checked the first half**. The
  new arm is **monotone** — it can only absorb fewer calls — which is what
  bounds a change to a predicate, exactly as `rootpath()`'s access-to-lstat fix
  did.
- **AND A SHARED HELPER CAN HAVE ONE CALLER'S POLICY BAKED INTO IT**, which is
  the unexercised-rule shape arriving in a *helper* rather than in a rule.
  `p9parent()` refused `.`/`..` as a basename for all four callers; right for
  create (a file named `..` is nonsense) and a hard limit on link and unlink,
  which is what mv rewrites. It takes the policy as a parameter now.
- **A CASE CAN GET STRONGER BY LOSING A GUARD, AND ITS OWN COMMENT PREDICTED
  THE BETTER ANSWER.** `dotdot as a new name` asserted 22 (EINVAL) **from our
  client**, and the comment beside it already said what the wire would answer:
  `nami.c:88-95` returns EEXIST for NI_LINK the moment the name is found, and
  `..` always is. Removing the client's refusal made the case assert **17, from
  Bell Labs**. Same shape as 5g's two deleted `do_link` checks: a refusal
  duplicating one upstream already makes is not a second guard, it is a layer
  that can disagree with them.
- **A MUTATION THAT FIRES ON EXACTLY ONE CASE IS WHAT VALIDATES THE CASE**, and
  is a different measurement from one that fires widely. Four mutations here:
  one fired twelve cases (the code matters), one fired exactly the case written
  for it (the case is *aimed*). A suite where every mutation fires a dozen cases
  cannot tell you which guard is load-bearing.
- **AND cites.awk CAUGHT ONE STALENESS EVENT OUT OF FIVE IN THIS ONE STEP,
  WHICH IS THE BLIND SPOT MEASURED RATHER THAN ESTIMATED.** The sweep went red
  once, on a citation that landed on a blank line. Four more were stale and it
  passed every one of them, because each had drifted onto **plausible code** —
  the failure mode its own header names and which had only ever been
  illustrated by a single historical example:

  | citation | landed on | truth |
  |---|---|---|
  | `p9cl.c` st_nlink (4 sites) | `return ((short)v);` | 19 lines further down |
  | `p9cl.c` p9uid range | `}` … a comment | 42 lines down |
  | `syscall.c` fold_ino (3 sites) | `static void` | 8 lines down |
  | `syscall.c` getwd (PLAN.md) | a comment about `mkdir`'s two links | **1102 lines** away |

  Three were **caused by this step's own edits to a different part of the same
  file** — a comment grew by twenty lines and invalidated citations in four
  other files — and the last was already stale and had never been noticed. Two
  things follow. **After editing a file, grep for every citation INTO it and
  re-measure each**; the sweep cannot do this for you and the citing file is
  usually not the one you touched. And **a citation "corrected" from memory is
  invention, not drift**: `rmdir.c`'s three unlinks were rewritten one line
  low while the sentence around them was being edited, and the wrong line is a
  `goto` label — live code, invisible to the sweep. Measure the line every
  time. The sweep catches a *subset* of drift and none of invention.

**AND THE MUTATION RULES GAINED TWO, BOTH FROM THE HARNESS RATHER THAN THE
CODE.** The existing entries cover the restore side; these are new:

- **THE SAME-SECOND TRAP HAS AN *APPLY* SIDE.** The previous mutation's restore
  rebuild finished in the same second the next mutation was written, so `make`
  declared the artefact current and two runs measured nothing. They were caught
  only by the harness's own artefact-hash check, which is the difference between
  "did not rebuild" and a false "the guard did not fire". Sleep and re-`touch`
  **after writing the mutation**, not only after restoring it. And when the
  artefact is a linked binary rather than an object, **measure its determinism
  first** — two builds of identical source, identical hash — before trusting it
  as the check; `v8fsd` passes, `ar` archives famously do not.
- **A CASE CAN BE VACUOUS FOR TWO INDEPENDENT REASONS, so finding one is not
  finishing.** "chmod cannot change the file type" survived every mutation
  because `ls` pre-sets `ftype` to `-` at `ls.c:336` and the switch at `:354`
  has **no `default` arm** to overwrite it, so a mode that lost its type bits
  entirely still prints as a plain file — **and** `p9tostat` rebuilds the type
  from `DMDIR` rather than
  passing the server's `IFMT` through, so `ls` on a mount cannot see the
  server's mode word at all. Fixing either alone would have left it green. The
  observable is a **directory**, because `statof` sets DMDIR from
  `(i_mode & IFMT) == IFDIR`.
- **AND A CONTAINMENT CASE CAN SURVIVE A STEP BY TESTING A BETTER PROPERTY.**
  "chmod through a mount does not reach the host" was written when chmod had no
  slot and the guard was the refusal. chmod is a slot now and the call goes to
  the server — and what the case asserts is the thing that always mattered: *a
  mounted path never reaches the host's chmod.* Do not delete such a case as
  stale; re-derive what it discriminates and **measure that** (revert the
  dispatch to `rawsys2(SYS_chmod, vpath(p), m)` and it fires). Only the comment
  was wrong.

**AND §8a step 5i MADE A MOUNT A PLACE A PROGRAM CAN *BE*, WHICH IS ONE
FUNCTION AND FOUR FINDINGS THAT ARE NOT ABOUT IT.** `cd /lib` and then
`cat g`, with `pwd` working inside a mount and `src/libc/gen/getwd.c`
**unmodified**. `v8fs_logical()` folds a path against a tracked working
directory and is hooked in at three places — `v8fs_typefor` for dispatch,
`v8sys_rootpath` for the passthrough type, `p9_t_path` for the server-backed
one — so all 39 path-taking syscalls inherit it unedited, and it is the
IDENTITY with no V8MOUNT set, which is what kept the other sixteen suites at
exactly their old numbers. `shim/kern/NOTES.md` has the whole account. Six
things generalise:

- **THE MODE HAD TO BE THREADED, AND "FOLD ALL BUT THE LAST EVERYWHERE" IS THE
  COMPROMISE THAT LOOKS RIGHT AND IS NOT.** A first attempt folded every path
  whole, which destroys the one thing 5h's `Tunlink` exists to name — `..` as
  an ENTRY, which `rmdir(1)`'s three unlinks and `mv(1)`'s reparenting are made
  of. `unlink("/mnt/d/..")` folded whole is `unlink("/mnt")`: 13 failures and
  `fsck` reporting FILE SYSTEM WAS MODIFIED, so it was backed out rather than
  shipped. But the obvious fix is wrong too, because `getwd.c:41-45` does
  `opendir("..")` and `chdir("..")` **in one loop and requires them to agree** —
  fold all but the last and one reaches the image root while the other reaches
  the jail root. `V8P_ENTRY` is the third value, and `V8P_MAKE` gets its fold
  for free: "key on the parent" and "the last component is a name rather than
  an object" are the same statement. **The DISPATCH needs the mode as much as
  the resolution** — a fully folded `/mnt/..` is the jail root and dispatches
  to passthrough, so the unlink would ask the host to remove the jail's own
  root.
- **A FOLD INTRODUCES A NORMAL FORM, AND EVERYTHING COMPARED AGAINST A FOLDED
  PATH HAS TO BE IN IT.** `vfs.c`'s static table is normalised because it is
  written out by hand; V8MOUNT's prefix is user input and was not. `$TMPDIR`
  ends in a slash on a Mac, so `tests/streams`' own V8MOUNT carries a **double
  slash** — and normalising one side alone made the mount stop claiming its own
  files, so `cat` read the host directory the mount was covering, in the case
  written to prove it could not. The bare-`/` refusal also had to move *after*
  the fold, because the fold is what can produce one: `V8MOUNT=/mnt/..=sock`
  reads as a directory name and normalises to the root.
- **POINTER IDENTITY STOPPED MEANING "RESOLVED", AND THAT WAS AN ESCAPE.**
  Three callers asked "did the rootfs claim this name" with `q != p`, which
  worked only because a `rootpath()` that did not redirect handed back the very
  pointer it was given. With a fold running first it hands back the fold
  buffer, so the test is **true for every path in a mounted process** —
  `pt_path` stops reaching `V8P_MAKE` and `creat("/etc/./x")` inside the jail
  writes to the Mac. `v8sys_rootjailed(q)` asks it against the named buffer,
  which is exact rather than a flag: the function returns that object when and
  only when it redirected, so nothing can go stale between the call and the
  test.
- **MOVING A BUFFER TO FILE SCOPE SILENTLY CHANGES `sizeof`.** `static char
  buf[1024]` inside `v8sys_rootpath` became `char *buf = rootbuf`, and
  `sizeof buf` went 1024 → **8**: the jail root was truncated to seven
  characters and every V8 binary lost its jail. It failed loudly — `cc` could
  not find `/usr/bin/clang` — and that is luck, not design.
- **getpeername(2) REPORTS THE SERVER'S BOUND NAME, NOT THE CLIENT'S, AND THE
  FAILURE IS SILENT DATA LOSS.** Measured both ways round: a client connecting
  to `/private/.../psock` and one connecting to `psock` **both** get `psock`
  back, because a Unix socket's peer address is whatever the listener passed to
  `bind(2)`. So absolutising the client's socket path — which step 5i must do,
  since `getwd` chdirs and a relative name stops resolving mid-walk — cannot
  also change what `ispeer()` compares. The mount keeps two spellings. Get it
  wrong and a 9P socket reads as passthrough, so `echo x > /lib/f` creates the
  file, writes the bytes **into the socket**, and exits 0 leaving it empty.
  There is no identity to fall back on: measured, `fstat` on a connected Unix
  socket reports the socket object (dev −1), not the filesystem node, so **the
  two ends must spell the socket the same way**.
- **AND THE MUTATION THAT DID NOT FIRE FOUND A THIRD KIND OF VACUOUS CASE: THE
  INSTRUMENT WAS POINTED AT THE WRONG PROCESS.** "No V8CWD in a process with no
  mount" survived removing the guard it was written for, and neither recorded
  cause applied — the mutation was strong and the code was live. A shell
  expands `$V8CWD` out of the table it built from its OWN environment at
  startup, *before* the cd, so `sh -c 'cd /etc; echo $V8CWD'` prints nothing
  whatever the shim does. The splice happens in the child of an **exec**, so an
  exec is what has to be observed; the case nests a second shell and the
  mutation then fires on exactly it.

  **One mutation artefact worth knowing before the next run**: a mutation that
  *inserts* a line rather than replacing one shifts every citation below it, so
  `cites.awk` goes red. That is the sweep working, not a finding.

**AND THE GOAL CHANGED, WHICH MADE A MEASUREMENT THAT WAS NEVER TAKEN THE MOST
IMPORTANT ONE.** The owner's target is now *"install a usable V8 world"* -- a
BREADTH requirement, where everything above is depth. Measured against it:
**V8 shipped 286 EXECUTABLE commands** across `/bin`, `/usr/bin` and `/etc`, and
this port installed **91**. (Count executables, not directory entries: a bare
listing also counts `/etc/passwd`, `/etc/group` and `/etc/ttys`, which are data,
and `/etc/utmp`, which libkmemu manufactures at run time so the number depends
on whether anything has run.) Of the 215 missing, **43 have no source at all** (VAX
firmware, data files, the BSD-licensed `more`/`pg` that shipped as
binaries) and 172 do. About 96 are legitimately out of scope -- the 43, eight
PDP-11 cross-tools, eight host-toolchain exceptions, ~14 uucp, ~10 Blit
graphics, ~7 language systems, ~6 kernel grovelers -- so the honest denominator
is **~210**.

**AND THAT LIST SAID `vi` FOR MONTHS, WHICH IS THE MOST EXPENSIVE THING IT
COULD HAVE BEEN WRONG ABOUT.** `vi` has **61 files of source** at
`usr/src/cmd/ex`, because `vi` IS `ex`: `cmp` says the two shipped binaries are
byte-identical, and `ex`'s own makefile is what makes them, with four `ln`
lines producing `vi`, `view`, `edit` and `e`. `more` and `pg` are genuinely
sourceless and the entry was right about them. What put `vi` beside them is
that **`usr/src/cmd` has no directory of that name** -- the sweep looked for
the program's name and the source is filed under the program it is a link to.
So the single most valuable interactive program in the tree was recorded as
unportable on the strength of a `ls` that could not have found it. Same shape
as the four false blockers: *something described as missing that was sitting in
the tree, unread.* **When a program is absent from `cmd/`, check whether it is
a LINK before recording it as sourceless** -- `Admin/dest` and every makefile's
install arm are where the links are declared.

**THE GAP WAS ENTIRELY AT IMPORT, AND NOTHING WAS STUCK.** 91 programs were
imported into `src/cmd` and 98 installed; the only imported-not-installed were
`Admin`, `ccom` and `cpp`, which are toolchain. There was no queue of programs
that failed to build or failed to install -- the pipeline was empty because
nothing had been put in it. Two measurements say it was never a difficulty
problem: PLAN.md's Wave A survey found **156 of 163** single-file `cmd/*.c`
programs compile under v8cc, and rung 5's `Admin/Mk` built, stripped and
installed **fifty** in one invocation. Adding a single-file command is
`tools/import.sh` plus one name in `$(V8BIN)`; `tests/wavea` asserts
imported == installed, so the two cannot drift.

**SO BULK-IMPORT AND LET THE SUITES TRIAGE, because they caught three real bugs
in the first batch and the compiler caught none.** All 37 of Wave A2's
single-file imports compiled and linked with zero failures, and then:

- **`tests/v8ccom`'s rootfs-wide truncation sweep** caught `last` truncating
  `asctime()` and `gmtime()` -- both return pointers, neither was declared, and
  `asctime(gmtime(&delta))+11` is two truncations in one expression.
- **`tests/kmemu`'s libc-import sweep** caught FIVE programs resolving
  `getpwnam`, `getgrgid` and `getpass` from `-lSystem`, which means `id` was
  reading the **Mac's** group database from inside the jail. All three existed
  upstream and had simply never been imported.
- **the linker** caught `cb`, whose `cb.c:3` is `#include "cbtype.c"` -- the
  fourteenth member of the included-non-header list, and upstream's own
  makefile says so outright (`cb.o: cb.c cbtype.c cbtype.h`). Building it as a
  translation unit is a duplicate `_ctype_`.

**AND find(1) HAD ELEVEN LP64 BUGS IN ONE FILE, WITH THE ONE PREDICATE
EVERYBODY TRIES FIRST WORKING BY ACCIDENT.** `struct anode` is
`{ int (*F)(); struct anode *L, *R; }` -- three POINTER-sized members -- and
each predicate reads its operands by punning the node as `{ int f, t, s; }`, so
the second field lands at offset 4, the upper half of the function pointer.
Every predicate reading it was **silently false**: `-type -perm -links -size
-user -group -atime -mtime -ctime -exec -ok`. `-name`'s pun is
`{ int f; char *pat; }` and the pointer's own alignment pushes `pat` to 8, so
it works -- which is why this survived import, compilation and a smoke test.
`find /usr/lib -type f` now returns 74, the same as the Mac's own find.
`src/cmd/find.PORTING.md` has the other three forced changes, including a
directory loop bounded by `lstat`'s `st_size` where only **fstat** reports the
snapshot length here.

**AND libtermlib IS IN -- THE FIRST IMPORT OUT OF `usr/src/lib` -- WITH
`/etc/termcap` BESIDE IT AS DATA.** `/usr/lib/libtermcap.a` built by v8cc,
`/usr/lib/libtermlib.a` a hard link to it as upstream's install makes it, and
`ul(1)` on top: `TERM=vt100 ul` emits the `us=`/`ue=` sequences the database
declares. `src/lib/libtermlib/PORTING.md` and `src/cmd/ul/PORTING.md`. Six
things generalise, and the first is the two-upstream-copies rule paying off
again:

- **THE FINGERPRINT SETTLED WHICH COPY THE BUILD READ, WITHOUT TRUSTING EITHER
  MAKEFILE.** `lib/libtermlib/termcap.c` and `cmd/ex/termlib/termcap.c` differ
  by exactly eleven lines -- a jerq window-size block -- and `ioctl` occurs in
  the whole library ONLY inside them. So an undefined `_ioctl` is a fingerprint
  of that source, and the shipped `usr/lib/libtermcap.a` has one. (The ex copy
  also installs to `$(UCB)/lib` with `UCB=/lusr/ucb`, which is in no part of
  the shipped tree.) `tests/wavea` asserts our archive carries the fingerprint
  too, so building the wrong copy is a failure rather than a discovery.
- **THE HEADER WAS NOT MISSING AND THE PRECEDENT WAS ALREADY IN THE TREE.**
  `termcap.c:230` (upstream `:217`) is `#include <jioctl.h>`, and it was an
  ABSOLUTE path -- and
  INSIDE a function body. A first reading recorded it as a dependency the
  distribution does not ship. Wrong: `jioctl.h` is at `jerq/include/`,
  `tools/import.sh` has a `blit/*|jerq/*` case for it, the Makefile already
  copies it into the rootfs, and **`src/cmd/ls.c:11` carries the identical
  change for the identical reason**. Before recording something as absent,
  grep the WHOLE archive rather than the release subtree -- `blit/` and
  `jerq/` are siblings of `v8/`, not children.
- **A `-D` THAT SELECTS A `case` FAILS SILENTLY, NOT AT COMPILE TIME.**
  Upstream's `CFLAGS= -DCM_N -DCM_GT -DCM_B -DCM_D` each guard one arm of
  `tgoto`'s interpreter; a missing one falls to `default: goto toohard` and
  `tgoto` returns the STRING `"OOPS"`. So the symptom is a cursor that never
  moves, which reads as a broken terminal. Measured: dropping all four takes
  `tgoto.o` from 2376 to 2096 bytes.
- **AND THAT PRODUCED A THIRD REASON FOR A MUTATION NOT FIRING: A STRONG
  MUTATION ON LIVE CODE WITH NO CONSUMER.** Deleting the four flags failed
  NOTHING. The two recorded causes did not apply -- the code is not dead and
  the cases are not vacuous -- because `ul` is the library's only consumer and
  it does no cursor addressing at all, so the largest member had nothing
  looking at it. `tests/wavea/tgotoprobe.c` is the answer, in the shape
  `tests/streams` already uses: when the gap is a missing CONSUMER rather than
  a missing case, a probe is the instrument. Re-mutated, it names each missing
  flag (`want [37] got [OOPS]`) while the unguarded arm keeps passing, which is
  what says it discriminates.
- **A HARD LINK CANNOT BE TESTED BY A STALENESS PROBE, and the case written for
  it was DELETED rather than left red.** `libtermlib.a` is `ln libtermcap.a
  libtermlib.a`, so the two names are ONE INODE -- measured: same inode number,
  and touching either moves both mtimes. The prerequisite *is* the target and
  can never be newer than itself, so `tests/deps`' touch-and-check is not hard
  but impossible. What guards it is an INODE comparison in `tests/wavea`, which
  is also the only thing that would catch the failure that matters: two
  identical copies pass a `cmp` and are not what V8 shipped.
- **THE JERQ BLOCK IS DEAD HERE AND STAYS DEAD, WITH THE REASON WRITTEN DOWN
  RATHER THAN THE GAP.** The shim implements no `JWINSIZE`, so `tgetnum` falls
  through to the database and `ls.c`'s copy of the same block never runs
  either. Implementing it is a step of its own and not a side effect, because
  `struct winsize` is `{ char bytesx, bytesy; ... }` -- the column count is a
  **char**, so a terminal past 127 columns reads NEGATIVE and past 255 wraps.
  That is the 16-bit-range table (`DIRSIZ`, `d_ino`, `p_pid`, `FSNMLG`,
  `u_uid`) arriving in a field narrower still, on a window size a Terminal.app
  user crosses routinely.

**AND ex/vi IS IN, INSTALLED UNDER ALL FOUR NAMES, AND IT EDITS -- WITH ZERO
SOURCE CHANGES.** All 28 objects compile under v8cc with upstream's own
`-DCRYPT -DLISPCODE -DCHDIR -DUCVISUAL -DVFORK -DVMUNIX -DSTDIO -DTABS=8`, the
binary links with an empty `nm -u`, and `ex`, `vi`, `view` and `edit` are one
inode as V8 shipped them. Print, substitute, append, delete and write all
produce exactly the right file. **Every fix it needed was OUTSIDE ex**, which
is the headline: a 19358-line 1981 BSD program needed nothing, and the port
needed three things. `src/cmd/ex/PORTING.md`. Seven things generalise, and the
fourth is the most expensive lesson in this file:

- **`<varargs.h>` WAS STILL 1985's, AND THE CLASS WAS NEVER UNKNOWN.**
  `va_arg` strode `sizeof(mode)` -- 4 for an `int` -- where v8cc spills
  **eight-byte** argument slots. The FORWARD spelling of the identical idiom
  (V8's `printf(fmt, args)` walking `&args`) was fixed years ago in seven
  files, every one taught to walk an eight-byte type; nobody noticed the header
  says the same thing a third way. It survived because **nothing had ever
  included it**: measured, `src/cmd/ex/printf.c` is the only file in `src`,
  `shim` or `compiler` using `va_alist`/`va_dcl`. The `sys/fblk.h` shape --
  a header nobody imported silently stays 1985's -- with the twist that the
  bug class was already this port's most documented one.
  - **The symptom names the stride.** A 4-byte walk over 8-byte slots splices
    two halves of adjacent arguments and **the first argument is always
    right**, so ex printed `"f.txt"` and SIGSEGV'd on the line count. A crash
    right after the first `%s` is this bug until proven otherwise.
  - **`((mode *)(list += SLOT))[-1]` IS THE WRONG FIX** and looks like the
    right one: it indexes back by `sizeof(mode)`, not by SLOT, so an `int`
    lands 4 bytes PAST the slot base -- the upper half, which is 0 for small
    positive values and therefore correct often enough to mislead. Read from
    the base: `(list += sizeof(long), *(mode *)(list - sizeof(long)))`.
- **`exit` AND `_exit` SHARED ONE ARCHIVE MEMBER, AND THE COMMENT BESIDE THEM
  SAID THEY MUST NOT.** A 1980s program that cleans up before leaving defines
  its own `exit()` and finishes with `_exit()`; the linker pulls the member to
  satisfy `_exit` and its `exit` collides with the program's. `stubs.c` said
  *"V8 keeps them in separate files (sys/_exit.s and sys/exit.s) for exactly
  that reason"* three lines above the code that put both in one, and the file's
  own header states the granularity rule and applies it to `signal`. Split now
  (`V8_PART_RAWEXIT`). **The one-object-per-syscall rule is only as good as its
  coarsest member.**
- **`tty_ld`/`ntty_ld` WERE NOT "GENUINELY KERNEL STATE".** This file recorded
  them that way, blocking `init` and `stty` as well. They are **24 initialised
  ints** in `libc/gen/linedis.c`, never imported -- the `getpwnam`/`getgrgid`/
  `getpass` shape again. Verified before trusting, because a table can be dead
  (`dev/conf.c`): `conf/devices` -- the file the BUILD reads -- agrees row for
  row, including `line-discipline 6 ntty` against `int ntty_ld = 6`.
- **A HARNESS THAT BACKGROUNDS ITS ARGUMENT EATS STDIN, AND THE PROGRAM GETS
  THE BLAME. THIS COST A WHOLE DIAGNOSIS, FOUR DOCUMENTS AND A TEST
  EXCLUSION.** ex was recorded here as "executes no command and exits 0" --
  a phantom. The deadline wrapper ran `"$@" &`, and **a backgrounded job in a
  non-interactive shell gets its stdin from `/dev/null`**, so ex read EOF
  immediately. Three-line control: `printf 'X\n' | sh -c 'cat & wait'` prints
  nothing. Deadline helpers elsewhere in this tree are fine because they wrap
  programs that take no stdin; the moment one does, the wrapper is not
  transparent.
  - **AND THE "PROOF" OF CORRUPTION WAS A TWO-VARIABLE COMPARISON.** The
    recorded clue was that an instrumented build -- same sources plus three
    `write(2)` calls -- DID execute commands, which reads as UB moving under a
    changed layout. It changed two things: the instrumented build was run
    **directly** and the clean one **through the wrapper**. The
    instrumentation was never the difference.
  - **THE CHEAP TELL WAS UNREAD: RUN IT TEN TIMES.** Ten runs of the same
    binary gave byte-identical output. Corruption is rarely that stable, and
    determinism costs one command to measure. Do it before writing the word
    "corruption" down.
  - **THIRD INSTRUMENT-AUTHORED FALSE FINDING IN ONE SESSION**, the others
    being the same wrapper reporting a hang as a clean exit and `$OPTS`
    unquoted under zsh. The rule that *an instrument you wrote is a suspect*
    went three for three, and the expensive one is the one that got written
    into four documents before anyone checked it. **Verify a recorded
    diagnosis before building on it** -- including your own, from an hour ago.
- **AND `$OPTS` UNQUOTED COST A WHOLE MEASUREMENT, WHICH THIS FILE WARNS
  ABOUT.** zsh does not word-split, so seven `-D` flags went through as ONE
  argv element and `VMUNIX` was never defined -- making "27 of 28 objects
  compile" a measurement of compiling ex with no options at all, and making
  `ex_subr.c` look like it had a real defect. The tell was a warning reported
  at `ex_tune.h:82` in one run and `:84` in another: `NCARGS` is defined once
  in each arm of `#ifndef VMUNIX`, so **the line number named which branch had
  been taken**. Run loops under `sh`, and when two runs of "the same" command
  disagree, find the byte that differs before theorising.
- **AND THE CARRIAGE RETURNS ARE ex's OWN, REACHABLE FROM THE USER SIDE.** `p`
  output is `aa\n\r`, identically for five terminal types and an unset `TERM`,
  so it is neither the shim nor termcap: `pstart()` (`ex_put.c:852`) clears
  **CRMOD** on fd 1, the kernel stops mapping `\n` to `\r\n`, and ex supplies
  the CR itself. That is upstream's `optimize` option and nothing about it is
  machine-dependent. It is a TEST rather than a sentence because `set
  nooptimize` takes the same early return `pstart()` takes when termcap says
  `NONL`, and the CRs vanish -- both asserted in `tests/wavea`. **Before
  calling an output convention a port defect, look for the option that turns
  it off.**

**AND `src/v8/etc/` BECAME `src/etc/`, BECAUSE THE IMPORT TOOL AND THE TREE HAD
DISAGREED SINCE THE RELEASE SPLIT.** `destfor()` maps `v8/etc/*` to
`v8/src/etc/*`; the four files already there sat at `v8/src/v8/etc/`, which the
Makefile read. Commit `53ccb4b` rewrote the mapping and did not relocate the
files it affected, and **nothing noticed for months because no `/etc` file was
imported in between** -- the unexercised-rule shape, inside the import tool.
The tool was right (`v8/src/v8/` says v8 twice inside an already-v8-scoped
tree, which is the redundancy that commit existed to remove), so the files
moved and six references followed.

## The world is a WORKING COPY of a golden image, and that is what makes it usable

`make install` writes a **pristine** tree to `$(PREFIX)/golden` and never
touches it again. `tools/v8launch.sh` copies it to `$HOME/.v8` on first run,
synthesizes `/etc/passwd` for whoever is running, creates `/usr/<name>`, and
lands them there. Everything after that persists -- a file, a program installed
into `/bin`, an edited `/etc` -- across launches, across the next
`make install`, and across a `make clean` in the build tree.

**IT CANNOT BE THE INSTALLED TREE ITSELF, and the first reason is fatal alone.**
`$PREFIX` defaults under `/usr/local`, which is root-owned on macOS, so the
world would be read-only to the person using it -- and this port's central
claim is that V8 rebuilds V8, which means `Admin/Mk` has to be able to
`cp prog /bin`. A read-only world cannot do the one thing the port exists to
demonstrate.

- **`v8 --reset`** is the only thing that destroys a working copy and requires
  the literal word `RESET`; `--golden` enters the pristine image read-only;
  `--pure` is `V8JAIL=strict`.
- **A HOME DIRECTORY NEEDED BARE `/usr/` IN THE MOUNT TABLE.** `/etc/passwd`
  gives a home of `/usr/<name>` and that path was the **Mac's**, so the world
  had nowhere to live. Safe because the rule is a **union, not a capture**:
  `rootpath()` returns the jailed path only `if (rootfs_has(buf))`, so
  `/usr/include` and `/usr/local` still fall through. The eight specific
  `/usr/*` rows above it are now redundant and kept, because first-match-wins
  gives the same type and deleting them would lose their recorded reasons.
- **AND THE LOGIN NEEDED NO CHANGE TO V8's SOURCE.** A first draft taught
  `v8.c` to read `$HOME`; `tests/jail` caught it in the same run, because a
  bare `v8` inherits the **Mac's** `HOME` and `chdir("/Users/...")` walks out
  of the world -- `/Users` is not a mount-table prefix. `v8(1)` already takes
  the directory as `argv[1]`, so the launcher passes it and `src/cmd/v8.c` is
  untouched. **The environment is the launcher's to set and the argument is the
  program's to take; crossing them made a host variable into a jail escape.**

**`macos(1)` IS THE ESCAPE AND IT SWITCHES WORLDS, NOT BINARIES.** Measured:
this world provides 81 commands and **62 share a name with one the Mac has** --
`make cc sed sh grep sort ls cp test` among them -- so a native build started
from inside finds V8's make and V8's cc. That is correct and is the premise;
the fix is to make the escape *sayable*. It restores `$V8HOSTPATH` before
exec'ing, because a wrapper that only located the Mac's `make` would still hand
it a PATH whose first entries are 1985's, and make's own children would be
wrong. `--pure` refuses it, which is the mode working rather than a gap.

**THE USEFUL FORMULATION: native apps work, native BUILDS do not.** Running an
app is one exec and `python3`, `git` and `node` do not collide at all; building
software is hundreds of PATH lookups against exactly the 62 names V8 owns. So
build on the Mac, work in V8 -- and `macos CMD` for the rest.

**su(1) IS AUTHENTIC AND IT REFUSES, which is the answer to "real root or jail
root" that neither option gave.** It is V8's own source, installs to `/etc`
because `Admin/etcfiles` says so, and `su root` prints `Password: Sorry` --
because `/etc/passwd` carries `*`, which no `crypt` output can equal. That is
upstream's own convention for an account that cannot be logged into, not a
refusal this port invented. `setuid(0)` beneath it is a raw host syscall and
fails EPERM anyway. **There are three different uid-0s here and none of them is
a login**: the host's (real, refused), `v8_foldid`'s in the streams/`/proc`
half (root maps to root, non-root NEVER maps to root), and v8fsd's `u_uid = 0`,
which is deliberate and argued at `shim/kern/sys/main.c:386`.

**AND ARTICLE.md MUST BE UPDATED WITH EVERY CHANGE — A STANDING INSTRUCTION
THAT WAS GIVEN ELEVEN TIMES AND IS ONLY NOW WRITTEN DOWN.** The owner asked for
it once ("write up an ARTICLE.md describing our whole approach"), asked twice
whether it was being kept current, and asked **eight times** "did you update
ARTICLE.md?". It was in the conversation and in no file, so a grep of this
document, PLAN.md and `.claude/` came back empty — and a grep was the wrong
instrument for the question.

**The failure shape is the one this repository documents about itself.**
Measured from the log: the last four commits touching that file changed
**nothing but a number** (`+1/-1` each), while two whole steps went unwritten.
Each time the question was asked, the file was touched and the test count moved
— which looks like compliance and asserts nothing. **ARTICLE.md was the only
artefact here with no guard**, and the one thing about it that *was*
mechanically checked, that count, is precisely the one thing that stayed
current. An unexercised rule cannot be seen to be incomplete, arriving in the
project's own write-up.

So it has a guard now, in `tests/wavea`: the article's stated command count
must equal one **derived from the rootfs**. Three copies of a number agreeing
with each other is the two-copies-of-a-wrong-list trap; the tree is the third
thing that is neither.

**AND WRITING THAT GUARD CORRECTED THE COUNT ITSELF, TWICE.** A listing of
`/bin`, `/usr/bin` and `/etc` is not a count of COMMANDS: it also counts
`/etc/passwd`, `/etc/group`, `/etc/ttys` and `/etc/motd`, which are data. Worse,
it counts `/etc/utmp`, which libkmemu manufactures lazily at first read — so the
number was **139 on a tree that had never been used and 140 after anything ran
`who`**, and the guard would have passed or failed on whether an earlier suite
happened to. Fourth instance of that question. Count `-type f -perm -u+x`:
**286 shipped, 138 installed**, stable.

## Architecture: three layers, three different rules

The single most important thing to get right is **which layer you are editing**,
because they have opposite policies.

**1. Authentic V8 source (`src/`)** — imported from `third_party/`, then
minimally patched. Changes must be forced by the target (LP64, Mach-O, ARM64
ABI), not by taste. Do not modernise K&R declarations, do not add prototypes, do
not "fix" warnings. Every change is recorded in that program's `PORTING.md` with
the reasoning.

**2. New code (`compiler/`, `shim/`)** — written for this port, modern C.
`shim/v8sys/vfs.c` is the **filesystem switch** (PLAN §8a step 2): one mount
table, **three** types behind it — passthrough, `/proc` in `shim/libkmemu/`,
and `/dev/fd`. The
table is the old `v8dirs[]` with a type column — do not add a second prefix list
beside it. `struct v8fstyp` answers to V8's own `struct fstypsw`; where it
departs (descriptors, not inodes) the header says why.

**`/dev/fd` is the cheap type and the instructive one: it implements THREE
operations and inherits the rest.** `t_path` is the identity (there is no host
path), `t_open` is `dup(minor)`, `t_stat` synthesizes a character device
`makedev(40, minor)` — and everything after open is passthrough's *unchanged*,
because a dup'd descriptor **is** an ordinary host descriptor. Giving it a type
of its own would invent a difference the kernel does not have, which is why
`fd_open` never calls `v8fs_bind()`. Row order is load-bearing and asserted:
exact `/dev/fd` → passthrough (the directory is a real directory) *before*
prefix `/dev/fd/` → fdfs, or `ls /dev/fd` asks the descriptor type to open a
name with no minor in it.

**A third type made two rules live that had never been exercised, and both were
incomplete.** `v8s_creat` went straight to `rawsys3(SYS_open, mkpath(path))` —
path resolution without dispatch, so **no second type could ever see a creat**;
`/proc` is read-only, so nothing noticed. And `v8s_dup`/`v8s_dup2` dropped the
descriptor's type, so `dup()` of an open `/proc` file returned one whose reads
went to the host. Both are the `v8s_mknod` shape: an unexercised rule cannot be
seen to be incomplete. Expect a fourth type to find a third.

Dispatch is **by descriptor, not by operation**, and `ioctl` is where that stops
being a detail: `v8s_ioctl` routes on `v8fs_fdtype(fd)`, so `PIOCGETPR` on an
ordinary file is `ENOTTY` and `TIOCGETP` on a `/proc` descriptor is `EINVAL` —
the same command number, two paths, which is the pair `tests/kmemu` asserts. The
sgtty/termios translation in `ioctl.c` did not move when the slot arrived; it
*became* the passthrough type's `t_ioctl`, which is what it always was.
`compiler/ccom-arm64/` is the machine-dependent half of the compiler, written
*inside ccom's own architecture* (`local.c`, `local2.c`, `gencode.c`, `macdefs.h`
— the same file names and hooks pcc expects). `shim/v8sys/` is `libv8sys`,
standing in for the VAX kernel.

**3. `third_party/`** — read-only, never edited in place. To bring a file into
`src/`: `tools/import.sh v8/usr/src/cmd/cpp`, which records the upstream blob
hash in a `PROVENANCE` file so the diff against pristine V8 stays reconstructible.

### Where a command is INSTALLED is upstream's decision, and upstream wrote it down

This port put everything in `/bin` for as long as `/bin` was the only directory
it had, and that was wrong for **forty-one commands**. V8's `/bin` is a 56-entry
root-filesystem set from when `/` had to fit on one pack; `wc`, `tr`, `sort`,
`sed`, `yacc`, `lex`, `dc` and most of Wave A live in `/usr/bin`. So `ls /bin`
inside the jail listed a machine that never existed.

Two upstream sources say so and they agree:
`third_party/.../cmd/Admin/dest` — a shell script that looks a name up in
`binfiles`, `etcfiles`, `libfiles` and falls through to `/usr/bin`, and which
`Admin/Mk` calls for each of the bare `cmd/*.c` programs — and the **shipped
tree itself**, `third_party/.../v8/{bin,usr/bin,lib,etc}`.

`Admin/Mk` is worth knowing about for its own sake: it is the build description
for the half of `cmd/` that has no makefile, and for a *directory* it runs
`make clean && make && make install`, which is exactly the rung-5 mechanism.

**THERE IS A THIRD SOURCE, AND IT IS RIGHT WHERE `dest` FALLS THROUGH.**
**Twenty** of the 22 imported makefiles say where they install themselves —
`mv lex $(DESTDIR)/usr/bin`, `cp ps /bin`, `D=/etc/quot`. They agree with the
tables on **eighteen**, and the two they do not are both programs that appear
in **no table at all**, so `Admin/dest` is answering by *fall-through* — which is "nobody said",
not "V8 said /usr/bin":

| | its makefile | shipped tree | `Admin/dest` |
|---|---|---|---|
| `cpp` | `/lib` | `/lib` | `/usr/bin` |
| `dump` | `/etc` | not shipped | `/usr/bin` |

`cpp` settles it: two sources against the fall-through. This port already puts
`cpp` in `/lib`, but by accident — it is a toolchain target with its own rule
and never goes through `$(call v8dest,...)`. `dump` had no such accident, so the
Makefile's `$(MKFILEETC)` follows the makefile on purpose, which also puts it
beside its two siblings `restor` and `dumpdir`. `tests/wavea` recomputes the
whole set from all twenty makefiles — expanding `$(VAR)` and `$B`, because
`tsort`'s `B = /usr/bin` read literally looks like a third disagreement — and
fails if it is anything but those two.

The Makefile **derives** the destination — `$(call v8dest,NAME)` reads Bell
Labs' tables at build time — so a newly imported command lands where V8 put it
without anyone deciding. `tests/wavea` asserts the result against the *other*
source, the shipped directories, so the two check each other rather than one
being read back twice.

Two traps came out of doing it:

- **`$(strip)` in `v8dest` is load-bearing.** A backslash-newline inside a
  variable's *value* becomes a space and the next line's indentation is kept, so
  without it the function returns `"          etc"` and make splits
  `$(ROOTFS)/          etc/mkfs` into two targets. It presents as
  `warning: overriding commands for target .../rootfs/` and
  `No rule to make target 'usr/bin/touch'`, neither of which names the cause.
- **A MISS IS NOT AN ESCAPE**, and `V8JAIL=strict` treated it as one. V8's `sh`
  searches PATH by calling `execve` on each directory in turn, so with
  `PATH=/bin:/usr/bin` every `/usr/bin` command probes `/bin/<name>` first — and
  the shim reported an escape for a file the Mac does not have either. Invisible
  while every tool lived in `/bin` and the first probe always hit. `v8s_execve`
  now refuses quietly when the host has no such file and loudly when it does,
  which is the only distinction `V8JAIL` was ever making.

### The deliberate exception list

`as`, `ld`, `ar`, `strip`, `nm` are the **host's**, because the object format is
Mach-O; porting V8's a.out assembler and link editor is out of scope. The `cc`
driver execs `clang` for assembly and linking. This is a decision, not a gap —
do not "fix" it. Everything else is ported rather than passed through; host
passthrough is meant to be the exception, not the rule.

**The prose list and `hosttools[]` are not the same list, deliberately.**
`sanctioned()` in `shim/v8sys/syscall.c` permits only what something actually
execs — for a long time that was `/usr/bin/clang` alone, on the reasoning that
`cc` reaches the rest through it. `as` joined it when `sh`'s `:fix` invoked the
assembler by name, which is the first thing in this port to do so. `strip`
joined it the same way, when Bell Labs' `Admin/Mk` ran: its `install()` is
`strip $1 && cp $1 $2`, so a refused `strip` short-circuits the `&&` and
**nothing is installed**, which reads as a build failure rather than as a jail
decision. `nm` is still absent and `tests/jail` asserts it is *refused*, so the
array cannot quietly drift into "everything the prose mentions". Same shape as
`v8s_mknod` passing its path unresolved: **an unexercised rule cannot be seen to
be incomplete.**

How a jailed `sh` reaches one of these is worth knowing, because it is not a
second mechanism: `sh` searches `PATH=/bin:/usr/bin:/etc` by `execve`, `/bin/`
and `/usr/bin/` are **union** mounts, and a name the rootfs half does not have
falls through to the host's — so `hosttools[]` is the gate on the fall-through
rather than a special case bolted beside it. `/bin/strip` is a quiet miss (the
Mac has none either) and `/usr/bin/strip` is the hit.

**`shim/libkmemu/` may link host libc** — the one component that may, and it is
built: `who` runs, with **no changes to `who.c` at all**, because the shim
manufactures `/etc/utmp` when a reader opens it rather than giving the program a
function to call. `df`, `load`, `w` and `uptime` followed; `load` also needed no
source change, because the shim manufactures a *kernel* — a namelist at `/unix`
and a `/dev/kmem` with the data where the namelist says it is, both generated
from one table in `shim/libkmemu/kmem.c` so they cannot drift apart.
`shim/libkmemu/NOTES.md` has the whole story. It answers
"what is running / what is mounted / who is logged in" through documented,
stable interfaces (`getutxent`, `getfsstat`, `proc_listpids`, `sysctl`) so
Phase 4's grovelers can be honest. The alternative was parsing
`/var/run/utmpx` by hand to keep `libv8sys` raw-syscall-only, and that file's
layout is private and undocumented — a wrong guess there yields a `who` that
looks right and lies. Reaching for libc here narrows what the port depends on.

The boundary matters more than the exception: **per-file, not per-shim**.
Everything in `shim/v8sys/` stays raw-syscall-only (`dir.c` says so at its top,
and that still holds). libc is for reading system facts, never for file I/O,
strings, or anything `rawsys.h` already covers — that would be convenience, and
convenience is how an exception list stops meaning anything. PLAN.md §7 has the
reasoning, and `tests/kmemu` turns it into an assertion.

**TWO SENTENCES HERE WERE STALE, and both went stale the same way — true when
libkmemu was `utmp.c` alone, made false by Phase 4's `df`/`load`/`w` and by
`ps`, while the rule they described stayed exactly right.** Re-measured:

- This said "`who` imports exactly `_setutxent _getutxent _endutxent` and
  nothing else does". `nm -u` says `who` imports **all eight** of
  `endutxent getfsstat getutxent proc_listpids proc_pidinfo setutxent statfs
  sysctlbyname`, and so do **`df`, `load`, `ps`, `uptime` and `w`** — six
  binaries, identically, because
  `KMEMU_LDADD` is `-Wl,-force_load` and pulls every archive member in. What
  `tests/kmemu` actually asserts is the **set** (`KMEMU_IMPORTS`, those eight)
  (This sentence has now been corrected TWICE and both times the same way —
  it named four binaries when six import the set, and `df` and `load` are
  called grovelers two paragraphs above. Re-measure the set, do not edit the
  list.) Plus the thing that carries the real weight: no other Mach-O in the
  rootfs
  imports anything at all — **with one stated exemption**: `lib/ccom` imports
  36 names and `lib/cpp` 26, because those two are still the clang-built
  stage-0 binaries (see the install note below), and `tests/kmemu` exempts them
  by name with that reason. `nm -u rootfs/bin/cat` is empty, measured.
- And it said "Only `utmp.c` names a libc function" inside libkmemu. **Four
  do** — `utmp.c` (the `getutxent` trio), `mtab.c` (`getfsstat`, `statfs`),
  `procfs.c` (`proc_listpids`, `proc_pidinfo`, `sysctlbyname`) and `kmem.c`
  (`sysctlbyname`). Every one of them is *reading a system fact*, which is the
  rule.

**AND THIS ENTRY SAID "FIVE", COUNTING `synth.c`, WHICH CALLS NONE OF THEM.**
Its only occurrences of `getutxent`, `getfsstat` and `sysctl` are lines 13-14
of its own header comment, where it lists the sanctioned interfaces as prose.
The sweep matched **the documentation of the rule and counted it as an instance
of the rule** — the same shape as the `time(&` sweep whose population grew every
time someone wrote a find down. `kmem.c` was credited with a bare `sysctl` it
never calls, for the same reason.

**AND THE SAME SHAPE HAS NOW COST REAL WORK, WHICH THE FIRST TWO DID NOT.**
`src/libc/gen/PORTING.md` recorded that `v8sys_fold_ino` must stay a pure
function "because folded values are written into files
(`shim/libkmemu/NOTES.md:247` — `e_tdev` in the manufactured `/etc/utmp`) that
another process reads", and used that to rule out the one easy fix for the
`pwd` collision. Every part of it is false: the function has **three** call
sites (`dir.c:467`, `dir.c:469`, `syscall.c:1841`) and none is in `libkmemu` —
the two `grep` hits there are comments; `NOTES.md:247` is about `u_ttyino` in
`/proc`'s u-area, says it is left zero, and calls filling it hypothetical; and
V8's `struct utmp` is `{ut_line[8], ut_name[8], ut_time}`, 24 bytes, with no
inode field. `e_tdev` is in no source file at all. **A recorded constraint that
blocks work has to be verified before it is obeyed** — read the citation, not
the sentence citing it. **And the cost is now countable rather than
hypothetical: with the constraint gone, the fix was one function and one
afternoon, and `pwd` went from 32-of-60 to 1752-of-1752.**

The rule is intact and `synth.c` is a *better* demonstration than the sentence
claimed, not a worse one: it is 100% `rawsys` and 0% libc — **8** call sites,
zero `open`/`write`/`fopen`. (That was "10 `rawsys` calls"; `grep -c` counts
lines, and three of them are the `#include` and two comments. A `grep -c` is a
line count, not a call count.) What has to be re-measured is which files *read*
a fact through libc, because that is the boundary an auditor would go looking
for — and `synth.c` is not one of them.

### The bootstrap ladder

Order matters and is not obvious. Each rung is built by the one above:

```
0 seed   host clang + host make + host yacc -> cpp, ccom-arm64, cc-seed, libv8sys, crt0
1 tools  cc-seed -> libv8c -> v8cc -> yacc -> lex -> make   (make needs yacc: it has gram.y)
2 jail   v8cc -> /bin: sh and the filters
3 close  regenerate cpp's grammar with V8 yacc; fixpoint v8cc1 == v8cc2
4 hand   V8 make rebuilds the compiler, inside the jail
5 world  V8 make + each program's own authentic makefile
```

**There is one cycle in this build, and `cc-seed` is how it is cut.** The
installed driver is a V8 binary, so it must be *linked* against `libv8c` — and
`libv8c` must be *compiled* by a driver. So `cc-seed` (the same `cc.c` built by
clang, never installed) compiles `libv8c`, `libv8c` links the real driver, and
the real driver compiles everything else. Both drivers exec the same `cpp` and
`ccom`, so the objects are identical either way; only the process differs.
Rung 4 is where the seed stops being needed.

**Rung 3 is closed.** All twenty translation units of `cpp` and `ccom` compile
under v8cc and link freestanding, the self-hosted `cpp` matches the stage-0 one
byte for byte, and the compiler reproduces itself: ccom2 (built by ccom1) and
ccom3 (built by ccom2) generate byte-identical assembly. `tests/selfhost`
asserts it. Note ccom1 == ccom2 is *false* by two instructions and that is
correct — ccom1 inherits one generation of the clang-built stage-0's beliefs,
and stage 2 washes it out. That is what a three-stage bootstrap is for.

**Rung 4 is closed too.** `tests/jail` drives a full rebuild of `ccom` with
V8's make, under `V8JAIL=strict`: V8's make reads the makefile, V8's sh runs the
recipes, V8's cc drives V8's cpp and ccom, and the only thing permitted out is
the documented as/ld exception. The makefile is plain 1985 make — no pattern
rules, no automatic variables past `$@`. The result compiles real source, the
build settles, and a second build from clean generates identical code.

**Rung 5 is demonstrated on eighteen programs, chosen for their makefile idioms
rather than their size**: `lex` (dependency line on `#include`d non-headers),
`sed` (target, prerequisites and recipe on one line; `*.o` glob), `fmt` (macro
expansion), `tsort` (`.SUFFIXES` and a `.c.o` suffix rule, no explicit object
rules), `tbl` (`t?.o` glob, three flags at once, a 22-target dependency line on
`t..c`), `yacc` (`$(CC)`, `y?.o`, dependencies on `dextern` and `files`), `spell`
(four programs from one makefile — `spellprog` specifically, since `all`
regenerates the word lists), `man` (the minimal case, one rule), `troff`
(**14 objects** out of a 22-file directory — the largest link here beside
`eqn`'s 22, and scale is its own idiom), `refer` (four programs
and an `#include`d non-header), `ps` (V8 make's `&`, which nothing else here
uses), `load` and `w` (two lines each, and grovelers — see below), `make`
(**V8's make building V8's make from V8's makefile** — the only entry that
closes a loop), `eqn` (whose target is `a.out`, not its own name), `pic` and
`grap` (`-lm`), and `quot` — **the first image tool to get here**, and it is
here by a measured no-op rather than an exemption: see the `$(IMGBIN)` note
below. V8's make handled every one unchanged.

**AND ON FIFTY MORE THAT HAVE NO MAKEFILE AT ALL, through `Admin/Mk`.** That is
the other half of `cmd/`, and its build description is not a makefile but a
shell script: for each bare `*.c` upstream runs
`eval D=\`Admin/dest $B\`; cc $CFLAGS -o $B $B.c`, then
`strip $1 && cp $1 $2`, then `rm -f $B.o $B`. Run verbatim inside the jail under
`V8JAIL=strict`, it builds, installs and cleans up all fifty of this port's
single-file commands — exercising V8's `sh` (`set -p`, functions, backquotes,
`eval`, `case`), two nested shell scripts with **no `#!` line**, and V8's `cc`
driving V8's `cpp` and `ccom`. Three host execs in total: `clang` twice per
program and `strip` once.

Two things it gives that the eighteen could not. **`Admin/dest` and the
Makefile's `$(call v8dest,...)` are two independent derivations of the same
answer**, one in V8's shell at run time and one in GNU make at build time, and
`tests/jail` compares them for all fifty — the only thing that would notice the
deliberately-omitted `ulibfiles` arm starting to matter. And it needed
`/usr/src/` in the mount table, because `cd /usr/src/cmd` is the one absolute
path in the script; `$(SRCTREE)` stages the sources there, so **the V8 world can
now rebuild the half of itself that never had a makefile.**

Writing that test taught one thing worth repeating: **`Mk` runs inside the world
it is rebuilding.** `Admin/lookline` *is* `grep`, the loop `echo`s, every name
goes through `basename`, and each program ends in `rm` — so a test that deletes
all fifty binaries before the run kills the script on its second iteration, and
the failure reads like a compiler error. Rebuilding those four over themselves
is fine; removing them first is not.

Three things came out of that which our own rules could not have surfaced.
`sed` found a *driver* gap — `-n`, the VAX shared-text flag, now accepted and
ignored like `-O`. `tbl` and `yacc` prove V8's make gets the
`#include`d-non-header dependency lines right, which means **the knowledge our
Makefile had to be told was in the tree the whole time.** And four makefiles
died on `Cannot load mv` — which is how it was discovered that **eleven
commands had been imported and never built**, so the V8 world had no `cp`, no
`mv` and no `sed`. That is the argument for doing the rest of them: upstream's
makefiles exercise the toolchain in ways our rules never do, because our rules
were written to work.

**`load` and `w` are in the sweep as grovelers, and the distinction they make
visible is worth keeping.** Upstream's makefile knows nothing about `libkmemu`,
which this port invented, so what rung 5 builds is a real program that cannot
answer — `w` says `No mem`. Rung 5 is a claim about the build *description*
being Bell Labs', not about the binary being the installed one.

**`who` through `Admin/Mk` is the third instance and the cleanest**, because
nothing about the description is deficient: `cc -Od2 -o who who.c` is complete
and correct, and the binary says `who: cannot open /etc/utmp`. `libkmemu`
reaches the link through the Makefile's groveler rules and **deliberately not
through the `cc` driver's default library list** — putting it there would make
every V8 binary import `libSystem` for facts it never asks about, which is the
same reasoning `noprocfs.c` records. `nm -u` on the Mk-built `who` is empty
where ours has `_setutxent`/`_getutxent`/`_endutxent`. `tests/jail` asserts the
pair — one binary answers and the other says it cannot, on the same host,
seconds apart — so the difference is visibly in the build description.

**FOUR OF THE SEVEN "BLOCKERS" WERE WRONG, AND TWO WERE BUGS IN THE SWEEP.**
The table that stood here named a blocker per program; re-measured, most of it
was false, and the errors have one shape — **something described as *generated*
or *missing* that was sitting in the tree, unread.** `make` was blocked on
"generated `ident.c`": `ident.c` is checked in, and the real cause was the
sweep not copying `defs`, an `#include`d non-header. `eqn` was blocked on "a
link failure": its target is `a.out`, and asking for `eqn` fell through to a
built-in rule that linked one object. Both build now, unchanged. Before
recording a program as blocked, run its makefile and read the error — a wrong
target and a missing input both produce output that reads like a port bug.

**`-lm` was the real one, and the answer was inside the archive.** Eleven
upstream makefiles link `-lm`, which reads as a claim that V8 had a math
library. `v8/usr/lib/libm.a` is **216 bytes**: one member, `dummy.o`, whose
entire symbol table is `_________`. It defines nothing — V8's math is in
`libc/math`, so `-lm` there linked an empty archive. Our driver handed the flag
to clang, which answered with the SDK's libm, a **libSystem re-export ahead of
`libv8c`**, and `_errno` resolved to an indirect symbol with no address. Behind
the specific bug was a general one: **every `-l` escaped to the host SDK.**
`libpath()` in `src/cmd/cc.c` resolves `-lNAME` against
`$V8ROOT/usr/lib/libNAME.a` first, by the same union rule `rootpath()` uses;
`shim/libm/dummy.c` reproduces the stub so the driver needs no special case.

**Where rung 5 genuinely stops: 1985 wanted its data in read-only text.** A
build description that names the target machine cannot be used unchanged, and
there are **two ways it can do that** — a flag and a helper. `src/cmd/cpp/Makefile`
opens `CFLAGS=-O -Dunix=1 -Dvax=1 ...`, and `cpp.c` tests `vax` in three places,
so running it as written builds a VAX preprocessor; it is the only makefile in
the tree that does (`grep -l 'Dvax' src/cmd/*/[Mm]akefile`). But `cpp` *also*
runs `:yyfix`, which lifts the yacc tables into `rodata.c` for `cc -R` — and
V8's driver passes `-R` straight to `as`. `sh` runs `:fix`, which compiles to
assembly, rewrites `.data` to `.text` with `ed`, and reassembles. **Those are
the same optimisation**: put initialised data in shared read-only text, which a
VAX gave and an arm64 Mach-O structurally cannot. `msg.c`'s `commands[]` is a
table of pointers, an initialised pointer in `__TEXT` is a text relocation, and
`-no_pie` is *ignored for arm64* — so it is not a flag away from working.
`tests/jail` asserts that boundary rather than leaving `sh` unmentioned. Do not
let a green rung-5 test tempt you into pretending such a build ran unmodified.

**`df` is the third stop and the instructive one: OUR source change broke a
makefile that was fine.** It is grouped with `load` and `w` as "blocked on
`libkmemu`", and that flattens the distinction those two exist to make. `load`
and `w` are unmodified, so upstream's makefiles link them and the result is a
real program that cannot answer (`No mem`). `df.c` was changed *by this port*
to call `kmemu_fsstat` — 0 occurrences upstream — so the link fails outright.
PLAN.md §4a has all three.

`tests/jail` builds `lex` from `src/cmd/lex/Makefile`
— upstream V8, unmodified — with V8's make, cc and yacc, in a directory holding
nothing but V8 sources, under `V8JAIL=strict`. That makefile is the one worth
proving: its line 11 declares `lmain.o: lmain.c ldefs.c once.c`, the dependency
whose absence caused this port's worst bug.

**What is left is FOUR programs, not three, and the fourth had no entry at
all.** There are 22 imported makefiles and rung 5 covers 18, so the uncovered
set is `cpp`, `sh`, `df` and **`dump`** — and none of the four is mechanical.
`cpp` and `sh` are the shared-text stop above; `df` needs its numbers to come
from `/dev/kmem` rather than from a call this port added; and `dump` is in
`$(IMGBIN)`, compiled `-DDIRSIZ=$(IMGDIRSIZ)`, so its own build description —
which passes no `-D` — would produce a *different program*, exactly the
argument `quot` escaped by measuring its object byte-identical either way.
That reason was already written down under `$(IMGBIN)`; what was missing is
that the two sentences never met, so the tally said three.

**Phase 6 is done.** `make install PREFIX=... BINDIR_HOST=...` (defaults
`/usr/local/v8` and `/usr/local/bin`) rebuilds with the prefix stamped into
every binary and writes a launcher script. `v8` drops you at `/` in a world
whose `/bin`, `/etc`, `pwd` and compiler are all V8's, with the Mac still
reachable through PATH.

The install rebuild starts with `make clean`, and that is load-bearing: make
does not track a change to `V8ROOT_DEFAULT` because it is a recipe flag rather
than a prerequisite, so an incremental build would install binaries still
carrying the build tree's path. The *installed* `cpp` and `ccom`
are still the clang-built ones (`nm` shows 0 V8 symbols); they sit inside the
rootfs without obeying the jail. Swapping them for the self-hosted ones is
Phase 6 work, not a ladder rung.

### The jail

`rootpath()` in `shim/v8sys/syscall.c` is a chroot implemented in the shim, not
the kernel: `chroot(2)` needs root, and every V8 binary is a Mach-O linked
against `libSystem`, so a real chroot would need the SIP-protected dyld cache
inside it. Consequence, and it is load-bearing: the jail is **per-binary, not
per-process-tree**. Host binaries never call `rootpath()`, so they see the real
macOS with no special casing; anything `cc` produces links `libv8sys`, so it is
jailed by construction.

The mount table is `mounts[]` in `shim/v8sys/vfs.c`, and **`/usr/src/` is on it
so V8's own source tree is inside the jail.** That is a rung-5 requirement
rather than decoration: `Admin/Mk` opens with `cd /usr/src/cmd`, the only
absolute path in it, so without the row the V8 world could rebuild a program
that has a makefile and not one that has none. Adding a prefix here is cheap and
its blast radius is the whole world — everything jailed then sees `/usr/src`
redirected — so weigh a new row against what actually needs it.

**A path resolver that keys on existence cannot resolve a creation, and that
gap was one-directional.** `rootpath()` redirects a path whose rootfs copy
exists — right for a reader, unanswerable for a name that does not exist yet —
so `creat("/etc/x")` went to the *Mac's* `/etc` while `open("/etc/x", 0)` read
the jail's. It always failed on macOS, and it failed with **EACCES**, which
reads as a permissions problem rather than as a missing jail. `v8s_creat`,
`v8s_link` and `v8s_mkdir` resolved nothing at all. Closed: `V8P_MAKE` keys on
the *parent*, and `mkpath()` (LOOK first, then MAKE) is what every creating
syscall uses. Readers keep `vpath()`, so the union is unchanged. `shim/NOTES.md`.

`v8s_mknod` was the LAST creating syscall still passing its path raw, and it
survived the V8P_MAKE conversion because nothing called it: `mkdir(1)` is the
only user of `mknod` in the tree, and `mkdir(1)` was one of eleven commands
imported and never built. **An unreachable syscall cannot be seen to be wrong.**
`mkdir(1)` shows both halves in one run — its `access()` asked the jail and its
`mknod` asked the host — and it failed closed only because every jailed prefix
is SIP-protected here.

**AND TWO SYSCALLS RESOLVED NOTHING AT ALL, WHICH IS THE SAME SHAPE ONE STEP
FURTHER ON.** `v8s_mknod` above passed its path *unresolved*; `v8s_readlink`
and `v8s_utime` did not call `vpath()` **or** `mkpath()` — every other
path-taking syscall in `syscall.c` does. So `ls -l` on a jailed symlink read
the Mac's (`ls.c:365`) and `mv` inside the jail stamped the Mac's file of that
name (`mv.c:129`). Two directions, and the quiet one is worse: loud `ENOENT`
where the host has no such name, silently wrong where it does — and `/etc`,
`/bin`, `/usr/bin` and `/usr/lib` are all names the Mac also has. `v8s_symlink`
twelve lines below `readlink` **does** resolve, with `mkpath`, so the port
could create a jailed symlink and then not read it back.

**AND tests/v8sys COULD NOT HAVE FOUND EITHER, BECAUSE IT HAD BEEN RUNNING WITH
THE JAIL OFF FOR ITS WHOLE LIFE.** The `test-v8sys` rule does not pass
`$(SHIMFLAGS)`, so the binary carried no `-DV8ROOT_DEFAULT`, so `v8root()`
returned 0 — and this file's own first sentence about the shim says what that
means: "when unset it silently falls back to the host filesystem". 164 cases
about the shim's syscalls, none of them able to see a syscall that resolved
nothing. The recipe sets `V8ROOT` now; every existing case still passes, which
is what says the jail was genuinely inert rather than merely unexercised.

**AND A THIRD INSTANCE OF THE SAME ROOT CAUSE CAME OUT OF FIXING THEM.**
`rootpath()` decides whether the rootfs has a name with
`rawsys2(SYS_access, buf, 0)`, and **`access(2)` follows a symlink** — so a
jailed symlink whose target does not exist reads as absent and *every*
operation on it falls through to the host. Not just the two above:
`v8s_unlink` cannot remove one either, which is how it was found, when a case
asserting the `readlink` limit left the link behind and broke the next run.
**FIXED NOW, AND THE MEASUREMENT IS WHAT MADE IT SAFE TO TOUCH THE MOST
LOAD-BEARING FUNCTION IN THE SHIM.** The predicate is a raw `lstat`, which
answers the question the union rule actually asks — *does the rootfs have this
NAME* — rather than "is there a reachable object at the end of it". Three
things settled it, and none is an argument:

- **The two predicates disagree on exactly FOUR shapes on this host, and every
  one is "the last component is a symlink whose resolution fails"**: a dangling
  absolute link (`ENOENT`), a dangling relative one (`ENOENT`), a loop
  (`ELOOP`), and a link whose target is behind an unsearchable directory
  (`EACCES`). Everywhere else they agree — a file behind `chmod 000` fails
  *both*, because `lstat` needs search permission on the prefix too. So a fix
  keyed on "access said ENOENT" would have covered half the class; keying on
  the **question** covers all of it.
- **The change is MONOTONE**, which is what bounds the blast radius: there is
  no case where `access` succeeds and `lstat` fails, so the union can only ever
  resolve *more* names into the jail, never fewer. (The candidate
  counterexample is a trailing slash on a link to a *directory*, where `lstat`
  follows; measured, both succeed.)
- **`wavea`, `jail` and `kmemu` are unchanged at 124/128/146**, run before and
  after — the diff the task asked for rather than a green run.

**AND THE NOTE RECORDING THE FIX WAS WRONG ABOUT WHICH MODE CHANGES.** It said
only `V8P_LOOK`, "because the parent case is a directory and cannot be a
dangling link". Nothing stops it being one — and with `access` the parent then
reads as absent, the path falls through, and `creat("/etc/x")` writes to the
**Mac's** `/etc`. That is the escape direction, in the mode that exists to stop
exactly it. Both modes changed.

**AND THE CASE FOR THAT SECOND HALF WAS VACUOUS ON ITS FIRST DRAFT.** Written
as a create, it stayed green with the predicate reverted, because this Mac has
no `/usr/src` — so the create fails whichever world it lands in and the guard
is indistinguishable from the absence. Same trap as `chmod 777 /mnt`. The fix
is to assert the **resolution**: `v8sys_rootpath()` is not static, and whether
it returns a `$V8ROOT`-prefixed path or the bare one *is* the behaviour that
changed. No host directory required. Eight of eleven cases now fire on the
revert.

`v8s_execve` also interprets `#!` itself. The kernel would resolve a shebang
against the real filesystem before the shim saw it, so every shell script ran
under the Mac's shell — the last hole in the chroot, and the most invisible.
Watch for the aliasing trap when touching it: `rootpath()` returns a pointer
into its own static buffer, so holding two results at once silently gives you
the same string twice.

## The bug classes that actually bite

**LP64 is the dominant one.** V8 assumes `sizeof(int) == sizeof(char *)`. The
tree calls `malloc` without declaring it and casts the `int` result to a pointer,
and undeclared K&R parameters are `int` but routinely hold pointers — common
enough that the compiler widens them deliberately, at `acctype()` in
`compiler/ccom-arm64/gencode.c`. That widening must not reach an int *member* of
an aggregate parameter, and `arm64_aggparam()` in `local.c` is what stops it:
`bfcode()` records the byte ranges of the aggregate parameters from the declared
types it is handed, and `acctype()` asks. Symptoms are wild pointers and heap
corruption far from the cause. When something is mysteriously broken, check
widths first.

One shape of it is worth naming because it hides in a *declaration* rather than
in code: **a yacc token declared with a scalar type while the lexer stores a
pointer into `yylval.p`.** On the VAX `.i` and `.p` were the same four bytes;
on LP64 the address loses its top half. Found in `pic` (`TROFF`) and then, by
sweeping, in `grap` (`PIC`) — neither had ever been reached by any input in the
tree. The whole-tree table is in `src/cmd/grap/PORTING.md`; only grammars that
declare types can have it, and the untyped ones are already covered globally by
this port's change to `src/cmd/yacc/y2.c:318` (`#define YYSTYPE long`, not
`int`). Re-run the sweep after importing any program with a grammar:

```bash
grep -n 'yylval\.p *=' src/cmd/*/*.l
grep -nE '^%(union|type|token)[[:blank:]]*<' src/cmd/*/*.y
```

**AND THAT SWEEP USED TO SAY `[ \t]`, WHICH MADE ITS ANSWER A PROPERTY OF WHICH
`grep` YOU HAVE.** `\t` inside a bracket expression is a GNU/ugrep extension.
POSIX reads `[ \t]` as three literal characters — space, **backslash**, `t` —
so BSD grep, which is what a stock macOS and the CI runner have, matches
neither a tab nor much else you meant. Measured on this tree, same command,
two greps:

| sweep | BSD grep | ugrep | after `[[:blank:]]` |
|---|---|---|---|
| `^%(union\|type\|token)[ \t]*<` | **3** | 62 | 62 both |
| `#[ \t]*include[ \t]*"[^"]*"` | 71 | 69 | 69 both |

The yacc one is the serious half: **59 of the 62 typed-token declarations in
the tree put a TAB after `%token`**, so on a stock macOS the sweep documented
to catch this very bug class found 3 — and `grap.y:9` is
`%token<TAB><p><TAB>PIC`, i.e. **the sweep would not have found the instance it
was written for.** The three it did find are all `make/gram.y`, which has the
bug in neither place.

The `#include` one fails the other way and shows why the reading is worse than
"tabs are missed": the **backslash** in the class matched the escape in
`printf("# include \"mfile2.h\"")` (`ccom/common/sty.y:979`,
`lex/header.c:7`), and that same escaped quote is what let the hits slip past
the `grep -v '\.h"'` filter. One stray character produced two cooperating
faults and a plausible number.

Every sweep in this file now spells it `[[:blank:]]` (and `[[:space:]]` for the
one `\s`), which is POSIX and agrees under both. Two rules: **write the POSIX
class, never `\t` or `\s`**, and when a sweep's count is load-bearing, **run it
under `/usr/bin/grep` as well** — that is the one CI has. And note the shape:
this is the machine-property class the test suites are already swept for,
arriving in a *documented sweep*, where nothing goes red.

**AND THE SAME CHARACTER IS UNSAFE IN `sed` NO MATTER WHICH `grep` IS
INSTALLED.** BSD `sed` has no `\t` extension at all, so `sed 's/[ \t]*(//'`
turns `iget(` into `ige` — it eats the trailing `t`. `awk` interprets it. This
was found by writing a callee-counting script for the §8a step 5 survey below,
whose first run reported `ige`, `ipu`, `iupda` and `findmoun` as the names V8
calls.

**A K&R PARAMETER'S ADDRESS IS NOT AN ARRAY, because the argument slot changed
size.** `mkfs`'s `gmode()` is `return((&m0)[i])` over four undeclared
parameters: take the address of the first and index forward through the rest.
Exact on a VAX — `ARGINIT 32`, arguments four bytes apart, `&m0` an `int *` — and
v8cc spills x0–x7 into **eight**-byte slots (`SZARG` is `SZLONG`), so `[1]` reads
the top half of `m0` and `[2]` the bottom half of `m1`. It is not fixable in the
compiler; the slot size is the ABI. mkfs could not create a filesystem at all:
`'d'` is index 3, the root inode got file type 0, and `iput` printed
`bad mode 777` on every run.

The *forward* form of the idiom — V8's `printf(fmt, args)` walking `&args` — was
handled long ago in `exec.c`, `doprnt.c`, `scanf.c`, `sprintf.c`, `printf.c`,
`fprintf.c` and `troff/n1.c`, all of which walk with an eight-byte type. Only the
indexed form was missed, and the sweep says it is a singleton:

```bash
grep -rnE '\(&[a-z_][a-z_0-9]*\)[[:space:]]*\[' src shim compiler
```

**AN ON-DISK STRUCT HAS AN END THAT IS NOT OURS, and every one of them was
wrong.** Until `mkfs` (§8a step 4) every struct in the port had two ends we
control — v8cc reads the header, clang re-spells it in the shim — so a widening
was safe if both agreed. A filesystem image has to agree with 1985. Measured
before the fix: `dinode` 80 (VAX 64), `filsys` 7960 (4096), `fblk` 1432 (716),
`NINDIR(0)` 128 (256). **V8's own compiler settles it in one line** —
`# define NOLONG`, "map longs to ints", at `cmd/ccom/vax/macdefs.h:20` — so
`long` was 32 bits there and `daddr_t`, `time_t` and `off_t` all silently
doubled here. `daddr_t` is narrowed globally (nothing hands one to macOS);
`time_t` and `off_t` are narrowed per *field* in `sys/ino.h` and `sys/filsys.h`,
because they cross the shim seam everywhere else. `src/include/PORTING.md`.

**AND A NARROWED ARRAY NEEDS ITS POINTERS NARROWED TOO, WHICH IS THE SAME BUG
ARRIVING A YEAR LATER AS A WARNING.** §8a step 4a narrowed the on-disk records;
§8a step 5 imported the kernel code that walks them, and `alloc.c:34` upstream is
`register long *p` over `s_bfree`, the superblock's free-block bit map that
step 4a had made `v8_i32[961]`. The array narrowed and the pointer did not, so
`*p &= ~(1 << (j&31))` is an 8-byte read-modify-write on a 4-byte word and
`p++` strides eight — scanning half the map and then running 961 words past the
end of the buffer. Same one line of cause as `nami.c`'s name compare
(`NOLONG`), and **upstream states the assumption five lines below in a
comment**: `for(j = 0; j < 32; j++) /* BITS PER LONG */`.

The pair is the lesson. `nami.c`'s deviation was an ERROR and stopped every
path lookup dead; `alloc.c`'s was two `-Wincompatible-pointer-types` warnings
in a build that **succeeded**, and it would have corrupted a free map on the
first write and been blamed on `mkfs`. So: **after narrowing a field, sweep for
pointers declared at the old width**, and treat a pointer-type warning on
imported source as the on-disk class until proven otherwise —

```bash
grep -rnE '\b(long|int)[[:blank:]]*\*[a-z_]+;' src/sys | grep -v '\.md:'
```

Three hits today and all three are benign, which is worth recording so the next
run has a baseline: `buf.h:45`'s `int *b_words` is a union arm for clearing a
buffer a word at a time, and `int` is four bytes on both machines; `swpf`
(`buf.h:69`, `bio.c:82`) is indexed by a swap-buffer number rather than walking
a record. Agrees under `/usr/bin/grep`.

**AND THE WIDTHS ARE NOW SAID OUT LOUD, BECAUSE THE MODEL CANNOT MOVE.**
`int di_size` did not mean "an int"; it meant "exactly four bytes, because a
VAX wrote four bytes there" — true only by coincidence of LP64, with the
declaration and the reason in different files. `<sys/types.h>` now declares
`v8_i16 v8_u16 v8_i32 v8_u32` and every record field is spelled with one
(`dinode`, `filsys` including both union arms, `fblk`, `spcl`); `char` stays
`char`, since a char array in a record is bytes.

**LP64 IS FORCED HERE, AND IT IS A COUNTING ARGUMENT** — worth knowing before
anyone proposes a "native 64-bit" `int`. ccom has exactly four integer types
(`CHAR SHORT INT LONG`, `manifest.h:224-227`, no `long long` in the front end)
and the port needs exactly four widths: 8, 16, **32** (`daddr_t`, the on-disk
times, `c_magic`, `c_checksum`) and 64 (a pointer). Four types, four widths, so
`char/short/int/long` = `8/16/32/64`, which *is* LP64 — the only assignment
that covers the set. With `int` at 64, **32 becomes unspellable** and every
field above becomes `char[4]` with hand-packing, i.e. editing the authentic
programs that read them. Measured, not argued: flipping `SZINT`/`ALINT` builds
until `local.c`'s `sz_incode()` — which tests `inwd == SZINT` before `SZLONG`
— emits a 64-bit initialiser as a 4-byte `.long`. PLAN.md §4k has what ILP64
would have bought in exchange, which is not nothing.

**Verifying a no-op refactor needs more than a green suite.** Three checks, and
the middle one is the reusable trick: layout measured from the V8 side
(`probe.c`); the generated image compared byte for byte against **a
same-binary, 1.2-seconds-apart noise floor**, which came out the identical
seven offsets, so nothing but the clock moved; and every differing tape byte
classified by its position within the 1024-byte record — 14 in `c_date`, 14 in
`c_checksum`, zero elsewhere. Compare artefacts against *what the clock alone
does*, not against each other.

**`sys/fblk.h` HAD NEVER BEEN IMPORTED**, and that generalises past this file.
`rootfs/usr/include` is third_party's pristine headers with ours copied over
the top (`Makefile:1960` then `:1966`), so **a header nobody imported silently
stays 1985's**. `struct fblk` is an on-disk record sitting in the same image as
`dinode` and `filsys`, both of which are patched copies in `src/include` — and
it was reading upstream's declaration. It measured 716 anyway, because `int` is
32 here as on the VAX and `daddr_t` came from our patched header: right by
coincidence twice, and **invisible to an audit of "the port's headers"**,
because it was not among them. `tests/deps` had the gap written down — its case
read *"sys/fblk.h is upstream, so sys/filsys.h stands for it"*.

Two more things generalise. **The tree already contradicted itself and no one had
looked**: `param.h` hardcodes `NMASK(0) 0377` and `INOPB(0) 16`, which assert
NINDIR 256 and `sizeof(dinode)` 64, one line from the `NINDIR` that computed 128.
When a header has both a hardcoded constant and a `sizeof`-derived one, make the
test compare *them*, not transcribed values. And **a global type change reaches
past the headers**: narrowing `daddr_t` silently broke `libc/gen/ltol3.c`, whose
arm64 arm strode eight bytes for exactly that type — two of this port's own
patches, each right when written.

**A NARROWED FIELD'S ADDRESS IS NOT A POINTER TO ITS OLD TYPE, and the sweep
that found this went ONE WAY when the seam goes two.** `di_atime`, `di_mtime`,
`di_ctime` and `s_time` are four bytes so the disk record is the VAX's; `time_t`
is eight. So `&dp->di_mtime` is not a `time_t *`, and the width is invisible at
the call. `icheck` met the **write** direction — `time(&sblock.s_time)` putting
four bytes onto `s_tfree` — and swept `grep -rn 'time(&'`, correctly, for a tree
that did not yet contain `fsck`. `fsck` brought a second one *and* the **read**
direction, which that pattern cannot match and which is far worse:
`ctime(&dp->di_mtime)` reads `di_ctime` as the high half, so `gmtime()` counts
towards the year 2.3e11 one year at a time — **a live lock with an empty stdout**,
diagnosed by sampling the stack rather than by reading the source. It only ever
runs on a *damaged* filesystem, so every clean-image case passed throughout.
Both directions, over three directories, and note the space in `load.c`'s
`time (&t)` that the older spelling missed:

```bash
grep -rnE '(ctime|localtime|gmtime|asctime)[[:blank:]]*\([[:blank:]]*&' src shim compiler
grep -rnE '(^|[^a-z_])time[[:blank:]]*\([[:blank:]]*&'                  src shim compiler
```

Two rules: **the pattern is not `time(&` but any callee reached through the
address of a narrowed field**, and **a sweep is a statement about a tree at a
moment** — where the property matters, put it in a suite, as `tests/mkfs` now
does by comparing `s_tfree` against icheck's independently walked free count.

**AND THE RAW COUNT IS THE WRONG THING TO RECORD, WHICH THIS ENTRY LEARNED BY
GOING STALE.** It said "19 and 22 today"; re-measured it is 38 and 39, and
nothing regressed. Two causes, both structural rather than accidental. The
tape trio landed *after* the number was written, so `dump`, `restor`,
`dumpdir` and `unctime.c` legitimately added sites. And — the reusable half —
**the sweep now matches the prose that records it**: eleven of the 38 are
prose, eight in `.md` files and three in the PORT comments that quote the
fixed line, so the population grows every time someone writes a find down. A
number that a *correct* fix increases is not a regression signal.

What is comparable across time is the narrow-target filter, so run that
instead of counting:

```bash
grep -rnE '(ctime|localtime|gmtime|asctime|(^|[^a-z_])time)[[:blank:]]*\([[:blank:]]*&' \
     src shim compiler | grep -vE '\.md:' |
     grep -E '&(sb|sblock|dp|ip|di_|s_|spcl|c_|.*->(di_|s_|c_))'
```

Three hits today, and all three are right: the PORT comments in `fsck.c:1711`
and `icheck.c:541` quoting the two fixes, and `pr.c:60`'s
`ctime(&sbuf.st_mtime)` — genuine, because only `sys/ino.h` and
`sys/filsys.h` narrow per field and `struct stat`'s `time_t` stays eight
bytes. (`sys/stat.h` is another header nobody imported, like `sys/fblk.h`
before it, so it is upstream's `time_t st_atime` reading our full-width
typedef. Right here, and right for the same by-coincidence reason — worth
knowing before anything narrows `time_t` globally.)

**A SAME-REGISTER RETURN IS NOT A SAME-TYPE RETURN, and floating point is
where the port had never looked.** Two bugs, both measured, and each hid the
other so that fixing one alone changed nothing observable:

- **`extern float atof()` where `atof` returns `double`.** On the VAX both came
  back in `r0/r1`; on ARM64 a `float` return is `s0` and a `double` return is
  `d0` — the same register read at a different width — so `atof("0.5")` gave 0.
  Five sites, all upstream's: `pic/picl.l` and `troff`'s `ta.c`, `hc.c`, `tc.c`,
  `devi10/makefonts.c`. Same family as the `yylval.p` token bug: a *declaration*
  that lies about a type. Sweep with `grep -rn 'float[[:blank:]]*atof()' src/cmd/`.
- **v8cc passes doubles in `x0`–`x7`, AAPCS64 passes them in `d0`–`d7`.** So
  every call into the host's libm read its argument from the wrong register:
  `sqrt(2.0)` returned 0.000000. `tests/kmemu` had tolerated libm as an allowed
  leak on the grounds that it was "non-variadic, so it works" — **that argument
  is about the shape of the call and is only as good as the register classes
  agreeing.** Fixed by building V8's own math, which is in `libc/math` and not
  in any libm, so "port libm" was the wrong question. The allowed-leak list is
  now empty. **`v8/usr/lib/libm.a` does exist — and inspecting it is what
  settled the question.** It is 216 bytes: one member, `dummy.o`, 62 bytes,
  whose entire symbol table is the name `_________`. It defines nothing.
  `shim/libm/dummy.c` reproduces it, because eleven upstream makefiles link
  `-lm` and the honest answer to them is the empty archive V8 actually shipped.

**AND THE INTEGER HALF OF THAT RULE IS NOT WHAT IT LOOKS LIKE, WHICH MATTERS
FOR STREAM DRIVERS.** `struct qinit`'s `qopen` is `long (*)()` and
`streamio.c` initialises it with functions declared `int` — the same shape.
Measured at -O0 and -O2 rather than assumed: the top half of x0 is **not**
unspecified garbage. Any write to `w0` zeroes bits 63:32 architecturally, so
`return 1` and `return a+b` come back zero-extended; and an `int` return that
forwards another call's result emits **no truncation instruction at all**, so a
64-bit pointer survives intact. Two consequences:

- **A `qopen` must never return a negative int.** `return -1` becomes
  `0x00000000ffffffff`, which `stopen:124` does not see as NULL and `:131` does
  not see as 1 — so the open "succeeds" and hands its caller an inode pointer
  of `0xffffffff`. `-1` is not hypothetical; `ufalloc()` in the same tree uses
  it.
- **The pointer case works today by accident of code shape**, not because the
  type is right, which is the worse half — it would break on a different clang,
  a different `-O`, or a callee that spills across the return.

Together these meant `pic` never computed a correct radius and every drawing it
or `grap` produced here was geometrically wrong. `tests/wavec` missed it twice
over: its inputs used only **default** sizes, which are compiled-in constants
that call neither `atof` nor the math library, and its end-to-end check counted
`grep -c '^D'` on troff's output — a pattern that only matched because every
coordinate was zero, so no motion preceded the draw. **The test had been
calibrated against the broken output, and fixing the program broke the test.**

**AN `int` MUST WRAP AT 32 BITS, AND FOR THIS PORT'S WHOLE LIFE IT DID NOT.**
Every integer here lives in an x register, properly extended — an `int` is
loaded with `ldrsw` and is correct as a 64-bit quantity. Arithmetic was then
emitted 64-bit, `add x9, x9, x10`. That is right for every result that **fits**,
and wrong the moment one does not, because a register has no 32-bit edge to wrap
at. The value then **disagrees with itself**:

```c
printf("%d", i)     /* reads the low half   -- RIGHT */
if (i == 84446)     /* compares all 64 bits -- WRONG */
```

Found in `dumpdir`'s `checksum()`, which sums 256 arbitrary ints off a tape
record: it computed exactly `CHECKSUM`, **printed** exactly `CHECKSUM`, and took
the not-equal branch — so every dump tape this port wrote was unreadable by the
two programs written to read it. `arm64_trunc()` in
`compiler/ccom-arm64/gencode.c` is the fix, at **three** `arm64_trunc()` sites
— binary op, its immediate form, compound assignment — plus an `arm64_widen()`
at `++`/`--`, which re-extends rather than truncating and reaches the same
answer. (This said "all four emission sites" and named `++`/`--` among them;
`arm64_trunc()` has five call sites today, the other two being the unary
`-` and `~` the sweep below added.)

Two things generalise:

- **It needs an overflowing accumulator that lives in a REGISTER.** An automatic
  is stored back through `str w` and re-narrowed by the store, so only
  `register` exposes it — measured: `register i` wrong, auto/global/struct
  member all right. That pair is why 1187 cases had not reached it.
- **Only `+`, `-`, `*` and `<<` need it** *among the binary operators*. `&`,
  `|`, `^`, `>>`, `/` and `%` of correctly-extended operands are correctly
  extended already, and `tests/v8ccom` has a negative control asserting they
  still are *without* the extra instruction. Same discipline as `SIGNCONVKEEP`.

**AND THAT LIST WAS THE BINARY OPERATORS ONLY, because the checksum happened to
be a `+`.** Sweeping it — the rule for a repeating class — found the two unary
ones, and for unsigned they are not edge cases at all:

| | signed | unsigned |
|---|---|---|
| `-x` (`neg`) | wrong for `INT_MIN` only, which comes out **positive** | wrong for **every nonzero value** |
| `~x` (`mvn`) | **already correct** — bits 63..32 all equal bit 31, and flipping every bit preserves that | wrong for **every value** |

That asymmetry is why `COMPL` is guarded on `tyunsigned()` rather than added to
the list: an unconditional `sxtw` on a signed `~` is dead weight. Both hid the
way the checksum did — `~mask` is nearly always consumed by an `&` against a
zero-extended value, which discards the wrong top half and restores the right
answer. Only a comparison or a divide reads it whole.

**And fixing it broke a test that had been calibrated against it.**
`t 'long arithmetic'` expected `100000000000` from
`f() { long a; a = 100000; return a*1000000; }` — but `f` has an implicit `int`
return, so the answer is `1215752192` and **clang `-std=gnu89` agrees**. It only
read as the full value because the truncation never happened. Third instance of
this shape here, after `wavec` counting drawing commands that matched only while
every coordinate was zero. When a compiler fix turns a test red, check which of
the two was wrong before assuming it was the fix.

**A same-size conversion is not always a no-op.** Widths are the common fault;
signedness is the other one. A signed and an unsigned value of the same size
have identical bits, so converting between them changes nothing about the
*result* — and `optim.c`'s `sconvert()` therefore drops the conversion and
paints its type onto the operand. That is sound for every operator whose bits
come out the same either way, and wrong for `/`, `%` and `>>`, where
`gencode.c` reads that same type to choose `udiv`/`sdiv` and `lsr`/`asr`. There
the type is an *instruction selector*, not a description. Guarded under
`SIGNCONVKEEP`; PLAN.md §4g has the account. Two rules follow: a back-end
operator whose instruction depends on `tyunsigned()` must be added to that
guard, and `sconvert()` must not be "simplified" — the same seven lines have
now produced **three** faults of this shape, `PTRCONVFULL` being the second.

**THE THIRD IS THE VALUE RATHER THAN THE INSTRUCTION, AND PATCHES.md PREDICTED
IT IN SO MANY WORDS** — "a third change here should be suspected of being a
fourth". The sentence above says a signedness change "changes nothing about the
result", and that is true of the 32 bits and false of the register holding
them: this back end keeps an `int` sign-extended and an `unsigned int`
zero-extended, and compares with an x-form `cmp`. So the paint left the
register carrying the *source* type's extension under the destination type's
name, and two values that were both `0xffffffff` compared unequal. **An
explicit `(unsigned)` cast did not help, because the cast is exactly what was
being deleted** — which is the tell, and the reason it reads as a comparison
bug rather than a conversion one.

Three things to carry:

- **Placement is the whole of the fix.** The new guard sits at the `t == lt`
  jump, not at the `paint:` label where the instruction-selection one is. The
  label is also reached by the narrowing fall-through, where the type is
  painted onto a memory reference and the *load* does the extension — already
  right, and returning early there would stack a CONV on a tree `adjust()` may
  have rewritten, converting twice. The representation fault exists only where
  the widths already agree.
- **It costs nothing above four bytes**, because `arm64_widen()` emits no
  instruction for an 8-byte type — `tests/v8ccom` asserts that rather than
  asserting the comment.
- **A redundant extension is invisible to a behavioural test**, since the
  answer stays right. Nine of the new cases therefore count instructions in
  `cc -S` output, and the mutation that proves them is the *inverse* one: make
  `COMPL` widen unconditionally and watch the negative control go red while
  every value-checking case stays green.

The diagnostic is the reusable part, because the symptom is narrow enough to
mislead. It needs an unsigned operand **with its top bit set**, so
`printf("%lx")` of a negative long lost *exactly one digit* — `val /= base`
clears the top bit after the first iteration, and every later digit was right.
One wrong digit reads as an off-by-one in a buffer, and is not. When a value is
wrong in exactly one place, stop reasoning about the source and read what was
emitted: `cc -S`, then look for an `sdiv` or `asr` where the C says unsigned.

**AND A VESTIGIAL FILE HAS NOW COST A SECOND TIME, WITH BELL LABS' OWN NOTE
SAYING SO.** The `syopen` case below says a dead file that answers your question
is the worst kind of evidence. `dev/conf.c` is the second: it holds
`struct fstypsw fstypsw[]` and `nfstyp = 4`, the obvious source for the kernel's
filesystem switch, and its row 0 names **`rnami`, which is defined nowhere in
the tree**. `sys/nami.c:167` upstream is a comment reading *"USED TO BE rnami"* directly
above `fsnami`. `conf/config_diff:11` settles it in one sentence —
*"dev/conf.c is no more. config makes a conf.c for each machine"* — and `:13-14`
lists the files "changed a little to make names regular", `nami.c` among them.
`dev/param.c` is a third instance, stale against `sys/param.c`.

**AND IT IS BOTH NAMES IN THAT ROW, NOT ONE.** Row 0 is
`{ 0,0,0,0,0,0,0,0, rnami, smount, 0 }`, and `smount` is defined nowhere either:
the whole 18k-line kernel mentions it in **exactly two lines, both in
`dev/conf.c`** — the `extern` and the table row. The live mount syscall is
`fsmount()` at `sys3.c:273`, wired at `sysent.c:103` as call 21. So §8a step
5e's costing, which had been carrying "`smount` is not imported" as an open
question, was asking about a function that does not exist.

Two things generalise. **A dead row goes stale in every column, so check them
all** — finding one bad name is a reason to check its neighbours, not evidence
that the rest are fine. And the sweep for it is a trap: `grep -rln 'smount'`
returns four files, because **`fsmount` contains `smount`**. That is the
instrument matching its own subject, one shape along from the `time(&` sweep
that counted its own documentation. Use `[^a-z_]smount[^a-z_]|^smount`.

**The live source is `conf/devices`**, which this file already cites for
`ttyld` (`:75`) and `/dev/tty` (`:55`), and whose `:70-73` are the filesystem
handlers. So the rule is sharper than "read the source": **when two upstream
files disagree, find out which one the BUILD reads** — `conf/files`,
`conf/makefile` and `conf/config_diff` are where V8 says so, and a `dev/` file
not named by any of them is a candidate for dead.

**Read the program before deciding how to port it — THREE times now the plan
was wrong about what a program talks to, and the third was wrong about a
*device node*.** PLAN.md §8a step 1b costed a host-fd stream driver to sit
under `/dev/tty`. **V8's `/dev/tty` is not a stream, not a device, and has no
code behind it**: it is a hard link to `/dev/fd/3`, and opening anything in
`/dev/fd` is `dup(2)`. Four confirmations, all read rather than recalled —
`proto-dev:91` (major 40 minor 3, link count 2), `conf/devices:55` (`device 40
std`, no driver name), `dev/conf.c:565` (every `cdevsw` slot `nodev`, null
`streamtab`), and `sys/sys2.c:174`, where `open1()` special-cases it *before*
the permission check with `getf(minor)`/`ufalloc()`/`u_ofile[i] = fp` — `dup`'s
body, written out. `man4/fd.4` says it in prose too. V7's `syopen` driver is
still in the tree at `sys/sys/sys.c` and is **dead**: not in `conf/files`, and
unable to compile, since `u_ttyp` is not in V8's `struct user`. A vestigial
file that answers the question you are asking is the worst kind of evidence.

What makes fd 3 the terminal is `init.c:368-382` — `open(tty,2)` as fd 0,
`FIOPUSHLD` the tty discipline, then `dup(0)` **three** times. "Controlling
terminal" is a userspace convention in V8, not a kernel fact, so the `v8`
launcher is this world's init and has to arrange it. `shim/NOTES.md`.

Two things generalise past this instance. **A survey's citations decay
independently of its conclusions** — the same block cited `conf/devices:82` for
`ttyld`, and `:82` is `bf`; `ttyld` is `:75`. And its ordering argument
("`ttyld` has no bottom end so it cannot be exercised") was false at the open
path: `ttyopen` never dereferences `q->next` and sends nothing downstream. Only
*traffic* needs a device below. **Re-read the source a survey cites before
building on the survey**, not just its summary.

PLAN.md said `ps` would be ported "on
top of `libproc`". V8's `ps` is a **`/proc` client**: `getdir("/proc")`,
`open("/proc/<pid>")`, `ioctl(PIOCGETPR)` for the `struct proc`, and the u-area
read at virtual address `UBASE`. That is Killian's process filesystem, V8's own
invention — `sys/pioctl.h` and `sys/sys/proca.c`. Bolting `libproc` on would
mean rewriting `doselect.c` against an interface V8 had already abandoned, in
order to avoid building the one it used. So `ps` waits for `/proc` (PLAN §8a
step 3), and so does the full form of `w`.

`w` is the counterpart: it is `@(#)w.c 4.4 (Berkeley) 6/5/81` and grovels
`/dev/kmem` and VAX page tables, while `ps` carries no `sccsid` at all. **Two
process tools in one tree, from different eras**, and the era shows in what
they open. Only the `uptime` half of `w` runs here; the full half says `No mem`,
and `tests/kmemu` asserts that message so a future `/dev/mem` is a decision
rather than a discovery. `src/cmd/w/PORTING.md`.

**V8 spells DIRSIZ in THREE headers and `#ifndef` means first-include wins.**
`<dir.h>` (`struct dir`), `<sys/dir.h>` (`struct direct`) and `<sys/param.h>`
(no struct, and the one that decides — `w.c` and `ps.h` both include it first).
This port raises 14 → 254; patching two of the three changed nothing for
exactly the programs that read directories raw, while looking like it had. All
three now agree, plus `shim/v8sys/v8sys.h`'s `V8_DIRSIZ` and
`src/libc/gen/readdir.c`'s `ODIRSIZ` and `shim/libkmemu/procfs.c`'s
`PR_DIRSIZ` — ~~six spellings of one number~~.

**AND THAT LINE SAID "six spellings of ONE NUMBER", WHICH IS WRONG TWICE — AND
COUNTING THEM PROPERLY FOUND A LIVE BUG.** Re-measured with
`grep -rn 'define[[:blank:]]*[A-Z_]*DIRSIZ' src shim`, there are **ten** and
they are **four** numbers:

| | value | whose | verdict |
|---|---|---|---|
| the six above | 254 | this port | agree, as claimed |
| `src/sys/h/dir.h:2` | **14** | upstream, imported | **right, and must stay** — `src/sys/` describes a *disk record*, so a kernel there reading 254 bytes would be wrong. Same reasoning as `mkfs -DDIRSIZ=14` |
| `src/cmd/cc.c:24` | **255** | this port | deliberate, commented at `:6-16` |
| `src/cmd/sh/spname.c:21` | **14** | upstream | **was a live 1-byte global-buffer-overflow** |
| `src/cmd/sh/expand.c:15,17` | `MAXNAMELEN` / 14 | upstream | bounded copy, `movstrn` |

So the rule is not "one number" but **one number per layer**, and the layer is
what says which.

**AND A `_Static_assert` CAN BE ON THE WRONG SIDE OF THAT LINE AND STILL PASS.**
`p9.h` picks `P9_NAMELEN` 256 "because it must exceed the longest name any
namespace on either side of this wire can produce, and that is V8's DIRSIZ,
which this port raises to 254", and then delegated the inequality to the
server, "where DIRSIZ is actually in scope". It is in scope there and it is
**14** — `v8fsd.c` includes `src/sys/h/dir.h` because it reads DISK RECORDS,
the same reason `mkfs` is built `-DDIRSIZ=14`, and `shim/kern/h/param.h`
defines no DIRSIZ at all. So the assertion read 256 > 15 while the sentence
delegating to it was about 256 > 255: **a guard that could not fail for the
reason it gave**, and the half with any slack in it was checked nowhere. Both
are asserted now, each in the file that can see its own number — and the
client's is the one worth having, at one byte against the server's 241. Two
ends of one wire is two layers, so a protocol constant justified against "the"
DIRSIZ was always going to name only one of them. Found by the lp64-auditor on
the 5g diff.

`spname.c` is the fifth member of the "1985 buffer sized
against DIRSIZ" table below — `static char best[DIRSIZ+1]` filled by an
unbounded `do; while(*p++ = *q++);` from a `d_name` that is now 254 wide.
Measured under ASan on the unmodified source; `src/cmd/sh/PORTING.md` has it.
Three things generalise:

- **The bound was in a different function**, so the copy reads as unbounded and
  is not: `SPdist` gates it on a score under 3 and will not tolerate a name
  more than one character longer than the guess, making the overrun **exactly
  one byte**. Reasoning about the copy alone gets the severity wrong in *both*
  directions.
- **The sibling copy IS bounded** (`if(p != guess+DIRSIZ)`), which is why an
  audit of this function for this very class finds a bound and stops.
- **`newname[128]` is `mv`'s `MAXN` again** — `&newname[128-DIRSIZ-2]` at `:31`
  — so raising DIRSIZ alone makes it `-132` and `spname` returns 0 forever:
  `cd` stops correcting, silently. Both numbers move or neither does.

And the method is the reusable half: this was not found by auditing `sh`, which
had been read for other reasons. It was found by **checking a count in this
file**. A claim of the form "N spellings of one number" is a testable assertion
about the tree, and nobody had run it.

**And the sentence above was wrong about `param.h` for months.** Upstream guards
`dir.h` and `sys/dir.h` and leaves `param.h` **bare**, so on a real V8 this one
always won by *redefinition* rather than by being first. It cost nothing while
every spelling said 254 and was found the instant something wanted a different
one: `mkfs` is compiled `-DDIRSIZ=14`, because what it writes is a disk image
and a V8 kernel reading a 256-byte record allocates into the fifteen
zero-`d_ino` slots it finds. cpp said `DIRSIZ redefined` and handed mkfs 254
anyway. `param.h` now carries the guard its own comment claimed. **A flag that
sets an on-disk format can be forgotten, so `tests/mkfs` asserts it on the bytes
of a generated image** — `..` at offset 16, root `i_size` 32 — never on the
compiler line.

**And that rule is load-bearing, because A WRONG WRITER IS INVISIBLE TO EVERY
READER WE HAVE.** Measured by building `mkfs` the way Bell Labs' own `Admin/Mk`
would — `cc $CFLAGS -o $B $B.c`, no `-D` of any kind, correct on a machine whose
`param.h` says 14. The image it writes has `i_size 512` and `..` at offset 256,
and **icheck, dcheck and fsck all pronounce it clean**: the 240 bytes between
`.` and `..` are zero, a zero `d_ino` is V7's own encoding for a deleted entry,
so a 16-byte-record reader skips fifteen empty slots and finds `..` exactly
where the 254 writer put it. The mirror of the accident above. So the three
byte-level cases are not belt and braces — they are the only guard, and the
group's own checkers cannot be one. `tests/mkfs` section 8; **this note used to
say the opposite**, that forgetting the flag reports a healthy filesystem as
corrupt, which is the harmless direction and the one that does not happen here.

**And with `fsck` the cost of forgetting it changed KIND.** For the plain readers
in `$(IMGBIN)` a wrong `DIRSIZ` is a wrong answer. `fsck`'s `pass2()` copies
`DIRSIZ` bytes per path component into `pathname[200]` with no bound, so at 254
a **single** component overruns it by 54 bytes — in the one program here that
writes to filesystems. 200 is upstream's sentence about 14, the same shape as
`mv`'s `MAXN-DIRSIZ-2`, so `pathname[]` is correct arithmetic and stays at 200;
what has to hold is the flag, and `tests/deps` asserts
`sys/param.h -> fsck object` so the edge cannot be lost.

**`ncheck` and `quot` are the group's two extremes, and having both is what
makes membership mean something.** `$(IMGBIN)` is ten now. `ncheck` is the most
flag-dependent program here: built at 254 it reads a **correct** image and prints
*nothing at all, exit status 0* — `NDIR(dev)` comes out 4 instead of 64 and the
step is 256 bytes rather than 16, so a root whose `di_size` is 64 is exhausted by
its own `.`, which `dotname()` filters. `quot` does not need the flag at all: no
`<sys/dir.h>`, no `struct direct`, and its object is **byte-identical** either
way, which `tests/mkfs` asserts by compiling it twice and `cmp`ing rather than by
this sentence. It is in the group on the group's rule, not on need.

That measured no-op has a payoff and a trap. **The payoff: `quot` is the first
image tool to close rung 5**, because its own makefile passes no `-D`, so Bell
Labs' build description produces the same program — `tests/jail` hands the
rung-5 binary and the installed one the same image and requires the same answer.
**The trap: an `$(IMGBIN)` program must never join `$(V8BIN)`**, because that is
the list `$(SRCTREE)` stages for `Admin/Mk`, and Mk compiles a bare `cmd/*.c`
with no `-D` either. `tests/jail` asserts the two lists are disjoint, because
measured, `make` emits **no warning of any kind** when a name is in both.

And that case turned out to catch a second thing, which is worth knowing before
you delete a name from `$(V8BIN)`: **`$(SRCTREE)` staging is additive.** make
copies a source into `rootfs/usr/src/cmd` when the name is listed and never
removes it when the name leaves, so a tree that once staged `ncheck.c` keeps
serving it to Mk. Found by that assertion firing on a run *after* a mutation had
been reverted — the third shape of the host-property trap again, a property of
what ran before rather than of the machine.

**LP64 is not the only width problem: V8's 16-BIT RANGES are the other, and
they fail later and quieter.** LP64 breaks a pointer immediately; a 16-bit field
holds a value the host has simply not reached yet. Five so far, and they are
one class:

| field | V8's range | the host's | how it failed |
|---|---|---|---|
| `DIRSIZ` | 14-char names | any length | truncated names, `pwd` could not `chdir` back |
| `d_ino` | 16-bit inode | 64-bit | **`pwd` printed another directory's path, exit 0** — see below |
| `p_pid` | `short`, wrapped at 30000 | to 99998 | **negative pids** — 44145 read as −21391 |
| `FSNMLG` | 32-char mount points | to 140 seen | `df` printed a mount point as a *device* |
| `u_uid` | `short`, to 32767 | to 100000+ | a uid ≡ 0 mod 65536 reads as **root** |

**THE `d_ino` ROW SAID "harmless" FOR MONTHS, THEN SAID "CANNOT BE FIXED", AND
BOTH WERE WRONG.** It is the one of the five that narrows an *identity* rather
than a value, and identity is what three of V7's idioms are built on.
`v8sys_fold_ino` (`shim/v8sys/dir.c:309`) feeds **both** sides of `getwd`'s
central comparison — `d_ino` in the directory snapshot and `st_ino` in
`stat_translate` — so while it was a plain XOR fold, `getwd.c:62`
(`while (dir->d_ino != d.st_ino)`) stopped on whichever colliding entry
`readdir` yielded first. Measured over every directory in `$TMPDIR`: right
**32 of 60** inside a collision group against **60 of 60** outside.

**The "cannot" rested on a requirement that was never real: that the map be a
pure function.** What the port actually needs is that it be **stable within a
process**, which is strictly weaker and admits an append-only table — the fold
proposes a number, a contended one probes for the next free, and an assignment
is never revised. Re-measured on the same host after: **6729 entries, 6729
distinct values** where the fold gave 519 entries sharing 257, and `pwd` right
**1752 of 1752**. What paid for it is that two *processes* can now disagree
about a colliding inode (7.7% of entries here), which nothing in the live tree
reads — `find` is not even ported. `src/libc/gen/PORTING.md` has the full
account, the 99.92% that keep their old number, and the two things left open.

Three things generalise, and the third is the one that cost months:

- **A confirming `stat()` does not help**, which is the trap. It returns the
  *folded* inode too, so the colliding file answers with the same `st_ino` and
  the same `st_dev` and the check passes. `ttyname.c:59-65` is upstream's
  careful version of the idiom — pre-filter on `d_ino`, then `stat` and compare
  both fields — and it is defeated identically. Two files the shim maps to one
  `(dev, ino)` are indistinguishable to a V8 program **by construction**, so no
  consumer-side change can separate them. A patch to `getwd.c` was written and
  withdrawn, and `getwd.c` is still unmodified: the fix belongs on the
  *producer* side, which is the whole lesson.
- **A fresh test tree structurally cannot reproduce it.** APFS hands out
  consecutive inodes and the fold separated consecutive values perfectly, so
  1500 directories made back to back collided **zero** times. Collisions need
  inodes spread over time. That is why `tests/wavea`'s `pwd` case fired roughly
  never, why the churn experiment recorded there had no chance — it churned
  entries it had just created — and why the guard that replaced it asserts
  distinctness over `$TMPDIR` and says so out loud when the population is below
  the birthday bound of 256.
- **THE REASON NOT TO TRY WAS A CITATION THAT SAID THE OPPOSITE.** The recorded
  constraint was that folded values "are written into `/etc/utmp` for another
  process to read". V8's `struct utmp` is `{ut_line[8], ut_name[8], ut_time}` —
  24 bytes, no inode field — the cited note is about a `/proc` field it calls
  hypothetical and leaves zero, and `libkmemu` never calls the function at all.
  Same shape as the `time(&` sweep and the `synth.c` miscount, but this one
  **blocked work** rather than inflating a number. A recorded constraint that
  stops you doing something has to be read at its source before it is obeyed.

**AND THE FIFTH ONE WAS WRITTEN ONE LINE BELOW THE PARAGRAPH ARGUING AGAINST
IT.** `shim/kern/sys/fio.c` folds a Darwin pid into a VAX `short p_pid`'s range
and says at length why a bare cast is wrong — *"a truncation can silently
produce the one value the code reads as absent"* — and then cast `u_uid` and
`u_gid` with `(short)` on the next two lines. That is CLAUDE.md's own rule
about correcting one of these: **the fix lands on one line and the line beside
it keeps the assumption.** Found by the `lp64-auditor` subagent, not by the
person who wrote both lines.

The magic value differs per field and that is what has to be preserved: 0 means
*absent* for a pgrp and *root* for a uid, and `streamio.c:44` lets root bypass
a stream's exclusive-use lock. So the contract is two properties rather than a
formula — **root maps to root, and non-root never maps to root** — with every
value that fits kept exact.

**A 1985 BUFFER SIZE IS THE SAME CLASS, and the ratio is what breaks.** Raising
`DIRSIZ` 14 → 254 did not just widen a field; it invalidated every buffer sized
*against* it. Four programs, all found by the `lp64-auditor` subagent reading
the source before it was built, all verified by reproducing the crash:

| program | was | symptom |
|---|---|---|
| `mv` | `MAXN 100`, guard `strlen(target) > MAXN-DIRSIZ-2` | the guard became `> -156`, so **every** directory move was refused with a false message |
| `mkdir` | `pname[128]`, `dname[128]` | a 255-char name (macOS `NAME_MAX`) SIGSEGVs — *after* making the directory |
| `rmdir` | `name[500]` | SIGBUS at 550 chars, on the argument alone |
| `sed -n l` | `trans[*p1]`, signed char | any byte ≥ 0200 indexes backwards; **UTF-8 crashes it** |

All four are now `PATH_MAX`-sized or unsigned-indexed, each recorded in its own
`PORTING.md`. Two lessons generalise. `mv`'s is that a constant can encode a
*relationship* — `MAXN-DIRSIZ-2` is a sentence about two numbers, and changing
one of them silently rewrote it. `sed`'s is that LP64 and Mach-O can turn an
upstream out-of-bounds *read* into a fault: `trans[]` is an array of pointers,
so the stride doubled, and Mach-O maps nothing where a.out did.

**A crash can happen after the work is done.** `mkdir`'s fault lands on the
return, so the directory exists and `[ -d ... ]` is satisfied by a run that died
of SIGSEGV. Assert the exit status.

**A truncated PATH is not the same class of loss as a truncated NAME**, and
conflating them is what let `FSNMLG` hide: `shim/libkmemu/mtab.c` documented the
truncation as "the same loss as dir.c's 14-character names" and was wrong. A
name is an opaque string to every reader; a path has to *resolve*, and `df`'s
`dfree()` branches on `stat()` succeeding — so the truncation did not shorten a
column, it sent df down the arm that assumes the string names a device. Widening
moves that boundary rather than removing it, so what still overflows is now
**reported and dropped** rather than truncated: an entry whose path cannot be
stored cannot be described truthfully. `src/include/PORTING.md`.

The `p_pid` one is the shape to remember: **a freshly booted host has low pids**,
so every check passes until the counter crosses 32767 and the same binary starts
lying. It was found by mutation-testing something unrelated, when a mutation
produced two extra failures it had no business producing. `tests/kmemu` now
asserts the *field width* beside the runtime value, because the width is true at
every pid and the comparison only at high ones. Widened in
`src/include/sys/proc.h`; `src/include/PORTING.md` is that tree's record, and it
holds the rule that a struct there has **two ends**, since v8cc and clang each
read one.

**A directory's `st_size` is the size of what `read(2)` gives, not what the host
charges.** The shim builds 256-byte records from variable-length host entries,
so the numbers are unrelated (nine entries: 2304 bytes of records, APFS says
288). Every reader that loops to EOF never noticed; `ps`'s `getdir` sizes an
array from `st_size` and demands `read` return exactly that. Fixed in
`v8sys_pt_fstat` via `v8sys_dirsize()` — **fstat only**, because nothing can
read a directory without opening it, and doing it for `stat(2)` would put a
`getdirentries` loop inside every `ls -l`.

**A preprocessor that is never fed downstream is not tested.** Both token bugs
above were invisible while the program itself looked perfectly correct, because
they lived at the seam: `grap`'s output crashed `pic`, and `grap` alone was
fine. The wavec suite now runs `grap | pic | troff` and asserts drawing commands
come out the far end. Pipe a new Wave C program into what consumes it before
believing it works.

**V8 assumes address 0 is readable, and this is the class that keeps coming
back.** The VAX put the text segment at 0, so `*(char *)0` returned a byte of the
program rather than trapping. macOS keeps page 0 unmapped. The first three:

| program | the call | when it fires |
|---|---|---|
| `refer` | `prefix(".[", lookat())` — `lookat()` returns NULL at end of input | the **last** citation in a file, so a one-citation test misses it |
| `quot` | `strcmp(p1->name, p2->name)` in `qcmp` | **the default invocation**, before a line is printed |
| `ncheck` | `atol(argv[1])` past the last `-i` number | `ncheck -i 5`, i.e. `-i` at the end of the command line |

`quot`'s is the one to remember, because nothing about it is an edge case: `du[]`
is indexed by uid, only the uids in `/etc/passwd` get a `name`, so **2046 of 2048
entries are null** and `qsort` compares them against each other. It was found by
auditing before building, not by running.

**THE PARAGRAPH ABOVE USED TO SAY "the sweep is not done", AND IT WAS RIGHT:
DOING IT FOUND NINE MORE, ALL MEASURED SIGSEGVs.** The tree was searched for
each shape of the class rather than for the next instance. Every one is the
program's *last* argument, which is the whole trigger:

| program | the command | what it was |
|---|---|---|
| `unexpand` | **no arguments at all** | `argv[0][0]` with argc 0 — and `expand.c:20` beside it has the guard. Berkeley's omission in one of a matched pair, on the primary documented use of a filter |
| `icheck` | `icheck -b 5` | `atol(argv[1])` — **byte for byte `ncheck`'s loop** |
| `dcheck` | `dcheck -i 5` | the **third** copy of that same loop |
| `fsck` | `fsck -t` | `**++argv == '-' \|\| --argc <= 0` — `\|\|` runs left to right, so the deref happens before the count is consulted |
| `join` | `join -o 1.1`, `join -j1` | the `-o` field list and `-j` both walk to the end |
| `yacc` | `yacc -o` | the output file name |
| `hunt` | **bare `hunt`** | the option loop's own condition |
| `nroff`/`troff` | `-F` | one upstream line, two binaries — `n1.o` is in `NROFF_NAMES` too |

Three things generalise, and the first is the reason to run a sweep at all:

- **The same loop existed three times and only one was fixed.** `n =
  atol(argv[1])` inside an option's number loop is identical in `ncheck`,
  `icheck` and `dcheck`. Fixing `ncheck` and writing it up did not find the
  other two, because the note was filed under `ncheck` rather than under the
  shape. `icheck.PORTING.md` had even audited that exact loop for a different
  overrun and gone one line past the null.
- **The crash is not always in the program.** `yacc -o` faulted in **our shim**:
  the output file cannot be created, `error()` runs `cleantmp()`, and its two
  `unlink()`s are of temp names `setup()` had not assigned yet — and
  `dotlink()` in `shim/v8sys/syscall.c` inspected the path before the syscall
  could answer `EFAULT`. The shim's own rule is that a null path belongs to the
  kernel; `rootpath()` returns one unchanged for exactly that reason. `v8s_link`
  had the same hole one function away and nothing had ever called it that way.
- **A fix must not just stop the crash.** Every case is paired with one asserting
  the option still *works* — `dcheck -i 2` still names all three references to
  the root, `join` still joins, `-F` still reports the path it was given. The
  mutation that proves those is a "fix" that makes the loop consume nothing:
  the crash goes away and the behaviour case goes red.

**And FOUR that were audited and deliberately NOT changed**, because the rule is
that a change to `src/` must be forced by the target. `make`'s `meter()`
dereferences an unchecked `getpwuid()`, but it returns on `meteron == 0` and
nothing in the tree ever sets it. `ls.c:285`'s `calloc` and `ls.c:257`'s `malloc` are
unchecked where the other three sites check — and a write to page 0 faults on a VAX too, so there is no
VAX answer to restore.

**`pr.c:259` and `troff/hc.c:767` are the same verdict reached by a longer
route, and `/dev/fd` is what made them reachable.** Both `fopen("/dev/tty")`
unchecked — `pr` then `getc(Ttyin)`, `hc` then `setbuf(rcf, NULL)` — and with
V8's `/dev/tty` meaning fd 3, that pointer is null whenever the launcher did
not run. It looks like a regression and is not: **`getc(p)` is
`(--(p)->_cnt >= 0 ? …)`, which WRITES to virtual 0**, and V8's binaries are
ZMAGIC (`a.out.h:17`), whose text is read-only shared — so a VAX takes a
protection fault too. What the port lost is an *accident*: `fopen` used to fall
through to the **host's** `/dev/tty`, a different device entirely. `dump` is the
one of the three that checks, and it `abort()`s by design.

Two things to carry. **The address-0 rule needs the struct, not just the byte** —
the sixteen crt0 bytes give `_cnt 0x08c20000`, `_ptr 0x08aed05e`,
`_base 0x0cae9e6e`, `_flag 0xd050`, and the last two reproduce values PLAN.md
already recorded, which is what says the layout is being read right. And
**whether a page-0 access is a read or a write is the whole question**:
`fflush(NULL)` reads `_flag` and returns harmlessly, `getc(NULL)` decrements
`_cnt` and faults, one line apart in the same header.

**THE SAME SWEEP FOUND THE PORT DISAGREEING WITH THE CODE V8 ACTUALLY RAN, and
upstream shipped the answer.** `strncat` read `s2[n]` — one past its own bound —
because the loop copies the byte first and only then notices `--n < 0`, and
overwrites it with the NUL. The *output* was therefore always correct and only
the read was out of bounds, which is why nothing had ever noticed. That is the
same shape as `%.Ns` in our `doprnt.c`, and for the same reason: a count
argument exists precisely because the source need not be terminated. All five
callers pass a fixed-width field (`d_name`, `utmp.ut_line`).

The authority for changing it is `libc/gen/strncat.s`, **which is what a VAX
executed**: it opens `movl 12(ap),r8 / bleq L6` — returning without touching
`s2` when `n <= 0` — and scans with `locc $0,r8,(r7)`, bounded to exactly `n`.
The `.C` beside it is the portable *reference*, its header calls itself "the
`standard' for the C-library", and it disagrees with the assembler shipped next
to it. So the overread came from **this port substituting the reference for the
assembler**, and removing it restores V8 rather than departing from it. The V7
twin `strcatn` has the identical body and *no* `.s`, so its note records a
deviation instead — the distinction is in the two comments, because it is the
justification and not the code that differs.

The testable diagnostic, needing no guard page, is the one `%.Ns` used:
`strncat(buf, (char *)1, 0)` faults on the old loop and never touches `s2` on
the new one. **A behavioural test cannot see this class at all** — the answer
was right the whole time.

**Fix to the VAX's ANSWER, not just to the absence of the fault.** Address 0 held
`0x00`, so `strcmp(name, 0)` compared against the **empty string** — below every
name — and an unnamed uid sorted *before* a named one while two unnamed ones
compared equal. `strcmp(p1->name? p1->name: "", ...)` reproduces both; a null
guard returning 0 would not, and `quot`'s ordering is visible in its output.
Same for `ncheck`: `atol("")` returned 0 and broke the loop, so the patch is
`argv[1] == 0? 0L: atol(argv[1])` rather than a `break`.

**AND FOR MONTHS THAT BYTE WAS RECORDED HERE AS `0207`, "the low byte of the
a.out magic".** It is wrong, it is repeated in a dozen `PORTING.md`s and source
comments, and **every fix built on it is still correct** — which is exactly why
nobody caught it. Measured: V8's shipped binaries are **ZMAGIC** (`od -An -tx1
-N4` gives `0b 01 00 00` = 0413, not 0407) and `a.out.h` says
`N_TXTOFF(x) = ((x).a_magic==ZMAGIC ? 1024 : sizeof (struct exec))`, with
`sys/sys/text.c:132` reading from `BSIZE(0)` into `u_base` 0. So virtual 0 is
the first byte of **crt0**; the header is never mapped. `0x00` and `0207` are
both "not `'-'`", both non-digits, and both below any name character, so the
guards agree — only `nroff`'s `oputs(0)` differs, and there the truth (nothing
came out) matches the fix better than the guess did. PLAN.md §4i has the table.

The payoff is that a VAX answer can now be computed for a *structure* and not
just a byte: those 16 bytes are identical in every V8 binary, so reading them
through the VAX `struct _iobuf` gives `_flag` `0xd050`, which is how `lex`'s
`fflush(NULL)` was settled — `0xd050 & (_IONBF|_IOWRT)` is 0, so it returned 0
having touched nothing, and `if(fout) fflush(fout)` restores exactly that.

**And the same audit found the mirror of it in OUR code.** `%.Ns` in
`src/libc/stdio/doprnt.c` was `for (len = 0; s[len]; len++) if (haveprec && len
>= prec) break;` — the condition runs before the body, so `s[prec]` is read and
discarded. `%.Ns` exists precisely for a fixed-width field that need **not** be
terminated; `ncheck` prints `d_name` with `%.14s`, so the byte read is the next
entry's `d_ino`, and against a field at the end of a mapped page it is a fault.
That file is this port's C rewrite of `doprnt.S`, so the bug is ours. Nothing had
reached it in 32 `printf` cases. The diagnostic that makes it testable without
arranging a guard page is `prec = 0`: `printf("%.0s", (char *)1)` faults on the
old loop and dereferences nothing on the new one.

**AND THE MIRROR OF IT IS A DUPLICATE DEFINITION, WHICH SPLITS IN TWO AND ONLY
ONE HALF IS LOUD.** §8a step 5 linked libv8kern against libv8c and libv8stubs
for the first time and produced **nine** collisions where four were costed.
Six are function-against-function: the linker refuses, and you find them the
first time you link. Three are a **variable against a function** — the
kernel's `time` (`systm.h:12`) against libv8stubs' `time(2)`, `int timezone`
against libc's `char *timezone(zone, dst)`, and `struct mount mount[NMOUNT]`
against the `mount(2)` stub — and those are **silent**, because a K&R tentative
definition is a COMMON symbol and resolving a common against a text definition
is what a linker is supposed to do. The kernel's clock would have become the
address of `time()`, and `iget.c:276`'s `dp->di_ctime = time` would write a
code address into an inode as a timestamp.

Two rules. **`nm -u` cannot see this class at all** — it is about what an
archive DEFINES — so the sweep is `nm -g` filtered to `T`/`D`/`S`/`C`, pairwise
between our own archives. And the verdict is **three-way, not two**: `T` vs `T`
is a collision the linker catches, `C` vs `T` is the silent one, `C` vs `C` is
deliberate sharing (only `errno`, which must be one object so a syscall stub
and `perror()` agree). `tests/kmemu` asserts it, and getting there found that
**the sweep had been reading three archives when the build makes five** — and
the first correction to that sentence said *four*, which is the shape worth
keeping. The one that mattered most was `libv8stubs.a`, the syscall stubs, i.e.
exactly the names a kernel is most likely to also define; the one the
correction then missed was `libkmemu.a`, which turned out to share two names
with `libv8sys.a` by a deliberate arrangement `noprocfs.c:10` documents and
nothing asserted. **A fix to a population bug is itself a population claim** —
count with `find`, do not recall. Same shape as the crash probe's fix that
added `/etc` and `/usr/lib/refer` and stopped one directory short.

**AND THE POPULATION WAS WRONG AGAIN, FOR A THIRD TIME, AND THIS ONE KILLED A
DESIGN.** "Pairwise between our own archives" is two of the three populations.
The third is **a PROGRAM's own objects against an archive**, and nothing had
ever swept it. §8a step 5e began by linking `libv8kern` into `cat`, since a
v8fs mount needs `namei` in the client. `ld`:

```
tentative definition of '_buf' with size 4096 from bin/cat.o
  is being replaced by real definition of smaller size 8
  from libv8kern.a[18](main.o)
```

`cat.c:10` is `char buf[BLOCK]`, 4096. `shim/kern/sys/main.c:213` is
`struct buf *buf`. Swept: **56 pairs, 33 objects, 29 programs, 27 names, and 25
of the 56 are silent** — on the 1985 vocabulary (`buf bread alloc bmap tty file
bwrite getblk iput itrunc panic copyin copyout`), with the checkers
over-represented because they reimplement the kernel's algorithms under the
kernel's names. Four things generalise:

- **It needs no `-force_load`.** One undefined reference to a kernel entry
  point pulls `main.o` in, and the collision is a consequence of the natural
  link. The `-force_load` in `KMEMU_LDADD` makes it *more* likely, not
  necessary.
- **Whether you notice is a property of the LAYOUT.** The same two objects:
  under `-force_load`, SIGSEGV (exit 139); in the natural link, **exit 0 with
  byte-identical output**, having written 4088 bytes over `_buffers` and
  `_nbuf` — the buffer cache's own pointers, which `nm -n` puts 8 and 16 bytes
  past `_buf`. A test on the output passes either way.
- **Hiding the symbols gets 22 of 27 and stops at exactly the wrong five.**
  `ld -r -exported_symbols_list` makes the `T`/`D` names private; it **cannot
  make a common a private extern**, so `bootime ecmx nswap runout tty` survive
  — and common-against-common merges by taking the larger size with no
  diagnostic at all. The mitigation converts the loud half into the quiet half.
- **The verdict table needs two more rows.** Beside `T`/`T`, `C`/`T` and
  `C`/`C`, the sweep found **`C`/`D`** (13 — the `cat` case, a common against
  an *initialised* definition, which warns) and **`T`/`C`** (1, the same thing
  inverted and silent).

`tests/kmemu` now sweeps this third population too, and asserts the thing that
actually matters: **in the built binary, a name the program declared as a common
must live in program storage and not in `__TEXT`.** Six such pairs exist in the
live link lines today (`od/max`, `dc/log10`, `mkfs/utime`, `nroff` and
`troff/nlist`, `sh/tmpnam`) and all six resolve correctly — only because nothing
pulls the archive member in. Derived every run, never transcribed.

**AND A SUPPRESSION ARGUED FOR ONE THING COVERS A DIFFERENT THING: FOURTEEN
MACROS WERE COMPILED AS CALLS TO UNDEFINED FUNCTIONS.** `KERNFLAGS` carries
`-Wno-implicit-function-declaration` because the imported half is 1985 K&R and
the diagnostic would fire on every line. That was argued for *declarations*.
It also covers a missing **macro** — and `BSIZE(dev)` with no macro in scope is
not an error and not a warning, it is a call to a function named `BSIZE`. The
build was clean. `BITFS BMASK BSHIFT BSIZE INOPB MIN NINDIR NMASK NSHIFT
dbtofsb fsbtodb itod itoo major`, all fourteen, found by subtracting what
`libv8kern.a` defines from what it undefines — the only instrument that could,
because we had told the compiler not to speak. A missing declaration changes
what is *checked*; a missing macro changes what the code **means**.

**A missing libc function does not fail the link — it resolves from `-lSystem`.**
For a non-variadic function that silently works and hides the gap. For a
*variadic* one it is an ABI mismatch: v8cc passes every argument positionally in
x0–x7, Apple's ARM64 ABI passes variadic arguments on the stack. This bit three
times (`scanf`, `printf` via the driver, `execl` — the last made `system()` start
an interactive shell that looked exactly like a hang). `tests/libv8c` guards the
variadic shape, and `tests/kmemu` sweeps **every Mach-O in the rootfs** with
`nm -u` — which found five more the first time it ran, including a `getgrent`
that made `ls -g` read the Mac's group database from inside the jail. Note what
that says about `tests/freestanding`: it links its own small programs, so it
proved the shim was clean and never the world built on it. **A guard on a seam is
not a guard on what crosses it.**

`tests/kmemu`'s allowed list names each remaining import with its reason, and the
suite fails if an entry goes stale. **It is empty** — `ALLOWED=""` — and getting
there took two deletions, both forced by the staleness check rather than
noticed. `sleep` came off when signal delivery worked and V8's own `sleep.c`
built. `libm` came off when V8's math went into `libv8c`; it had been excused on
the grounds that it was "non-variadic, so it works", and it was in fact
returning wrong answers the whole time. **An entry on that list is a claim, and
the staleness check is the only thing that has ever audited one.**

**The struct a syscall takes is not always the struct libc takes.** For four
months no V8 program in this port could catch a signal, because `v8s_signal`
handed the raw `sigaction` syscall a userland `struct sigaction` where the
kernel wants `struct __sigaction` — 24 bytes, with a signal-trampoline pointer
at offset 8, exactly where the userland struct keeps `sa_mask`. Every handler
was installed with a null trampoline; `sigaction` returned 0 and nothing looked
wrong until delivery, when the process hung or died. Fixed:
`shim/v8sys/sigtramp.s` is the trampoline the kernel enters, and `shim/NOTES.md`
has the whole account. Two things generalise. **libc's wrapper is often not a
thin one** — `sigaction()` exists largely to convert between those two structs
and fill in a trampoline, so "the shim goes straight to the kernel" means
inheriting work libc was doing. And a struct at this seam that is the wrong
shape costs *nothing* at the call and fails much later, so the ones this port
depends on are now `_Static_assert`ed on size and offset.

**A guard on numbering is not a guard on delivery.** `tests/v8sys` checked that
signal numbers translated and never that a handler ran, so a shim in which no
handler could ever run passed 44 of 44. Same family as the `tests/freestanding`
gap below. Delivery cases each fork a child with a deadline, because the failure
mode is a hang rather than a wrong answer — run inline, the first one takes the
suite down and prints nothing.

**A stale object does not look like a build problem — it looks like wrong code.**
Four debugging rounds went to correct source compiled from already-fixed files.
`tests/deps` exists for this; see below.

**Files `#include`d that are not headers** are invisible to every dependency
scanner *and* to a `*.c` glob. **Thirteen**, not the seven this used to list:
`lex/ldefs.c`, `lex/once.c`, `tbl/t..c`, `refer/refer..c`, `refer/what..c`,
`make/defs`, `yacc/dextern`, `yacc/files`, `yacc/y.debug`, `ccom/common`,
`ccom/y.debug`, `cpp/yylex.c`, `eqn/e.def`. All are declared explicitly in the
Makefile **except `refer/what..c`**, which feeds `whatabout` — not in
`REFER_PROGS`, so this port does not build it and the gap is latent rather
than live. Re-derive the list rather than trusting it; the `#`-then-space
spelling is why it is easy to miss one:
```bash
grep -rnE '#[[:blank:]]*include[[:blank:]]*"[^"]*"' src/cmd | grep -v '\.h"'
```

**The compiler has no known unimplemented feature.** The last one was `STARG`,
passing a struct by value, which went unnoticed through 156 Wave A programs and
all of Wave B and C because none of them does it. `grap` does. `placeargs()` in
`compiler/ccom-arm64/gencode.c` now copies the aggregate into **consecutive
argument slots** — the V8/VAX convention, deliberately not AAPCS64's
by-reference rule for composites over 16 bytes, because v8cc passes every
argument positionally and a second convention for one node type would make it
neither. PLAN.md §4f records the decision and what it was measured against.

Two things there are worth carrying forward. `countargs()` counts **slots, not
arguments**, and getting that wrong is silent — the program builds, links and
emits nothing. And `stn.stsize` arrives at pass 2 **already rounded** to a
multiple of `ALSTACK`, because `argsize()` mutates the node in place; so the
copy reads a few bytes past a struct whose size is not a multiple of 8, exactly
as the VAX did from the same field.

**The frame has THREE regions, and fixing one collision created another.**
`arm64_endfunction()` in `emit.c` lays out, top to bottom: saved `x29/x30`,
locals, callee-saved registers, call area. The saves cannot sit immediately
below `x29` — pass 1's `oalloc()` hands out automatics as negative offsets from
the frame pointer under BACKAUTO, so every save lands on a local, and the first
real libc function died on it. The fix moved them to the *bottom*, which put
them exactly where AAPCS64 puts the ninth and later arguments of a call:
`[sp, #0]`. A function that both used register variables and called something
with more than eight arguments **overwrote the register it had saved on behalf
of its caller**, and handed the corrupted value back on return.

Two lessons, and the second is the reusable one:

- **"When a back-end bug depends on a code shape, count the shapes before
  assuming coverage" — AND THEN THE COUNT ITSELF WAS WRONG.** This said "a
  sweep found exactly four functions in the whole tree with a >8-argument
  call: `printp` in `ps`, `dfree` and `main` in `df`, `ngs` in `ls`", and used
  that to explain why Wave A, B and C never saw the bug. Re-measured with a
  paren-matching scan of `src shim compiler`: **at least 25 (file, function)
  pairs** make a call with nine or more arguments, and Wave C is full of them
  — `eqn`'s `fromto`, `bshiftb`, `shift2`, `text`; all four of `pic`'s
  `*gen.c`; `grap`'s `do_autoticks` (ten arguments); `lex`'s `statistics`;
  `troff`'s `error` and `loadfont`. `dprintf` is `if(dbg)printf`
  (`src/cmd/pic/pic.h:1`, `src/cmd/eqn/e.h:3`), a real call. Those programs
  were all in `src/cmd` when the sweep ran, so it was wrong on the day.

  The bug needs a function that uses **register variables** *and* makes the
  wide call, so the four named are the ones that happened to have both — but
  ">8 arguments" was never the rare half, and the sentence explained the
  coverage gap with the wrong number. Count the shape that actually gates the
  bug, and say which half you counted.
- **A test that checks the callee RECEIVED its arguments cannot see the caller
  being destroyed.** `tests/v8ccom` already had two nine-argument cases and both
  passed throughout. The new ones need **three frames**, because the damage
  lands on the caller of the function making the wide call.

## Build-system discipline

The Makefile is written defensively for reasons recorded in its comments. Before
changing it:

- **Define variables above first use.** Make expands a variable in a target *or a
  prerequisite* when it reads the rule, so a variable defined lower down expands
  to nothing and the dependency silently is not there. This has happened three
  times. `tests/deps` now fails on any `--warn-undefined-variables` warning.
- **A RULE THAT LINKS SOURCES DIRECTLY PRODUCES NO `.d`, SO ITS HEADER EDGES DO
  NOT EXIST.** `$(DEPFLAGS)` gives every ordinary object its header
  dependencies; `$(BUILD)/v8sys/test` and `$(BUILD)/v8sys/p9clprobe` compile
  `$(SHIM_SRC)` straight into a host binary in one step, so they get none, and
  for a long time listed no headers at all. What that costs is not a build
  failure: §8a step 5f added three slots to `struct v8fstyp`, and a binary built
  from a stale `vfs.h` has a **table with the wrong number of entries** —
  `t_access` becomes whatever field sat at that offset before, it links, and it
  dispatches into the wrong function. `$(SHIM_HDR)` is named on both rules now,
  with the recipes changed from `$^` to `$(filter %.c %.s,$^)` so a header can
  be a prerequisite without becoming a compiler input.
- **macOS ships GNU Make 3.81.** No grouped targets (`&:`) — a two-target rule is
  two rules sharing a recipe, which races under `-j`. Mtimes compare at
  **whole-second** granularity though APFS records nanoseconds, so a file edited
  in the same second as the build that consumed it is missed.
- **make's built-in `%.c: %.y` and `%.c: %.l` rules are cancelled, and must stay
  cancelled.** The built-in yacc recipe ends `mv -f y.tab.c $@`, and
  `src/cmd/ccom/common/cgram.y` sits next to the *checked-in* `cgram.c` the ccom
  rules deliberately use. So `make -B` on anything reaching `cgram.o` rewrites
  authentic source in place, with a different yaccpar and absolute paths in its
  `# line` directives. Seen happening here; `git diff` was the only thing that
  noticed. `tests/deps` asserts the built-ins stay dead.
- **Never depend on a phony target** for a real file. Phony means always out of
  date; that once recompiled 39 objects on every single `make`.
- **Order-only (`| foo`) means "exist before me", not "I depend on you."**
- Chained pattern rules make the middle file an *intermediate*, which make
  deletes. `.SECONDARY` where that matters.

`tests/deps` asserts the graph with `make -q` (nothing is compiled) and includes
negative controls. If you add build rules, add cases — and verify by mutation
that they can fail.

## Automation in this repo

`.claude/` carries seven things, all of them encoding a bug that has already
happened here:

- **`hooks/block-third-party.sh`** (PreToolUse) refuses any write under
  `third_party/`, which would silently destroy the provenance hashes.
- **`hooks/v8-make.sh`** (PreToolUse, Bash) refuses the **host's** make where an
  authentic V8 makefile is what would be read — `cd src/cmd/lex && make`,
  `make -C`, `make -f`. Rungs 4 and 5 exist to prove the build *description* is
  Bell Labs' and not ours, and GNU make would run it perfectly well, which is
  the problem: the objects come out and nothing says the rung did not happen.
  Keyed on `PROVENANCE` rather than a list of names, so a program is covered the
  day its makefile is imported. The everyday `make -j8` / `make test` is
  untouched by construction — our own Makefile has no PROVENANCE line.
- **`hooks/ci-green.sh`** (PreToolUse, Bash) refuses `git push` when the last
  push is still red, or when `make test` has not passed since a source file
  changed. **It cannot check CI for the commit being pushed** — that run does
  not exist yet, which is the circularity in the obvious reading of "green
  before push". What it gates is the two things that actually fill a mailbox:
  piling commits onto a red build, and pushing untested. Nine of the ten failure
  mails from the session that added it were the first shape. It **fails open**
  on no `gh`, no login, offline, no run found or a run still going — a push gate
  that blocks when GitHub is unreachable is off within the hour. Override with
  `PUSH_ANYWAY=1 git push`, which is also how the fix for a red build goes out.
  The "tested" half reads `build/stage0/.tests-passed`, which `make test` writes
  as its recipe and therefore only when all seventeen suites passed.
- **`hooks/article-fresh.sh`** (PreToolUse, Bash) refuses `git commit` when the
  commit changes `src/`, `shim/`, `compiler/`, a Makefile or `tools/` without a
  SUBSTANTIVE entry in ARTICLE.md. The substantive part is the whole hook:
  "must include ARTICLE.md" would have passed all four of the commits that let
  it go stale, because each one touched the file and moved only the test count.
  `$ARTICLE_MINLINES` (6) is the line between them, and it is measured rather
  than chosen -- the real entry in that history was +116, the hollow ones +1.
  Overridable with `ARTICLE_ANYWAY=1 git commit`, which is how a genuine
  one-line correction gets in; a commit touching only tests, docs or the
  article itself is not gated at all, because a hook that stops the everyday
  commit is off within the hour.

  **AND ITS PRESENCE CHECK IS SUBSUMED BY ITS LENGTH CHECK**, found by
  mutation: deleting the former changes no verdict, because an absent file has
  a line count of zero. The two differ only in what they SAY, and that is the
  half worth keeping -- "you did not write it up" and "you moved a number" send
  a person to different places. So the two cases assert the MESSAGE, and the
  mutation fires again. A block/pass case could not tell them apart and
  reported the mutation as harmless.
- **`hooks/check-makefile.sh`** (PostToolUse) runs
  `make -n --warn-undefined-variables` after any Makefile edit, and flags
  multi-target rules that carry a recipe. ~60ms.
- **`agents/lp64-auditor.md`** — subagent for the width, collision and variadic
  hazards. Run it on a freshly imported program before building — **and on the
  shim code written to make that program build, which is where it has actually
  found things.** Measured on the `streamio.c` import: 1093 lines of authentic
  source came back clean for the dominant class, and both live findings were in
  the shim written that hour. That is not luck. The imported code had been
  surveyed at length before it was imported; the new code was written under the
  confidence that survey produced, and whoever writes the fold is the person
  least able to see the bare cast on the next line.
- **`skills/port-program/`** — the workflow below, with `audit.sh` bundled.
  Invoke with `/port-program NAME`.

`tests/hooks` covers the two blocking hooks, because a hook fails in the
direction that is hardest to notice: it lets something through and says nothing,
so the tripwire simply is not there. `v8-make.sh` had two such bugs in its first
draft — two `jq` calls on the same stdin, so `cwd` came back empty and every
command looked like it ran at the project root; and a split on `&&` that threw
away the `cd` in `cd src/cmd/lex && make`, the single most likely spelling of the
mistake it exists to catch. Both passed a casual look. The negative cases matter
as much: a hook that blocks the everyday build gets switched off within the hour.

CI (`.github/workflows/ci.yml`) builds and tests on **`macos-26`** (ARM64 — an
x86 runner would not exercise the AAPCS64 bugs this port keeps finding), then
asserts that a no-op `make` does zero work and that a clean `-j8` build passes.
This paragraph said `macos-14` until the workflow was re-read; `macos-14` is
deprecated (actions/runner-images#13518) and the pin moved without the prose
following it. The workflow file is the authority, and it says why in a comment.

**There are TWO jobs now**, and the split is about time-to-signal rather than
tidiness. `build-and-test` is the fast one. `crash-probe` runs
`tests/crash-probe.sh` — 6360 invocations, ~13 minutes — against
`tests/crash-probe.floor`, plus the `PROBE=mutating` set whose expectation is
**empty**. In one job the fast signal would arrive thirteen minutes late on
every push, and a slow check is one people stop reading. Note the probe must
be the only thing touching the tree while it runs: a concurrent rebuild
replacing a Mach-O mid-execution shows up as SIGKILL, which the script counts
as *tainted* and now refuses to call a pass.

## Porting a program

The recurring task. Steps 4 and 6 are the ones most often skipped and the two
with a history of costing multi-round debugging. `/port-program NAME` walks it.

1. `tools/import.sh v8/usr/src/cmd/NAME` — never copy by hand; this records the
   upstream blob hash in `PROVENANCE` so the diff against pristine V8 stays
   reconstructible.
2. LP64 audit before building, not after. The hazard shapes:
   ```bash
   grep -nE 'char \*[a-z_]*\(\);|\(int\) *signal|int +[a-z]+ *= *(malloc|sbrk)' src/cmd/NAME/*.c
   ```
3. Makefile block. Use `$(V8CC_DEPS)` on object rules and `$(V8DEPS)` /
   `$(V8LIBS)` / `$(V8LDFLAGS)` on the link rule — never respell the library
   list, and never introduce a variable below its first use.
4. **Declare any `#include`d file that is not a `.h`.** Invisible to every
   dependency scanner *and* to a `*.c` glob. Note the `[[:blank:]]*` after the `#` —
   V8 writes `# include`, so a pattern anchored on `#include` silently finds
   nothing, which is the failure this step exists to prevent:
   ```bash
   grep -rnE '#[[:blank:]]*include[[:blank:]]*"[^"]*"' src/cmd/NAME | grep -v '\.h"'
   ```
5. Add the program to `.PHONY` and to the `stage0` target.
6. Add cases to `tests/deps` for the new rules, including step 4's. Verify by
   mutation that they can fail.
7. Write `src/cmd/NAME/PORTING.md`: what changed and why, what was eliminated by
   measurement, what is still open. Then add cases to the relevant wave suite.

Object files land in `build/stage0/NAME/`; `rootfs/` is the installed view that
`$V8ROOT` points at, and the copies there are real make targets — a program is
not testable until it is installed.

## Conventions

- Each ported program gets a `PORTING.md` recording what changed and **why**, what
  was eliminated by measurement, and what is still open. These are the project's
  memory; read the relevant one before touching a program.
- Prefer measuring over reasoning. The hardest bugs here were settled by making
  the program print what a value *is* rather than arguing about what it should
  be — `V8DBG=1`, or logging what `malloc` wrote versus what was found there later.
- **`make … | grep …; echo $?` REPORTS THE GREP, AND HID A FAILED BUILD FOR AN
  HOUR.** `make -j8 2>&1 | grep -iE "error|warn" | head -20` looks like a
  careful check and is not one: the exit status belongs to the last element of
  the pipeline, the *previous* build's archive is still sitting there, and the
  next thing you run links against it. What that cost here was a debugging round
  spent on a shim change that had never been compiled. Run
  `make -j8 > /tmp/mk.log 2>&1; echo "make $?"` and grep the log afterwards —
  the log is also the thing you need when the failure is real.
- **A MEASURING INSTRUMENT YOU WROTE IS A SUSPECT, AND THIS ONE WAS WRONG THREE
  TIMES.** `tests/crash-probe.sh` runs every installed binary against every
  single-letter option and counts signal deaths. It reported 254, then 195,
  then 148, then **96** — and only the last is true. Each error inflated the
  count, which is the direction that wastes the most time, and each looked
  authoritative:
  - **It was not hermetic.** All invocations shared one working directory, so
    programs read each other's litter — `yacc` leaves `(null).tab.c`, others
    leave a file named after the option they got. `dcheck` then "crashed" on 45
    options, because its loop calls `check(*argv)` for *every* argument
    including options, so `dcheck -Q` opened a file literally called `-Q` and
    read a superblock out of it. A prober must be a pure function of the
    program and its arguments, or its findings are a function of iteration
    order.
  - **The shell cannot tell a signal from an exit status.** `$?` is 128+N for a
    signal, but a program may `exit(134)` itself — and a V8 `main()` that falls
    off the end returns whatever was in the register. `primes` did, and 42 of
    its garbage statuses landed in 129..159 and were counted as SIGABRT. Run
    the child under something that keeps the real wait status and ask
    `$? & 127`.
  - **And the first diagnosis of the first fault was wrong.** Signals 9 and 10
    in one program and no other reads exactly like a concurrent rebuild
    replacing a Mach-O mid-execution — so a filter was added discarding SIGKILL
    *and SIGBUS*, which would have hidden 48 genuine crashes. SIGKILL is never
    a program bug; SIGBUS very much can be.

  **AND THE FLOOR IS 54, WHICH NOTHING SAID UNTIL NOW.** Re-measured on the
  installed rootfs: **4187 invocations, 54 signal deaths, all SIGSEGV — 53
  `lex` and 1 `bcd`**, and every one of them is already argued somewhere as
  upstream's defect on upstream's hardware (`src/cmd/lex/PORTING.md:175-250`
  for the 53, PLAN.md §4j's triage table for `bcd` — a SECTION rather than a
  line, because that one citation went stale three times in a single session
  as §7c grew above it, and each correction was itself invalidated by the next
  edit. When a target sits in a file you are actively editing, cite something
  that does not move). `lex`'s 53 is *exactly* the number that
  file predicts, and it explains why fixing the first of its three faults moved
  the count by zero: the probe feeds every program `/dev/null`, so all 53
  invocations also reach the empty-spec path and the 40 that used to die in
  `warning()` now die further along in `ctail()`.

  The number is recorded here because its absence cost a run and a diagnosis:
  `54 died on a signal` reads as a regression, and establishing that it was not
  meant excavating two other files. **A prober whose expected output is nonzero
  needs its floor written down beside it**, or every future run starts with a
  scare. If this ever reads 55, the new one is the finding.

  **AND IT NOW READS 160, WHICH IS THE FLOOR ITSELF GOING STALE -- THE EXACT
  FAILURE THIS ENTRY WAS WRITTEN TO PREVENT.** Re-measured after ex/vi landed:
  **6360 invocations over 120 programs, 160 signal deaths, all SIGSEGV.** 106
  of them are new and none is `ex`:

  | program | count | |
  |---|---|---|
  | `lex` | 53 | the known floor |
  | `diffh` | **53** | NEW |
  | `cb` | **50** | NEW |
  | `cpio` | **2** | NEW |
  | `tar` | **1** | NEW |
  | `bcd` | 1 | the known floor |

  All four arrived with the Wave A2 batch, and **nothing measured them because
  the probe is not in `make test`** -- it is a manual instrument whose expected
  output is a number in this file, and a number only a human runs is a number
  that rots. Same shape as ARTICLE.md, one level further out: the guard against
  a stale count was itself a count with no guard. Task #76 carries the
  diagnoses; three are this port's address-0 class and the fourth is not:

  - **`diffh`** -- `while(*argv[1]=='-' && ...)` is the FIRST statement of
    `main`, so a bare `diffh` dereferences the NULL terminator, and so does the
    run after the loop eats the last option.
  - **`cb`** -- `fprintf(..., *argv[1])` in the `default` arm, which is a
    **precedence** bug: `*(argv[1])` is the first character of the NEXT
    argument where `(*argv)[1]` -- what the `switch` three lines above uses --
    was meant. Measured discriminator: `cb -a t.c` says *illegal option **t***
    and exits 1, so the wrong message is upstream's on any machine and only the
    crash belongs to this target.
  - **`tar -b`** -- `atoi(*argv++)` with `-b` last, the `ncheck`/`icheck`/
    `dcheck` shape a fourth time.
  - **`cpio`** -- two sites and only one of them is that class. Bare `cpio` is
    (`if(*argv[1] != '-')`). But **`cpio -i` is EOF handling, not argv**:
    measured, it succeeds on a valid archive and faults only on empty stdin.

  Two rules. **A partial probe log is not a population** -- `diffh` sorts after
  `lex` and was invisible in every intermediate read of the running log, so the
  finding grew from three programs to four only when it finished. And **a floor
  belongs in a suite or in CI, not in prose**, which is the open question Task
  #76 ends on.

  **AND FIXING THEM CORRECTED TWO OF THE FOUR DIAGNOSES ABOVE, WHICH WERE
  WRITTEN THE SAME DAY.** Both are left standing rather than deleted, because
  the errors are the interesting part.

  **Re-measured on the same population -- 6360 invocations, 120 programs --
  160 -> 55, exactly 105 removed.** The floor is **NOT back to 54**: it is
  `lex` 53 + `bcd` 1 + **`cpio -i` 1**, because that last one turns out to be a
  permanent member of the upstream's-defect-on-upstream's-hardware set rather
  than something to fix. Say 55 and say why, or the next person reads 55 as one
  unfixed regression. `tests/wavea` guards it now, so this paragraph is no
  longer the only thing that knows.

  - **The `cb` entry named ONE site and the count said TWO.** 50 of 53 is the
    tell: `-s` and `-j` continue and bare `cb` reads stdin, so 52 - 2 = 50, and
    49 of those die in the `default` arm while the fiftieth dies in `-l`'s
    `atoi(*++argv)`. The number was sitting in the table above the sentence.
    **A count that does not match the mechanism you have described is the
    mechanism telling you it is incomplete.**
  - **`cpio -i` IS NOT "EOF handling", and the real answer flips the verdict.**
    It is `chgreel()`'s unchecked `fopen("/dev/tty")` -- so it is the *`/dev/fd`
    class*, reachable because V8's `/dev/tty` is a link to `/dev/fd/3` -- and
    the `fgets` that follows is a `getc`, which is `--(p)->_cnt`, a **WRITE** to
    virtual 0. ZMAGIC text is read-only, so **a VAX faulted there too**: there
    is no answer to restore and S1 forbids the change. Third member of the
    family after `pr.c`'s `Ttyin` and `troff/hc.c`'s `rcf`. Measured both ways
    -- no fd 3 gives SIGSEGV, `exec 3</dev/null` gives a clean exit 2 -- which
    also explains why it would not reproduce under **lldb**, a debugger leaving
    an fd 3 open. *The instrument removed the condition it was pointed at.*

  So the class splits on one question that no symptom shows you: **is the page-0
  access a READ or a WRITE?** A read has a VAX answer to reproduce; a write
  never did. Three fixes here restore an answer (`diffh` -> `must have 2 file
  arguments`, `tar` -> `Invalid blocksize`, `cpio` -> usage and exit 2), and
  `cb`'s restores a **NUL in the middle of its message**, because `%c` of
  address 0 is what a VAX printed.

  **AND cb's FIX HAD TO PRESERVE A BUG, which is a shape this file has not
  recorded before.** `*argv[1]` naming the wrong argument is a *precedence*
  error and it is upstream's on upstream's hardware -- measured, `cb -a x.c`
  reports `illegal option x` on any machine -- so S1 forbids correcting it while
  requiring the crash be fixed. The two live on one line. `tests/wavea` therefore
  carries a case whose entire purpose is to assert that a bug is **still there**,
  so that repairing it has to be a decision rather than a tidy-up.

  **AND THE PROBE IS IN CI NOW, WHICH REQUIRED IT TO GROW AN EXIT STATUS -- IT
  HAD NEVER HAD ONE.** For its whole life it printed a number and returned 0, so
  "the floor" was a human comparing prose to a terminal. `tests/crash-probe.floor`
  is the expectation, the script diffs against it, and `.github/workflows/ci.yml`
  runs it as a **separate job** (~13 min, against `make test`'s well under one --
  same job would triple time-to-signal and people stop reading it). Five things
  generalise, and three are about the shape of the expectation rather than the
  plumbing:

  - **THE EXPECTATION IS A LIST, NOT A COUNT, and it is checked BOTH WAYS.** A
    count lets two changes cancel. And a crash that has *gone* fails too, because
    a floor that over-states is where the next regression hides -- so fixing
    something requires deleting its line in the same commit, which makes the
    removal reviewable instead of silent. Same discipline as `tests/kmemu`'s
    allowed-import list, which is the only thing that has ever audited one.
  - **A MISSING FLOOR FILE IS A FAILURE, NOT A SKIP** -- `tests/cpp`'s
    `if [ -d "$V8INC" ]` reporting `12 passed` is the precedent, and a probe with
    no expectation is precisely the thing the file exists to stop.
  - **`comm` NEEDS BOTH SIDES IN ONE COLLATING ORDER, WHICH IS A HOST PROPERTY.**
    Unset, a runner's locale could pair the wrong lines and report entries as
    simultaneously new and gone -- on some machines. `LC_ALL=C` makes the order
    ours. The committed order of the floor file is then irrelevant, because both
    sides are re-sorted by that same sort.
  - **A TAINTED RUN MUST NOT READ AS A PASS.** SIGKILL means something rebuilt
    the tree mid-run; the script already refused to count those as findings, and
    now refuses to call the run clean either.
  - **AND A 13-MINUTE JOB IS A SLOW WAY TO LEARN THE EXPECTATION FILE IS
    MALFORMED**, so three millisecond-cost cases in `tests/wavea` check that it
    exists, that every line parses, and that **every program it names is still
    installed** -- a floor naming a deleted program can never be satisfied and
    would report "gone" forever.

  **AND THE FIRST CI RUN FOUND A CRASH NO LOCAL RUN HAS EVER SHOWN, WHICH IS
  THE WHOLE ARGUMENT FOR PUTTING IT THERE — AND IT BROKE THE FLOOR'S DESIGN.**
  `tar -u` SIGSEGV'd on a GitHub macos-26 runner and has never done so here.

  - **A BOTH-DIRECTIONS FLOOR CANNOT EXPRESS A CRASH THAT IS NOT A PURE
    FUNCTION OF THE TREE**, and that is arithmetic rather than an oversight:
    present, the entry fails wherever it does not fire; absent, it fails
    wherever it does. No value is correct on both. So there is a third
    category — a `?` prefix, removed from **both** sides — and it must stay
    small and loud or it becomes the allow-list that swallows everything. The
    count is printed every run, the entries are named, and `tests/wavea`
    asserts the tolerated set is **exactly one**, so a second needs a
    deliberate edit in two files.
  - **AND I DIAGNOSED IT WRONG TWICE BEFORE MEASURING THE RIGHT THING.** First
    as *host-dependent* — "dies on a runner, not here" — on a **single**
    observation in each direction, which is not a reproduction; the next CI run
    cleared it three times. Then as an **unreproduced intermittent**, filed
    beside #50/#61/#69. Both wrong, and the second was wrong in the more
    comfortable direction: "intermittent, cause unknown" is a diagnosis that
    asks nothing further of you.
  - **THE CAUSE WAS CONTAINMENT, AND IT WAS IN THE TEST RATHER THAN THE PORT.**
    `tar -c` with no `-f` falls back to `usefile = magtape = "/dev/rmt1"`, the
    open fails, `cflag` is set so it **creats** it — and creat keys on the
    parent, which inside the jail is `$V8ROOT/dev`, **a directory that
    exists**. So it writes a 10240-byte tape *into the rootfs*, and every later
    `tar` finds a tape that opens and takes a different path. Measured both
    ways: `tar -u` exits **1** without the leftover and **0** with it. So
    tar's results are a function of ITERATION ORDER, which is this script's own
    most-documented rule — programs reading each other's litter — arriving
    through an **absolute path in the jail** rather than through the shared
    working directory the earlier fix addressed. A fresh cwd per invocation
    cannot contain a program that writes to `/dev/rmt1`.
  - **`tar` WAS SIMPLY MISSING FROM `MUTATES`, and `dump` and `restor` — the
    same shape, a default tape — were already in it.** That is the tell: the
    list was assembled from what obviously writes, and the third member of the
    family was missed. **So the probe checks its own containment now**, every
    run: the rootfs's file LIST before and after, where a **new path** is a
    program that escaped its cell. Mutation-verified — put `tar` back in the
    safe set and it names `/dev/rmt1`.
  - **IT IS THE PATH LIST RATHER THAN A CONTENT HASH, AND THAT IS FORCED.**
    libkmemu manufactures `/unix`, `/dev/kmem`, `/etc/utmp` and `/etc/mtab` on
    first read with **live** data, so their contents move every run by design
    and a content comparison would be noise nobody reads. **And measuring beat
    assuming a second time**: the first draft said those files "already exist
    in a built rootfs, so they are not additions and need no exemption" —
    true of a tree something had already used, false of a fresh one. Deleted
    all five and rebuilt: `make` restores only `/etc/fstab`. So the other four
    ARE additions on a clean CI checkout, and the guard would have fired on our
    own shim. They are exempt by name and **used exemptions are printed**, so
    the list cannot quietly cover something new.
  - **AND THE GUARD'S FIRST REAL RUN FOUND A SECOND ESCAPE: `/dev/null`.** A
    jailed `creat("/dev/null")` makes a **regular file** in the rootfs, by the
    same parent-keyed fall-through tar used — and it surfaced only on a fresh
    checkout, because this tree had had one for so long that nothing noticed.
    **V8 shipped `/dev/null`** (`proto-dev:25`, `crw-rw-rw- ... 3, 2 null`), so
    its absence from `ROOTFS_DEVSTD` was a *gap* rather than a decision, and
    the fix is to materialise it beside `tty`/`stdin`/`stdout`/`stderr`. What
    that does **not** fix is that writes to it accumulate instead of being
    discarded — nothing writes enough to notice today, which is precisely the
    "not observable yet" this file keeps getting caught by. Task #78, and the
    fix is a `vfs.c` slot rather than a file.
  - **THE SHAPE TO CARRY: A CONTAINMENT CHECK IS ALSO A COMPLETENESS CHECK ON
    THE ROOTFS.** Both findings are the jail's creat fall-through, and both
    were invisible on a tree that had been used. Ask of any such guard whether
    it would say the same thing on a tree that has never been run — which is
    the same question this file already asks of green suites.
  - **AND IT COST TWO RED CI RUNS, BOTH OF THEM MY SWEEP.** `tar -c` and
    `tar -r` each crashed once on a runner and a create case saw an empty
    archive — all downstream of one leftover tape. `tar` is out of the wavea
    sweep now (containing it properly costs a clone per invocation, 53 ×
    0.146 s, which `make test` should not pay) and swept in CI under
    `PROBE=mutating`; its own fix keeps its own targeted cases.
  - **THE `?` MECHANISM WAS BUILT FOR ONE ENTRY AND DIAGNOSING THAT ENTRY MADE
    IT UNNECESSARY**, which is the argument for not reaching for an escape
    hatch early. It stays, verified and **empty**, with `tests/wavea` asserting
    the set is exactly 0 — the bar for adding a member is now visibly high,
    because the only candidate so far had a real cause sitting behind it.
  - **AND `tests/wavea` DERIVES ITS EXPECTATION FROM THE FLOOR FILE NOW**,
    rather than spelling the list a second time. Two hand-written copies of one
    list agree with each other about a set that is wrong — measured here once
    already, when v8fsd's and p9cl's errno tables agreed perfectly about a set
    missing seven names.

  **AND EDITING A RUNNING SHELL SCRIPT CORRUPTS IT, WHICH IS THE
  never-edit-while-a-suite-runs RULE WITH A MECHANISM.** `sh` reads a script
  incrementally rather than slurping it, so inserting lines *below* the
  currently-executing point shifts the read offset and the shell resumes
  mid-token. It happened here -- the floor block was edited 8 minutes into a
  13-minute run -- and the right response is to kill the run, because a garbled
  one is a measurement you cannot distinguish from a finding. The existing rule
  was about a suite rebuilding what another suite reads; this is the same rule
  reaching the interpreter itself.

  Validate a prober against a **known crasher and a known-clean program**
  before believing any number from it. And note what survived all four runs
  unchanged: the *set* of programs, which is what the fixes were driven from.

  **AND A FOURTH WAY, WHICH IS THE SET ITSELF: IT WAS TRANSCRIBED.** The scan
  was six literal globs, and `$ROOT/usr/lib/spell/*` treats spell as a
  *directory* by analogy with `refer` — `/usr/lib/spell` is a Mach-O **file**,
  V8's spellprog, the binary the `/usr/bin/spell` script calls. So it matched
  nothing. `/usr/lib/man` was not named at all. Same direction as the original
  `/etc` omission: the fix that added `/etc` and `/usr/lib/refer` **stopped one
  directory short**. Both are clean (106 invocations, zero signals), so it cost
  nothing this time — a hole that happens to be empty still hides the next
  thing to fall in it. The population is now **derived** (`find` over the
  installed directories, filtered to Mach-O) and verified to be the old set
  plus exactly those two, 95 → 97, with **every** Mach-O in the rootfs
  reachable. The script prints what it found, because the mutating count had
  already drifted 17 → 18 (the `v8` launcher) with nothing to say so.

  The old one-blob `SKIP` is now a **classification**, which is the part worth
  copying. `UNSAFE` escapes the jail and no throwaway rootfs contains it —
  `halt`/`reboot`/`init`/`sync`/`mount` act on the host, `kill` signals host
  pids, `adb` wants ptrace on them, `as`/`ld`/`ar` are the host's by §1.
  `MUTATES` only changes things *inside* the jail, which the jail therefore
  bounds: `PROBE=mutating` gives each invocation its own `cp -ac` clone (0.146 s
  for 15 MB, measured) and runs the binary out of it. Re-measured putting it in
  CI: **21 programs, 1113 invocations, zero** signal deaths in 243 s, with the
  real rootfs byte-identical afterwards — containment proved by hashing before
  and after, not asserted. (This said 18 and 954; three more MUTATES programs
  have been installed since. The *count* went stale and the **expectation did
  not**, because "zero" is a property rather than a number — which is the
  cheapest available argument for stating an expectation as a property wherever
  one exists. The safe half cannot do that, which is why it needs a floor
  FILE.)
- A guard that has never been seen to fail is not a guard. New test suites are
  verified by mutation (break the thing, watch the test fail, restore). Two
  traps in doing that: **verify the object actually rebuilt** — mtimes compare
  at whole-second granularity, and a mutation that silently did not get
  compiled looks exactly like a test correctly passing (this has now produced
  two false "the guard did not fire" readings) — and remember that mutation
  proves a test can fail, never that it can *pass elsewhere*.

  **AND "THE OBJECT" IS NOT ONE OBJECT: CHECK THE ARTEFACT THE SUITE ACTUALLY
  READS.** A harness that watched `build/stage0/v8sys/dir.o` reported three of
  five mutations "meaningless, object did not rebuild" — and it had not, because
  `test-v8sys` compiles the shim **sources** straight into
  `build/stage0/v8sys/test` and never links that object at all. The same run
  called `tests/wavea` green under a mutation it should have caught, because
  wavea runs `rootfs/bin/ls` and only a *full* build relinks it. Both errors are
  in the safe direction, and both cost a round. Before trusting a rebuild
  check, read the rule that builds the thing the suite runs.

  **AND THE ARTEFACT CAN BE WRONG BY BEING NONDETERMINISTIC, WHICH IS THE SAME
  ERROR MAKING THE OPPOSITE NOISE.** A harness for the `ttyld` traffic cases
  hashed `libv8kern.a` to decide whether a mutation had been compiled — and
  `ar rcs` embeds timestamps, so **two builds of byte-identical objects differ**
  (measured: the archive changed, `ttyld.o` did not). The rebuild check
  therefore passed always, and the *restore* check cried wolf on a restore that
  was perfect. Both readings are useless in opposite directions from one bad
  choice of artefact. Hash the **object**; it is deterministic here, and it is
  also what the compile actually produced. And when the restore check does
  fire, confirm against the **backup**, not `git diff` — a file holding
  uncommitted work is dirty against HEAD no matter how clean the restore was,
  which produced a third false alarm in the same session.

  **AND THE SAME SENTENCE HAS A WORSE DIRECTION: `git diff` ON AN UNTRACKED
  FILE READS CLEAN UNCONDITIONALLY.** A mutation of a brand-new file wedged the
  suite, the run was killed, and `git diff --stat` on it printed nothing — not
  because the restore had happened but because the file was new and git had
  never seen it. The source sat mutated and the check said fine. The first
  direction cries wolf; this one reads as success, which is the direction that
  outlives the experiment. **Diff against the backup, or grep the content.**

  **AND A MUTATION CAN HANG THE HARNESS IN TWO PLACES, THE SECOND LOOKING
  EXACTLY LIKE THE FIRST.** Breaking a framing loop so it trusted one read made
  a receive block forever; a `SO_RCVTIMEO` on the reading socket fixed that and
  the very next run hung again — in `waitpid`, because the same mutation leaves
  the *child* blocked writing into a socket nobody is draining. **A deadline on
  one end of a pipe is not a deadline on the pipe.** Bound the wait as well as
  the read, and put both in the SUITE rather than only in the harness: a hang
  in `make test` reports nothing at all, which is strictly worse than a failure.

  **AND THE TRAP RUNS THE OTHER WAY TOO: THE RESTORE CAN BE THE THING THAT DOES
  NOT COMPILE.** Measured — `hunt1.o` and `hunt1.c` ended up with the same
  mtime to the second, `15:50:40`, because the mutate/build/test/restore cycle
  finished inside one second. make declared the object current, the restored
  source was never compiled, and **a mutated binary was left installed in the
  rootfs**. That direction is worse than the first: the mutation half reports
  correctly and looks like a clean verification, so nothing draws attention to
  it, and the damage outlives the experiment. It was caught only by running the
  full suite afterwards, which is the reason to do that rather than trust the
  one suite the mutation targeted. `touch` the source after restoring, or diff
  the built binary — and never end a mutation run without a full `make test`.

  **AND WHEN THE MUTATION IS IN A HEADER AND THE BUILD FAILS, THE OBJECTS THAT
  *SUCCEEDED* ARE THE STALE ONES — which is the opposite of where anyone
  looks.** A failing `make -j8` is not a build that did nothing: it compiles
  every translation unit it reached before the failing one stopped it, and
  those objects carry the mutated header. The one that FAILED produced no
  object at all, so make is guaranteed to redo it — and redoing it is exactly
  what makes the next build *look* like it did the work. Measured twice, the
  second time deliberately: mutating `P9_NAMELEN` failed at `p9cl.c`, and
  `p9.h`, `p9.o` and `p9io.o` all came out stamped the same second, so
  `make -q` called both objects **up to date** while flagging only the one that
  did not exist. They survived **two** further full builds.
  - **It presents as failures in a feature the mutation never touched.** Three
    `chown`-through-a-mount cases went red — a stale 9P *codec* on one side of
    the wire — and nothing pointed at the header. The discriminator was
    cheap and is the one to reach for: three identical failures, then `touch`
    the header and rebuild, then four consecutive passes with no source change.
  - **`cmp` against the backup is not the check.** It says the SOURCE was
    restored, which it was; it says nothing about whether anything recompiled.
    The one documented step skipped here was `touch` after restoring, and it is
    the whole remedy.

  **AND `git stash push` / `git stash pop` IS A MUTATION HARNESS, WHICH IS NOT
  WHAT IT LOOKS LIKE.** Stashing to measure a baseline — "how many warnings did
  this file have before my change?" — is mutate, build, measure, restore, and
  the same-second trap applies in full. Measured: a stash, a `make -B` of one
  object, and a pop all finished inside second `17:56:03`, `syscall.o` and
  `syscall.c` came out with the identical mtime, `make -q` said up to date, and
  the next full build linked **the pre-change object into every binary**. What
  that looks like is not a build problem: `access()` and `unlink()` behaved
  exactly as they had before the change, on a tree where every source file read
  correctly, and half an hour went into reading the new code for a bug that was
  not in it. The tell was on the wire — a server trace showing `Tstat` where the
  new code sends `Taccess`, i.e. the OLD code path, which no amount of reading
  the new source could have explained. `touch` every file the stash touched.

  **AND THE MUTATION THAT DOES NOT FIRE IS THE INFORMATIVE ONE — it says the
  guard is VACUOUS, which no green run ever will.** Seven mutations were run
  against the `/dev/fd` cases and six went red. The seventh deleted a
  `v8fs_unbind()` standing in front of a `v8fs_bind()` in `v8s_dup2` and changed
  nothing, because `v8fs_bind` already stores null for the passthrough type — so
  the call was dead code *and* the case named for it asserted nothing. The
  comment beside it had even said the two calls collapse, and kept both. Do not
  read a non-firing mutation as "the mutation was too weak"; check first whether
  the code it targeted does anything. Then delete the dead call, re-aim the case
  at the property that is load-bearing (here: `v8fs_bind` clearing the row
  rather than merging into it), and re-mutate to prove the new one fires.

  **AND THERE IS A THIRD REASON A MUTATION DOES NOT FIRE, WHICH IS NEITHER OF
  THE TWO ABOVE: THE DEFECT IS UNDEFINED BEHAVIOUR THAT HAPPENS TO GIVE THE
  RIGHT ANSWER.** `v8fsd`'s `do_seek` tests its overflow guard *before* the
  addition; an auditor had found the first version adding and then testing for
  negative. Reverting it left all 525 cases green — and the case is not vacuous
  and the code is not dead. Every overflow reachable there wraps to a
  **negative** value (both operands are in `[0, LLONG_MAX]`, so any sum past
  the top lands in `[-2^63, -2]`), so the broken form reaches the same `EINVAL`
  by executing UB. The defect is that the compiler is entitled to assume the
  overflow cannot happen and **delete the check** — invisible to every
  behavioural test, the `strncat` shape exactly.

  **The instrument for that class is a sanitizer, and it is cheap enough to be
  a permanent case.** `make` builds a second `v8fsd` with
  `-fsanitize=undefined -fno-sanitize-recover=all` and `tests/streams` runs the
  probe, `ls -l` and a 28000-byte `cat` through it. Current code: silent.
  Guard reverted: `signed integer overflow: 4611686018427387904 +
  4611686018427387904` and the process dies. Two things make it practical —
  **only our own code is instrumented**, because `libv8kern.a` is already
  compiled and Bell Labs' UB would be a different project; and the assertion is
  that the **server is still alive**, since `-fno-sanitize-recover` makes a
  dead peer the observable and a dead peer is harder to lose than stderr.

  **And it immediately found something it was not built for**, which is the
  argument for having it: when that server aborts, the client came back **141 =
  128 + SIGPIPE**. See the transport-semantics entry in the v8fs section — a
  socket's signal behaviour was reaching a V7 program that has no idea it is
  talking over one.

  **AND THE MUTATION THAT FIRES ONLY IN THE INDEPENDENT CHECKER IS THE ONE THAT
  JUSTIFIES HAVING ONE.** §8a step 5d broke `alloc()`'s free-list refill so that
  handed-out blocks repeat. **Every case in the probe stayed green** — the
  writes succeeded, `s_tfree` moved by the right amount, the bytes read back —
  and the only things that went red were `icheck`, `fsck` and a `cmp` on the
  image. A probe's writer and reader are one program and share its beliefs; a
  duplicate-block bug is invisible from inside by construction. So when the
  artefact under test has *independent* readers — here three of Bell Labs' own,
  which know nothing about the probe — run them and assert on them, and expect
  them to be the only thing that catches a whole class.

  Two more from the same session. **A mutation can kill the harness rather than
  fail a case, and that is a finding**: leaving `u_limit` zero made `writei`
  signal SIGXFSZ, which this port delivers with a real `kill(2)`, and
  `kill(0, sig)` took down the runner and the shell. A harness that filtered to
  `^FAIL` reported nothing at all — capture whole. And **bound every capture
  around a program the mutation may have made verbose**: a corrupt free list
  made `fsck -y` print for its entire deadline and bash died on the command
  substitution, so the suite could not report the failure it had caused.
- **`make -j8 test` IS NOT `make -j8` FOLLOWED BY `make test`, AND IT FAILS IN
  THE SHAPE OF A CODE BUG.** The two commands at the top of this file are two
  commands on purpose. Under `-j`, make runs the seventeen suites concurrently
  with each other's prerequisite *builds*, so a suite reads objects another
  suite's build is midway through writing. Measured: 42 failures across
  `libv8c`, `deps`, `wavea` and `mkfs`, four suites never ran at all, and every
  message read like a real defect — `libv8c`'s were all "(compile)", `deps`'
  were all "was already stale before the touch". Serially, **that same tree**
  was 1428 passed, 0 failed (the total of the day, not today's — the point is
  the pair, measured minutes apart on identical sources). The tell is the
  *shape*: whole suites failing on
  build steps rather than on assertions. This is the same root cause as the rule
  below — something modifying the tree while a suite reads it — arriving from
  make rather than from an editor.
- **NEVER EDIT SOURCE WHILE A SUITE IS RUNNING, and a filtered log cannot
  testify about what it filtered.** `make test` builds each suite's
  prerequisites when it reaches that suite, so an edit landing mid-run can
  rebuild binaries a later suite is about to test — an edit touching
  `gencode.c` rebuilds all ten `$(IMGBIN)` tools from a half-written
  compiler, and what comes out is one wrong branch rather than a build error.
  That is the leading explanation for the single `mkfs` `dcheck` failure that
  stood open for a session while three innocent hypotheses were measured and
  killed (120 clean suite runs, a rootfs hash diff showing only `/dev/kmem`
  changes, and `sync(2)` unmoved by 800 MB of dirty pages). It took that long
  partly because **the captured log had been filtered to `passed|failed|FAIL`,
  which discards every compile line, and its lack of build output was read as
  evidence that nothing had been built.** It is evidence of nothing. Capture
  runs whole, or say out loud what the filter removed.

  **AND IT RECURRED WITHIN HOURS OF BEING WRITTEN DOWN, WHICH IS THE POINT.**
  `tests/wavea`'s `pwd` case then failed once, under the same filter, so the
  `want`/`got` lines — the entire diagnosis — were thrown away again and the
  run had to be repeated to learn anything. `tests/wavea/run.sh:266` records
  what is known. Knowing the rule is not the same as having the habit: pipe to
  `tee` and read the tail, rather than grepping and hoping.

  The summariser in use was also wrong, in a way worth copying the fix for.
  `awk -F'[ :]+' '{p+=$2; f+=$5}'` over `wavea: 123 passed, 4 failed` splits to
  `$2=123 $3=passed, $4=4 $5=failed`, so **`f` accumulated the word `failed`
  and every "0 failed" it printed was arithmetic on a string, not a
  measurement.** Nothing was actually hidden — each suite prints its own `FAIL`
  lines, and a failing suite exits nonzero so `make` stops and the missing
  suites are conspicuous — but the number reported was not the number measured.
  The failure count is `$4`.
- **A test that asserts a property of the machine fails on some other machine,
  and mutation testing cannot see it.** Both CI breaks in this repo were this:
  `p_nice == NZERO` assumed the host's baseline nice is 0 (a GitHub runner
  starts jobs renice'd), and "some pid exceeds 32767" assumed a host that has
  been up a while (a runner is always freshly booted — the very property that
  let the 16-bit `p_pid` survive). Assert a *relation* the port controls — a
  difference between two processes, a field width — and where coverage genuinely
  depends on the host, print "not exercised" rather than passing silently or
  failing. `tests/kmemu`'s nice and pid checks are the worked examples.

  **AND A RELATION IS NOT ENOUGH IF THE INSTRUMENT'S RESOLUTION IS COARSER THAN
  THE EFFECT.** §8a step 5f gave the kernel a clock and asserted it with
  `ls -l`: a file written now must be newer than one `mkfs` wrote. The relation
  is right and the instrument prints **minutes**, and the section runs seconds
  after `mkfs` — so both read `Aug 10 18:13` whether or not the fix existed,
  and the case could only fail by crossing a minute boundary. It reads the
  superblock's `s_time` at `SB+216` before and after instead, in seconds, which
  is the same relation with two more digits. Before writing a comparison, ask
  what the smallest difference the instrument can show is, and whether the
  effect is bigger than that.

  **It runs the other way too, and that direction is easier to miss.** `who -i`
  compared `who | head -1` — one line — against *every* line of `who -i`, an
  equality only while the host has exactly one login session. It passed for
  months, passes on a runner, and broke the moment a second terminal was open,
  with a diff that reads like `who` printing the user twice. Do not treat "green
  in CI" as evidence the assumption is gone; a runner is a *machine*, with its
  own peculiarities, not a neutral referee.

  **And a THIRD shape, which is not a property of the machine but of what ran
  before it.** `libkmemu` manufactures `/etc/utmp` lazily, when the first reader
  opens it — so after any earlier `who`, `rootfs/etc/utmp` is a real file that
  `cp -a` carries into a copy. A new `tests/jail` case built `who` with
  `Admin/Mk` and compared it against ours; it passed here and failed on a
  runner, and **the runner was right**: the Mk-built `who` has no `libkmemu` at
  all, and what it had been reading was a file an earlier run left behind. A
  fresh runner is the only machine with no history, which makes it the only one
  that can see this. The fix is not to relax the case but to *remove the
  artefact* — the suite now deletes the file first, and asserts the honest
  answer (`who: cannot open /etc/utmp`) instead of the flattering one. Ask of
  any green suite: **would this still pass on a tree that has never been used?**

  **All sixteen suites then existing were swept for this after the third
  instance** — `tests/mkfs` was added afterwards and has never been covered by
  it, which is worth knowing before treating the sweep as complete. **The
  finding worth keeping is where the bugs were: three of five were in blocks
  ALREADY corrected once for this class** — the fix landed on one line and the
  line beside it kept the assumption. `ut_name` compared against `id -un` while
  `ut_line` one line below used `$hostwho`; the mtab terminator probe kept
  offsets 31 and 63 after `FSNMLG` went 32 → 1024; the `$4` cpu-time column kept
  indexing past the nice marker whose shift had been fixed 25 lines above. When
  you correct one of these, re-read the whole block.

  Residual dependencies are known and deliberate, not oversights: `tests/hooks`
  needs `python3` and `jq` (intrinsic — they are what the hooks run),
  `tests/wavec` uses `perl -e 'alarm'` as a deadline, `tests/wavea` assumes
  `NAME_MAX >= 255` on `$TMPDIR`, `tests/jail` assumes the tester is the builder
  and that `/usr/lib/dyld` exists. Each would fail loudly rather than quietly.

- **A case that silently disappears is worse than one that asserts a host
  property, because it never even goes red.** `tests/cpp` was the one suite
  using relative paths without `cd`-ing anywhere, and wrapped its most valuable
  case — authentic V8 source through authentic V8 headers — in
  `if [ -d "$V8INC" ]`. Run from outside the repo root it reported
  `cpp: 12 passed, 0 failed`. Anchor to `$(dirname "$0")`, and make a missing
  input a **failure** rather than a skip.
