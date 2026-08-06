# mkdir

One of eleven commands imported into a directory of their own and never built —
see the note beside `V8DIRBIN` in the Makefile. Compiles under v8cc with zero
warnings; one change, and it exposed a hole in the shim.

## `pname[128]`, `dname[128]` → 1024

Both are filled by unbounded copies — `strncpy(pname, d, slash)` and
`strcpy(dname, d)` — so the argument's length is the only bound there is. 128
was chosen when a component was `DIRSIZ` = 14 bytes and 128 held nine of them.
This port raises `DIRSIZ` to 254 and macOS `NAME_MAX` is 255, so **one legal
filename overruns both buffers**.

Measured: a 255-character name is `SIGSEGV`, and the fault lands on the *return*
from `mkdir()` — after the directory has been made. So the program dies having
done its work, which is also why a test that checks only `[ -d ... ]` passes on
the broken build. `tests/wavea` asserts the **exit status**; that distinction
cost a mutation round to notice.

1024 is macOS's `PATH_MAX`, the longest argument the kernel will hand over, so
the copies are bounded by construction rather than by a guard to maintain.

## What it found in the shim: `v8s_mknod` did not resolve its path

`mkdir(1)` makes a directory the V7 way — `mknod(d, S_IFDIR|mode, 0)` then two
`link`s — and it is **the only caller of `mknod` in the entire tree**. So when
the creation fix converted `creat`, `link`, `mkdir` and `unlink` to `mkpath()`,
`mknod` was left passing its path raw, and nothing noticed, because nothing
called it. An unreachable syscall cannot be seen to be wrong; building this
program is what made it reachable.

`mkdir(1)` shows both halves of the split in one run: its `access(pname, 02)`
goes through `v8s_access` → `vpath()` and asks the **jail**, then its `mknod`
asked the **host**. Checked one filesystem, wrote to another.

It failed closed only because every jailed prefix happens to be SIP-protected
on this Mac — on a writable one it would have created the directory outside the
jail and reported success. Fixed in `shim/v8sys/syscall.c`; `tests/jail`
asserts where `mkdir /usr/lib/...` lands, and that `rmdir(1)` takes the same
path apart again.

## Not changed

`dname[strlen(dname)] = '\0'` is a no-op — almost certainly meant `-1`. Upstream's,
harmless, left alone.
