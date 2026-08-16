# struct(1) — porting notes

Wave A2 batch 2e.  Brenda Baker's Fortran-to-Ratfor restructurer: 40 `.c`, a
grammar, a lexer, 5695 lines.  Upstream: `v8/usr/src/cmd/struct`.

**IMPORTED AND DELIBERATELY NOT BUILT.**  It is not in the Makefile and
`tests/wavea` exempts it by name.  What follows is the state it is in and why
stopping here was the right place to stop.

## It builds and links

- **37 objects, zero compile failures** under v8cc.
- `structure` links from 35 of them (255592 bytes) with an **empty `nm -u`**.
- `beautify` links from `beauty.o tree.o lextab.o bdef.o` plus `libl.a`
  (182264 bytes), also with an empty `nm -u`.

Three build idioms, all of which this tree already handles:

- **`beauty.c` and `y.tab.h` are CHECKED IN** — the generated parser is in the
  distribution, which is `ccom`'s `cgram.c` idiom exactly, and the reason the
  built-in `%.c: %.y` rule must stay cancelled.
- **`lextab.o` comes from a lex file** with no yacc file beside it, which is
  `pp`'s idiom from batch 2d.
- **The link line globs `0.*.o 1.*.o …` while the dependency list does not.**
  `$(2FILES.o)` excludes `2.test.o` and `3.test.o`, but `cc -o structure … 2.*.o`
  would pick them up if they existed, and each has its own `main`.  Same trap as
  `tbl`'s `t?.o`.  Compile only what the FILES lists name.

## `-lln` names a library the distribution does not ship

`beautify` links `-lln` and there is no `libln.a` anywhere in the archive — only
`libl.a`.  Measured rather than guessed, because guessing from a `-l` name is
exactly the mistake `src/libplot/PORTING.md` records: `nm -u` on the four
objects wants `yywrap` and nothing else, and `libl.a` (imported in batch 2d for
`pp`) provides it.  So `-lln` is satisfiable, and the third instance here of a
`-l` flag whose archive is absent — after `libm.a` and `libplot`.

## Why it is not built: it is written in `int` where it means pointer

`structure` SIGSEGVs on its first line of real Fortran.  **IT NOW DOES ITS
JOB.**  Measured, on seven inputs of increasing shape:

| input | before | now |
|---|---|---|
| straight-line assignment | SIGSEGV in `fixvalue` | correct Ratfor |
| two statements | SIGSEGV in `fixvalue` | correct Ratfor |
| a plain `IF` | SIGSEGV in `fixvalue` | correct Ratfor |
| `IF … GOTO` backwards | SIGSEGV in `fixvalue` | **`REPEAT … UNTIL`** |
| `DO` with a `GOTO` out | SIGSEGV in `fixvalue` | **`DO … { break 1 }`** |
| three-way `GOTO` branch | SIGSEGV in `fixvalue` | **`IF / ELSE IF / ELSE`** |
| computed `GOTO`, 2 routines | SIGSEGV in `fixvalue` | **`SWITCH … CASE`** |

The baseline column is a *control*, not a memory: the pre-change sources were
rebuilt in a scratch tree and run on the same files, and **every one died in
`fixvalue`, including the trivial one.**

The bottom four are the point of the program — every `GOTO` is gone, replaced
by the structured construct it encoded:

```
      SUBROUTINE D(N)              subroutine d(n)
   10 N = N - 1                    REPEAT
      IF (N .GT. 0) GOTO 10   -->  	n = n - 1
      RETURN                       	UNTIL(!(n .gt. 0))
      END                          return
                                   END
```

**Defect 1, FIXED here: the five allocators in `0.alloc.c`.**  Every one returns
a pointer through an implicit `int`, and `challoc` stacks two truncations:

```c
challoc(n)          /* implicit int return */
int n;
	{
	int i;          /* an int holding malloc's result */
	i = malloc(n);  /* malloc undeclared -> int */
	if(i) { space += n; return(i); }
```

