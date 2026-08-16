# qed(1) — porting notes

Wave A2 batch 2e, the first of its four.  Ken Thompson's editor, `ed`'s
ancestor: fourteen sources and one header, 3926 lines.  Upstream:
`v8/usr/src/cmd/qed`.  Installs to `/usr/bin`, which all three sources agree on
— its makefile says `cp a.out /usr/bin/qed`, the shipped tree has
`usr/bin/qed`, and `Admin/dest` falls through to the same place.

Twelve of the fifteen files are **byte-identical to upstream**.  Three changed,
for one reason.

## The change: five variables that hold a signal handler

`signal(2)` returns the **previous handler** — a function pointer — and `qed`
saves it to restore later.  Upstream stores it in an `int`, which is exact on a
VAX and loses the top half here.

```c
vars.h      int  onhup, onquit, onintr;      -> long
getfile.c   int  savint = -1;                -> long
getfile.c   int  onbpipe;                    -> long
misc.c      extern savint;   /* implicit int */ -> extern long savint;
```

**IT IS LIVE, NOT LATENT, AND THE PATH IS ORDINARY USE.**  `main.c:243`
installs the real function `interrupt` for SIGINT.  Every `!command`, `<`, `>`
and `|` then goes through `getfile.c:218`

```c
savint = signal(SIGINT, 1);
```

which truncates a text address, and `getfile.c:246` and `misc.c:99` hand it
back to `signal`.  The next `^C` after a shell escape jumps to the low half of
a pointer.

Measured directly rather than argued, with a program that does exactly what
`qed` does:

```
handler   100ecc660
via int   ecc660
via long  100ecc660
```

The top byte is gone.  On a VAX both widths are four bytes and the same code is
correct, so this is forced by the target — the `yylval` shape exactly, a
declaration that lies about a type, in a *variable* rather than a grammar.

### Why `long` and not a function-pointer type

`savint` doubles as a **sentinel**.  `misc.c:98` is

```c
if(savint>=0){ signal(SIGINT, savint); savint = -1; }
```

with −1 meaning *nothing saved*, initialised that way at `getfile.c:124`.  A
function-pointer type makes that comparison meaningless; a 64-bit `long` keeps
upstream's idiom exactly and is pointer-width here.  Same one-word fix, for the
same reason, as this port's `#define YYSTYPE long` in `yacc`.

### The second declaration is in another file, and fixing one is worse than fixing neither

`savint` is **defined** in `getfile.c` and **re-declared** in `misc.c:52`, as an
implicit-int `extern savint;` inside `error()`.  There is no header between
them — upstream's `vars.h` does not mention it — so nothing in the build graph
couples the two, and `tests/deps` carries a `nodep` saying so.

Widening the definition alone would have left `misc.c` reading the low four
bytes of an eight-byte object: silently, on a little-endian machine, and
correctly for every value below 2^31.  The sentinel test would have kept
working while `signal()` received half an address.  That is this file's
most-repeated shape — the fix lands on one line and the line beside it keeps
the assumption — with the line beside it in a different translation unit.

### THE COMPILER'S DIAGNOSTIC DID NOT CHANGE

v8cc warns `illegal pointer/integer combination, op =` at all five sites both
before and after, because the warning is about the *kind* mismatch and not the
*width*.  A build that counted warnings would have reported the fix as a no-op.
The measurement above is the only thing that distinguishes them.

## Eliminated by measurement

- **`sbrk` is declared.**  `vars.h:185` is `char *sbrk();`, so
  `main.c:237`'s `fendcore = (int *)sbrk(0)` is not the truncated-pointer
  class.  `com.c:151` compares its result against `(char *)-1` and is likewise
  fine.
- **The option loop is guarded.**  `main.c:206` is `while(argc > 1 && **argv=='-')`
  and the `-x` arm checks `argc == 2` before taking `argv[1]`, so the
  argv-exhaustion class (nine instances elsewhere) is absent.
- **`blkio.c:88`** casts correctly: `lseek(tfile, ((long) b) * 512L, 0)`.
- **`line[70]`** is bounded at `putchar.c:141`.
- No absolute includes, no `#include`d non-headers, no `(&x)[i]`, no
  `time(&narrowed)`, no `float atof()`.

## Still open

- **`^C` cannot be delivered by the suite**, so what `tests/wavea` asserts is
  the shell escape completing and the *declarations* being pointer-width,
  rather than the restored handler running.  The declarations are where the
  defect lives and where a regression would reappear.
- Upstream's makefile target is **`a.out`** (`cc $(FILES)`, no `-o`), the shape
  that once had `eqn` recorded as rung-5 blocked.  Nothing here depends on it —
  our rule names the binary — but rung 5 for `qed` would need `make a.out`
  rather than `make qed`.
