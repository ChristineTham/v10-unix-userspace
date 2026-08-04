# The Bourne shell, ported

All 24 source files compile with V8's own compiler and link freestanding. The
shell starts, reads its profile logic, parses, forks, execs and reaps.

## What runs

All of it, as far as the test suite goes (`tests/sh/run.sh`, 21 cases):

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

## The last one: a null pointer that used to read as zero

`x=42` hung, and so did every other variable assignment. It looked like a
corrupt name tree — `findnam` walks one — but instrumenting `findnam` to print
its argument showed it never reached the walk:

```
F[]
```

The name printed as nothing at all, because `write(2, NULL, 3)` writes nothing.
`com[0]` is null: a bare assignment leaves `argn` 0 and no command word.
`findnam` passes it to `chkid`, whose loop

```c
while(!ctrlchar(*nam) && (*nam&QUOTE)==0 && *nam!='(' && *nam!='=')
	nam++;
```

is bounded only by the string's terminator. **On a VAX or PDP-11 running Unix,
address 0 read as zero** — so `ctrlchar(0)` was true and the loop stopped on the
first character. The shell depends on that without saying so, and so does a good
deal of V7-era code.

macOS reserves the entire first 4GB as `__PAGEZERO` and it cannot be mapped, so
the read faults. Guarded at the call site in `xec.c`, which is the one place
that can know the word is optional.

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
