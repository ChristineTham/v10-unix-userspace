# awk

Aho, Weinberger and Kernighan's awk, `usr/src/cmd/awk`. Nine translation
units, one of which does not exist until the build makes it. It is the largest
single gain in the world-shape count so far and the first program here that
carries **addresses in objects declared as integers in four separate places** —
because on a VAX that was the same thing.

## What changed, and why each was forced

Three files, and every change is the same one fact: `sizeof(char *)` is 8 and
`sizeof(int)` is 4.

### 1. `awk.lx.l:24` — `extern int yylval` against an eight-byte object

Upstream is `extern int yylval;`. **yacc defines that object, and this port's
yacc defines it as a `long`**: `src/cmd/yacc/y2.c:318` emits
`#define YYSTYPE long` for a grammar that declares no types, which is the
global fix for the `yylval.p` token bug recorded in
`src/cmd/grap/PORTING.md`. So the lexer's declaration described four bytes of
an eight-byte object.

That alone is a width bug. What makes it fatal is that **seven of the stores
are pointers**:

| line (upstream) | what it stores |
|---|---|
| 90 (76) | `lookup("$0", symtab)` — the whole-record Cell |
| 91 (77) | `fieldadr(atoi(yytext+1))` — a `$N` field Cell |
| 93 (79) | `setsymtab(...)` for `NF` |
| 96 (82) | `setsymtab(...)` for a numeric constant |
| 139 (125) | `setsymtab(...)` for every bare name |
| 164 (150) | `tostring(cbuf)` for a regular expression |
| 172 (158) | `setsymtab(...)` for a string constant |

and the grammar reads every one back as `(Node *) $1`. On a VAX, where `int`,
`char *` and `YYSTYPE` are all four bytes, the idiom is exact. Here the lexer
would write the low half of a heap address and the parser would read all eight
bytes of it — so **every variable, number, string, field and regex in an awk
program** would be a wild pointer. `extern long yylval;` and `(long)` on the
seven casts.

This is the `extern float atof()` class: a *declaration that lies about a
type*, not a cast that loses one. The compiler cannot help, because the
declaration is what it is told to believe.

### 2. `b.c:38` — `int rlxval`, the regex lexer's value

`relex()` stores a character value here for `CHAR` and
`(long) tostring(cbuf)` — a heap pointer — for `CCL` and `NCCL` (`b.c:637`,
upstream `:620`). `primary()` then hands it to `cclenter()`, which takes a
`char *`. Four bytes held both on a VAX. `long` here.

### 3. `awk.h:177` — `int lval` in `struct rrow`, the DFA table

The strongest of the four, and **b.c's own header comment is what says so**:

> leaf (CCL, NCCL, CHAR, DOT, FINAL, ALL): left is index, right contains value
> **or pointer to value**

`cfoll()` stores `(long) right(v)` into `lval` (`b.c:222`, upstream `:205`) and
`cgoto()` reads it back as `(char *) f->re[i].lval` for `member()` (`b.c:683`,
upstream `:666`). So a character-class string round-trips through the field.
Widened to `long`.

