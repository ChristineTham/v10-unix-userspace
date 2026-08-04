# troff and nroff, ported

Both build from V8 source with V8's compiler (`TFILES` and `NFILES` from the
original makefile, `nroff` with `-DSMALLER -DNROFF`) and both run far enough to
open their data files.

## Three porting changes

| Where | What | Why |
|---|---|---|
| `tdef.h` | `typedef int tchar` | tdef.h's own comment calls a tchar "a 32 bit cookie", and troff means it: the type packs a character with its size and font using the masks beside it. On the VAX `long` *was* 32 bits. Under LP64 it is 64, which changes the packing and made `tchar gettch()` conflict with the implicit-int declarations elsewhere — nroff would not compile at all. |
| `n1.c` | `fdprintf` walks its arguments with `unsigned long *` | V8's varargs idiom: take the address of the last named parameter and walk forward. The stride must match an argument slot, and SZARG is SZLONG here. With the original 4-byte stride every argument after the first came from the wrong half of a slot, and `%s` produced a pointer missing its top 32 bits — nroff died on its very first message reading `0x6fdfe460`. The `%D`/`%O` cases lost their extra `adx +=` for the same reason: a long is one slot now, like everything else. |
| `n1.c` | temp file in `/tmp` | `/usr/tmp` was a link to `/var/tmp`; macOS has neither, and `/usr` is protected by SIP so it cannot be made. |

The `fdprintf` bug is worth noting for what it did *not* look like. It crashed
inside troff's own printf, which reads as a troff problem; it was the argument
block, which is a property of the target model. The same idiom is why
`libc/stdio/doprnt.c` walks with an 8-byte stride, and any other program that
rolls its own varargs will need the same treatment.

## What is left

They open `/usr/lib/term/tab.37` (nroff) and `/usr/lib/font/...` (troff) by
absolute path, and those directories cannot be created on macOS either. The
tables themselves are in the tree — `src/cmd/troff/term/` and `dev*/`.

The fix is to root those paths at `$V8ROOT`, which is what the rootfs is for,
and it is worth doing once rather than per program: `eqn`, `tbl`, `refer` and
`man` all reach for `/usr/lib/tmac/...` the same way. A small helper that
prefixes `$V8ROOT` when the variable is set, defaulting to the original path
when it is not, keeps every one of those sources honest.
