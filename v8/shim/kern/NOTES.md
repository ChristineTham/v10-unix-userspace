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

## §8a step 5e, second pass: a client probe, and what it found

The client section above drives `p9cl.c` through the **shipped binaries** —
`cat`, `ls`, `tail`, `wc`, `sh`, `chmod`. That is the right headline claim and
it is why those cases are written that way. It left three paths with no case at
all, because nothing in this tree performs them, and `p9_t_fstat`'s comment said
so out loud: fstat on a directory descriptor, `lseek` in all three whences, and
`dup` sharing one offset.

`tests/streams/p9clprobe.c` is the shim's own sources linked into a host binary
— the shape `tests/v8sys/test.c` already has — run under the same mount, jail
and deadline every program above got. **The Makefile builds it, not `run.sh`**,
because the other four probes in that suite link one archive the script can name
and this one needs `$(SHIM_SRC)`; a second copy of that wildcard in a shell
script is the two-lists-that-must-agree shape `kmem.c`'s one-table rule refuses.
It links the shim and **not** `libv8kern` — 56 collisions, 25 silent, and the
kernel is in the other process, which is the design.

Five things came out of it, and only the first was the one it was written for.

### 1. The vacuous guard, and the observable nobody had used

Deleting `p9_t_fstat`'s `st_size` override left the suite green last session.
The code is not dead: it is `dir.c:114`'s rule, and every existing reader loops
to EOF and never looks. What was missing is that **nothing had ever asked both
questions**. `stat` reports what the image charges and `fstat` what `read(2)`
will produce, and the observable is that the two *differ*.

**And the pair recorded in the comment was a measurement of no directory at
all.** It said "64 bytes on the image against 768 of records". 768 is three
records and belongs to the **subdirectory**; the root has four entries, so its
pair is 64/1024. Neither number is wrong alone and the sentence is
arithmetically impossible — 64 bytes of 16-byte entries cannot be three records
— which is the tell, and a pair of plausible numbers reads as one measurement.

So the case asserts a **ratio** over **two** directories of different sizes:
`stat/16 == readable/(V8_DIRSIZ+2)`, the same count in two units. No transcribed
pair satisfies that by accident. The mutation now fires on five cases.

### 2. `ENOENT` where V7 says `ENOTDIR`