This is the class `CLAUDE.md` opens with, and it fires at the program's first
piece of real work: `hash_init()` (`1.hash.c:92`) does `hashtab = challoc(...)`
then `hashtab[i] = -1L`.  Measured — `EXC_BAD_ACCESS at 0x26e04c90`, a 32-bit
value, this class's signature.

Fixed by declaring what they return: `char *challoc()`, `int *balloc()`,
`int *talloc()`, `int *galloc()`, `struct coreblk *morespace()`, plus
`char *malloc()` in `0.alloc.c` and the matching declarations in `def.h` —
which is the right header because upstream's own makefile already says
`main.o $(0FILES.o) $(1FILES.o) …: def.h`, so one place reaches every caller.
Not one statement changed.

**Defect 2, FIXED: it was ONE `#define`, and the fix is `def.h:11`.**  The
previous note read this as "not one more line but a pass over the module, and
an unknown number of the other 39 files may do the same", and costed it far too
large.  What `1.hash.c` is doing is not style: while a Fortran label is
unresolved it threads a **fixup chain through the graph's own cells** —
`addref` stores the address of a cell, each cell holds the address of the next,
`fixvalue` walks the chain overwriting every one with the real vertex.  So a
cell means *a vertex number or a pointer*, and the only thing that has to
change is that the CELL be pointer-sized.  `#define VERT long`.

**The cascade was two declarations, measured rather than feared.**  Widening it
broke 13 of 38 objects; both classes are one line each and both are upstream's
own latent contradictions rather than consequences:

- **`after` is declared at two widths in two headers** — `extern VERT *after`
  (`2.def.h:2`) and `extern int *after` (`def.h`, then).  It is *defined*
  `VERT *after` at `2.main.c:5`, so `2.def.h` was right and `def.h` was always
  wrong.  It cost nothing for forty years because `#define VERT int` made the
  two spellings the same type.  Seven translation units refused at once.
- **`arcsper[]` must share a width with a graph cell**, because `ARCNUM` is a
  ternary yielding either `&arcsper[t]` or `&graph[v][-arcsper[t]]` — a
  non-negative entry IS the arc count, a negative one is the OFFSET of the
  count inside the node.  The two arms are interchangeable by construction, so
  the ternary has no type unless they agree.  Eleven sites, one macro.

With those two, **all 38 objects compile, zero errors — the same count as the
baseline.**

**Defect 3, FIXED, and it is THE LINE BESIDE IT in the most literal form this
repository has recorded.**  `def.h` declared `arc()` and `lchild()` as
`VERT *`; **the very next line** declared `vxpart() negpart() predic()
expres() level() stlfmt()` as `int *`.  All eight return `&graph[v][...]`.
Upstream spelled one type two ways on two adjacent lines and a VAX could not
tell them apart.

The consequence is not a warning but silent half-width access: `BEGCODE(v)` is
`*vxpart(v,…)`, so `BEGCODE(num) = stcode` (`1.recog.c:109`) stored only the
**low half of a string pointer**.  Measured as SIGSEGV inside `_doprnt` on a
`%s`, at an address that **changed between runs** — `0x4b4686c`, then
`0x1de0686c`.  That variation is the evidence: ASLR moving it is what says it
is a real heap pointer with its top half gone, rather than a constant.

**Defect 4, FIXED: `stralloc` and `remtilda` return `char *` undeclared.**
Real but not the cause of anything observed — `remtilda` escapes by returning
its own parameter, which emits no truncation.  Fixed as a pair so the next
reader need not re-derive which of the two was safe.

## Two instruments are blind to the return direction, and both were silent

Worth knowing before trusting either on this class:

- **v8cc warns on a pointer mismatch in an assignment and NOT in a `return`.**
  Six functions returning `VERT *` from an `int *` declaration produced no
  diagnostic at all.  A whole-module warning diff against the baseline named
  exactly **one** new site, and it was a different bug (`galloc`).
