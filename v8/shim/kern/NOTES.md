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

## AND THE SERVER IS BUILT, so the section above is a design and not just a refusal

§8a step 5e. `shim/v8fsd/v8fsd.c` is a host binary that holds a disk image
open, links this archive, and answers **9P2000** on a Unix-domain socket. A
client walks a path, opens a file and reads it; `namei → fsnami → dsearch →
iget → bmap → readi → bread` runs on the far side of the socket, and `cmp`
says the 28000 bytes that come back are the ones that went in.

`tests/streams` 372 → 412. The probe is `tests/streams/p9probe.c`, which
speaks the wire directly rather than through the shim — the same
independent-reader argument step 5d had to learn, where a mutation that made
`alloc()` hand out one block twice left every case in `fsprobe` green and was
caught only by `icheck`, `fsck` and a `cmp`.

Three things came out of building it that were not in the costing.

- **A DRIVER SET IS PART OF A CONFIGURATION, NOT OF THE KERNEL LIBRARY.** The
  image driver moved out of `tests/streams/fsprobe.c` — the unconsumed-component
  rule finally has a consumer — and the obvious place to put it was
  `KERN_OBJ`. That immediately broke the case asserting this archive imports
  only `_memcpy`, `_setjmp` and `_longjmp`: a block driver does host I/O by
  definition, so it brought `_pread` and `_pwrite` in with it. The guard was
  right and the placement was wrong. `config(8)` is what chooses a driver set
  on a real V8 and `v8k_bdconf` already stands in for `config(8)`, so
  `imgdev.o` belongs on the link line of whatever is being configured. The
  probe and the server now get the same object, which makes the probe's 236
  cases coverage for the server's block layer.

  **And removing it from the archive did not remove it from the archive.**
  Dropping an object from `KERN_OBJ` leaves the target newer than every
  remaining prerequisite, so the `rm -f && ar rcs` rule never re-ran and
  `nm -u` still showed both names. That is the `ar r` note in the Makefile one
  level up: there, dropping a *source* leaves a stale member; here, dropping an
  *object* leaves a stale archive.

- **NOTHING IN THE PATH SLEEPS, WHICH IS WHAT MAKES ONE PROCESS ENOUGH.** The
  server must be single-threaded — the buffer cache needs exactly one authority,
  and V8's kernel keeps its per-call state in a global u-area, so it cannot be
  re-entered at all. What makes that *sufficient* rather than merely necessary
  is the synchronous driver: `iodone()` runs inside `strategy`, so `iowait()` at
  `bio.c:426` finds `B_DONE` already set and never sleeps. Every request is
  carried to completion between two `poll()` returns.

- **A DEADLINE THAT APPLIES ONLY MID-MESSAGE, and the two halves are what make
  it right.** A connection here *is* an open file and may idle for as long as
  the program holds it, so it must never be dropped for saying nothing;
  `poll()` is what protects that, since an idle connection is never read. Once
  poll reports data the first read cannot block, so `SO_RCVTIMEO` can only fire
  part-way through a message — which means the peer died between two writes,
  and a single-threaded server that waited would be wedged by one dead client.

Two things it does *not* do, stated rather than left to be discovered.
**It is read-only**: `Twrite`, `Tcreate`, `Tremove` and `Twstat` answer
`EROFS`. The kernel underneath them is written and tested — step 5d did
`writei`, `bmap`'s allocating arm, `alloc`/`free`, `ialloc`/`ifree`, `itrunc`
and `namei` with `NI_CREAT`/`NI_DEL` — so that is a boundary in the server
rather than a gap in the port, and splitting it the way 5c and 5d were split
keeps a failure attributable. **And no client speaks to it yet**: the fourth
type in `shim/v8sys/vfs.c` is the next piece, and the sweep run before writing
it found eleven entry points with no slot in `struct v8fstyp` at all.

### And a review of it found a remote crash in four messages

A review subagent read the server the day it landed. Inode accounting came back
clean — measured, not read: twelve request shapes × 400 iterations with
`NINODE` at 80, plus 400 hard disconnects holding three fids, and nothing
leaks. The poll-array indexing is clean too, and for a reason worth knowing:
`accept` runs *before* the serve loop and nothing in the serve loop opens a
descriptor, so no fd number can be freed and re-issued inside one cycle.