A short Rwalk carries no errno. `namei` has two answers one line apart and so
does this server — `do_walk`'s `if ((ip->i_mode & IFMT) != IFDIR) u.u_error =
ENOTDIR` at `v8fsd.c:699` — but the reply cannot say which, so `p9walk`
flattened both to `ENOENT`.

The information is in the qids the reply carries, which `p9walk` was discarding:
components before `got` succeeded and the one **at** `got` did not, so the last
qid describes what the failed component was looked up *in*. Not a directory
means `ENOTDIR`.

**Three cases, not two.** A missing name at the top is an *Rerror* and already
carried the server's own errno; the other two are short walks on the same code
path and must come out differently. The middle one — a missing name inside a
real subdirectory — is the discriminator, and both one-sided mutations fire on
exactly one case each.

### 3. A transport leaking its signal semantics into the filesystem

Found at one remove, which is the argument for the sanitized server below: when
that server aborts, the probe came back **141 = 128 + SIGPIPE**. The connection
is a socket and the caller is a V7 program that has no idea it is one, so a
server that died mid-conversation *killed* `cat` instead of failing its read. On
a real V8 a disk that stops answering is `EIO`.

`p9dial` sets `SO_NOSIGPIPE`, and **per-socket is the whole reason** rather than
`signal(SIGPIPE, SIG_IGN)`: ignoring the signal changes the program's own
disposition, and a V8 program in a pipeline must still die when its reader goes
away — that is how `yes | head` terminates. `rawsys5()` is new in `rawsys.h` for
it; the gap between 4 and 6 was not a decision, nothing had taken five
arguments.

The case produces the condition with **`shutdown(2)` on the client's own
descriptor**, which puts it in exactly the state a dead peer would: no second
server, no process to kill, no timing. The assertion is that the probe reaches
its last line.

### 4. A mutation that would not fire, for a third reason

`do_seek`'s overflow guard tests before adding. Reverting it to the auditor's
original — add, then test for negative — left all 525 cases green, and this is
**neither** of the two reasons a mutation usually survives. The case is not
vacuous and the code is not dead: both operands are in `[0, LLONG_MAX]`, so
every reachable overflow wraps to a negative value and the broken form lands on
the same `EINVAL`. It gets there by executing undefined behaviour, which
licenses the compiler to delete the check.

`$(BUILD)/v8fsd/v8fsd-ubsan` is the same server under
`-fsanitize=undefined -fno-sanitize-recover=all`, and the suite runs the probe,
`ls -l` and a 28000-byte `cat` through it. Current code: silent. Guard
reverted: `signed integer overflow: 4611686018427387904 + 4611686018427387904`
and the process dies. **Only our own code is instrumented** — `libv8kern.a` is
already compiled, so Bell Labs' 1985 kernel is linked in uninstrumented, and UB
in imported source would be a different project.

### 5. Two tables that cite each other and were never compared

`v8fsd`'s `errnames[]` and `p9cl.c`'s `enames[]` are the two halves of one seam,
each file's comment citing the other. The suite now compares them as text, and
**the direction is not symmetric**, so it is two cases and not a `diff`: a name
the server can send that the client does not know falls into `enumber()`'s
`EIO` fallback, which is documented for a *foreign* server's prose and is
therefore silent. The values are deliberately **not** compared — `ENAMETOOLONG`
is `V8_ENOENT` and `ENOTEMPTY` is `V8_EEXIST` because V7 has neither, so a value
comparison would fail on exactly the entries whose collapse is the considered
answer. What has to agree is the name set.

`streams` 466 → 535; nine mutations, nine fire.

---

## §8a step 5f — the mount is writable, and three things were hiding behind one

Twrite, Tcreate, Tremove, Twstat and Taccess are implemented; `v8fsd -r` mounts
read-only. `sh -c 'echo x > /mnt/fresh'` creates a file on a disk image through
Bell Labs' `namei`/`ialloc`/`writei` in another process, `cat` reads it back,
`rm`, `mkdir` and `rmdir` work, and `icheck`, `dcheck` and `fsck` — three
readers that know nothing about 9P — pronounce the result clean with the block
count back to exactly what it was.

### The atime finding, re-measured, and it was three accidents rather than two

The task carried a measured claim: `readi` sets `IACC` (`rdwri.c:50`), so `iput`
at `i_count == 1` runs `IUPDAT` and dirties the disk inode, and *two* accidents
hid it — `O_RDONLY` on the image fd, and nothing calling `bflush()`. Re-measured
with three scratch builds of the server differing only in the open mode and the
flush, plus an `imgdev.c` instrumented to print every `pwrite` with a flag
comparing what is about to be written against what is already at that offset:

- **`O_RDONLY` was doing no work at all.** With `O_RDWR` and no flush, a small
  read, a 28000-byte read and twenty more reads produced **zero** pwrites. The
  `bdwrite`'d buffer sits in the cache and `NBUF` is never exhausted, so it is
  never recycled. There was one accident, not two.
- **AND THE WRITE IS INVISIBLE ON A PRISTINE IMAGE, WHICH IS THE THIRD.**
  `time` is set once, by `iinit` from the superblock's `s_time`, and nothing
  advances it — upstream's clock interrupt is `sys/clock.c`, about a VAX
  interval timer, and is not imported. `mkfs` writes `di_atime == di_mtime ==
  di_ctime == s_time` on every inode. So `dp->di_atime = *ta` stores the bytes
  that are already there: the driver prints the write, `cmp` on the image
  prints nothing. Perturb one `di_atime` first and **exactly four bytes move**
  (offsets 2229-2232, inode 3's `di_atime`), with `same=0` on the first write
  and `same=1` on the second.

That is the round-trip class — `ttldioc`'s ten bytes, `strncat`'s overread —
arriving in an artefact rather than in memory: **the write is real, the result
is correct, and a byte comparison of the thing written cannot see it.**

The recorded claim was right in substance and its measurement had been taken on
a deliberately perturbed image, which is why it saw four bytes. What it did not
say is why nobody had ever noticed on an ordinary one.

### The frozen clock is a live defect the moment anything writes

Every `mtime` and `ctime` a create or a write laid down would have been the
moment `mkfs` made the image. `v8fs_clock()` in `v8fs.c` is the other half of
the substitution `iinit` already makes for `clkinit`, and `serve1` calls it once
per request — which is the right grain rather than a compromise, since a request
here *is* a syscall and nothing can run between two of them. It is on the read
path too, deliberately, because a read stamps an atime.

**A raw `gettimeofday`, not `time(3)`**, because `tests/kmemu` asserts
`libv8kern.a` imports exactly `_memcpy`, `_setjmp` and `_longjmp`. `rawsys.h`
was already included in `v8fs.c` and had no consumer until now.

### "Read only" is a mount flag, and Bell Labs wrote both lines

`iinit(int ronly)` and `v8k_kinit(dev, ronly)`. `fsmount()` at `sys/sys3.c:299`
and `:316` is the general form of what upstream's `iinit` hardcodes for the
root:

	(*bdevsw[major(dev)].d_open)(dev, !uap->ronly);
	fp->s_ronly = uap->ronly & 1;

With it set, `iupdat` returns at `iget.c:248` before it breads anything and
`access()` refuses `IWRITE` through the arm §8a step 5d restored. So a
read-only mount cannot move an atime — a guarantee the old `rerror(EROFS)` in
the dispatch never gave, because that was a boundary in the *protocol* over a
filesystem that was never read-only at all.

`kinit` takes it as an argument because there is no `mount(2)` here: the one
mount this kernel ever makes is the one `kinit` makes, so what `fsmount` reads
out of `u_ap` has to arrive as a parameter.

### Four things that were wrong in the writing, and how each was found

- **`iupdat(ip, &time, &time, 1)` PASSES THE ADDRESS OF libc's `time()`.**
  `hostok.h` gives thirteen names back to the C library so `v8fsd.c` can have
  host headers, and `time` is one of them — so upstream's own line, transcribed
  verbatim into `kmkdir`, would hand `iupdat` a function pointer as a `time_t *`
  and write four bytes of instructions into an inode as a timestamp. It
  compiles, because `iupdat` is declared K&R. Fourth instance of the `hostok.h`
  class after `access`, `free` and `ialloc`, and the first where the wrong thing
  is a **variable** rather than a call. Spelled `v8k_time`.
- **`itrunc` ALREADY DOES WHAT A DRAFT ASSUMED IT DID NOT.** `do_open`'s OTRUNC
  arm was written as `itrunc(ip); ip->i_size = 0; ip->i_flag |= IUPD|ICHG;` on
  the theory that itrunc only frees blocks. `iget.c:349` is `ip->i_size = 0`,
  and the three lines above it say the inode has already been written and the
  flags already updated — itrunc writes a **zeroed copy** synchronously first,
  so that a crash mid-free leaves harmless missing blocks rather than a
  duplicate claim, and then clears `IUPD|IACC|ICHG` on purpose. Setting them
  again would undo exactly that reasoning. `open1` at `sys2.c:200` calls
  `itrunc(ip)` and nothing else, and so does this now.
- **`do_remove` USED THE ROOT AS THE PARENT.** A Tremove carries a fid and
  nothing else; V7's unlink names a *directory* and an *entry*, and nothing in
  a `struct inode` bridges that — `..` is an entry, so it exists for a directory
  and not for a plain file. The first version took `rootdir` for a plain file,
  which is right only for names directly under the mount: `rm /mnt/newdir/f`
  asked the server to unlink `f` from the **root**, found nothing, and reported
  a failure `rm` swallowed. Fixed with `f_pino`, recorded during the walk — a
  number rather than a pointer, so a fid holds no reference to balance, and
  zero for `.`/`..`/attach/clone so that Tremove refuses rather than guessing.
  **Found by running it**: the walk and the remove are in different functions
  and each reads correctly alone.
- **AND THE TARGET MUST BE `iput` BEFORE THE REMOVE.** `nami.c`'s NI_DEL arm
  does its own `iget` on the entry and then `iput`s it, and it is *that* `iput`
  at `i_count == 1` which sees `i_nlink` reach 0 and runs `itrunc` and `ifree`.
  A fid still holding the inode keeps `i_count` at 2, so the blocks never come
  back — one leaked file per remove, invisible to any reader and loud in
  `icheck`. The step-5d lesson about independent checkers, arriving before it
  could bite.

### mkdir(1) is mknod plus two link()s, and the guard was in the way

V7 has no `mkdir(2)`: `mkdir(1)` is `mknod(d, IFDIR|mode, 0)` and then `link`
of `d/.` and `d/..`, which is why it was setuid root. Neither macOS nor a v8fs
mount works that way — the host's `mkdir(2)` writes both entries, and so does
`NI_MKDIR` plus the eleven lines of `sys2.c:246-256` that `kmkdir` transcribes.
`syscall.c` already had the arm that makes those links succeed-and-do-nothing;
it sat *below* `MOUNTED()`, so `mkdir /mnt/d` created the directory correctly
and then printed `cannot link /mnt/d/.` and exited 1 — a failure message about
a completed operation, which is the worst of the three possible answers. The
arm moved above the guard unchanged, because it was already the same statement
about a different filesystem.

`kmkdir` is worth reading beside upstream for one detail that reads like a bug:
`nmarg.ino` comes back holding the **parent's** number, because `nami.c` does
`flagp->ino = dp->i_number` while `dp` is still the parent and only then
`dp = dip`. So `x[1]` — `..` — takes it. Upstream's two lines in upstream's
order, because reasoning about which is which from the names gets it backwards.

### Taccess — the second extension, and the same sentence as Tseek

9P has no `access(2)` for the reason it has no seek: Plan 9's kernel decides
permission when it opens the file and has no `access(2)` to ask in advance. V7
does, and `test -r` is an ordinary program in this world.

`v8s_access` has now had three answers and the first two were both left standing
above it, one contradicting the other — the stale-comment class arriving as a
*pair*, where the rewrite added its reasoning and did not remove what it
replaced. (1) Recomputed locally, from the image's mode bits against the host's
uid: wrong on every file of every image. (2) Reported what would happen —
R_OK from a stat, W_OK always `EROFS`: right while the server refused every
write, and its own comment said the day 5f arrived it should be asked over the
wire instead. (3) Asked.

**And it answers V7's answer, which is not BSD's.** `fio.c:193` is
`if(u.u_uid == 0) return(0)` with no `0111` special case, so `test -x` on a
0644 file says **yes**. A case was written expecting `no` and the code was
right. What the old `EACCES` was reaching for is real but belongs elsewhere:
nothing can be *executed* off a mount, because `v8s_execve` is passthrough —
a fact about execve rather than about the file, and no live caller asks, since
V8's `sh` searches PATH by calling `execve` on each directory.

### The uid question, settled by a note that was already written

The server runs as root and a client cannot say otherwise. `main.c:370-379`
had already argued it: folding the host's uid in would make `access()` compare
against inodes `mkfs` wrote as uid 0, so **whether a write was permitted would
depend on who ran the test** — the host-property class arriving through a
u-area field. What 5f adds is that the mount now has a *real* read-only flag,
so the interesting refusal is available without inventing an identity.

### The test suite: three servers stopped sharing a mutable artefact

`sock`, `csock` and `ubsock` all serve `$FSTMP/p9img` and all three now run
`-r`. Before 5f they shared it "read-only by assumption"; now the superblock
says so and the fd is `O_RDONLY` as well, which turns the contamination hazard
into a guard — and every existing expectation is unchanged, because a
read-only mount refuses exactly what the protocol used to.

The writable section gets its own copy and its own server, and a **second**
server `-r` on a second copy so the pair differs by one argument.

Three of its cases were wrong on the first draft and each is a known shape:

- **The read-only pair was VACUOUS**, because the first draft used `csock` —
  killed a hundred lines above. Every "refused" was really a dial that could
  not connect. The tell was `rm` exiting **0** where a refusal must exit 1: a
  dead server and a strict one do not fail the same way.
- **`ls -l` CANNOT MEASURE A CLOCK THAT ADVANCED SECONDS AGO.** Its output is
  minute-granular and the section runs seconds after `mkfs`, so the new file
  and the image both read `Aug 10 18:13` whether or not `v8fs_clock` existed.
  A test whose resolution is coarser than the effect it measures is not a test.
  It compares the superblock's `s_time` before and after instead — `update()`
  writes `fp->s_time = time` whenever `s_fmod` is set — at `SB+216`, the offset
  `tests/mkfs` already uses, and asserts only `before < after`.
- **`rm`'s EXIT STATUS IS NOT AN INSTRUMENT**, which is the `cat`-and-read-errors
  lesson with a second member. On a file it may not write, V7's `rm` *asks*,
  gets no answer from a non-tty, and returns having done nothing and set no
  error: exit 0. With `-f` it skips the question, the unlink fails with `EROFS`,
  and `fflg` suppresses both the message and the count: exit 0 again. Both are
  correct `rm`. Assert the file.

And one expected value was a guess dressed as a measurement: the block count
was asserted as `pristine + 4`, came out one off, and had no way to say which
number was wrong. The section removes everything it made and asserts the
**identity** instead, which is the shape `fsprobe`'s own round-trip case uses.

`streams` 535 → 567; eight mutations, eight fire.

### And two build edges that did not exist, found by adding the case for one

`struct v8fstyp` grew three slots, so every object that compiles against
`vfs.h` had to be rebuilt — and `tests/deps` had a case for three of the four
implementations. `p9cl.c` had none, purely because it was written after that
block. Adding it found something worse than a missing case: **the two rules
that compile `$(SHIM_SRC)` straight into a host binary — `$(BUILD)/v8sys/test`
and `$(BUILD)/v8sys/p9clprobe` — listed no headers at all.** Every ordinary
object under `$(BUILD)/v8sys` gets its header edges from `$(DEPFLAGS)`; these
two never produce a `.d`, so their edges have to be written out and never had
been.

What a stale header costs here is not a build failure. It is a **table with the
wrong number of entries**: `v8fs_p9.t_access` becomes whatever field sat at
that offset in the older layout, the binary links, and it dispatches into the
wrong function. That is not hypothetical — it is exactly what happened by
accident an hour earlier when a `git stash` baseline measurement left
`syscall.o` stale, and the only place the truth showed was a server trace
showing `Tstat` where the new code sends `Taccess`.

`$(SHIM_HDR)` is four named headers rather than a wildcard, and the recipes
changed from `$^` to `$(filter %.c %.s,$^)` so the headers are prerequisites
without becoming compiler inputs. `deps` 353 → 357.

## §8a step 5f-b — chmod, chown and utime, which are one message

The last three of the fourteen slotless syscalls that had a 9P answer. They are
**one Twstat between them**: a wstat carries a whole stat and the server applies
whichever fields are not the "do not touch" sentinel, so `p9_t_chmod` sets
`s_mode`, `p9_t_chown` sets `s_uid`/`s_gid`, and `p9_t_utime` sets
`s_atime`/`s_mtime`, over one wire format. What is left refusing is `link` and
`symlink`, which 9P2000 has no message for at all.

`streams` 567 → 592, `make test` 2060 → 2085. Eight mutations, eight fire.

### Reading upstream first found three deviations and one missing arm

The server's `do_wstat` had been written from a recalled citation. Reading
`sys4.c` and `fio.c`:

| | upstream | do_wstat as it stood |
|---|---|---|
| `chmod` | `sys4.c:238`, gated on `owner(1)` | `suser()` |
| `chown` | `sys4.c:282`, `!suser() \|\| owner(1)==NULL` | `suser()` — **right** |
| `utime` | `sys4.c:521`, `owner(1)`, **both** times, `IACC\|IUPD\|ICHG` | `suser()`, mtime only, no ICHG |
| sticky | `sys4.c:250`, `if (u.u_uid) fmode &= ~ISVTX` | absent |

`owner()` is `fio.c:215-228` and its rule is **ownership OR superuser**. The
comment above the function said exactly that — "ownership for everything else"
— while the code tested `suser()` alone, and cited `sys3.c`, which is
`fsmount`. None of it is observable, because `u_uid` is 0 here and both rules
therefore always permit; **that is why the sentence and the line could disagree
for a whole step**. `wowner()` is the rule written out, and the `||`
short-circuits in upstream's order because `suser()` has a side effect
(`u_acflag |= ASU`).

### An arm declined for a reason that was true and never covered the case

`s_atime` was not honoured, and the recorded reason was: *"nothing in this
world sets atime alone, and an unexercised arm is a claim nothing can check."*
Still true. It never covered the consumer that turned up.

`mv.c:129` is `utime(target, &s1.st_atime)` — the address of one `struct stat`
field, relying on `st_atime`/`st_mtime` being adjacent `time_t`s to pass a
`time_t[2]` — and on a mount it is not an unusual path but the **only** path,
because `link(2)` has no slot and is refused, so `mv` always falls through to
fork, `/bin/cp` and utime. **Declining an arm because nothing sets a field
ALONE is a different claim from nothing setting it at all**, and only the second
would have been a reason. Before the arm, `mv` on a mount copied and unlinked
correctly and silently lost both timestamps.

The two times are one arm now, because `iupdat` is one call: it writes
`di_atime` when IACC is set and `di_mtime` when IUPD is set (`iget.c:272-275`)
and there is no way to ask for one, so the field the client did not send is
re-written with the value already on the disk. `ICHG` goes with them, which
this arm did not set — `sys4.c:536` is all three, and the comment four lines
above it, "Can't set ICHG", means the caller cannot *choose* a ctime, not that
ctime stays put.

### The two ends of one wire, an hour apart, and only the reading end had the guard

`do_wstat` parsed the owner with `atoi`, which has no error return. Measured
over a real Twstat before the fix:

| sent | resulting `i_uid` | reply |
|---|---|---|
| `"nobody"` | **0 (root)** | Rwstat |
| `"--"` | **0 (root)** | Rwstat |
| `"12x"` | 12 | Rwstat |

0 is root, and root is the identity `fio.c:193` lets bypass every permission
check on the image — so an unparseable owner was not a wrong answer, it was a
privilege grant with a success reply. **The client end of this same field
already had the guard**, given to `p9uid` by an earlier audit, with the contract
spelled out beside it: *root maps to root, and non-root never maps to root.*

It matters because plain 9P2000 specifies the field as a **name**, and
`statof`'s own comment acknowledges that and sends a number anyway. A
conforming foreign client doing a wstat sends `"chris"`.

**Range is not parseability, and only the second is guarded.** `"65536"` is
accepted and truncates, because that is V7's own answer — `sys4.c:294` is
`ip->i_uid = uap->uid`, an int into a short, unchecked. A leading `-` is
accepted too, because `statof` renders `i_uid` with `"%d"` of a signed short and
a negative owner is a value this server itself emits.

Nothing could have caught it: `do_wstat` had **no client caller at all** until
`p9_t_chown`, which is this step. The case for it is a new `-w` mode in
`p9probe`, because no V8 program can send a name — `p9_t_chown` formats an int.

### Two hand-rolled copies of one length patch

`do_stat` spent eight lines writing 9P2000's outer count and patching it
afterwards, with a comment explaining the wart. The client's Twstat needed the
same eight lines. `p9_pstatw()` is the one definition, and `p9_nostat()` is
beside it — the all-ones initialiser, which is a function rather than a
`memset` of `0xff` because **the strings are the asymmetry**: all-ones has no
spelling in a string field, so "do not touch" there is the empty string, and
filling the struct with `0xff` would send four 255-byte names.

### Four dead declarations kept cover on one line with two live ones

`int iinit(), binit(), bhinit(), ihinit(), update(), brelse();` — the first four
have no call site in `v8fsd.c` (`v8k_kinit` calls them), and the paragraph
directly above the line says *"a declaration with no call site is an unconsumed
component"*. The rule was being stated and broken in adjacent lines. The one
that mattered is `iinit`, because **5f changed it**: `main.c:304` is
`void iinit(int ronly)` and the declaration still said `int iinit()`. Deleted
rather than corrected — the fix for an unconsumed declaration is not a better
declaration.

### EROFS is a claim about the medium; EPERM is a claim about the operation

`v8s_mknod`'s device arm said *"the refusal is the same one the host arm
gives"*, and it was not: a `MOUNTED(p)` made the mounted answer **EROFS** and
the host answer **EPERM**. EROFS says the filesystem will not take writes, which
stopped being true in 5f; EPERM says the operation is meaningless, which is the
actual reason and is true of both worlds. The macro could go because that arm
never touches the path — it sets errno and returns, so there was nothing for the
guard to contain. `procfs.c`'s three new slots record the same distinction from
the other side: `/proc` refuses chmod/chown/utime with EPERM and not EROFS,
because `/proc` very much does take writes and it is these three operations that
are meaningless there.

### What the tests learned

- **`ls -l` is minute-granular**, so it cannot show that a utime worked when the
  file was written seconds ago. A **1991** date is a difference it can show, and
  `ls -lu` is what makes the atime arm's case distinct from the mtime arm's.
  Order matters: the `cat` two cases later moves the atime, which is then
  asserted as a *relation* (no longer the value we put there) rather than as
  today's date.
- **A case can be vacuous for two independent reasons.** "chmod cannot change
  the file type" on a plain file passed under every mutation: `ls` switches on
  `st_mode & S_IFMT` pre-set to `-` at `ls.c:336` and not overwritten by the switch at `:354`, which has no `default` arm, so a mode with no
  type bits still prints as a plain file — *and* `p9tostat` rebuilds the type
  from `DMDIR` rather than passing the server's IFMT through, so `ls` on a
  mount cannot see the server's mode word at all. The **directory** is where it
  is observable, because `statof` sets DMDIR from `(i_mode & IFMT) == IFDIR`.
- **The read-only server has to still be up.** The containment cases were first
  written after `kill $RPID`, which is precisely how §9f's read-only pair was
  vacuous until an `rm` exiting 0 gave it away. Both servers now live across the
  whole section, and the byte-identical `cmp` moved below it so it covers 5f-b's
  writes too — a refused wstat that had already dirtied an inode shows there and
  nowhere else.
- **A containment case survived by testing a better property.** "chmod through a
  mount does not reach the host" was written when chmod had no slot, and the
  guard was the refusal. chmod is a slot now and the call goes to the server —
  and the property being asserted is the one that always mattered: *a mounted
  path never reaches the host's chmod.* Measured: revert the dispatch to
  `rawsys2(SYS_chmod, vpath(p), m)` and it fires, along with two others.
- **The mutation harness had the same-second trap on the APPLY side.** The
  documented one is the restore side. Here the previous restore's rebuild
  finished in the same second the next mutation was written, so `make` declared
  the artefact current and two runs were meaningless — caught by the harness's
  own artefact-hash check, which is the only reason they were not read as "the
  guard did not fire". `v8fsd` is a binary rather than an object, so
  determinism was measured first (two builds of identical source, identical
  hash) before it was used as the check.
- **And the harness littered the repo root**, because a script `exec`'d from
  stdin has no `__file__` worth trusting. `tests/deps` caught it.

## Filling the image killed the server, and two defects were hiding each other

Found by the `lp64-auditor` on the 5f/5f-b diff, reproduced here before being
believed. `cat big > /mnt/x` on a 200-block image:

```
/: file system full

