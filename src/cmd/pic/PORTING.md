# pic

Builds and runs. It is the first program in the tree that needs **both** of V8's
own generators: `yacc` for `picy.y` and `lex` for `picl.l`, so it could not be
attempted until lex worked (see `../lex/PORTING.md`).

```
$ cat p1.pic
.PS
box "hello"; arrow; circle "world"
.PE
$ pic p1.pic
.PS 0.500i 1.750i
\h'0.375i'\v'0.250i'\v'.2m'\h'-\w'hello'u/2u'hello\h'-\w'hello'u/2u'
...
\h'1.500i'\v'0.250i'\v'.2m'\h'-\w'world'u/2u'world\h'-\w'world'u/2u'
```

## Second change: `TROFF` was declared `<i>` and carries a `char *`

Found by `grap`, not by pic's own tests, and that is the point of it.

`picl.l:59` matches a troff request inside a picture and stores a string:

```
<A>^".".*	{ yylval.p = tostring(yytext); return(TROFF); }
```

but `picy.y:17` declared the token's semantic type as an **int**:

```
%token	<i>	TROFF	10
```

so yacc emitted `troffgen(yypvt[-0].i)` — reading four bytes of an eight-byte
pointer. `troffgen(s) YYSTYPE s;` then hands `s.p` to `savetext()`, and the
truncated pointer is dereferenced later when `print.c` prints the saved text.

On the VAX this was invisible: `sizeof(int) == sizeof(char *) == 4`, so `.i` and
`.p` were the same four bytes and the declaration was a documentation error with
no consequence. On LP64 it drops the top half of the address. Measured under
lldb rather than argued:

```
stop reason = EXC_BAD_ACCESS (code=1, address=0x4a57c50)
frame #0: pic`_doprnt + 3852
```

`0x4a57c50` is a heap pointer — `0x104a57c50` — with the leading `1` gone.

The fix is the declaration, `%token <p> TROFF 10`, because that is what the
lexer actually stores. Nothing else changes: `makenode(TROFF, 0)` and
`case TROFF:` in `print.c` use the token *number*, not the semantic value.

**The sweep matters more than the fix.** Every lexer action in `picl.l` that
writes `yylval.p` was checked against its `%token` declaration:

| action | token | declared | |
|---|---|---|---|
| `picl.l:59`  | `TROFF`     | `<i>` | **wrong** |
| `picl.l:178` | `DOSTR`     | `<p>` | ok |
| `picl.l:182` | `DEFNAME`   | `<p>` | ok |
| `picl.l:187` | `THENSTR`   | `<p>` | ok |
| `picl.l:189` | `ELSESTR`   | `<p>` | ok |
| `picl.l:239` | `VARNAME`   | `<p>` | ok |
| `picl.l:242` | `PLACENAME` | `<p>` | ok |
| `picl.l:248` | `TEXT`      | `<p>` | ok |

`TROFF` is the only one **in pic**. Reproduce the sweep with
`grep -n 'yylval\.p *=' picl.l` against the `%token` block.

Sweeping the rest of the tree afterwards found the identical fault in `grap`'s
`PIC` token, which had never been hit either. `../grap/PORTING.md` has the
whole-tree table, including why the untyped grammars (`lex`, `eqn`, `cpp`) are
safe: this port already changed V8's yacc to emit `#define YYSTYPE long`
instead of `int` when a grammar declares no types. That covers the untyped case
globally; the typed case has to be fixed one token at a time.

## Why nothing caught this for so long

pic passed 16 wavec cases and its own suite throughout. None of them ever put a
line beginning with `.` **inside** a `.PS`, and outside a picture the same line
is handled by `main.c`'s own loop, which never touches `yylval`. So the whole
token path was unreachable from any input the tree contained.

`grap` emits `.lf` on every graph — it is how a preprocessor tells troff which
source line an error belongs to — so `grap | pic` hit it immediately, and hit it
first with `grap` alone still looking perfectly correct. The pipeline is the
test; a preprocessor whose output is never fed downstream is not tested at all.
`tests/wavec` now runs `grap | pic | troff` and asserts drawing commands come
out the far end.

## One change: a union that grew

`YYSTYPE` is pic's yacc stack type, and also the type of an attribute's value:

```c
typedef union {		/* the yacc stack type */
	int	i;
	char	*p;
	obj	*o;
	float	f;
} YYSTYPE;
```

On the VAX every member was 4 bytes and so was the union. Under LP64 the two
pointers make it **8**, and the int and float members no longer fill it.

That matters because of how the attribute table is cleared. Attributes
accumulate as the grammar reduces `attrlist`, and the table is emptied between
statements by the action on `prim ST`:

```
prim ST		{ codegen = 1; makeiattr(0, 0); }
```

`makeiattr` builds a `YYSTYPE` on the stack, sets `.i`, and passes it **by
value**; `makeattr` recognises the clear by testing the whole thing:

```c
if (type == 0 && val.i == 0) {	/* clear table for next stat */
	nattr = 0;
	return;
}
```

With `val.i = 0` writing only four of the union's eight bytes, the other four
held whatever was on the stack. The clear silently did not happen, so attributes
accumulated across statements and every object inherited the text of every
object before it:

```
box "hello"; arrow; circle "world"
   -> hello on the box, hello on the arrow, hello AND world on the circle
```

`makeiattr` and `makefattr` now clear the union before setting their member.
`makeoattr`, `maketattr` and `makevattr` set `.p`/`.o`, which are already the
full width, and are unchanged.

## Why it is fixed here and not in the compiler

Strictly, `val.i == 0` should compare four bytes and the compiler should not
have looked at the padding. It did, for a reason that is deliberate elsewhere:
the ARM64 back end reads an `int` **parameter** at its full 8-byte argument slot,
because K&R gives an undeclared parameter the type `int` and 271 parameters
across 109 files in `usr/src/cmd` use one to hold a pointer. See the comment on
`acctype()` in `compiler/ccom-arm64/gencode.c`.

Pass 1 hands pass 2 the parameter's own node retyped to the member's type, so a
4-byte member at offset 0 and a 4-byte parameter arrive as the same node.
Separating them needs the declared type, which only pass 1 has.

The fix went to the source because **this is where the VAX/LP64 change is**: the
union itself changed size. It is the same shape as `tbl`'s `ct = reg(...)` and
`lex`'s `left[]` — a width decided by a declaration, with no conversion node
anywhere for the compiler to correct.

**The compiler limitation is now fixed as well**, so this source change is no
longer load-bearing for that reason — it is still right on its own terms. When
`STARG` was implemented the same widening bit a 12-byte struct of three ints,
which was the second case `acctype()`'s note said to wait for. Pass 1 now hands
the declared types over: `bfcode()` in `compiler/ccom-arm64/local.c` records the
byte ranges of aggregate parameters and `acctype()` declines to widen inside
them. `tests/v8ccom` carries this union verbatim as a case.
