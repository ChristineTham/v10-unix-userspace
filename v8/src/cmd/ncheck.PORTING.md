# Porting notes: ncheck(8)

PLAN.md §8a step 4e. `@(#)ncheck.c 4.1 (Berkeley) 10/1/80`, 356 lines, imported
and patched in **two places**. It takes the filesystem as an argument, so like
`icheck`, `dcheck` and `clri` it needs no mount and arrives without waiting for
step 5.

*Named `ncheck.PORTING.md` for `mkfs.PORTING.md`'s reason: upstream keeps it as
a bare `cmd/ncheck.c` and `tools/import.sh` mirrors the upstream path.*

## What it computes that nothing else here does

`icheck` walks `di_addr[]` and answers in blocks. `dcheck` walks directories and
answers in link counts. `fsck` does both and repairs. All four report in **inode
numbers** — `bad; inode=2`, `COUNT 3 SHOULD BE 2` — and a number is where every
one of them stops.

`ncheck` is the program that turns that number into a path. It reads the whole
i-list, hashes every directory entry it finds, and then walks the hash table
backwards through `h_pino` to build a name:

```
$ /etc/ncheck fs.img
fs.img:
3	/hello
4	/sub/.
5	/sub/deeper
$ /etc/ncheck -i 5 fs.img
fs.img:
5	/sub/deeper
```

The trailing `/.` is upstream's mark for a directory. That pairing is the case
`tests/mkfs` section 9 makes: corrupt inode 5, let `icheck` name the number, and
hand *its own output* to `ncheck -i` rather than writing the number twice.

## The two patches, both the same bug class

Neither is a width. Both are **"V8 assumes address 0 is readable"** — the class
`refer5.c` already records, where the VAX put the text segment at 0 so
`*(char *)0` returned a byte of the program instead of trapping.

### 1. `atol(argv[1])` walks off the end of the vector

```c
		case 'i':
			for(i=0; i<NB; i++) {
				n = atol(argv[1]);
				if(n == 0)
					break;
```

The loop consumes numbers until `atol` returns 0. When `-i`'s last number is
also the last argument, `argv[1]` is the vector's terminating NULL, and `atol`
dereferences it immediately (`for(;;p++) switch(*p)`). On the VAX that read a
text byte, which is not a digit, so `atol` returned 0 and the loop broke — the
program's exit condition depended on it. macOS leaves page 0 unmapped, so
**`ncheck -i 5` SIGSEGVs before opening anything**.

Patched to `n = argv[1] == 0? 0L: atol(argv[1]);`, which is the VAX's answer
rather than merely the absence of the fault.

### 2. `k` is read uninitialised under `-s`

```c
			if(ilist[0]) {
				for(k=0; ilist[k] != 0; k++)
					if(ilist[k] == kno) goto pr;
				continue;
			}
		pr:
			...
			if (sflg && kno == ilist[k])
```

`pr:` is reached from **both sides** of the `-i` test, and only one of them sets
`k`. With `-s` and no `-i` list, `ilist[k]` indexes by an uninitialised `int`:
`ldrsw`, `lsl #1`, added to `_ilist` — a ±4 GB signed offset. Initialised to 0
rather than guarded, because `ilist[0]` is then 0 and a `kno` of 0 is already
`continue`d above, which is what the print meant to test.

**Mutation does not reproduce the fault, and `tests/mkfs` says so in the case
rather than pretending otherwise.** Measured: with the initialiser removed, ten
runs exit 0, because the stack slot holds a small stale value left by the same
frame. So the case beside it asserts the *contract* — that `-s` with no `-i`
list prints exactly what a plain run prints — which is deterministic, and the
fix stands on the disassembly rather than on a red test.

## `-DDIRSIZ=14` is not optional here, and forgetting it is SILENT

`ncheck` joins the `$(IMGBIN)` group, and it is the member that shows what the
group is for. Built at the host's 254 it reads a **correct** image and prints
nothing at all, exit status 0:

| built | output on a three-file image |
|---|---|
| `-DDIRSIZ=14` | `3 /hello` / `4 /sub/.` / `5 /sub/deeper`, rc 0 |
| no flag | *nothing*, rc 0 |

`NDIR(dev)` is `BSIZE/sizeof(struct direct)`, so it comes out 4 instead of 64,
and `doff += sizeof(struct direct)` steps 256 bytes rather than 16. A root whose
`di_size` is 64 is therefore exhausted by its own `.` entry, which `dotname()`
filters. Every later directory is unreachable.

