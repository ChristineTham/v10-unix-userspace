# lcomp — porting notes

`cmd/lcomp` is Bell Labs' basic-block profiler.  **One directory, two
programs, and only one of them is portable** — `pack`/`unpack`'s shape with the
second half genuinely blocked rather than merely deferred.

`lprint(1)` is in, at `/usr/bin/lprint`, with **zero source changes**: all
twelve imported files hash to their recorded `PROVENANCE` blobs.  `nm -u` on
the result is empty.

## Why `bb` cannot come, and why that is not a deferral

`bb` **reads and rewrites VAX assembler** to insert tally code.  `instr.c` is a
table of VAX mnemonics against the condition codes each one kills —
`struct inst { char *iname; short type; }` at `instr.c:8-10`, then `acbb`,
`adawi`, `addb2`, `addb3` and some four hundred more.  It is `#include`d into
`bb.c:9` and `5bb.c:9` rather than compiled, which makes it the tree's newest
included non-header.

`lcc:6-8` is the whole mechanism, and it names the blocker in three lines:

    cc -g -S $clist $i
    $DIR/bb $u"s"
    cc X$u"s" -c

Compile to assembly, rewrite the assembly, reassemble.  Porting that means
authoring an arm64 instruction table with condition-code semantics — **writing
a new program rather than porting one**, which is the `as`/`ld` exception
reached by a different route rather than a new decision.  So `bb`, `5bb`,
`lsub`, `syscore`, `sysprof` and `nexit` are imported and **not built**;
`refer`'s `whatabout` is the precedent, and the gap is latent rather than live.

`tests/deps` states it as two `nodep`s rather than as this paragraph, because
the half that could rot back in unnoticed is the half nothing compiles.

## `lprint` touches none of it

It reads `prof.out`, which is a **text** file — `fscanf("%s")` for a
`/`-leading filename, `fscanf("%d")` for each count, `fprintf("%u\n")` to write
one back under `-c`.  There is no binary record and therefore no on-disk width
question.  Its only occurrence of the word `instr` is the `.s` filename suffix
test at `lprint.c:243`.

## The hazard that measured clean, and the instrument that settled it

`realloc` is used at `lprint.c:113` and `lprint.c:138` and is **not declared**,
where `malloc` at `lprint.c:19` is.  That is this port's signature truncation
shape: an undeclared function returning a pointer is implicitly `int`, and the
cast on the call is not a use.

It does not truncate here, and behaviour is not what says so — the heap can sit
low enough to hide it, which this tree has already recorded.  `cc -S` is the
instrument.  Both `realloc` sites emit

    bl  _realloc
    ldr x9, [sp, #256]
    mov x10, x0          <- the WHOLE register
    str x10, [x9]        <- a full 8-byte store

instruction for instruction identical to the `malloc` control that *is*
declared `char *`.  v8cc never materialises the result as a 32-bit quantity, so
a declaration would change nothing and S1 forbids a change that is not forced.
`maketab.c`'s verdict reached a second time: **record the measurement, do not
patch the cast.**

Both `realloc` arms were also exercised rather than reasoned about — the count
array grows past `quot = 100` and the file table past `ltab = 20`:

| input | answer |
|---|---|
| 300 counts, one file | `300 bbs 300 execs 0 untouched` |
| 25 files, one count each | all 25 listed, `f24` and `f25` past the boundary |

## A narrow store that is exact by an accident of STORAGE CLASS

`lprint.c:21` declares `unsigned long val` and `lprint.c:129` fills it with
`fscanf(fd, "%d", &val)` — a **four-byte write into an eight-byte object**.
Exact on a VAX, where `# define NOLONG` (`cmd/ccom/vax/macdefs.h:20`) made
`long` 32 bits.

It is exact here too, and the reason is worth stating because it is not design.
`val` has exactly three occurrences — the declaration, that `fscanf`, and
`curtab->cnt[index++] += val` at `lprint.c:145`.  It is a **global**, so its
high half is zero-initialised and **nothing in the program ever writes it**;
the little-endian `%d` fills the low half and the read at `:145` is correct.
Move that declaration inside a function and the same line reads uninitialised
stack.  Not changed, because nothing is wrong today — recorded because the
thing keeping it right is one word (`static` storage) that a tidy-up would
remove without appearing to touch the arithmetic.

## Recorded, not repaired

- **`lprint.c:16` is `char fname[512] = "/";` with upstream's own comment
  `/* not checked for overflow */`.**  `fscanf(fd, "%s", fname+1)` is
  unbounded.  512 is a bare number rather than a `DIRSIZ`-derived one, so it is
  not the cascade class, and a VAX overran it identically.  S1: upstream's
  defect on upstream's hardware.
- **`prof.out has weird format` is followed by `abort()`**, not an exit.  Any
  byte that is not `/`, a digit or a newline dumps core.  Upstream's.
- **The summary accumulators widened, and that is LP64 rather than a change.**
  `unsigned long N, B, L, V` wrapped at 2^32 on a VAX and do not here, so this
  `lprint` is arithmetically *more* correct than the one Bell Labs ran on a
  profile totalling more than four thousand million executions.  Nothing in
  this world can produce one.

## The stale record in the directory

`README` opens *"There's no makefile."*  There is: `makefile`, 190 bytes,
imported beside it, with `all`, `install` and `clean` arms.  `cyntax`'s `Made`
transcript is the same shape — a record in the tree that invites the wrong
verdict about the tree.  Left as imported.

## What is still open

`lprint` reads a `prof.out` that nothing in this world can write, because `bb`
is the only producer.  It builds, links and refuses honestly
(`prof.out: No such file or directory`, exit 1), which is `load(1)` saying
`No mem` and `dmesg` refusing — a real program that cannot answer, and a
documented-acceptable outcome rather than a gap.  Its cases therefore feed it a
hand-written `prof.out`, which is the only way to test the half that computes.