Safe to widen without further thought, and the reason is worth stating rather
than assuming: `struct fa` is an in-memory table private to the regex engine.
It is not an on-disk record (`src/include/PORTING.md`'s rule) and it does not
cross the shim seam, so it has only one end and we control it.

### 4. `run.c:3` — the `execute` macro reads before the null check

The one that mattered most, and it is not an LP64 bug at all. Upstream:

```c
#define	execute(p)	(isvalue(p) ? (Cell *)((p)->narg[0]) : real_execute(p))
```

`real_execute()` opens with `if (u == NULL) return(true);` — so a null
statement is *expected* and harmless there. But `isvalue(p)` is
`(p)->ntype == NVALUE`, which reads offset 0 of `p` **before** that guard is
ever reached.

A null `p` is not an edge case. `awk.g.y:177` makes an empty `pa_stats` the
integer 0, and a program with no main pattern-action rule reduces through it —
so `BEGIN{...}`, `END{...}`, `{}` and `''` all put a null in `narg[1]` of the
PROGRAM node, and `program()` executes exactly that once per input record.
Bell Labs' own null guard is the evidence this was reached in 1985 and did not
fault: a VAX maps virtual 0 (the first byte of crt0 in a ZMAGIC binary), so the
read returned a word of program text, which is not 1, so the conditional fell
through to `real_execute` and its guard answered.

So `awk 'BEGIN{print 1}'` — the single most common awk invocation there is —
was a SIGSEGV on any input. Address-0 class, the eighteenth member; the fix is
`(p) != NULL &&`, which reproduces the VAX exactly.

Note what hid it: with **empty stdin** the record loop never runs and the
program exits 0. So `awk 'BEGIN{print 1}' < /dev/null` was fine and
`echo x | awk 'BEGIN{print 1}'` was not, which is why the first hour of testing
this port did not meet it.

### 5. `main.c` — three `argv[1]` reads past the end of the option loop

Every option here consumes an argument, so an option in the **last** position
leaves `argv[1]` pointing at the argv terminator, which is a null pointer.
Upstream then hands it to `fopen()` (`-f`), indexes it (`-F`), or assigns it to
`lexprog` for the scanner to walk (the no-`-f` case). That is the
`ncheck`/`icheck`/`dcheck` shape a fourth time, and here it is **63 of the 64
single-letter options** plus `--` — because the option loop consumes an unknown
letter silently and falls through to the `lexprog` assignment.

A VAX read the empty string at address 0 in all three, so `ARG1` supplies one.
Restoring the *answer* rather than the absence of the fault matters, and here it
is visible: `awk -a` is then the empty program (reads its input, prints nothing,
exits 0) and `awk -f` is `fopen("")`, which in the shim as in V7 is **the
current directory** — awk parses raw directory bytes and says `syntax error at
source line 1`, exit 2. Both are what V8 did.

`dprintf("program = |%s|\n", argv[1])` two lines below is deliberately **not**
changed: our `doprnt.c` guards a null `%s` and prints `(null)`, so it does not
fault, and a change that is not forced by the target does not go in. It is
visible under `-d` only, and it is a divergence from V8 — where `%s` of address
0 printed the empty string — that belongs to `doprnt.c` rather than to awk.

## What was eliminated by measurement

**`maketab.c` needs no change, and the obvious one was written and then
removed.** It calls `(char *) malloc(strlen(name)+1)` with `malloc`
undeclared — the shape that `tests/v8ccom`'s rootfs sweep catches as a
truncated pointer return, and which that sweep structurally *cannot* see here
because maketab is a build tool and is never installed. A `char *malloc();`
was added on that reasoning.

Measured, it is not needed:

- the generated `proctab.c` is **byte-identical** with and without it, and
- the emitted assembly at the call site is **identical instruction for
  instruction** —

```
	bl	_malloc
	ldr	x9, [sp, #256]
	mov	x10, x0        <- the whole register
	str	x10, [x9]
```

The distinction is that **an explicit cast applied directly to the call is not
a use of the int type**. v8cc never materialises the result as a 32-bit
quantity, so there is nothing to truncate. Compare `last`, where the sweep did
find a real one: `asctime(gmtime(&delta))+11` does arithmetic on the int, and
the int-ness is load-bearing. So the rule is narrower than "an undeclared
pointer return is truncated" — it is truncated **where the int type is used**.

Under S1 a change that is not forced by the target does not go in, so
`maketab.c` is byte-identical to upstream and this paragraph is the record.

## What it found outside itself

`tran.c:271` prints an integral value with `sprintf(s, "%.20g", vp->fval)`, and
this port answered `3.0000000000000000000`. That is not awk's defect: our
`doprnt.c` never stripped `%g`'s trailing zeros, and its own comment said it
did. V8's `doprnt.S:625-631` strips them, and Berkeley's `gcvt.c` in the same
directory does too. Fixed there, with a second defect found beside it (`%g`'s
precision counts significant digits, so its e-style form is `%.(P-1)e`, not
`%.Pe`). Five cases in `tests/libv8c`, and one already-shipped program was
visibly wrong all along — grap's tick labels read `1.00000`, `1.50000`,
`2.00000`, and `tests/wavec` had frozen that in an expectation.

## The build, and the thing that makes it unusual

`proctab.c` holds `Cell *(*proctab[])()` — one function pointer per grammar
token, in token order — and is written by `maketab`, which the build compiles
and runs, and which reads `y.tab.h`. So there are **two generators in series**:

```
awk.g.y  --yacc--> y.tab.c + y.tab.h
maketab.c + y.tab.h --v8cc--> maketab --run--> proctab.c --v8cc--> proctab.o
```

Nothing else in the tree has that shape. Upstream's makefile opens with four
lines admitting theirs does not get it right:

> This makefile is wrong -- it doesn't properly recompile everything when a new
> token is added to awk.g.y.  Watch out!

Ours does, and `tests/deps` has eight cases saying so, the load-bearing one
being `awk grammar -> proctab.o`, which walks the whole chain in a single
assertion.

Two details of the Makefile block are decisions rather than mechanics.

**maketab is built by V8's cc and run**, not by the host's. That is upstream's
own mechanism — `cc maketab.c -o maketab` in a world where `cc` is V8's — and
the precedent is already here: `$(YACC)` and `$(LEX)` in the grap and pic
blocks are V8 binaries this Makefile executes. It is the third kind of
build-time V8 program in the tree.

**The redirection goes through a temporary.** `.DELETE_ON_ERROR` is not set in
this Makefile, and `> proctab.c` creates the file *before* maketab runs — so a
maketab that died would leave an empty `proctab.c` newer than its
prerequisites, and the next make would call it current. That is the stale-object
failure this tree has already paid for four times, arriving through a shell
redirect rather than through a rule.

## Where it installs

`/usr/bin`, and nothing here chose it: the shipped tree has `usr/bin/awk` and
no `bin/awk`, awk is in none of `Admin/binfiles`, `etcfiles` or `libfiles`, so
`$(call v8dest,awk)` answers by fall-through — and awk's own makefile agrees
(`cp a.out /usr/bin/awk`).

Note that `tests/wavea`'s makefile-versus-`Admin/dest` sweep does **not** see
awk, and correctly so: its parser requires `cp <progname> <dest>`, and awk's
line copies `a.out`. That is the same shape as `eqn`, whose makefile target is
`a.out` too. Widening the parser to follow `a.out` is a separate measurement,
not a fix owed here — it would change what that case's disagreement set means.

## Still open

- **`tokname()` is dead and its generated body has an off-by-one.** maketab
  emits `return printname[n-256];` while filling the array indexed by
  `tok-FIRSTTOKEN`, and FIRSTTOKEN is 257 — so every name it returns is the
  next token's. Upstream's bug on upstream's hardware, so `S1` leaves it.
  Dead was *measured* rather than argued: the only call is in the lexer's
  `RET()` macro, that macro's `tokname` arm is inside `#ifdef DEBUG`, and
  **awk.lx.l is the one source file here that does not define DEBUG** (the six
  `.c` files each do it on line 1). `nm` agrees — `proctab.o` defines
  `_tokname` and no object in the program has it undefined.
- **154 shift/reduce and 1 reduce/reduce conflict** from our yacc. Upstream's
  grammar is famously ambiguous and its own makefile does not suppress the
  message either; the parser is correct on everything in `tests/wavea`. Not
  compared against what V8's yacc reported in 1985, because nothing records it.
- **No `-f progfile` case yet.** The option works; there is simply no case for
  it, and the ones that exist were chosen to aim at the four pointer sites.
