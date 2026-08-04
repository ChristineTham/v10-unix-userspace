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

**Variable assignment hangs.** `x=42` spins in `findnam`'s tree walk — and so
does assigning to a name that already exists, so it is the walk itself rather
than the insert. `namep`'s tree is built by `setup_env()` before this, and the
first `setenv()` walks it 57 nodes deep without trouble, so something corrupts
it between then and the first assignment.

Ruled out already: `namwalk` is bounded (instrumented to abort after 2000
visits; it never fires), the arena no longer runs past the break (see below),
and `chkid`/`cf` both terminate on their own.

The next thing to do is instrument `lookup()` — print `namep` and each node's
`namlft`/`namrgt` before and after the first assignment — rather than reason
about it. Every bug in this port so far has been found by making the program
print what it has, not by working out what it should have.

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
