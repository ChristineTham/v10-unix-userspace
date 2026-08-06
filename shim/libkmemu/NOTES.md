# libkmemu

The system-facts half of the shim, and the one part of it that may link the
host's libc. `make libkmemu`; `make test-kmemu` (34 tests).

`synth.c` states the boundary and PLAN.md §7 records the decision. In short:
this library may call the documented interfaces that answer what is running and
who is logged in — `getutxent(3)`, `getfsstat(2)`, `proc_listpids`/`proc_pidinfo`,
`sysctl(3)` — and nothing else. Everything in `shim/v8sys/` stays
raw-syscall-only, and so does everything in *this* library that is not one of
those calls. `utmp.c` is the only file where libc actually appears.

The measured symbol table is the whole argument, and `tests/kmemu` asserts it:

```
$ nm -u rootfs/bin/who
_endutxent  _getutxent  _setutxent
```

Three symbols. Every other binary in the rootfs imports nothing at all.

## who needed no changes

Not one line. `who.c` is imported from upstream and compiled as written; the
only thing Phase 4 costs it is `-lkmemu` on its link line.

That is the point of manufacturing `/etc/utmp` rather than giving `who` a
function to call. `who` does `fopen("/etc/utmp")`, and so does `w`; a library
call would have cost one recorded deviation per program, each a small lie about
what the authentic source does. It is also what the real system did — `/etc/utmp`
was an ordinary file kept current by `init` and `login`, both of which live on
the kernel's side of this seam, and the shim *is* that side. The only difference
is that the bookkeeping happens when a reader opens the file instead of when a
session begins.

Mechanically: `v8s_open` calls `kmemu_synth(path, root)` before `vpath()`
resolves the path, because `rootpath()` only redirects a path whose rootfs copy
already exists — the point is to create it so the resolution that follows finds
it.

**The definition is what varies, not the call.** `nokmemu.c` in libv8sys answers
0 to everything, and a program that links libkmemu gets the real one instead.
Not a weak reference, which is the obvious way to write this and does not work:
`extern int f() __attribute__((weak))` does emit an undefined weak symbol — `nm`
confirms it — and ld64 then refuses the link anyway unless something defines
`f`. Measured before this was rewritten.

The record is 24 bytes, not the VAX's 20, because `ut_time` is a `long`. That is
only safe because nothing else in this world reads or writes utmp: there is no
1985 file to stay compatible with, only the two ends of this seam. `tests/kmemu`
asserts the size from the V8 side, compiled against the authentic header.

Two documented losses, both of the same kind as `dir.c` truncating a name to 14
characters: a login or tty name longer than 8 is cut, and `ut_host` is dropped
because V7's record has nowhere to put it, so a remote login reads as a local
one on the same tty.

## What the boundary test found, which was the actual yield

`tests/kmemu` sweeps every Mach-O binary in the rootfs for libc imports. Writing
that check turned up five functions that had been resolving out of libSystem
unnoticed — CLAUDE.md's "a missing libc function does not fail the link" class,
five more times. None of them made anything look broken.

| Symbol | What it meant | Fix |
|---|---|---|
| `getgrent` etc. | **`ls -g` was reading the Mac's group database from inside the jail** | build V8's `getgrent.c` |
| `ftime` | a syscall the shim had simply never implemented | `v8s_ftime` + `tz.c` |
| `tolower`, `toupper` | V8 has both in C; never built | build them |
| `atof` | 319 lines of VAX assembly, so never ported | `src/libc/gen/atof.c`, new code |

`tests/freestanding` could not have caught any of them: it links its own small
programs, so it only ever proved the *shim* was clean, never the world built on
it. That gap is worth remembering — a guard on the seam is not a guard on what
crosses it.

`getgrent` is the one that mattered most. The output looked entirely plausible,
because a list of group names is a list of group names.

## A truncated path is not a shortened name

`/etc/mtab`'s fields were V7's 32 bytes, and this file used to truncate a long
mount point into one and call it "the same loss as dir.c's 14-character names".
That comparison was wrong, and it is worth keeping as an example of a comment
that documented a bug into invisibility.

A truncated **name** is a wrong name and still just a name — every reader treats
it as an opaque string. A truncated **path** stops resolving, and `df`'s
`dfree()` branches on `stat(file)` succeeding: when it fails, it takes the arm
that assumes the string is a device name. So the two CoreSimulator volumes on
this machine printed as rows with an empty `dir` column and the first nine
characters of the mount point sitting in the `dev` column, which reads as
corruption rather than as truncation.

