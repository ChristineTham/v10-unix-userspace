# Porting lint(1)

`lint` is a shell script that runs `/lib/cpp` into `lint1` into `lint2`.
`lint1` is **pcc1's C front end with lint's back end bolted on**: it links
eight objects and **seven of them are `../pcc1/mip`'s** -- `cgram comm1 optim
pftn scan trees xdefs` -- with only `lint1.o` lint's own.

**29 of the 30 imported files are byte-identical to pristine V8.** All ten of
`pcc1/mip` and all twelve of `cflow` are untouched; the one file that changed
is `lint1.c`, and both changes are the same bug class.

## The recorded blocker was wrong, in a way that survived because its verdict
## was right

Task #28 carried `cflow` as *"BLOCKED, and well"*, on the grounds that
`lint1` is *"a pcc1-family C front end shipped BINARY-ONLY in
usr/lib/lint/"*. `usr/src/cmd/lint/` contains `lint1.c` (1043 lines),
`lint2.c`, `lint.h`, `macdefs` and a makefile. The blocker was reasoning from
`ls usr/lib/lint/`, which is the *installed* directory.

That is the `vi`-is-`ex` shape -- **something recorded as sourceless sitting in
the tree, unread** -- with the twist that the conclusion (`cflow` cannot be
built today) was true, so nothing about it invited re-checking. A correct
answer to the wrong question is more durable than a wrong one.

**AND THE f77 FINDING DOES NOT APPLY**, which is the half that was doing the
real damage. That finding is that pcc1's **pass 2** (`mfile2`) matches on
SHAPES and COOKIES where this port's pcc2 matches on TYPES, so `UCtable.c`
cannot be reused. `mip` is machine-**independent** pass 1, and **lint1 has no
code generator at all** -- it writes `.ln` files, not assembly. The blocker
cited a real result about the half of pcc1 that lint1 does not use.

## Two decisions of Berkeley's are why the LP64 surface is nearly empty

Both were found by reading before building, and both are the `efl` verdict
reached by different routes.

- **`mfile1:142` is `typedef union { int intval; NODE *nodep; } YYSTYPE`** -- a
  real union with a POINTER arm (`mfile1:141`), so a semantic value is eight
  bytes here and nothing truncates. Compare `struct(1)`, where `#define VERT
  int` stored pointers in an `int` and cascaded into two headers. **Read the
  central typedef before costing the audit.**
- **`SZCHAR`/`SZINT`/`SZPOINT` are `extern int`, not `#define`.**
  `lint1.c:916` sets them from `sizeof()` **in the compiler that built lint**,
  so a v8cc-built lint describes arm64 with no arm64 arm anywhere. `macdefs`
  has `#ifdef pdp11` and `#ifdef ibm` arms that only fix up alignment, and
  natural alignment is already what `AL* = SZ*` gives. Compare f77's
  `values.h`, which needed a fourth arm written by hand.

## The four LP64 candidates, and why each is LATENT rather than live

Measured, not argued.

| site | verdict |
|---|---|
| `pftn.c:1698` `i = (int)name`, then `stab[i%SYMTSZ]` | latent |
| `lint2.c:221` `h = ((int)r.l.name)%NSZ`, then `stab[h]` | latent |
| `in.stalign` against `stn.stalign` | not exercised |
| `lint1.c:1069` `(int) ftell` | benign |

The first two are the interesting pair. Both cast a pointer to **signed**
`int` and use the result as an array index, and a negative `int` gives a
negative `%`, which indexes out of bounds. On a VAX the addresses were small
and positive. Measured here with a V8 binary calling `malloc(4096)` forty
times: `0x105998008` and neighbours, low 32 bits `0x05998008`, **zero of forty
negative** -- V8's heap sits just above the Mach-O image base, so bit 31 would
need 2GB of allocation. Recorded rather than patched, per S1: the change is not
forced by the target today.

The third is a union whose arms were hand-sized equal in 1980: under
FLEXNAMES `char *name; int stalign;` was 4+4, matching the `char name[NCHNAM]`
it replaced, and on LP64 it is 8+4, which moves `in.stalign` off
`stn.stalign`. Not exercised: `stalign` and `stsize` are reached through the
`stn` arm only, one site each, measured.

`CONSZ` is `long` and `CONFMT` is `"%Ld"`, which this port's `doprnt.c` does
not handle (it takes `l`, `D`, `O`, `U`). Both sites are in `eprint`, reached
only under `bdebug`, `edebug` or `ddebug > 2`. Recorded, not patched.

## The two changes, and both are the address-0 class

**1. `lint1.c:792` -- `strcmp(s, fsname.f.fn)` where `f.fn` is NULL.**

`fsave`'s `fsname` is **static**, so on the first call `f.fn` is 0. Under
FLEXNAMES `lint.h:60` makes it a `char *`; the other arm is a `char
fn[LFNM]` ARRAY, which zero-initialises to the empty string and cannot fault.
**So the defect is selected by the `-D`**, and upstream's own CFLAGS carry
`-DFLEXNAMES`.

A VAX did not fault: ZMAGIC text begins `00 00 ...`, so address 0 IS the empty
string, `strcmp` returns nonzero, and the "new one" arm is taken -- which is
what the first call is supposed to do. Spelling the empty string reproduces
that ANSWER rather than merely avoiding the fault, which is `quot`'s `qcmp`
construction for `quot`'s reason.

