# cyntax(1)

Bruce Ellis's whole-program type checker, September 1984.  Three binaries and
two generated library descriptions; **66 of 68 imported files are
byte-identical to pristine V8**, and the two changes are `cc.c:112`'s and one
address-0 read.

| component | from | installs to | lines |
|---|---|---|---|
| the driver | `cem/cyntax.c` | `/usr/bin/cyntax` | 1113 |
| the front end | `cyn/c00.c`..`c40.c` | `/usr/lib/cyntax/ccom` | 23750 |
| the cross-module pass | `cem/*.c` (12 objects) | `/usr/lib/cyntax/cem` | 5787 |
| library descriptions | `lib/llib-lc`, `lib/llib-lj` | `/usr/lib/cyntax/{libc,libj}` | 361 |

`cyntax` execs `/lib/cpp`, then `ccom` once per `.c` (writing a `.O`), then
`cem` over the `.O`s.  **Correctness is a property of all three**, which is
`bc`/`dc`'s shape at one more remove and the reason this is one step.

It is the largest single program this port has taken -- 23750 lines in `cyn/`
against `f77pass1`'s ~13000 and `cfront`'s 22442.

## What it does, measured

```
$ cyntax bad.c
bad.c: 5: operands of '=' are pointer to char (p) and int (i)

$ cyntax m1.c m2.c
function sub: m2.c: 1
	m1.c: 4, arg 1 expected int, found char *

$ cyntax libmis.c
function strlen: libc
	libmis.c: 4, arg 1 expected char *, found int
```

The second is what the program exists for and what `lint` needs `.ln` files to
do.  The third is the *generated* `libc` description being consulted, which is
the only thing that proves `lib/Makefile`'s two-program chain produced a real
artefact rather than a plausible-looking file.

## THE SOURCE IS OBFUSCATED, AND THAT IS UPSTREAM'S

Every identifier in `cyn/` is a random dictionary word -- `colonic *feller()`,
`polymath`, `entoblast`, `womb`, `Lombardic` -- and all 41 files carry an
identical 213-line preamble of typedefs and externs.  `cyn/c18.c:272` is
`feller`'s only definition.  Nothing here did that; it is how Bell Labs
shipped the sources.

**AND THE OBFUSCATOR CORRUPTED EVERY COMPOUND ASSIGNMENT OPERATOR, WHICH IS
WHAT DECIDES WHETHER THIS PROGRAM CAN BE BUILT AT ALL.**  It emitted a SPACE
inside each one: `cyn/c01.c:349` is `poignant | = girn & (...)`, meaning `|=`;
elsewhere `girn | = 0x0002` and `poignant & = ~0x0080`.  Measured:

| compiler | result |
|---|---|
| `clang -std=gnu89` | **21 of 41 files fail**, 119 `expected expression` errors |
| `v8cc` | **41 of 41 compile** |

and the decisive three-line control is `a | = b` with `a=6`, `b=3`, which
v8cc compiles and which yields **7**.  So the spelling is not merely tolerated,
it means `|=`.  That is a lexical fact about V8's own compiler, and it is why
`$(V8CCRUN)` is not interchangeable with `$(HOSTCC)` in these rules.

**A PORT WHOSE COMPILER WAS A HOST COMPILER COULD NOT HAVE BUILT THIS PROGRAM
AT ALL.**  It is the sharpest payment yet for the fidelity contract's most
expensive clause -- that the authentic compiler compiles the authentic
sources.

## `Made` IS A TRANSCRIPT OF BELL LABS' OWN BUILD, AND IT FAILED

The directory ships a file recording a build that ended

```
cc -o ccom  c00.o ... c37.o
Undefined:
_feller
*** Error code 1
```

Read as a verdict this says cyntax was unbuildable when it shipped.  It is not
one: `feller` is defined at `cyn/c18.c:272`, which is *inside* the range that
transcript linked, so the failure is not a missing file.  `Made` is a stale
record of an intermediate state, and the makefile beside it has moved on.

**THREE COPIES OF THE FILE LIST EXIST AND THEY DISAGREE**, which is this
tree's two-hand-maintained-copies trap with a third copy:

| source | says |
|---|---|
| the directory | `c00.c` .. `c40.c` (41) |
| `cyn/Makefile`'s `OBS` | `c00.o` .. `c40.o` (41) |
| `file-list` | stops at `c38` (39) |
| `Made`'s link line | `c00.o` .. `c37.o` (38) |

`cyn/Makefile` is the one the build reads, so it is the one `CYN_NAMES`
transcribes -- and `tests/wavea` compares the two as sets in both directions
rather than leaving that to this sentence.  **`c00.c` is ZERO BYTES** and is in
the list; it is kept because upstream lists it, and it compiles to an empty
object.

## The one source change