What it found instead was eight things, and the first is the reason to send a
reviewer at new code rather than only at imported code.

**A directory `Tread` at any offset ≥ 2^63 was an out-of-bounds heap read and,
a little further out, a SIGSEGV.** The offset is a `p9_u64` on the wire and the
bound cast it to a signed `long`, so every such offset read as negative and
passed; `f_dirlen - off` then exceeded the buffer and `f_dir + off` pointed
below it. Measured on a 219-byte listing: 2^64-1 returned 220 bytes starting
one byte early, -7973 returned 7973 bytes of heap, and -2^40 crashed the
server — **taking every other connection with it**, verified with a second
client attached. Four messages, no authentication. The *file* arm eight lines
away had the guard all along (`off > 0x7fffffffULL`): the fix landed on one line
and the line beside it kept the assumption.

**AND THE FIRST FIX WAS TWO GUARDS, ONE OF THEM DEAD.** An unsigned range test
plus an entry-boundary walk — and reverting the range test changed nothing,
because the walk rejects a negative offset too. A mutation that does not fire
is the informative one. The range test moved *inside* `dirboundary`, where it
runs before any cast to a signed type, and there is one guard instead of two
with one unexercised. Mutating to the pre-fix line now takes the suite from 429
to 374 with the probe dead.

The other seven, briefly:

- **No send-side deadline**, while the comment beside it argued the read side at
  length. The socket buffer is 8192 and an `Rread` is up to 8203, so one client
  that stops reading blocks the whole server in `write(2)` — measured, a
  victim's latency went from 0.0000s to a 5-second timeout and recovered the
  instant the other client drained. It needs no malice.
- **A stale ALL-CAPS claim**: the comment said a mid-entry offset got `EINVAL`
  and nothing enforced it. Now it does.
- **"Read only" is a claim about the PROTOCOL, not about the filesystem.**
  `readi` sets `IACC`, so `iput` at `i_count == 1` runs `IUPDAT` and dirties the
  disk inode. Two accidents hide it — the fd is `O_RDONLY` so `pwrite` fails and
  `bdwrite` discards the error, and nothing calls `bflush()` — and **both stop
  applying at step 5f**, which needs `O_RDWR`. Measured on an image whose
  `di_atime` had been touched: the read path changed four bytes.
- **`makedev`/`major`/`minor` were silently the HOST's in `v8fsd.c`**, because
  `<sys/socket.h>` pulls `<sys/types.h>` and the redefinition is inside a
  system header, so there is no `-Wmacro-redefined`. V8 shifts the major by 8
  and Darwin by 24, and `dev_t` is a `u_short`, so `makedev(i, 0)` was **zero
  for every i**. Latent only because the image driver registers first and 0
  packs the same either way. It is precisely the trap `param.h` claims a guard
  for, arriving through the three names it does not claim. Fixed structurally:
  `v8k_imgdev()` does the packing in `imgdev.c`, which includes no host header.
- **A refused `Tversion` had already clunked every fid**, because the reset ran
  before the validation. The client was told the negotiation failed and lost its
  state anyway.
- **A `Twalk` element was not checked to be one path component.** `namei` splits
  on `/` and restarts at the root for a leading one, so `sub/deep` traversed two
  components and reported one qid, and `/hello` escaped the directory the fid
  was walked from. Not a containment hole — `namei` cannot leave the image — but
  the qid count is exactly how a client tells how much of its path exists.
- Three small ones: `ENOMEM` was missing from the error table so the server's
  own error reached clients as `EIO`; fid-table exhaustion was reported as
  `EEXIST`, indistinguishable from a naming clash; and a `NULL` check on
  `bread`, which cannot return one.

---

## §8a step 5e, the other half: the CLIENT, and where 9P assumes a kernel

