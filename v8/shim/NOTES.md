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

* **no `SA_RESTART`** — V8 programs expect a slow read to fail with `EINTR`;
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
