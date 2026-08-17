# libv8sys

The layer standing in for the VAX kernel. `make libv8sys`; `make test-v8sys`
(53 tests) and `make test-freestanding` (8) cover it.

`shim/libkmemu/` is a **separate** library with its own notes and its own rules —
it is the one part of the shim that may link host libc, and keeping it out of
`libv8sys.a` is what stops every V8 binary from importing libc. See
`libkmemu/NOTES.md`.

## Signal delivery, and the trampoline it needs

Until this landed, **no V8 program in this port could catch a signal**.
`v8s_signal` handed the raw `sigaction` syscall a userland `struct sigaction`,
and the kernel wants `struct __sigaction`:

```
userland struct sigaction:   size=16  handler@0            mask@8   flags@12
kernel   struct __sigaction: size=24  handler@0  tramp@8   mask@16  flags@20
```

So the kernel read the zeroed `sa_mask` as `sa_tramp` and took `sa_flags` and
`sa_mask` from past the end of the struct. Every handler was installed with a
**null trampoline**; `sigaction` returned 0 and nothing looked wrong until
delivery, when the process hung or died. Found in Phase 4 by building V8's
`sleep(3)` — `alarm` + a handler + `for(;;) pause()` — which simply hung.

**The kernel does not jump to a handler.** It jumps to a trampoline the process
supplied in `sa_tramp`, passing the handler along as an argument; the trampoline
calls the handler and then asks the kernel to restore the interrupted context
with `sigreturn(2)`. That last step is userland's, which is why a process with
no trampoline has nowhere to be entered. libc's `sigaction()` exists largely to
fill this in with its own `_sigtramp`, and a shim that goes straight to the
syscall has to supply one itself.

`shim/v8sys/sigtramp.s` is three instructions, and that is the design rather
than luck. XNU enters the trampoline with `x0` = handler, `x1` = infostyle,
`x2` = signal, `x3` = `siginfo_t *`, `x4` = `ucontext_t *`, `x5` = the
sigreturn token — which is already AAPCS64 argument order, so `v8sys_sigcall()`
in `signal.c` declares its parameters in the kernel's order and the register
shuffle disappears. Everything needing judgement is C: mapping the number back
into V8's numbering, calling the handler, issuing `sigreturn`.

Three flags and the absence of a fourth make it V7's `signal(2)`:

* **no `SA_RESTART`** — V8 programs expect a slow read to fail with `EINTR`.
  **This is true of `signal(2)` and NOT of the reliable interface**, which the
  first version of this line did not say. V8 decides on one flag: `ssig()` sets
  `SNUSIG` when the action is `SIGISDEFER(f)` (`sys4.c:318-320`), and `read()`
  restarts for such a process when nothing was transferred (`sys2.c:55`,
  `trap.c:184`). `v8s_sigsys` therefore sets `SA_RESTART` on its deferred arm,
  and `tests/v8sys` asserts the two as a pair — one case per arm, because a
  blanket rule in either direction passes half of them;
* **`SA_RESETHAND`** — reset-on-delivery, which V8 code re-arms inside the
  handler;
* **`SA_NODEFER`**, which is the subtle one. `sigaction` blocks the signal for
  the duration of the handler and `sigreturn` unblocks it — so a handler that
  **longjmps out never unblocks it**. V8's `sleep(3)` longjmps out of its
  SIGALRM handler and `sh` out of its SIGINT handler, and our `setjmp.s` saves
  registers only, since the VAX original had no mask to save. Without
  `SA_NODEFER` the first `sleep()` works and every later one hangs in `pause()`,
  which is the same bug wearing a better disguise. V7 had no signal mask at all,
  and V8's own header agrees from the other side: it spells deferral as an
  *opt-in*, `DEFERSIG(handler)` setting the low bit of the handler address.
  Nothing in this tree uses it and it is not implemented.

The old action comes back in the **smaller** struct. That asymmetry looks like a
bug and is the kernel's interface: `__sigaction` copies the new action in as
`struct __sigaction` and the old one out as `struct sigaction`, which has no
`sa_tramp` to report.

`v8s_alarm` was fixed alongside, because `sleep(3)` needs it: it returned 0
unconditionally, having passed `setitimer`'s old-value argument as 0, so every
`sleep()` silently cancelled its caller's pending alarm. Reading that value back
is where Darwin's `struct timeval` being `{ long tv_sec; int tv_usec; }` starts
to matter — writing one as two longs is harmless, since the extra zeroes land in
padding the kernel ignores, which is why the shape survived everywhere else in
`syscall.c`.

**What let this live so long is worth more than the fix.** `tests/v8sys` covered
signal *numbering* and never delivery, so a shim in which no handler could ever
run passed 44 of 44. It now forks a child with a deadline for each delivery
case — the failure mode is a hang, not a wrong answer — and
`tests/freestanding` catches a signal in a program *v8cc compiled*, because the
suite that proves the seam is clean is never the suite that proves the world
built on it is.