The server above answered a probe. It now answers `cat`. `shim/v8sys/p9cl.c`
is a fourth type in the filesystem switch, and with `V8MOUNT=/mnt=sock` set,
`rootfs/bin/cat /mnt/sub/deep` returns 28000 bytes byte-identical to the file
`mkfs` was handed — through Bell Labs' `namei`, `iget`, `bmap` and `readi`, in
another process, over a disk image. `ls`, `tail`, `wc`, `grep` and `sh`
redirection all work unmodified. `tests/streams` 429 → 449.

**THE CONNECTION IS THE OPEN FILE DESCRIPTION, and that one sentence is the
design.** One `connect()` per `open(2)`. A `struct file` is the object Unix
shares across `dup`, shares across `fork`, and carries through a program
replacing its own image — and a socket is shared by `dup`, shared by `fork`,
and survives the image being replaced. They are the same object. So:

- the **fid is a constant** (attach → 0, the walk clones onto 1), because a
  connection carries exactly one open file;
- the **offset lives on the server**, in the fid;
- and therefore **the client holds no per-descriptor state for a regular file
  at all**. An inherited descriptor is fully usable by a program that knows
  nothing about it — not because anything was arranged to be inherited, but
  because there is nothing to inherit.

**WHICH EXPOSED THE ONE THING 9P HAS NO MESSAGE FOR.** 9P is a pread/pwrite
protocol: every `Tread` carries an absolute offset, because Plan 9's *kernel*
holds the file offset in the Chan behind the descriptor. There is no kernel
here. The extension is one concept — a fid has a cursor, `P9_OFFCUR` uses and
advances it, any other offset is 9P's own pread and does not touch it — plus
`Tseek`/`Rseek` numbered outside 100..127 to read and set it. `p9.h` argues it
at length.

**AND THE FIRST DRAFT CLAIMED A CONFORMING CLIENT COULD NOT TELL, WHICH
`p9probe` REFUTED WITHIN THE HOUR.** It reads a directory at 2^64-1 on purpose,
to prove the unsigned-offset crash guard is still there, and that offset is now
the sentinel. The honest claim is narrower: the extension is invisible at every
offset a conforming client can read a byte from, and 2^64-1 is not one. The
probe's case moved to 2^64-2, which exercises the identical arm. **A test
refuting a sentence written the same hour is the right way round.**

### Three bugs the end-to-end run found that no probe could

- **`t_close` sent a `Tclunk`, and a clunk is not `close(2)` — it is the LAST
  close.** The fid belongs to the connection, which every `dup` of the
  descriptor shares. So `sh` doing the ordinary thing for `cat < /mnt/hello` —
  open, `dup2` onto 0, close the original — clunked the file out from under the
  descriptor it had just made, and `cat` printed nothing. The right number of
  clunks is **zero**: the kernel drops the connection at the last close because
  it is the thing that knows the reference count, and `connclose()` frees the
  fids on EOF. **The comment beside the bug got the mechanism exactly right**
  — "dropping the connection is what actually releases the server's fids" —
  and then called the clunk "politeness". It was not politeness, it was the bug,
  written inside the sentence explaining why it was unnecessary.
- **A directory read carries BARE stat structures; only `Rstat` has the outer
  count.** `Rstat` wraps one stat in the message's `stat[n]` field, which is a
  2-byte count in front of a structure whose first field is a 2-byte count. A
  directory read is a plain sequence of the structures. The client had the
  extra `p9_g16` in both places, so every entry decoded shifted by two — and it
  presented as `ls: /mnt unreadable`, because `open(2)` is what fails when the
  snapshot cannot be built.
- **A directory's `st_size` is the SNAPSHOT's length, not the server's**, which
  is `dir.c:114`'s rule arriving in a second filesystem. 64 bytes on the image
  against 768 bytes of 256-byte V7 records. The port fixed this once for
  passthrough (`v8sys_pt_fstat`, fstat only) and the fix had to be made again
  here — *"the fix landed on one line and the line beside it kept the
  assumption"*, where the line beside it is a whole second implementation of
  the same interface.

### getpeername(2) is the identification, and the table became a cache

`vfs.c`'s `fdtyp[]` is process memory. It dies when a program replaces its own
image, and a server-backed descriptor that then reads as passthrough gets a raw
`read(2)` on a 9P socket — which **blocks forever**, because the server sends
nothing unsolicited. So the authority moved to the kernel, which still knows
what a descriptor is: a *connected* client reports the server's bound path
(len 106), while an `accept()`ed descriptor and a `socketpair` report an empty
path (len 16), so the answer is positive identification of our own socket
rather than "this is a Unix socket".

