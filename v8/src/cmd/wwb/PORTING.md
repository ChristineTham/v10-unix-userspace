# wwb — porting notes

The Writer's Workbench, Bell Labs 1982.  Eight programs and their data are in
at `/usr/lib/style`, and the 27-byte dispatcher V8 ships is at `/usr/bin/wwb`.
**One source change**, forced by the target and described below; the other 63
imported files hash to their recorded `PROVENANCE` blobs, and `nm -u` on each
of the eight programs is empty.

## A string literal split across a `-D`, and the compiler that can read it

`Makefile:48` is

    $(CC) $(CFLAGS)  -DLIB=\"$(LIB) prose.c -o prose

— an **opening** quote and no closing one — and `prose.c:125` is

    strcpy(path,LIB");

— a stray **closing** quote.  Neither half is a well-formed token on its own;
together they make `"/usr/lib/style"`.  The macro carries the opening quote and
the source supplies the close.

This is the `-DCM_N` shape sharpened.  There a missing `-D` selected an empty
`case` arm and failed silently; here the flag supplies **half a token**, so the
file cannot be compiled at all without it — which is the good direction.

**v8cc compiles it; clang refuses it.**  Measured:

| compiler | result |
|---|---|
| `cc -DLIB='"/usr/lib/style' prose.c` | exit 0, `prose.o` 23160 bytes |
| `clang -std=gnu89 -DLIB='"/usr/lib/style' prose.c` | `error: expected expression` |

That makes wwb the **second** program here whose buildability is a property of
the authentic compiler, after `cyntax`'s space-separated compound operators
(`poignant | = girn`), where clang rejects 21 of 41 files and v8cc compiles all
41.  Do not "correct" either quote: both are upstream's and the pair is
well-formed.

The guard is a **pair**, because a case asserting only that `prose` exists
would pass against a `prose` built with no `-DLIB` at all.  `tests/wavea`
asserts that `prose` contains the string `/usr/lib/style` exactly once and that
`chunk` — which takes no `-D` — contains it zero times.

## What is installed, and why it is not what the build description says

Upstream installs to two directories: `LIB = /usr/lib/style` (`Makefile:16`)
and `BIN = /usr/bin/WWB` (`Makefile:17`).  Only the first is reproduced, and
that is forced twice over.

**`/usr/bin/WWB` is not in the archive.**  `git ls-files` records exactly one
matching entry — lowercase `usr/bin/wwb`, one blob — and no `WWB` directory
either as a file or as a tree.  Independently, all 23 shell scripts that would
be installed there are absent from the shipped tree.  So V8 shipped this suite
half-installed.

**And it could not be installed here even if it were there**, because `WWB` and
`wwb` are **one name** on a case-insensitive filesystem, which is the macOS
default and what the CI runner has.  A V7 filesystem is case-sensitive, so the
layout was fine in 1985; it is the *host* that cannot represent it.  This is
the first thing in the port blocked by a property of the host filesystem rather
than of the machine.

So the install set is the archive's, exactly: **35 files under
`/usr/lib/style`** — `prose chunk syl mkstand dictadd punlx gramlx orglx`,
`standlkup`, and 26 data files — plus the dispatcher.  Verified as a set
comparison rather than a count: every file installed is in the archive, and the
only archive entries absent are the nine belonging to `style`/`diction`.

The scripts, `double.c` and `cbtype.c` are imported and **not built**;
`lcomp`'s `bb` and `refer`'s `whatabout` are the precedent, and `tests/deps`
states it as two `nodep`s so it cannot quietly become live.

### A caution about how that was measured

An earlier reading of this called `/usr/bin/wwb` and `/usr/bin/WWB` a **hard
link**, on the strength of `ls -li` reporting one inode for both spellings.
That measurement was taken *through* the case-insensitive volume and was
worthless — `ls WWB` simply opened `wwb`.  The case-sensitivity probe that
licensed it was itself vacuous: an `&&` chain that broke on an empty glob under
zsh, so the file it tested for was never created and `[ -e AA ]` was false for
the wrong reason.  **`git ls-files` is the authority**, because git records
names exactly whatever the working volume does.

## What cannot run, and it could not run in 1985 either

The build description's own header says the package *"assumes deroff, style,
and diction are normal commands on the system"*.  `deroff` is here.  **V8 ships
neither `style` nor `diction`, anywhere** — no binary, no source — though nine
of their data and helper files (`style1`, `style2`, `style3`, `dprog`, `rewrt`,
`dict.d`, `suggest.d`, `macs.tr`, `sq2006.e`) sit in `/usr/lib/style` in the
archive.  They are the `more`/`pg` class: shipped without source, and out of
scope.

Of roughly a hundred command references across the 23 scripts, `style` accounts
for two and `diction` for three.  So `proofr`'s style arm and `wwb`'s style
pass cannot run, and `prose` — which consumes style's table — says so in
upstream's own words:

    The file named after  the -f flag doesn't seem to contain a style table.
    Or else style cannot produce a table for your file.
      Try running style alone on your file.

That is the `load(1)` says `No mem` outcome: a real program that cannot answer.
`tests/wavea` asserts the message, so it is a case rather than a sentence.

## The three scanners need three build directories

`lex` always writes a file called `lex.yy.c`, and `punct.l`, `gram.l` and
`org.l` are three separate programs.  A shared build directory would race under
`-j` and silently compile whichever finished last, which is a wrong *program*
rather than a build failure.  Each gets `$(BUILD)/wwb/<name>/`.

## The one source change: the address-0 argv class, found by the crash probe

`mkstand.c:63` (upstream `:49`) is `number = *argv[1];` — the **first statement of `main`** —
and `main` has no `argc` check anywhere.  So a bare `mkstand` dereferences the
argument vector's NULL terminator.  A VAX put the text segment at virtual 0 and
read the first byte of crt0 there, measured elsewhere in this port as `0x00`, so
`number` became `0` and then `0 - '0'`; macOS leaves page 0 unmapped and it is
SIGSEGV.

Two reads are guarded, because `argv[1]+1` is address 1 and crt0's second byte
is `0x00` too.  The guards yield `0` and a false `isdigit()` rather than
returning early, because the rule here is to restore the VAX's **answer** and
not merely to remove the fault.

Measured after the change, and it is the answer the reasoning predicted:

    $ mkstand
    Can't read /tmp/OSLogRateLimit=64stat.out.        (exit 2)

`OSLogRateLimit=64` is this host's first environment variable.  That confirms
the part deliberately **not** guarded: `pid = argv[2]` three lines below reads
*past* the terminator into the environment vector, which is a valid read on
both machines, and the `sprintf` into `char name[14]` that follows overruns it
identically on a VAX.  Upstream's defect on upstream's hardware; S1 leaves it.

`tests/wavea` therefore asserts the **status** and the **diagnostic** and never
the filename, which is a host property — with the two-argument case as the
control, so a "fix" that made every path answer the same thing would fail.

It was found by `tests/crash-probe.sh` on the day wwb landed: the population
went 170 → 178 as the eight programs joined it, and the floor went 107 → 108
with one new line, `mkstand (no arguments)`.  That line is gone again now.

## Still open

`double(1)` is genuinely portable — `double.c` and `cbtype.c` both compile
under v8cc, measured — and has nowhere to go: upstream installs it to
`/usr/bin/WWB`, which cannot exist here.  Putting it in `/usr/bin` would invent
a destination no source states.  Left imported and unbuilt, with the reason
recorded rather than the gap.