`cem/cyntax.c` gains `incdir`, built from `$V8ROOT` and appended to the cpp
argument list after `options()` has run.  This is `cc.c:112` verbatim in
intent: V8 needed no such flag because `/usr/include` **was** the system's and
cpp hardcodes it, ours lives under `$V8ROOT`, and the installed `/lib/cpp` is
still the clang-built stage-0 binary, so it never sees `rootpath()`.  Without
it every program answers `Can't find include file stdio.h`.

Appended rather than prepended, so a user's own `-I` still wins -- the ordering
`cc.c` chose and states.

## The second change: a null guard that dereferenced the OTHER pointer

`ccom` SIGSEGV'd on **every unknown option** -- 49 of them, found by the crash
probe, with no diagnostic at all.  `main()`'s `default:` arm (`c28.c:332`)
calls `enucleate()`, which calls `veiling()`, which calls
`Shylock((unhorse *)0, 2, msg)`.  `Shylock` **has** a guard for that null
first argument, and inside it dereferences a different pointer:

```c
	if ( ingratiatingly == 0 ) {
		chromatographically = kerchief ;
		melanotic = perceptibly ->plangently ;
	} else {
```

`perceptibly` is a tentative definition in all 41 files, so it is zero until a
source position exists -- and it is assigned only ever **in lockstep with
`kerchief`** (`c03.c:542` and eleven siblings, always the same comma
expression).  At option-parsing time neither is set.

**IT IS THE READ HALF OF THE ADDRESS-0 CLASS, SO THERE IS A VAX ANSWER TO
RESTORE**, which is the discriminator this port already uses for `cpio`'s
`/dev/tty` (a WRITE, and therefore recorded rather than fixed).  Better than
that: the value is **discarded**.  `kerchief` is 0, so the branch below takes
`reductionistic(bibliophilism)` and `melanotic` is never used.  A VAX read a
harmless byte of its own text and printed the usage line; the guard reproduces
that exactly, and `ccom -q` now answers

```
ccom: [-f srcname[modtime]] [-l libname] [-V func:n] [-Orw] [infile [outfile]]
```

Note the `@` that is missing from that line.  The source string is
`srcname[@modtime]`, and `@` is one of Shylock's own format directives (beside
`%`, `/`, `$`, `!` and `#`), so an unescaped one is consumed rather than
printed.  That is upstream's on any machine -- a VAX printed `srcname[modtime]`
too -- and is left alone under S1, the same verdict `cb`'s precedence bug got.
The output above is therefore what V8 itself produced, not a port artefact.

**A GUARD ON ONE POINTER IS NOT A GUARD ON THE LINE INSIDE IT.**  This is the
fix-landed-on-one-line-and-the-line-beside-it-kept-the-assumption shape with
the two lines being *the guard and its own body*: whoever wrote
`ingratiatingly == 0` knew the argument could be absent and did not ask the
same question of the global they reached for instead.

## Dead on arrival upstream, and left that way

Three of the driver's options reach paths that do not exist in the
distribution, and none of that is this port's doing:

- `cyntax.c:153-155` are `/user1/other/brucee/mh/c/bin/ccom.{debug,lcomp,prof}`
  -- a Bell Labs researcher's home directory, reached by `-G`, `-L` and `-P`.
- `cyntax.c:150` is `/lib/sets`, reached by `-Z`.  **The distribution ships no
  `/lib/sets`**, so that pass was already missing when V8 shipped.
- `cyntax.c:148-149` and `:152` and `:156` define `COMP_PATH` and `COMP_NAME`
  **twice each**, identically.  Harmless and upstream's.

Recorded rather than repaired, per S1.

## What does not work, and why it is not a gap in this port

`cyntax -j` adds `-I/usr/jerq/include` to the cpp line at RUN time, and the
unjailed cpp resolves that against the Mac.  The *build* of `libj` is fine --
it reads `$(JERQINC)` directly, which is what the rootfs-include rule already
does for `jioctl.h`.  This is the recorded "the installed cpp and ccom are
still the clang-built stage-0 binaries" limitation, not a new one, and it will
close when they do.

`TMP_DIR` is `/tmp` (`cyntax.c:165`), which is **not** a jailed prefix, so the
driver's temporaries land in the host's `/tmp`.  That is the union rule working
-- sixteen V8 programs name `/tmp` and the Mac has one -- and is stated here
only because it is the one path in the program that leaves the world.

## LP64

Near-empty, and for `efl`'s reason read from the other side.  `cyn/` has **no
`#include` at all** -- each file carries its own copy of the preamble -- and
that preamble declares real named struct types, so every pointer-returning
function is declared returning a pointer.  `cem/` is ordinary readable C.  The
rootfs-wide truncation sweep in `tests/v8ccom` is clean over all three
binaries.

**The one width defect this program found was in OUR COMPILER, not in it** --
see PLAN.md §4m and the note beside `MYINIT` in
`compiler/ccom-arm64/macdefs.h`.  An enum member in a static aggregate initialiser was emitted
eight bytes wide where the struct layout said four, so `ld` refused the link of
both `ccom` and `cyntax` with `pointer not aligned`.  On a VAX `SZLONG` and
`SZINT` were both 32 and the fault could not exist.