/: write failed, file system is full
panic: tsleep: no device below, and no timeout
```

Server exit 2, every client's connection dropped mid-transaction — not just the
writer's. Reachable from an ordinary unprivileged program. The image survives
(`icheck` and `fsck` clean afterwards), so it is **availability, not
corruption**.

### The chain, and why every link looked right

Bell Labs' out-of-space path is a kludge and they label it one in capitals:

```c
nospace:					/* alloc.c:187-195 */
	fserr(fp, "file system full");
	/* THIS IS A KLUDGE... */
	for (i = 0; i < 5; i++)
		sleep((caddr_t)&lbolt, PRIBIO);
	u.u_error = ENOSPC;
```

`v8fs.c`'s `sleep()` maps onto `tsleep(chan, pri, 0)`; `tsleep`'s third
argument is **seconds**, and `seconds > 0 ? seconds*1000 : -1` turns 0 into "no
timeout"; `slp.c` then panics on `ndrvfd == 0 && timeout < 0`, and `v8fsd`
registers a *block* driver so `ndrvfd` is 0.

**Every one of those is correct for the caller it was written for.** The
panic's own comment reasons entirely about streams — *"a test's mistake, or a
driver that forgot `v8k_drvfd()`"* — and that was a true and complete account
of every caller in existence when it was written. This is a **second consumer
arriving at a guard argued for the first one**, which is the shape this file
records more than any other.

### The survey that should have caught it listed everything except the one that fires

`v8fs.c`'s `sleep()` comment enumerates the sleeping callers as *"iget.c:93 and
alloc.c:89,215,295 … waiting for a locked inode"* and attributes the PRIBIO
waits to "bio.c's". All four PINOD waits are **unreachable in a
single-threaded server** — nothing else is running to hold a lock.
`alloc.c:194`, the only one that can fire, is in neither list.

### The fix is the channel, and it is provable rather than probable

Two greps settle it:

- `alloc.c:194` is the **only** sleeper on `lbolt` in the imported tree.
- `clock.c:290` is the **only** waker of `lbolt` in the whole 18k-line kernel —
  the clock interrupt handler — and this port has no clock interrupt and does
  not import `clock.c`.

So a sleep on `lbolt` here can never wake. That is the same *form* of argument
`slp.c`'s panic makes about a stream with no device, reaching the opposite
verdict because the caller is different. `sleep()` returns immediately for that
one channel.

**Returning is not a semantic change**, because the wait is futile by
construction: upstream sleeps hoping *another process* frees a block, and here
the caller is the only thing running, so the loop is guaranteed to fall through
to ENOSPC. Same observable, without five seconds of dead time in a file server.
What is *not* safe is a future caller that sleeps on `lbolt` inside a condition
loop — `alloc.c`'s is a bounded `for` — and there is no way to detect that here,
so it is written down instead. Re-run both greps after importing more of `sys/`.

### And the second defect only became reachable once the first was fixed

`kmkdir` writes `.` and `..` with `writei` and never consulted `u.u_error`;
`do_create` tests only `nip == NULL`. On a full image, **`mkdir /mnt/d` exited
0** and `fsck` said:

```
LINK COUNT DIR I=2  ... COUNT 3 SHOULD BE 2
LINK COUNT DIR I=47 ... COUNT 2 SHOULD BE 1
SIZE=0
```

— the parent's link count bumped for a `..` that was never written, and a
directory with no entries at all. Upstream's `mkdir()` ignores the same return
and **can afford to**: it *is* the system call, so `u_error` reaches the user
and `mkdir(1)` prints something. Here the value died in the wrapper.

So this reports rather than unwinds. The damaged directory is not the defect —
it is exactly what a V7 kernel leaves when that write fails, and what `fsck`
exists to repair. The **success reply** was the defect. `u_error` is saved
across the `iput`, because a filesystem that has just refused a write will
likely refuse the inode flush too, and the second failure would otherwise
overwrite the first.

The two hid each other exactly as the audit predicted: the panic fired first, so
nothing ever reached the `mkdir`.

### The test, and which case is the guard

Its own 200-block image and its own server, because the point is to exhaust the
free list and every other case depends on the shared image's accounting being
exact. Five cases; `streams` 592 → 597.

**"The write fails" is not the guard.** It passes whether or not the server
survives, because a server that dies mid-write also looks like a failed write
from the client. The guards are *the server is still alive* and *it still
answers a read* — measured by mutation: reverting the `lbolt` guard fires those
two and leaves the write case green.

And the image is deliberately **not** asserted clean afterwards, only still
*readable* by `icheck` — a server that died mid-write could have left a
superblock nothing can parse, and that is the thing worth ruling out.

## The open mode was checked at open and never again

The third of the auditor's findings, and the smallest to state: V7 re-checks
what a file was opened for on **every** transfer, in one line —

```c
	if((fp->f_flag&mode) == 0) { u.u_error = EBADF; return; }	/* rdwr() */
