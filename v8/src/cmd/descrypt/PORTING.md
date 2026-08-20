# encrypt(1) and decrypt(1)

Don Mitchell's DES file encryption, 1983. Two programs from one directory --
ten shared objects plus a `main` each -- which is `pack`/`unpack`'s shape.
Both are in none of `Admin`'s tables, so `Admin/dest` answers `/usr/bin` by
fall-through and the shipped tree agrees: `usr/bin/encrypt` and
`usr/bin/decrypt`, 18432 bytes each and **not** byte-identical. Two real
programs that happen to be the same size, which is the opposite of the
`pcat`/`unpack` and `e`/`ed` families.

**Zero source changes.** All 19 files are byte-identical to pristine V8.

## The interesting part is the LP64 defect that is not there

Read cold, this looks like the worst width exposure in the tree.
`crypt.h` declares

```c
typedef struct Block {
	long left, right;
} Block;
```

with a comment saying, in the author's own words, *"bit 31 is the high-order
bit (sign bit on VAX) of block.left"*. So a DES block is two **32-bit** halves,
stated outright, and V8's own compiler made `long` 32 bits
(`# define NOLONG`, `cmd/ccom/vax/macdefs.h`). Measured here: `sizeof(long)`
is 8 and `sizeof(Block)` is **16**, where the author's machine had 4 and 8.

And `des.c` builds its rotate out of shifts that name the width:

```c
	temp = (right << 1) | ((right >> 31) & 1);
	temp = ((right & 1) << 5) | ((right >> 27) & 0x1f);
```

That is the `daddr_t`/`NOLONG` class at its most alarming -- a 33rd bit that
should have fallen off the end, and a rotate that should have wrapped.

**It is not a defect, because every extraction is masked.** The round function
only ever reads `temp` through `& 0x3f`, `(& 0x3f0) >> 4`, `(& 0x3f00) >> 8`
and so on up to `& 0x3f000000`, so the stray bit 32 is never looked at; `left`
and `right` are rebuilt each round from S-box outputs that are 32-bit values;
and `io.c` packs and unpacks the byte stream explicitly with `>> 24 & 0377`
rather than by reinterpreting the struct. The algorithm is width-agnostic by
construction. `Block` doubling in size costs memory and nothing else, because
nothing writes it to disk as a struct.

This is `efl`'s lesson from the other side: there, one typedef being already
pointer-sized meant a 12000-line program had no LP64 surface at all; here, one
discipline in how fields are extracted means a program whose central type is
provably the wrong width still computes the right answer. **Read how the value
is USED before costing a width change.**

## Measured with the author's own acceptance test

`README` says: *"The file, CIPHERTEST, is provided to insure that this code
performs correctly on your machine. `decrypt -p testkeyword < CIPHERTEST`
should result in readable text."*

It does, and what comes out is the README itself. That is **bit-exact interop
with a 1983 VAX**, and it is a far stronger statement than a round trip,
because an implementation that is consistently wrong round-trips with itself
perfectly. Don Mitchell shipped a known-answer test precisely because he
expected machine differences, and it is the right test for this port forty
years later.

Four properties are asserted in `tests/wavea`:

| | |
|---|---|
| the acceptance test | `decrypt -p testkeyword < CIPHERTEST` yields the README |
| round trip | `encrypt` then `decrypt` with one key returns the input |
| negative control | a different key does **not** return the input |
| key length | key material past 8 characters changes the ciphertext |

The last one matters for a reason below.

**Mutation measured the first claim rather than leaving it as a sentence.**
Perturbing the DES rotate (`>> 31` to `>> 30`) breaks the cipher in BOTH
programs, since they share `des.o`. Result: the CIPHERTEST case fails and
**the round-trip case stays green**, along with the wrong-key and key-length
ones. That is exactly what "a consistently wrong implementation round-trips
with itself perfectly" means, and it is why the author's known-answer file is
the load-bearing case here and the round trip is not.

## `getpass.o` is built in deliberately, and it is a T/T duplicate

Upstream's `OBJS` includes `getpass.o` although `libv8c` also defines
`getpass`. That is not redundancy. Berkeley's truncates a passphrase to
**eight characters** -- `static char pbuf[9]` against this directory's
`pbuf[128]` -- and `README` says so in as many words. The two also differ in
how they open the terminal: libc's is `fopen("/dev/tty", "r")` and this one is
`fdopen(open("/dev/tty", 2), "r")`.

Measured: both define `_getpass` as a `T` symbol, so this is the T/T row of
`tests/kmemu`'s duplicate-definition table. It resolves silently and correctly,
because the program's own object is on the link line and an archive member is
only pulled when the symbol is still undefined. Dropping it would compile,
link, and quietly cut every passphrase to eight characters.

**No behavioural case can see it**, because `getpass` is reached only when no
`-p` is given, i.e. only from a terminal. So its guard is the `tests/deps`
edge asserting the object is a prerequisite of both programs, and this
paragraph. Measured by dropping `getpass.o` from the link: the build
**succeeds** -- libv8c's member is pulled in to satisfy the now-undefined
symbol -- **wavea reports 483 passed and 0 failed**, and the only thing that
goes red is the `deps` edge. Without that edge the change would be invisible
until somebody typed a passphrase longer than eight characters. The key-length case above tests the neighbouring property -- that
the *key schedule* uses everything it is given -- which is what would look the
same if truncation happened somewhere else.

## Audited and clean

- No undeclared pointer-returning functions; `alpha.c` declares
  `extern char *fgets()`.
- No address-0 argv defect: both `main`s use `argc`-guarded option loops.
- `register int *key` in `des.c` and `register int *ip` in `io.c` walk `int`
  arrays (`subkeys[]`, and a byte buffer built as ints), not narrowed records.
- `genSP.c` is a generator upstream ran once to produce `SP.c`; `SP.c` is
  checked in, so it is not built here -- the same relationship `awk`'s
  `maketab` has, except that upstream does **not** re-run it, so there is no
  chain to assert.
- `getpass.USG` is the System V variant, not built. Upstream ships both and
  builds `getpass.c`.
