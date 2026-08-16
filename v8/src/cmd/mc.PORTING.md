# mc

Columnate. `usr/src/cmd/mc.c`, 183 lines, installed to `/usr/bin`. Two changes,
both forced by the target, and one of them is a class this port has now fixed
eight times.

## 1. `mc.c:49` — the option loop walks onto the argv terminator

Upstream:

```c
if(argc>1){
	while(*argv[1]=='-'){
		--argc; argv++;
		switch(argv[0][1]){
		...
```

Every arm of the switch consumes an argument, so once the loop has eaten the
**last** option `argv[1]` is the vector's NULL terminator. Reached by `mc -20`,
`mc -t`, `mc -` — every option in the final position, which for a program whose
whole interface is `mc [-][-WIDTH][-t] [file...]` is the ordinary case rather
than an edge one. Measured: `printf ... | mc -20` was a SIGSEGV **after**
reading the width and **before** reading a byte of input.

A VAX mapped virtual 0 — the first byte of crt0, `0x00`, since V8's binaries are
ZMAGIC and `N_TXTOFF` is 1024 so the a.out header is never mapped — which is not
`'-'`, so the loop simply ended and the `argc==1` test below sent mc to stdin.
`argv[1]!=0 &&` restores that **answer** rather than merely removing the fault.

Byte for byte the shape `diff/diffh.c:93` carries, and `ncheck`'s `-i` loop, and
`cpio`'s option guard. PLAN.md §4i has the class.

The guard goes on the first test only. `argv[0][1]` inside the body is reached
only after `*argv[1]=='-'` has passed, which a null `argv[1]` cannot do on
either machine.

## 2. `mc.c:15` — an absolute include into the 1985 filesystem

`#include "/usr/jerq/include/jioctl.h"` → `#include <jioctl.h>`. An absolute
path is the one kind of include cpp cannot be told to resolve elsewhere; no `-I`
affects it. The header itself is authentic and unchanged, and the rootfs already
carried it at `usr/include/jioctl.h` — the Makefile's `$(ROOTFS_INC)` rule has
copied it from `jerq/include` since `ls(1)` needed the same thing, and its
comment already named `mc(1)` as the other consumer.

Identical change, for the identical reason, to `src/cmd/ls.c:11` and
`src/lib/libtermlib/termcap.c`.

**The difference is that here the block is LIVE.** `mc.c`'s *first line* is
`#define JERQ`, so the file turns the Blit code on itself, where `ls.c` and
`termcap.c` leave it to the compile line and nothing defines it. So this is the
first program in the tree that actually compiles the jerq window-size query.

It still does nothing: the shim implements no `JWINSIZE`, so
`ioctl(1, JWINSIZE, &wbuf)` fails and `linewidth` keeps `WIDTH` (80) — which is
upstream's own fallback two lines further on. Worth knowing that upstream also
guards the signed-char hazard CLAUDE.md records for `struct winsize`:

```c
linewidth=wbuf.bytesx;
if(linewidth<0)
	linewidth=WIDTH;
```

`bytesx` is a `char`, so a terminal past 127 columns reads negative — and Bell
Labs wrote the check. Implementing `JWINSIZE` would make that arm live and is a
step of its own.

## Eliminated by measurement

**cpp was suspected and is innocent.** The first reading of the build failure —
`Can't find include file /usr/jerq/include/jioctl.h` on a line inside
`#ifdef JERQ` — was that V8's cpp processes `#include` inside a false
conditional. Tested both spellings, with a space and with a tab after `#ifdef`,
against a path that does not exist: both compile clean. The file defines `JERQ`
on line 1. Reading the first line of the file settled in one command what two
experiments could not.

## Still open

- **No `JWINSIZE`**, so the jerq arm compiles and never fires. See CLAUDE.md's
  entry on `struct winsize`: implementing it is a step rather than a side
  effect, because the column count is a `char`.
- **`-` alone (suppress the colon break) is exercised but not asserted** on its
  effect, only on not crashing — the colon behaviour needs input with colons in
  it and a reference answer, which nothing here has.
