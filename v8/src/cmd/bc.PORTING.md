# bc(1) -- porting notes

`bc` is an arbitrary-precision calculator language, and it is not an
interpreter: it **compiles** its input into `dc(1)`'s reverse-Polish language,
forks, and execs the interpreter with a pipe between them. That makes it the
first program this port has imported whose correctness depends on a *second*
V8 binary already being installed, and `dc` was.

Upstream: `usr/src/cmd/bc.y`, blob `6dccc5e1a9d260e1f38c4fd1d445c6a528b1a96e`,
625 lines. A bare `*.y` in `cmd/` with no directory and no makefile -- the
second this port has imported after `egrep` -- so its build description
upstream is `Admin/Mk`'s `*.y` arm (`yacc`, `cc -o $B y.tab.c -ly`, install).
It is in none of `binfiles`, `etcfiles` or `libfiles`, so `Admin/dest` answers
`/usr/bin` by **fall-through**; the shipped tree agrees at `v8/usr/bin/bc`.

No `-ly`: liby exists to supply `main()` and `yyerror()`, and `bc.y` defines
both itself. Same reading as `egrep`.

## The one change, and it is a single type

Nine declarations move from `int` to `long`. They are one fact, and the fact is
that **b_space's cell is one machine word and the word grew**.

`bundle(n, ...)` copies its variadic arguments into `b_space`, an arena of
compiled `dc` fragments, and returns a pointer to the run it wrote. Every cell
holds a **pointer**: either a `char *` literal (`"S"`, `"s."`, or `getf()`'s
two-byte name out of `funtab`) or a pointer to a nested bundle. `routput()`
tells the two apart by asking whether the value lies inside `b_space`, which is
why the cell has to be able to hold either.

On a VAX a pointer was four bytes and `int` was the right cell. Here it is
eight, and an `int` cell truncates every fragment address it is handed.

**Widen the TYPE, not the uses.** That is `struct(1)`'s `VERT` verdict, and
`efl`'s `typedef int *ptr` seen from the other side -- one `#define`-sized
change with the declarations cascading from it:

| | upstream | here |
|---|---|---|
| the arena | `int b_space[3000]` | `long b_space[3000]` |
| its cursor | `int *b_sp_nxt` | `long *b_sp_nxt` |
| the union slot | `int *iptr` | `long *iptr` |
| the temporaries | `int *ttp`, `int *pre, *post` | `long *` |
| `bundle` | returns `int *`, locals `int i, *p, *q` | `long *`, `long i, *p, *q` |
| its readers | `routput`, `output`, `conout` take `int *p` | `long *p` |
| the forward declaration | `int *bundle()` | `long *bundle()` |

## ...and the argument walk is the same fact, which is why one change fixed both

`bundle`'s body opens

```c
p = &a;  i = *p++;
while (i-- > 0) *b_sp_nxt++ = *p++;
```

-- the address of the first K&R parameter, strided forward through the rest.
Exact on a VAX, where arguments sat four bytes apart, and wrong here because
v8cc spills `x0`-`x7` into **eight**-byte slots (`SZARG` is `SZLONG`).

This is the **forward** form of the idiom, which libc already walks with an
eight-byte type -- `doprnt.c`'s `NEXTLONG`, with `fprintf.c` handing it
`&args`. CLAUDE.md records the *indexed* form (`(&m0)[i]` in `mkfs`'s
`gmode()`) as a singleton and gives a sweep for it; this is not that shape, and
the sweep cannot see it, because the walk is `*p++` rather than a subscript.

Declaring `a` as `long` is what makes the stride eight -- and it is the same
one-line change, because a `long` cell also holds a whole pointer.

### The symptom was silence, not a crash

Measured before the fix, with `bc -c` (compile only, do not exec `dc`):

```
$ echo '2+3' | bc -c
q
```

Nothing but the `q`. The walk reads `i` from the low half of slot 0 correctly,
then takes the **high** half of that same slot as the first fragment -- which
is zero -- so `routput`'s `while (*p != 0)` stopped before printing anything.
Every expression compiled to the empty `dc` program and `dc` dutifully printed
nothing, exit 0. **A truncating pointer walk that desynchronises by a half-word
produces no output rather than a wild pointer**, which is why it looked like a
program that simply did not work rather than like memory corruption.

After:

| input | output |
|---|---|
| `2+3` | `5` |
| `scale=4; 10/3` | `3.3333` |
| `x=7; x*6` | `42` |
| `scale=6; sqrt(2)` | `1.414213` |
| `for(i=1;i<=3;i++) i` | `1 2 3` |
| `2^64` | `18446744073709551616` |
| `obase=16; 255` | `FF` |
| `define f(n) {\n return(n*n)\n}\n f(9)` | `81` |

