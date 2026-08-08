# Porting notes: dump(8), restor(8) and dumpdir(8)

PLAN.md §8a step 4f. The last three of the ten programs that section writes off
as "raw VAX disks", and the only ones of the ten that are not filesystem tools:
a *dump* is a tape format, not a disk format, so these three answer to a
different 1985 record than `mkfs` and the checkers do.

`dump` is six files and a private header; `restor.c` is 1158 lines and
`dumpdir.c` 493. One file records all three because every interesting fact is
about the format they share.

*One `PORTING.md` in `dump/` rather than three, and `restor`/`dumpdir` are bare
`cmd/*.c` so they have no directory of their own — the same arrangement
`icheck.PORTING.md` uses for its three.*

## What upstream ships that we deliberately did not import

`third_party/.../cmd/dump/dump` is a **1985 VAX `a.out`** — magic `0413`,
demand-paged pure executable, 30275 bytes, left in the source directory by
whoever last built it. `tools/import.sh` brought it in with everything else and
it was removed by re-importing the directory file by file.

It is not fidelity to keep it. It is a build artefact, and its name is the
makefile's target, so a rung-5 test that copies `src/cmd/dump/*` into a scratch
directory and then asks `[ -x dump ]` would be satisfied by a VAX binary that
cannot run here — a case passing vacuously, which is the failure `tests/cpp`
already had once. `third_party/` still has it, with its hash in the upstream
tree; nothing is lost.

## `struct spcl` is a WIRE FORMAT, and that is the whole port

`<dumprestor.h>` was imported for these three and patched in two fields:
`c_date` and `c_ddate`, spelled `int` where upstream spells `time_t`. The
argument is `<sys/ino.h>`'s, one layer out — the other end of this record is not
another program in this port.

```c
struct	spcl {
	int	c_type;		int	c_date;		/* upstream time_t */
	int	c_ddate;	/* upstream time_t */
	int	c_volume;	daddr_t	c_tapea;	ino_t	c_inumber;
	int	c_magic;	int	c_checksum;
	struct	dinode	c_dinode;
	int	c_count;	char	c_addr[BSIZE(0)];
};
```

Three things make it more than a style choice:

- **The struct already had a VAX-shaped half.** `c_dinode` is a `struct dinode`,
  fixed at 64 bytes by step 4a. At eight-byte times the header would have been
  VAX-correct from `c_dinode` onwards and shifted by eight before it, which is
  worse than either choice taken whole.
- **The record is a fixed 1024 bytes, so widening does not grow it — it slides.**
  `dumptape.c` writes from `char tblock[NTREC][BSIZE(0)]` and `taprec()` copies
  exactly `BSIZE(0)` bytes, so what reaches the tape is the **first 1024 bytes**
  of a 1124-byte struct: the 100-byte header plus 924 of `c_addr`, with the rest
  truncated by design. A widened header field pushes every later field along and
  drops eight more bytes off `c_addr`.
- **The checksum cannot see it.** `restor.c:1013` sums `BSIZE(0)/sizeof(int)` =
  256 ints over those same 1024 bytes and requires `CHECKSUM`; `dump` writes a
  compensating word so it comes out. That catches a writer and a reader who
  disagree — and here both are ours, so both would have been wrong together.
  The same shape as `mkfs`'s `-DDIRSIZ=14`: a format error that every reader we
  have agrees with.

So the layout is asserted on the numbers, from the V8 side, by
`tests/mkfs/probe.c`:

```
spcl 1124
spcloff 0 4 8 12 16 20 24 28 32 96 100
spclwords 256
```

**`struct idates` is deliberately NOT narrowed.** `/etc/ddate` is a *text* file —
`DUMPOUTFMT` is `"%-16s %c %s"` and `DUMPINFMT` its inverse — so `id_ddate`
never leaves memory as bytes and `ctime(&itwalk->id_ddate)` is correct at eight.
One struct in this header is a wire format and one is not, and that is the
entire difference between them.

## The build

All three join the Makefile's `$(IMGBIN)` group, which now has ten members, and
adding `dump` is what forced that group to learn about multi-object programs:
`$(imgsrc_dump)` is six sources, `$(imgobjs)` collects the objects, and
`$(imginc_dump)` and `$(imgdep_dump)` carry `-I` and the `dump.h` prerequisite.

