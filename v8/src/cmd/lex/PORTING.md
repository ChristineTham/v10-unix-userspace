# lex

Builds with V8's own yacc and V8's own compiler, and runs. **This line used to
say "Its output is truncated", and it is no longer true**: measured today, a
two-rule specification exits 0 and writes 6758 bytes, and a 62-rule one with a
`%{ %}` block exits 0 and writes 9362. Whether that closes the malloc
investigation at the end of this file is *not* established — it may simply mean
nothing here reaches the arena growth that section describes — so the section
stays open rather than being declared solved.

Also not established, and separate: there is no `libl.a` in the rootfs, so the
generated scanner cannot be linked without supplying `main` and `yywrap`.

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
## The 53 crashes are THREE bugs, and only one of them is ours to fix

`tests/crash-probe.sh` reported lex dying on all 53 invocations — bare and
every single-letter option — and PLAN.md recorded that as "one root cause: an
empty specification faults in `fprintf` on a null `fout`". Re-measured, it is
three distinct faults, and the split matters because they have different
answers:

| n | invocations | fault | site |
|---|---|---|---|
| 40 | every letter outside `rRcCtTvVfFnN` | `fflush(fout)` | `sub1.c`, in `warning()` |
| 11 | bare, and `-c -C -f -F -n -N -r -R -v -V` | `fprintf(fout, ...)` | `header.c:84` (`ctail`), `:93` (`rtail`) |
| 2 | `-t -T` | `free(NULL)` | `lmain.c:158` → `free2core` |

**The probe could not tell them apart, and that is a limitation of the probe
rather than of the audit.** It feeds every program `/dev/null`, so for a
program that requires input all 53 invocations also reach the empty-spec path.
Fixing the first bug therefore changes the probe's count by *zero* — the 40 now
die further along, at the second. What the fix actually buys is only visible
with a real specification, which the probe never supplies.

### The 40 are ours: address 0, with a measured VAX answer

`warning()` ends `fflush(errorf); fflush(fout); fflush(stdout);`. `fout` is
NULL until `lgate()` opens it (`sub1.c:99`), and `lgate()` runs only from the
five section-one sites in `parser.y` that see `%%`, `%T`, `%{`, `%s` or an
indented code line. The unknown-option arm runs long before any of them — so

```
lex -a spec.l
```

SIGSEGV'd on a specification `lex spec.l` compiles perfectly.

The VAX answer is measured, not assumed, and it is **"do nothing"**. V8 binaries
are ZMAGIC (0413), so `N_TXTOFF` is 1024 and virtual address 0 is the first
byte of *crt0*, not the a.out header — `usr/sys/sys/text.c:132` reads from
`BSIZE(0)` into `u_base` 0. Those 16 bytes are byte-identical in every V8
binary; read through the VAX `struct _iobuf` (`_cnt` 0, `_ptr` 4, `_base` 8,
`_flag` 12) they give `_flag` `0xd050`. `fflush` opens

```c
	if ((iop->_flag&(_IONBF|_IOWRT))==_IOWRT && (base=iop->_base)!=NULL && ...
```

with `_IOWRT` 02 and `_IONBF` 04. `0xd050 & 06` is 0, which is not `_IOWRT`, so
the `&&` short-circuits before `_base` is ever read and `fflush` returns 0
having touched nothing. `if(fout) fflush(fout)` restores exactly that.

Guarded at the **caller**, as `quot`'s `strcmp` and `ncheck`'s `atol` were. A
null check inside libc's `fflush` would reproduce the same answer for every
caller — and would make the next bug of this shape invisible.

### The 13 are upstream's, and are deliberately left alone

With no `%%` anywhere, `ptail()` reaches `ctail()`'s `fprintf(fout, ...)` with
`fout` still NULL. On a VAX that got past the `_IONBF` test and `_doprnt` wrote
through `_ptr`, which those crt0 bytes make `0x08aed05e` — about 145 MB, far
past a 56 KB lex's break, so `SEGFLT` and SIGSEGV. `free(NULL)` under `-t` is
the same story: `allocp = --p` gives `0xFFFFFFF8`, VAX system space from user
mode, PROTFLT and SIGBUS.

So **a VAX crashed here too**, which puts this with `bcd` and `ls.c:259` rather
than with `quot`: upstream's defect on upstream's hardware, and S1 says record
it rather than patch it. The grammar already calls a missing `%%` a syntax
error (`lexinput: defns delim prods end`, `delim: DELIM`), but the `| error`
alternative recovers and `yyparse` returns 0, so `lmain.c:68`'s
`if(yyparse(0)) exit(1)` never fires and `main` runs on over state that only
the `%%` branch initialises. A one-line `if(sect == DEFSECTION) error(...)` at
`parser.y:676` would close both — it is written down here so the option is
known — but it restores no VAX behaviour, so taking it would be a departure
from upstream justified only by the probe.

`tests/wavea` asserts both halves: that an unknown option now warns and still
produces byte-identical output, and that an empty specification writes no
`lex.yy.c` — which is the part that is true on both machines.
