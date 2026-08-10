# shim/kern — the machine V8's kernel thinks it is running on

`src/sys/` holds Bell Labs kernel source, unmodified. This directory is
everything that source assumes about the hardware underneath it, written for a
userspace process on ARM64. `src/sys/PORTING.md` is the companion; the reasoning
for each decision is in the file that makes it.

Same division of labour as `compiler/ccom-arm64`: an authentic body, and a
machine-dependent half carrying the names the body expects. There it is
`local.c` and `macdefs.h`; here it is `dev/machdep.c` and `h/`.

```
h/param.h     types, NULL, and the kernel services -- plus the redirections
              (printf, bcopy, longjmp, psignal, uballoc) that let the authentic
              sources stay unedited, and the three host type-guards
h/mtpr.h      the VAX privileged-register write, honouring SIRR and nothing else
h/conf.h      struct streamtab; the driver switch tables are deliberately absent
h/user.h      the u-area, thirteen fields, claiming no layout
h/proc.h      four fields, at UPSTREAM's widths -- see src/sys/PORTING.md hazard 2
h/buf.h       two constants out of a 107-line buffer cache
dev/machdep.c spl6/splx, the queue scheduler's trigger, panic, kernel printf,
              bcopy -- raw syscalls only, like the rest of shim/ outside libkmemu
sys/slp.c     tsleep and wakeup, and the setjmp half of u_qsav
sys/fio.c     the u-area, the proc entry, the file table, the inode edge
sys/subr.c    min, nulldev, copyin/copyout, iomove, the signal names
sys/ioconf.c  streamtab[] and nstream, which config(8) generated upstream
```

The `sys/` half arrived with `src/sys/sys/streamio.c` and is named after the
files V8 keeps each function in, so the mapping back to upstream is legible
without a table.

## The four translations that carry meaning

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

**`tsleep` is `queuerun()` and then `poll()`, and that is a fourth translation
of the same kind.** PLAN.md §8a step 2 answered "what is at the bottom of a
stack" for filesystems with "the host"; a stream's driver end is a host
descriptor, so the kernel's *wait for the device to interrupt* becomes *wait for
the fd standing in for the device*. What a driver registers is therefore a
descriptor **and a handler** — registering only the fd would make `tsleep`
return to a caller whose queue is still empty, because nobody read the device.

And `wakeup` is not the no-op the survey predicted: it counts. Every `wakeup` in
`streamio.c` sits where a producer has just made progress, so a counter compared
across `queuerun()` is exactly the one bit a single-threaded kernel can use, and
it is what lets `tsleep` distinguish "something happened" from "nothing will".
`sys/slp.c` has the argument.

## What this does not do yet, said plainly

`spl6` does **not** block signals. Nothing in this port delivers stream messages
asynchronously — there are no device interrupts here and no signal-driven
producer — so a `sigprocmask` on every `putq` would cost two syscalls on the hot
path to exclude something that cannot happen, and could not be tested. A guard
that has never been seen to fail is not a guard.

When a signal-driven source arrives, `spl6` gains the mask and the counter stays
exactly as it is. That is the one line of this file most likely to need changing,
so it is the one written down.

## THIS ARCHIVE CANNOT BE LINKED INTO A V8 PROGRAM, and that is measured

§8a step 5e set out to make `open(2)` land in v8fs by adding a fourth type to
`shim/v8sys/vfs.c`. That type has to call `namei`, `iget` and `readi`, so the
client would have to link this archive. It cannot, and the reason is not the
240.7 KB of bss that made it a separate archive in the first place.

**Link `cat` against it and `cat`'s buffer disappears.** `ld` says so:

```
tentative definition of '_buf' with size 4096 from bin/cat.o
  is being replaced by real definition of smaller size 8
  from libv8kern.a[18](main.o)
```

`cat.c:10` is `char buf[BLOCK]`, `BLOCK` 4096, and `shim/kern/sys/main.c:213`
is `struct buf *buf = v8k_buftab`. A K&R tentative definition is a **common**,
the kernel's is a real definition, and resolving a common against a real
definition is exactly what a linker is supposed to do. So `read(0, buf, 4096)`
writes 4096 bytes into an eight-byte pointer.

**Whether you notice is a property of the layout, not of the bug.** Two links
of the same two objects:

| link | `_buf` | result |
|---|---|---|
| `-force_load` | `__DATA,__data`, 8 bytes | **SIGSEGV**, exit 139 — *and the 3000-byte output was still byte-identical*, which is `mkdir`'s "a crash can happen after the work is done" arriving in the linker |
| natural (`-u _v8k_kinit`, no force) | `__DATA,__data`, 8 bytes | **exit 0**, output byte-identical, nothing said |

The second is the real one, because a `vfs.c` type referencing one kernel entry
point is all it takes to pull `main.o` in — no `-force_load` required. And what
the overrun lands on is not arbitrary. `nm -n`:

```
000000010001d588 D _buf
000000010001d590 D _buffers
000000010001d598 D _nbuf
```

`cat`'s first `read` scribbles over `_buffers` and `_nbuf`, **the buffer
cache's own pointers**, and exits 0.

### The population, swept rather than guessed

297 program objects against this archive's 266 defined names: **56 (object,
name) pairs, over 33 objects and 29 programs, on 27 distinct names.**

| pair | count | what the linker does |
|---|---|---|
| `T`–`T` | 30 | refuses under `-force_load`; without it the program's own wins, which is correct |
| `C`–`D` | 13 | **silent** — the common is replaced. `cat`'s case |
| `C`–`C` | 6 | **silent** — merged, larger size wins |
| `C`–`T` | 5 | **silent** — the common resolves to a text address |
| `T`–`C` | 1 | **silent**, the other way round |
| `D`–`D` | 1 | refuses |

**25 of the 56 are silent.** They are not obscure names either — they are the
1985 vocabulary: `buf bread alloc bmap tty file bwrite getblk iput itrunc panic
copyin copyout nfile proc`. `_buf` alone hits eight programs (`cat join wc clri
lex man mkfs refer`), and the checkers are over-represented for a structural
reason: `icheck`, `dcheck`, `ncheck`, `fsck`, `mkfs`, `clri` and `restor`
reimplement the kernel's own algorithms under the kernel's own names.

### Hiding the symbols gets most of the way and stops

`ld -r -exported_symbols_list` (keeping only `_v8k_*`) makes 22 of the 27 names
private. The other five — `bootime ecmx nswap runout tty` — survive, because
**`ld` will not make a common symbol a private extern**, and two commons merge
by taking the larger size with no diagnostic at all. So the mitigation converts
the loudest half of the problem into the quietest half of the problem.

### And the second reason is independent of all of it

`shim/v8sys/vfs.c:167` already said it, before any of this was measured: the
descriptor-type table "does not survive a program replacing itself". A v8fs
descriptor is an inode pointer and an offset in process memory, so after `exec`
the integer means nothing — and `cat /mnt/a > /mnt/b` needs `sh` to open the
target and `cat` to inherit it. Even a perfect symbol-hiding scheme leaves
shell redirection into a mount impossible.

**Both roads lead to a server**, which is what PLAN.md §8a said the seam was
for. `tests/kmemu` asserts that no V8 binary links this archive, so re-opening
the in-process option means deleting a case that says why it was closed.

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

**`setjmp` is the same decision one level up, and it is why the archive imports
three names rather than one.** `streamio.c` aborts a system call with
`longjmp(u.u_qsav)` when a signal arrives mid-sleep, so `sys/slp.c` has to do
the matching `setjmp` — and taking that from `<setjmp.h>` would be `bcopy` all
over again.

(That last clause used to read "in an archive linked into V8 programs", and the
section above is what made it false: **nothing links this archive into a V8
program and nothing can.** The argument survives the correction intact, because
it was never about who links the archive — it is about a Bell Labs kernel using
Apple's implementation of a primitive V8 shipped its own version of. The one
caller today is `tests/streams/fsprobe.c`, a host binary, and the claim would be
the same if it stayed the only one for ever.)

`src/include/setjmp.h` is
this port's own, 24 longs for AAPCS64's callee-saved set, implemented in
`compiler/setjmp.s` to the same ABI clang compiles this directory with. So the
kernel and the program above it use one jump buffer and one implementation.

`tests/streams` now asserts the externals by **subtraction** — everything the
archive undefines minus everything it defines — rather than by grepping away a
hand-written list of the names it exports. The list would have had to grow by
every name `streamio.c` and `shim/kern/sys/` added, and a name-by-name allow
list is exactly how `tests/kmemu`'s allowed leaks went stale. The answer is
`_memcpy`, `_setjmp`, `_longjmp`, and all three are checked to be V8's own.