**`-I` and a prerequisite are different statements and only one of them was
there first.** `-I` tells `cpp` where to look; the prerequisite tells `make` to
recompile. Without `$(imgdep_dump)`, editing `dump.h` rebuilt nothing — the
exact failure `tests/deps` exists for, and it caught it on the first run.

### `getgrnam` was resolving from `-lSystem`

`dumpoptr.c` notifies the `operator` group and calls `getgrnam()`. V8's libc has
`getgrent`, `setgrent` and `endgrent`, and this port had imported those — but
not `getgrnam`, so it linked from `-lSystem` and `dump` would have read **the
Mac's** `/etc/group` from inside the jail. Exactly the `getgrent`/`ls -g` leak
CLAUDE.md already records, caught the same way: `nm -u` on the linked binary.

V8 ships `libc/stdio/getgrnam.c`; it is imported and in `libv8c` now, and it is
fifteen lines that call `setgrent`/`getgrent`/`endgrent`. `nm -u` on all three
binaries is empty.

`dumpoptr.c` also declares its own `struct Group` rather than including
`<grp.h>`, with a comment saying `param.h`'s `struct group` conflicts. That is
upstream's workaround for a 1980 header collision and it still works.

## Where it installs: a THIRD upstream source, and it is right

`dump` appears in **none** of `binfiles`, `etcfiles` or `libfiles`, so
`Admin/dest` answers `/usr/bin` **by fall-through** — which is "nobody said",
not "V8 said". Something did say: dump's own Makefile ends `mv dump
$(DESTDIR)/etc`, and `restor` and `dumpdir` are both `etcfiles` entries.

Sweeping all eleven imported makefiles that state a destination found the
fall-through is wrong twice, and the other one settles it:

| | its makefile | shipped tree | `Admin/dest` |
|---|---|---|---|
| `cpp` | `/lib` | `/lib` | `/usr/bin` |
| `dump` | `/etc` | not shipped | `/usr/bin` |

Two sources against the fall-through for `cpp`. This port already puts `cpp` in
`/lib` — but by accident, because it is a toolchain target with its own rule and
never goes through `$(call v8dest,...)`. `dump` had no such accident, so the
Makefile carries `$(MKFILEETC)` and `tests/wavea` recomputes the whole set so
the exception list cannot quietly grow.

## Eliminated by measurement

- **`(&param)[i]`** — none in any of the three.
- **Declarations.** `extern char *ctime()` is correctly declared in `restor.c:150`
  and `dumpdir.c:56`; `dumpoptr.c:15` declares `struct Group *getgrnam()`. No
  `float atof()`, no `(int)signal`.
- **The narrowed-field seam in LOCALS.** `dumpoptr.c:152`'s `localtime(&clock)`
  and `unctime.c:80`'s `localtime(&conv)` both take the address of a local
  declared `time_t`, so both are correct at eight bytes. Only the fields of
  `struct spcl` are narrow. (`unctime.c`'s binary search sets bits 0..30 only,
  so it cannot produce a value above 2^31 — upstream's 2038 limit, which happens
  to match the narrowed field exactly.)
- **Quoted includes that are not headers** — none, so nothing for the Makefile
  to declare beyond `dump.h` itself.
- **libc gaps and the variadic ABI** — `nm -u` is empty on all three binaries
  after `getgrnam`.

## The round trip closes, and it is the point of having all three

```
$ /etc/mkfs fs.img proto            # hello, sub/, sub/deeper
$ /etc/dump 0f tape fs.img
  DUMP: DUMP: 24 tape blocks on 1 tape(s)
$ /etc/dumpdir f tape
Dump   date: Sat Aug  8 14:01:50 2026
Dumped from: Thu Jan  1 10:00:00 1970
    2 /.   2 /..   3 /hello   4 /sub/.   2 /sub/..   5 /sub/deeper
