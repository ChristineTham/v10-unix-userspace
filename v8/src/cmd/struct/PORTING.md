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

`structure` SIGSEGVs on its first line of real Fortran.  **It no longer does.**
Measured, on four inputs of increasing shape:

| input | before | now |
|---|---|---|
| straight-line assignment | SIGSEGV in `fixvalue` | **correct Ratfor**, exit 0 |
| two statements | SIGSEGV in `fixvalue` | **correct Ratfor**, exit 0 |
| a plain `IF` | SIGSEGV in `fixvalue` | **correct Ratfor**, exit 0 |
| `IF … GOTO` (a loop) | SIGSEGV in `fixvalue` | assertion in `mkthen` |

The baseline column is a *control*, not a memory: the pre-change sources were
rebuilt in a scratch tree and run on the same four files, and **every one of
them died in `fixvalue`, including the trivial one**.  That is what says the
remaining `mkthen` failure is newly *reachable* rather than a regression.

It is still not in the Makefile, for the reason below that has not changed:
`struct` exists to turn `GOTO`s into loops, so a `struct` that fails on a
`GOTO` fails at the one thing it is for.

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

## Where to start next: the same loop, one more turn

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

The phase order is `build()` → `gettree`, then `structure()` (`3.main.c`) →
`getreach` → `getflow` → `getthen`, and `mkthen` is inside `getthen`.  So the
corrupter is **`getreach` or `getflow`**.  `getflow`'s `fixflow`
(`3.flow.c:54`) is the one that writes `LCHILD(v,i) = x` with
`x = makebr(z)` — the prime suspect, and the next thing to instrument.

**Do not reach for lldb**: v8cc emits no unwind info, so a backtrace stops at
frame #0 every time.  What worked was instrumenting the scratch copy.

**And instrument a brace-less loop with braces.**  `gettree`'s initialiser is
`for (i = 0; i < CHILDNUM(v); ++i)` with **no braces**, so a `fprintf` added
after the body lands *outside* the loop where `i == CHILDNUM(v)`, and
`lchild`'s own assertion fires.  That reads as a new finding and is the
instrument.  Third instance here of *an instrument you wrote is a suspect*.

**And still do not add it to the Makefile.**  A program installed into the
world that dies on its own primary input is worse than one that is not there:
the crash probe would gain a floor entry, `tests/wavea`'s
imported-equals-installed guard would go green on a lie, and the world would
grow a command that cannot be used.  `struct` restructures `GOTO`s; until the
`GOTO` case runs, it does not do its job.