Widened to 128 in `<fstab.h>` — `src/include/PORTING.md` has the account and
names all four places that spell the number. Anything that still does not fit is
**reported and dropped**, the way `/proc` reports a process-table overflow: an
entry whose path cannot be stored cannot be described truthfully. Both mtab and
fstab apply the same rule, because `df`'s `devlen()` merges any fstab entry
whose device is not already in mtab — drop it from one file only and the merge
hands it straight back.

The two files had in fact already disagreed about those mounts **by one
character**: `field0()` copies `dlen-1` and terminates, `puts0()` copies up to
`max` and does not, so mtab held 31 characters where fstab held 32. Nothing
noticed, because `devlen()` matches on the device rather than the path.

## The timezone, and a measurement that lied

`ftime(2)` is syscall 35 in V8 and the shim did not have it. `tz.c` implements
it by reading `/etc/localtime`.

Reading a TZif file is allowed where reading `/var/run/utmpx` is not, and it is
the same distinction PLAN.md §7 draws: TZif is a published, versioned,
byte-for-byte specified format (RFC 8536) that every Unix reads with its own
code, while utmpx's on-disk layout is private, undocumented, and does not match
its own documented struct. One is an interface; the other is a guess that
happens to work.

The obvious shortcut was `gettimeofday`'s second argument, which is a
`struct timezone *` and looks exactly like the answer. It is not: the **raw
syscall writes something else there**. Passing it a zeroed struct came back with
775410594 minutes west and `date` printed a day in the following week. libc's
`gettimeofday` *does* return −600, which is what made this look verified when it
was not — the same shape as running `inv(1)` with no stdin. The number was real;
it was not a measurement of the path being used.

`dstflag` is always 0, deliberately. The offset is the one in force now with
daylight saving already applied, so setting the flag would make V8's `localtime`
add another hour — using the US 1974-75 rule, `last Sunday in April`, with those
two years special-cased in a table.

The offset is cached, which is authentic rather than lazy: the VAX read one
kernel variable, so a program running across a DST change kept the offset it
started with. The same fact makes one offset the whole answer — `ftime` takes no
timestamp, so it cannot say what the offset was in another month, and `ls -l` on
a file from the other side of a change shows it an hour out. Exactly as the VAX
did, for the same reason.

## The debt this left, and how it came off — signals

V8's `libc/gen/sleep.c` was imported alongside the others and taken back out,
because it hung. It is `alarm` + a handler + `for(;;) pause()`, and

**`v8s_signal` installed every handler with a null trampoline.** It handed the
raw `sigaction` syscall a userland `struct sigaction`, and the kernel wants
`struct __sigaction`:

```
userland struct sigaction:   size=16  handler@0            mask@8   flags@12
kernel   struct __sigaction: size=24  handler@0  tramp@8   mask@16  flags@20
```

So the kernel read `sa_mask` as the signal-trampoline pointer, and `sa_flags`
and `sa_mask` from the wrong offsets. `sigaction` returned 0 and nothing looked
wrong until a signal was delivered, at which point the process hung or died.
Measured: `kill(getpid(), SIGINT)` with a handler installed did not return.
`sigsuspend` and `setitimer` were never implicated — both worked with the same
raw wrappers when the host's `signal()` installed the handler.

That is fixed: `shim/v8sys/sigtramp.s` supplies the trampoline the kernel enters
and `signal.c` passes the struct the syscall actually takes. `shim/NOTES.md` has
the account, including `SA_NODEFER` — the flag without which the fix would have
worked exactly once per program, because a handler that longjmps out never
reaches `sigreturn` to unblock its own signal. `v8s_alarm` now reports the
seconds remaining on the previous alarm, which is what `sleep.c` saves and
restores.

**`sleep` came off the allowed-leak list, and the list is what said so.**
`tests/kmemu` fails when an entry goes stale, so once V8's own `sleep.c` built
and nothing imported the host's any more, the suite demanded the entry's
deletion rather than waiting for someone to remember. That is the half of a
tolerated exception that usually goes unwritten.

The wider lesson is the same one this file already tells about
`tests/freestanding`. `tests/v8sys` covered signal *numbering* and never
delivery, so a shim in which no handler could ever run passed 44 of 44. A guard
on a seam is not a guard on what crosses it — and it is not a guard on the half
of the seam nobody wrote a case for either.

## `/proc`, and three host facts that would each have lied plausibly