```

— and this server had one third of it. `do_read` checked only that the fid was
open at all; `do_write` refused `P9_OREAD` and not `P9_OEXEC`. Measured by the
auditor against a real server: a fid opened `0x03` (OEXEC) accepted a Tread
**and** a Twrite, and `0x13` (OEXEC|OTRUNC) ran `itrunc`. The plain half is
reachable by an ordinary program — `open("/mnt/f", 1)` then `read()` returned
the bytes.

### OEXEC reads and does not write, which is 9P's rule and not a choice

`open(5)` gives mode 3 as *"execute (read, but check execute permission)"*,
because Plan 9's kernel has to read a binary in order to run it. So the two
gates are **not complements**:

| mode | read | write |
|---|---|---|
| OREAD (0) | yes | no |
| OWRITE (1) | no | yes |
| ORDWR (2) | yes | yes |
| OEXEC (3) | **yes** | no |

`canread()` is therefore "anything but OWRITE" and `canwrite()` is "OWRITE or
ORDWR", and each direction needs a refusal case *and* a success case — a server
that simply refused OEXEC outright would pass a refusals-only suite.

### What this does NOT fix, said out loud

`u_uid` is 0, so Bell Labs' `access()` takes `fio.c:193`'s root bypass and
grants IWRITE on every file of every image. So the **permission** dimension is
moot, and the `OEXEC|OTRUNC` truncation the auditor measured happens before this
change and after it. Folding OTRUNC into `want` is right — `open(5)` says
truncation requires write permission "even if the mode is OREAD", which is also
`open1`'s reading — and it collapses the `mntronly` arm from two spellings of
one rule to one. But it changes no answer today, in the same way `wowner()`
does not, and the comment says so rather than implying a fix.

What *is* live is the gate, which is a property of the **fid** rather than of
the identity, and wrong at any uid.

### Only a probe can ask

The client opens with V7's `O_RDONLY`/`O_WRONLY`/`O_RDWR`, has no spelling for
OEXEC at all, and would never write to a fid it opened for reading. Every case
here is a message no client of ours emits — which is the class `p9probe` exists
for, and the second time in two steps that the guard for a defect had to be a
wire-level case (the first was `atoi` on an owner name).

Ten cases; `streams` 597 → 607. Two mutations, two fire, and each fires on
exactly one case — removing the read gate reddens *an OWRITE fid cannot read*
and nothing else; reverting the write gate to its old `== P9_OREAD` form
reddens *an OEXEC fid cannot write* and nothing else.

**And the probe puts its byte back.** These cases write to `hello` on the
shared writable image, which the chmod cases, the owner control and the
round-trip accounting all read afterwards — the artefact-leak shape this suite
has met three times (between programs sharing a directory, between cases
sharing a stream, between sections sharing an image). The original byte is read
first and written back last, and the restore is **asserted** rather than
assumed, because a restore nothing checks is a restore that stops happening.

## v8s_unlink was the last path-taking call that did not dispatch

The fourth of the auditor's findings and the only one with **no observable at
all** — which is the reason to record it rather than to skip it.

It tested `v8fs_mounted(p)` and reached into `v8fs_p9` by name, then fell
through to a raw `SYS_rmdir`/`SYS_unlink` on a `vpath()`-resolved path. Every
sibling added by 5f and 5f-b goes through `FSFOR(p)` and `t->t_path(...)`. Two
consequences, both structural:

- **`pt_remove`'s unlink arm had no caller anywhere.** `v8s_rmdir` always passes
  `isdir = 1`, and `v8s_unlink` never dispatched. Same for `pr_remove`'s and
  `/dev/fd`'s.
- **`unlink()` on a `/proc` path reached the host verbatim**, where `rmdir()` on
  the same path got the type's answer. Two operations on one path, two worlds.

This is the recorded `v8s_creat` shape exactly: *path resolution without
dispatch, so no second type could ever see it.* And nothing misbehaved, because
`pt_path(p, V8P_LOOK)` **is** `vpath(p)` and the two roads met — which is
precisely why an auditor found it and no test did.

### What moved, and what deliberately did not

The macOS reasoning moved into `pt_remove` unchanged: V7's `unlink(2)` removes
an entry of *any* kind if the caller is privileged enough (that is how `rmdir(1)`
works, and why it was setuid root), and macOS refuses a directory outright, so
the choice V7 declines to make has to be made somewhere. That somewhere is the
passthrough type, because it is a fact about the **host** filesystem. A mount
needs none of it — the server's `suser()` decides and `nami.c`'s NI_DEL arm
performs it — so `isdir` stays `-1`, "no opinion", which is exactly what
`unlink(2)` has.

The dot-entry block stays **above** the dispatch and untouched. `rmdir(1)` is
`unlink("d/..")`, `unlink("d/.")`, `unlink("d")`, and an earlier version of this
function had that arm on the wrong side of the guard, which made `mkdir(1)`
print "cannot link /mnt/d/." after successfully creating the directory.

### No new case, and that is the disciplined answer

The change is behaviour-identical today: passthrough resolves the same way it
did, and the `/proc` difference is invisible because only a groveler links the
real `procfs.c` and no groveler unlinks anything (`SHIM_SRC` gives
`tests/v8sys` `noprocfs.c`). A case written for it could not fail, and this
repository's own rule is that a mutation which does not fire means the case is
vacuous.

**What does have coverage is the arm the move activated**, and mutation says so:
delete `pt_remove`'s `isdir < 0` stat-then-choose and `tests/jail`'s *"rmdir(1)
removes it from the jail"* goes red — because V7's `rmdir(1)` ends by unlinking
a **directory**, which macOS will not do. One case, and it was already there.
