# make

V8's own make, built by v8cc. It compiled and linked with **no source changes**
and no warnings on the first attempt.

Object list, flags (`-DASCARCH -DVERSION8`) and the `$(OBJECTS): defs` line are
lifted from V8's own `Makefile` unchanged.

## Why this is a bootstrap rung, and where it sits

Until this landed, every part of this port was built by the **host's** make.
That is not a detail: it means the build description was ours rather than Bell
Labs', and the dependency knowledge in the authentic makefiles was not being
used. `src/cmd/lex/Makefile` line 11 says

```
lmain.o:lmain.c ldefs.c once.c
```

which is exactly the dependency whose absence caused the lex heap-overrun bug —
a bug that cost a full session to re-derive from scratch. The information was
in the tree the whole time, in a file the build ignored.

**make cannot be the first program ported after the compiler.** It has a
440-line `gram.y`, so it needs yacc. The order is `cc -> yacc -> make`. It
needs no lex.

## What it needs at run time: a shell, and therefore a /bin

`defs:12` defines `SHELLCOM "/bin/sh"`, and `dosys.c:171` execs it for any
command line containing shell metacharacters:

```c
execl(shellcom, shellstr, (nohalt ? "-c" : "-ce"), comstring, 0);
```

So make is the reason the rootfs became a chroot rather than a data directory.
See the `v8dirs[]` comment in `shim/v8sys/syscall.c`: `/bin/` and `/usr/bin/`
are on the redirect list now, and `v8s_execve` routes through `vpath()`, which
it never used to. Verified: a build driven by V8 make under `V8JAIL=strict`
completes with nothing escaping to a host binary.

## Not exercised: archive members

`files.c` implements V8's `a(b)` and `a((b))` notation — file member `b` inside
archive `a`, and entry point `_b` inside an object archive — using `<ar.h>` and
`<a.out.h>` (`struct exec`, `struct nlist`). Both headers exist in the V8
include tree, so it compiles.

It will not *work* against Mach-O archives, because `struct exec` is a.out.
Nothing in this port uses the notation — no makefile here names `lib.a(foo.o)` —
so it is dead code that has to compile rather than a gap. If archive-member
dependencies are ever wanted, that is where the work is.

## LP64 notes

* `TIMETYPE` is `long int` (`defs:15`), used for every file timestamp. On the
  VAX that was 4 bytes and so was `time_t`; here both are 8. The assumption
  holds again for the opposite reason.
* `main.c:246`: `sigivalue = (int) signal(SIGINT, SIG_IGN) & 01;` — the idiom
  the PLAN survey flagged for make specifically. It truncates a function
  pointer to `int`, which is lossy under LP64. It survives anyway: the code
  only wants the low bit, to test whether the previous disposition was
  `SIG_IGN` (which is `(void(*)())1`), and truncation preserves the low bit.
  Real handler addresses are aligned, so their low bit is 0 either way.
  Left as written — it is correct here, and rewriting it would be a change
  made for appearance rather than behaviour.
* Pointer-returning functions are declared where used: `char *malloc()`
  (`misc.c:152`), `char *mkqlist()` (`doname.c:33`), `struct varblock *varptr()`
  in three files. This is unusually careful for the tree and is why nothing
  needed fixing.

## `defs` is invisible to dependency scanning

make's shared header is named `defs`, not `defs.h`, and is included as
`#include "defs"`. It is invisible both to a header scanner and to a `*.c`
glob — the same shape as lex's `once.c`, tbl's `t..c` and refer's `refer..c`,
all four of which have now caused or nearly caused a staleness bug. The
dependency is spelled out in the Makefile and covered by `tests/deps`.

## What the jail does NOT yet contain: the cc driver

Worth stating plainly, because it qualifies the claim above. `rootfs/bin/cc` is
**not a V8 binary**:

```
$ nm rootfs/bin/cc  | grep -c '_v8s_\|_v8start'      ->  0
$ nm rootfs/bin/cat | grep -c '_v8s_\|_v8start'      -> 72
```

The Makefile builds it with `$(HOSTCC)`, and it has never been rebuilt by v8cc
since stage 0. It runs on the host's libc, so the shim never sees its syscalls
and `V8JAIL` cannot observe it. A compile started inside the jail leaves it at
the driver and does not come back — which is why `V8JAIL=strict cc -o h h.c`
succeeds and reports nothing, despite the driver exec'ing clang.

So: a build driven by V8 make runs V8's sh and V8's filters, and that part is
real and tested. The compiler driver is the exception, and it is an
unfinished bootstrap rung rather than a design decision — unlike `as`/`ld`,
which are deliberate. Tracked as B5, with the fixpoint.