The table now has **three** states where it had two — null means *unexamined*
rather than *passthrough* — because without that distinction the fallback would
run a `getpeername` on every read of stdin forever.

### Eleven syscalls have no slot, and that stopped being containable

`link unlink rmdir mkdir mknod symlink readlink chmod chown utime` reach the
host directly: they are passthrough by construction, and `rootpath()` no longer
prepends `$V8ROOT` for a mounted path (it must not — the file is on an image in
another process). That was survivable while every type answered out of the
jail; with a mount it means `rm /mnt/x` asks **the Mac** to unlink `/mnt/x`.
There is no `/mnt` on this machine, which is luck and not a design.

They refuse with `EROFS`, one line each, rather than being given slots — a slot
is a claim that the operation is implemented, and `EROFS` is the truth, and it
is the same answer the server gives to `Twrite`/`Tcreate`/`Tremove`/`Twstat`
today. The three readers are not in that list: `access()` is implemented over
`t_stat`, `readlink()` is `EINVAL` (a V7 image contains no symbolic link, so
every name on it is "not a symbolic link"), and **`chdir()` is the one genuine
gap** — nothing here tracks a working directory, so a program that got inside a
mount would find every relative name resolving against the host.

### Two costs, stated rather than hidden

`t_stat` is a whole connection per question, so `ls -l` on a mount is one
connect/version/attach/walk/stat/close per entry. Not fixed, because the fix is
a cache and nothing has measured the cost yet.

And **the socket path is not free**: `sun_path` is 104 bytes and a Mac's
`$TMPDIR` is around 50 before anything is appended. A mount whose socket path
will not fit does not exist, which presents as `ENOENT` on the file — the first
end-to-end run used a 130-character path under the session scratch directory
and `cat` said "No such file or directory" about a file that was there. The
suite binds and connects relative, by `cd`-ing into the directory, which is
what the server section already did for its own half.

### The auditor on the client, and what twelve findings had in common

`lp64-auditor` was run on the client the hour it was written — CLAUDE.md's rule
that the subagent earns its keep on *new shim code* rather than on the 1985
half. It came back clean on the dominant class (no libc leak, no symbol
collision, no variadic call, the Rstat double-prefix asymmetry correct, the
three-state descriptor cache correctly invalidated) and found **twelve other
things, every one of them measured**. The pattern: none is an LP64 bug. They
are all *a rule stated in one place and not applied in the one beside it*.

**Four could hand a program a wrong answer with exit 0**, which is the class
this port refuses:

- **A dup'd or inherited v8fs DIRECTORY descriptor returned raw 9P stat
  records.** The snapshot is keyed on the fd, so `dup(2)` alone loses it — no
  redirection, no exec — and `p9_t_read` then fell through to a real Tread.
  Measured: **222 bytes** whose first two are a stat's `size[2]`, read as a
  `d_ino` of 51, exit 0. And `dir.c`'s comment claimed this "inherits a limit
  this shim has had since `v8sys_diropen` was written… nothing redirects a
  descriptor from a directory, so the case does not arise". Both halves wrong:
  the case arises through `dup`, and the pre-existing passthrough version fails
  **loudly** (`read(2)` on a host directory fd the shim does not know is −1),
  so the new one was not inheriting a limit but inventing a worse one. The
  server now refuses a cursor read on a directory fid — the client converts to
  V7 records and the position that matters is in *that* stream, so there is no
  sensible cursor for this end to keep.
- **`access()` disagreed with `open()` on every file of every image.** It
  recomputed permission from the image's mode bits against **the host's** uid;
  the server calls Bell Labs' `access()` with `u.u_uid`, which nothing sets and
  which is therefore **0**, so `fio.c`'s root bypass applies. `mkfs` protos make
  everything root-owned and no V8 program here runs as uid 0, so the
  disagreement was total: `test -r` said no and `cat` printed the file. The
  bits were the server's; the **identity** was the host's, and identity is the
  half that decides.
