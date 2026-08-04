# libv8sys

The layer standing in for the VAX kernel. `make libv8sys`; `make test-v8sys`
(44 tests) and `make test-freestanding` (7) cover it.

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