**2. `lint1.c:1069` -- `ftell(stmpfile)` where `stmpfile` is NULL.**

`stmpfile` is opened by the `-S` option (`lint1.c:893`) and by nothing else,
and **upstream's own rule for the two `.ln` libraries runs `lint1 -v` with no
`-S`**. So this `ftell` gets a null `FILE *` every time a library is built.

`bycode()`, the function immediately above at `lint1.c:1039`, tests
`c >= 0 && stmpfile`; this one tested nothing. **The fix landing on one line
while the line beside it keeps the assumption**, from the other side.

A VAX did not fault here either, and for a reason worth keeping: `ftell` only
READS the FILE -- `_cnt`, `_ptr`, `_base`, `_flag`, then `lseek(_file)` -- and
virtual 0 in a ZMAGIC binary is crt0 rather than an unmapped page. It returned
a garbage long nothing looked at. **The answer restored is not that garbage**:
10 is upstream's own, at `macdefs:56`, which is what `getlab()` expands to when
FMTARGS is not defined at all, and the comment above the function already says
the value must be at least 10.

**And my own audit had already cleared that line.** It flagged `(int) ftell`
as a width question and pronounced it benign under a 2GB temp file, which is
true. The live defect is a null `FILE *` at the same site. **Clearing a line
for one hazard is not clearing the line.**

## Two hand-maintained copies of one token list

`cgram.y:1` onward is `%term NAME 2`, `%term ICON 4`, ... -- the token numbers
are **pinned in the grammar**, because in pcc a token number IS a tree operator
number, and `manifest:8` declares the same list by hand for the lexer and for
`NODE.in.op`. That is `efl`'s `gram.c`/`tokens` shape, so upstream runs yacc
with **no `-d`** and `tests/wavea` compares the two lists as SETS IN BOTH
DIRECTIONS: two lists of the same length can differ and still both be right
about their length.

This port's yacc honours explicit numbers (`y2.c`'s TERM arm reads a NUMBER
after the identifier), and its global `#define YYSTYPE long` fix does not
fire here, because that line is guarded by `if( !ntypes )` and this grammar
declares types.

## `/usr/tmp`, and a stale reason in a neighbouring PORT comment

`lint.sh` puts its temp files in `/usr/tmp`. V8's tarball ships neither `/tmp`
nor `/usr/tmp` because both are empty runtime directories -- the same reason it
ships no `/usr/adm` -- and **sixteen V8 programs name `/usr/tmp`**, including
`sort.c:42`, which is `{"/usr/tmp", "/tmp", NULL}` and has been silently taking
the fallback for the life of this port. **This Mac has no `/usr/tmp` at all**
for the union to fall through to.

`troff/n1.c` took the other road and its PORT comment says `/usr` *"is
protected by SIP so it cannot be made"*. That is true of the **host's** `/usr`
and does not apply inside the jail: `$(ROOTFS)/usr` is an ordinary build-tree
directory, and the mount table's bare `/usr/` is a UNION, so `/usr/include` and
`/usr/local` still fall through untouched. The Makefile makes
`$(ROOTFS)/usr/tmp`, which is what keeps `lint.sh` **byte-identical to
upstream**; S1 prefers that to a one-token change to authentic source. troff is
deliberately left alone -- it works, and changing it would be forced by tidiness
rather than by the target.

## `ulibfiles` -- the fourth Admin table, and lint is the first to need it

`Admin/dest` has FOUR table arms and this port's `$(call v8dest,...)` reads
three, deliberately. `lint` is in `ulibfiles`, so `dest` answers `/usr/lib`
while lint's own makefile and the shipped tree both say `/usr/bin`.

The omission is right and `tests/wavea` already said so before this import:
*"Adding it for completeness would break `man`."* `man`, `spell` and `lint` are
the identical install shape -- a script at `/usr/bin/NAME`, the machinery at
`/usr/lib/NAME`, the name in `ulibfiles` -- and the table is describing the
machinery.

What lint adds is the NINTH entry to the makefile-versus-`dest` set and **the
first of the opposite shape**: the other eight are all "in no table, so `dest`
answers `/usr/bin` by fall-through". `lint` IS in a table. The set's stated
characterisation went stale the moment a second shape joined it.

`man` and `spell` are absent from that set for a reason worth knowing: their
machinery line is a SINGLE-source command the basename test reads (`mv man
/usr/lib`, `mv spellprog /usr/lib/spell`), while lint's is `cp lint[12] llib*
${LINTDIR}` -- MULTI-source, which the parser deliberately skips after the
`struct` false positive -- so lint falls through to its `/usr/bin` script line.
**Three programs of one shape, landing differently on the arity of one `cp`.**

## What is deliberately not done

- The shipped `llib-lc.ln` and `llib-port.ln` are **not** installed. lint1 and
  lint2 exchange raw structs through `fwrite`/`fread`, so the format is
  whatever the compiler that built the pair says it is. Both ends are ours,
  which is the case where a width change is safe; a shipped VAX `.ln` read by
  an arm64 lint2 is the opposite case. The build regenerates both.
- `llib-lj` and `llib` are in the shipped `/usr/lib/lint` and are not built
  here: `llib-lj` is the jerq lint library and neither is named by lint's own
  makefile install arm.
- The nineteen `mip` files lint1 does not link are not imported. They are pass
  2 and the Fortran front end, and an unconsumed component invents a
  difference the tree does not have.
