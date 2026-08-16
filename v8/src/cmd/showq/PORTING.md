# showq(8) — porting notes

Wave A2 batch 2d.  Prints the STREAMS queue, cblock and stream-head tables out
of a running kernel.  One source, 515 lines, installs to `/etc` because
`Admin/etcfiles` says so.

Upstream: `v8/usr/src/cmd/showq`.

## The one change: four absolute includes

Upstream opens

```c
#include <nlist.h>
#include "/usr/sys/h/param.h"
#include <ctype.h>
#include "/usr/sys/h/stream.h"
#include "/usr/sys/h/inode.h"
#include "/usr/sys/h/conf.h"
```

and this port has no `/usr/sys`.  Same change `ls.c:11`, `mc.c` and
`termcap.c` already carry for `"/usr/jerq/include/jioctl.h"`, and `p.c` carries
in this same batch — an absolute path is the one include cpp cannot be told to
resolve elsewhere.

**IT IS A MEASURED NO-OP RATHER THAN AN EQUIVALENCE ARGUED FROM THE NAMES**,
which is what makes it a stronger case than the jerq four.  On the shipped V8
the kernel headers are installed into `/usr/include/sys` as well, and all four
pairs are byte-identical:

```
usr/include/sys/param.h   vs usr/sys/h/param.h    IDENTICAL
usr/include/sys/stream.h  vs usr/sys/h/stream.h   IDENTICAL
usr/include/sys/inode.h   vs usr/sys/h/inode.h    IDENTICAL
usr/include/sys/conf.h    vs usr/sys/h/conf.h     IDENTICAL
```

So the two spellings named one file.  Only `param.h` differs **here**, because
this port patches its own — `DIRSIZ` 254 and the `#ifndef` guard, 26 lines in
all — and `showq` reads no directories, so that difference cannot reach it.

## What it does on this machine

```
$ showq
can't open /dev/mem
```

and exit 1.  That is the honest answer and it is why the program is here at all.

`showq` `nlist()`s a kernel for eight symbols — `_queue`, `_cblock`,
`_streams`, `_qfreelist`, `_dtlinfo`, `_dkpinfo`, `_Nblock`, `_Nqueue` — and
then reads them out of `/dev/mem`.  `shim/libkmemu/kmem.c` manufactures a
namelist at `/unix` and a `/dev/kmem` from one table, and that table holds
`_avenrun` and `_bootime` and nothing else.  Supplying the other eight would
mean manufacturing a STREAMS subsystem: eight tables of made-up queues for a
kernel that is not running.  That is a decision, not a side effect of importing
a program, so **`showq` is deliberately not linked against `libkmemu`** and says
what is true.

This is the `load(1)` and `w(1)` precedent — `w` says `No mem` for the same
reason — and the distinction those two exist to make: rung 5 is a claim about
the build *description*, and a correctly built program that cannot answer is a
correct outcome.

## What it found

Nothing in `showq` itself, but importing it and `dmesg(8)` together is what
reached `nlist(3)`'s missing null guard — see `src/libc/gen/PORTING.md`.  Both
ask for symbols that are **absent** from the manufactured namelist, which is a
code path no previous consumer had taken, and the walk to the list terminator
dereferenced address 0.

## Eliminated by measurement

`showq.c:244-245` call `malloc` undeclared and cast the result.  The
`src/cmd/awk/PORTING.md` measurement applies: a cast on the call is not a use of
the `int` type, v8cc never materialises the value as 32 bits, and there is
nothing to truncate.  `tests/v8ccom`'s rootfs-wide sweep is the standing guard
and is silent for `showq`.

## Still open

Whether `libkmemu` should ever grow a synthetic STREAMS table.  It would make
`showq` print something, and everything it printed would be invented.  The
present state is preferable: the program is authentic, it builds, it runs, and
it declines.