**So `ncheck` must never join `$(V8BIN)`.** That list is what `$(SRCTREE)` stages
for `Admin/Mk`, and Mk compiles a bare `cmd/*.c` with `cc $CFLAGS -o $B $B.c` and
no `-D` — correct on a machine whose `param.h` says 14. This is
`mkfs.PORTING.md`'s "a wrong writer is invisible to every reader" running in the
**reader** direction, and it is worse in one respect: a wrong writer at least
produces a file to inspect. `tests/jail` asserts `$(V8BIN)` and `$(IMGBIN)` are
disjoint, because nothing at build time notices a name moving between them —
measured, no `make` warning of any kind.

## What it found in OUR code: `%.Ns` read one byte past

`ncheck` prints directory entries with `printf("/%.14s", dp->d_name)` — a
fixed-width field that need not be terminated, which is what `%.Ns` is *for*.
`src/libc/stdio/doprnt.c` had

```c
			for (len = 0; s[len]; len++)
				if (haveprec && len >= prec) break;
```

The loop condition runs before the body, so `s[prec]` is read and discarded.
That is **the port's bug, not V8's** — this file is a C rewrite of `doprnt.S` —
and nothing in the tree had reached it before, which is why 32 cases of `printf`
in `tests/libv8c` did not. Here the byte read is the next entry's `d_ino`;
against a `DIRSIZ` field at the end of a mapped page it is a fault. Fixed by
moving the precision test into the loop condition. `tests/libv8c` has both a
behaviour case and a diagnostic one at `prec = 0`, where the pointer can be made
unmapped without arranging a page.

## Eliminated by measurement

- **The narrowed-field-address seam, both directions.** `ncheck` names no time
  function at all; both sweeps in CLAUDE.md are empty on this file.
- **`(&param)[i]`** — none.
- **Declarations.** All three wide/pointer returns are declared: `daddr_t
  bmap()`, `long atol()`, `struct htab *lookup()`. No `float atof()`.
- **Calls with more than eight arguments** — none; the widest is `bread` at 3.
- **libc gaps and the variadic ABI** — `nm -u` on the linked binary is **empty**.
- **Symbol collisions with `-lSystem`** — none.
- **Narrow K&R parameters.** `pname(i,lev) ino_t i;` and `lookup(i,ef) ino_t i;`
  take a 16-bit type through an 8-byte argument slot. Four precedents in the
  tree already build and run this way (`mkfs.c:504`, `mv/mv.c:275`,
  `w/w.c:700`, `fsck.c:1562`); little-endian makes the low half correct.
- **`l3tol(iaddr, ip->di_addr, NADDR)`** — `iaddr` is `daddr_t[13]` against this
  port's narrowed `daddr_t *`. Match. `ncheck` is `l3tol`'s third caller.
- **The 16-bit `ino_t` ceiling.** `dcheck` *hangs* on a superblock claiming more
  than 65535 inodes, because `ino` is 16-bit and `nfiles` is not. `ncheck`
  compares two `ino_t`s, so the guard never diverges: on the same image it
  terminates in under a second having silently scanned 14432 inodes — `icheck`'s
  quiet truncation, not `dcheck`'s hang. **It needs no deadline in the suite**,
  and that is a measurement rather than an assumption.

## Reproduced, not introduced

- `ncheck.c:350` `if(i > NINDIR(dev))` is `dcheck.c:229`'s off-by-one verbatim.
  At `i == 256` it returns uninitialised stack from inside `ibuf`. Needs a
  267-block directory; unreachable from anything this suite can build.
- `ilist[i] = n` truncates an `atol` result above 65535 into `ino_t`. That is
  the format's limit, not a port bug.
- The `default:` flag arm falls out of the switch into `check(*argv)`, so a bad
  flag is also opened as a filesystem.
- Exit status is `nerror`: 1 on cannot-open or cannot-stat, and **0** for
  everything else, including a wholly unreadable i-list. Every case in
  `tests/mkfs` reads the output, as with the other five.

## Still open

- **The BITFS half.** `ncheck` is one of the four checkers that understand V8's
  bitmap format (`BITFSBIT 64`, the `-B` flag). Nothing here can create one —
  `mkfs` writes only the free-list format — so `bigflag` is never set and the
  `BIGINOPB`/`BIGBSIZE` arms are unexercised. PLAN.md §8a records BITFS as
  deliberately out of scope; if that changes, this is one of the programs that
  is already written for it.
- **`dev = status.st_rdev`** without `makedev(0, bigflag)`. For a regular file
  the shim masks the host value to 16 bits and it comes out 0, which is the
  right answer for the 1024-byte format. For a block or character special it
  would be bit 6 of a macOS device number — the caveat `icheck.PORTING.md` and
  `fsck.PORTING.md` already record, and not reachable from anything the suite
  can point it at.