- **`tests/trunc-sweep.awk` reads CALL SITES**, so a callee narrowing its own
  result has nothing at the call to match.  It reported **zero hits** over this
  binary throughout — correctly, and validated against `rootfs/bin/ls`, which
  gives 4.

So the warning diff and the sweep between them covered the assignment
direction twice and the return direction not at all.  What found defect 3 was
reading two adjacent declarations.

**Defect 5, FIXED: ELEVEN functions RETURN a VERT and were declared implicit
`int`.**  This is the widened-VERT half of defect 3's class, and it is the
worse half, because it does not crash — **it inverts a predicate**.
`UNDEFINED` is −1; returned from an implicit-`int` function it goes back in
`w0`, a write to `w0` zeroes bits 63:32, and the caller reads
`0x00000000FFFFFFFF` = 4294967295.  `DEFINED(v)` is `(v >= 0)`.  So after
widening VERT, **every undefined value returned by one of these tested as
defined.**

Found by sweeping, in two passes, and **the first pass was too narrow**:

| pass | what it asked | found |
|---|---|---|
| 1 | which functions `return(UNDEFINED)` | `oneelt innerdo maxentry lexval NUM` |
| 2 | which return a **VERT-typed local** with no return type | `addum makenode makeif comdom lowanc makebr` |

The second pass is the right question and the first is a special case of it.
Two of the six only pass 2 could find decide the tree's **shape** rather than
one node's field: `comdom` fills `dom[]` and `lowanc` fills `head[]`, and
`gettree` branches on `head[v] == head[from]`.  A truncated `UNDEFINED` there
does not crash — it builds a different program.

Each is called before its own definition, so all eleven needed the forward
declaration and not merely the definition corrected.

**Defect 6, FIXED, AND IT IS THE ONE THAT MADE IT WORK: `exchange()` is a
generic MACHINE-WORD swap, and the machine word grew.**  `2.dfs.c` declares

```c
exchange(p1,p2)  int *p1,*p2;  { int temp; temp = *p1; *p1 = *p2; *p2 = temp; }
```

and its four callers pass **three different things**, all four bytes on a VAX:

| site | argument | what it is |
|---|---|---|
| `2.dfs.c:140` | `&graph[v], &graph[loo]` | `VERT **` — **row pointers** |
| `2.dfs.c:141` | `&v, &loo` | `VERT *` — vertex numbers |
| `3.loop.c:136` | `&graph[temp], &graph[v]` | `VERT **` — row pointers |
| `3.then.c:73` | `&LCHILD(v,THEN), &..ELSE` | `VERT *` — graph cells |

With `int *` it exchanged only the **low 32 bits** and left both high halves.
`negate()` swaps THEN=−1 with ELSE=8, and the cells came out
`0xFFFFFFFF00000008` and `0x00000000FFFFFFFF` — each with the correct low half
and *the other value's* high half.  `DEFINED()` is `v >= 0`, so the undefined
child read as the positive 4294967295 and `mkthen` fired.

Declared `long` rather than `VERT`, and that is the only honest choice: two
callers pass pointers.  What upstream means is *one machine word*, which `int`
was on a VAX and `long` is here; `VERT` would be right for two callers and a
lie for the other two.

**The two row-pointer sites are worse and were never reached** by any of these
inputs — a half-width swap of two heap pointers corrupts the graph's row table
outright.  They are fixed by the same one line.

**IT WAS IN MY OWN SWEEP OUTPUT AND I MISREAD THE LINE.**  The `int *` sweep
printed `2.dfs.c:148:int *p1,*p2;` under a heading I had written as "int\*
locals", and I moved past it looking for locals.  It is a **K&R parameter
list**, which is the one context where a declaration on its own line is not a
local at all.  Two further phases of instrumentation were spent re-deriving
what that line had already said.

## A verified latent hazard, NOT the current cause: `create()` reallocs an
## arena pointer

`0.parts.c` grows the graph with

```c
temp = realloc(graph, maxnode*sizeof(*graph));
free(graph);            /* after realloc has already released it */
graph = temp;
```