$ /etc/mkfs new.img 2000
$ /etc/restor rf tape new.img
Last chance before scribbling on new.img.
```

and the **restored** image, judged by the five readers that know nothing about
tapes: `icheck` `files 5 (r=3,d=2) used 4 missing 0`, `dcheck` silent, `ncheck`
`/hello /sub/. /sub/deeper`, `fsck` clean in all five phases.

`restor` is therefore the port's **second filesystem writer** after `mkfs`, and
unlike `mkfs` it is judged by programs that were already here.

Note `restor r` reads a newline for `Last chance before scribbling on ...`, and
`dump` exits **1** on success (`X_FINOK`). Neither is a bug; both will trip a
test that assumes otherwise.

## THE BUG THAT STOPPED ALL OF IT WAS IN THE COMPILER

Neither reader could read a tape `dump` wrote, and the tapes were correct — an
independent 32-bit sum over every record of a written tape gives exactly
`CHECKSUM`. What failed was `checksum()` in both readers:

```c
	register i, j;
	do  i += *b++;  while (--j);
	if (i != CHECKSUM) { printf("Checksum error %o\n", i); return(0); }
```

v8cc kept `i` in a 64-bit register and never wrapped it at 32 bits, so the sum
came to `84446 + 2·2³²`. It **printed** `Checksum error 244736` — which is 84446
in octal, the number it was looking for. CLAUDE.md has the general rule and
`tests/v8ccom` the cases; `arm64_trunc()` is the fix.

Worth keeping: the writer was unaffected, because `spcl.c_checksum = CHECKSUM -
s` is a **store** and `str w` re-narrows. So the port could write correct 1985
tapes it could not itself read.

## The narrowed-field seam, and a level-0 tape hides it

Both readers do `ctime(&spcl.c_date)` and `ctime(&spcl.c_ddate)`, and both are
the fsck bug. The two behave completely differently:

| tape | what `ctime` reads | result |
|---|---|---|
| level 0 | `&c_date` → `{c_date, c_ddate=0}` | accidentally **correct** |
| level 0 | `&c_ddate` → `{0, c_volume=1}` = 4294967296 | a date in 2006 where dump says `the epoch` |
| incremental | `&c_date` → `{c_date, c_ddate}` ≈ year 2.4e11 | **live lock, empty stdout** |

So **a test written against a level-0 tape passes**. Fixed with a `time_t`
temporary in both, the same shape as `fsck.c`'s `pinode()`.

`dumpmain.c:18`'s `time(&(spcl.c_date))` is the write direction — an 8-byte
store into a 4-byte field, landing on `c_ddate`. Invisible today for **three**
independent reasons (little-endian puts the zero half there, the high half stays
zero until 2106, and `getitime()` reassigns `c_ddate` afterwards), and fixed
anyway, because that is three accidents rather than a guarantee.

## `-DDIRSIZ=14`, measured per object

`restor.o`, `dumpdir.o` and `dump/dumptraverse.o` **differ** with and without the
flag; `dumpmain.o`, `dumptape.o`, `dumpoptr.o`, `dumpitime.o` and `unctime.o` are
identical. What it buys, measured rather than argued:

- `dumpdir` at 254 on a good tape prints `2 /.` and exits 0 — six entries become
  one, `ncheck`'s failure mode exactly.
- `dump` at 254 loses **directories from an incremental**. `dsrch()`
  (`dumptraverse.c:203`) is the only consumer and runs only in pass II of a
  level >0 dump; at a 256-byte stride it sees `.` and three zero-`d_ino` slots
  and reports nothing changed. Measured: inodes `[2, 4, 1, 3, 5]` at 14 against
  `[1, 3, 5]` at 254. Same tape length, checksums fine, no diagnostic — and a
  restore then produces files with no path to them.

So **rung 5 is blocked for all three**, for `ncheck`'s reason: `dump`'s makefile
is `CFLAGS = -O $(DFLAGS)` with `DFLAGS` empty, so upstream's own description
builds the 254 program. None of the three may join `$(V8BIN)`, which
`tests/jail` asserts.

## Still open

- **`dump J` SIGSEGVs, after destroying data.** `dumpitime.c:193` `fclose`s an
  `oldfile` that is NULL whenever `/etc/ddate` is absent — always, here — and
  line 171 has already `fopen(NINCREM, "w")`, so `/etc/dumpdates` is truncated
  before the crash. Upstream's, and only reachable because the VAX had readable
  text at address 0. Left as upstream wrote it: `J` converts a format this port
  has never had, and `tests/mkfs` does not run it.
- **`id_name[16]` truncation is observable.** `dumpitime.c:112`'s
  `strncpy(itwalk->id_name, fname, 16)` leaves no NUL for a filesystem name of
  16 characters or more, so `recout`'s `%-16s` runs into `id_incno` and the next
  read says `Unknown intermediate format in ./dumpdates` — after which the
  incremental silently degrades to a full dump. `strncmp(fname, id_name, 16)` at
  line 73 also makes any two images sharing a 16-character prefix one entry,
  which is true of every image under a temp directory here. A 1985 field width
  meeting 2026 paths, the `FSNMLG` class; left because it is the *format* of
  `/etc/dumpdates`, not a buffer sized against it.
- **`rawbuf[32]` is reached on EVERY run**, not only via an fstab argument:
  `dumpmain.c:132` calls `fstabsearch` unconditionally and `dumpoptr.c:327`
  calls `rawname()` on every fstab entry. It survives because the jail's
  `/etc/fstab` is manufactured by `kmemu_fstab()`, which types anything not
  beginning `/dev/` as `xx`, and `getfstab()` keeps only `rw`/`ro`. So the bound
  holds two files away, in a shim not written with this caller in mind. The same
  1985 constant fsck's `rawbuf[32]` was.
- **`rewind` collides, and inside libc.** `dumptape.c:91` defines `rewind()`
  taking no argument, while `libv8c.a(getgrent.o)` — linked into `dump` by
  `getgrnam` — *references* `_rewind`. So `setgrent()`'s `rewind(grf)` would
  reach dump's tape rewind, which closes the tape and blocks in
  `while ((f = open(tape,0)) < 0) sleep(10)`. Unreachable only because
  `getgrnam` brackets `setgrent`…`endgrent` and `endgrent` nulls `grf`. The
  `fmin`/`fmax` tripwire in `fsck.PORTING.md` pointing the other way: not libc
  replacing us, us replacing libc *inside* libc.
- **`while (getchar() != '\n')` at EOF** (`dumpdir.c:255`, `restor.c:520`) turns
  any premature end of tape into a silent spin rather than an error. It is what
  made the compiler bug present as a hang. Upstream assumed an operator.
- **`query()` opens `/dev/tty` and `abort()`s if it cannot.** The jail's
  `/dev` has no `tty`, so `rootpath()` falls through to the Mac's; with no
  controlling terminal `dump` aborts. Only reachable from the tape-error and
  volume-change arms, which a file-as-tape never takes.

## `rawname()` returns 0, and neither caller checked

`dumpmain.c:239` returns `0` for a special-file name containing no `/`, and both
users took it at face value:

- `dumpoptr.c:327` — `strncmp(rawname(dt->fs_spec), key, keylength)`, run for
  every `/etc/fstab` entry on every invocation.
- `dumpmain.c:143` — `disk = rawname(dt->fs_spec)`, after which `msg("Dumping
  %s")` and `open(disk)` both read address 0.

On the VAX that read the `0207` there, which matched no key byte, so the entry
simply did not match. The guards say that directly. Not reachable through the
`/etc/fstab` this port installs — twelve `/dev/raNN` lines — but nothing in dump
enforces it, and the shim's manufactured fstab does carry slash-less specs
(`devfs`, `map auto_home`) which survive only because `getfstab()` keeps `rw`/`ro`
entries and `kmemu_fstab()` types those two `xx`. Two accidents, two files apart.

**`fsck` has its own `rawname()` and it answers the question differently:**
`fsck.c:466` returns `cp` unchanged when there is no `/` to rewrite around. That
is the better answer and it is upstream's, in the same source tree — but only
the null is guarded here, because changing dump's `rawname()` to match would
alter what `dumpmain.c` stores in the ordinary case.

One trap in doing it: `rawname()` **mutates its argument**, `*dp = 0` then
`*dp = '/'`, so it is safe to call twice only by accident. `dumpmain.c` keeps
the result in a local rather than calling it once per operand.

Part of the whole-tree address-0 sweep; PLAN.md §4i.
