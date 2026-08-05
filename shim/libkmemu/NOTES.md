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

## Next

`/etc/mtab` for `df` lands beside `utmp.c` and in the same table in `synth.c`;
`df` then reads a superblock per device, which is where it stops being a file
problem. `load` and `w` want a namelist and `/dev/kmem`, which is the part this
library is actually named for. `ps` needs `libproc` on top, shows the V8 world's
own subtree by default, and prints a sentinel for any column with no honest
source — a fabricated `WCHAN` is the one thing that would make the output a lie.