Two things are wrong and both are measured, not read.  `graph` is allocated by
**`challoc`** (`1.init.c:11`), and `challoc` sub-allocates inside a larger
block — `morespace()` does `q = malloc(...)` then `q->blk = q + 1` — so
`graph` **is not a `malloc` pointer** and neither `realloc` nor `free` may be
called on it.  And `free(graph)` after `realloc(graph, …)` is a double free
when the block moved, or frees the block just returned when it did not.

**Not reachable in anything tested here**: `maxnode` starts at **400**
(`0.args.c:10`) and the reproducers create ~10 nodes.  It is recorded because
it fires on a large Fortran routine, which is exactly the input nobody will
try until the small ones work.

## How the last one was found, because the route matters more than the fix

`mkthen` asserts `!DEFINED(w) || (DEFINED(tc) && BRANCHTYPE(NTYPE(tc)))`.
Instrumented (in a scratch copy — never in `src/`), the values are

```
MKTHEN v=3 w=4294967295 tc=-4294967288 ntc=-1
```

and they name the defect precisely.  `w` is **0xFFFFFFFF** — `UNDEFINED` (−1)
written as 32 bits and read as 64, so it reads *positive* and `DEFINED(w)` is
true when it must be false.  `tc` is **0xFFFFFFFF00000008**: top half all-ones,
low half 8.

**The values are unchanged by all eleven fixes of defect 5**, which is what
says the remaining defect is a different one and not a missed instance.

**Dumping the whole row is what narrowed it, and it refuted two hypotheses.**
For the failing node (`NTYPE`=IFVX, `nonarcs`=8, `childper`=2, so `LCHILD`
THEN is `[7]` and ELSE is `[6]`):

```
[0]=1 [1]=-1 [2]=-1 [3]=-1 [4]=4425033912 [5]=1 [6]=4294967295 [7]=-4294967288 [8]=7 [9]=4
```

- **`[4]` is NOT corrupt**, though it reads like it.  It is `PREDIC(v)`, a
  string pointer, and it **changes between runs** (`12884912312`, then
  `4425033912` = `0x107C4C0B8`, an ordinary macOS heap address).  ASLR varying
  it is what says it is a correctly stored 64-bit pointer — which is the whole
  point of widening VERT.  A first reading called it garbage; the second run
  is what corrected that.
- **`[6]` and `[7]` are correct when `gettree` finishes initialising them.**
  Instrumented, `INIT v=3 i=0 cell=-1` and `INIT v=3 i=1 cell=-1`, and the
  `LCHILD(from,THEN) = v` write **never executes** for this node.  So the
  corruption is in a *later phase*, not in `gettree`.

**And a per-phase dump found it in ONE run, after two phases of guessing.**
The order is `build()` → `gettree`, then `structure()` (`3.main.c`) →
`getreach` → `getflow` → `getthen`, with `mkthen` inside `getthen`.  Printing
the two cells between each phase:

```
BEFORE getreach [6]=-1 [7]=-1
AFTER  getreach [6]=-1 [7]=-1
AFTER  getflow  [6]=8  [7]=-1        <-- still CLEAN
```

So nothing was corrupt going into `getthen`, which refuted the whole
"something writes at the wrong stride during graph construction" line of
enquiry.  `getthen` reads `tch = LCHILD(v,THEN)` = −1 and
`fch = LCHILD(v,ELSE)` = 8, takes the `!DEFINED(tch)` arm, and calls
**`negate(v)`** — one line, `exchange(&LCHILD(v,THEN), &LCHILD(v,ELSE))`.
Comparing the clean pair `(8, −1)` against what `mkthen` then saw showed each
cell holding the correct low half and *the other one's* high half, which
names a half-width swap and nothing else.

Three things about the route generalise:

- **Bisect by phase before reasoning about mechanism.**  Three `fprintf`s in
  the phase driver did in one run what two rounds of reading declarations and
  one row dump had not.
