# libv8sys: state and the one open problem

`make libv8sys` builds it; `make test-v8sys` runs 44 behaviour tests, all
passing. The syscall surface, V7 directory emulation, V7 signal semantics, the
sbrk arena and the sgtty/termios mapping are done and tested.

`compiler/crt0.s` assembles and `shim/v8sys/stubs.c` compiles.

## Open: how the V8 world reaches the shim without the shim reaching itself

The V8 world calls `open`, `read`, `write`. The shim implements them as
`v8s_open`, `v8s_read`, `v8s_write` — and implements them **by calling the host
functions of the original names**. So some mechanism has to give a program two
different `write`s: the world's, and the host's.

Two approaches tried:

**Defining `write` in stubs.c.** Then the shim's own `write(fd, b, n)` in
syscall.c binds to our definition and recurses.

**Linker aliasing** — `-Wl,-alias,_v8s_write,_write`, leaving the C names
distinct. This links cleanly and looks right, and then dies:

```
stop reason = EXC_BAD_ACCESS (code=2, address=0x16f603ff0)
frame #0: v8s_write + 4
```

That is a stack-overflow fault in a function prologue. The alias is global, so
the shim's own call to host `write` also resolves to `v8s_write` — unbounded
recursion. The alias cannot distinguish "calls from the V8 world" from "calls
from inside the shim", because at link time there is only one `_write`.

### The fix, for next session

Have the shim reach the kernel **without naming the libc functions at all**, so
there is no symbol left to collide over. Options, in order of preference:

1. **`syscall(SYS_write, ...)`** via `<sys/syscall.h>`. One edit per stub,
   no assembly, works identically on Linux with its own numbers. macOS
   deprecates `syscall(2)` but it still functions; if that becomes a problem,
   option 2.
2. **Inline `svc #0`** with the number in `x16`, which is what libSystem itself
   does. Fully under our control and cannot be intercepted, at the cost of a
   small amount of per-target assembly.
3. **Two libraries** — build the shim against host libc as a dylib with its own
   two-level namespace, and let the static V8-facing names live only in the
   executable. Works, but the linking gets subtle in a way the first two do not.

Option 1 first; it is a mechanical change to `syscall.c` and the tests already
cover the behaviour it must preserve.

### Why this did not surface earlier

`tests/v8sys/test.c` links the shim against host libc and calls the `v8s_`
names directly, which is the right way to test the seam's *behaviour* — and it
never creates the collision, because nothing is aliased. The collision only
appears when a V8 program is linked with `-nostartfiles` and expects the shim to
BE libc. Worth remembering: the test suite passing 44/44 says the semantics are
right, not that the linkage is.