`procfs.c` is the second filesystem type in the shim's switch, and the first
thing in this library that is not a manufactured *file* but a manufactured
*filesystem*. The bargain is the one `utmp.c` already struck, one level up:
`who` reads an `/etc/utmp` nothing else writes; `ps` reads a `/proc` nothing
else mounts. `PLAN.md` §8a step 3 has why `proca.c` is not imported.

`PIOCGETPR` is the interesting half, because **`prioctl` copies the kernel's own
`struct proc` out verbatim** (`iomove((char *)p, sizeof(struct proc), B_READ)`,
proca.c:323). There is no marshalling step, so the struct's shape *is* the ABI
and a field in the wrong place produces plausible numbers rather than an error.
Guarded twice: `_Static_assert` on the clang side, and the same offsets measured
from the V8 side in `tests/kmemu`, because a static assertion only ever sees one
compiler.

Three host facts had to be measured rather than read off, and each one has the
same failure shape — an answer in the right range that is wrong:

**`pti_total_user` is in mach ticks, not nanoseconds.** `<sys/proc_info.h>` says
only "total time". Measured against `CLOCK_PROCESS_CPUTIME_ID`: a 0.3096 s burn
reported 7560618, which is 0.0076 s read as nanoseconds and 0.3150 s read as
ticks. **On Intel the timebase is 1/1 and the two readings coincide exactly** —
so this is a bug an x86 CI runner cannot see and an Apple Silicon one is wrong
by 41.67×, which is the same reason `ci.yml` pins `macos-14`. The rate comes
from `hw.tbfrequency` (sysctl, already sanctioned) rather than
`mach_timebase_info()`, which would have been a second way to ask.

**The stat codes disagree on every value but one.** macOS
`SIDL/SRUN/SSLEEP/SSTOP/SZOMB` are 1..5; V8's are 4/3/1/6/5. Both are small
integers in the same range, so a straight copy compiles, runs, stays in bounds
and prints the wrong letter for every process — a running one would read `w`.
Hence a table rather than an assignment.

**`p_pid` was a `short`.** Not a host fact so much as a 1985 one the host
exceeds: V7 wrapped pids at 30000, macOS runs to 99998, and the truncation is
signed — 44145 came back as −21391. See `src/include/PORTING.md`; the reason it
survived initial testing is that a freshly booted host has low pids.

What is left zero is left zero on purpose, and `procfs.c` names the reader for
each: `p_wchan` (no documented source — a fabricated one is the single thing
that would make `ps -l` a lie), `p_clktim`, `p_textp`, `p_swaddr`.

## The u-area, and the read that has to fail

`ps` gets the other half with `Sread(fd, UBASE, up)` — a seek to a *virtual
address*, because `/proc/<pid>` is the process's address space and the u-area
sits at its top. So this is a region of the one file rather than a second
format, and `pr_read` serves `[UBASE, UBASE+4016)` and reports end of file
everywhere else.

**That EOF is load-bearing.** `getargs` reads the process's stack image to
recover `argv`; there is no stack image here, so the read must come up short,
and `getargs` then prints `"(u_comm)"` — which is exactly what V8 shows for a
swapped-out process. Getting there needs `u_ssize` non-zero: with zero,
`ctob(0)` is a zero-length read, which *succeeds*, and `getargs` scans backwards
from `stack+0`, reading `stack[-1]` before its own guard can stop it. So
`u_ssize` is `NSTACK`'s worth — a behavioural choice, spelled as one in the
source, not a measurement.

`u_ttyino` is left zero and that is a `/dev` question, not a `/proc` one:
`gettty()` looks the number up in the directory records of `/dev`, `/dev/dk` and
`/dev/pt`, and inside the jail `/dev` holds one entry, `kmem`. Filling it is a
stat of `/dev/ttys<minor>` folded through `v8sys_fold_ino` — `e_tdev`'s minor
does map to the name, measured — and buys nothing until those nodes exist.

The boundary is where this shape of code breaks, so it is tested from both
sides: one byte below `UBASE` must read as EOF rather than indexing the buffer
*negatively*, the last byte of the u-area must be readable, and the one after it
must not. That gap was found by a mutation that widened the window eight bytes
and which nothing failed on.

## Next

`ps` itself: nine files, an LP64 audit — `getargs.c` walks the stack with
`*(long *)(sp -= 4)`, stepping four bytes and reading eight — and `/dev/dk`,
`/dev/pt` and `/dev/drum` have to exist or it `error()`s before it starts
(`ps.c:21-28`).