## How it reaches the kernel, and why that mattered

The V8 world calls `write()`, and the shim implements `write()` **by doing what
`write()` does**. Something has to give one program both.

Two approaches were tried and both failed, in the same way:

* **Defining `write` in C.** The shim's own `write(fd, b, n)` then binds to our
  definition and recurses.
* **Linker aliasing** (`-Wl,-alias,_v8s_write,_write`). Links cleanly, looks
  right, dies with `EXC_BAD_ACCESS` in `v8s_write+4` — a stack-overflow fault in
  a function prologue. The alias is global, so it captures the shim's own call
  too. There is only one `_write` at link time and no way to say "this one, but
  not from in here".

So the shim **names no libc function at all**. `shim/v8sys/rawsys.h` goes
straight to the kernel: number in `x16`, `svc #0x80`, carry flag signals failure
on macOS; number in `x8`, `svc #0`, negative return on Linux. Every wrapper
returns a negated errno on failure so callers check `< 0` on both.

That is not a workaround — it is what a libc replacement should do, and it has a
better property than either failed approach: a V8 program links `-nostdlib` and
imports **nothing**. `tests/freestanding/run.sh` asserts that with `nm -u`, so
if anything in the shim starts calling libc again, the suite says so before the
recursion comes back.

Consequences worth knowing:

* `errno` is never read from the host. Nothing sets it; the error arrives in the
  return value. Files that used to say `errno = ENOMEM` now set `v8_errno`.
* `dir.c` walks `getdirentries64` directly rather than `fdopendir`/`readdir`.
* `mem.c` has a small bump allocator (`v8sys_alloc`) because it can call neither
  the host's `malloc` nor V8's — the latter lives above the seam and grows the
  arena `mem.c` manages.
* The shim is built with `-fno-stack-protector -fno-stack-check`; both emit
  calls to libc helpers (`___stack_chk_fail`, `___chkstk_darwin`).
* `libSystem.dylib` is still on the link line, because macOS refuses to build a
  dynamic executable without it. No symbol is taken from it.

## What the two test suites each prove

`tests/v8sys/test.c` links the shim against host libc and calls the `v8s_` names
directly. It checks the *semantics* of the seam — V7 records, errno mapping,
signal numbering, the sbrk arena.

`tests/freestanding/run.sh` compiles with v8cc and links `-nostdlib`. It checks
the *linkage* — that a V8 program can actually be built this way. The
distinction matters: 44/44 passing on the first suite said nothing about the
second, and the aliasing bug lived entirely in the gap between them.

## exit() and _cleanup

`stubs.c`'s `exit()` calls `_cleanup()` weakly before the syscall, which is what
V8's own `libc/sys/exit.s` does. Weakly, because freestanding programs link no
stdio at all: an undefined weak symbol resolves to zero, is skipped, and pulls
nothing in. Without it, every program that printed anything exited silently with
its output still in the buffer.

## The clang seam cuts both ways

The shim is clang-compiled and the V8 world is not, so the ABI contract between
them is AAPCS64 rather than V8's own conventions. That is a seam with rules, and
breaking them is invisible until real code runs:

* **A syscall returning `int` sets only `w0`.** The top half of `x0` is
  unspecified. v8cc now sign-extends after every call to a narrower return type
  (see `arm64_widen` in `compiler/ccom-arm64/gencode.c`); before that, `open()`
  returning -1 tested as positive and *every* `< 0` error check in the V8 world
  silently failed.
* **The other direction is safe by construction.** A V8 function spills `x0`-`x7`
  whole and reloads each parameter at its declared width with `ldrsw`/`ldrsb`,
  so garbage in the high half of an incoming argument register is never read.
  This was checked in the emitted code, for both plain and `register` parameters,
  rather than assumed — signal handlers are called this way.

---

# libv8c (V8's libc), current state

`make libv8c` builds `libv8c.a` **with v8cc** from authentic V8 sources:
malloc, ecvt/fcvt, the seven portable string routines (V8's own `.C` reference
implementations, which is what a machine without the VAX string instructions
was meant to use), and 20 stdio files.

Two files replace VAX assembly and are new code, each saying so at the top:

* `src/libc/stdio/doprnt.c` for `doprnt.S` (765 lines of VAX assembly using
  decimal-string instructions and a `locc` translate table). Walks the argument
  block with an 8-byte stride, which is exactly what v8cc's arg-spilling
  prologue guarantees — `printf(fmt, args) { _doprnt(fmt, &args, stdout); }`
  compiles and runs unmodified because of it.
* `src/libc/gen/ieeefp.c` for `modf`, `frexp`, `ldexp`, `fabs`. Verified
  against V8's own `ecvt.c`, which is portable C and needed no changes beyond
  a `static` on a forward declaration that K&R allowed to disagree with the
  definition.

