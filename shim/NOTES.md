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

## Still to do here

`stubs.c` provides a placeholder `exit()` that skips straight to the syscall.
V8's real one calls `_cleanup()` to flush stdio first (`libc/sys/exit.s`), and
should replace it as soon as stdio is ported.
