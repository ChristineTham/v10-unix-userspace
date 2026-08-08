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

## Still open

Recorded when the runtime story is settled.
