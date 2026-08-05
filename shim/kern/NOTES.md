# shim/kern — the machine V8's kernel thinks it is running on

`src/sys/` holds Bell Labs kernel source, unmodified. This directory is
everything that source assumes about the hardware underneath it, written for a
userspace process on ARM64. `src/sys/PORTING.md` is the companion; the reasoning
for each decision is in the file that makes it.

Same division of labour as `compiler/ccom-arm64`: an authentic body, and a
machine-dependent half carrying the names the body expects. There it is
`local.c` and `macdefs.h`; here it is `dev/machdep.c` and `h/`.

```
h/param.h     types, NULL, and the kernel services -- plus the three
              redirections (printf, bcopy, uballoc) that let stream.c stay
              byte-identical instead of being edited
h/mtpr.h      the VAX privileged-register write, honouring SIRR and nothing else
h/conf.h      struct streamtab; the driver switch tables are deliberately absent
dev/machdep.c spl6/splx, the queue scheduler's trigger, panic, kernel printf,
              bcopy -- raw syscalls only, like the rest of shim/ outside libkmemu
```

## The three translations that carry meaning

**`spl6`/`splx` are a nesting counter, and the counter is load-bearing.** On the
VAX these wrote the processor's interrupt priority level. There is no IPL in a
process, so the obvious move is to make them no-ops — and that would be wrong,
because `setqsched()` *consults* the level. A `qenable()` inside a critical
section must not run the service procedures immediately; it must defer them to
the `splx()` that ends the section, exactly as the VAX's software interrupt was
held off until the level dropped. The counter keeps that, and it makes the
behaviour observable, so `tests/streams` asserts it: two of its cases are the
only ones a no-op pair would fail.

**`mtpr(SIRR, 1)` still means what it meant.** V8's `stream.h` ends
`#define setqsched() mtpr(SIRR, 0x1);` and that macro compiles unchanged.
Writing 1 to the Software Interrupt Request Register requests an interrupt at
IPL 1 — "run the queue scheduler as soon as the level allows" — and that request
survives the move off the hardware intact. What changed is the mechanism, not
the meaning. Every *other* privileged register describes hardware that is not
here, so writing one panics; a silent no-op is how a machine-dependent gap turns
into a program that runs and is wrong.

**`panic` really stops.** A kernel that panics does not return to its caller, and
returning would let a corrupted freelist keep being used with a line of output
to show for it. `panicstr` is set first, because `queuerun()` reads it to bail
out early — V8's own comment says "to minimize destruction".

## What this does not do yet, said plainly

`spl6` does **not** block signals. Nothing in this port delivers stream messages
asynchronously — there are no device interrupts here and no signal-driven
producer — so a `sigprocmask` on every `putq` would cost two syscalls on the hot
path to exclude something that cannot happen, and could not be tested. A guard
that has never been seen to fail is not a guard.

When a signal-driven source arrives, `spl6` gains the mask and the counter stays
exactly as it is. That is the one line of this file most likely to need changing,
so it is the one written down.

## The boundary, same as everywhere else in `shim/`

Raw syscalls only. `libkmemu` remains the sole component permitted to link the
host's libc, and this is not it: `v8k_printf` writes with `SYS_write`, and
`v8k_bcopy` is written out rather than taken from libSystem.

That last one is not hygiene. `stream.c` calls `bcopy` and neither `libv8c` nor
`libv8sys` defines one, so leaving it out would not fail the link — it would
resolve out of libSystem, work perfectly, and leave a 1985 Bell Labs stream
engine copying its messages with Apple's code. `tests/streams` checks the
archive's externals for exactly this, and mutation-testing that guard turned up
something worth knowing: removing the `#define` leaks **`_memmove`**, not
`bcopy`, because clang recognises the pattern and lowers it. An assertion phrased
as "bcopy is absent" would have missed it.