One source change, in `ieeefp.c` itself: it was first written in C99 and V8's
compiler rejected it. No `long long` (the type did not exist in 1985), no `ULL`
suffixes. Under LP64 a plain `long` is already the 64 bits an IEEE double needs.

## What stdio cost, and what it bought

`fputs` was the first real libc function compiled by this back end, and it died
immediately with `EXC_BAD_ACCESS` inside the `putc` macro. It turned out to be
the callee-saved registers being pushed at `[x29,#-16]` downward — exactly where
`oalloc()` puts automatics under `BACKAUTO` — so every save landed on a local.
The bug needed a function with both `register` variables and automatics, which
is why 62 synthetic tests missed it and the first real one found it in seconds.

That set the pattern for everything since. The suites now lead with authentic V8
sources for the same reason: the back end is correct on every feature in
isolation and has only ever been wrong about combinations.

`sys_errlist`/`sys_nerr` are ported from V8's own `errlst.c`. The `ed` script in
V8's libc Makefile that appears to build them is a VAX trick for moving the
table into read-only text, not part of the interface.

## The jail could be read but not written, and that was invisible

Found while surveying `syscall.c` for the S8a step-2 switch, and closed before
it.

`rootpath()` decides by asking whether the rootfs has the path. That is exactly
right for a reader and it is **unanswerable for a name that does not exist
yet** -- so nothing being created could be resolved, and creation escaped the
jail:

```
creat("/etc/x")        ->  the MAC's /etc, refused with EACCES
open("/etc/x", 0)      ->  the JAIL's /etc, works
```

The same name meaning two different worlds depending on which syscall asked --
the same shape as the `/etc` versus `/etc/` bug already recorded in
`rootpath()`'s own comment, and the reason it survived is that on macOS every
V8 directory is root-owned, so it always failed, and it failed with **EACCES**.
That reads as a permissions problem, not as a missing jail. On a host directory
that happened to be writable it would not have failed at all -- it would have
written outside, quietly.

Three defects, one family:

- `v8s_creat` resolved **nothing**. Not just new files: `creat` on an existing
  jailed path reached for the Mac's copy too.
- `v8s_link` resolved **neither** name, so `ln /bin/cat x` linked the Mac's
  `/bin/cat` -- a file the V8 world cannot see with `open(2)`.
- `v8s_mkdir` resolved nothing, and could not have benefited if it did.

The fix is a second resolution mode. `V8P_LOOK` is today's rule and every
reader keeps it, so the union is unchanged: a path the rootfs lacks still falls
through to the host. `V8P_MAKE` keys on the **parent** -- if `$V8ROOT/etc`
exists then `/etc/newfile` resolves inside the jail whether or not it is there
yet. `mkpath()` tries LOOK first so an existing rootfs file still wins, and is
used by `open(O_CREAT)`, `creat`, `mkdir`, `mknod`, and the *new* name of `link`
and `symlink`.

Two details worth keeping:

- **`symlink`'s first argument is not resolved.** It is link text, stored
  verbatim and interpreted later, inside the jail. Resolving it would bake this
  machine's rootfs path into the link.
- **`link` holds two resolutions at once**, which is the aliasing trap CLAUDE.md
  names: `rootpath()` returns a pointer into its own static buffer, so the
  second call overwrites the first and you get `link(new, new)`. The existing
  name is copied out before the new one is resolved.

`tests/jail` gained eight cases, and they check *where the file landed* rather
than that the call succeeded -- both would pass on a host that let the write
through, which is the outcome being ruled out. One case reads a path only the
Mac has, which is what fails if the parent rule is ever applied to readers too.

## A null path belongs to the kernel, and two places looked at it first

`rootpath()` hands a null path straight back — deliberately, because `unlink(0)`
is owed `EFAULT` from the kernel and not a decision from us. Two functions broke
that rule by *inspecting* the string before the syscall could run:

- **`dotlink()`**, which decides whether a name ends in `.` or `..`, opened
  `for (p = base = b; *p; p++)`.
- **`v8s_link()`**, whose `q = vpath(a)` copy loop dereferences the result, one
  function away and reached by nothing at the time.

**Found through `yacc`, not by writing a test.** `yacc -o` with `-o` last leaves
the output file name null; `openup()` cannot create it, and `error()` runs
`cleantmp()` — whose two `ZAPFILE`s are `unlink()` of temp names `setup()` had
not yet assigned. So the crash was in the shim and read as a crash in yacc, which
is the same misattribution `v8s_signal`'s trampoline produced.

`v8s_link` is the `v8s_mknod` lesson again: a rule that nothing exercises cannot
be seen to be incomplete. `tests/v8sys` now calls both with nulls — reaching the
call *is* the assertion, since a regression takes the whole suite down on
SIGSEGV — and `tests/wavea` keeps the program that found it.

