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
anywhere for the compiler to correct. The compiler limitation is recorded at
`acctype()`, with what to do if a second case appears.