- **Do not reach for lldb**: v8cc emits no unwind info, so a backtrace stops
  at frame #0 every time.  Every useful measurement here came from
  instrumenting the scratch copy.
- **Instrument a brace-less loop WITH BRACES.**  `gettree`'s initialiser is
  `for (i = 0; i < CHILDNUM(v); ++i)` with no braces, so a `fprintf` after the
  body lands *outside* the loop where `i == CHILDNUM(v)`, and `lchild`'s own
  assertion fires.  That reads as a new finding and is the instrument.

## beautify: TWO more, and the first is a header this port cannot reach

`structure` working is not `struct(1)` working.  The command is a shell script
that pipes `structure` into `beautify`, and beautify SIGSEGV'd on the first
real input while exiting 0 on empty stdin — an empty file never reaches the
lexer's `fixval()`.

**Defect 7: `extern int yylval` in `lextab.l:8`.  SECOND INSTANCE OF awk's
BUG, and the sweep `src/cmd/awk/PORTING.md` prescribes is what found it.**
This port's yacc emits `#define YYSTYPE long` (`y2.c:318`), so the grammar
side declares `long yylval` and the lexer declared the same object `int`.
Every value the lexer stores there is a pointer — `yylval = malloc(i)`,
`yylval = xxtbuff`, `xxp = yylval` read back, and `beauty.c:226` `free()`s it.
`malloc` was undeclared too, putting two truncations on one line.

```
grep -rnE '(extern|static)?[[:blank:]]*(int|short)[[:blank:]]+yylval' \
     src/cmd/*/*.l src/cmd/*/*.c
```

**And `char *malloc()` was missing from `b.h`**, which is beautify's whole
parse tree: `tree.c:12` is every NODE, `:16,:75,:89,:95` every literal string,
`beauty.y:304` the output buffer.  Declared in `b.h` because upstream's own
makefile already makes it the shared dependency of exactly those files.

**Defect 8: the CHECKED-IN parser defaults `YYSTYPE` to `int`, and neither
fix above could reach it.**  `beauty.c` was generated by 1985's yacc, so it
carries that yacc's default —

```c
#ifndef YYSTYPE
#define YYSTYPE int
#endif
YYSTYPE yylval, yyval;      /* and YYSTYPE yyv[YYMAXDEPTH], the value stack */
```

— so the entire value stack was 32-bit and every `$1` (each one a string
pointer: `putout(xxident,$1)`) lost its top half.  The `#ifndef` is upstream's
own escape hatch, so the fix is `-DYYSTYPE=long` on that object's compile line
and **no source edit at all**; the checked-in parser stays byte-identical.

- **`nm -S` is the instrument, and it is the only one that could have said
  so.** Measured: `_yylval` 4 → 8 bytes, `_yyv` 600 → 1200 (`YYMAXDEPTH` is
  150, so 150×4 → 150×8).  Nothing in the source changed, so a diff says
  nothing.
- **AND THE FIRST ATTEMPT SILENTLY DID NOTHING, for this file's own recorded
  reason: MAKE DOES NOT TRACK A RECIPE FLAG.**  Adding `-DYYSTYPE=long` to the
  rule leaves every object current, so beautify was relinked from the old
  `beauty.o` and crashed identically.  It is the `V8ROOT_DEFAULT` note
  arriving in a `-D`.  The control that caught it was compiling the same
  `#ifndef` fragment standalone — the flag worked there and not here, which
  is what pointed at the build rather than at cpp.  `touch` the source.

## What is left

Nothing, for struct.  All four integration steps are done: Makefile rules
(35 objects → `structure`, four + `libl.a` → `beautify`, the lex file, the
`/usr/lib/struct` install and the `/usr/bin/struct` script), 14 `tests/deps`
cases mutation-verified, 8 `tests/wavea` cases, and the crash-probe floor
re-measured with the two new binaries in the swept population.

The four `GOTO` cases are the load-bearing half of the wavea set, and they are
four rather than one on purpose: each asserts a *different* structured
construct, so a regression in one arm cannot hide behind the others.
