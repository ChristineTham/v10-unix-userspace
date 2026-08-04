# spell

All four programs build and the pipeline runs end to end:

```
$ hashmake < words | sort -u > w.hash
$ spellin $(wc -l < w.hash) < w.hash > hlist
$ printf 'apple\nbanana\nzzzqqq\ncherry\nwibble\n' | spellprog hlist /dev/null
zzzqqq
wibble
```

`spell.sh` is the driver; `hashcheck` reverses `spellin` for verification.

## The fetch macro: LP64 is the PDP-11's case

`hashlook.c` reads Huffman-coded differences out of a table of `unsigned`, and
assembles them into a `long`. It picks between two macros:

```c
#ifdef pdp11	/*sizeof(unsigned)==sizeof(long)/2 */
#define fetch(wp,bp) (((((long)wp[0]<<B)|wp[1])<<(B-bp))|(wp[2]>>bp))
#else 		/*sizeof(unsigned)==sizeof(long)*/
#define fetch(wp,bp) (bp==B?wp[0]:((wp[0]<<(B-bp))|(wp[1]>>bp)))
#endif
```

There were only two cases in 1985: the PDP-11, where an `unsigned` was **half** a
`long`, and everything else, where they were the same width.

**LP64 is the PDP-11's case again** — `sizeof(unsigned)` is 4 and `sizeof(long)`
is 8. The macro V8 labels `pdp11` is the correct one here: it is the one that
assembles a long out of words half its width.

The file says so itself, at runtime, and it is the only place in the tree where
the original authors left an assertion for precisely this:

```c
if(sizeof(long) > sizeof(unsigned))
	abort();	/*wrong fetch macro*/
```

That abort fired on the first run — a diagnosis rather than a crash. The
selection is now on `HALFWORD` (defined in `hash.h`) rather than on `pdp11`.
It has to be a preprocessor choice, since `sizeof` is not available to `#if`,
which is also how the file already did it.

## `index` renamed to `hindex`

spell names a global array `index`, which is also a V7 libc function — `index(3)`,
what C89 calls `strchr`.

On V8 that was harmless. a.out `ld` pulls an archive member only for a symbol
that is still **undefined**, and a tentative definition is not undefined, so
libc's `index.o` was simply never loaded.

Mach-O's linker resolves **common** symbols from archives too. It found `_index`
in `libv8c.a` and quietly replaced spell's 2050-byte array with the 156-byte
function:

```
ld: warning: tentative definition of '_index' with size 2050 ... is being
replaced by real definition of smaller size 156 from libv8c.a[17](index.o)
```

Every write to `index[]` would have landed in libc's code. Nothing in spell or
in libc *references* `index()`; the mere tentative definition triggers the
lookup.

Renamed in the three files that define it, rather than changing how the compiler
emits file-scope arrays: V8 code relies throughout on tentative definitions
merging across translation units, and emitting real definitions instead would
break that. Since the host linker is a fixed constraint of this port (PLAN.md
§1), the source is what adapts.

## Not spell's bug: scanf was missing from libc

`spellin` reads its input with `scanf("%lo", &h)`, and that faulted inside
`libsystem_c.dylib__svfscanf_l` — the **host's** scanf.

`scanf.c` and `doscan.c` existed in `src/libc/stdio` but had never been added to
`libv8c.a`. A missing libc function does not fail the link; it is resolved from
`-lSystem`. For a non-variadic function that would have quietly worked and
hidden the gap. For a variadic one it is an ABI mismatch: v8cc passes arguments
in x0–x7, Apple's ABI passes variadic arguments on the stack, so the host's
scanf read the format pointer as its destination.

`scanf`, `doscan`, `popen` and `system` are now in the library. The general
hazard — a gap in libc silently satisfied by the host — is noted in the Makefile
beside the file list.
