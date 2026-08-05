# grap

Builds and runs. It is the program that made this compiler implement **passing
a struct by value**, and the program that found the `TROFF` truncation in `pic`
(see `../pic/PORTING.md`). Both were latent for the whole port; neither was
reachable from anything already in the tree.

```
$ cat g1.grap
.G1
frame invis ht 2 wid 3 left solid bot solid
coord x 0,10 y 0,100
draw solid
1 1
2 4
3 9
10 100
.G2
$ grap g1.grap | pic | troff | head -3
x T 202
x res 972 1 2
x init
```

## One change: `PIC` was declared `<i>` and carries a `char *`

Every `.c` and `.h` file compiles unmodified — the diff against `third_party/`
is empty for all of them. `grap.y` needed one declaration changed, the same
LP64 fault as `pic`'s `TROFF`:

```
grapl.l:53   <A>pic{WS}.*   { yylval.p = tostring(yytext+3); return(PIC); }
grapl.l:148                   yylval.p = tostring(yytext);   return(PIC);
grap.y:8     %token <i> LINE ARROW CIRCLE DRAW NEW PLOT PIC NEXT
grap.y:74    | PIC          { codegen = 1; pic($1); }
```

`pic(s) char *s;` gets `yypvt[-0].i` — four bytes of an eight-byte pointer —
and dereferences it. Fixed by moving `PIC` to its own `%token <p>` line.

Two lexer rules reach it, and both matter:

- an explicit `pic` statement, which passes raw pic through;
- **any** troff request inside `.G1`/`.G2` that is not `.G2` itself.

Both segfaulted. Note what this means about the first version of this file: the
graphs used to bring grap up contained neither construct, so grap looked
completely correct while two of its statement forms could not run at all.

This was **not** found by hitting it. It was found by sweeping every grammar in
the tree after fixing the same shape in `pic`.

## The class, swept to the end

A yacc token declared with a scalar `%type` while the lexer stores a pointer
into `yylval.p`. Harmless on the VAX, where `sizeof(int) == sizeof(char *)`;
on LP64 it truncates the address.

Only grammars that declare types at all can have it. In this tree:

| grammar | typed decls | result |
|---|---|---|
| `pic/picy.y`  | 35 | `TROFF` — **fixed** |
| `grap/grap.y` | 24 | `PIC` — **fixed** |
| `ccom/cgram.y` | 4 | sound: `scan.c` writes `.nodep` only before `Return(TYPE)`, and `TYPE` is the only `<nodep>` token — plus the compiler reaches a byte-identical self-host fixpoint |
| `make/gram.y` | 3 | sound: a real `%union`, every member a pointer |
| `ccom/sty.y` | 6 | not built |
| `lex/parser.y`, `eqn/eqn.y`, `cpp/cpy.y` | 0 | untyped, so covered globally — see below |

The untyped grammars assign pointers to a bare `yylval` too (`eqn/lex.c:42`
does `yylval = (int) &token[0];`). They are safe because this port already
patched V8's yacc: `src/cmd/yacc/y2.c:302` emits `#define YYSTYPE long` rather
than `int` for a grammar with no declared types. That fix covers the untyped
case for the whole tree at once; the **typed** case has to be fixed per token,
which is why these two survived it.

Reproduce the sweep:

```
grep -n 'yylval\.p *=' src/cmd/*/*.l          # what the lexer stores
grep -nE '^%(union|type|token)[ \t]*<' src/cmd/*/*.y   # what the grammar declares
```

## What it needed: STARG

`plot.c` stopped the build at

```
compiler error: gencode: unimplemented operator 99 (STARG)
```

`STARG` is pass 1's node for an argument that is a struct passed by value.
`grap`'s `Point` is

```c
typedef struct { struct obj *obj; double x, y; } Point;   /* 24 bytes */
```

and `plot.c` calls `line(type, p1, p2, desc)` with two of them. 156 Wave A
programs, all of Wave B and the rest of Wave C never do this, which is why the
gap survived to here — the note above `acctype()` in
`compiler/ccom-arm64/gencode.c` had said so since the back end was written.

Smaller aggregates never reached it: pass 1's `simpstr()`
(`common/optim.c:71`) rewrites a struct argument to a plain scalar when its size
is exactly `SZCHAR`, `SZSHORT`, `SZINT` or `SZLONG`, so 1-, 2-, 4- and 8-byte
structs become ordinary `FUNARG`s. Only sizes outside that set become `STARG`.

