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

`structure` SIGSEGVs on its first line of real Fortran.

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

**Defect 2, NOT fixed: `fixvalue` and the rest of `1.hash.c`.**  Fixing the
allocators moved the crash rather than removing it — `EXC_BAD_ACCESS at
0x1ee06290` in `fixvalue`, whose signature is

```c
fixvalue (x,ptr)
long x;
int ptr;			/* a POINTER parameter declared int */
```

and whose body declares `int *temp1, *temp2, index, temp0;`.  The file uses
`int` for pointers as a matter of style, so this is not one more line but a
pass over the module — and an unknown number of the other 39 files may do the
same.

## Where to start next

**The crash moving is the useful signal**: each fix reveals the next, so the
loop is `build → run → lldb → fix declarations → repeat`, and it terminates.
Do not guess at the count; `structure` processes a six-line Fortran program, so
the reproducer is instant.

Two instruments are better than lldb for this once the program runs at all:
`tests/v8ccom`'s `trunc-sweep.awk` over the linked binary names every call whose
result is truncated, and the `INTFNS` list separates "returns int" from "returns
a pointer nobody declared".  That sweep is what caught `last`'s `asctime` and
`qed`'s `getnum`.

**And do not add it to the Makefile until it runs.**  A program installed into
the world that dies on its own primary input is worse than one that is not
there: the crash probe would gain a floor entry, `tests/wavea`'s
imported-equals-installed guard would go green on a lie, and the world would
grow a command that cannot be used.