## Audited and deliberately NOT changed

- **`getf()` and `geta()` are declared `int *` and return `&funtab[...]`, a
  `char *`.** Upstream's own imprecision, and it does **not** truncate: both
  are eight-byte pointers here. S1 forbids a change that is not forced by the
  target, so they stay. The cost is two **new** compiler warnings (`illegal
  pointer combination` at the two `$$ = getf(...)`/`$$ = geta(...)` actions),
  because the slot they assign into is now `long *` where it was `int *`. Six
  warnings of that same class are **upstream's own** and were there before this
  change -- `$$ = "l."` assigns a `char *` into the same slot -- so the class is
  not new and the count went 6 -> 8. Before answering a warning, check whether
  it is upstream's own; six of these are.
- **`*argv[1]` at the top of `main`** is guarded by `argc > 1` on the same
  line, so this is **not** the address-0 argv class this port has met ten
  times. Checked rather than assumed, because the shape matches.
- **`execl("/bin/dc", ...)` then `execl("/usr/bin/dc", ...)`.** This port
  installs `dc` at `/usr/bin/dc`, so the **first exec is a union MISS** and the
  second is the hit. That is the distinction `v8s_execve` draws deliberately: a
  name the rootfs half does not have is a quiet miss, not a jail escape. No
  change needed, and it is the reason `bc` works at all.
- **The typed-token class is clean.** `bc.y` declares a `%union` with
  `int *iptr; char *cptr; int cc;` and the lexer stores pointers into `cptr`
  and the character into `cc` -- the right members. This is the grammar shape
  CLAUDE.md's `yylval.p` sweep exists for, and it comes back clean.

## The math library, and what importing it uncovered

`bc -l` rewrites `argv[1]` to `/usr/lib/lib.b` and parses it as bc source, so
the shipped artefact **is** the source -- data in the `/etc/termcap` sense,
nothing compiles it. Upstream ships it at `usr/lib/lib.b` (2299 bytes), **not**
under `usr/src`, which is why a first reading of this file said it was absent.
It is imported and installed, because `bc(1)` is its consumer and a documented
option of an installed program was failing -- the opposite of the
unconsumed-component rule rather than an instance of it. Measured before:
`bc -l` answered `cannot open input file on line 1`.

With it in place `a(1)*4` gives `3.14159265358979323844`, character for
character what the host's `bc -l` gives.

**And `e()`, `l()` and `s()` give nothing, which is not bc's fault.** It is a
pre-existing defect in `dc(1)` that nothing had ever reached: **dc silently
consumes the rest of its input after reading any number with an ODD count of
fractional digits.**

```
$ printf '4 p\n.4 p\n5 p\n' | dc
4
```

-- the `.4` and the `5` after it both vanish, exit 0, no diagnostic. The rule
is parity: `.44`, `.4444`, `1.00` and `4.34` are exact; `.4`, `.444`, `1.0`
and `0.5` produce nothing. V7 dc stores numbers **base 100**, two decimal
digits per byte, so an odd count is a half-used byte -- and `add0()`'s only odd
arm, `if(ct == 1) t = mult(tenptr,q)`, is the whole difference between the two
paths.

`lib.b` opens `scale = 20`, which is even, so `a()` survives while the
functions that work through odd intermediate scales do not. Task #25 carries
the full account and everything ruled out by measurement; `dc.c` and `dc.h` are
**pristine** against PROVENANCE, so it worked on a VAX and the fault is this
port's.

Three things generalise:

- **A program can be correct and useless because its back end is not.** bc
  compiles `.434*2` to exactly the right dc program -- ` 3k .434 2*ps.`,
  verified with `bc -c` -- and dc answers nothing. Checking the *emitted*
  intermediate rather than the final answer is what separated the two in one
  command, and it is the same discipline as reading `cc -S` before theorising.
- **The tests dc already had could not have found it.** All three
  (`2 3+`, `3 4*`, `6 7*`) are integer. A calculator with no fractional test is
  a calculator whose arithmetic is untested, and dc has no `PORTING.md` either
  -- it was imported before this project audited what it imported.
- **This is NOT the cb/calendar4/ratfor-BUGS shape and must not be asserted as
  one.** Those are upstream defects a VAX shared, deliberately frozen so that
  repairing them is a decision. This is a port defect in pristine source: a bug
  to fix, so nothing here asserts it stays.

## Still open

`dc`'s odd-fraction defect above (task #25) -- which bounds `bc -l` to the
functions whose intermediate scales stay even. Everything else measured works.