Part of the whole-tree address-0 sweep; PLAN.md §4i.

## /dev/tty is not a device, and this port spent a survey looking for its driver

`PLAN.md` §8a step 1b costed three candidates for "the first stream driver",
and picked a host-fd driver to put underneath `/dev/tty`. The premise was
wrong. **V8's controlling terminal is not a stream, is not a driver, and has no
code behind it at all.** `/dev/tty` is a hard link to `/dev/fd/3`, and opening
anything in `/dev/fd` is `dup(2)`.

Bell Labs wrote it down, in `usr/man/man4/fd.4`:

> If file descriptor *n* is open, these two system calls have the same effect:
> `fd = open("/dev/fd/n", mode);` `fd = dup(n);` *Creat(2)* is equivalent to
> *open,* and *mode* is ignored. As with *dup,* subsequent IO on *fd* fails
> unless the original file descriptor allows the read or write operation.
> … Entry `/dev/fd/3` is conventionally the `control terminal' … *Open*
> returns −1 if the related file descriptor is not open.

and the kernel agrees four times over, each checked in `third_party/`:

| | |
|---|---|
| `proto-dev:91` | `tty` is major 40 minor 3, **link count 2** — the other link is `fd/3`. `stdin`/`stdout`/`stderr` are 40,0–2, also 2. Every other fd node is 1. |
| `conf/devices:55` | `device 40  std`, and `int stdio_no = 40` on the next line. No driver name, no `stream-device` keyword. |
| `dev/conf.c:565` | major 40's `cdevsw` row is `nodev, nodev, nodev, nodev, nodev, nulldev, NULL`. Every slot, and a null `streamtab`. There is nothing to call. |
| `sys/sys2.c:174` | `open1()` special-cases it **before the permission check**: `getf(minor)`, `ufalloc()`, `u_ofile[i] = fp`, `fp->f_count++` — the body of `dup(2)`, written out. |

V7's answer *was* a driver — `syopen`, redirecting through `u.u_ttyp`. That file
is still in the V8 tree, as `sys/sys/sys.c`, and it is **dead**: absent from
`conf/files`, pointed at by nothing in `conf.c`, and unable to compile anyway
because `u_ttyp` and `u_ttyd` are not in V8's `struct user`. Killian replaced a
driver with a filesystem convention, and the vestige outlived it.

**And `conf/devices:82` — cited in the survey as `ttyld` — is `bf`.** `ttyld` is
`:75`. Both errors are the same shape as the ones this repo keeps recording:
something read once, written down, and then built on. Re-measured here.

### Why fd 3, and who has to arrange it

`cmd/init.c:368-382`:

```c
	while (open(tty, 2) != 0) sleep(10);	/* the terminal, as fd 0 */
	ioctl(0, TIOCSPGRP, (char *)0);
	while (ioctl(0, FIOPOPLD, (char *)0) >= 0) ;
	ioctl(0, FIOPUSHLD, &tty_ld);		/* ttyld, line discipline 0 */
	dup(0); dup(0); dup(0);			/* -> 1, 2 and 3 */
