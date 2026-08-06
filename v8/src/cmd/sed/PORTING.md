# sed

One of eleven commands imported and never built — which meant **the V8 world had
no `sed`**. Zero warnings under v8cc; one change.

## `l` crashed on any byte ≥ 0200, and UTF-8 is made of them

`sed1.c` prints an unambiguous listing with

```c
if(*p1 >= 040) { ... } else { p3 = trans[*p1]; ...
```

`char` is signed here, as it was on the VAX (`compiler/ccom-arm64/macdefs.h`,
and Apple's ARM64 agrees), so a byte ≥ 0200 is **negative**: the test sends it
down the control-character arm, and `trans[*p1]` indexes a 32-entry array with a
negative subscript. The next line dereferences whatever it loaded.

Measured boundary:

```
byte 0176 -> "~"        exit 0
byte 0177 -> "\177"     exit 0     (the rub[] special case)
byte 0200 -> SIGSEGV
printf '\303\251x\n' | sed -n l    -> SIGSEGV      (that is UTF-8 "é")
```

**The out-of-bounds read is upstream's**; two things about the target turned it
into a fault. `trans[]` is an array of *pointers*, so LP64 doubles the stride —
`trans[-128]` now reads 1024 bytes before the array rather than 512 — and Mach-O
maps nothing there where a.out did. The VAX printed nonsense; this crashes.

It is also newly *reachable*. The byte that does it is any UTF-8 continuation,
and piping UTF-8 through `sed -n l` is an ordinary thing to do on this host and
was not in 1985.

The fix is `& 0377` on the comparison, which makes it unsigned: a high byte
takes the printable arm and is emitted as itself, which is what an
unsigned-`char` machine would always have done, and `trans[]` is then reachable
only with 0..037. `tests/wavea` checks both directions — the UTF-8 case exits
cleanly, and a tab is still escaped rather than passed through.

## Not changed, upstream's own

`sed0.c:742` — a bracket expression containing a byte ≥ 0200 does
`ep[c>>3] |= bittab[c&07]` with signed `c`, writing 8 bytes *before* the class
bitmap; the class then never matches. Verified: `sed 's/a[\303]b/HIT/'` silently
does not match. Identical on the VAX — `ep` is a `char *`, so LP64 changed
nothing here — and it is a wrong answer rather than a crash, so it stays.
