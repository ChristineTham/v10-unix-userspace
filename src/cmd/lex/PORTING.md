# lex

Builds with V8's own yacc and V8's own compiler, and runs. Its output is
truncated.

## Build structure

`once.c` and `ldefs.c` are **included**, not compiled — `lmain.c` starts with
`# include "ldefs.c"` and `# include "once.c"`. The object list is
`lmain.o y.tab.o sub1.o sub2.o header.o`, five objects plus the grammar.

Anything that builds the directory by globbing `*.c` will try to compile the two
included files on their own and fail — `once.c` uses `NCH`, which `ldefs.c`
defines. That is not a porting problem; it is the same shape as `tbl`'s `t..c`.

`lex` opens its skeleton as `/usr/lib/lex/ncform`, which the shim resolves inside
`$V8ROOT`.

## It no longer crashes

`left[]` and `right[]` are `int` arrays holding two different things: a node
index into the parse tree, and — for a character class — a **pointer** into the
packed-character array. `cfoll()` in `sub2.c` does

```c
char *p;
p = left[v];
```

and then scans it for a NUL. On the VAX an int held a pointer exactly; under
LP64 it holds half of one, and lex died scanning `0x4995010`. Both are `long`
now. Every allocation uses `sizeof(*left)`, so the sizes follow.

The compiler cannot rescue this one either — genuine `int` arrays, stored into
four bytes and reloaded from four, no conversion anywhere. Exactly `tbl`'s
`ct = reg(...)`, which is now the third instance of this shape.

The 8192-byte output was a red herring: it was simply the last flushed stdio
buffer before the crash, not a truncation.

## It works

`lex` builds and runs. On pic's own lexer:

```
1323/1700 nodes(%e), 3659/5000 positions(%p), 534/700 (%n), 29275 transitions
105/120 packed char classes(%k), 1544/1800 packed transitions(%a),
1217/1500 output slots(%o)
```

## The two bugs, and why only one was real

`myalloc` held `calloc`'s result in an `int`:

```c
register int i;
i = calloc(a, b);
if (i == 0) warning("OOPS - calloc returns a 0");
```

The shim's sbrk arena starts at **0x300000000**, whose low word is exactly zero,
so a truncated pointer from it *is* 0 and the test fired on a perfectly good
allocation. A rare truncation that announces itself; usually half a pointer is a
plausible-looking address and the program dies somewhere else entirely. Fixed to
`register char *i`.

The second was not a lex bug at all — it was a **stale object**, and it cost
far more than it should have.

`left[]` and `right[]` hold two different things: a node index, and, for a
character class, a `char *` into the packed-character array. They are widened to
`long` in `once.c`. But they are *allocated* in `parser.y`:

```c
left = myalloc(treesize, sizeof(*left));
```

`once.c` is `#include`d by `lmain.c`; `parser.y` becomes `y.tab.c`. Widening the
declaration changes `sizeof(*left)` from 4 to 8, so a `y.tab.o` built before the
change allocates `1700 * 4` bytes for an array that is then written as 8-byte
longs — a **2x overrun**, straight through the next block's malloc header.

What that looked like from the outside was `malloc` returning 0 for requests
above about 2800 bytes while smaller ones succeeded, which is a very convincing
impression of an allocator bug. It is not. Ritchie's malloc keeps its free list
*in* the arena, one link word per block, so the first thing a heap overrun
destroys is the allocator's own bookkeeping. The clobbering value was `0x351` —
849, a node index — sitting where a forward pointer belonged.

Two reproductions were written and both passed, because neither had lex's array
layout. What settled it was logging every header malloc wrote and comparing it
with the value found there later: malloc had written `0x1074a46b1`, exactly
right. That one measurement exonerated the allocator and turned an allocator bug
into a bounds bug.

**The build now encodes the dependency** (see the `v8lex` section of the top
Makefile): every lex object depends on `ldefs.c` and `once.c`, neither of which
is visible to the compiler as a dependency because both are `#include`d source
files. This is the third stale-object incident in this port. The rule that
follows from it: when a *type width* changes, rebuild everything, and make the
build know why.

## Where it is now

`myalloc` held `calloc`'s result in an `int`, which is fixed — and worth
recording for the coincidence:

```c
register int i;
i = calloc(a, b);
if (i == 0) warning("OOPS - calloc returns a 0");
```

The shim's sbrk arena starts at **0x300000000**, whose low word is exactly zero,
so a truncated pointer from it *is* 0 and the test fired on a perfectly good
allocation. A rare truncation that announces itself; usually half a pointer is a
plausible address and the program dies somewhere else entirely.

That was not the whole story. With `i` a `char *` and `calloc` properly declared
(`extern char *calloc()` in `ldefs.c`), the warning still fires nine times, so
**malloc is genuinely returning 0** for some of lex's requests. Some succeed —
the run reports `1323/1700 nodes` — so it is not failing outright.

Measured. Printing `a`, `b` and the result at every `myalloc`:

```
MA 000003e8 00000001 0000000300001010     1000 x 1   -> ok
MA 00000028 00000008 00000003000017f0       40 x 8   -> ok
MA 000002bc 00000001 0000000105adda80      700 x 1   -> ok, DIFFERENT region
MA 000002bc 00000004 0000000000000000      700 x 4   -> FAILS
MA 0000052c 00000008 0000000000000000     1324 x 8   -> FAILS
MA 00001388 00000004 0000000000000000     5000 x 4   -> FAILS
```

So this is **a malloc problem, not a lex problem**: small requests succeed and
requests from about 2800 bytes upward return 0. Note also that successful
allocations come from two different regions — `0x3000xxxx`, the shim's sbrk
arena, and `0x105adda80`, which is nowhere near it.

That second address is the thing to pull on. `src/libc/gen/malloc.c` is Ritchie
first-fit over `sbrk`, and `ialloc()` splices each new `sbrk` block into a
circularly-linked arena that it requires to be **monotonically increasing**. If
one block comes back far from the last, the ordering assumption breaks and the
search walks off — which is exactly the shape of "small ones work, larger ones
return 0".

Two reproductions tried, and **both work**:

* the four sizes straight through — 700, 2800, 10592, 20000 — all succeed from
  the arena;
* lex's shape — allocate six blocks, free them all, allocate six larger ones —
  also succeeds.

So it needs something more specific than size or a simple free/reallocate cycle.

The clue still unexplained is `0x105adda80`. That is nowhere near the sbrk arena
at `0x300000000`, and the only other memory malloc knows about is its own
`static union store alloca` — the **one-element** initial arena that `allocb`
and `allocp` start pointing at. If malloc is handing that out, it is returning
eight bytes of its own bookkeeping as if it were a buffer, and everything after
that is corruption rather than a clean failure.

Next: instrument `malloc` itself in a run of *lex* — not a reproduction — and
print `allocb`, `allocp` and the returned pointer per call. The question to
answer is how `allocp` comes to point at `alloca` again after the arena has
grown, since `ialloc()` sets `allocb = allocp = ` the new low block.