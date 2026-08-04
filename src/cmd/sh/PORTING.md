# The Bourne shell, ported

All 24 source files compile with V8's own compiler and link freestanding. The
shell starts, reads its profile logic, parses, forks, execs and reaps.

## What runs

```
echo hello world                      hello world
for i in a b c; do echo $i; done      a b c
echo one | tr a-z A-Z                 ONE
if true; then echo yes; fi            yes
echo a > f1; cat f1                   a
echo `echo bq`                        bq
set -- p q; echo $# $1 $2             2 p q
f() { echo infunc; }; f               infunc
case abc in a*) echo matched;; esac   matched
echo ${HOME}                          /Users/christie
test -d /tmp && echo isdir            isdir
```

`$((...))` is correctly a syntax error: arithmetic expansion postdates 1985.

## What does not, and where it is

**Variable assignment hangs.** `x=42` spins under `findnam`, and so does
assigning to a name that already exists.

It is **not** the tree. Instrumenting `findnam` to print its argument and cap
the walk at 200 nodes shows it never reaches the walk at all:

```
F[]
```

— the name it was handed prints as three invisible bytes, and the marker after
`chkid()` never appears. So `findnam` is called with a garbage `nam` pointer and
`chkid` runs off the end of it:

```c
while(!ctrlchar(*nam) && (*nam&QUOTE)==0 && *nam!='(' && *nam!='=')
	nam++;
```

That loop has no bound but the string's own terminator.

So the fault is upstream, in whoever builds the name: `execute()` in `xec.c`
handling the assignment word (the sample puts the call at `execute+808`), or the
macro expansion that produced it. `namwalk` is bounded (instrumented to abort
after 2000 visits; it never fires) and the arena no longer runs past the break,
so both of those are cleared.

Next: print the assignment word in `execute` before it reaches `findnam`, and
walk back from there to where the pointer is made. As always in this port, make
it print what it has rather than working out what it should have.

## The four porting changes, all LP64

Each is in the file that exists to hold the assumption, and each is marked
`PORT:` in place.

| Where | What | Why |
|---|---|---|
| `mode.h` | `Rcheat` casts to `long`, and the `_cheat` union holds one | The shell's own pointer/integer pun. `addblok` computes `(struct blk *)(Rcheat(bloktop) + reqd)` with `reqd` unsigned, so once the cast says `int` the *addition* is 32-bit and C is right to truncate. |
| `defs.h` | `round()` casts to `long` | Applied to pointers as well as sizes: `rndstak = (char *)round(staktop, BYTESPERWORD)`. |
| `brkincr.h` | `BRKINCR`/`BRKMAX` scaled ×4 | Everything in the arena grew: `BYTESPERWORD` is 8, and `struct namnod` went from about 20 bytes to 64. |
| `stak.c`, `name.c` | `stakroom(n)`, called by `getstak` and by `staknam` before it copies | See below. |

## The hang that was not a crash

Three of these presented identically, and none of them looked like a memory
problem: **the shell hung, in the child of a fork, with the parent asleep in
`wait`.** That reads as a fork or wait bug.

It was not. A store landed on an unmapped page, the handler `stdsigs()` had just
installed returned, and the faulting instruction retried — forever. A fault that
would be a clean `SIGSEGV` in a program without handlers becomes a silent spin
in one with them.

The last of the three is worth stating on its own, because it is not a width
problem at all. `staknam()` copies a whole `"name=value"` to `staktop` with
`movstr()` and only *afterwards* tells `getstak()` how many bytes it used. So
the guaranteed headroom has to cover the **longest single item**, not a fixed
step. `BRKINCR` was 512 bytes, which was ample for a 1985 environment; `PATH` in
the environment this was debugged in is 4315 bytes. `stakroom()` now reserves
for the actual string before the copy, so the size of the environment stops
mattering.

## One that was ours, not the shell's

`fork` was returning the same value in parent and child. XNU's fork syscall
returns two values — x0 holds a pid in *both* processes, and x1 is 0 in the
parent and 1 in the child — and the shim was reading only x0 through `rawsys0`.
Both processes carried on as the shell, neither execd, and the real parent
waited forever. Fixed in `shim/v8sys/syscall.c`, which already had the same
two-register handling for `pipe`.