## The convention, and why

The back end now copies the aggregate into **consecutive 8-byte argument
slots** — the V8/VAX convention, where the VAX pushed the struct whole with
`subl2 $size,sp; movc3 $size,src,(sp)` (`ccom/vax/stin:276`,
`ccom/vax/local2.c:101`).

The alternative was AAPCS64's rule: composites over 16 bytes passed *by
reference*, 16 bytes and under in one or two registers, all-float structs in up
to four float registers. That is right for a call into host code and wrong here.
v8cc already passes every argument positionally in `x0`–`x7` with a spill area,
deliberately, because that is what makes V8's own `printf(fmt, args)` work —
Apple's ABI puts variadic arguments on the stack. Adopting Apple's aggregate
rule would have left the compiler following one convention for scalars and
another for structs, which is neither convention. PLAN.md §4f records the
decision and CLAUDE.md's exception list is where the V8/host boundary lives.

**The callee needed no change at all.** Pass 1's `oalloc()`
(`common/pftn.c:1516`) already lays a struct parameter out as `tsize()`
contiguous bytes in the argument area, because `BACKPARAM` is undefined for this
target. The two halves had agreed all along; only the caller was missing.

Three things in the implementation are worth knowing:

- `countargs()` counts **slots, not arguments**, since one struct is several.
  Getting this wrong is silent: with a struct counted as one slot the program
  builds and links and emits nothing. `tests/wavec` catches it (verified by
  mutation).
- The copy goes through `argslot()` one word at a time rather than as a block
  move, because `argslot()` is discontinuous — slots 0–7 are scratch that gets
  loaded into `x0`–`x7` just before the branch, slots 8 and up are the real
  outgoing area at `sp+0`. A struct straddling that boundary lands in two
  places, and per-word placement makes that fall out for free.
- `stn.stsize` **arrives already rounded** to a multiple of `ALSTACK`, because
  `argsize()` (`common/catch2.c:177`) mutates the node in place with `SETOFF`.
  Measured: a 12-byte struct presents as 128 bits, a 5-byte one as 64. So the
  copy reads up to 7 bytes past the object — which is what the VAX did too, from
  the same already-rounded field, just with `ALSTACK` 32 instead of 64. Not a
  deviation, and narrowing it would mean patching pass 1 to round a copy, which
  is a change by taste rather than one forced by the target.

## What it also closed: `acctype()`'s int-member limitation

Implementing STARG made a documented compiler limitation reachable, and grap's
own test cases found it the same day. `acctype()` widens an int `VPARAM` to its
whole 8-byte slot, which is right for an undeclared K&R parameter holding a
pointer and wrong for an int **member** of an aggregate parameter. Its note had
said the case was unreachable *"for aggregates larger than 8 bytes — those hit
the unimplemented STARG path first"*. That stopped being true.

Symptom, from a 12-byte struct of three ints:

```
ldr	x10, [x29, #24]		; v.a -- 8 bytes, so it also gets v.b
ldr	x10, [x29, #28]		; v.b -- and v.c
```

The sum was right in its low 32 bits and rubbish above them, so it only looked
wrong once something read it as a long. Fixed in the back end, not in any
authentic source: `bfcode()` in `compiler/ccom-arm64/local.c` records the byte
ranges of aggregate parameters from the declared types pcc hands it, and
`acctype()` asks before widening. PLAN.md §4f has the full account.

## grap.defines is runtime data, not an include

`main.c:7` has `char *lib_defines = "/usr/lib/grap.defines";` and warns if it
cannot open it. It is installed like troff's macros and spell's word lists.

An earlier version of the Makefile block called it an `#include`d non-header and
made every grap object depend on it. That was wrong twice over, and wrong about
the one bug class this tree is most careful about. There is **no** `#include`d
non-header in grap; step 4 of the porting checklist finds nothing:

```
grep -rnE '#[ \t]*include[ \t]*"[^"]*"' src/cmd/grap | grep -v '\.h"'
```

`tests/deps` now asserts both halves: touching `grap.defines` must make the
installed copy stale and must **not** make `plot.o` stale.

## What is still open

- Upstream's own makefile is not used yet (task B6). It names `prevy.tab.h` —
  the "y.tab.h under another name" trick `pic` also uses — which the top-level
  rules do not reproduce, for the same reason recorded in the pic block.
- `grap.202` is byte-identical to `grap.defines` upstream (same blob hash in
  `PROVENANCE`); only the latter is installed, since nothing reads the former.
