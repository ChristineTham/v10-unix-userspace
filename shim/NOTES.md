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

## Open: stdio faults inside putc

`fputs("...", stdout)` dies with `EXC_BAD_ACCESS` reading `0xffffffffffffffff`
at `fputs+76` — inside the `putc` macro, before `_flsbuf` is reached.

Ruled out so far:

* `_iob` links and is correctly initialised; `_iob[0]` reads
  `{0, _sibuf, _sibuf, _IOREAD, 0}` at runtime, and the struct is the expected
  32 bytes.
* The pre-decrement through a struct pointer — `--(p)->_cnt >= 0`, the awkward
  half of the macro — compiles correctly on its own; the generated code was
  read and is right.
* The only undefined symbol in the image is `_isatty`, which `_flsbuf` calls
  and which we have not ported yet.

So the remaining suspects are the *other* half of the macro,
`*(p)->_ptr++ = (unsigned char)(x)` — a post-increment through a pointer field
combined with a narrowing store — or something about how the two arms of the
conditional expression are joined. Next step is to compile that expression
alone, the way the pre-decrement was, and read the code.

Note the shape of this: the compiler passes 62 tests including post-increment,
struct pointers and narrowing stores *individually*. Whatever this is, it is in
their combination, which is what real libc code does and synthetic tests do not.
