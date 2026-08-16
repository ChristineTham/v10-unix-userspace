# libplot — porting notes

The plot libraries, imported for task #89.  `usr/src/libplot` is a **sibling of
`usr/src/lib`**, not a child, which is why no survey in this port had read it:
seven libraries, one per terminal.

```
libplot/  lib4014/  lib2621/  lib5620/  libblit/  libpen/  libtr/
```

`whoami()` is what reveals the design — it returns `"general"` in `libplot`,
`"tek"` in `lib4014`, `"hp"` in `lib2621`.  They are interchangeable back ends.

Imported: `libplot` and `lib4014`.  Both **byte-identical to upstream**; nothing
inside either needed a change.

## THE FINDING THIS FILE EXISTS TO CORRECT

The first reading of this tree, recorded in four documents before it was
checked, was that **`-lplot` would not have linked on a VAX either** — that
`libplot.a`, at 1008 bytes and two members, defines none of the six primitives
`graph` calls, so V8's own makefile was broken.

Every step of that was measured and the conclusion is false.

`graph.c:4` is `#include <iplot.h>`, and that header is nothing but macros:

```c
#define erase()               printf("e\n")
#define closepl()             printf("cl\n")
#define line(_x,_y,_X,_Y)     printf("li %d %d %d %d\n", _x,_y,_X,_Y)
```

**The plot interface is a header, not a library.**  A program that includes it
emits `plot(1)`'s textual command language and calls nothing.  So `libplot.a` is
*complete* at two members: `subr.c`'s `putnum()` is the one thing the macros
cannot express inline — the spline and fill macros pass arrays — and
`whoami.c` names the device.

The error was a grep.  `grep -oE '\b(openpl|line|move)\b *\('` matches a macro
invocation exactly as it matches a call, so the six "unresolved primitives" were
six `printf`s.  **The instrument that settles it is `nm -u` on the object:**

| object | undefined symbols |
|---|---|
| `graph.o` | `atof ceil fabs floor log10 malloc printf realloc scanf sprintf strcat strcpy strlen ungetc` — **no plot functions** |
| `prof.o` (with upstream's `-Dplot`) | libc only — **no plot functions** |
| `driver.o` (`plot(1)`) | **28 plot functions**, plus libc |

Same shape as `awk`'s `execute`, one batch earlier: a macro that dereferences
its argument in front of the null guard `real_execute()` opens with, so reading
the function proved nothing about the call.  There the macro hid a dereference;
here it hid the absence of one.

## What each library is for here

**`libplot.a`** — the "general" device, and what `-lplot` resolves to.  It links
nothing that `graph` or `prof` needs, and naming it on the link line is still
right: `src/cmd/cc.c`'s `libpath()` resolves `-lNAME` against
`$V8ROOT/usr/lib/libNAME.a` first, so without the archive the flag would escape
to the host SDK.  Exactly the reason `shim/libm/dummy.c` reproduces V8's
216-byte empty `libm.a`.

**`lib4014.a`** — the Tektronix 4014 back end, 38 functions from 31 sources, and
the one thing here that is genuinely required: `plot(1)`'s `driver.c` parses the
command language and dispatches through `struct pcall { void (*plot)(); }`, so
it references the primitives for real.  All 28 it needs are defined.

## The build idiom, which is new to this tree

The sources live **inside an `ar` archive** and the makefile extracts them:

```make
lib4014.a: tek.c.a
	mkdir xplot; cd xplot; ar x ../tek.c.a; cc -c -O *.c
	cd xplot; ar rc ../lib4014.a *.o; rm -r xplot
```

Reproduced as **one rule per archive** rather than per object.  That is not
laziness: the member list does not exist until the extraction has run, and
`$(wildcard)` is expanded when make *reads* the rule, so a stamp-and-wildcard
scheme would compile nothing on a clean tree and everything on the next run.
One rule is also upstream's own granularity, and 31 files take under a second.

`PROVENANCE` records the hash of the **bundle**, which is the file upstream
ships.  Nothing inside needed patching — the LP64 and address-0 sweeps came back
clean over all 937 lines — so the question of how to record a patched member has
not arisen.  When it does, extract that member into `src/` beside the bundle and
record it separately; do not rewrite the archive, because that would destroy the
one hash that ties this tree to Bell Labs'.

## Not ported, and why

- **`lib2621`** (HP 2621) needs `libcurses`, 43 unported files.  With it,
  `hpplot` builds and `plot -Thp` works.
- **`lib5620`, `libblit`** are jerq/Blit, whose terminal support was dropped by
  owner decision.
- **`libpen`** opens `/dev/hp7580`, a pen plotter.
- **`libtr`** emits troff, and its consumer `trplot` is not imported.
- **`/usr/bin/plot`**, the dispatcher that picks a renderer from `-T`, is a
  16-line shell script that V8 ships **with no source** — `usr/src/cmd/plot`
  builds `tek` and `hpplot` and never installs it.  Installing it from the
  binary tree would make it the first program in this world that the port did
  not build, which is a decision worth taking deliberately rather than as a side
  effect.  `tek` reads the same input directly.
