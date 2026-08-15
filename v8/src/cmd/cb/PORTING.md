# Porting notes: cb(1)

The C beautifier. Imported with the Wave A2 batch. **Two forced changes**, both
the address-0 class, and **one upstream bug deliberately left in place** — which
is the interesting half, because the two live on the same line.

## Built as a directory, because `cb.c:3` includes a `.c`

```c
#include "cbtype.c"
```

That is the fourteenth member of this port's included-non-header list, and
upstream's own makefile says so outright (`cb.o: cb.c cbtype.c cbtype.h`).
Building `cbtype.c` as its own translation unit gives a duplicate `_ctype_` at
link time — which is how it was caught. See CLAUDE.md's step 4.

## The crashes: 50 of 53, and the count is the diagnosis

The crash probe reported exactly **50** signal deaths across bare + 52
single-letter options. That number is the tell that there are **two** sites, not
one:

- `-s` and `-j` are real options that `continue`, and bare `cb` reads stdin — so
  three invocations were always fine, and 52 − 2 = 50.
- 49 die in the `default:` arm.
- The fiftieth dies in `-l`.

A first reading recorded only the `default:` arm. Counting properly found the
other. (CLAUDE.md's *"a claim of the form 'N spellings of one number' is a
testable assertion about the tree"* — the same move, applied to a crash count.)

### Site 1 — `-l` with no number

```c
maxleng = atoi(*++argv);
```

With `-l` last, `*++argv` is the vector's NULL terminator. On the VAX `atoi`
read address 0 — `0x00`, so an empty string — and returned 0, giving `maxleng`
0 and `maxtabs` −2, after which cb read stdin as usual and exited 0. Restored as
`atoi(*++argv? *argv: "")`; the increment is kept because the VAX performed it
too. Fourth instance of this exact loop shape after `ncheck`, `icheck` and
`dcheck`.

### Site 2 — the `default:` arm, and the bug that stays

```c
fprintf(stderr, "cb: illegal option %c\n", *argv[1]);
```

`*argv[1]` is `*(argv[1])` — the first character of the **next argument** —
where `(*argv)[1]`, which the `switch` four lines above uses, was meant. That is
a precedence bug, and it is **upstream's, on upstream's hardware**. Measured:

| command | prints | on a VAX too |
|---|---|---|
| `cb -a x.c` | `cb: illegal option x` | yes |
| `cb -Q zzz` | `cb: illegal option z` | yes |
| `cb -a` | *SIGSEGV here, `option \0` on a VAX* | no |

So the wrong letter is **not** corrected: §1 says a change to `src/` must be
forced by the target, and naming the wrong letter is not. What *is* forced is
the crash, and the fix reproduces what a VAX printed — the byte at address 0,
which is `0x00`:

```c
fprintf(stderr, "cb: illegal option %c\n", argv[1]? *argv[1]: '\0');
```

Measured after, with `od -c`:

```
0000000    c   b   :       i   l   l   e   g   a   l       o   p   t   i
0000020    o   n      \0  \n
```

21 bytes, exit 1. The NUL is the whole point: a null guard that printed nothing,
or a `(*argv)[1]` that printed `a`, would both be *this port inventing an
answer* rather than restoring one.

## Guarded

`tests/wavea` asserts, for the crash: survival, exit 1, the **21-byte** length,
and the text with NULs stripped. For the non-change: that `cb -a x.c` still
names `x` — a case whose whole purpose is to assert a bug is still there, so
that "fixing" it goes red and has to be a decision. For the `-l` site: that it
survives, and that `cb -l 60` still sets the width and reformats.

Mutations M2 (the `default:` guard) and M3 (the `-l` guard) fire 5 and 2 cases
respectively; neither fires the wrong-letter case, because `cb -a x.c` has a
non-null `argv[1]` — which is what says the cases are aimed rather than blanket.