```

Three dups. "Controlling terminal" is not a kernel fact in V8; it is the
userspace convention that fd 3 is one, and `init` is what establishes it. So the
`v8` launcher is this world's init and does `exec 3<>/dev/tty` for the same
reason — with no fallback, because when there is no controlling terminal (a
pipeline, cron, CI) `fd.4`'s answer is that open returns −1.

### What the type implements, and what it deliberately does not

`v8fs_fdfs` in `vfs.c` owns three operations — `t_path` (identity: there is no
host path), `t_open` (parse the minor, `dup`) and `t_stat`. Everything after
open is the passthrough type's, *unchanged rather than merely equivalent*,
because a dup'd descriptor **is** an ordinary host descriptor; giving it a type
of its own would invent a difference the kernel does not have. That is also why
`fd_open` never calls `v8fs_bind()`.

`stat` and `fstat` therefore disagree, and that is V8 rather than a defect:
`stat` reads an inode in `/dev`, which is a character device whatever the
descriptor turns out to point at, while `fstat` follows the open file to the
real object. `test -c /dev/tty` is true with a plain file on fd 3.

### macOS has a /dev/fd too, and the first draft of the comment was wrong about it

Measured, not reasoned. Darwin's is a **dup as well**, so the shared file offset
— the classic `fdescfs` difference, and the thing I wrote down first — is *not*
a difference here: both continue where the other left off. Four real ones:

| | V8 (this type) | macOS |
|---|---|---|
| `open("/dev/fd/3", O_WRONLY)` on a read-only fd | succeeds; the **write** fails | `EACCES` **at open** |
| `open("/dev/fd/999")` | `ENOENT` — no such node | `EBADF` |
| `stat("/dev/fd/1")` | `crw-rw-rw-`, rdev `makedev(40,1)` | the underlying object |
| `/dev/tty` | fd 3 | the controlling terminal |

Delegating to the host would have imported all four. The `ENOENT`/`EBADF` split
is the subtle one: V8 shipped nodes `0`–`127` and `NOFILE` is 128, so the node
set **is** the file table — a name past the end never reaches `open1`, so it is
`namei`'s error and not `getf`'s.

### Two gaps it made live

Neither was reachable before, and that is the whole reason neither was found.

- **`v8s_creat` bypassed the filesystem switch.** It was
  `RET(rawsys3(SYS_open, (long)mkpath(path), ...))` — path resolution without
  dispatch, correct for passthrough and structurally unable to reach a second
  type. `/proc` is read-only, so nothing noticed. `fd.4`'s "creat is equivalent
  to open" is what makes it matter: `creat("/dev/tty")` must dup fd 3, and
  before the fix it truncated `rootfs/dev/tty` and returned a descriptor on an
  empty file. The `v8s_mknod` lesson again.
- **`v8s_dup` and `v8s_dup2` dropped the descriptor's type.** `v8fs_fdtype()` is
  how every read, write and ioctl finds its filesystem, and an unbound
  descriptor reads as passthrough — so `dup()` of an open `/proc` file returned
  one whose reads went to the host, silently, with the right-looking number.
  Nothing in the tree dup'd one. `/dev/fd` is nothing *but* dup.

### A guard I wrote was vacuous, and only the mutation said so

`v8s_dup2` had a `v8fs_unbind()` in front of its `v8fs_bind()`, with a test
named for it. `v8fs_bind` already stores null for the passthrough type, so the
unbind was **dead code** — and the mutation that deleted it changed no test.
Worse, the comment beside it *acknowledged* the two calls collapse and kept
both. The call is gone; the case now names the property that is actually
load-bearing, and the mutation that proves it is a `v8fs_bind` that skips the
passthrough case instead.

### The three consumers, and an address-0 case that is faithful rather than fixable

`/dev/tty` has exactly three readers in this tree, and making it mean what V8
means changed what all three do when fd 3 is closed — which, outside the
launcher, is most of the time.

| | the call | with no fd 3 |
|---|---|---|
| `dump/dumpoptr.c:36` | `fopen`, **checked** | `msg("fopen on /dev/tty fails")` then `abort()` |
| `troff/hc.c:766` | `fopen`, then `setbuf(rcf, NULL)` | writes through a null `FILE *` |
| `pr.c:201` | `fopen`, then `getc(Ttyin)` at `:259` | reads through a null `FILE *` |

The last two are the address-0 class, and **there is no VAX answer to restore**,
which is the `ls.c:285` precedent and means §1 says leave them alone. Computed
rather than assumed, from the sixteen crt0 bytes at virtual 0
(`00 00 c2 08 5e d0 ae 08 6e 9e ae 0c 50 d0 50 ae`, identical in `cat`, `ls` and
`lex`) read through the VAX `struct _iobuf`:

```
_cnt = 0x08c20000   _ptr = 0x08aed05e   _base = 0x0cae9e6e   _flag = 0xd050
```

`_ptr` and `_flag` reproduce the two values PLAN.md §4i and §4j already
recorded, which is what says the layout is being read right. And then the
decisive part: `getc(p)` is `(--(p)->_cnt >= 0 ? ... )`, so it **writes** to
virtual address 0 — and V8's binaries are ZMAGIC (`0413`, `a.out.h:17`), whose
text is read-only shared. A VAX takes a protection fault. `setbuf` writes
`_base` and `_flag` at 0 and 12 for the same result.

So `pr -p` and `troff`'s hyphenation prompt die here exactly as they died on a
VAX with fd 3 closed, and the port has become *more* faithful rather than less:
what used to happen was `fopen` falling through to the **host's** `/dev/tty`,
which is a different device and succeeded whenever a controlling terminal
existed. That accident is what the launcher's `exec 3<>/dev/tty` now replaces
honestly.

**The crash probe structurally cannot see this**, and that is worth knowing
before treating its zero as coverage: the branch needs `Ttyout`, i.e. stdout a
terminal, and the probe gives every invocation `/dev/null` on all three
descriptors. Nor is it a mutation-testable property — "a VAX would also fault"
has no runtime witness. What is testable is the shim's half, and
`tests/libv8c` now pins it at the level the three programs actually call:
`fopen("/dev/tty")` is NULL with fd 3 closed and a real stream with it open,
with the program arranging its own fd 3 rather than inheriting the harness's.

### One limit `/dev/fd` makes spellable: a dup of a directory descriptor

`dir.c`'s V7 snapshot is keyed by **descriptor**, so a `dup` produces a bare
host directory fd with no snapshot behind it — and macOS refuses `read(2)` on
one. Measured: the original reads a 256-byte record, the dup returns −1. V8,
sharing the file struct, would have continued the scan at the shared offset.

It is a limit rather than a defect to fix here, and it predates `/dev/fd` — but
`/dev/fd` is what makes it **spellable**, since `open("/dev/fd/N")` reaches it
without anyone writing `dup()`. It fails loudly, which is the tolerable
direction. `tests/v8sys` asserts the honest behaviour so that a future `dir.c`
which refcounts a shared snapshot turns the case red instead of quietly
changing something nobody had examined — the `v8s_mknod` rule, applied before
the fact for once rather than after.

The neighbouring case *is* handled: `v8s_dup2` closes the target's snapshot when
it dup2s over a directory descriptor, because the host closes that descriptor
and `dir.c` would otherwise keep serving records for a file that is now
something else.

## /dev/null: the fifth type, and why making the name authentic broke the object

V8 shipped `/dev/null` — `proto-dev:25` is `crw-rw-rw- 1 root man 3, 2 ... null`
— and this port had not, so `ls /dev` listed a machine that never existed. The
crash probe's containment check found it the way it finds everything: as a path
that appeared during a sweep, because a jailed `creat("/dev/null")` resolves
against its **parent** and `$V8ROOT/dev` exists. The obvious fix was to build
the node, and the obvious fix was half right.

**Before the node existed, `/dev/null` worked.** With nothing in the rootfs the
union rule found nothing and the path fell through to the host's device: reads
were EOF, writes were discarded. What the check had actually caught was a
program *poisoning* that fall-through for everything after it. Building the node
did not stop that happening — it did it once, at build time — so the breakage
moved from **after the first write** to **always**, and the detector went blind,
because a path already in the "before" list can never be reported as new.

Measured at the prompt, four faults:

| | the jail said | V8 says |
|---|---|---|
| `echo x > /dev/null` | 14 bytes in the file | discarded |
| `echo y >> /dev/null` | 21 bytes | discarded |
| `cat /dev/null` | prints the litter | nothing |
| `test -c` / `test -f` | false / true | true / false |
| `ls -l /dev/null` | `-rw-r--r-- … 21` | `crw-rw-rw- … 3, 2` |

The accumulation is the one the task named and the least interesting. **The read
is the sharp one**: `prog < /dev/null` is how a program is handed empty input,
and it was being handed whatever last wrote — the crash probe's own founding
lesson, programs reading each other's litter, inside the one device whose whole
job is to have nothing in it.

### Two operations, and only one of them is about /dev/null

`t_path` returns the name unresolved, so every inherited operation reaches the
**host's** device and the rootfs node is never the object. It stays, because the
name has to be real; it is simply not what anything opens. That is the same
arrangement the four `/dev/fd` rows already use, reached one step later.

Nothing else is implemented, and that is `fd_open`'s rule rather than economy.
Bell Labs' null is two arms of the memory driver — `dev/mem.c:68` is
`case 2: return;`, transferring nothing, which *is* end of file, and
`dev/mem.c:156` is `u.u_offset += u.u_count; u.u_count = 0;`, consuming
everything and keeping none of it. Darwin's device is exactly that. Read and
write slots of our own would invent a difference the kernel does not have.

`t_stat` overrides **one field**, and the reason is not about this device at all.
The two machines agree completely about what it is — 3,2, mode 0666 — and
disagree about how a major and a minor are *packed*: Darwin's `makedev` is
`(major << 24) | minor`, V8's is `(major << 8) | minor`, and `stat_translate`
narrows with `& 0xffff`. **A mask cannot preserve a field at bit 24 at any
destination width**, so the major is not truncated, it is deleted. Measured on
the nodes that still fall through:

| node | truth | the jail reports |
|---|---|---|
| `/dev/zero` | 3, 3 | `0, 3` |
| `/dev/random` | 17, 0 | `0, 0` |

For `/dev/null` that would be major 0, which `conf/devices` gives to `console`.
Not a failure — a plausible wrong answer.

**The general defect is deliberately not fixed, on a measurement.** With this row
in place, every node V8 *actually shipped* reports V8's numbers: `/dev/tty`,
`/dev/std{in,out,err}` and the 128 `/dev/fd` nodes from `fd_stat`, `/dev/null`
from here, `/dev/kmem` and `/unix` from libkmemu as ordinary files. What is left
mis-encoded is exactly the set of host nodes V8 never had, where there is no 1985
answer to restore and the host's numbers narrowed are the honest report — and a
correct re-encoding would need a rule for a major above 255, which is
`v8_foldid`'s problem rather than this one.

### The mutation that did not fire, and what it found in the test

Four mutations. The one that reintroduces the bug — `t_path` resolving through
the union again — fired two cases and **not** the case written for it, "13 bytes
written through the jail did not reach the node". Neither recorded cause applied:
the code was live and the assertion was a relation rather than a value, which is
this project's own standing prescription against host-property cases.

Measured rather than theorised: the node held **3 bytes** when the run finished,
so the write had escaped exactly as intended. The *previous* mutation's run had
left its bytes in the file, so this run's baseline was already 3, and its
truncating `creat` landed back on precisely 3.

That is the litter shape one level further out than it had been seen: not
between programs sharing a directory, nor cases sharing a stream, nor suite
sections sharing an image, but **between two runs of one case, through the
artefact the case is about**. A relation is not enough when the failing run is
what contaminates the next one's baseline. The case establishes its baseline now
rather than observing it.

### And the fix blinded the guard that found it

The containment check reports paths that are **new**. Once `/dev/null` is a build
product it never is, so a program writing into it adds nothing to the list. The
probe therefore measures the node's size before and after the whole sweep — 6300
invocations of every installed program against every single-letter option is the
widest net in the tree for "does anything write here", and it costs one `stat`.
As a *difference*, for the same reason the path list is one: bytes already in the
node are the checkout's history rather than this run's finding.

**Ask of any fix to a containment finding whether it leaves the detector able to
see a relapse.**

## The jail's existence predicate: access(2) → lstat(2)

`v8sys_rootpath()` decides whether the rootfs has a name, and it asked with
`access(buf, 0)`. **access(2) follows the last component**, so a symlink inside
the jail whose target could not be resolved read as *absent*, the path fell
through unresolved, and every operation on it went to the host — the wrong
direction for a chroot to fail in. It is now a raw `lstat`.

This is the single most load-bearing function in the shim: every path in the
world goes through it. Three measurements are what made changing it safe, and
they are recorded because the next person to touch it will want them.

**Exactly four shapes disagree on this host**, and all four are "the last
component is a symlink whose resolution fails":

| | access | lstat |
|---|---|---|
| dangling absolute link | `ENOENT` | ok |
| dangling relative link | `ENOENT` | ok |
| symlink loop | `ELOOP` | ok |
| link → target behind an unsearchable directory | `EACCES` | ok |
| a file behind `chmod 000` | `EACCES` | **`EACCES`** — agrees |
| a name below a dangling link | `ENOENT` | **`ENOENT`** — agrees |
| trailing slash on a link to a file | `ENOTDIR` | **`ENOTDIR`** — agrees |

The errnos differ across the four, which is the useful part: a fix keyed on
"`access` said ENOENT" would have covered two of them. Keying on the *question*
covers the class.

**The change is monotone.** There is no case where `access` succeeds and
`lstat` fails, so the union rule can only ever resolve *more* names into the
jail and never fewer. The candidate counterexample is a trailing slash on a
symlink to a **directory**, where `lstat` is documented to follow — measured,
both succeed.

**The union-heavy suites are unchanged**: `wavea` 124, `jail` 128, `kmemu` 146,
before and after. `v8sys` went 173 → 185, and the two cases that failed on the
first run are the two that had been written to *state the limit*, which is what
the old comment asked a future fix to have to change on purpose.

### Two things the fix's own note got wrong

**It said only `V8P_LOOK` changes**, "because the parent case is a directory
and cannot be a dangling link". Nothing stops it being one. With `access`, a
dangling `$V8ROOT/etc` reads as absent, `creat("/etc/x")` falls through, and
the file lands on the **Mac** — the escape direction, in the mode that exists
to prevent it. With `lstat` the name is the jail's and the create fails
`ENOENT`. Neither answer creates the file; only one stays inside. Both modes
changed.

**And the case for that half was vacuous when written as a create.** Reverting
the predicate left it green, because this Mac has no `/usr/src`: the create
fails whichever world it lands in, so the guard and the absence of the
directory are indistinguishable. That is the `chmod 777 /mnt` trap exactly. The
answer is to assert the **resolution** rather than its consequence —
`v8sys_rootpath()` is not static, and whether it returns a `$V8ROOT`-prefixed
path or the bare one *is* what changed. No host directory required.

### And the litter came back

The first run of the rewritten cases failed on `a dangling symlink can stand
where a directory would` — because the *previous* run had died between creating
the stand-in and removing it, and a dangling symlink is exactly what the old
code could not clean up. The case removes it with `hostrm` before creating it
now. A case has to be a pure function of what it set up; this suite is the
third place that lesson has had to be applied.

## A host id narrowed into V8's 16 bits — one rule, four sites, and the fix had reached one

`shim/v8id.h` is `v8_foldid()`, and the contract is two properties rather than a
formula:

```
	root maps to root		(0 -> 0)
	non-root NEVER maps to root	(nothing else -> 0)
	everything that fits stays exact (1..32767 unchanged)