- **The client could not read a byte from a conforming 9P2000 server**, and
  said nothing about it: it sends `P9_OFFCUR` on every Tread, a server with no
  cursor returns zero bytes, and `cat` printed nothing and exited 0 against an
  `Rstat` reporting a length. 9P's own version negotiation is the place to
  catch that, and it was not being used — the client now offers `9P2000.v8`
  and refuses a mount that answers `9P2000`.
- **`p9uid()` mapped an unparseable owner to 0, i.e. root**, against
  CLAUDE.md's explicit contract for every 16-bit narrowing in this port: *root
  maps to root, and non-root never maps to root*. Reachable by an ordinary
  route — `di_uid` is `v8_i16` and therefore **signed**, so a file owned by
  40000 loads as −25536, `statof` renders `"-25536"`, and the `'-'` is a
  non-digit. `ls -l` printed it as `root`. Note the shape: the range guard that
  *was* written (`v > 32767`) is dead against this server, because `"%d"` of a
  short cannot exceed it — **the guard that could not fire and the case that
  did fire returned the same value.**

**One was a jail escape.** `rootpath()` now returns a mounted path unchanged —
it must, since the file is in another process — and `chdir` had no guard. With
`V8MOUNT=/etc=sock`, `cd /etc` returned **0**, `pwd` said `/private/etc`, and
`cat passwd` read the Mac's password database. `/mnt` is safe only because this
machine has no `/mnt`. And the comment describing the gap called it
"ENOTDIR-or-worse", which is the wrong half of its own sentence: nothing
errored at all.

**One was found by predicting it.** The mount parser's strip loop and its
socket-path scan shared an index, so `V8MOUNT=/mnt/=sock` took the socket path
as `"=sock"` — verified by binding a server to a socket literally named
`=sock` and reaching it. Two slashes give `"/=sock"`, a mount that silently
does not exist, which is precisely the mode the parser's own comment says was
moved to parse time to avoid.

**And five were sentences.** A count of "eleven syscalls" that was nine, with
two more (`chroot`, `execve`) missing from the enumeration — counting names
while describing calls. A foot-gun comment claiming `V8MOUNT=/` would "shadow
`/bin` and the whole world", when `p9rel` requires `p[pfxlen] == '/'` and a
prefix of length 1 therefore claims **exactly `/`** and nothing under it; the
real hazard was an incoherent half-mount nobody had described, and it is
refused now. A `p9.h` claim that Twrite honours the cursor, when there is no
`do_write` at all — a trap for step 5f rather than a gap, since the client
already sends the sentinel. The negotiated `msize` read into nothing. And
`P9_NOTAG` defined and used nowhere, while `Tversion` sent tag 1.

Plus one real UB: `do_seek` computed `base + off` and *then* tested for
negative, in the guard written to prevent exactly that class, reachable from an
ordinary `lseek(2^62, SEEK_CUR)`.

### Two instruments were wrong, and one of them was the deadline

**`perl -e 'alarm N; exec @ARGV'` does not bound a V8 program**, and the client
section hung for twenty-six minutes proving it. The alarm mechanism is fine —
measured, `sleep 10` under `alarm 2` dies at two seconds — and there are two
independent reasons it does not help here. **V8's `sh` catches SIGALRM on
purpose**: `src/cmd/sh/fault.c:123` is `setsig(SIGALRM)`, because the shell
uses `alarm(2)` itself for `$TIMEOUT` and the fork retry. And **a deadline on
one end of a pipe is not a deadline on the pipe** — CLAUDE.md already records
that from the ttyld harness — so even against a program that dies,
`sh -c 'cat < /mnt/f'` leaves `cat` holding the stdout a command substitution
is reading. The wrapper now forks, puts the child in its own **process group**,
and kills the group.

**And V8's `cat` is not an instrument for read errors.** `while ((n = read(fi,
buf, BUFSIZ)) > 0)` — an error is not `> 0`, so the loop ends and `cat` exits
**0**. Two cases written to assert a failing directory read both passed for
that reason, *including the passthrough control*, which is what gave it away.
The property that actually matters is the bytes, and it is the bytes that are
asserted.