```

The second is the one with teeth. `u_uid` and `st_uid` are `short` — V8's own
widths — and a host id is 32 bits, so a bare `(short)` cast maps every multiple
of 65536 onto **zero**. Measured: 65536 → 0, 131072 → 0. Zero is root, the
identity `fio.c:193`'s `access()` bypasses and `streamio.c:44` lets past a
stream's exclusive-use lock. The cast does not produce a wrong number, it
produces a **privilege**.

### The sweep, and why the earlier fix stopped at one file

`fio.c` was given this fold after an auditor found it folding `p_pid` and then
casting `u_uid` and `u_gid` **on the next two lines**, directly under its own
paragraph explaining why a truncation is wrong. Sweeping for the same shape
across the whole shim found it still standing in two more places:

| | what it feeds | |
|---|---|---|
| `syscall.c` `stat_translate` | every `ls -l`, via `stat(2)` | was a cast |
| `procfs.c` `prgetuarea` | `ps(1)`'s uid column | was a cast |
| `fio.c` `v8k_procinit` | the v8fs server's own identity | already folded |
| `p9cl.c` `p9_t_chown` | an id written **into a disk image** | added below |

So the shape CLAUDE.md records — *the fix landed on one line and the line beside
it kept the assumption* — with the line beside it in **another component**.

`p_uid` is deliberately **not** folded twelve lines from `prgetuarea`: this port
widened it to `int` (`src/include/sys/proc.h:32-35`, because the alignment
padding after a 16-bit field cost the same four bytes the field did), so there
is no narrowing there to guard.

### Why a header and not a function anybody links

No two of the components may share an archive: libv8sys must not link
libv8kern (56 symbol collisions over 29 programs, 25 of them silent), and
libkmemu is the one component that may link host libc. A pure arithmetic rule
needs no link edge, so it is `static` in a header and each translation unit gets
its own copy of the same sentence. Spelling it out four times is the antipattern
`kmem.c`'s one-table rule exists to refuse — and is how the third site came to be
missed.

### getuid(2) is deliberately NOT folded

Every 16-bit *field* is folded. `getuid` is not a field, it is a value that flows
back **out** to the host, and the tree settles it in one line: `mv.c:56` is
`setuid(getuid())`, and `v8s_setuid` hands its argument straight to the kernel.
`mkdir.c:69`'s `chown(d, getuid(), getgid())` is the same shape — on a
passthrough path it reaches the host's `chown`, which wants the real number.

The cost, stated because it is the honest half: `st_uid == getuid()` (`ls.c:81`,
`ps/doselect.c:30`) compares a folded 16-bit value against a raw 32-bit one, so
above 32767 they disagree. **That is not a regression** — the bare cast
disagreed just as surely, for the same values. Making them agree needs a two-way
map (fold in, unfold out to setuid and chown), which is a real design and was
not this change. What this change fixes is the contract: non-root must never
read as root, because root is a privilege and a colliding non-root id is only a
wrong name.

### The fourth site came from the audit, and it is a round-trip failure

`p9_t_chown` sent the raw host id; the server truncates it with V7's own
`ip->i_uid = uap->uid`; and `p9uid()` at the **reading** end refuses any string
beginning with `-`. So `chown(f, 40000)` stored −25536 and `stat(2)` read it
back as `P9UID_BAD` — a value that does not survive the port's own two halves,
and that disagrees with what `stat` reports for the same host id on a jailed
path. Folding before the wire fixes both: the number the server stores is the
number `statof` renders is the number `p9uid` parses, and it is the same number
`stat_translate` produces for that id. **A mount and the jail agree about who
owns a file.**

### Two guards, and neither is redundant

This host's uid is 501 and a CI runner's is lower, so *nothing end to end can
reach the values that matter* — CLAUDE.md's rule is then to assert a relation
the port controls, and there are two different relations here:

- **the arithmetic**, over a table including 0, 32767, 65536, 131072 and −1, in
  `tests/v8sys`. Mutation: make `v8_foldid` a bare cast and four cases fire.
- **the call sites**, as a source sweep in `tests/kmemu` — nothing in the shim
  may narrow an id with a cast again — plus a **derived** count of the files
  that call the fold. Mutation: revert any one site and both fire; revert the
  client's and the count goes 4 → 3.

The unit test cannot see a missing call and the sweep cannot see broken
arithmetic, which is why there are two.

**And the sweep matches its own documentation**, which is a standing note here:
four files now *discuss* the bare cast in prose, so a naive grep counts the
explanation as an instance. Comment lines are excluded and the excluded count is
**printed**, so a reader can see the filter is doing something rather than
hiding something. Both `grep` and `/usr/bin/grep` agree — worth checking,
because the interactive shell here is ugrep, not BSD grep.
